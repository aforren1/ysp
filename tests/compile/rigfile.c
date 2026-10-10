/* Compile check: ysp/rigfile.h as a C translation unit, with its
 * implementation (which implements ysp/json.h and ysp/device.h, and so
 * ysp/rt.h, ysp/box.h and ysp/serial.h). CMake and CI build this as C11
 * with warnings as errors; rigfile.cpp builds the same source as C++17. It
 * reads a profile from text, writes it back, fills a desc and starts and
 * stops a manual instance whose port never opens, and returns 0. */
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <string.h>

static yrig_profile g_prof;

int main(void) {
    static const char text[] =
        "{\"format\": \"ysp-rig 1\", \"rig\": \"check\", \"written\": \"2026-10-09T00:00:00Z\", "
        "\"roles\": {\"resp\": {\"family\": \"line\", \"key\": \"port:ysp-compile-check-no-such-port\"}}}";
    static ydev_device dev;
    char err[256], hash[65], out[512];
    ydev_desc d;
    int ok;
    if (yrig_parse(&g_prof, text, sizeof text - 1, NULL, err, sizeof err) != YRIG_OK) return 1;
    if (yrig_write(&g_prof, out, sizeof out) == 0 || yrig_hash(&g_prof, hash) != YRIG_OK) return 2;
    memset(&d, 0, sizeof d);
    d.device = 1;
    d.manual = true;
    if (!yrig_start(&dev, &g_prof, "resp", &d, err, sizeof err)) return 3;
    ok = ydev_poll(&dev, 0) == YDEV_OPENING && yrig_source(&g_prof, &dev).tier == YIN_TIER_UNKNOWN;
    ydev_close(&dev);
    return ok ? 0 : 4;
}
