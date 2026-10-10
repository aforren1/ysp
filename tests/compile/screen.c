/* Compile check: ysp/screen.h as a C translation unit with the
 * implementation enabled. CMake and CI build this as C11 with warnings as
 * errors when SDL3 is found; screen.cpp builds the same source as C++17.
 * It runs three frames on the simulated display, which needs no window, so
 * it passes on a machine with no display and no ANGLE. */
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

/* v0.5.2: desc.on_context runs once, before settling (skipped on SIM) */
static void on_context(void* ctx, yscr_screen* s) {
    if (yscr_is_open(s)) ++*(int*)ctx;
}

/* v0.6.0: the calls that write yscr_error() on failure, by type, so a
 * changed signature is a compile error here rather than a surprise in a
 * caller. */
typedef struct screen_api {
    int (*begin)(yscr_screen*, yscr_frame*);
    int (*flip_at)(yscr_screen*, int64_t, yscr_record*);
    int (*flip)(yscr_screen*);
    int (*wait_flip)(yscr_screen*, yscr_record*);
    int (*code)(yscr_screen*, int, uint32_t);
    int (*code_frames)(yscr_screen*, int, uint32_t, int);
    int (*code_row)(yscr_screen*, int, const uint32_t*, int, int);
    int (*trigger)(yscr_screen*, int, uint32_t);
    int (*trigger_at)(yscr_screen*, int, uint32_t, int64_t);
    int (*native)(yscr_screen*, yscr_native_info*);
    int (*begin_group)(yscr_screen* const*, int, yscr_frame*);
    int (*flip_group_at)(yscr_screen* const*, int, int64_t);
    int (*text_input)(yscr_screen*, bool, int, int, int, int);
    const char* (*error)(const yscr_screen*);
} screen_api;

static const screen_api g_api = {
    yscr_begin, yscr_flip_at, yscr_flip, yscr_wait_flip, yscr_code, yscr_code_frames,
    yscr_code_row, yscr_trigger, yscr_trigger_at, yscr_native, yscr_begin_group,
    yscr_flip_group_at, yscr_text_input, yscr_error
};

int main(void) {
    yscr_screen s;
    yscr_desc d;
    yscr_frame f;
    yscr_sync_info si;
    yscr_settle_info st;
    char line[1024];
    int i, n = 0, hooked = 0;
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 2000000;
    d.depth = 2;   /* v0.4.4: a pinned depth */
    d.settle = YSCR_SETTLE_STRICT;   /* v0.5.0: SIM skips settling, recorded */
    d.settle_flips = 6;
    d.foreground = YSCR_FOREGROUND_FORCE;   /* v0.5.1: no window, n/a */
    d.on_context = on_context;
    d.on_context_ctx = &hooked;
    d.panic_window_ns = YRT_S(2);   /* v0.6.0: int64 ns */
    d.panic_grace_ns = YRT_S(10);
    d.trigger_spin_ns = YRT_US(50);
    if (!yscr_open(&s, &d)) return 1;
    for (i = 0; i < 3; i++) {
        if (yscr_begin(&s, &f) != YSCR_OK) return 2;
        if (yscr_flip(&s) != YSCR_OK) return 3;
    }
    yscr_sync_check(&s, &si);   /* v0.4.2: the simulated display is synced */
    if (si.untimed || si.fired_index != -1) return 5;
    yscr_describe(&s, line, sizeof line);
    if (!strstr(line, "depth=2(pin,0 changes)")) return 6;
    yscr_settle_check(&s, &st);
    if (st.result != YSCR_SETTLE_SKIPPED || !strstr(line, "settle=SKIPPED(sim)")) return 7;
    if (st.foreground != YSCR_FG_NA || !strstr(line, "foreground=n/a")) return 8;
    if (hooked != 1 || st.hook_ns < 0) return 9;
    if (g_api.wait_flip(&s, NULL) != YSCR_OK || g_api.begin(&s, &f) != YSCR_OK) return 10;
    if (g_api.begin(&s, &f) != YSCR_ERR_ORDER || !strstr(g_api.error(&s), "begin")) return 11;
    if (g_api.flip(&s) != YSCR_OK) return 12;
    yscr_close(&s);
    return yscr_params(&n) && n > 0 ? 0 : 4;
}
