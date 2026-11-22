/* platform_test.c - dsg_remove_tree() (designer/core/platform.c; docs/
 * designer.md, "Part S as built") and the process, dsg_proc_* ("Part X as
 * built"). dsg_remove_tree() deletes files, so every path it touches is
 * under WORK/remove_tree, and every refusal it checks is one that, if it
 * did not hold, would delete only inside that folder: a ".." from the root
 * reaches the sandbox and no further, and the absolute path and the links
 * name the sandbox's own "outside" folder. The process part starts only
 * CHILD (tests/designer/test_child.c), its logs in WORK/proc.
 *
 * Usage: test_designer_platform WORK [CHILD [--measure]] (WORK an absolute
 * folder). Exit 0 on pass. Without CHILD the process part is skipped.
 * --measure runs only the process measurements and prints them.
 *
 *   sandbox/
 *     root/            the root given to dsg_remove_tree()
 *       a/f1, a/sub/f2, a/sub/deep/f3
 *       a/esc       -> ../../outside   a link inside the tree, out of root
 *       a/esc_file  -> ../../outside/keep
 *       b/f4                           root's other child
 *       file5                          a file as rel
 *       link_out    -> ../outside      a link as rel, and on the way
 *       link_in     -> b               a link inside root, as rel
 *     outside/keep                     what the links point at
 *     sibling/keep                     root's sibling
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   /* nftw(), symlink() under -std=c11 */
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* RLIMIT_RTPRIO */
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
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char box[DSG_PATH_MAX], root[DSG_PATH_MAX];

static const char* at(const char* rel) {
    static char p[4][DSG_PATH_MAX];
    static int k;
    k = (k + 1) & 3;
    if (dsg_join(p[k], sizeof p[k], box, rel)) {
        printf("FAIL: path too long\n");
        exit(1);
    }
    return p[k];
}

static bool exists(const char* rel) { return dsg_is_dir(at(rel)) || dsg_is_file(at(rel)); }

static void put(const char* rel) {
    char err[512];
    if (dsg_write_file(at(rel), "x", 1, err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }
}

static void mkd(const char* rel) {
    char err[512];
    if (dsg_make_dir(at(rel), err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }
}

#if !defined(_WIN32)
static bool is_link(const char* rel) {
    struct stat st;
    return lstat(at(rel), &st) == 0 && S_ISLNK(st.st_mode);
}

static void link_to(const char* target, const char* rel) {
    if (symlink(target, at(rel)) != 0) {
        printf("FAIL: setup: symlink %s\n", rel);
        exit(1);
    }
}

/* The test's own cleaner, so a mutant of dsg_remove_tree() cannot make the
 * setup of the next run follow a link: nftw() with FTW_PHYS never does. */
static int rm_one(const char* p, const struct stat* st, int flag, struct FTW* f) {
    (void)st;
    (void)f;
    return flag == FTW_DP ? rmdir(p) : unlink(p);
}
static void clean_box(void) {
    if (dsg_is_dir(box) && nftw(box, rm_one, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        printf("FAIL: cannot clean %s\n", box);
        exit(1);
    }
}
#else
static void clean_box(void) {
    char err[512], parent[DSG_PATH_MAX];
    const char* slash = strrchr(box, '/');
    if (!dsg_is_dir(box) || !slash) return;
    snprintf(parent, sizeof parent, "%.*s", (int)(slash - box), box);
    if (dsg_remove_tree(parent, slash + 1, err, sizeof err)) {
        printf("FAIL: cannot clean %s: %s\n", box, err);
        exit(1);
    }
}
#endif

static void setup(void) {
    char err[512] = "";
    clean_box();
    if (dsg_make_dir(box, err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }
    mkd("root");
    mkd("root/a");
    mkd("root/a/sub");
    mkd("root/a/sub/deep");
    mkd("root/b");
    mkd("outside");
    mkd("sibling");
    put("root/a/f1");
    put("root/a/sub/f2");
    put("root/a/sub/deep/f3");
    put("root/b/f4");
    put("root/file5");
    put("outside/keep");
    put("sibling/keep");
#if !defined(_WIN32)
    link_to("../../outside", "root/a/esc");
    link_to("../../outside/keep", "root/a/esc_file");
    link_to("../outside", "root/link_out");
    link_to("b", "root/link_in");
#endif
}

/* Every file of the setup outside root/a is still there. */
static bool others_whole(void) {
    return exists("outside/keep") && exists("sibling/keep") && exists("root/b/f4") && exists("root/file5") && dsg_is_dir(root);
}

static void refused(const char* r, const char* why, int line) {
    char err[512] = "";
    int rc = dsg_remove_tree(root, r, err, sizeof err);
    checks++;
    if (rc != -1 || !err[0]) {
        fails++;
        printf("FAIL line %d: '%s' (%s) gave %d, '%s'\n", line, r ? r : "(null)", why, rc, err);
    }
    checks++;
    if (!others_whole() || !exists("root/a/f1") || !exists("root/a/sub/deep/f3")) {
        fails++;
        printf("FAIL line %d: '%s' (%s) removed something\n", line, r ? r : "(null)", why);
    }
}
#define REFUSED(r, why) refused(r, why, __LINE__)

/* --- the process (dsg_proc_*) ----------------------------------------------- */

#if defined(_WIN32)
typedef SRWLOCK t_lock;
static void t_lock_init(t_lock* l) { InitializeSRWLock(l); }
static void t_lock_take(t_lock* l) { AcquireSRWLockExclusive(l); }
static void t_lock_give(t_lock* l) { ReleaseSRWLockExclusive(l); }
static long long t_now(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
static void t_sleep(long ms) { Sleep((DWORD)ms); }
static long long t_cpu_ns(void) {
    FILETIME a, b, k, u;
    ULARGE_INTEGER x, y;
    if (!GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u)) return 0;
    x.LowPart = k.dwLowDateTime;
    x.HighPart = k.dwHighDateTime;
    y.LowPart = u.dwLowDateTime;
    y.HighPart = u.dwHighDateTime;
    return (long long)(x.QuadPart + y.QuadPart) * 100;
}
#else
typedef pthread_mutex_t t_lock;
static void t_lock_init(t_lock* l) { pthread_mutex_init(l, NULL); }
static void t_lock_take(t_lock* l) { pthread_mutex_lock(l); }
static void t_lock_give(t_lock* l) { pthread_mutex_unlock(l); }
static long long t_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void t_sleep(long ms) {
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}
static long long t_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
#endif

/* What the watch thread's wake calls record. */
#define WAKE_MAX 4096
typedef struct waker {
    t_lock lock;
    int n;
    long long t[WAKE_MAX];
    int policy;   /* the watch thread's, at its first wake; -1 none */
} waker;

static void on_wake(void* ctx) {
    waker* w = (waker*)ctx;
    long long now = t_now();
    t_lock_take(&w->lock);
    if (w->n < WAKE_MAX) w->t[w->n] = now;
    if (w->n == 0) {
#if defined(_WIN32)
        w->policy = 0;
#else
        struct sched_param sp;
        int pol = -1;
        memset(&sp, 0, sizeof sp);
        if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0) w->policy = pol;
#endif
    }
    w->n++;
    t_lock_give(&w->lock);
}

static void waker_init(waker* w) {
    memset(w, 0, sizeof *w);
    t_lock_init(&w->lock);
    w->policy = -1;
}

static int waker_count(waker* w) {
    int n;
    t_lock_take(&w->lock);
    n = w->n;
    t_lock_give(&w->lock);
    return n;
}

/* The first wake at or after t, minus t, in ns; -1 when none. */
static long long waker_after(waker* w, long long t) {
    long long r = -1;
    int i;
    t_lock_take(&w->lock);
    for (i = 0; i < w->n && i < WAKE_MAX; i++)
        if (w->t[i] >= t) {
            r = w->t[i] - t;
            break;
        }
    t_lock_give(&w->lock);
    return r;
}

static char pdir[DSG_PATH_MAX], child_path[DSG_PATH_MAX];

static const char* ppath(const char* name) {
    static char p[4][DSG_PATH_MAX];
    static int k;
    k = (k + 1) & 3;
    if (dsg_join(p[k], sizeof p[k], pdir, name)) {
        printf("FAIL: path too long\n");
        exit(1);
    }
    return p[k];
}

/* Starts CHILD with args (NULL-ended, after the program) and its log at
 * WORK/proc/NAME.log; NULL with err filled when it fails. */
static dsg_proc* start_child(const char* name, const char* const* args, const char* cwd, const char* const* watch, waker* w,
                             char* err, size_t cap) {
    const char* argv[24];
    dsg_proc_desc d;
    dsg_proc* p = NULL;
    int i, n = 0;
    argv[n++] = child_path;
    for (i = 0; args && args[i] && n < 23; i++) argv[n++] = args[i];
    argv[n] = NULL;
    memset(&d, 0, sizeof d);
    d.argv = argv;
    d.cwd = cwd;
    d.out_path = ppath(name);
    d.watch = watch;
    d.wake = w ? on_wake : NULL;
    d.ctx = w;
    if (dsg_proc_start(&p, &d, err, cap) != 0) return NULL;
    return p;
}

/* 1 when ended within ms; status filled. Sleeps between polls. */
static int wait_end(dsg_proc* p, long ms, int* status) {
    long long end = t_now() + (long long)ms * 1000000LL;
    for (;;) {
        if (dsg_proc_poll(p, status)) return 1;
        if (t_now() > end) return 0;
        t_sleep(5);
    }
}

/* The log's text (static buffer), "" when it cannot be read. */
static const char* log_text(const char* name) {
    static char buf[65536];
    char* data = NULL;
    size_t n = 0;
    char err[512];
    buf[0] = 0;
    if (dsg_read_file(ppath(name), &data, &n, err, sizeof err) == 0) {
        snprintf(buf, sizeof buf, "%s", data);
        free(data);
    }
    return buf;
}

static int wait_log(const char* name, const char* needle, long ms) {
    long long end = t_now() + (long long)ms * 1000000LL;
    while (!strstr(log_text(name), needle)) {
        if (t_now() > end) return 0;
        t_sleep(5);
    }
    return 1;
}

/* 1 when wake was called at least once within ms. */
static int wait_wake(waker* w, long ms) {
    long long end = t_now() + (long long)ms * 1000000LL;
    while (waker_count(w) == 0) {
        if (t_now() > end) return 0;
        t_sleep(5);
    }
    return 1;
}

static void hex_line(char* out, size_t cap, const char* a) {
    size_t at = (size_t)snprintf(out, cap, "arg=");
    for (; *a && at + 3 < cap; a++) at += (size_t)snprintf(out + at, cap - at, "%02x", (unsigned)(unsigned char)*a);
    snprintf(out + at, cap - at, ";\n");
}

#if !defined(_WIN32)
/* A zombie child of this process: one that waitid() could reap now. */
static int have_zombie(void) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    if (waitid(P_ALL, 0, &si, WEXITED | WNOHANG | WNOWAIT) != 0) return 0;   /* ECHILD: no child at all */
    return si.si_pid != 0;
}
#endif

static void proc_tests(const char* work, const char* child) {
    char err[512] = "";
    int st = 0;
    dsg_proc* p;
    waker* w = (waker*)malloc(sizeof *w);
    if (!w || dsg_join(pdir, sizeof pdir, work, "proc") || snprintf(child_path, sizeof child_path, "%s", child) >= (int)sizeof child_path) {
        printf("FAIL: setup of the process part\n");
        exit(1);
    }
    dsg_remove_tree(work, "proc", err, sizeof err);
    if (dsg_make_dir(pdir, err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }

    /* the exit code, and stdout and stderr in the log */
    {
        const char* a[] = {"exit", "7", NULL};
        waker_init(w);
        p = start_child("exit.log", a, NULL, NULL, w, err, sizeof err);
        CHECK(p != NULL, "exit: started: %s", err);
        if (p) {
            CHECK(wait_end(p, 5000, &st) && st == 7, "exit: status 7, got %d", st);
            CHECK(strstr(log_text("exit.log"), "out line\n") != NULL, "exit: stdout in the log: '%s'", log_text("exit.log"));
            CHECK(strstr(log_text("exit.log"), "err line\n") != NULL, "exit: stderr in the log: '%s'", log_text("exit.log"));
            CHECK(wait_wake(w, 1000), "exit: woke");
            dsg_proc_free(p);
        }
    }

    /* argv arrives whole: spaces, quotes, backslashes before a quote and at
     * the end, an empty argument, a tab, UTF-8 (Windows: the quoting) */
    {
        const char* a[] = {"args", "plain", "two words", "", "quo\"te", "trail\\", "trail2\\\\", "back\\\"slash",
                           "a\\b c", "\\\\server\\share dir\\", "tab\there", "\"", "Gr\xc3\xbc\xc3\x9f" "e \xe2\x9c\x93", NULL};
        char want[4096] = "", line[512];
        int i;
        for (i = 1; a[i]; i++) {
            hex_line(line, sizeof line, a[i]);
            snprintf(want + strlen(want), sizeof want - strlen(want), "%s", line);
        }
        waker_init(w);
        p = start_child("args.log", a, NULL, NULL, w, err, sizeof err);
        CHECK(p != NULL, "args: started: %s", err);
        if (p) {
            CHECK(wait_end(p, 5000, &st) && st == 0, "args: status 0, got %d", st);
            CHECK(strcmp(log_text("args.log"), want) == 0, "args: arrive whole:\n%s\nwanted:\n%s", log_text("args.log"), want);
            dsg_proc_free(p);
        }
    }

    /* only 0, 1 and 2: descriptors without close-on-exec stay the designer's */
    {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa;
        HANDLE ev;
        char hv[32];
        const char* a[] = {"fds", hv, NULL};
        memset(&sa, 0, sizeof sa);
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        ev = CreateEventW(&sa, TRUE, FALSE, NULL);
        snprintf(hv, sizeof hv, "%llu", (unsigned long long)(uintptr_t)ev);
        p = start_child("fds.log", a, NULL, NULL, NULL, err, sizeof err);
        CHECK(p != NULL, "fds: started: %s", err);
        if (p) {
            CHECK(wait_end(p, 5000, &st) && st == 0, "fds: status 0, got %d", st);
            CHECK(strcmp(log_text("fds.log"), "handle invalid\n") == 0, "fds: an inheritable handle not inherited: '%s'", log_text("fds.log"));
            dsg_proc_free(p);
        }
        if (ev) CloseHandle(ev);
#else
        const char* a[] = {"fds", NULL};
        int leak = open("/dev/null", O_RDONLY), pp[2] = {-1, -1}, in0 = dup(0);
        if (pipe(pp) != 0) pp[0] = pp[1] = -1;
        /* this process's standard input is a pipe: the child's must not be */
        if (pp[0] >= 0) dup2(pp[0], 0);
        p = start_child("fds.log", a, NULL, NULL, NULL, err, sizeof err);
        if (in0 >= 0) {
            dup2(in0, 0);
            close(in0);
        }
        CHECK(p != NULL, "fds: started: %s", err);
        if (p) {
            CHECK(wait_end(p, 5000, &st) && st == 0, "fds: status 0, got %d", st);
            CHECK(strcmp(log_text("fds.log"), "fds 0 1 2\nstdin null\npgrp own\n") == 0,
                  "fds: only 0, 1, 2 (leak %d, pipe %d %d), stdin /dev/null, a process group of its own: '%s'", leak, pp[0], pp[1],
                  log_text("fds.log"));
            dsg_proc_free(p);
        }
        if (leak >= 0) close(leak);
        if (pp[0] >= 0) close(pp[0]);
        if (pp[1] >= 0) close(pp[1]);
#endif
    }

    /* the child's policy is SCHED_OTHER, and so is the watch thread's, when
     * the thread that starts them is SCHED_FIFO. Windows: the child's class
     * is NORMAL when the designer's is below. */
    {
        const char* a[] = {"policy", NULL};
#if defined(__linux__)
        struct rlimit rl;
        struct sched_param sp, old_sp;
        int old_pol = SCHED_OTHER;
        memset(&sp, 0, sizeof sp);
        memset(&old_sp, 0, sizeof old_sp);
        if (getrlimit(RLIMIT_RTPRIO, &rl) != 0 || rl.rlim_cur < 1) {
            printf("SKIP: policy under SCHED_FIFO (no rtprio grant; ulimit -r is 0)\n");
        } else {
            pthread_getschedparam(pthread_self(), &old_pol, &old_sp);
            sp.sched_priority = 1;
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
                printf("SKIP: policy under SCHED_FIFO (refused)\n");
            } else {
                waker_init(w);
                p = start_child("policy.log", a, NULL, NULL, w, err, sizeof err);
                /* back at once: nothing of this test runs at FIFO but the start */
                pthread_setschedparam(pthread_self(), old_pol, &old_sp);
                CHECK(p != NULL, "policy: started: %s", err);
                if (p) {
                    char want[64];
                    snprintf(want, sizeof want, "policy %d\n", SCHED_OTHER);
                    CHECK(wait_end(p, 5000, &st) && st == 0, "policy: status 0, got %d", st);
                    CHECK(strcmp(log_text("policy.log"), want) == 0, "policy: the child is SCHED_OTHER (%d), FIFO is %d: '%s'", SCHED_OTHER,
                          SCHED_FIFO, log_text("policy.log"));
                    CHECK(wait_wake(w, 1000), "policy: the watch thread woke");
                    dsg_proc_free(p);
                    CHECK(w->policy == SCHED_OTHER, "policy: the watch thread is SCHED_OTHER (%d), got %d", SCHED_OTHER, w->policy);
                }
            }
        }
#elif defined(_WIN32)
        if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)) {
            printf("SKIP: the priority class\n");
        } else {
            p = start_child("policy.log", a, NULL, NULL, NULL, err, sizeof err);
            SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
            CHECK(p != NULL, "policy: started: %s", err);
            if (p) {
                char want[64];
                snprintf(want, sizeof want, "class %d\n", (int)NORMAL_PRIORITY_CLASS);
                CHECK(wait_end(p, 5000, &st) && st == 0, "policy: status 0, got %d", st);
                CHECK(strcmp(log_text("policy.log"), want) == 0, "policy: the child's class is NORMAL: '%s'", log_text("policy.log"));
                dsg_proc_free(p);
            }
        }
#else
        (void)a;
        printf("SKIP: policy (no SCHED_FIFO grant on this system)\n");
#endif
    }

#if !defined(_WIN32)
    /* each signal default and none blocked, though the designer ignores
     * SIGPIPE and blocks SIGTERM, SIGINT and SIGPIPE */
    {
        const char* a[] = {"signals", NULL};
        struct sigaction ign, old;
        sigset_t set, old_set;
        memset(&ign, 0, sizeof ign);
        memset(&old, 0, sizeof old);
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        sigaction(SIGPIPE, &ign, &old);
        sigemptyset(&set);
        sigaddset(&set, SIGTERM);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, &old_set);
        p = start_child("signals.log", a, NULL, NULL, NULL, err, sizeof err);
        pthread_sigmask(SIG_SETMASK, &old_set, NULL);
        sigaction(SIGPIPE, &old, NULL);
        CHECK(p != NULL, "signals: started: %s", err);
        if (p) {
            CHECK(wait_end(p, 5000, &st) && st == 0, "signals: status 0, got %d", st);
            CHECK(strcmp(log_text("signals.log"), "sigpipe default\nblocked 0\n") == 0, "signals: default and unblocked: '%s'",
                  log_text("signals.log"));
            dsg_proc_free(p);
        }
    }
#endif

    /* the working folder; a relative program with one is refused */
    {
        const char* a[] = {"cwd", NULL};
        char sub[DSG_PATH_MAX], want[DSG_PATH_MAX + 8], real[DSG_PATH_MAX];
        snprintf(sub, sizeof sub, "%s", ppath("sub"));
        dsg_make_dir(sub, err, sizeof err);
#if defined(_WIN32)
        snprintf(real, sizeof real, "%s", sub);
#else
        if (!realpath(sub, real)) snprintf(real, sizeof real, "%s", sub);
#endif
        snprintf(want, sizeof want, "cwd %s\n", real);
        p = start_child("cwd.log", a, sub, NULL, NULL, err, sizeof err);
        CHECK(p != NULL, "cwd: started: %s", err);
        if (p) {
            char got[DSG_PATH_MAX + 8];
            size_t i;
            CHECK(wait_end(p, 5000, &st) && st == 0, "cwd: status 0, got %d", st);
            snprintf(got, sizeof got, "%s", log_text("cwd.log"));
#if defined(_WIN32)
            for (i = 0; got[i]; i++)
                if (got[i] == '\\') got[i] = '/';
            for (i = 0; want[i]; i++)
                if (want[i] == '\\') want[i] = '/';
            CHECK(_stricmp(got, want) == 0, "cwd: the child's folder: '%s', wanted '%s'", got, want);
#else
            (void)i;
            CHECK(strcmp(got, want) == 0, "cwd: the child's folder: '%s', wanted '%s'", got, want);
#endif
            dsg_proc_free(p);
        }
        {
            const char* argv[] = {"test_child", "cwd", NULL};
            dsg_proc_desc d;
            dsg_proc* q = NULL;
            memset(&d, 0, sizeof d);
            d.argv = argv;
            d.cwd = sub;
            d.out_path = ppath("rel.log");
            err[0] = 0;
            CHECK(dsg_proc_start(&q, &d, err, sizeof err) == -1 && !q && strstr(err, "absolute") && !dsg_is_file(ppath("rel.log")),
                  "cwd: a relative program with a working folder is refused: '%s'", err);
            dsg_proc_free(q);
        }
    }

    /* Stop: a child that takes SIGTERM (WM_CLOSE) ends with its own code */
    {
        const char* a[] = {"term", "5", NULL};
        p = start_child("term.log", a, NULL, NULL, NULL, err, sizeof err);
        CHECK(p != NULL, "stop: started: %s", err);
        if (p) {
            CHECK(wait_log("term.log", "ready", 5000), "stop: the child is ready");
            CHECK(dsg_proc_stop(p) == 1, "stop: the request went out");
            CHECK(wait_end(p, 5000, &st) && st == 5, "stop: status 5, got %d", st);
            CHECK(strstr(log_text("term.log"), "got term\n") != NULL, "stop: the child took it: '%s'", log_text("term.log"));
            CHECK(dsg_proc_stop(p) == 0, "stop: an ended child gets nothing");
            dsg_proc_free(p);
        }
    }

    /* Kill: a child that ignores Stop */
    {
        const char* a[] = {"ignore", NULL};
        p = start_child("ignore.log", a, NULL, NULL, NULL, err, sizeof err);
        CHECK(p != NULL, "kill: started: %s", err);
        if (p) {
            CHECK(wait_log("ignore.log", "ready", 5000), "kill: the child is ready");
            CHECK(dsg_proc_stop(p) == 1, "kill: Stop's request went out");
            t_sleep(300);
            CHECK(dsg_proc_poll(p, &st) == 0, "kill: Stop did not end a child that ignores it");
            dsg_proc_kill(p);
            CHECK(wait_end(p, 5000, &st) && st == -9, "kill: status -9, got %d", st);
            dsg_proc_free(p);
        }
    }

    /* the watch thread wakes within 300 ms of the log's growth, of a watched
     * file's growth and of the exit (Linux: the exit within 50 ms, a pidfd) */
    {
        /* 330 ms apart: the exit falls far from a 250 ms tick, so the
         * pidfd's check fails without it */
        const char* a[] = {"lines", "4", "330", NULL};
        const char* watch[] = {ppath("extra.txt"), NULL};
        long long t_write = 0;
        waker_init(w);
        p = start_child("lines.log", a, NULL, watch, w, err, sizeof err);
        CHECK(p != NULL, "watch: started: %s", err);
        if (p) {
            char put_err[256];
            const char* text;
            const char* at;
            int lines = 0, late = 0;
            long long worst = 0, exit_lat = -1;
            t_sleep(450);
            t_write = t_now();
            CHECK(dsg_write_file(watch[0], "x", 1, put_err, sizeof put_err) == 0, "watch: write: %s", put_err);
            CHECK(wait_end(p, 5000, &st) && st == 0, "watch: status 0, got %d", st);
            /* the exit's wake may come after this poll saw the end */
            at = strstr(log_text("lines.log"), "exit t=");
            if (at) {
                long long t_exit = atoll(at + 7), end = t_now() + 1000000000LL;
                while (waker_after(w, t_exit) < 0 && t_now() < end) t_sleep(5);
            }
            dsg_proc_free(p);
            text = log_text("lines.log");
            for (at = strstr(text, "t="); at; at = strstr(at + 2, "t=")) {
                long long t = atoll(at + 2), lat = waker_after(w, t);
                if (strncmp(at - 5, "exit ", 5) == 0) {
                    exit_lat = lat;
                    continue;
                }
                lines++;
                if (lat < 0 || lat > 300000000LL) late++;
                if (lat > worst) worst = lat;
            }
            CHECK(lines == 4 && late == 0, "watch: each of 4 lines woke within 300 ms: %d lines, %d late, worst %.1f ms", lines, late,
                  (double)worst / 1e6);
            {
                long long lat = waker_after(w, t_write);
                CHECK(lat >= 0 && lat <= 300000000LL, "watch: a watched file's growth woke within 300 ms: %.1f ms", (double)lat / 1e6);
            }
#if defined(__linux__)
            CHECK(exit_lat >= 0 && exit_lat <= 50000000LL, "watch: the exit woke within 50 ms (pidfd): %.1f ms", (double)exit_lat / 1e6);
#else
            CHECK(exit_lat >= 0 && exit_lat <= 300000000LL, "watch: the exit woke within 300 ms: %.1f ms", (double)exit_lat / 1e6);
#endif
        }
    }

#if defined(_WIN32)
    /* Stop of a child with no window (as `ysp_player --sim`): 0, nothing sent */
    {
        const char* a[] = {"sleep", "2000", NULL};
        p = start_child("nowin.log", a, NULL, NULL, NULL, err, sizeof err);
        CHECK(p != NULL, "nowin: started: %s", err);
        if (p) {
            CHECK(dsg_proc_stop(p) == 0, "nowin: Stop of a child with no window gives 0");
            dsg_proc_kill(p);
            CHECK(wait_end(p, 5000, &st) && st == -9, "nowin: Kill ends it, got %d", st);
            dsg_proc_free(p);
        }
    }
#endif

    /* the exit alone wakes: a child that writes nothing */
    {
        const char* a[] = {"sleep", "100", NULL};
        waker_init(w);
        p = start_child("quiet.log", a, NULL, NULL, w, err, sizeof err);
        CHECK(p != NULL, "quiet: started: %s", err);
        if (p) {
            CHECK(wait_end(p, 5000, &st) && st == 0, "quiet: status 0, got %d", st);
            CHECK(wait_wake(w, 1000), "quiet: the exit woke");
            dsg_proc_free(p);
            CHECK(strcmp(log_text("quiet.log"), "") == 0, "quiet: the log is empty: '%s'", log_text("quiet.log"));
        }
    }

    /* the watch thread blocks: a second of a running child costs this
     * process almost no CPU */
    {
        const char* a[] = {"sleep", "1500", NULL};
        p = start_child("sleep.log", a, NULL, NULL, w, err, sizeof err);
        CHECK(p != NULL, "block: started: %s", err);
        if (p) {
            long long c0 = t_cpu_ns(), c1;
            t_sleep(1000);
            c1 = t_cpu_ns();
            CHECK(c1 - c0 < 50000000LL, "block: %.1f ms of CPU in 1 s", (double)(c1 - c0) / 1e6);
            CHECK(wait_end(p, 5000, &st) && st == 0, "block: status 0, got %d", st);
            dsg_proc_free(p);
        }
    }

    /* a log that exists is refused and kept; a program that does not start
     * leaves no log */
    {
        const char* a[] = {"exit", "0", NULL};
        char put_err[256];
        dsg_write_file(ppath("exists.log"), "keep", 4, put_err, sizeof put_err);
        err[0] = 0;
        p = start_child("exists.log", a, NULL, NULL, NULL, err, sizeof err);
        CHECK(p == NULL && strstr(err, "exists") != NULL, "exists: refused: '%s'", err);
        if (p) {
            wait_end(p, 5000, &st);
            dsg_proc_free(p);
        }
        CHECK(strcmp(log_text("exists.log"), "keep") == 0, "exists: the old log is whole: '%s'", log_text("exists.log"));
    }
    {
        char keep[DSG_PATH_MAX];
        const char* a[] = {"exit", "0", NULL};
        snprintf(keep, sizeof keep, "%s", child_path);
        snprintf(child_path, sizeof child_path, "%s", ppath("no_such_program"));
        err[0] = 0;
        p = start_child("missing.log", a, NULL, NULL, NULL, err, sizeof err);
        snprintf(child_path, sizeof child_path, "%s", keep);
        CHECK(p == NULL && err[0], "missing: refused: '%s'", err);
        if (p) {
            wait_end(p, 5000, &st);
            dsg_proc_free(p);
        }
        CHECK(!dsg_is_file(ppath("missing.log")), "missing: no log left");
        err[0] = 0;
        CHECK(dsg_proc_start(&p, NULL, err, sizeof err) == -1 && !p && err[0], "no desc: refused: '%s'", err);
    }

#if !defined(_WIN32)
    /* no zombie: each ended child is reaped by the time it is freed */
    CHECK(!have_zombie(), "zombie: none after the frees");
#endif

    /* free of a running child leaves it running, calls wake no more, and
     * (POSIX) reaps it when it ends */
    {
        const char* a[] = {"term", "0", NULL};
        const char* watch[] = {ppath("after.txt"), NULL};
        waker_init(w);
        p = start_child("left.log", a, NULL, watch, w, err, sizeof err);
        CHECK(p != NULL, "left: started: %s", err);
        if (p) {
            const char* r;
            long long pid = 0;
            int n_free;
            char put_err[256];
            CHECK(wait_log("left.log", "ready", 5000), "left: the child is ready");
            r = strstr(log_text("left.log"), "ready ");
            if (r) pid = atoll(r + 6);
            dsg_proc_free(p);
            n_free = waker_count(w);
            dsg_write_file(watch[0], "grown", 5, put_err, sizeof put_err);
            t_sleep(400);
#if defined(_WIN32)
            {
                HANDLE h = pid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid) : NULL;
                CHECK(h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT, "left: still running after the free");
                if (h) {
                    TerminateProcess(h, 0);
                    WaitForSingleObject(h, 5000);
                    CloseHandle(h);
                }
            }
#else
            {
                long long end;
                CHECK(pid > 0 && kill((pid_t)pid, 0) == 0, "left: still running after the free");
                if (pid > 0) kill((pid_t)pid, SIGTERM);
                end = t_now() + 3000000000LL;
                while (pid > 0 && kill((pid_t)pid, 0) == 0 && t_now() < end) t_sleep(5);
                /* kill(pid, 0) answers for a zombie too: ESRCH only after the reap */
                CHECK(pid > 0 && kill((pid_t)pid, 0) == -1 && errno == ESRCH, "left: reaped after it ended (no zombie)");
                CHECK(!have_zombie(), "left: no zombie");
            }
#endif
            CHECK(waker_count(w) == n_free, "left: no wake after the free (%d, then %d)", n_free, waker_count(w));
        }
    }

    dsg_remove_tree(work, "proc", err, sizeof err);
    free(w);
}

/* --measure: the costs and the wake's latency, printed. One at a time on a
 * quiet machine (CLAUDE.md). */
static int cmp_ll(const void* a, const void* b) {
    long long x = *(const long long*)a, y = *(const long long*)b;
    return x < y ? -1 : x > y;
}
static void stats(const char* what, long long* v, int n, double unit, const char* uname) {
    if (n <= 0) {
        printf("%s: no samples\n", what);
        return;
    }
    qsort(v, (size_t)n, sizeof *v, cmp_ll);
    printf("%s (n %d): median %.3f, p95 %.3f, max %.3f %s\n", what, n, (double)v[n / 2] / unit, (double)v[(n * 95) / 100] / unit,
           (double)v[n - 1] / unit, uname);
}

static int proc_measure(const char* work, const char* child) {
    enum { RUNS = 200 };
    static long long call[RUNS], exitw[RUNS], grow[RUNS];
    char err[512] = "";
    waker* w = (waker*)malloc(sizeof *w);
    int i, nc = 0, ne = 0, ng = 0, st = 0;
    unsigned seed = 12345;
    if (!w || dsg_join(pdir, sizeof pdir, work, "proc") || snprintf(child_path, sizeof child_path, "%s", child) >= (int)sizeof child_path)
        return 1;
    dsg_remove_tree(work, "proc", err, sizeof err);
    if (dsg_make_dir(pdir, err, sizeof err)) return 1;
    /* the start's call, and the exit's wake, of a child that exits at once */
    for (i = 0; i < RUNS; i++) {
        const char* a[] = {"lines", "0", "0", NULL};
        char name[64];
        dsg_proc* p;
        long long t0, t1;
        snprintf(name, sizeof name, "m%d.log", i);
        waker_init(w);
        t0 = t_now();
        p = start_child(name, a, NULL, NULL, w, err, sizeof err);
        t1 = t_now();
        if (!p) {
            printf("start failed: %s\n", err);
            return 1;
        }
        call[nc++] = t1 - t0;
        wait_end(p, 5000, &st);
        while (waker_count(w) == 0) t_sleep(1);
        dsg_proc_free(p);
        {
            const char* at = strstr(log_text(name), "exit t=");
            if (at) exitw[ne++] = waker_after(w, atoll(at + 7));
        }
    }
    stats("dsg_proc_start() call", call, nc, 1e3, "us");
    stats("exit to wake", exitw, ne, 1e6, "ms");
    /* a watched file's growth to the wake, at random phases of the tick */
    {
        const char* a[] = {"sleep", "120000", NULL};
        const char* watch[] = {ppath("grow.txt"), NULL};
        dsg_proc* p;
        char buf[16];
        long long c0, c1, t0;
        waker_init(w);
        p = start_child("grow.log", a, NULL, watch, w, err, sizeof err);
        if (!p) return 1;
        for (i = 0; i < RUNS / 2; i++) {
            long long t;
            seed = seed * 1103515245u + 12345u;
            t_sleep(300 + (long)((seed >> 16) % 300));
            snprintf(buf, sizeof buf, "%d\n", i);
            t = t_now();
            /* an append, as the player's writes: the size grows */
            {
                FILE* f = fopen(watch[0], "ab");
                if (f) {
                    fwrite(buf, 1, strlen(buf), f);
                    fclose(f);
                }
            }
            t_sleep(400);
            grow[ng++] = waker_after(w, t);
        }
        stats("growth to wake", grow, ng, 1e6, "ms");
        c0 = t_cpu_ns();
        t0 = t_now();
        t_sleep(10000);
        c1 = t_cpu_ns();
        printf("idle with a running child: %.1f us of CPU a second (process, %.1f s)\n", (double)(c1 - c0) / 1e3 / ((double)(t_now() - t0) / 1e9),
               (double)(t_now() - t0) / 1e9);
        dsg_proc_kill(p);
        wait_end(p, 5000, &st);
        dsg_proc_free(p);
    }
    dsg_remove_tree(work, "proc", err, sizeof err);
    free(w);
    return 0;
}

int main(int argc, char** argv) {
    char err[512] = "";
    char abs_out[DSG_PATH_MAX] = "";
    if (argc < 2) {
        printf("usage: test_designer_platform WORK [CHILD [--measure]]\n");
        return 2;
    }
    /* the sandbox must be the build tree's own folder: refuse a relative
     * WORK, which would put it wherever the test runs */
#if defined(_WIN32)
    if (!(argv[1][0] && argv[1][1] == ':') && argv[1][0] != '/' && argv[1][0] != '\\') {
#else
    if (argv[1][0] != '/') {
#endif
        printf("FAIL: WORK must be absolute: %s\n", argv[1]);
        return 1;
    }
    if (dsg_make_dir(argv[1], err, sizeof err) || dsg_join(box, sizeof box, argv[1], "remove_tree") ||
        dsg_join(root, sizeof root, box, "root")) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    if (argc > 3 && strcmp(argv[3], "--measure") == 0) return proc_measure(argv[1], argv[2]);
    setup();

    /* refusals: nothing goes */
    REFUSED(NULL, "no path");
    REFUSED("", "an empty path");
    REFUSED(".", "the root itself");
    REFUSED("a/.", "a '.' name");
    REFUSED("..", "the root's parent");
    REFUSED("../sibling", "the root's sibling");
    REFUSED("a/../..", "up through a name");
    REFUSED("a//sub", "an empty name");
    REFUSED("a/", "a trailing separator");
    snprintf(abs_out, sizeof abs_out, "%s", at("outside"));
    REFUSED(abs_out, "an absolute path");
    REFUSED("C:/outside", "a drive");
    REFUSED("a\\sub", "a backslash");
    {
        char e2[512] = "";
        CHECK(dsg_remove_tree("", "a", e2, sizeof e2) == -1 && exists("root/a/f1"), "an empty root is refused");
    }
#if !defined(_WIN32)
    REFUSED("link_out", "a link out of the root, as the name");
    REFUSED("link_out/keep", "a link out of the root, on the way");
    CHECK(is_link("root/link_out"), "the refused link is still there");
    REFUSED("link_in", "a link inside the root");
    CHECK(is_link("root/link_in") && exists("root/b/f4"), "the link inside and its folder are still there");
#endif

    /* not there: nothing to do */
    CHECK(dsg_remove_tree(root, "missing", err, sizeof err) == 0, "a missing name is not an error: %s", err);
    CHECK(dsg_remove_tree(root, "missing/deeper", err, sizeof err) == 0, "a missing folder on the way is not an error: %s", err);
    CHECK(dsg_remove_tree(root, "file5/x", err, sizeof err) == 0 && exists("root/file5"), "a file on the way: nothing removed");

    /* a file */
    CHECK(dsg_remove_tree(root, "file5", err, sizeof err) == 0 && !exists("root/file5"), "a file is removed: %s", err);
    put("root/file5");

    /* a folder below another */
    CHECK(dsg_remove_tree(root, "a/sub/deep", err, sizeof err) == 0 && !exists("root/a/sub/deep"), "a/sub/deep removed: %s", err);
    CHECK(exists("root/a/sub/f2") && exists("root/a/f1") && others_whole(), "its parent and the rest stay");

    /* the tree with the links in it: the links go, their targets stay */
    CHECK(dsg_remove_tree(root, "a", err, sizeof err) == 0, "a removed: %s", err);
    CHECK(!exists("root/a"), "a is gone, with its files and links");
    CHECK(exists("outside/keep") && dsg_is_dir(at("outside")), "the link inside a went, not what it pointed at");
    CHECK(others_whole(), "root's other child, root's sibling and root itself stay");
#if !defined(_WIN32)
    CHECK(is_link("root/link_out") && is_link("root/link_in"), "root's other links stay");
#endif
    CHECK(dsg_remove_tree(root, "a", err, sizeof err) == 0, "removing it again is not an error: %s", err);

    /* the preview's case: run-N folders under DSG_DIR/preview, one kept */
    mkd("root/run-1");
    mkd("root/run-2");
    put("root/run-1/trials.csv");
    put("root/run-2/trials.csv");
    CHECK(dsg_remove_tree(root, "run-1", err, sizeof err) == 0 && !exists("root/run-1") && exists("root/run-2/trials.csv"),
          "run-1 removed, run-2 kept: %s", err);

    clean_box();
    CHECK(!dsg_is_dir(box), "the sandbox is gone");

    if (argc > 2) proc_tests(argv[1], argv[2]);
    else printf("SKIP: the process part (no CHILD)\n");
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
