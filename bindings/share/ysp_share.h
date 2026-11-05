/* ysp_share.h - one implementation of a ysp header, shared by binding modules
 *
 * Not a library header: the Python extensions and MEX files of the bindings
 * include it. A header with state that other modules must reach (ysp/audio.h,
 * ysp/timeline.h) is compiled into ONE module, its owner (ysp.audio and
 * ysp_audio; ysp.timeline and ysp_timeline). The owner exports a table: an
 * ABI number, the header version it compiled, the sizes of the structs a
 * consumer touches, the functions a consumer may call, and acquire/release,
 * which turn a host object (a Python object, a MEX handle) into the C
 * pointer and keep it valid. A consumer (ysp-vis: video.h, gfx.h, screen.h)
 * compiles the header's DECLARATIONS only and links small forwarders (the
 * shims, from the library's *_api.h) that call through the table. So a
 * ysp.audio.Audio can be a movie's soundtrack, and a ysp.timeline.Timeline
 * can drive the movie's clock, with one copy of each implementation in the
 * process. This is NumPy's C-API pattern (a capsule); docs/bindings.md,
 * "Sharing one implementation across modules", has the rules.
 *
 *   Python  the owner module has an attribute _C_API, a capsule named
 *           "<module>._C_API" holding a pointer to the table.
 *           PyCapsule_Import() imports the owner and returns the pointer.
 *   MEX     the owner MEX file exports one function, ysp_<name>_api_table()
 *           (dllexport, or default visibility), that returns the table.
 *           The consumer loads the owner
 *           file from its own directory with LoadLibrary/dlopen, which
 *           returns the module MATLAB or Octave already loaded, so the
 *           handles the owner's own commands opened are the same objects.
 *
 * VERSIONS. A consumer refuses a table whose abi differs from its
 * YSP_SHARE_ABI, whose host differs, whose size is smaller than its own
 * table, whose header version has another major.minor than the one the
 * consumer compiled, or whose configuration values or struct sizes differ
 * (a configuration macro such as YTL_MAX_CHANNELS changes a size without
 * changing the version). A different patch number is accepted: a change to
 * a struct's layout or to a function a table carries bumps at least the
 * header's minor version (CLAUDE.md, "Changing a header"), so a patch
 * changes neither. Entries are only appended; a change to an existing
 * entry bumps YSP_SHARE_ABI.
 *
 * LIFETIME. acquire() returns the C object and pins it: the owner refuses to
 * close a pinned object (Python: BusyError; MEX: <module>:busy), and in
 * Python acquire also takes a strong reference to the owner's object. The
 * pointer stays valid until the consumer calls release() with it. release()
 * drops the pin (and the reference). A MEX owner whose handle table is
 * cleared (clear <module>, clear mex) while an object is pinned keeps that
 * object open; the last release() closes and frees it. The consumer keeps
 * its LoadLibrary/dlopen reference to the owner until it has released every
 * pin, so the owner's code stays mapped.
 *
 * THREADS. The table's functions keep the header's thread rules. acquire()
 * and release() run on the host's thread: in Python with the GIL held; in
 * MEX on the MATLAB/Octave thread.
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_SHARE_H
#define YSP_SHARE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define YSP_SHARE_ABI 1

#define YSP_SHARE_HOST_PYTHON 1   /* acquire's ref is a PyObject*            */
#define YSP_SHARE_HOST_MEX    2   /* acquire's ref is a const uint64_t*: the
                                   * handle the owner's 'open' returned      */

/* A forwarded call that ran a host callback which failed: Python's error
 * is set (the consumer holds the GIL and raises it). */
#define YSP_SHARE_ERR_HOST (-1000)

typedef struct ysp_share_head {
    uint32_t    abi;            /* YSP_SHARE_ABI of the owner                */
    uint32_t    size;           /* sizeof the owner's whole table            */
    uint32_t    host;           /* YSP_SHARE_HOST_*                          */
    uint32_t    reserved_;
    const char* library;        /* "audio", "timeline"                       */
    const char* version;        /* the header's version string, as compiled  */
    /* The C object behind a host reference, pinned (LIFETIME); NULL with a
     * message in err (no Python error is set) when ref is not an open
     * object of this library. */
    void* (*acquire)(void* ref, char* err, size_t cap);
    /* Drops one pin taken by acquire(); obj is the pointer it returned. */
    void  (*release)(void* obj);
} ysp_share_head;

#if defined(_WIN32) || defined(__CYGWIN__)
#define YSP_SHARE_EXPORT __declspec(dllexport)
#else
#define YSP_SHARE_EXPORT __attribute__((visibility("default")))
#endif

/* An owner MEX file's mexFunction. MinGW's linker exports every symbol only
 * when no symbol is dllexport'd, and Octave's mkoctfile relies on that for
 * mexFunction: with the table function exported, mexFunction must be too.
 * MATLAB's MSVC link names mexFunction itself (/EXPORT). */
#if defined(__MINGW32__)
#define YSP_SHARE_MEX_ENTRY __declspec(dllexport)
#else
#define YSP_SHARE_MEX_ENTRY
#endif

/* Inline, so a unit that does not call one has no unused-function warning. */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define YSP_SHARE_FN static inline
#else
#define YSP_SHARE_FN static __inline
#endif

/* The major and minor numbers of a version string "major.minor.patch";
 * 0, or -1 when v does not start with two numbers. */
YSP_SHARE_FN int ysp_share_major_minor(const char* v, unsigned long* major, unsigned long* minor) {
    unsigned long n[2] = { 0, 0 };
    int i, digits;
    if (!v) return -1;
    for (i = 0; i < 2; i++) {
        for (digits = 0; *v >= '0' && *v <= '9' && digits < 9; v++, digits++) n[i] = n[i] * 10 + (unsigned long)(*v - '0');
        if (digits == 0 || (*v >= '0' && *v <= '9')) return -1;
        if (i == 0 && *v++ != '.') return -1;
    }
    if (*v != '\0' && *v != '.') return -1;
    *major = n[0];
    *minor = n[1];
    return 0;
}

/* The checks every consumer runs before it uses a table (VERSIONS). */
YSP_SHARE_FN int ysp_share_check_head(const ysp_share_head* h, const char* library, const char* version,
                                      uint32_t host, size_t size, char* err, size_t cap) {
    if (!h) { snprintf(err, cap, "ysp_%s: no table", library); return -1; }
    if (h->abi != YSP_SHARE_ABI) {
        snprintf(err, cap, "ysp_%s: the owner's table has ABI %u, this module needs %u: install matching versions",
                 library, (unsigned)h->abi, (unsigned)YSP_SHARE_ABI);
        return -1;
    }
    if (!h->library || strcmp(h->library, library) != 0) {
        snprintf(err, cap, "ysp_%s: the table is for '%s'", library, h->library ? h->library : "?");
        return -1;
    }
    if (h->host != host) {
        snprintf(err, cap, "ysp_%s: the table is for host %u, this module is host %u", library, (unsigned)h->host,
                 (unsigned)host);
        return -1;
    }
    if (h->size < size) {
        snprintf(err, cap, "ysp_%s: the owner's table has %u bytes, this module needs %u: the owner is older",
                 library, (unsigned)h->size, (unsigned)size);
        return -1;
    }
    {
        unsigned long oma, omi, mma, mmi;
        if (ysp_share_major_minor(h->version, &oma, &omi) < 0 || ysp_share_major_minor(version, &mma, &mmi) < 0 ||
            oma != mma || omi != mmi) {
            snprintf(err, cap,
                     "ysp_%s: the owner compiled ysp/%s.h %s, this module was built against %s: the major.minor "
                     "versions must match (a patch may differ); install matching versions",
                     library, library, h->version ? h->version : "?", version);
            return -1;
        }
    }
    return 0;
}

YSP_SHARE_FN int ysp_share_check_size(const char* library, const char* what, uint32_t owner, size_t mine,
                                      char* err, size_t cap) {
    if ((size_t)owner == mine) return 0;
    snprintf(err, cap, "ysp_%s: sizeof(%s) is %u in the owner and %u here: the two were built with other settings",
             library, what, (unsigned)owner, (unsigned)mine);
    return -1;
}

/* ---- MEX: load the owner from this module's directory ---------------------- */
/* Define YSP_SHARE_MEX before including. On Linux and macOS the consumer
 * defines _GNU_SOURCE before its first include, for dladdr(). Returns the
 * owner's table function's result, or NULL with err. *module gets the
 * library handle; the consumer keeps it (LIFETIME) and may pass it to
 * ysp_share_mex_unload() after its last release(). */
#ifdef YSP_SHARE_MEX
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

YSP_SHARE_FN const void* ysp_share_mex_load(const char* owner, const char* symbol, void** module, char* err,
                                            size_t cap) {
    HMODULE self = NULL, m;
    wchar_t path[1024];
    DWORD n;
    size_t dir, ext, i, k, olen = strlen(owner);
    FARPROC f;
    *module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(const void*)&ysp_share_mex_load, &self)) {
        snprintf(err, cap, "%s: this module's own path is not known (error %lu)", owner, (unsigned long)GetLastError());
        return NULL;
    }
    n = GetModuleFileNameW(self, path, (DWORD)(sizeof path / sizeof path[0]));
    if (n == 0 || n >= sizeof path / sizeof path[0] - 1) { snprintf(err, cap, "%s: this module's path is too long", owner); return NULL; }
    for (dir = 0, i = 0; i < n; i++) if (path[i] == L'\\' || path[i] == L'/') dir = i + 1;
    for (ext = n, i = dir; i < n; i++) if (path[i] == L'.') ext = i;
    {
        wchar_t tail[32];
        size_t tl = n - ext;
        if (tl >= 32 || dir + olen + tl + 1 > sizeof path / sizeof path[0]) { snprintf(err, cap, "%s: path too long", owner); return NULL; }
        memcpy(tail, path + ext, tl * sizeof(wchar_t));
        for (k = 0; k < olen; k++) path[dir + k] = (wchar_t)(unsigned char)owner[k];
        memcpy(path + dir + olen, tail, tl * sizeof(wchar_t));
        path[dir + olen + tl] = 0;
    }
    /* A full path: Windows returns the module already loaded from it. */
    m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        snprintf(err, cap, "%s: %ls did not load (error %lu): build it beside this module", owner, path,
                 (unsigned long)GetLastError());
        return NULL;
    }
    f = GetProcAddress(m, symbol);
    if (!f) {
        snprintf(err, cap, "%s: %ls exports no %s: an older build", owner, path, symbol);
        FreeLibrary(m);
        return NULL;
    }
    *module = (void*)m;
    return ((const void* (*)(void))(void (*)(void))f)();
}

YSP_SHARE_FN void ysp_share_mex_unload(void* module) {
    if (module) FreeLibrary((HMODULE)module);
}
#else
#include <dlfcn.h>

YSP_SHARE_FN const void* ysp_share_mex_load(const char* owner, const char* symbol, void** module, char* err,
                                            size_t cap) {
    Dl_info info;
    char path[4096];
    const char *slash, *dot;
    size_t dir;
    void *m, *f;
    *module = NULL;
    if (!dladdr((void*)&ysp_share_mex_load, &info) || !info.dli_fname) {
        snprintf(err, cap, "%s: this module's own path is not known", owner);
        return NULL;
    }
    slash = strrchr(info.dli_fname, '/');
    dir = slash ? (size_t)(slash - info.dli_fname) + 1 : 0;
    dot = strrchr(info.dli_fname + dir, '.');
    if (!dot || dir + strlen(owner) + strlen(dot) + 1 > sizeof path) { snprintf(err, cap, "%s: bad module path", owner); return NULL; }
    memcpy(path, info.dli_fname, dir);
    snprintf(path + dir, sizeof path - dir, "%s%s", owner, dot);
    /* The same path: dlopen() returns the object already loaded. */
    m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m) { snprintf(err, cap, "%s: %s did not load: %s", owner, path, dlerror()); return NULL; }
    f = dlsym(m, symbol);
    if (!f) { snprintf(err, cap, "%s: %s exports no %s: an older build", owner, path, symbol); dlclose(m); return NULL; }
    *module = m;
    return ((const void* (*)(void))f)();
}

YSP_SHARE_FN void ysp_share_mex_unload(void* module) {
    if (module) dlclose(module);
}
#endif
#endif /* YSP_SHARE_MEX */

#endif /* YSP_SHARE_H */
