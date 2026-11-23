/* test_child.c - the child that test_designer_platform starts through
 * dsg_proc_start() (docs/designer.md, "Part X as built"). It never spins:
 * each wait is a sleep or a blocking call, so a mutant that leaves it at
 * SCHED_FIFO cannot take a CPU for good. Each wait ends by itself after
 * 30 s.
 *
 *   test_child exit CODE      "out line" on stdout, "err line" on stderr; exit CODE
 *   test_child args A...      "arg=HEX;" per argument, its UTF-8 bytes in hex
 *   test_child fds [HANDLE]   POSIX: "fds 0 1 2" (the open descriptors),
 *                             "stdin null" (or "stdin other"), "pgrp own"
 *                             (its own process group) or "pgrp parent".
 *                             Windows: "handle valid" when HANDLE is an
 *                             inherited handle, else "handle invalid"
 *   test_child policy         POSIX: "policy N" (sched_getscheduler();
 *                             macOS: pthread_getschedparam()).
 *                             Windows: "class N" (GetPriorityClass())
 *   test_child signals        POSIX: "sigpipe default|ignored|caught" and
 *                             "blocked N" (how many of SIGTERM, SIGINT and
 *                             SIGPIPE the mask holds)
 *   test_child cwd            "cwd PATH"
 *   test_child term CODE      "ready PID"; SIGTERM (Windows: WM_CLOSE to its
 *                             hidden window) gives "got term" and exit CODE
 *   test_child ignore         "ready PID"; SIGTERM ignored (WM_CLOSE too)
 *   test_child lines N MS     N times "line K t=NS", MS apart, then
 *                             "exit t=NS" and exit 0; NS is the monotonic
 *                             clock (Windows: QueryPerformanceCounter) just
 *                             before the write
 *   test_child sleep MS       sleeps, exit 0
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* nanosleep(), dirfd() under -std=c11 */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static void say(const char* fmt, long long v) {
    printf(fmt, v);
    fflush(stdout);
}

static long long now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
#endif
}

static void sleep_ms(long ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) != 0) {}
#endif
}

static void hex_arg(const char* a) {
    const unsigned char* s = (const unsigned char*)a;
    printf("arg=");
    for (; *s; s++) printf("%02x", *s);
    printf(";\n");
}

#if defined(_WIN32)
static int close_code;
static int take_close;

static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        if (!take_close) return 0;
        say("got term\n", 0);
        exit(close_code);
    }
    if (m == WM_TIMER) exit(0);
    return DefWindowProcW(h, m, w, l);
}

/* A hidden top-level window: EnumWindows() lists it, so Stop's WM_CLOSE
 * reaches it, and nothing shows. */
static int wait_window(int take, int code) {
    WNDCLASSW wc;
    HWND h;
    MSG msg;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = child_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ysp_test_child";
    take_close = take;
    close_code = code;
    if (!RegisterClassW(&wc)) return 90;
    h = CreateWindowExW(0, wc.lpszClassName, L"test_child", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (!h) return 91;
    SetTimer(h, 1, 30000, NULL);
    say("ready %lld\n", (long long)GetCurrentProcessId());
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
#else
static volatile sig_atomic_t got_term;
static void on_term(int s) {
    (void)s;
    got_term = 1;
}
#endif

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
#if defined(_WIN32)
    /* the arguments as the program got them, in UTF-16, to UTF-8 */
    int wn = 0, i;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wn);
    /* the test compares the log's bytes: no "\r\n" from text mode */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (strcmp(mode, "exit") == 0 && argc > 2) {
        printf("out line\n");
        fflush(stdout);
        fprintf(stderr, "err line\n");
        fflush(stderr);
        return atoi(argv[2]);
    }
    if (strcmp(mode, "args") == 0) {
#if defined(_WIN32)
        if (!wargv) return 92;
        for (i = 2; i < wn; i++) {
            char buf[4096];
            int k = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, (int)sizeof buf, NULL, NULL);
            if (k <= 0) return 93;
            hex_arg(buf);
        }
#else
        int i;
        for (i = 2; i < argc; i++) hex_arg(argv[i]);
#endif
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "fds") == 0) {
#if defined(_WIN32)
        HANDLE h = argc > 2 ? (HANDLE)(uintptr_t)_strtoui64(argv[2], NULL, 10) : NULL;
        DWORD f = 0;
        /* the value may name another handle of this process: the parent's
         * would also be inheritable, and is none of the standard three */
        int leaked = h && GetHandleInformation(h, &f) && (f & HANDLE_FLAG_INHERIT) && h != GetStdHandle(STD_INPUT_HANDLE) &&
                     h != GetStdHandle(STD_OUTPUT_HANDLE) && h != GetStdHandle(STD_ERROR_HANDLE);
        say(leaked ? "handle valid\n" : "handle invalid\n", 0);
#else
        printf("fds");
#if defined(__linux__)
        {
            DIR* d = opendir("/proc/self/fd");
            struct dirent* e;
            int self = d ? dirfd(d) : -1;
            int fd[256], n = 0, i, j;
            while (d && (e = readdir(d)) != NULL) {
                int v;
                if (e->d_name[0] == '.') continue;
                v = atoi(e->d_name);
                if (v != self && n < 256) fd[n++] = v;
            }
            if (d) closedir(d);
            for (i = 1; i < n; i++)
                for (j = i; j > 0 && fd[j - 1] > fd[j]; j--) {
                    int t = fd[j];
                    fd[j] = fd[j - 1];
                    fd[j - 1] = t;
                }
            for (i = 0; i < n; i++) printf(" %d", fd[i]);
        }
#else
        {
            int fd;
            for (fd = 0; fd < 4096; fd++)
                if (fcntl(fd, F_GETFD) != -1) printf(" %d", fd);
        }
#endif
        printf("\n");
        {
            struct stat a, b;
            int null = fstat(0, &a) == 0 && stat("/dev/null", &b) == 0 && S_ISCHR(a.st_mode) && a.st_rdev == b.st_rdev;
            printf("stdin %s\n", null ? "null" : "other");
        }
        printf("pgrp %s\n", getpgrp() == getpid() ? "own" : "parent");
        fflush(stdout);
#endif
        return 0;
    }
    if (strcmp(mode, "policy") == 0) {
#if defined(_WIN32)
        say("class %lld\n", (long long)GetPriorityClass(GetCurrentProcess()));
#else
#if defined(__APPLE__)
        /* macOS has no sched_getscheduler() */
        struct sched_param sp;
        int pol = -1;
        if (pthread_getschedparam(pthread_self(), &pol, &sp) != 0) pol = -1;
        say("policy %lld\n", (long long)pol);
#else
        say("policy %lld\n", (long long)sched_getscheduler(0));
#endif
#endif
        return 0;
    }
    if (strcmp(mode, "signals") == 0) {
#if !defined(_WIN32)
        struct sigaction sa;
        sigset_t m;
        memset(&sa, 0, sizeof sa);
        sigaction(SIGPIPE, NULL, &sa);
        printf("sigpipe %s\n", sa.sa_handler == SIG_DFL ? "default" : sa.sa_handler == SIG_IGN ? "ignored" : "caught");
        sigemptyset(&m);
        sigprocmask(SIG_BLOCK, NULL, &m);
        printf("blocked %d\n", sigismember(&m, SIGTERM) + sigismember(&m, SIGINT) + sigismember(&m, SIGPIPE));
        fflush(stdout);
#endif
        return 0;
    }
    if (strcmp(mode, "cwd") == 0) {
#if defined(_WIN32)
        wchar_t w[1024];
        char buf[4096];
        DWORD k = GetCurrentDirectoryW(1024, w);
        if (!k || k >= 1024 || WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL) <= 0) return 94;
#else
        char buf[4096];
        if (!getcwd(buf, sizeof buf)) return 94;
#endif
        printf("cwd %s\n", buf);
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "term") == 0 && argc > 2) {
#if defined(_WIN32)
        return wait_window(1, atoi(argv[2]));
#else
        struct sigaction sa;
        int k;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_term;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);
        say("ready %lld\n", (long long)getpid());
        for (k = 0; k < 3000 && !got_term; k++) sleep_ms(10);
        if (!got_term) return 0;
        say("got term\n", 0);
        return atoi(argv[2]);
#endif
    }
    if (strcmp(mode, "ignore") == 0) {
#if defined(_WIN32)
        return wait_window(0, 0);
#else
        signal(SIGTERM, SIG_IGN);
        say("ready %lld\n", (long long)getpid());
        sleep_ms(30000);
        return 0;
#endif
    }
    if (strcmp(mode, "lines") == 0 && argc > 3) {
        int n = atoi(argv[2]), k;
        long ms = atol(argv[3]);
        for (k = 0; k < n; k++) {
            sleep_ms(ms);
            printf("line %d ", k);
            say("t=%lld\n", now_ns());
        }
        say("exit t=%lld\n", now_ns());
        return 0;
    }
    if (strcmp(mode, "sleep") == 0 && argc > 2) {
        sleep_ms(atol(argv[2]));
        return 0;
    }
    fprintf(stderr, "test_child: unknown mode '%s'\n", mode);
    return 2;
}
