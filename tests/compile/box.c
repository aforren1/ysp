/* Compile check: ysp/box.h as a C translation unit, with its implementation.
 * CMake and CI build this as C11 with warnings as errors; box.cpp builds the
 * same source as C++17. It decodes one line-protocol edge, encodes one pulse
 * and returns 0. */
#define YSP_BOX_IMPLEMENTATION
#include "ysp/box.h"

#include <string.h>

/* One field per entry point, spelled out, so a changed signature is a compile
 * error here rather than a surprise in a caller. ybox_init() is an init (int,
 * no close); ybox_encode() takes the width in int64_t ns. */
typedef struct box_api {
    const char* (*version)(void);
    const char* (*family_name)(int);
    const char* (*strerror)(int);
    int    (*init)(ybox_decoder*, int, uint8_t, uint32_t);
    size_t (*decode)(ybox_decoder*, const uint8_t*, size_t, int64_t, ybox_out*);
    void   (*out_clear)(ybox_out*);
    int    (*probe)(int, uint32_t, uint8_t*, int);
    int    (*identify)(int, uint8_t*, int);
    double (*ns_per_tick)(int);
    bool   (*slow_write)(int);
    uint32_t (*caps)(int);
    uint32_t (*code_max)(int);
    int64_t  (*fixed_pulse_ns)(int);
    uint32_t (*default_baud)(int);
    int    (*encode)(int, int, uint32_t, int64_t, uint8_t*, int);
} box_api;

static const box_api g_api = {
    ybox_version, ybox_family_name, ybox_strerror, ybox_init, ybox_decode,
    ybox_out_clear, ybox_probe, ybox_identify, ybox_ns_per_tick,
    ybox_slow_write, ybox_caps, ybox_code_max, ybox_fixed_pulse_ns,
    ybox_default_baud, ybox_encode
};

int main(void) {
    static const char line[] = "E 1000 1 1\n";
    ybox_decoder d;
    yin_event ev[4];
    ybox_pair pair[4];
    ybox_out out;
    uint8_t b[16];
    memset(&out, 0, sizeof out);
    out.ev = ev; out.ev_cap = 4; out.pair = pair; out.pair_cap = 4;
    if (g_api.init(&d, YBOX_LINE, YIN_KIND_SYNC, 1) < 0) return 1;
    if (g_api.init(&d, YBOX_NONE, YIN_KIND_SYNC, 1) != YBOX_ERR_ARG) return 1;
    if (g_api.init(&d, YBOX_LINE, YIN_KIND_SYNC, 1) < 0) return 1;
    (void)g_api.decode(&d, (const uint8_t*)line, sizeof line - 1, 5, &out);
    if (g_api.encode(YBOX_LINE, YBOX_OP_PULSE, 3, YRT_MS(2), b, (int)sizeof b) != 9) return 1;
    return out.n_ev == 1 && ev[0].ticks == 1000u && out.n_pair == 1 ? 0 : 1;
}
