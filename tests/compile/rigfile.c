/* Compile check: ysp/rigfile.h as a C translation unit, with its
 * implementation (which implements ysp/json.h and ysp/device.h, and so
 * ysp/rt.h, ysp/box.h and ysp/serial.h). CMake and CI build this as C11
 * with warnings as errors; rigfile.cpp builds the same source as C++17. It
 * reads a profile from text, writes it back, fills a desc and opens and
 * closes a manual instance whose port never opens, and returns 0. */
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <string.h>

/* One field per entry point, spelled out, so a changed signature is a compile
 * error here rather than a surprise in a caller. init, load/save with a
 * caller buffer, and one open of another header's handle. */
typedef struct rigfile_api {
    const char* (*version)(void);
    const char* (*check_name)(int);
    int  (*dir)(char*, size_t);
    int  (*path)(const char*, const char*, char*, size_t);
    int  (*load)(yrig_profile*, const char*, char*, size_t);
    int  (*load_file)(yrig_profile*, const char*, char*, size_t);
    int  (*parse)(yrig_profile*, const char*, size_t, const char*, char*, size_t);
    int  (*save)(yrig_profile*, const char*, char*, size_t);
    int  (*save_file)(yrig_profile*, const char*, char*, size_t);
    void (*init)(yrig_profile*, const char*);
    size_t (*write)(const yrig_profile*, char*, size_t);
    int  (*hash)(const yrig_profile*, char[65]);
    yrig_role* (*find)(const yrig_profile*, const char*);
    yrig_role* (*add)(yrig_profile*, const char*);
    bool (*remove)(yrig_profile*, const char*);
    int  (*bind)(yrig_profile*, const char*, int, const char*);
    int  (*store_loopback)(const char*, const void*, size_t, char[65], char*, size_t);
    int  (*read_latency)(const char*, size_t, yrig_loopback*, char*, size_t);
    int  (*read_loopback)(const char*, size_t, yrig_loopback*, char*, size_t);
    void (*sha256)(const void*, size_t, uint8_t[32]);
    int  (*desc)(const yrig_profile*, const char*, ydev_desc*, char*, size_t);
    bool (*open_device)(ydev_device*, const yrig_profile*, const char*, const ydev_desc*);
    yin_source (*source)(const yrig_profile*, const ydev_device*);
    yin_source (*source_role)(const yrig_profile*, const char*, yin_source);
} rigfile_api;

static const rigfile_api g_api = {
    yrig_version, yrig_check_name, yrig_dir, yrig_path, yrig_load, yrig_load_file,
    yrig_parse, yrig_save, yrig_save_file, yrig_init, yrig_write, yrig_hash,
    yrig_find, yrig_add, yrig_remove, yrig_bind, yrig_store_loopback,
    yrig_read_latency, yrig_read_loopback, yrig_sha256, yrig_desc, yrig_open_device, yrig_source,
    yrig_source_role
};

static yrig_profile g_prof;

int main(void) {
    static const char text[] =
        "{\"format\": \"ysp-rig 1\", \"rig\": \"check\", \"written\": \"2026-10-09T00:00:00Z\", "
        "\"roles\": {\"resp\": {\"family\": \"line\", \"key\": \"port:ysp-compile-check-no-such-port\"}}}";
    static ydev_device dev;
    char err[256], hash[65], out[512];
    ydev_desc d;
    int ok;
    if (g_api.parse(&g_prof, text, sizeof text - 1, NULL, err, sizeof err) != YRIG_OK) return 1;
    if (g_api.write(&g_prof, out, sizeof out) == 0 || g_api.hash(&g_prof, hash) != YRIG_OK) return 2;
    {
        static const char bounds[] = "format ysp-in-bounds 1\nn 1\nlo_s -0.0001\nhi_s 0.0002\ndate 2026-10-13T00:00:00Z\n";
        yrig_loopback lb;
        if (g_api.read_loopback(bounds, sizeof bounds - 1, &lb, err, sizeof err) != YRIG_FILE_IN_BOUNDS || lb.hi_ns != 200000) return 5;
    }
    memset(&d, 0, sizeof d);
    d.device = 1;
    d.manual = true;
    if (g_api.open_device(&dev, &g_prof, "nobody", &d) || !strstr(ydev_error(&dev), "nobody")) return 3;
    if (!g_api.open_device(&dev, &g_prof, "resp", &d)) return 3;
    ok = ydev_poll(&dev, 0) == YDEV_OPENING && g_api.source(&g_prof, &dev).tier == YIN_TIER_UNKNOWN;
    ydev_close(&dev);
    return ok ? 0 : 4;
}
