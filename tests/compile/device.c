/* Compile check: ysp/device.h as a C translation unit, with its
 * implementation (which implements ysp/rt.h, ysp/box.h and ysp/serial.h).
 * CMake and CI build this as C11 with warnings as errors; device.cpp builds
 * the same source as C++17. It opens and closes a manual instance whose
 * port never opens, and returns 0. v0.5: the response rule, and
 * ysp/response.h's glue for it (that header included after this one). */
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
/* after ysp/device.h: ysp/response.h's rule glue (v0.5) */
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <string.h>

/* One field per entry point, spelled out, so a changed signature is a compile
 * error here rather than a surprise in a caller. Times are int64_t ns. */
typedef struct device_api {
    const char* (*version)(void);
    bool (*open)(ydev_device*, const ydev_desc*);
    void (*close)(ydev_device*);
    int  (*poll)(ydev_device*, int64_t);
    int  (*wait_running)(ydev_device*, int64_t);
    int  (*state)(ydev_device*);
    void (*get_stats)(ydev_device*, ydev_stats*);
    const char* (*error)(const ydev_device*);
    const char* (*strerror)(int);
    const char* (*state_name)(int);
    yin_source (*source)(const ydev_device*);
    int  (*port_key)(const yser_port_info*, char*, size_t);
    bool (*key_match)(const char*, const char*);
    ydev_transport (*serial_transport)(ydev_device*);
    int  (*out_set)(ydev_device*, uint32_t);
    int  (*out_pulse)(ydev_device*, uint32_t, int64_t);
    int  (*out_mark)(ydev_device*, uint32_t, const char*);
    int  (*out_trigger)(ydev_device*, uint32_t, int64_t);
    void (*out_last)(ydev_device*, ydev_out_info*);
    int  (*role_index)(const ydev_roles*, const char*);
    ydev_device* (*role_device)(const ydev_roles*, int);
    const char* (*role_name)(const ydev_roles*, int);
    const ydev_param* (*params)(int, int*);       /* v0.4 */
    bool (*rule_open)(ydev_rule*, const ydev_rule_desc*);   /* v0.5 */
    void (*rule_close)(ydev_rule*);
    const char* (*rule_error)(const ydev_rule*);
    void (*rule_arm)(ydev_rule*, int64_t, int64_t);
    void (*rule_window)(ydev_rule*, int64_t, int64_t);
    void (*rule_disarm)(ydev_rule*);
    int  (*rule_input)(ydev_rule*, const yin_event*);
    void (*rule_get)(ydev_rule*, ydev_rule_info*);
    int  (*rule_fires)(ydev_rule*, ydev_rule_fire*, int);
    /* ysp/response.h's glue */
    void (*rsp_rule_arm)(ydev_rule*, const yrsp_collector*);
    void (*rsp_rule_window)(ydev_rule*, const yrsp_collector*);
    int  (*rsp_rule_choices)(const yrsp_collector*, const uint32_t*, ydev_rule_choice*, int);
    int  (*rsp_rule_sent)(ydev_rule*, yrsp_collector*);
} device_api;

static const device_api g_api = {
    ydev_version, ydev_open, ydev_close, ydev_poll, ydev_wait_running,
    ydev_state, ydev_get_stats, ydev_error, ydev_strerror, ydev_state_name,
    ydev_source, ydev_port_key, ydev_key_match, ydev_serial_transport,
    ydev_out_set, ydev_out_pulse, ydev_out_mark, ydev_out_trigger,
    ydev_out_last, ydev_role_index, ydev_role_device, ydev_role_name, ydev_params,
    ydev_rule_open, ydev_rule_close, ydev_rule_error, ydev_rule_arm, ydev_rule_window,
    ydev_rule_disarm, ydev_rule_input, ydev_rule_get, ydev_rule_fires,
    yrsp_rule_arm, yrsp_rule_window, yrsp_rule_choices, yrsp_rule_sent
};

/* The transport's read takes the timeout in ns. */
typedef int (*read_fn)(void*, uint8_t*, int, int64_t);

int main(void) {
    static ydev_device dev;
    ydev_desc d;
    char key[YDEV_KEY_MAX + 8];
    read_fn rf = g_api.serial_transport(&dev).read;
    int ok;
    memset(&d, 0, sizeof d);
    memset(key, 'x', sizeof key);
    key[sizeof key - 1] = '\0';
    d.role = "check";
    d.family = YBOX_LINE;
    d.key = key;                       /* longer than YDEV_KEY_MAX - 1 */
    d.device = 1;
    d.manual = true;
    d.read_timeout_ns = YRT_MS(20);
    if (g_api.open(&dev, &d)) return 1;
    g_api.close(&dev);                 /* after a failed open */
    memcpy(key, "port:ysp-compile-check-no-such-port", sizeof "port:ysp-compile-check-no-such-port");
    if (!g_api.open(&dev, &d)) return 1;
    key[0] = '\0';                     /* the handle has its own copy */
    ok = g_api.poll(&dev, 0) == YDEV_OPENING && strcmp(dev.d.key, "port:ysp-compile-check-no-such-port") == 0;
    ok = ok && g_api.wait_running(&dev, 0) == YDEV_ERR_TIMEOUT && g_api.error(&dev)[0] != '\0';
    ok = ok && g_api.out_pulse(&dev, 1, YRT_MS(2)) == YDEV_ERR_STATE && rf != NULL;
    g_api.close(&dev);
    g_api.close(&dev);                 /* twice */
    memcpy(key, "port:x", sizeof "port:x");
    d.min_hold_ns = -1;                /* v0.3.1: ns, >= 0 */
    ok = ok && !g_api.open(&dev, &d) && strstr(g_api.error(&dev), "min_hold_ns") != NULL;
    {   /* v0.4: a table per family; the MMBT-S's own baud */
        int n = 0, i;
        const ydev_param* p = g_api.params(YBOX_MMBTS, &n);
        for (i = 0; p && i < n; i++)
            if (!strcmp(p[i].name, "baud")) ok = ok && p[i].def == 9600 && p[i].values == NULL;
        ok = ok && n > 0 && g_api.params(YBOX_NONE, &n) == NULL && n == 0;
    }
    {   /* v0.5: a rule refuses an output that is not open, and a closed
         * rule fires nothing */
        static ydev_rule r;
        ydev_rule_desc rd;
        ydev_rule_choice ch;
        ydev_rule_info ri;
        yin_event e;
        memset(&rd, 0, sizeof rd);
        memset(&ch, 0, sizeof ch);
        memset(&e, 0, sizeof e);
        ch.kind = YIN_KIND_BOX;
        ch.code = 1;
        rd.out = &dev;
        rd.choices = &ch;
        rd.n_choices = 1;
        ok = ok && !g_api.rule_open(&r, &rd) && strstr(g_api.rule_error(&r), "outputs") != NULL;
        g_api.rule_arm(&r, 0, YDEV_RULE_NEVER);
        g_api.rule_window(&r, 0, YDEV_RULE_NEVER);
        g_api.rule_disarm(&r);
        e.type = YIN_PRESS;
        e.kind = YIN_KIND_BOX;
        ok = ok && g_api.rule_input(&r, &e) == 0;
        g_api.rule_get(&r, &ri);
        ok = ok && ri.choice == -1 && ri.fired == 0;
        g_api.rule_close(&r);
        d.rule = &r;                   /* the desc field, by type */
        {
            static yrsp_collector c;
            static const uint32_t codes[1] = { 9 };
            yrsp_desc cd;
            yrsp_choice k;
            ydev_rule_choice out[2];
            memset(&cd, 0, sizeof cd);
            memset(&k, 0, sizeof k);
            k.key = "f";
            cd.choices = &k;
            cd.n_choices = 1;
            ok = ok && yrsp_open(&c, &cd) && g_api.rsp_rule_choices(&c, codes, out, 2) == 1 && out[0].code == 9;
            g_api.rsp_rule_arm(&r, &c);      /* a closed rule: nothing */
            g_api.rsp_rule_window(&r, &c);
            ok = ok && g_api.rule_fires(&r, NULL, 0) == 0 && g_api.rsp_rule_sent(&r, &c) == 0;
            yrsp_close(&c);
        }
    }
    return ok && g_api.state(&dev) == YDEV_CLOSED && g_api.key_match("serial:16C0::", "serial:16c0:0483:1:A") ? 0 : 1;
}
