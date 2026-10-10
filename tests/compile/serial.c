/* Compile check: ysp/serial.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors, once with
 * threading and once with YSER_NO_THREADS (which drops the async-pulse
 * worker); serial.cpp builds the same source as C++17. It runs and
 * returns 0 to prove it links. */
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"

#include <string.h>

/* One field per entry point, spelled out, so a changed signature is a compile
 * error here rather than a surprise in a caller. The times are int64_t ns. */
typedef struct serial_api {
    int  (*list_ports)(yser_port_info*, int);
    int  (*find_ports)(const yser_port_filter*, yser_port_info*, int);
    bool (*open)(yser_port*, const yser_desc*);
    void (*close)(yser_port*);
    int  (*interrupt)(yser_port*);
    const char* (*error)(const yser_port*);
    const char* (*strerror)(int);
    bool (*is_open)(const yser_port*);
    int  (*write)(yser_port*, const void*, int);
    int  (*write_byte)(yser_port*, uint8_t);
    int  (*drain)(yser_port*);
    int  (*purge)(yser_port*, int);
    int  (*pulse)(yser_port*, uint8_t, uint8_t, int64_t);
    int  (*pulse_async)(yser_port*, uint8_t, uint8_t, int64_t);
    int  (*read)(yser_port*, void*, int, int64_t, int);
    int  (*available)(yser_port*);
    int  (*set_dtr)(yser_port*, bool);
    int  (*set_rts)(yser_port*, bool);
    int  (*get_lines)(yser_port*);
    int  (*send_break)(yser_port*, int64_t);
} serial_api;

static const serial_api g_api = {
    yser_list_ports, yser_find_ports, yser_open, yser_close, yser_interrupt,
    yser_error, yser_strerror, yser_is_open, yser_write, yser_write_byte,
    yser_drain, yser_purge, yser_pulse, yser_pulse_async, yser_read,
    yser_available, yser_set_dtr, yser_set_rts, yser_get_lines,
    yser_send_break
};

/* The desc's time field by name and type, written the way the USAGE block
 * writes a desc (zeroed, then set: C99 and C++17 both). */
static int64_t g_forever = YSER_TIMEOUT_INFINITE;
typedef char yser_version_is_0_5[YSER_VERSION_MAJOR == 0 && YSER_VERSION_MINOR == 5 ? 1 : -1];
typedef char yser_units_are_ns[YSER_DEFAULT_WRITE_TIMEOUT_NS == 1000000000 && YSER_TIMEOUT_INFINITE == -1 ? 1 : -1];

int main(void) {
    yser_desc d;
    yser_port p;
    memset(&d, 0, sizeof d);
    memset(&p, 0, sizeof p);
    d.device = "ysp-compile-check-no-such-port";
    d.write_timeout_ns = YRT_MS(100);
    (void)sizeof(yser_version_is_0_5);
    (void)sizeof(yser_units_are_ns);
    /* Calls on a closed handle: no device is needed, and the codes are the
     * documented ones. */
    if (g_api.pulse(&p, 1, 0, YRT_MS(2)) != YSER_ERR_CLOSED) return 1;
    if (g_api.read(&p, &d, 1, g_forever, YSER_READ_ANY) != YSER_ERR_CLOSED) return 2;
    if (g_api.send_break(&p, YRT_MS(1)) != YSER_ERR_CLOSED) return 3;
    if (!g_api.error(&p) || g_api.is_open(&p)) return 5;
    g_api.close(&p);
    return d.write_timeout_ns == 100000000 ? 0 : 6;
}
