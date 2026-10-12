/* screen_input.c - how good ysp/screen.h's input timestamps are here.
 *
 *   1. The restamp's clock correlation: yrt_correlate() of SDL_GetTicksNS
 *      against the ysp_rt clock, 1000 times: width and cost.
 *   2. Key events injected at known ysp_rt times and read back with
 *      yscr_poll(). Linux X11: through XTEST on a connection of its own,
 *      keycode 192 (XF86Launch5 in Xfce's keymap; nothing binds it), which
 *      enters the X server above the kernel, as SendInput enters Windows.
 *      ysp/screen.h stamps X11 keys with the X server's time (ms), so the
 *      SDL timestamp's sub-ms part is the restamp's, not SDL's.
 *      Windows: SendInput(), with SDL's message-loop keyboard
 *      and then with SDL_HINT_WINDOWS_RAW_KEYBOARD. When the window cannot
 *      take the keyboard focus (a program started in the background does
 *      not get it), WM_KEYDOWN is posted to the window instead, which tests
 *      the message-loop path only. For each event: the
 *      restamped time minus the injection time, and the sub-millisecond part
 *      of SDL's own timestamp, which shows the granularity of the OS path.
 *
 * The key is F24, which nothing maps by default, and it is injected only
 * while this program's window has the keyboard focus, so it cannot type into
 * another program. To get the foreground, the program sends one zero-size
 * mouse move, which does not move the pointer.
 *
 * Usage: screen_input [--timer-res] [--panic] [--reports] [--hand [s]] [--devices]
 *                     [--mice [s]] [injections]
 *   injections   per path, default 200 (50 with --reports)
 *   --timer-res  call yrt_timer_resolution_begin() first (timeBeginPeriod)
 *   --panic      arm the panic watchdog (its keyboard hook sees every key of
 *                the session) and test the raw path only: the hook's cost
 *   --reports    on the raw path, count SDL's key reports per injected press:
 *                virtual-key against scan-code injection, a tap against a
 *                100 ms hold (the double report, docs/response.md)
 *   --hand [s]   keys pressed by hand for s seconds (default 30): reports per
 *                press, and from which path. Click the window first
 *   --mice [s]   desc.raw_mice by hand for s seconds (default 30): each
 *                click and wheel notch with its mouse, then one line per
 *                mouse. Two physical mice should come apart
 *   --devices    the input devices ysp/screen.h logs to its ring
 *                (YSCR_EV_DEVICE): kind, id and name; then 5 s of key and
 *                mouse events with the device id each one carries

 * Esc or closing the window ends the run at once.
 * Exit code: 0, 1 when the screen did not open, 2 for a bad argument.
 */
/* --panic arms the watchdog in this program's window (a test seam). */
#define YSCR__PANIC_WINDOWED 1
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__linux__)
    #include <dlfcn.h>
#endif

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static void row(const char* name, double* v, int n) {
    double sum = 0;
    int i;
    if (n <= 0) { printf("  %-40s n=0\n", name); return; }
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    for (i = 0; i < n; i++) sum += v[i];
    printf("  %-40s n=%5d mean %9.3f p50 %9.3f p99 %9.3f max %9.3f min %9.3f\n", name, n, sum / n,
           v[n / 2], v[(int)((double)n * 0.99)], v[n - 1], v[0]);
}

static double g_a[1000], g_b[1000];

#if defined(_WIN32)
static void inject_f24(WORD flags) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_F24;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof in);
}

/* One keyboard path: SendInput through the message loop (0) or raw input
 * (1), or a WM_KEYDOWN posted to the window (2), which needs no keyboard
 * focus and reaches the message loop only. Returns 1 when Esc or close
 * ended the run. */
static int run_path(yscr_screen* s, int mode, int count) {
    static double delta[4096], subms[4096];
    static const char* const names[3] = { "SendInput, message loop", "SendInput, raw input",
                                          "posted WM_KEYDOWN, message loop" };
    int nd = 0, injected = 0, waiting = 0, quit = 0;
    int64_t t_inj = 0, give_up = 0;
    uint32_t rng = 777u + (uint32_t)mode;
    int64_t end = yrt_now_ns() + (int64_t)count * 60000000 + 5000000000LL;
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(yscr_window(s)),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, mode == 1 ? "1" : "0");
    while (injected < count && !quit && yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev;
        if (yscr_begin(s, &f) != YSCR_OK) { quit = 1; break; }
        while (yscr_poll(s, &ev, &t_ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) quit = 1;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F24 && waiting && nd < 4096) {
                delta[nd] = (double)(t_ev - t_inj) / 1e6;
                subms[nd] = (double)(ev.common.timestamp % 1000000u) / 1e3;
                nd++;
                waiting = 0;
            }
        }
        if (waiting && yrt_now_ns() > give_up) waiting = 0;   /* lost: not counted */
        if (!waiting && !quit && (mode == 2 || SDL_GetKeyboardFocus() == yscr_window(s))) {
            /* a random phase inside the frame, so the frame loop is no clock */
            rng = rng * 1664525u + 1013904223u;
            yrt_sleep_until(yrt_now_ns() + (rng >> 8) % 8000000u, 0);
            t_inj = yrt_now_ns();
            if (mode == 2) {
                PostMessageW(hwnd, WM_KEYDOWN, VK_F24, 1 | (0x76u << 16));
                PostMessageW(hwnd, WM_KEYUP, VK_F24, 1 | (0x76u << 16) | (3u << 30));
            } else {
                inject_f24(0);
                inject_f24(KEYEVENTF_KEYUP);
            }
            give_up = t_inj + 500000000;
            waiting = 1;
            injected++;
        }
        yscr_flip(s);
    }
    printf("%s: %d injected, %d received\n", names[mode], injected, nd);
    row("restamped event - injection, ms", delta, nd);
    row("SDL timestamp mod 1 ms, us", subms, nd);
    return quit;
}


/* --reports: how many key reports SDL gives per injected press on the raw
 * path, for virtual-key injection (wVk, scan code 0) and scan-code
 * injection (KEYEVENTF_SCANCODE), each with a tap (down and up back to
 * back) and a 100 ms hold. A report is a key-down that is not a repeat. */
typedef struct report_tally {
    int presses, downs, repeats, ups, doubled, raw_which, zero_which;
    double gap[512];
    int n_gap;
} report_tally;

static void inject_key(int scan, int up) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    if (scan) { in.ki.wScan = 0x76; in.ki.dwFlags = KEYEVENTF_SCANCODE; }
    else in.ki.wVk = VK_F24;
    if (up) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
}

static int run_reports(yscr_screen* s, int scan, int hold_ms, int count, report_tally* r) {
    int state = 0, quit = 0, downs_this = 0;
    int64_t t_next = yrt_now_ns() + 100000000, t_down = 0, t_first = 0;
    int64_t end = yrt_now_ns() + (int64_t)count * 400000000LL + 3000000000LL;
    memset(r, 0, sizeof *r);
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    while (!quit && yrt_now_ns() < end && (r->presses < count || state != 0 ||
                                                       yrt_now_ns() < t_next)) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev, now;
        if (yscr_begin(s, &f) != YSCR_OK) { quit = 1; break; }
        while (yscr_poll(s, &ev, &t_ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) quit = 1;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F24) {
                if (ev.key.repeat) { r->repeats++; continue; }
                r->downs++;
                if (ev.key.which) r->raw_which++; else r->zero_which++;
                if (downs_this == 0) t_first = t_ev;
                else if (r->n_gap < 512) r->gap[r->n_gap++] = (double)(t_ev - t_first) / 1e6;
                if (++downs_this == 2) r->doubled++;
            }
            if (ev.type == SDL_EVENT_KEY_UP && ev.key.scancode == SDL_SCANCODE_F24) r->ups++;
        }
        now = yrt_now_ns();
        if (state == 0 && r->presses < count && now >= t_next && SDL_GetKeyboardFocus() == yscr_window(s)) {
            downs_this = 0;
            inject_key(scan, 0);
            r->presses++;
            t_down = now;
            if (hold_ms == 0) { inject_key(scan, 1); t_next = now + 150000000; }
            else state = 1;
        } else if (state == 1 && now >= t_down + (int64_t)hold_ms * 1000000) {
            inject_key(scan, 1);
            state = 0;
            t_next = now + 150000000;
        }
        yscr_flip(s);
    }
    if (state == 1) inject_key(scan, 1);
    return quit;
}

static int reports_mode(yscr_screen* s, int count) {
    static const char* const inj[2] = { "virtual key (wVk, scan 0)", "scan code (KEYEVENTF_SCANCODE)" };
    report_tally r;
    int scan, h, quit = 0;
    static const int holds[2] = { 0, 100 };
    printf("raw keyboard path on; F24; %d presses per row\n", count);
    printf("  %-32s %5s %8s %6s %7s %5s %8s %10s %10s\n", "injection", "hold", "presses", "downs",
           "repeats", "ups", "doubled", "which!=0", "which==0");
    for (scan = 0; scan < 2 && !quit; scan++)
        for (h = 0; h < 2 && !quit; h++) {
            quit = run_reports(s, scan, holds[h], count, &r);
            printf("  %-32s %3dms %8d %6d %7d %5d %8d %10d %10d\n", inj[scan], holds[h], r.presses,
                   r.downs, r.repeats, r.ups, r.doubled, r.raw_which, r.zero_which);
            if (r.n_gap) row("    second report - first, ms", r.gap, r.n_gap);
        }
    return quit;
}

/* --hand: keys pressed by hand for `seconds`. For each press of a key
 * (a key-down that is not a repeat, after a key-up of that key or the
 * start), the reports SDL gives within 50 ms, and from which path: the
 * raw path carries the keyboard's device id (which != 0), the message
 * path 0. A physical key that reports once shows 1 report per press. */
static int hand_mode(yscr_screen* s, int seconds) {
    static int64_t last_down[SDL_SCANCODE_COUNT];
    int presses = 0, second = 0, raw = 0, msg = 0, repeats = 0, quit = 0;
    int64_t end = yrt_now_ns() + (int64_t)seconds * 1000000000LL;
    memset(last_down, 0, sizeof last_down);
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    printf("press keys by hand for %d s (letters, space; Esc ends early)\n", seconds);
    while (!quit && yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev;
        if (yscr_begin(s, &f) != YSCR_OK) { quit = 1; break; }
        while (yscr_poll(s, &ev, &t_ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) quit = 1;
            if (ev.type == SDL_EVENT_KEY_DOWN && (int)ev.key.scancode < SDL_SCANCODE_COUNT) {
                int64_t* ld = &last_down[ev.key.scancode];
                if (ev.key.repeat) { repeats++; continue; }
                if (ev.key.which) raw++; else msg++;
                if (*ld && t_ev - *ld < 50000000 && t_ev - *ld > -50000000) {
                    second++;
                    printf("  %-10s second report %+.3f ms after the first, which=%u\n",
                           SDL_GetScancodeName(ev.key.scancode), (double)(t_ev - *ld) / 1e6,
                           (unsigned)ev.key.which);
                } else {
                    presses++;
                    *ld = t_ev;
                    printf("  %-10s press, which=%u\n", SDL_GetScancodeName(ev.key.scancode),
                           (unsigned)ev.key.which);
                }
            }
        }
        yscr_flip(s);
    }
    printf("%d presses, %d second reports within 50 ms, %d OS repeats; reports with a device id %d, "
           "without %d\n", presses, second, repeats, raw, msg);
    if (!presses) printf("RESULT: no key was pressed (click the window first)\n");
    else printf(second ? "RESULT: some keys reported twice\n" : "RESULT: one report per press\n");
    return quit;
}
#endif

/* --devices: the ring's device records, then which device each key or
 * click comes from, to map a data file's device column. */
static unsigned char g_ring_mem[YRT_RING_BYTES(1024)];
static yrt_ring g_ring;
static yrt_event g_ev[1024];

static void devices_mode(yscr_screen* s) {
    static const char* const kinds[6] = { "?", "keyboard", "mouse", "gamepad", "touch", "pen" };
    static const char* const changes[3] = { "present", "added", "removed" };
    int i, n;
    int64_t end = yrt_now_ns() + 5000000000LL;
    printf("press keys or click in the window for 5 s (Esc ends early)\n");
    while (yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t;
        int quit = 0;
        if (yscr_begin(s, &f) != YSCR_OK) break;
        while (yscr_poll(s, &ev, &t)) {
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                printf("  key %-10s from keyboard %u\n", SDL_GetScancodeName(ev.key.scancode), (unsigned)ev.key.which);
                if (ev.key.key == SDLK_ESCAPE) quit = 1;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                printf("  click %u from mouse %u\n", (unsigned)ev.button.button, (unsigned)ev.button.which);
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
        }
        yscr_flip(s);
        if (quit) break;
    }
    n = yrt_ring_drain(&g_ring, g_ev, 1024);
    printf("devices in the ring:\n");
    for (i = 0; i < n; i++) {
        const yrt_event* e = &g_ev[i];
        if (e->source != YRT_SRC_SCREEN || e->kind != YSCR_EV_DEVICE) continue;
        printf("  %-8s %-7s id %-12llu %s\n", kinds[e->u.u32[0] < 6 ? e->u.u32[0] : 0],
               changes[e->u.u32[1] < 3 ? e->u.u32[1] : 0], (unsigned long long)e->u.u64[1], YSCR_DEV_NAME_OF(e));
    }
    {   /* the ring cuts names at 23 bytes; SDL's full names */
        SDL_KeyboardID* kb = SDL_GetKeyboards(&n);
        SDL_MouseID* m;
        printf("full names:\n");
        for (i = 0; kb && i < n; i++)
            printf("  keyboard id %-12u %s\n", (unsigned)kb[i], SDL_GetKeyboardNameForID(kb[i]));
        SDL_free(kb);
        m = SDL_GetMice(&n);
        for (i = 0; m && i < n; i++) printf("  mouse    id %-12u %s\n", (unsigned)m[i], SDL_GetMouseNameForID(m[i]));
        SDL_free(m);
    }
}

/* --mice: desc.raw_mice by hand, read through the input bridge. Each click
 * and wheel notch is printed with the mouse it came from; at the end, one
 * line per device: events, clicks, the summed movement in counts, and its
 * name. Device 0 is shown too: Windows gives no device handle to the
 * pointer it synthesizes from a precision touchpad, nor to injected input. Two physical mice
 * that come apart show as two devices. */
static void mice_mode(yscr_screen* s, int seconds) {
    struct { uint32_t device; long reports, clicks, wheel; long long ax, ay; int unlisted; } dev[16];
    int i, n, nd = 0, physical = 0;
    int64_t end = yrt_now_ns() + (int64_t)seconds * 1000000000LL;
    memset(dev, 0, sizeof dev);
    printf("move and click each mouse in the window for %d s (Esc ends early)\n", seconds);
    while (yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        yin_event m;
        int quit = 0;
        if (yscr_begin(s, &f) != YSCR_OK) break;
        /* one loop: SDL's events and the bridge's doorbells (v0.4.0) */
        while (yscr_poll(s, &ev, NULL)) {
            int k;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) quit = 1;
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
            if (ev.type != yscr_input_event_type() || !yscr_event_input(s, &ev, &m)) continue;
            if (m.kind != YIN_KIND_MOUSE) continue;
            for (k = 0; k < nd && dev[k].device != m.device; k++) { }
            if (k == nd) {
                if (nd == 16) continue;
                dev[nd++].device = m.device;
            }
            dev[k].reports++;
            dev[k].unlisted = (m.flags & YIN_UNLISTED) != 0;
            if (m.type == YIN_PRESS) {
                dev[k].clicks++;
                printf("  mouse %-10u button %u down\n", (unsigned)m.device, (unsigned)m.control);
            } else if (m.control == YIN_AXIS_DELTA) {
                dev[k].ax += (long long)(m.x < 0 ? -m.x : m.x);
                dev[k].ay += (long long)(m.y < 0 ? -m.y : m.y);
            } else if (m.control == YIN_AXIS_WHEEL && m.type == YIN_SAMPLE) {
                dev[k].wheel += (long)m.value;
                printf("  mouse %-10u wheel %+d\n", (unsigned)m.device, (int)m.value);
            }
        }
        yscr_flip(s);
        if (quit) break;
    }
    printf("per device (counts are raw, unaccelerated):\n");
    for (i = 0; i < nd; i++) {
        /* Windows synthesizes a precision touchpad's pointer, and SendInput,
         * remote desktop and VMs inject theirs, with no device handle */
        const char* name = dev[i].device ? SDL_GetMouseNameForID(dev[i].device)
                                         : "device 0 (system-synthesized: touchpad, injected, remote)";
        if (dev[i].device) physical++;
        printf("  mouse %-10u %6ld events, %3ld clicks, wheel %+4ld, moved %lld x %lld counts  %s%s\n",
               (unsigned)dev[i].device, dev[i].reports, dev[i].clicks, dev[i].wheel, dev[i].ax, dev[i].ay,
               name ? name : "", dev[i].unlisted ? " (not in SDL's list)" : "");
    }
    n = physical;
    printf(n >= 2 ? "RESULT: %d devices with their own handle came apart\n"
                  : "RESULT: %d device with its own handle; use two to check\n", n);
    if (nd > physical)
        printf("  device 0 is every pointer Windows makes itself: a precision touchpad's, injected input's\n");
}

#if defined(__linux__)
/* XTEST from a second X connection: the server routes the key to the window
 * with the focus, as it would a keyboard's. libX11 and libXtst are loaded
 * here, so the example links nothing more. Returns 1 when Esc or close ended
 * the run, -1 when XTEST is not there. */
#define XT_KEYCODE 192u
static int run_xtest(yscr_screen* s, int count) {
    static double delta[4096], subms[4096];
    void* (*open_display)(const char*) = NULL;
    int (*close_display)(void*) = NULL;
    int (*flush)(void*) = NULL;
    int (*fake_key)(void*, unsigned int, int, unsigned long) = NULL;
    void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL), *xtst = dlopen("libXtst.so.6", RTLD_NOW);
    void *p, *dpy;
    int nd = 0, injected = 0, waiting = 0, quit = 0;
    int64_t t_inj = 0, give_up = 0, end;
    uint32_t rng = 777u;
    if (!x11 || !xtst) { printf("XTEST: libX11.so.6 or libXtst.so.6 not found\n"); return -1; }
    p = dlsym(x11, "XOpenDisplay");  memcpy(&open_display, &p, sizeof p);
    p = dlsym(x11, "XCloseDisplay"); memcpy(&close_display, &p, sizeof p);
    p = dlsym(x11, "XFlush");        memcpy(&flush, &p, sizeof p);
    p = dlsym(xtst, "XTestFakeKeyEvent"); memcpy(&fake_key, &p, sizeof p);
    if (!open_display || !close_display || !flush || !fake_key) { printf("XTEST: an entry point is missing\n"); return -1; }
    dpy = open_display(NULL);
    if (!dpy) { printf("XTEST: cannot open the display\n"); return -1; }
    end = (int64_t)yrt_now_ns() + (int64_t)count * 60000000 + 5000000000LL;
    while ((injected < count || waiting) && !quit && (int64_t)yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev;
        if (yscr_begin(s, &f) != YSCR_OK) { quit = 1; break; }
        while (yscr_poll(s, &ev, &t_ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) quit = 1;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.raw == XT_KEYCODE && waiting && nd < 4096) {
                delta[nd] = (double)(t_ev - t_inj) / 1e6;
                subms[nd] = (double)(ev.common.timestamp % 1000000u) / 1e3;
                nd++;
                waiting = 0;
            }
        }
        if (waiting && yrt_now_ns() > give_up) waiting = 0;   /* lost: not counted */
        if (!waiting && !quit && injected < count && SDL_GetKeyboardFocus() == yscr_window(s)) {
            /* a random phase inside the frame, so the frame loop is no clock */
            rng = rng * 1664525u + 1013904223u;
            yrt_sleep_until(yrt_now_ns() + (rng >> 8) % 8000000u, 0);
            t_inj = (int64_t)yrt_now_ns();
            fake_key(dpy, XT_KEYCODE, 1, 0);
            fake_key(dpy, XT_KEYCODE, 0, 0);
            flush(dpy);
            give_up = t_inj + 500000000;
            waiting = 1;
            injected++;
        }
        yscr_flip(s);
    }
    close_display(dpy);
    printf("XTEST, keycode %u: %d injected, %d received\n", XT_KEYCODE, injected, nd);
    row("restamped event - injection, ms", delta, nd);
    row("SDL timestamp mod 1 ms, us", subms, nd);
    return quit;
}
#endif

int main(int argc, char** argv) {
    yscr_screen s;
    yscr_desc d;
    int i, count = 200, timer_res = 0, panic = 0, reports = 0, hand = 0, devices = 0, mice = 0;
    char line[512];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timer-res")) timer_res = 1;
        else if (!strcmp(argv[i], "--panic")) panic = 1;
        else if (!strcmp(argv[i], "--reports")) reports = 1;
        else if (!strcmp(argv[i], "--devices")) devices = 1;
        else if (!strcmp(argv[i], "--mice")) {
            mice = 30;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) mice = atoi(argv[++i]);
            if (mice > 600) { fprintf(stderr, "screen_input: --mice takes 1..600 s\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--hand")) {
            hand = 30;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) hand = atoi(argv[++i]);
            if (hand > 600) { fprintf(stderr, "screen_input: --hand takes 1..600 s\n"); return 2; }
        } else {
            count = atoi(argv[i]);
            if (count < 1 || count > 4000) {
                fprintf(stderr, "usage: screen_input [--timer-res] [--panic] [--reports] [--hand [s]] "
                                "[--devices] [--mice [s]] [injections 1..4000]\n");
                return 2;
            }
        }
    }
    if (timer_res) printf("timeBeginPeriod(1): %s\n", yrt_timer_resolution_begin() == 0 ? "granted" : "refused");
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    d.windowed = true;
    d.window_w = 400;
    d.window_h = 300;
    d.panic = panic != 0;
    d.raw_mice = mice > 0;
    if (devices) {
        yrt_ring_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.memory = g_ring_mem;
        rd.bytes = sizeof g_ring_mem;
        if (!yrt_ring_open(&g_ring, &rd)) { fprintf(stderr, "screen_input: %s\n", yrt_ring_error(&g_ring)); return 1; }
        d.ring = &g_ring;
    }
    if (!yscr_open(&s, &d)) { fprintf(stderr, "screen_input: %s\n", yscr_error(&s)); return 1; }
    yscr_describe(&s, line, sizeof line);
    printf("%s\n", line);
    if (devices) {
        devices_mode(&s);
        yscr_close(&s);
        return 0;
    }
    if (mice) {
        mice_mode(&s, mice);
        yscr_close(&s);
        return 0;
    }
#if defined(_WIN32)
    if (hand) {
        hand_mode(&s, hand);
        yscr_close(&s);
        return 0;
    }
#endif
    for (i = 0; i < 1000; i++) {
        yrt_corr_desc cd;
        yrt_corr c;
        uint64_t t0;
        memset(&cd, 0, sizeof cd);
        cd.read = SDL_GetTicksNS;
        t0 = yrt_now_ns();
        yrt_correlate(&cd, &c);
        g_b[i] = (double)(yrt_now_ns() - t0) / 1e3;
        g_a[i] = (double)c.width_ns / 1e3;
    }
    printf("correlation of SDL_GetTicksNS with the ysp_rt clock, 16 tries each:\n");
    row("width us", g_a, 1000);
    row("cost of one yrt_correlate us", g_b, 1000);
#if defined(_WIN32)
    {
        /* SendInput reaches only the foreground window. Windows gives the
         * foreground to a program started from the background after the
         * program sends one input event: a zero-size mouse move. */
        int64_t until = yrt_now_ns() + 2000000000LL;
        {
            HWND hw = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(yscr_window(&s)),
                                                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
            INPUT in;
            memset(&in, 0, sizeof in);
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof in);
            SetForegroundWindow(hw);
        }
        while (SDL_GetKeyboardFocus() != yscr_window(&s) && yrt_now_ns() < until) {
            yscr_frame f;
            if (yscr_begin(&s, &f) != YSCR_OK) break;
            yscr_flip(&s);
        }
        if (SDL_GetKeyboardFocus() == yscr_window(&s)) {
            if (reports) reports_mode(&s, count < 200 ? count : 50);
            else if (panic) run_path(&s, 1, count);
            else if (!run_path(&s, 0, count)) run_path(&s, 1, count);
        } else {
            printf("the window has no keyboard focus: raw input is not tested, posted messages are\n");
            run_path(&s, 2, count);
        }
    }
#elif defined(__linux__)
    (void)reports;   /* the report count is about Windows' two key paths */
    {
        int64_t until = (int64_t)yrt_now_ns() + 2000000000LL;
        while (SDL_GetKeyboardFocus() != yscr_window(&s) && (int64_t)yrt_now_ns() < until) {
            yscr_frame f;
            if (yscr_begin(&s, &f) != YSCR_OK) break;
            yscr_flip(&s);
        }
        if (SDL_GetKeyboardFocus() == yscr_window(&s)) run_xtest(&s, count);
        else printf("the window has no keyboard focus: no key is injected\n");
    }
#else
    (void)reports;   /* the report count injects keys: Windows only */
    printf("key injection is Windows and Linux X11 only here\n");
#endif
    yscr_close(&s);
    if (timer_res) yrt_timer_resolution_end();
    return 0;
}
