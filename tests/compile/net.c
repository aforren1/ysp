/* Compile check: ysp/net.h as a C translation unit, with its implementation
 * (which implements ysp/rt.h). CMake and CI build this as C11 with warnings
 * as errors; net.cpp builds the same source as C++17. It opens liblsl from a
 * path that does not exist (the loader must report it, not crash), checks a
 * key, and opens nothing. Returns 0. */
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"

#include <string.h>

/* One field per entry point, spelled out, so a changed signature is a compile
 * error here rather than a surprise in a caller. Times are int64_t ns; the
 * lsl_ values are liblsl's seconds. */
typedef struct net_api {
    const char* (*version)(void);
    bool (*lsl_open)(ynet_lsl*, const char*);
    void (*lsl_close)(ynet_lsl*);
    const char* (*lsl_error)(const ynet_lsl*);
    int64_t (*lsl_now_ns)(const ynet_lsl*);
    const char* (*strerror)(int);
    const char* (*state_name)(int);
    bool (*key_parse)(const char*, ynet_key*);
    int  (*key_predicate)(const ynet_key*, char*, size_t);
    bool (*key_match)(const char*, const char*);
    int  (*resolve_all)(const ynet_lsl*, ynet_found*, int, int64_t);
    int  (*stream_init)(ynet_stream*, void*, size_t, int);
    int  (*stream_read)(ynet_stream*, int64_t*, double*, float*, int);
    bool (*stream_newest)(ynet_stream*, int64_t*, double*, float*);
    uint32_t (*stream_capacity)(const ynet_stream*);
    uint32_t (*stream_dropped)(ynet_stream*);
    bool (*outlet_open)(ynet_outlet*, const ynet_outlet_desc*);
    void (*outlet_close)(ynet_outlet*);
    int  (*out_mark)(ynet_outlet*, uint32_t, const char*);
    int  (*out_mark_at)(ynet_outlet*, uint32_t, const char*, int64_t);
    int  (*out_trigger)(ynet_outlet*, uint32_t, int64_t);
    int  (*out_push)(ynet_outlet*, const float*, int, const double*);
    int  (*outlet_consumers)(ynet_outlet*);
    void (*out_last)(ynet_outlet*, ynet_out_info*);
    double (*outlet_to_lsl)(ynet_outlet*, int64_t);
    const char* (*outlet_error)(const ynet_outlet*);
    bool (*inlet_open)(ynet_inlet*, const ynet_inlet_desc*);
    void (*inlet_close)(ynet_inlet*);
    int  (*inlet_poll)(ynet_inlet*, int64_t);
    int  (*inlet_wait_running)(ynet_inlet*, int64_t);
    int  (*inlet_state)(ynet_inlet*);
    void (*inlet_stats)(ynet_inlet*, ynet_stats*);
    int64_t (*inlet_map)(ynet_inlet*, double);
    yin_source (*inlet_source)(const ynet_inlet*);
    const char* (*inlet_error)(const ynet_inlet*);
} net_api;

static const net_api g_api = {
    ynet_version, ynet_lsl_open, ynet_lsl_close, ynet_lsl_error, ynet_lsl_now_ns,
    ynet_strerror, ynet_state_name, ynet_key_parse, ynet_key_predicate,
    ynet_key_match, ynet_resolve_all, ynet_stream_init, ynet_stream_read,
    ynet_stream_newest, ynet_stream_capacity, ynet_stream_dropped,
    ynet_outlet_open, ynet_outlet_close, ynet_out_mark, ynet_out_mark_at,
    ynet_out_trigger, ynet_out_push, ynet_outlet_consumers, ynet_out_last,
    ynet_outlet_to_lsl, ynet_outlet_error, ynet_inlet_open, ynet_inlet_close,
    ynet_inlet_poll, ynet_inlet_wait_running, ynet_inlet_state,
    ynet_inlet_stats, ynet_inlet_map, ynet_inlet_source, ynet_inlet_error
};

int main(void) {
    static ynet_lsl lib;
    static unsigned char mem[YNET_STREAM_BYTES(16, 4)];
    static ynet_stream s;
    static ynet_inlet in;
    ynet_inlet_desc d;
    ynet_key k;
    char pred[200];
    int ok = !g_api.lsl_open(&lib, "ysp-compile-check-no-such-liblsl") && g_api.lsl_error(&lib)[0] != '\0';
    ok = ok && g_api.key_parse("lsl:Data::LabStreamer:", &k) && g_api.key_predicate(&k, pred, sizeof pred) > 0;
    ok = ok && strcmp(pred, "name='Data' and source_id='LabStreamer'") == 0;
    ok = ok && g_api.stream_init(&s, mem, sizeof mem, 4) == 0 && g_api.stream_capacity(&s) == 16;
    memset(&d, 0, sizeof d);
    d.lib = &lib;   /* not open: refused */
    d.role = "check";
    d.key = "lsl:Data::";
    d.device = 1;
    d.read_timeout_ns = YRT_MS(10);
    d.lsl_max_buflen = 30;
    ok = ok && !g_api.inlet_open(&in, &d) && g_api.inlet_wait_running(&in, 0) == YNET_ERR_STATE;
    g_api.inlet_close(&in);
    g_api.lsl_close(&lib);
    g_api.lsl_close(&lib);
    return ok ? 0 : 1;
}
