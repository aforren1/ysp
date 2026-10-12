/* Compile check: ysp/device.h as a C translation unit, with its
 * implementation (which implements ysp/rt.h, ysp/box.h and ysp/serial.h).
 * CMake and CI build this as C11 with warnings as errors; device.cpp builds
 * the same source as C++17. It opens and closes a manual instance whose
 * port never opens, and returns 0. */
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"

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
} device_api;

static const device_api g_api = {
    ydev_version, ydev_open, ydev_close, ydev_poll, ydev_wait_running,
    ydev_state, ydev_get_stats, ydev_error, ydev_strerror, ydev_state_name,
    ydev_source, ydev_port_key, ydev_key_match, ydev_serial_transport,
    ydev_out_set, ydev_out_pulse, ydev_out_mark, ydev_out_trigger,
    ydev_out_last, ydev_role_index, ydev_role_device, ydev_role_name
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
    return ok && g_api.state(&dev) == YDEV_CLOSED && g_api.key_match("serial:16C0::", "serial:16c0:0483:1:A") ? 0 : 1;
}
