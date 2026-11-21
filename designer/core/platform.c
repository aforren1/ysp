/* platform.c - the designer core's only OS calls: files and folders, the
 * removal of a folder's tree (the preview's old data sets), and the atomics
 * of the check thread's shared counters (worker.c; the thread itself is
 * ysp/rt.h's yrt_pump).
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
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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

/* --- dsg_remove_tree() ------------------------------------------------------ */

/* Deeper trees are refused, not recursed into: a data set is two levels. */
#define RM_MAX_DEPTH 32

/* The names of rel, checked before anything is touched. A separator that
 * leads or doubles gives an empty name, so an absolute POSIX path is
 * refused here too; ':' refuses a Windows drive and a stream. */
static int rm_check_rel(const char* rel, char* err, size_t cap) {
    const char* p = rel;
    if (!rel || !rel[0]) return fail(err, cap, "not removed: %s%s", "", "the path is empty");
    if (strlen(rel) >= DSG_PATH_MAX) return fail(err, cap, "not removed: '%s': %s", rel, "the path is too long");
    if (strchr(rel, ':') || strchr(rel, '\\')) return fail(err, cap, "not removed: '%s': %s", rel, "not a relative path of names");
    for (;;) {
        const char* e = strchr(p, '/');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == 0) return fail(err, cap, "not removed: '%s': %s", rel, "an empty name (an absolute path, or '//')");
        if ((k == 1 && p[0] == '.') || (k == 2 && p[0] == '.' && p[1] == '.'))
            return fail(err, cap, "not removed: '%s': %s", rel, "'.' and '..' are not names");
        if (!e) return 0;
        p = e + 1;
    }
}

#if defined(_WIN32)
static bool rm_gone(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

static void rm_fail_w(char* err, size_t cap, int* rc, const char* what) {
    if (*rc == 0) {
        DWORD e = GetLastError();
        if (err && cap) snprintf(err, cap, "not removed: %s (Windows error %lu)", what, (unsigned long)e);
    }
    *rc = -1;
}

/* A name surrogate (a symbolic link, a junction, a mount point) names
 * another place. Other reparse points, such as a cloud folder's
 * placeholders, are the file itself, and a project can be in one. An entry
 * whose tag cannot be read counts as a link. */
static bool rm_is_link(const wchar_t* path, DWORD a) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    memset(&fd, 0, sizeof fd);
    h = FindFirstFileW(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    FindClose(h);
    return IsReparseTagNameSurrogate(fd.dwReserved0) != 0;
}

/* A link is removed as itself: RemoveDirectoryW() and DeleteFileW() do not
 * follow one. */
static int rm_tree_w(const wchar_t* path, int depth, char* err, size_t cap) {
    DWORD a = GetFileAttributesW(path);
    int rc = 0;
    bool link;
    if (a == INVALID_FILE_ATTRIBUTES) {
        if (rm_gone(GetLastError())) return 0;
        rm_fail_w(err, cap, &rc, "a file's attributes");
        return rc;
    }
    link = rm_is_link(path, a);
    /* a link's own attributes are not changed: that could reach its target */
    if (!link && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(path, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !link) {
        size_t n = wcslen(path);
        wchar_t* pat = (wchar_t*)malloc((n + 3) * sizeof(wchar_t));
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (!pat) return fail(err, cap, "not removed: %s%s", "", "out of memory");
        if (depth >= RM_MAX_DEPTH) {
            free(pat);
            return fail(err, cap, "not removed: %s%s", "", "the tree is too deep");
        }
        memcpy(pat, path, n * sizeof(wchar_t));
        pat[n] = L'\\';
        pat[n + 1] = L'*';
        pat[n + 2] = 0;
        h = FindFirstFileW(pat, &fd);
        free(pat);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                size_t k = wcslen(fd.cFileName);
                wchar_t* child;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                child = (wchar_t*)malloc((n + k + 2) * sizeof(wchar_t));
                if (!child) {
                    if (rc == 0) fail(err, cap, "not removed: %s%s", "", "out of memory");
                    rc = -1;
                    continue;
                }
                memcpy(child, path, n * sizeof(wchar_t));
                child[n] = L'\\';
                memcpy(child + n + 1, fd.cFileName, (k + 1) * sizeof(wchar_t));
                if (rm_tree_w(child, depth + 1, rc ? NULL : err, rc ? 0 : cap) != 0) rc = -1;
                free(child);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        } else if (!rm_gone(GetLastError())) {
            rm_fail_w(err, cap, &rc, "a folder's list");
        }
        if (!RemoveDirectoryW(path) && !rm_gone(GetLastError())) rm_fail_w(err, cap, &rc, "a folder");
    } else if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!RemoveDirectoryW(path) && !rm_gone(GetLastError())) rm_fail_w(err, cap, &rc, "a link to a folder");
    } else {
        if (!DeleteFileW(path) && !rm_gone(GetLastError())) rm_fail_w(err, cap, &rc, "a file");
    }
    return rc;
}

int dsg_remove_tree(const char* root, const char* rel, char* err, size_t cap) {
    char path[DSG_PATH_MAX];
    size_t at;
    wchar_t* w;
    int rc;
    if (!root || !root[0]) return fail(err, cap, "not removed: %s%s", "", "no root folder");
    if (rm_check_rel(rel, err, cap)) return -1;
    if (dsg_join(path, sizeof path, root, rel)) return fail(err, cap, "not removed: '%s': %s", rel, "the path is too long");
    /* each name on the way, the last one too: a link there would take the
     * removal out of root */
    at = strlen(path) - strlen(rel);
    for (;;) {
        char* e = strchr(path + at, '/');
        char keep = 0;
        DWORD a;
        bool link;
        if (e) {
            keep = *e;
            *e = 0;
        }
        w = wide(path);
        if (e) *e = keep;
        if (!w) return fail(err, cap, "not removed: '%s': %s", rel, "the path is not UTF-8");
        a = GetFileAttributesW(w);
        if (a == INVALID_FILE_ATTRIBUTES) {
            DWORD le = GetLastError();
            free(w);
            if (rm_gone(le)) return 0;   /* not there: nothing to remove */
            return fail(err, cap, "not removed: '%s': %s", rel, "its attributes cannot be read");
        }
        link = rm_is_link(w, a);
        free(w);
        if (link) return fail(err, cap, "not removed: '%s': %s", rel, "a link or a junction on the way");
        if (!e) break;
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
        at = (size_t)(e - path) + 1;
    }
    w = wide(path);
    if (!w) return fail(err, cap, "not removed: '%s': %s", rel, "the path is not UTF-8");
    rc = rm_tree_w(w, 0, err, cap);
    free(w);
    return rc;
}
#else
static int rm_fail_errno(char* err, size_t cap, int* rc, const char* what, const char* name) {
    if (*rc == 0 && err && cap) snprintf(err, cap, "not removed: %s '%s': %s", what, name, strerror(errno));
    *rc = -1;
    return -1;
}

/* name in the folder dirfd. fstatat() and openat() with AT_SYMLINK_NOFOLLOW
 * and O_NOFOLLOW: a link is unlinked as itself, and a folder that became a
 * link after the fstatat() fails the openat(). */
static int rm_at(int dirfd, const char* name, int depth, char* err, size_t cap) {
    struct stat st;
    int rc = 0, fd;
    DIR* d;
    struct dirent* e;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return 0;
        return rm_fail_errno(err, cap, &rc, "cannot read", name);
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlinkat(dirfd, name, 0) != 0 && errno != ENOENT) return rm_fail_errno(err, cap, &rc, "cannot remove", name);
        return 0;
    }
    if (depth >= RM_MAX_DEPTH) return fail(err, cap, "not removed: '%s': %s", name, "the tree is too deep");
    fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return rm_fail_errno(err, cap, &rc, "cannot open the folder", name);
    d = fdopendir(fd);
    if (!d) {
        close(fd);
        return rm_fail_errno(err, cap, &rc, "cannot list the folder", name);
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (rm_at(fd, e->d_name, depth + 1, rc ? NULL : err, rc ? 0 : cap) != 0) rc = -1;
    }
    closedir(d);   /* closes fd */
    if (unlinkat(dirfd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) rm_fail_errno(err, cap, &rc, "cannot remove the folder", name);
    return rc;
}

int dsg_remove_tree(const char* root, const char* rel, char* err, size_t cap) {
    char names[DSG_PATH_MAX];
    char* p;
    int dirfd, rc;
    if (!root || !root[0]) return fail(err, cap, "not removed: %s%s", "", "no root folder");
    if (rm_check_rel(rel, err, cap)) return -1;
    snprintf(names, sizeof names, "%s", rel);
    /* root itself is the caller's, links in it included */
    dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        if (errno == ENOENT) return 0;
        rc = 0;
        return rm_fail_errno(err, cap, &rc, "cannot open the root", root);
    }
    p = names;
    for (;;) {
        char* e = strchr(p, '/');
        struct stat st;
        int next;
        if (e) *e = 0;
        if (fstatat(dirfd, p, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            close(dirfd);
            if (errno == ENOENT || errno == ENOTDIR) return 0;   /* not there: nothing to remove */
            rc = 0;
            return rm_fail_errno(err, cap, &rc, "cannot read", rel);
        }
        if (S_ISLNK(st.st_mode)) {
            close(dirfd);
            return fail(err, cap, "not removed: '%s': %s", rel, "a symbolic link on the way");
        }
        if (!e) break;
        if (!S_ISDIR(st.st_mode)) {
            close(dirfd);
            return 0;
        }
        next = openat(dirfd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dirfd);
        if (next < 0) {
            rc = 0;
            return rm_fail_errno(err, cap, &rc, "cannot open the folder", rel);
        }
        dirfd = next;
        p = e + 1;
    }
    rc = rm_at(dirfd, p, 0, err, cap);
    close(dirfd);
    return rc;
}
#endif

/* C11's <stdatomic.h> needs /experimental:c11atomics on MSVC, so the
 * compilers' own intrinsics. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
int32_t dsg_atomic_add(volatile int32_t* v, int32_t d) { return (int32_t)_InterlockedExchangeAdd((volatile long*)v, (long)d) + d; }
int32_t dsg_atomic_load(const volatile int32_t* v) { return (int32_t)_InterlockedCompareExchange((volatile long*)(uintptr_t)v, 0, 0); }
#else
int32_t dsg_atomic_add(volatile int32_t* v, int32_t d) { return __atomic_add_fetch(v, d, __ATOMIC_ACQ_REL); }
int32_t dsg_atomic_load(const volatile int32_t* v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
#endif
