/* platform.c - the designer core's only OS calls: files and folders.
 * Paths are UTF-8 everywhere; Windows takes them as UTF-16. The POSIX branch
 * also serves emscripten, whose file system is in memory. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* struct stat's S_ISDIR under -std=c11 */
#endif
#include "dsg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static int fail(char* err, size_t cap, const char* fmt, const char* path, const char* why) {
    if (err && cap) snprintf(err, cap, fmt, path, why);
    return -1;
}

#if defined(_WIN32)
static wchar_t* wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t* w;
    if (n <= 0) return NULL;
    w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) { free(w); w = NULL; }
    return w;
}

static FILE* open_utf8(const char* path, const wchar_t* mode) {
    wchar_t* w = wide(path);
    FILE* f = w ? _wfopen(w, mode) : NULL;
    free(w);
    return f;
}

static DWORD attrs(const char* path) {
    wchar_t* w = wide(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a;
}

bool dsg_is_dir(const char* path) {
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dsg_is_file(const char* path) {
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dsg_make_dir(const char* path, char* err, size_t cap) {
    wchar_t* w = wide(path);
    int ok = w && (_wmkdir(w) == 0 || errno == EEXIST);
    free(w);
    if (!ok || !dsg_is_dir(path)) return fail(err, cap, "cannot make the folder '%s': %s", path, strerror(errno));
    return 0;
}

/* rename() refuses an existing target on Windows. */
static int replace_file(const char* from, const char* to) {
    wchar_t* a = wide(from);
    wchar_t* b = wide(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

static void remove_file(const char* path) {
    wchar_t* w = wide(path);
    if (w) _wremove(w);
    free(w);
}
#define FOPEN_R(p) open_utf8(p, L"rb")
#define FOPEN_W(p) open_utf8(p, L"wb")
#else
bool dsg_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool dsg_is_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int dsg_make_dir(const char* path, char* err, size_t cap) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) return fail(err, cap, "cannot make the folder '%s': %s", path, strerror(errno));
    if (!dsg_is_dir(path)) return fail(err, cap, "cannot make the folder '%s': %s", path, "a file has that name");
    return 0;
}

static int replace_file(const char* from, const char* to) { return rename(from, to); }
static void remove_file(const char* path) { remove(path); }
#define FOPEN_R(p) fopen(p, "rb")
#define FOPEN_W(p) fopen(p, "wb")
#endif

int dsg_read_file(const char* path, char** data, size_t* n, char* err, size_t cap) {
    FILE* f = FOPEN_R(path);
    char* buf = NULL;
    size_t len = 0, have = 0;
    *data = NULL;
    *n = 0;
    if (!f) return fail(err, cap, "cannot read '%s': %s", path, strerror(errno));
    /* read in growing blocks: ftell() is not 64-bit everywhere, and the
     * files here are small */
    for (;;) {
        size_t got;
        if (len + 4096 + 1 > have) {
            size_t nh = have ? have * 2 : 65536;
            char* nb = (char*)realloc(buf, nh);
            if (!nb) { free(buf); fclose(f); return fail(err, cap, "cannot read '%s': %s", path, "out of memory"); }
            buf = nb;
            have = nh;
        }
        got = fread(buf + len, 1, have - len - 1, f);
        len += got;
        if (got == 0) break;
    }
    if (ferror(f)) { free(buf); fclose(f); return fail(err, cap, "cannot read '%s': %s", path, "read error"); }
    fclose(f);
    buf[len] = 0;
    *data = buf;
    *n = len;
    return 0;
}

int dsg_write_file(const char* path, const void* data, size_t n, char* err, size_t cap) {
    char tmp[DSG_PATH_MAX + 8];
    FILE* f;
    int bad;
    if (strlen(path) >= DSG_PATH_MAX) return fail(err, cap, "cannot write '%s': %s", path, "the path is too long");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = FOPEN_W(tmp);
    if (!f) return fail(err, cap, "cannot write '%s': %s", tmp, strerror(errno));
    bad = n && fwrite(data, 1, n, f) != n;
    bad |= fflush(f) != 0;
    bad |= fclose(f) != 0;
    if (bad) { remove_file(tmp); return fail(err, cap, "cannot write '%s': %s", tmp, strerror(errno)); }
    if (replace_file(tmp, path) != 0) {
        remove_file(tmp);
        return fail(err, cap, "cannot replace '%s': %s", path, strerror(errno));
    }
    return 0;
}
