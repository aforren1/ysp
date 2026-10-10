/* Compile check: ysp/parallel.h as a C translation unit with the implementation
 * enabled. CMake and CI build this with warnings as errors, once as C11 and
 * once with threading disabled. It runs and returns 0 to prove it links. */
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"

#include <string.h>

/* One field per entry point, spelled out, so a changed signature is a compile
 * error here rather than a surprise in a caller. Widths are int64_t ns; every
 * call but open returns an int (a byte, 0, or a YPAR_ERR_* code). */
typedef struct parallel_api {
    int  (*list_ports)(ypar_port_info*, int);
    bool (*open)(ypar_port*, const ypar_desc*);
    void (*close)(ypar_port*);
    const char* (*error)(const ypar_port*);
    const char* (*strerror)(int);
    bool (*is_open)(const ypar_port*);
    int  (*write_data)(ypar_port*, uint8_t);
    int  (*read_data)(ypar_port*);
    int  (*set_data_dir)(ypar_port*, bool);
    int  (*pulse)(ypar_port*, uint8_t, int64_t);
    int  (*pulse_async)(ypar_port*, uint8_t, int64_t);
    int  (*read_status)(ypar_port*);
    int  (*get_status_bit)(ypar_port*, uint8_t);
    int  (*read_control)(ypar_port*);
    int  (*write_control)(ypar_port*, uint8_t);
    int  (*set_control_bit)(ypar_port*, uint8_t, bool);
} parallel_api;

static const parallel_api g_api = {
    ypar_list_ports, ypar_open, ypar_close, ypar_error, ypar_strerror,
    ypar_is_open, ypar_write_data, ypar_read_data, ypar_set_data_dir,
    ypar_pulse, ypar_pulse_async, ypar_read_status, ypar_get_status_bit,
    ypar_read_control, ypar_write_control, ypar_set_control_bit
};

typedef char ypar_version_is_0_4[YPAR_VERSION_MAJOR == 0 && YPAR_VERSION_MINOR == 4 ? 1 : -1];

int main(void) {
    ypar_port p;
    memset(&p, 0, sizeof p);
    (void)sizeof(ypar_version_is_0_4);
    /* A zeroed handle is closed: every call returns the code and writes the
     * message, and close is safe on it twice. No port is touched. */
    if (g_api.pulse(&p, 1, YRT_MS(2)) != YPAR_ERR_CLOSED) return 1;
    if (strcmp(g_api.error(&p), "port not open") != 0) return 2;
    if (g_api.read_status(&p) != YPAR_ERR_CLOSED) return 3;
    if (g_api.get_status_bit(&p, YPAR_STATUS_BUSY) != YPAR_ERR_CLOSED) return 4;
    if (g_api.set_control_bit(&p, YPAR_CONTROL_INIT, true) != YPAR_ERR_CLOSED) return 5;
    if (strcmp(g_api.strerror(YPAR_ERR_ARG), "bad argument") != 0) return 6;
    g_api.close(&p);
    g_api.close(&p);
    return g_api.is_open(&p) ? 7 : 0;
}
