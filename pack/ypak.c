/* ypak - the pack tool's command line (docs/pack.md 5.1). Each command is
 * one call of pack/ysp/pack_tool.h; the exit code is 0 on success, 1 on a
 * failure (the message on stderr), 2 on a usage error. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/pack_tool.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

static int usage(void) {
    fputs("ypak " YPT_VERSION_STRING " - ysp pack tool (docs/pack.md)\n"
#ifdef YPT_NO_LAYOUT
          "core build: no glyph runs (configure with -DYSP_BUILD_LAYOUT=ON for them)\n"
#endif
          "usage:\n"
          "  ypak build SOURCE.json -o OUT.ysppak [--sources DIR] [--ffmpeg PATH] [-v]\n"
          "  ypak verify PACK\n"
          "  ypak list PACK\n"
          "  ypak info PACK\n"
          "  ypak extract PACK [NAME...] -C DIR [--force]\n"
          "  ypak cat PACK NAME\n"
          "  ypak rebuild PACK --sources DIR [-o OUT] [--ffmpeg PATH]\n"
          "  ypak append PLAYER PACK -o OUT\n", stderr);
    return 2;
}

static const char* opt(int argc, char** argv, const char* name) {
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static int flag(int argc, char** argv, const char* name) {
    int i;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
}

int main(int argc, char** argv) {
    char err[1024];
    const char* cmd;
    int rc;
    err[0] = 0;
    if (argc < 3) return usage();
    cmd = argv[1];
    if (!strcmp(cmd, "build")) {
        ypt_options o;
        const char* out = opt(argc, argv, "-o");
        if (!out) return usage();
        memset(&o, 0, sizeof o);
        o.sources = opt(argc, argv, "--sources");
        o.ffmpeg = opt(argc, argv, "--ffmpeg");
        o.log = flag(argc, argv, "-v") ? stdout : NULL;
        rc = ypt_build(argv[2], out, &o, err, sizeof err);
        if (rc == 0) printf("ypak: wrote %s\n", out);
    } else if (!strcmp(cmd, "verify")) {
        rc = ypt_verify(argv[2], stdout, err, sizeof err);
    } else if (!strcmp(cmd, "list")) {
        rc = ypt_list(argv[2], stdout, err, sizeof err);
    } else if (!strcmp(cmd, "info")) {
        rc = ypt_info(argv[2], stdout, err, sizeof err);
    } else if (!strcmp(cmd, "extract")) {
        const char* dir = opt(argc, argv, "-C");
        const char* names[256];
        int n = 0, i;
        if (!dir) return usage();
        for (i = 3; i < argc && n < 256; i++) {
            if (!strcmp(argv[i], "-C")) { i++; continue; }
            if (!strcmp(argv[i], "--force")) continue;
            names[n++] = argv[i];
        }
        rc = ypt_extract(argv[2], names, n, dir, flag(argc, argv, "--force"), stdout, err, sizeof err);
    } else if (!strcmp(cmd, "cat")) {
        if (argc < 4) return usage();
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        rc = ypt_cat(argv[2], argv[3], stdout, err, sizeof err);
    } else if (!strcmp(cmd, "rebuild")) {
        const char* src = opt(argc, argv, "--sources");
        ypt_options o;
        if (!src) return usage();
        memset(&o, 0, sizeof o);
        o.ffmpeg = opt(argc, argv, "--ffmpeg");
        rc = ypt_rebuild_ex(argv[2], src, opt(argc, argv, "-o"), &o, stdout, err, sizeof err);
    } else if (!strcmp(cmd, "append")) {
        const char* out = opt(argc, argv, "-o");
        if (argc < 4 || !out) return usage();
        rc = ypt_append(argv[2], argv[3], out, err, sizeof err);
        if (rc == 0) printf("ypak: wrote %s\n", out);
    } else {
        return usage();
    }
    if (rc) {
        fprintf(stderr, "%s\n", err[0] ? err : "ypak: failed");
        return 1;
    }
    return 0;
}
