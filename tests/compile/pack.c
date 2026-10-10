/* Compile check: ysp/pack.h as a C translation unit with the implementation
 * and the file source. CMake and CI build this as C11 with warnings as
 * errors; pack.cpp builds the same source as C++17. It checks two hash
 * vectors and returns 0 to prove it links and runs. The table names the
 * lifecycle by type, so a signature change fails here and not in a user's
 * build. */
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"

typedef struct pack_api {
    bool (*open)(ypak_pack*, const ypak_desc*);
    int (*feed)(ypak_pack*, const void*, size_t);
    void (*close)(ypak_pack*);
    const char* (*error)(const ypak_pack*);
    const char* (*strerror)(int);
    int (*cursor_init)(ypak_cursor*, ypak_pack*, const ypak_entry*, void*, size_t);
} pack_api;

static const pack_api g_pack_api = {
    ypak_open, ypak_feed, ypak_close, ypak_error, ypak_strerror, ypak_cursor_init
};

int main(void) {
    static ypak_pack pk;
    uint8_t h[32];
    /* The zero-init rule (README, Conventions): { 0 } in C, {} in C++. */
#ifdef __cplusplus
    ypak_desc d = {};
#else
    ypak_desc d = { 0 };
#endif
    ypak_sha256("abc", 3, h);
    if (h[0] != 0xBA || h[31] != 0xAD) return 1;
    if (ypak_xxh64("", 0, 0) != 0xEF46DB3751D8E999ULL) return 2;
    if (ypak_crc32(0, "123456789", 9) != 0xCBF43926u) return 3;
    if (g_pack_api.open(&pk, &d) || pk.error_code != YPAK_ERR_ARG || !g_pack_api.error(&pk)[0]) return 5;
    g_pack_api.close(&pk);
    g_pack_api.close(&pk);
    if (g_pack_api.feed(&pk, NULL, 0) >= 0) return 6;
    (void)g_pack_api.strerror;
    (void)g_pack_api.cursor_init;
    return ypak_kind_id("table") == YPAK_KIND_TABLE ? 0 : 4;
}
