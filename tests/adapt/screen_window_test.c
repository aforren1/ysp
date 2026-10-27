/* screen_window_test.c - ysp/screen.h's window thread (v0.10.0) on real
 * windows: self-checking, Windows only, with a display.
 *
 * The core test (screen_test.c) checks the window thread's rules without
 * SDL. This program runs the thread itself: windows it owns, a host that
 * sleeps between frames, yscr_window_call(), the display lists while it
 * runs, text input and the memory of its strings, the panic's release and
 * the end of a process whose window thread does not answer, two screens,
 * open and close cycles (handles, threads, memory), a fullscreen change
 * while open, gamepads, raw mice and triggers. It prints one line per
 * check and returns 0 when every check passed, 1 otherwise, 2 for a bad
 * argument. Windows are small and dark gray, in the top-left corner;
 * keys and mouse moves are sent with SendInput only while the test's own
 * window is the foreground window.
 *
 *     test_screen_window              every case but --mode (about 60 s)
 *     test_screen_window --mode       also a display-mode switch in
 *                                     fullscreen (the display shows another
 *                                     mode for about 2 s)
 *     test_screen_window --only NAME  one case: idle call displays mixed
 *                                     text release kill two cycles
 *                                     fullscreen gamepads mice triggers mode
 *                                     (or YSCR_WINDOW_TEST_ONLY=NAME)
 *
 * Build: cl /W4 /Iinclude /I<SDL3>/include tests\adapt\screen_window_test.c
 * /link <SDL3>/lib/x64/SDL3.lib; SDL3.dll beside it or on PATH, ANGLE in
 * YSCR_ANGLE_DIR. CMake builds it as test_screen_window and does not run
 * it: CI has no display.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* The test seam of examples/screen/abort.c: arm the watchdog in a window,
 * for the kill case. */
#define YSCR__PANIC_WINDOWED 1
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
int main(void) { printf("screen_window_test: Windows only (the window thread is)\n"); return 0; }
#else
#include <windows.h>

typedef void (__stdcall *gl_clear_color_fn)(float, float, float, float);
typedef void (__stdcall *gl_clear_fn)(unsigned int);

static int g_fail, g_checks;
static void check(const char* what, int ok) {
    g_checks++;
    if (!ok) g_fail++;
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
}

/* --- SDL's allocations, counted (SDL_SetMemoryFunctions, before any SDL call) */
static volatile LONG g_sdl_allocs;
static SDL_malloc_func g_m;
static SDL_calloc_func g_c;
static SDL_realloc_func g_r;
static SDL_free_func g_f;
static void* SDLCALL c_malloc(size_t n) { void* p = g_m(n); if (p) InterlockedIncrement(&g_sdl_allocs); return p; }
static void* SDLCALL c_calloc(size_t a, size_t b) { void* p = g_c(a, b); if (p) InterlockedIncrement(&g_sdl_allocs); return p; }
static void* SDLCALL c_realloc(void* q, size_t n) {
    void* p = g_r(q, n);
    if (p && !q) InterlockedIncrement(&g_sdl_allocs);
    else if (!p && q && n == 0) InterlockedDecrement(&g_sdl_allocs);
    return p;
}
static void SDLCALL c_free(void* p) { if (p) InterlockedDecrement(&g_sdl_allocs); g_f(p); }

/* --- the process's own counts ---------------------------------------------- */
typedef struct { DWORD handles, threads; SIZE_T priv; LONG sdl; } proc_counts;
typedef BOOL (WINAPI *pmi_fn)(HANDLE, void*, DWORD);
static int threads_of_process(void) {
    /* Toolhelp, from kernel32 */
    typedef HANDLE (WINAPI *snap_fn)(DWORD, DWORD);
    typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } te32;
    typedef BOOL (WINAPI *walk_fn)(HANDLE, te32*);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    snap_fn snap = (snap_fn)(yscr_proc)GetProcAddress(k, "CreateToolhelp32Snapshot");
    walk_fn first = (walk_fn)(yscr_proc)GetProcAddress(k, "Thread32First");
    walk_fn next = (walk_fn)(yscr_proc)GetProcAddress(k, "Thread32Next");
    HANDLE h;
    te32 t;
    int n = 0;
    if (!snap || !first || !next) return -1;
    h = snap(4 /* TH32CS_SNAPTHREAD */, 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    t.dwSize = sizeof t;
    if (first(h, &t)) do { if (t.th32OwnerProcessID == GetCurrentProcessId()) n++; } while (next(h, &t));
    CloseHandle(h);
    return n;
}
static void counts(proc_counts* c) {
    struct { DWORD cb, pf; SIZE_T peak_ws, ws, qp1, qp2, qnp1, qnp2, pagefile, peak_pagefile, priv; } m;
    pmi_fn pmi = (pmi_fn)(yscr_proc)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
    memset(&m, 0, sizeof m);
    m.cb = sizeof m;
    GetProcessHandleCount(GetCurrentProcess(), &c->handles);
    c->threads = (DWORD)threads_of_process();
    c->priv = pmi && pmi(GetCurrentProcess(), &m, sizeof m) ? m.priv : 0;
    c->sdl = g_sdl_allocs;
}

/* --- a screen ---------------------------------------------------------------- */
static gl_clear_color_fn g_cc;
static gl_clear_fn g_cl;

static void base_desc(yscr_desc* d, int32_t wt) {
    memset(d, 0, sizeof *d);
    d->windowed = true;
    d->window_w = 480;
    d->window_h = 300;
    d->foreground = YSCR_FOREGROUND_FORCE;
    d->settle = YSCR_SETTLE_OFF;
    d->window_thread = wt;
}

static int open_screen(yscr_screen* s, const yscr_desc* d) {
    memset(s, 0, sizeof *s);
    if (!yscr_open(s, d)) { printf("  open: %s\n", yscr_error(s)); return 0; }
    g_cc = (gl_clear_color_fn)yscr_gl_proc(s, "glClearColor");
    g_cl = (gl_clear_fn)yscr_gl_proc(s, "glClear");
    return 1;
}

static HWND hwnd_of(yscr_screen* s) { return (HWND)s->hwnd; }

/* n frames, events read before each begin(). Returns the last rc; *drops
 * gets the records with a dropped vblank. */
static int frames(yscr_screen* s, int n, int* drops, int* texts) {
    int i, rc = YSCR_OK;
    yscr_frame f;
    SDL_Event ev;
    for (i = 0; i < n; i++) {
        int k;
        while (yscr_poll(s, &ev, NULL)) if (texts && ev.type == SDL_EVENT_TEXT_INPUT) *texts += (int)strlen(ev.text.text);
        rc = yscr_begin(s, &f);
        if (rc != YSCR_OK) return rc;
        for (k = 0; drops && k < f.n_done; k++) if (f.done[k].dropped) (*drops)++;
        if (g_cc && g_cl) { g_cc(0.12f, 0.12f, 0.12f, 1.0f); g_cl(0x4000u); }
        rc = yscr_flip(s);
        if (rc != YSCR_OK) return rc;
    }
    return rc;
}

static int in_front(yscr_screen* s) { return GetForegroundWindow() == hwnd_of(s); }

/* --- watching the window from another thread (hung, ghost) ------------------ */
static struct { HWND h; volatile LONG stop, hung, ghost, lost_fg; HANDLE th; } g_w;
static DWORD WINAPI watch_main(void* p) {
    (void)p;
    while (!g_w.stop) {
        HWND fg = GetForegroundWindow();
        char cls[16] = "";
        if (IsHungAppWindow(g_w.h)) g_w.hung = 1;
        if (fg && GetClassNameA(fg, cls, sizeof cls) && !strcmp(cls, "Ghost")) g_w.ghost = 1;
        if (fg != g_w.h) g_w.lost_fg = 1;
        Sleep(50);
    }
    return 0;
}
static void watch_start(HWND h) {
    memset((void*)&g_w, 0, sizeof g_w);
    g_w.h = h;
    g_w.th = CreateThread(NULL, 0, watch_main, NULL, 0, NULL);
}
static void watch_stop(void) {
    g_w.stop = 1;
    WaitForSingleObject(g_w.th, INFINITE);
    CloseHandle(g_w.th);
}

/* --- the cases ------------------------------------------------------------------ */

/* A host that sleeps between frames: no window marked hung, no ghost. */
static void case_idle(void) {
    static yscr_screen s;
    yscr_desc d;
    char line[1024];
    int drops = 0, rc;
    printf("idle: frames, Sleep(5 s) with no pump, frames\n");
    base_desc(&d, YSCR_WINDOW_THREAD_AUTO);
    d.settle = 0;   /* AUTO: settled, as a run would be */
    if (!open_screen(&s, &d)) { check("open (AUTO)", 0); return; }
    yscr_describe(&s, line, sizeof line);
    check("AUTO puts the window on the window thread", s.wt == 1 && strstr(line, "window_thread=on(auto)") != NULL);
    frames(&s, 60, NULL, NULL);
    watch_start(hwnd_of(&s));
    Sleep(5000);
    watch_stop();
    check("not hung during 5 s without a pump", !g_w.hung);
    check("no ghost window", !g_w.ghost);
    rc = frames(&s, 30, &drops, NULL);
    check("frames after the gap", rc == YSCR_OK);
    printf("  records with a dropped vblank after the gap: %d of 30 (not a check: timing)\n", drops);
    yscr_close(&s);
}

static void noop(void* p) { (void)p; }
static void set_title(void* p) { SDL_SetWindowTitle(yscr_window((yscr_screen*)p), "screen_window_test"); }
typedef struct { yscr_screen* s; int inner; DWORD tid; } nest_ctx;
static void inner_fn(void* p) { ((nest_ctx*)p)->inner = (int)GetCurrentThreadId(); }
static void outer_fn(void* p) {
    nest_ctx* n = (nest_ctx*)p;
    n->tid = GetCurrentThreadId();
    yscr_window_call(n->s, inner_fn, n);   /* on the window thread: runs inline */
}
static volatile LONG g_calls;
static void count_fn(void* p) { (void)p; InterlockedIncrement(&g_calls); }
static DWORD WINAPI caller_main(void* p) {
    int i;
    for (i = 0; i < 200; i++) yscr_window_call((yscr_screen*)p, count_fn, NULL);
    return 0;
}
static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void case_call(void) {
    static yscr_screen s;
    static int64_t t[1000];
    yscr_desc d;
    nest_ctx n;
    HANDLE th[2];
    int i;
    char title[64] = "";
    printf("call: yscr_window_call\n");
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    if (!open_screen(&s, &d)) { check("open (ON)", 0); return; }
    frames(&s, 10, NULL, NULL);
    check("a call that sets the title", yscr_window_call(&s, set_title, &s) == YSCR_OK &&
                                           GetWindowTextA(hwnd_of(&s), title, sizeof title) > 0 &&
                                           !strcmp(title, "screen_window_test"));
    memset(&n, 0, sizeof n);
    n.s = &s;
    check("runs on the window thread, a nested call inline", yscr_window_call(&s, outer_fn, &n) == YSCR_OK &&
          n.tid == yscr__wt.tid && n.inner == (int)yscr__wt.tid && n.tid != GetCurrentThreadId());
    g_calls = 0;
    th[0] = CreateThread(NULL, 0, caller_main, &s, 0, NULL);
    th[1] = CreateThread(NULL, 0, caller_main, NULL, 0, NULL);
    for (i = 0; i < 200; i++) yscr_window_call(&s, count_fn, NULL);
    WaitForMultipleObjects(2, th, TRUE, INFINITE);
    CloseHandle(th[0]); CloseHandle(th[1]);
    check("600 calls from 3 threads, each ran once", g_calls == 600);
    for (i = 0; i < 1000; i++) {
        int64_t t0 = yrt_now_ns();
        yscr_window_call(&s, noop, NULL);
        t[i] = yrt_now_ns() - t0;
    }
    qsort(t, 1000, sizeof t[0], cmp64);
    printf("  round trip of an empty call: median %.1f us, p99 %.1f us, max %.1f us (1000)\n",
           t[500] * 1e-3, t[990] * 1e-3, t[999] * 1e-3);
    check("frames after the calls", frames(&s, 10, NULL, NULL) == YSCR_OK);
    yscr_close(&s);
    check("closed: the thread is gone", yscr__wt.thread == NULL && yscr__wt.users == 0);
}

static void case_displays(void) {
    static yscr_screen s;
    yscr_display_info a[8], b[8];
    yscr_mode ma[64], mb[64];
    yscr_desc d;
    int na, nb, ka, kb;
    printf("displays: the lists while the window thread runs\n");
    na = yscr_displays(a, 8);
    ka = yscr_modes(0, ma, 64);
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    if (!open_screen(&s, &d)) { check("open (ON)", 0); return; }
    nb = yscr_displays(b, 8);
    kb = yscr_modes(0, mb, 64);
    printf("  before open: %d display(s), id %u %dx%d; with the thread: %d, id %u %dx%d\n", na, na > 0 ? a[0].id : 0,
           na > 0 ? a[0].w : 0, na > 0 ? a[0].h : 0, nb, nb > 0 ? b[0].id : 0, nb > 0 ? b[0].w : 0, nb > 0 ? b[0].h : 0);
    check("yscr_displays() the same with the thread", na > 0 && na == nb && !strcmp(a[0].name, b[0].name) &&
                                                        a[0].desktop.w == b[0].desktop.w);
    check("yscr_modes() the same with the thread", ka > 0 && ka == kb && ma[0].w == mb[0].w);
    {
        yscr_mode out;
        double err;
        check("yscr_mode_multiple() runs with the thread", yscr_mode_multiple(0, 60, 1, 0, 0, 0, &out, &err) >= 1 ||
                                                             yscr_mode_multiple(0, 60, 1, 0, 0, 0, &out, &err) == YSCR_ERR_REFUSED);
    }
    check("frames", frames(&s, 10, NULL, NULL) == YSCR_OK);
    yscr_close(&s);
}

/* One mode per process; AUTO joins the screens already open. */
static void case_mixed(void) {
    static yscr_screen s, s2;
    yscr_desc d;
    printf("mixed: one mode per process\n");
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    if (!open_screen(&s, &d)) { check("open (ON)", 0); return; }
    base_desc(&d, YSCR_WINDOW_THREAD_OFF);
    memset(&s2, 0, sizeof s2);
    check("OFF refused while ON is open", !yscr_open(&s2, &d) && strstr(yscr_error(&s2), "one mode per process") != NULL);
    yscr_close(&s);
    check("OFF after the close", open_screen(&s, &d) && s.wt == 0 && frames(&s, 10, NULL, NULL) == YSCR_OK);
    base_desc(&d, YSCR_WINDOW_THREAD_AUTO);
    {
        char line[1024];
        int ok = open_screen(&s2, &d);
        if (ok) yscr_describe(&s2, line, sizeof line);
        check("AUTO joins OFF", ok && s2.wt == 0 && strstr(line, "window_thread=off(auto,other-screen)") != NULL);
        if (ok) yscr_close(&s2);
    }
    yscr_close(&s);
}

/* SendInput of n characters by UTF-16 code (KEYEVENTF_UNICODE): the
 * message path, as text input takes keys. */
static void type_chars(int n) {
    int i;
    for (i = 0; i < n; i++) {
        INPUT in[2];
        memset(in, 0, sizeof in);
        in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = (WORD)('a' + i % 26); in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

static void case_text(void) {
    static yscr_screen s;
    yscr_desc d;
    int texts = 0, round, sent = 0, got_total = 0;
    LONG first = 0, last = 0;
    printf("text: text input on the window thread, and its strings (YSCR__TLS_CLEANUP %d)\n", YSCR__TLS_CLEANUP);
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    if (!open_screen(&s, &d)) { check("open (ON)", 0); return; }
    frames(&s, 20, NULL, NULL);
    check("text input on", yscr_text_input(&s, true, 10, 10, 200, 30) == YSCR_OK);
    check("SDL says it is on (read on the window thread)", SDL_TextInputActive(yscr_window(&s)));
    frames(&s, 5, NULL, NULL);
    /* 5 rounds of 100 characters, each polled, then a begin(): SDL's event
     * queue keeps the entries of the first round's burst for reuse, so the
     * growth is counted from round 1 on */
    for (round = 0; round < 5; round++) {
        int k;
        if (!in_front(&s)) { printf("  the window is not in front: no keys sent\n"); break; }
        type_chars(100);
        sent += 100;
        texts = 0;
        for (k = 0; k < 30; k++) frames(&s, 1, NULL, &texts);
        got_total += texts;
        if (round == 0) first = g_sdl_allocs;
        last = g_sdl_allocs;
    }
    printf("  sent %d characters, polled %d; SDL allocations outstanding after round 1: %ld, after round 5: %ld "
           "(%+.2f per character)\n", sent, got_total, (long)first, (long)last,
           sent > 100 ? (double)(last - first) / (sent - 100) : 0.0);
    check("every typed character arrived as text", sent > 0 && got_total == sent);
    if (YSCR__TLS_CLEANUP) check("no SDL allocation left per character (< 0.1)", sent == 500 && last - first < 40);
    check("text input off", yscr_text_input(&s, false, 0, 0, 0, 0) == YSCR_OK && !SDL_TextInputActive(yscr_window(&s)));
    yscr_close(&s);
}

/* The panic's release, as the watchdog runs it (yscr__panic), on a host
 * that sleeps: the window minimized, the process lives, every begin()
 * reports YSCR_ABORT_PANIC until close; a new open is a new session. */
static DWORD WINAPI panic_main(void* p) { (void)p; yscr__panic(); return 0; }
static void case_release(void) {
    static yscr_screen s;
    static yrt_event ev[64];
    static unsigned char ring_mem[YRT_RING_BYTES(64)];
    yrt_ring ring;
    yrt_ring_desc rd;
    yscr_desc d;
    yscr_frame f;
    HANDLE th;
    int rc1, rc2, i, n, rec = 0;
    uint32_t a1, a2;
    printf("release: the panic while the window thread answers\n");
    memset(&rd, 0, sizeof rd);
    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    yrt_ring_open(&ring, &rd);
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    d.panic = true;
    d.ring = &ring;
    if (!open_screen(&s, &d)) { check("open (ON, panic)", 0); return; }
    frames(&s, 20, NULL, NULL);
    th = CreateThread(NULL, 0, panic_main, NULL, 0, NULL);
    Sleep(500);   /* the host sleeps */
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    check("the window is minimized", IsIconic(hwnd_of(&s)) != 0);
    printf("  release took %.1f ms on the window thread\n", (double)(yscr__wt.t_release - yscr__wd.t_panic) * 1e-6);
    rc1 = yscr_begin(&s, &f); a1 = f.abort;
    rc2 = yscr_begin(&s, &f); a2 = f.abort;
    check("begin(): QUIT with PANIC, and again", rc1 == YSCR_QUIT && (a1 & YSCR_ABORT_PANIC) && rc2 == YSCR_QUIT &&
                                                  a2 == YSCR_ABORT_PANIC);
    n = yrt_ring_drain(&ring, ev, 64);
    for (i = 0; i < n; i++) if (ev[i].kind == YSCR_EV_PANIC && ev[i].u.u32[1] == 1) rec++;
    check("one YSCR_EV_PANIC record, released", rec == 1);
    yscr_close(&s);
    check("a new open after the close", open_screen(&s, &d) && frames(&s, 10, NULL, NULL) == YSCR_OK);
    yscr_close(&s);
}


/* The window thread does not answer: the watchdog ends the process. The
 * child blocks its window thread with a window call of 4 s; 1.5 s into it
 * the heartbeat is older than 1 s, and the panic (called as the watchdog
 * calls it) must end the child with YSCR_PANIC_EXIT_CODE. */
static void block_fn(void* p) { Sleep((DWORD)(uintptr_t)p); }
static DWORD WINAPI late_panic_main(void* p) { (void)p; Sleep(1500); yscr__panic(); return 0; }
static int child_kill(void) {
    static yscr_screen s;
    yscr_desc d;
    HANDLE th;
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    d.panic = true;
    if (!open_screen(&s, &d)) return 1;
    frames(&s, 20, NULL, NULL);
    printf("blocking the window thread for 4 s\n");
    fflush(stdout);
    th = CreateThread(NULL, 0, late_panic_main, NULL, 0, NULL);
    yscr_window_call(&s, block_fn, (void*)(uintptr_t)4000);
    WaitForSingleObject(th, INFINITE);
    printf("the process lived\n");
    fflush(stdout);
    yscr_close(&s);
    return 0;
}

static void case_kill(void) {
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0, w;
    int64_t t0;
    printf("kill: the panic when the window thread does not answer (a child process)\n");
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --child-kill", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { check("start the child", 0); return; }
    t0 = yrt_now_ns();
    w = WaitForSingleObject(pi.hProcess, 20000);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 7);
    GetExitCodeProcess(pi.hProcess, &code);
    printf("  child exit code %lu after %.1f s\n", (unsigned long)code, (double)(yrt_now_ns() - t0) * 1e-9);
    check("the child ended with YSCR_PANIC_EXIT_CODE", w == WAIT_OBJECT_0 && code == YSCR_PANIC_EXIT_CODE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* Two screens on one window thread; a group frame; one closes first. */
static void move_fn(void* p) { SDL_SetWindowPosition(yscr_window((yscr_screen*)p), 520, 40); }
static void case_two(void) {
    static yscr_screen s[2];
    yscr_screen* sp[2];
    yscr_frame f[2];
    yscr_desc d;
    int i, ok = 1;
    printf("two: two screens on one window thread\n");
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    if (!open_screen(&s[0], &d)) { check("open the first", 0); return; }
    d.display_index = 1;
    if (!open_screen(&s[1], &d)) { check("open the second", 0); yscr_close(&s[0]); return; }
    yscr_window_call(&s[1], move_fn, &s[1]);
    check("one thread, two users", s[0].wt && s[1].wt && yscr__wt.users == 2);
    sp[0] = &s[0]; sp[1] = &s[1];
    for (i = 0; i < 60 && ok; i++) {
        ok = yscr_begin_group(sp, 2, f) == YSCR_OK;
        if (ok) {
            yscr_bind(&s[0]); g_cc(0.12f, 0.12f, 0.12f, 1.0f); g_cl(0x4000u);
            yscr_bind(&s[1]); g_cc(0.12f, 0.12f, 0.12f, 1.0f); g_cl(0x4000u);
            ok = yscr_flip_group_at(sp, 2, f[0].onset) == YSCR_OK;
        }
    }
    check("60 group frames", ok);
    yscr_close(&s[0]);
    check("the other runs on after the first closes", yscr__wt.users == 1 && frames(&s[1], 30, NULL, NULL) == YSCR_OK);
    yscr_close(&s[1]);
    check("both closed: the thread is gone", yscr__wt.thread == NULL);
}

/* Open and close cycles: nothing grows with the cycles, and the window
 * thread adds nothing to what 0.9.0's path (OFF) leaves. Counted from the
 * second cycle, after the first opens have loaded ANGLE, D3D and SDL. */
static int cycles(int32_t wt, int n, proc_counts* c1, proc_counts* c2) {
    static yscr_screen s;
    yscr_desc d;
    int i, ok = 1;
    base_desc(&d, wt);
    for (i = 0; i < n && ok; i++) {
        ok = open_screen(&s, &d) && frames(&s, 20, NULL, NULL) == YSCR_OK;
        yscr_close(&s);
        if (i == 1) counts(c1);
    }
    counts(c2);
    return ok;
}

static void case_cycles(void) {
    proc_counts a1, a2, b1, b2;
    const int n = 10;
    int ok_off, ok_on;
    double h_off, h_on;
    printf("cycles: %d opens and closes, window thread OFF, then ON\n", n);
    ok_off = cycles(YSCR_WINDOW_THREAD_OFF, n, &a1, &a2);
    ok_on = cycles(YSCR_WINDOW_THREAD_ON, n, &b1, &b2);
    h_off = ((double)a2.handles - (double)a1.handles) / (n - 2);
    h_on = ((double)b2.handles - (double)b1.handles) / (n - 2);
    printf("  OFF, cycle 2 to %d: handles %+ld (%+.2f a cycle), threads %+ld, SDL allocations %+ld, private %+.2f MB\n", n,
           (long)a2.handles - (long)a1.handles, h_off, (long)a2.threads - (long)a1.threads, (long)(a2.sdl - a1.sdl),
           ((double)a2.priv - (double)a1.priv) / 1048576.0);
    printf("  ON,  cycle 2 to %d: handles %+ld (%+.2f a cycle), threads %+ld, SDL allocations %+ld, private %+.2f MB\n", n,
           (long)b2.handles - (long)b1.handles, h_on, (long)b2.threads - (long)b1.threads, (long)(b2.sdl - b1.sdl),
           ((double)b2.priv - (double)b1.priv) / 1048576.0);
    check("every cycle opened and ran", ok_off && ok_on);
    check("ON: no more handles a cycle than OFF (+0.5)", h_on <= h_off + 0.5);
    check("ON: no thread left behind", b2.threads <= b1.threads);
    check("ON: no SDL allocation left behind", b2.sdl <= b1.sdl);
}

/* Fullscreen off and on again while open, through the window call. */
typedef struct { yscr_screen* s; int on; } fs_arg;
static void fs_fn(void* p) {
    fs_arg* a = (fs_arg*)p;
    SDL_SetWindowFullscreen(yscr_window(a->s), a->on != 0);
    SDL_SyncWindow(yscr_window(a->s));
}
static void case_fullscreen(void) {
    static yscr_screen s;
    yscr_desc d;
    fs_arg a;
    int ok;
    yscr_caps caps;
    printf("fullscreen: a fullscreen screen leaves fullscreen and comes back while open\n");
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    d.windowed = false;
    if (!open_screen(&s, &d)) { check("open fullscreen (ON)", 0); return; }
    ok = frames(&s, 60, NULL, NULL) == YSCR_OK;
    a.s = &s; a.on = 0;
    ok = ok && yscr_window_call(&s, fs_fn, &a) == YSCR_OK;
    ok = ok && frames(&s, 60, NULL, NULL) == YSCR_OK;
    a.on = 1;
    ok = ok && yscr_window_call(&s, fs_fn, &a) == YSCR_OK;
    ok = ok && frames(&s, 60, NULL, NULL) == YSCR_OK;
    yscr_get_caps(&s, &caps);
    check("frames ran through both changes", ok);
    yscr_close(&s);
}

static void count_pads(void* p) {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_free(ids);
    *(int*)p = n;
}
static void case_gamepads(void) {
    static yscr_screen s, s2;
    yscr_desc d;
    int n = -1;
    printf("gamepads: desc.gamepads with the window thread, beside a screen without\n");
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    if (!open_screen(&s2, &d)) { check("open without gamepads", 0); return; }
    d.gamepads = true;
    if (!open_screen(&s, &d)) { check("open with gamepads", 0); yscr_close(&s2); return; }
    yscr_window_call(&s, count_pads, &n);
    printf("  gamepads attached: %d%s\n", n, n == 0 ? " (none: open and close only, no pad events)" : "");
    check("the thread wakes every 4 ms with gamepads", yscr__wt.pads == 1);
    check("frames", frames(&s, 60, NULL, NULL) == YSCR_OK);
    yscr_close(&s);
    check("back to 100 ms when that screen closes", yscr__wt.pads == 0 && frames(&s2, 10, NULL, NULL) == YSCR_OK);
    yscr_close(&s2);
}

static void case_mice(void) {
    static yscr_screen s;
    yscr_desc d;
    SDL_Event ev;
    int i, raw = 0, k;
    printf("mice: desc.raw_mice with the window thread\n");
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    d.raw_mice = true;
    if (!open_screen(&s, &d)) { check("open with raw mice", 0); return; }
    frames(&s, 20, NULL, NULL);
    for (i = 0; i < 20 && in_front(&s); i++) {
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dx = (i & 1) ? 3 : -3;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
        Sleep(10);
    }
    for (k = 0; k < 20; k++) {
        yscr_frame f;
        while (yscr_poll(&s, &ev, NULL)) {
            yin_event e;
            if (ev.type == yscr_input_event_type() && yscr_event_input(&s, &ev, &e) && e.kind == YIN_KIND_MOUSE) raw++;
        }
        if (yscr_begin(&s, &f) != YSCR_OK) break;
        g_cc(0.12f, 0.12f, 0.12f, 1.0f); g_cl(0x4000u);
        yscr_flip(&s);
    }
    printf("  sent %d moves, raw mouse events %d\n", i, raw);
    check("raw mouse reports through the bridge", i == 20 && raw >= 20);
    yscr_close(&s);
}

static volatile LONG g_fired;
static void trig_fn(void* ctx, const yscr_trigger_info* t) { (void)ctx; (void)t; InterlockedIncrement(&g_fired); }
static void case_triggers(void) {
    static yscr_screen s;
    yscr_desc d;
    yscr_trigger_desc td;
    yscr_frame f;
    int i, ok = 1;
    printf("triggers: a trigger channel with the window thread\n");
    memset(&td, 0, sizeof td);
    td.fn = trig_fn;
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    d.triggers = &td;
    d.n_triggers = 1;
    if (!open_screen(&s, &d)) { check("open with a trigger", 0); return; }
    g_fired = 0;
    for (i = 0; i < 60 && ok; i++) {
        ok = yscr_begin(&s, &f) == YSCR_OK && yscr_trigger(&s, 0, (uint32_t)i) == YSCR_OK;
        g_cc(0.12f, 0.12f, 0.12f, 1.0f); g_cl(0x4000u);
        ok = ok && yscr_flip(&s) == YSCR_OK;
    }
    yscr_wait_flip(&s, NULL);
    Sleep(50);
    printf("  triggers fired: %ld of 60\n", (long)g_fired);
    check("every trigger fired", ok && g_fired == 60);
    yscr_close(&s);
}

/* A display mode other than the desktop's, fullscreen, on the window thread. */
static void case_mode(void) {
    static yscr_screen s;
    yscr_mode modes[64];
    yscr_display_info di;
    yscr_desc d;
    yscr_caps caps;
    DEVMODEW before, during, after;
    int i, n = yscr_modes(0, modes, 64), pick = -1;
    printf("mode: a mode switch at open with the window thread\n");
    if (yscr_displays(&di, 1) < 1) { check("a display", 0); return; }
    for (i = 0; i < n && i < 64; i++)
        if ((modes[i].w != di.desktop.w || modes[i].h != di.desktop.h) && modes[i].w < di.desktop.w &&
            (int64_t)modes[i].refresh_num * di.desktop.refresh_den == (int64_t)di.desktop.refresh_num * modes[i].refresh_den) {
            pick = i;
            break;
        }
    if (pick < 0) { printf("  no other mode at the desktop's refresh: not run\n"); return; }
    memset(&before, 0, sizeof before); before.dmSize = sizeof before;
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &before);
    base_desc(&d, YSCR_WINDOW_THREAD_ON);
    d.windowed = false;
    d.mode = modes[pick];
    printf("  desktop %dx%d, switching to %dx%d\n", di.desktop.w, di.desktop.h, modes[pick].w, modes[pick].h);
    if (!open_screen(&s, &d)) { check("open in the other mode", 0); return; }
    memset(&during, 0, sizeof during); during.dmSize = sizeof during;
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &during);
    yscr_get_caps(&s, &caps);
    check("the display is in the mode asked for", (int)during.dmPelsWidth == modes[pick].w &&
                                                   caps.mode.w == modes[pick].w && caps.mode.h == modes[pick].h);
    check("frames", frames(&s, 60, NULL, NULL) == YSCR_OK);
    yscr_close(&s);
    Sleep(300);
    memset(&after, 0, sizeof after); after.dmSize = sizeof after;
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &after);
    check("the desktop mode back after close", after.dmPelsWidth == before.dmPelsWidth &&
                                               after.dmPelsHeight == before.dmPelsHeight &&
                                               after.dmDisplayFrequency == before.dmDisplayFrequency);
    if (after.dmPelsWidth != before.dmPelsWidth) ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
}

typedef struct { const char* name; void (*fn)(void); int default_on; } test_case;
static const test_case g_cases[] = {
    { "idle", case_idle, 1 }, { "call", case_call, 1 }, { "displays", case_displays, 1 },
    { "mixed", case_mixed, 1 }, { "text", case_text, 1 }, { "release", case_release, 1 },
    { "kill", case_kill, 1 }, { "two", case_two, 1 }, { "cycles", case_cycles, 1 },
    { "fullscreen", case_fullscreen, 1 }, { "gamepads", case_gamepads, 1 }, { "mice", case_mice, 1 },
    { "triggers", case_triggers, 1 }, { "mode", case_mode, 0 }
};

int main(int argc, char** argv) {
    int i, k, mode = 0;
    const char* only = NULL;
    SDL_GetOriginalMemoryFunctions(&g_m, &g_c, &g_r, &g_f);
    SDL_SetMemoryFunctions(c_malloc, c_calloc, c_realloc, c_free);
    if (argc > 1 && !strcmp(argv[1], "--child-kill")) return child_kill();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode")) mode = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else { fprintf(stderr, "usage: test_screen_window [--mode] [--only NAME]\n"); return 2; }
    }
    if (!only) only = getenv("YSCR_WINDOW_TEST_ONLY");   /* tests/mutate/screen.toml picks a case */
    if (only && !*only) only = NULL;
    yrt_thread_elevate(NULL);
    for (k = 0; k < (int)(sizeof g_cases / sizeof g_cases[0]); k++) {
        const test_case* c = &g_cases[k];
        if (only ? strcmp(only, c->name) != 0 : !(c->default_on || mode)) continue;
        c->fn();
    }
    if (only && g_checks == 0) { fprintf(stderr, "test_screen_window: no case %s\n", only); return 2; }
    printf("screen_window_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
#endif
