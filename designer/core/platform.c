/* platform.c - the designer core's only OS calls: files and folders, the
 * removal of a folder's tree (the preview's old data sets), the atomics
 * of the check thread's shared counters (worker.c; the thread itself is
 * ysp/rt.h's yrt_pump), and the player's process (dsg_proc_*).
 * Paths are UTF-8 everywhere; Windows takes them as UTF-16. The POSIX branch
 * also serves emscripten, whose file system is in memory. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1       /* posix_spawn_file_actions_addclosefrom_np(), pipe2() */
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

/* --- the process (dsg_proc_*) ------------------------------------------------ */

/* The watch thread's period: the sizes are read, and without a pidfd the
 * exit is seen, at most this long after it happens (docs/designer.md, "Part
 * X as built": the measured wake). The UI draws a run's progress at 2 Hz. */
#define PROC_TICK_MS 250

#if defined(__EMSCRIPTEN__)

struct dsg_proc {
    int unused;
};

int dsg_proc_start(dsg_proc** out, const dsg_proc_desc* d, char* err, size_t cap) {
    (void)d;
    if (out) *out = NULL;
    if (err && cap) snprintf(err, cap, "%s", "no processes in a browser");
    return -1;
}
void dsg_proc_stop(dsg_proc* p) { (void)p; }
void dsg_proc_kill(dsg_proc* p) { (void)p; }
int dsg_proc_poll(dsg_proc* p, int* status) {
    (void)p;
    if (status) *status = -1;
    return 1;
}
void dsg_proc_free(dsg_proc* p) { (void)p; }

#else

#if defined(_WIN32)
typedef SRWLOCK proc_lock;
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#else
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
extern char** environ;
typedef pthread_mutex_t proc_lock;
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#endif

struct dsg_proc {
    /* set before the watch thread starts, then read only */
    int nwatch;              /* the log, then desc->watch */
#if defined(_WIN32)
    wchar_t** watch;
#else
    char** watch;
#endif
    int64_t* size;           /* the watch thread's alone after the start */
    void (*wake)(void* ctx);
    void* ctx;

    /* lock: the exit. Reaping and signaling under one lock: a pid is never
     * signaled after its reap, when another process may have it. */
    proc_lock lock;
    int ended, status;
    /* gate: the calls of wake, and free's decision. Taken before lock. */
    proc_lock gate;
    int gone;                /* free was called: no more wake */
    int detach;              /* free left it running: the thread frees p */

#if defined(_WIN32)
    HANDLE proc, thread, stop_ev;
    DWORD pid;
#else
    pid_t pid;
    int pidfd;               /* -1: none (not Linux, or before 5.3) */
    int stop_rd, stop_wr;    /* free writes a byte */
    pthread_t thread;
#endif
};

static int64_t proc_size(const dsg_proc* p, int i) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fa;
    memset(&fa, 0, sizeof fa);
    if (!GetFileAttributesExW(p->watch[i], GetFileExInfoStandard, &fa)) return -1;
    return (int64_t)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
#else
    struct stat st;
    if (stat(p->watch[i], &st) != 0) return -1;
    return (int64_t)st.st_size;
#endif
}

static void proc_destroy(dsg_proc* p) {
    int i;
    if (!p) return;
    for (i = 0; p->watch && i < p->nwatch; i++) free(p->watch[i]);
    free(p->watch);
    free(p->size);
#if defined(_WIN32)
    if (p->proc) CloseHandle(p->proc);
    if (p->thread) CloseHandle(p->thread);
    if (p->stop_ev) CloseHandle(p->stop_ev);
#else
    if (p->pidfd >= 0) close(p->pidfd);
    if (p->stop_rd >= 0) close(p->stop_rd);
    if (p->stop_wr >= 0) close(p->stop_wr);
    pthread_mutex_destroy(&p->lock);
    pthread_mutex_destroy(&p->gate);
#endif
    free(p);
}

/* The struct with its copies of the watched paths, the log first. */
static dsg_proc* proc_new(const dsg_proc_desc* d) {
    dsg_proc* p = (dsg_proc*)calloc(1, sizeof *p);
    int n = 1, i;
    if (!p) return NULL;
#if defined(_WIN32)
    InitializeSRWLock(&p->lock);
    InitializeSRWLock(&p->gate);
#else
    p->pidfd = p->stop_rd = p->stop_wr = -1;
    pthread_mutex_init(&p->lock, NULL);
    pthread_mutex_init(&p->gate, NULL);
#endif
    while (d->watch && d->watch[n - 1]) n++;
    p->watch = calloc((size_t)n, sizeof *p->watch);
    p->size = (int64_t*)calloc((size_t)n, sizeof *p->size);
    if (!p->watch || !p->size) { proc_destroy(p); return NULL; }
    p->nwatch = n;
    for (i = 0; i < n; i++) {
        const char* s = i ? d->watch[i - 1] : d->out_path;
#if defined(_WIN32)
        p->watch[i] = wide(s);
#else
        size_t k = strlen(s) + 1;
        p->watch[i] = (char*)malloc(k);
        if (p->watch[i]) memcpy(p->watch[i], s, k);
#endif
        if (!p->watch[i]) { proc_destroy(p); return NULL; }
    }
    p->wake = d->wake;
    p->ctx = d->ctx;
    return p;
}

/* One look at the sizes; 1 when one changed. */
static int proc_grew(dsg_proc* p) {
    int i, changed = 0;
    for (i = 0; i < p->nwatch; i++) {
        int64_t s = proc_size(p, i);
        if (s != p->size[i]) {
            p->size[i] = s;
            changed = 1;
        }
    }
    return changed;
}

static void proc_wake(dsg_proc* p) {
    LOCK(&p->gate);
    if (!p->gone && p->wake) p->wake(p->ctx);
    UNLOCK(&p->gate);
}

#if defined(_WIN32)

/* The status of an ended process, under lock. */
static void proc_reap(dsg_proc* p) {
    DWORD code = 0;
    if (p->ended || WaitForSingleObject(p->proc, 0) != WAIT_OBJECT_0) return;
    if (!GetExitCodeProcess(p->proc, &code)) code = (DWORD)-1;
    p->ended = 1;
    p->status = (int)code;
}

static DWORD WINAPI proc_watch(LPVOID arg) {
    dsg_proc* p = (dsg_proc*)arg;
    HANDLE h[2];
    h[0] = p->stop_ev;
    h[1] = p->proc;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, h, FALSE, PROC_TICK_MS);
        int ended;
        if (r == WAIT_OBJECT_0) break;   /* free */
        if (r == WAIT_FAILED) Sleep(PROC_TICK_MS);
        LOCK(&p->lock);
        proc_reap(p);
        ended = p->ended;
        UNLOCK(&p->lock);
        if (proc_grew(p) || ended) proc_wake(p);
        if (ended) break;
    }
    return 0;
}

/* The command line that the C runtime splits back into argv (the rules of
 * CommandLineToArgvW()): an argument with a space, a tab, a quote or
 * nothing in it is quoted; in it, a quote is escaped, and the backslashes
 * before a quote, the closing one too, are doubled. Other backslashes stay
 * as they are. */
static char* proc_command_line(const char* const* argv) {
    size_t cap = 1, at = 0;
    int i;
    char* s;
    for (i = 0; argv[i]; i++) cap += 2 * strlen(argv[i]) + 3;
    s = (char*)malloc(cap);
    if (!s) return NULL;
    for (i = 0; argv[i]; i++) {
        const char* a = argv[i];
        size_t bs = 0;
        if (i) s[at++] = ' ';
        if (a[0] && !strpbrk(a, " \t\n\v\"")) {
            size_t k = strlen(a);
            memcpy(s + at, a, k);
            at += k;
            continue;
        }
        s[at++] = '"';
        for (; *a; a++) {
            if (*a == '\\') {
                bs++;
                continue;
            }
            if (*a == '"') bs = 2 * bs + 1;
            while (bs) {
                s[at++] = '\\';
                bs--;
            }
            s[at++] = *a;
        }
        bs *= 2;
        while (bs) {
            s[at++] = '\\';
            bs--;
        }
        s[at++] = '"';
    }
    s[at] = 0;
    return s;
}

int dsg_proc_start(dsg_proc** out, const dsg_proc_desc* d, char* err, size_t cap) {
    dsg_proc* p = NULL;
    char* cl = NULL;
    wchar_t *wcl = NULL, *wprog = NULL, *wcwd = NULL;
    HANDLE logh = INVALID_HANDLE_VALUE, nul = INVALID_HANDLE_VALUE, list[2];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T alen = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    int rc = -1, i;
    if (out) *out = NULL;
    if (!out || !d || !d->argv || !d->argv[0] || !d->argv[0][0] || !d->out_path || !d->out_path[0])
        return fail(err, cap, "cannot start: %s%s", "", "no program or no log path");
    if (d->cwd && !(d->argv[0][0] && d->argv[0][1] == ':') && d->argv[0][0] != '\\' && d->argv[0][0] != '/')
        return fail(err, cap, "cannot start '%s': %s", d->argv[0], "with a working folder, the program's path must be absolute");
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    p = proc_new(d);
    cl = proc_command_line(d->argv);
    wcl = cl ? wide(cl) : NULL;
    wprog = wide(d->argv[0]);
    wcwd = d->cwd ? wide(d->cwd) : NULL;
    for (i = 0; d->argv[i]; i++) {
        wchar_t* w = wide(d->argv[i]);
        if (!w) {
            fail(err, cap, "cannot start '%s': %s", d->argv[0], "an argument is not UTF-8");
            goto done;
        }
        free(w);
    }
    if (!p || !wcl || !wprog || (d->cwd && !wcwd)) {
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "out of memory, or a path is not UTF-8");
        goto done;
    }
    p->stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!p->stop_ev) {
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "no event");
        goto done;
    }
    /* inheritable, as the handle list needs; only the list is inherited */
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    logh = CreateFileW(p->watch[0], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (logh == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        fail(err, cap, "cannot make the log '%s': %s", d->out_path,
             e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? "it exists" : "the file cannot be made");
        goto done;
    }
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "NUL cannot be opened");
        goto done;
    }
    list[0] = nul;
    list[1] = logh;
    InitializeProcThreadAttributeList(NULL, 1, 0, &alen);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(alen);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &alen)) {
        free(attrs);
        attrs = NULL;
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "no attribute list");
        goto done;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof list, NULL, NULL)) {
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "the handle list was refused");
        goto done;
    }
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = logh;
    si.StartupInfo.hStdError = logh;
    si.lpAttributeList = attrs;
    /* CREATE_NO_WINDOW: a console program gets no console window, and the
     * designer's console's Ctrl+C does not reach it */
    if (!CreateProcessW(wprog, wcl, NULL, NULL, TRUE, CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
                        NULL, wcwd, &si.StartupInfo, &pi)) {
        char why[64];
        snprintf(why, sizeof why, "Windows error %lu", (unsigned long)GetLastError());
        fail(err, cap, "cannot start '%s': %s", d->argv[0], why);
        goto done;
    }
    CloseHandle(pi.hThread);
    p->proc = pi.hProcess;
    p->pid = pi.dwProcessId;
    /* the designer has the foreground when Run is clicked; the player needs
     * it (rig_spec 6) */
    AllowSetForegroundWindow(pi.dwProcessId);
    for (i = 0; i < p->nwatch; i++) p->size[i] = proc_size(p, i);
    p->thread = CreateThread(NULL, 0, proc_watch, p, 0, NULL);
    if (!p->thread) {
        TerminateProcess(p->proc, (UINT)-9);
        WaitForSingleObject(p->proc, INFINITE);
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "no watch thread");
        goto done;
    }
    *out = p;
    p = NULL;
    rc = 0;
done:
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
    /* a log that no process writes is not left: the next start may use the name */
    if (rc != 0 && logh != INVALID_HANDLE_VALUE) DeleteFileW(p->watch[0]);
    proc_destroy(p);
    free(cl);
    free(wcl);
    free(wprog);
    free(wcwd);
    return rc;
}

static BOOL CALLBACK proc_close_window(HWND h, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(h, &owner);
    if (owner == (DWORD)pid) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

/* The handle keeps the process's ID from reuse while p holds it. */
void dsg_proc_stop(dsg_proc* p) {
    if (!p) return;
    LOCK(&p->lock);
    proc_reap(p);
    if (!p->ended) EnumWindows(proc_close_window, (LPARAM)p->pid);
    UNLOCK(&p->lock);
}

void dsg_proc_kill(dsg_proc* p) {
    if (!p) return;
    LOCK(&p->lock);
    proc_reap(p);
    if (!p->ended) TerminateProcess(p->proc, (UINT)-9);
    UNLOCK(&p->lock);
}

int dsg_proc_poll(dsg_proc* p, int* status) {
    int ended;
    if (!p) {
        if (status) *status = -1;
        return 1;
    }
    LOCK(&p->lock);
    proc_reap(p);
    ended = p->ended;
    if (ended && status) *status = p->status;
    UNLOCK(&p->lock);
    return ended;
}

/* A process with no thread of ours left needs nothing: Windows keeps no
 * zombie, and closing the handle leaves it running. */
void dsg_proc_free(dsg_proc* p) {
    if (!p) return;
    LOCK(&p->gate);
    p->gone = 1;
    UNLOCK(&p->gate);
    SetEvent(p->stop_ev);
    WaitForSingleObject(p->thread, INFINITE);
    proc_destroy(p);
}

#else /* POSIX */

/* The status of an ended child, under lock (or by the thread alone after a
 * free). */
static void proc_reap(dsg_proc* p, int block) {
    int st = 0;
    pid_t r;
    if (p->ended) return;
    do r = waitpid(p->pid, &st, block ? 0 : WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == p->pid) {
        p->ended = 1;
        p->status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? -WTERMSIG(st) : -1;
    } else if (r < 0) {
        /* ECHILD: reaped by someone else (SIGCHLD ignored); no status */
        p->ended = 1;
        p->status = -1;
    }
}

static void* proc_watch(void* arg) {
    dsg_proc* p = (dsg_proc*)arg;
    int detach, ended = 0;
    for (;;) {
        struct pollfd f[2];
        nfds_t n = 1;
        int stop;
        memset(f, 0, sizeof f);
        f[0].fd = p->stop_rd;
        f[0].events = POLLIN;
        if (p->pidfd >= 0) {
            f[1].fd = p->pidfd;
            f[1].events = POLLIN;
            n = 2;
        }
        if (poll(f, n, PROC_TICK_MS) < 0 && errno != EINTR) {
            /* never spin on a poll that fails at once */
            struct timespec ts = {0, PROC_TICK_MS * 1000000L};
            nanosleep(&ts, NULL);
        }
        stop = f[0].revents != 0;
        LOCK(&p->lock);
        proc_reap(p, 0);
        ended = p->ended;
        UNLOCK(&p->lock);
        if (stop) break;
        if (proc_grew(p) || ended) proc_wake(p);
        if (ended) break;
    }
    LOCK(&p->gate);
    detach = p->detach;
    UNLOCK(&p->gate);
    if (!detach) return NULL;
    /* free left the child running and returned: p is this thread's. Block
     * until the child ends, so that it is no zombie. */
    proc_reap(p, 1);
    proc_destroy(p);
    return NULL;
}

int dsg_proc_start(dsg_proc** out, const dsg_proc_desc* d, char* err, size_t cap) {
    dsg_proc* p = NULL;
    int logfd = -1, rc, i, fa_ok = 0, at_ok = 0, sfd[2] = {-1, -1};
    short flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
    pid_t pid = 0;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    sigset_t none, all;
    pthread_attr_t ta;
    struct sched_param sp;
    if (out) *out = NULL;
    if (!out || !d || !d->argv || !d->argv[0] || !d->argv[0][0] || !d->out_path || !d->out_path[0])
        return fail(err, cap, "cannot start: %s%s", "", "no program or no log path");
    /* the program is found from the designer's folder on every system;
     * posix_spawn() would look from cwd, CreateProcessW() from ours */
    if (d->cwd && d->argv[0][0] != '/')
        return fail(err, cap, "cannot start '%s': %s", d->argv[0], "with a working folder, the program's path must be absolute");
    p = proc_new(d);
    if (!p) return fail(err, cap, "cannot start '%s': %s", d->argv[0], "out of memory");
    /* O_EXCL: an old run's log is never written over */
    logfd = open(d->out_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (logfd < 0) {
        fail(err, cap, "cannot make the log '%s': %s", d->out_path, errno == EEXIST ? "it exists" : strerror(errno));
        proc_destroy(p);
        return -1;
    }
#if defined(__linux__)
    rc = pipe2(sfd, O_CLOEXEC | O_NONBLOCK);
#else
    rc = pipe(sfd);
#endif
    if (rc == 0) {
        p->stop_rd = sfd[0];
        p->stop_wr = sfd[1];
    }
#if !defined(__linux__)
    if (rc == 0 && (fcntl(sfd[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(sfd[1], F_SETFD, FD_CLOEXEC) != 0 ||
                    fcntl(sfd[1], F_SETFL, O_NONBLOCK) != 0))
        rc = -1;
#endif
    if (rc != 0) {
        fail(err, cap, "cannot start '%s': %s", d->argv[0], strerror(errno));
        goto fail_log;
    }
    rc = posix_spawn_file_actions_init(&fa);
    fa_ok = rc == 0;
    if (!rc) rc = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, logfd, 1);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, logfd, 2);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    /* the designer's other descriptors (a font, a pack, the X connection,
     * the check thread's pipe) are not the player's */
    if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
    if (!rc && d->cwd) {
#if (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))) || defined(__APPLE__)
        rc = posix_spawn_file_actions_addchdir_np(&fa, d->cwd);
#else
        rc = ENOSYS;
#endif
    }
    if (!rc) rc = posix_spawnattr_init(&at);
    at_ok = rc == 0;
    /* each signal at its default action and none blocked: the designer's
     * ignored SIGPIPE would otherwise reach the player */
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    sigemptyset(&none);
    if (!rc) rc = posix_spawnattr_setsigdefault(&at, &all);
    if (!rc) rc = posix_spawnattr_setsigmask(&at, &none);
    if (!rc) rc = posix_spawnattr_setpgroup(&at, 0);
#if defined(POSIX_SPAWN_SETSCHEDULER)
    /* an elevated designer thread's SCHED_FIFO is never the player's
     * (docs/rt.md, "Inherited policy"); the player elevates its own frame
     * thread */
    flags = (short)(flags | POSIX_SPAWN_SETSCHEDULER);
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 0;
    if (!rc) rc = posix_spawnattr_setschedpolicy(&at, SCHED_OTHER);
    if (!rc) rc = posix_spawnattr_setschedparam(&at, &sp);
#endif
#if defined(__APPLE__)
    flags = (short)(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    if (!rc) rc = posix_spawnattr_setflags(&at, flags);
    if (!rc) rc = posix_spawn(&pid, d->argv[0], &fa, &at, (char* const*)d->argv, environ);
    if (fa_ok) posix_spawn_file_actions_destroy(&fa);
    if (at_ok) posix_spawnattr_destroy(&at);
    if (rc) {
        fail(err, cap, "cannot start '%s': %s", d->argv[0], strerror(rc));
        goto fail_log;
    }
    close(logfd);
    p->pid = pid;
#if defined(__linux__) && defined(SYS_pidfd_open)
    /* the exit wakes the watch thread at once; the child is not reaped yet,
     * so the pid is still its. ENOSYS before Linux 5.3: the tick sees it. */
    p->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
#endif
    for (i = 0; i < p->nwatch; i++) p->size[i] = proc_size(p, i);
    /* explicit SCHED_OTHER: a thread made by an elevated thread inherits
     * SCHED_FIFO otherwise (docs/rt.md, "Inherited policy") */
    rc = pthread_attr_init(&ta);
    if (!rc) {
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = 0;
        rc = pthread_attr_setinheritsched(&ta, PTHREAD_EXPLICIT_SCHED);
        if (!rc) rc = pthread_attr_setschedpolicy(&ta, SCHED_OTHER);
        if (!rc) rc = pthread_attr_setschedparam(&ta, &sp);
        if (!rc) rc = pthread_create(&p->thread, &ta, proc_watch, p);
        pthread_attr_destroy(&ta);
    }
    if (rc) {
        kill(pid, SIGKILL);
        proc_reap(p, 1);
        fail(err, cap, "cannot start '%s': %s", d->argv[0], "no watch thread");
        proc_destroy(p);
        return -1;
    }
    *out = p;
    return 0;
fail_log:
    close(logfd);
    unlink(d->out_path);   /* no process writes it: the next start may use the name */
    proc_destroy(p);
    return -1;
}

void dsg_proc_stop(dsg_proc* p) {
    if (!p) return;
    LOCK(&p->lock);
    proc_reap(p, 0);
    if (!p->ended) kill(p->pid, SIGTERM);
    UNLOCK(&p->lock);
}

void dsg_proc_kill(dsg_proc* p) {
    if (!p) return;
    LOCK(&p->lock);
    proc_reap(p, 0);
    if (!p->ended) kill(p->pid, SIGKILL);
    UNLOCK(&p->lock);
}

int dsg_proc_poll(dsg_proc* p, int* status) {
    int ended;
    if (!p) {
        if (status) *status = -1;
        return 1;
    }
    LOCK(&p->lock);
    proc_reap(p, 0);
    ended = p->ended;
    if (ended && status) *status = p->status;
    UNLOCK(&p->lock);
    return ended;
}

void dsg_proc_free(dsg_proc* p) {
    pthread_t t;
    int detach;
    ssize_t w;
    if (!p) return;
    t = p->thread;
    LOCK(&p->gate);
    p->gone = 1;
    LOCK(&p->lock);
    proc_reap(p, 0);
    detach = p->detach = !p->ended;
    UNLOCK(&p->lock);
    /* inside the gate: a detached thread frees p only after it takes it */
    do w = write(p->stop_wr, "x", 1);
    while (w < 0 && errno == EINTR);
    if (detach) pthread_detach(t);
    UNLOCK(&p->gate);
    if (detach) return;
    pthread_join(t, NULL);
    proc_destroy(p);
}

#endif /* POSIX */
#endif /* not emscripten */
