/* screen_abort.c - the abort combination and the panic watchdog, checked on
 * this machine with keys sent by SendInput.
 *
 * The program starts itself as a child with a small window (or, with
 * --mode-test, fullscreen in another display mode) and sends Shift+Esc to
 * it. The cases:
 *   live    the child's frame loop runs and reads its events with
 *           yscr_poll() before begin(). Esc alone must not abort; one
 *           Shift+Esc is one abort; Shift+Esc held with 8 repeats is one
 *           press; 3 presses in 1 s are 3 aborts and no panic, because the
 *           loop reported the first. The child exits 0. desc.window_thread
 *           AUTO: the window thread runs (WINDOW THREAD, v0.10.0).
 *   hang    the frame loop stops (a hang) 1 s after it started; 3 presses
 *           must end the child with YSCR_PANIC_EXIT_CODE, after its panic
 *           callback ran and the gamma entry was restored. The window
 *           thread OFF: the frame thread owns the window, as in v0.9.0.
 *   ghost   the same after a 7 s hang, when Windows has put a ghost window
 *           in front of the hung one.
 *   held    a hung child gets Shift+Esc held with 8 repeats: no panic.
 *   release the window thread ON, the frame loop idle for 4 s (a MATLAB
 *           pause): 3 presses must release the display (the window
 *           minimized) and the child must live; its next begin() reports
 *           0x21 (KEY | PANIC), the one after 0x20, and it exits 0.
 *   kill    the window thread ON and blocked (a window call of 8 s): 3
 *           presses must end the child with YSCR_PANIC_EXIT_CODE.
 *   mode    (--mode-test only) fullscreen in a mode other than the
 *           desktop's, hung, window thread OFF: after the panic the
 *           desktop mode must be back. Prints when it came back, while
 *           the child still lived. Then the same with the window thread ON
 *           and the release: the mode must come back while the child lives
 *           on. The display shows the other mode for about 3 s each.
 * Keys go out only while the child's window (or its ghost) is the
 * foreground window, so they cannot reach another program; when another
 * window takes the foreground, the case fails without sending. The
 * child's screen is dark gray, low contrast.
 *
 * Usage: screen_abort [--mode-test]
 * Exit code: 0 when every case passed, 1 otherwise, 2 for a bad argument.
 */
/* The test seam: arm the watchdog in a window, so no case but --mode-test
 * goes fullscreen. */
#define YSCR__PANIC_WINDOWED 1
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
int main(void) { printf("screen_abort: Windows only\n"); return 0; }
#else
#include <windows.h>

#if defined(_WIN32)
    #define GLCALL __stdcall
#else
    #define GLCALL
#endif

/* --- the child ------------------------------------------------------------- */

static yscr_screen g_scr;
static unsigned char g_ring_mem[YRT_RING_BYTES(256)];
static yrt_ring g_ring;
static int g_fake_gamma = -1;

static void last_words(void* ctx) {
    (void)ctx;
    printf("last words: %.1f ms into the panic, gamma_claimed=%d\n",
           (double)(yrt_now_ns() - yscr__wd.t_panic) / 1e6,
           g_fake_gamma >= 0 ? yscr__gamma[g_fake_gamma].used == 0 : -1);
    fflush(stdout);
}

/* A gamma entry as a fullscreen open() makes one, holding the ramp the
 * display has now: the panic's restore sets the same ramp, which shows
 * nothing, and its claim on the entry shows that it ran. */
static void fake_gamma(void) {
    HMONITOR mon = MonitorFromWindow((HWND)g_scr.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi;
    HDC dc;
    int k;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    yscr__win_load();   /* gdi32 by name, as the header does */
    if (!GetMonitorInfoW(mon, (LPMONITORINFO)&mi) || !yscr__win.create_dc) return;
    dc = yscr__win.create_dc(mi.szDevice, NULL, NULL, NULL);
    for (k = 0; k < YSCR__GAMMA_MAX && yscr__gamma[k].used; k++) { }
    if (dc && k < YSCR__GAMMA_MAX && yscr__win.get_ramp(dc, yscr__gamma[k].saved)) {
        memcpy(yscr__gamma[k].dev, mi.szDevice, sizeof yscr__gamma[k].dev);
        yscr__gamma[k].used = 1;
        g_fake_gamma = k;
    }
    if (dc) yscr__win.delete_dc(dc);
}

static void block_fn(void* ms) { Sleep((DWORD)(uintptr_t)ms); }

/* After the release: begin() reports the panic, every time, until close. */
static int child_after_release(void) {
    yscr_frame f;
    int64_t until = yrt_now_ns() + 1000000000LL;
    int rc;
    printf("released=%d release_ms=%.1f iconic=%d\n", (int)yscr__released,
           (double)(yscr__wt.t_release - yscr__wd.t_panic) / 1e6, IsIconic((HWND)g_scr.hwnd) ? 1 : 0);
    while (yrt_now_ns() < until) {
        rc = yscr_begin(&g_scr, &f);
        if (rc == YSCR_QUIT) printf("abort mask=0x%x presses=%d\n", f.abort, f.abort_presses);
        else { printf("child: begin %s after the release\n", yscr_strerror(rc)); break; }
        Sleep(200);
    }
    printf("lived\n");
    fflush(stdout);
    yscr_close(&g_scr);
    return 0;
}

static int child(int argc, char** argv) {
    yscr_desc d;
    yscr_frame f;
    int i, hang = 0, idle = 0, block = 0, aborts = 0, presses = 0;
    DWORD idle_ms = 4000;
    int64_t hang_after = 1000000000LL, until, t0;
    void (GLCALL *clear_color)(float, float, float, float);
    void (GLCALL *clear)(unsigned int);
    char line[600];
    {
        yrt_ring_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.memory = g_ring_mem;
        rd.bytes = sizeof g_ring_mem;
        yrt_ring_open(&g_ring, &rd);
    }
    memset(&d, 0, sizeof d);
    d.ring = &g_ring;
    d.windowed = true;
    d.window_w = 320;
    d.window_h = 200;
    d.panic = true;
    d.panic_fn = last_words;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "hang")) hang = 1;
        else if (!strcmp(argv[i], "idle")) hang = idle = 1;
        else if (!strcmp(argv[i], "block")) hang = block = 1;
        else if (!strcmp(argv[i], "--idle-ms") && i + 1 < argc) idle_ms = (DWORD)atol(argv[++i]);
        else if (!strcmp(argv[i], "--wt") && i + 1 < argc) {
            i++;
            d.window_thread = !strcmp(argv[i], "on") ? YSCR_WINDOW_THREAD_ON
                            : !strcmp(argv[i], "off") ? YSCR_WINDOW_THREAD_OFF : YSCR_WINDOW_THREAD_AUTO;
        }
        else if (!strcmp(argv[i], "--hang-ms") && i + 1 < argc) hang_after = atoll(argv[++i]) * 1000000LL;
        else if (!strcmp(argv[i], "--mode") && i + 4 < argc) {
            d.windowed = false;
            d.mode.w = atoi(argv[++i]); d.mode.h = atoi(argv[++i]);
            d.mode.refresh_num = atoi(argv[++i]); d.mode.refresh_den = atoi(argv[++i]);
        }
    }
    if (!yscr_open(&g_scr, &d)) { printf("child: %s\n", yscr_error(&g_scr)); return 1; }
    if (d.windowed) fake_gamma();
    yscr_describe(&g_scr, line, sizeof line);
    printf("child: %s\n", line);
    clear_color = (void (GLCALL*)(float, float, float, float))yscr_gl_proc(&g_scr, "glClearColor");
    clear = (void (GLCALL*)(unsigned int))yscr_gl_proc(&g_scr, "glClear");
    {   /* the foreground, as screen_input.c gets it */
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
        SetForegroundWindow((HWND)g_scr.hwnd);
    }
    t0 = yrt_now_ns();
    until = t0 + (hang ? hang_after : 9000000000LL);
    printf("ready %lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);
    while (yrt_now_ns() < until) {
        SDL_Event ev;
        int rc;
        while (yscr_poll(&g_scr, &ev, NULL)) { }   /* the events read before begin() */
        rc = yscr_begin(&g_scr, &f);
        if (rc == YSCR_QUIT) {
            aborts++;
            presses += f.abort_presses;
            printf("abort mask=0x%x presses=%d t=%lld\n", f.abort, f.abort_presses, (long long)f.abort_ns);
            {   /* who saw each: flags 1 the watchdog's hook, 4 SDL, 2 injected */
                yrt_event ev2[32];
                int j, n = yrt_ring_drain(&g_ring, ev2, 32);
                for (j = 0; j < n; j++)
                    if (ev2[j].kind == YSCR_EV_ABORT)
                        printf("  record reason=0x%x flags=0x%x t=%lld\n", ev2[j].u.u32[0], ev2[j].u.u32[1],
                               (long long)ev2[j].t_ns);
            }
            fflush(stdout);
            continue;
        }
        if (rc != YSCR_OK) { printf("child: begin %s\n", yscr_strerror(rc)); break; }
        clear_color(0.12f, 0.12f, 0.12f, 1.0f);
        clear(0x4000u);
        yscr_flip(&g_scr);
    }
    if (block) {   /* the window thread does not answer: the panic ends the process */
        printf("hanging\n");
        fflush(stdout);
        yscr_window_call(&g_scr, block_fn, (void*)(uintptr_t)8000);
        printf("child: the block ended and the process lived\n");
        fflush(stdout);
        for (;;) Sleep(1000);
    }
    if (idle) {    /* a host that waits (MATLAB's pause): the release */
        printf("hanging\n");
        fflush(stdout);
        Sleep(idle_ms);
        return child_after_release();
    }
    if (hang) {
        printf("hanging\n");
        fflush(stdout);
        for (;;) Sleep(1000);
    }
    printf("live done aborts=%d presses=%d\n", aborts, presses);
    fflush(stdout);
    yscr_close(&g_scr);
    return 0;
}

/* --- the parent ------------------------------------------------------------ */

typedef struct kid {
    PROCESS_INFORMATION pi;
    HANDLE out;
    char buf[8192];
    int n;
} kid;

static int start(kid* k, const char* args) {
    char exe[MAX_PATH], cmd[1024];
    STARTUPINFOA si;
    SECURITY_ATTRIBUTES sa;
    HANDLE wr;
    memset(k, 0, sizeof *k);
    memset(&si, 0, sizeof si);
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&k->out, &wr, &sa, 0)) return 0;
    SetHandleInformation(k->out, HANDLE_FLAG_INHERIT, 0);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --child %s", exe, args);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &k->pi)) { CloseHandle(wr); return 0; }
    CloseHandle(wr);
    return 1;
}

/* Reads the child's output until a line starts with want (or it ends). */
static int wait_line(kid* k, const char* want, DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        DWORD avail = 0, got = 0;
        char* p;
        if (!PeekNamedPipe(k->out, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail && k->n < (int)sizeof k->buf - 1) {
            ReadFile(k->out, k->buf + k->n, (DWORD)((int)sizeof k->buf - 1 - k->n) < avail ? (DWORD)((int)sizeof k->buf - 1 - k->n) : avail, &got, NULL);
            k->n += (int)got;
            k->buf[k->n] = '\0';
        }
        for (p = k->buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
            if (!strncmp(p, want, strlen(want))) return 1;
        Sleep(5);
    }
    return 0;
}

static void drain(kid* k) { wait_line(k, "\x01", 200); }

/* The child's window, or a ghost Windows put in front of it. */
static int child_in_front(const kid* k) {
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    char cls[16] = "";
    GetWindowThreadProcessId(h, &pid);
    GetClassNameA(h, cls, sizeof cls);
    return pid == k->pi.dwProcessId || !strcmp(cls, "Ghost");
}

/* The child takes the foreground itself; wait for it. */
static int in_front(const kid* k) {
    int i;
    for (i = 0; i < 300 && !child_in_front(k); i++) Sleep(10);
    if (i == 300) printf("  the child did not get the foreground\n");
    Sleep(300);
    return i < 300;
}

static void key(WORD vk, int up) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

/* Shift+Esc with `repeats` extra key-downs of Esc while it is held, as
 * Windows sends them; Esc alone when shift is 0. Returns the send time. */
static int64_t press(const kid* k, int shift, int repeats) {
    int64_t t;
    int i;
    if (!child_in_front(k)) { printf("  the child is not in front: no key sent\n"); return 0; }
    if (shift) key(VK_LSHIFT, 0);
    t = yrt_now_ns();
    key(VK_ESCAPE, 0);
    for (i = 0; i < repeats; i++) { Sleep(30); key(VK_ESCAPE, 0); }
    key(VK_ESCAPE, 1);
    if (shift) key(VK_LSHIFT, 1);
    return t;
}

static int count_lines(const kid* k, const char* what) {
    int n = 0;
    const char* p = k->buf;
    while ((p = strstr(p, what)) != NULL) { n++; p++; }
    return n;
}

static void finish(kid* k) {
    if (WaitForSingleObject(k->pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(k->pi.hProcess, 7);
    WaitForSingleObject(k->pi.hProcess, 5000);
    CloseHandle(k->pi.hProcess);
    CloseHandle(k->pi.hThread);
    CloseHandle(k->out);
}

static int g_fail;
static void verdict(const char* name, int ok) { printf("%-6s %s\n", name, ok ? "pass" : "FAIL"); if (!ok) g_fail = 1; }

static DWORD g_desk_w;          /* the desktop's width, in the mode case */
static double g_mode_back_ms;

/* 3 presses 300 ms apart; the time from the third to the child's exit. */
static double three(kid* k, DWORD* code) {
    int64_t t = 0;
    int i;
    for (i = 0; i < 3; i++) { if (i) Sleep(300); t = press(k, 1, 0); }
    *code = 0;
    g_mode_back_ms = -1;
    if (!t) return -1;
    /* when the display has the desktop's width again, while the child may
     * still live: the watchdog's restore, not the end of the process */
    while (WaitForSingleObject(k->pi.hProcess, 2) == WAIT_TIMEOUT) {
        DEVMODEW m;
        memset(&m, 0, sizeof m);
        m.dmSize = sizeof m;
        if (g_mode_back_ms < 0 && g_desk_w && EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &m) &&
            m.dmPelsWidth == g_desk_w)
            g_mode_back_ms = (double)(yrt_now_ns() - t) / 1e6;
        if (yrt_now_ns() - t > 5000000000LL) return -1;
    }
    GetExitCodeProcess(k->pi.hProcess, code);
    return (double)(yrt_now_ns() - t) / 1e6;
}

static void case_live(void) {
    kid k;
    int64_t t1;
    DWORD code = 1;
    if (!start(&k, "live") || !wait_line(&k, "ready", 8000) || !in_front(&k)) { verdict("live", 0); finish(&k); return; }
    press(&k, 0, 0);                                /* Esc alone */
    Sleep(400);
    drain(&k);
    verdict("live: Esc alone does not abort", count_lines(&k, "abort ") == 0);
    t1 = press(&k, 1, 0);
    wait_line(&k, "abort ", 1000);
    {
        const char* p = strstr(k.buf, "abort ");
        long long ta = 0;
        if (p) { p = strstr(p, "t="); if (p) ta = atoll(p + 2); }
        printf("  Shift+Esc: stamped %+.3f ms after SendInput\n", (double)(ta - t1) / 1e6);
    }
    verdict("live: one press, one abort", count_lines(&k, "abort mask=0x1 presses=1") == 1);
    Sleep(400);
    press(&k, 1, 8);                                /* held, with repeats */
    Sleep(500);
    drain(&k);
    verdict("live: held is one press", count_lines(&k, "abort mask=0x1 presses=1") == 2);
    {
        int i;
        for (i = 0; i < 3; i++) { Sleep(300); press(&k, 1, 0); }
    }
    if (WaitForSingleObject(k.pi.hProcess, 12000) == WAIT_OBJECT_0) GetExitCodeProcess(k.pi.hProcess, &code);
    drain(&k);
    printf("%s", k.buf);
    verdict("live: 3 presses after a report do not panic", code == 0 && strstr(k.buf, "presses=5") != NULL);
    finish(&k);
}

static void case_hang(const char* name, const char* args, DWORD wait_ms) {
    kid k;
    DWORD code = 0;
    double ms;
    if (!start(&k, args) || !wait_line(&k, "ready", 8000) || !in_front(&k) || !wait_line(&k, "hanging", 15000)) {
        verdict(name, 0); finish(&k); return;
    }
    Sleep(wait_ms);
    ms = three(&k, &code);
    drain(&k);
    printf("  third press to exit: %.1f ms, exit code %lu\n", ms, (unsigned long)code);
    printf("%s", k.buf);
    verdict(name, code == YSCR_PANIC_EXIT_CODE && strstr(k.buf, "gamma_claimed=1") != NULL);
    finish(&k);
}

static void case_held(void) {
    kid k;
    DWORD code = 0;
    if (!start(&k, "hang --wt off") || !wait_line(&k, "ready", 8000) || !in_front(&k) || !wait_line(&k, "hanging", 15000)) {
        verdict("held", 0); finish(&k); return;
    }
    Sleep(300);
    press(&k, 1, 8);
    Sleep(1500);
    verdict("held: no panic", WaitForSingleObject(k.pi.hProcess, 0) == WAIT_TIMEOUT);
    three(&k, &code);   /* and end it */
    verdict("held: then 3 presses panic", code == YSCR_PANIC_EXIT_CODE);
    finish(&k);
}

/* 3 presses 300 ms apart to a child whose window thread answers: it must
 * live, with its window minimized; later it reports the panic and exits 0.
 * Returns the time from the third press to the child's exit, or -1. */
static double three_release(kid* k, DWORD* code, double* lived_ms) {
    int64_t t = 0;
    int i;
    for (i = 0; i < 3; i++) { if (i) Sleep(300); t = press(k, 1, 0); }
    *code = 0;
    *lived_ms = -1;
    g_mode_back_ms = -1;
    if (!t) return -1;
    while (WaitForSingleObject(k->pi.hProcess, 2) == WAIT_TIMEOUT) {
        DEVMODEW m;
        memset(&m, 0, sizeof m);
        m.dmSize = sizeof m;
        if (g_mode_back_ms < 0 && g_desk_w && EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &m) &&
            m.dmPelsWidth == g_desk_w)
            g_mode_back_ms = (double)(yrt_now_ns() - t) / 1e6;
        if (yrt_now_ns() - t > 12000000000LL) return -1;
    }
    *lived_ms = (double)(yrt_now_ns() - t) / 1e6;
    GetExitCodeProcess(k->pi.hProcess, code);
    return *lived_ms;
}

static void case_release(const char* name, const char* args, int mode_case) {
    kid k;
    DWORD code = 7;
    double lived;
    DEVMODEW before, after;
    memset(&before, 0, sizeof before);
    if (mode_case) {
        before.dmSize = sizeof before;
        EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &before);
        g_desk_w = before.dmPelsWidth;
    }
    if (!start(&k, args) || !wait_line(&k, "ready", 8000) || !in_front(&k) || !wait_line(&k, "hanging", 15000)) {
        printf("%s", k.buf); verdict(name, 0); finish(&k); return;
    }
    Sleep(300);
    three_release(&k, &code, &lived);
    g_desk_w = 0;
    drain(&k);
    printf("%s", k.buf);
    printf("  the child lived %.1f ms after press 3, exit code %lu\n", lived, (unsigned long)code);
    if (mode_case) {
        printf("  desktop width back %.1f ms after press 3, while the process lived\n", g_mode_back_ms);
        memset(&after, 0, sizeof after);
        after.dmSize = sizeof after;
        EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &after);
        if (after.dmPelsWidth != before.dmPelsWidth || after.dmPelsHeight != before.dmPelsHeight) {
            ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
            printf("  the mode was put back by the parent\n");
        }
    }
    verdict(name, code == 0 && lived > 1000 && strstr(k.buf, "released=1") != NULL && strstr(k.buf, "iconic=1") != NULL &&
                  strstr(k.buf, "abort mask=0x21 presses=3") != NULL && strstr(k.buf, "abort mask=0x20 presses=0") != NULL &&
                  strstr(k.buf, "lived") != NULL && (!mode_case || g_mode_back_ms >= 0));
    finish(&k);
}

static int mode_now(DEVMODEW* m) {
    memset(m, 0, sizeof *m);
    m->dmSize = sizeof *m;
    return EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, m);
}

static void case_mode(void) {
    yscr_mode modes[64], desk;
    yscr_display_info di;
    DEVMODEW before, after;
    char args[128];
    kid k;
    DWORD code = 0;
    double ms;
    int i, n = yscr_modes(0, modes, 64), pick = -1;
    if (yscr_displays(&di, 1) < 1) { verdict("mode", 0); return; }
    desk = di.desktop;
    for (i = 0; i < n && i < 64; i++)
        if ((modes[i].w != desk.w || modes[i].h != desk.h) && modes[i].refresh_num * (int64_t)desk.refresh_den ==
            desk.refresh_num * (int64_t)modes[i].refresh_den && modes[i].w < desk.w) { pick = i; break; }
    if (pick < 0) { printf("mode: no other mode at the desktop's refresh\n"); return; }
    mode_now(&before);
    g_desk_w = before.dmPelsWidth;
    snprintf(args, sizeof args, "hang --wt off --hang-ms 1500 --mode %d %d %d %d", modes[pick].w, modes[pick].h,
             modes[pick].refresh_num, modes[pick].refresh_den);
    printf("mode: desktop %lux%lu, switching to %dx%d for about 3 s\n", (unsigned long)before.dmPelsWidth,
           (unsigned long)before.dmPelsHeight, modes[pick].w, modes[pick].h);
    if (!start(&k, args) || !wait_line(&k, "ready", 8000) || !in_front(&k) || !wait_line(&k, "hanging", 15000)) {
        printf("%s", k.buf); verdict("mode", 0); finish(&k); return;
    }
    {
        DEVMODEW during;
        mode_now(&during);
        printf("  during the hang: %lux%lu\n", (unsigned long)during.dmPelsWidth, (unsigned long)during.dmPelsHeight);
    }
    Sleep(300);
    ms = three(&k, &code);
    g_desk_w = 0;
    printf("  desktop width back %.1f ms after press 3, while the process lived (-1: only after its end)\n", g_mode_back_ms);
    Sleep(500);
    mode_now(&after);
    drain(&k);
    printf("%s", k.buf);
    printf("  third press to exit: %.1f ms, exit code %lu; after: %lux%lu@%lu\n", ms, (unsigned long)code,
           (unsigned long)after.dmPelsWidth, (unsigned long)after.dmPelsHeight, (unsigned long)after.dmDisplayFrequency);
    verdict("mode", code == YSCR_PANIC_EXIT_CODE && after.dmPelsWidth == before.dmPelsWidth &&
                    after.dmPelsHeight == before.dmPelsHeight && after.dmDisplayFrequency == before.dmDisplayFrequency);
    finish(&k);
    if (after.dmPelsWidth != before.dmPelsWidth || after.dmPelsHeight != before.dmPelsHeight) {
        ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);   /* never leave it changed */
        printf("  the mode was put back by the parent\n");
    }
    /* the same mode with the window thread: released, not ended */
    snprintf(args, sizeof args, "idle --wt on --hang-ms 1500 --idle-ms 4000 --mode %d %d %d %d", modes[pick].w,
             modes[pick].h, modes[pick].refresh_num, modes[pick].refresh_den);
    /* the panel takes a while after the mode came back: a switch at once
     * gave no flip statistics for 3 s, and settling failed (2026-10-27) */
    Sleep(3000);
    printf("mode-release: switching to %dx%d for about 3 s\n", modes[pick].w, modes[pick].h);
    case_release("mode-release", args, 1);
}

int main(int argc, char** argv) {
    int i, mode_test = 0;
    if (argc > 1 && !strcmp(argv[1], "--child")) return child(argc, argv);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode-test")) mode_test = 1;
        else { fprintf(stderr, "usage: screen_abort [--mode-test]\n"); return 2; }
    }
    case_live();
    case_hang("hang", "hang --wt off", 300);
    case_hang("ghost", "hang --wt off", 7000);
    case_held();
    case_release("release", "idle --wt on --idle-ms 4000", 0);
    case_hang("kill", "block --wt on", 300);
    if (mode_test) case_mode();
    return g_fail;
}
#endif
