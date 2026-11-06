/* screen_glx_test.c - ysp/screen.h's GLX_OML backend on a real X display.
 *
 * Self-checking, like screen_test.c, but it needs an X session with Mesa
 * (DRI3 and Present) and takes the screen for about 6 s, so ctest does not
 * run it. CMake builds it as test_screen_glx on Linux with SDL3; the
 * mutation list (tests/mutate/screen.toml, glx-*) runs it.
 *
 *   1. 120 frames fullscreen, each for the next vblank: every record has an
 *      observed onset (not ESTIMATED or OCCLUDED), on the planned vblank,
 *      path independent (Present flip), tier 1, and the onset is on the
 *      ysp_rt clock (within a second before the record completes).
 *   2. 120 frames held 0 to 3 vblanks past the prediction: each is shown
 *      on the vblank asked for, never early.
 *   3. The describe line reads _VARIABLE_REFRESH back as 0.
 *   4. 20 keys injected through XTEST (keycode 192, which Xfce's keymap
 *      gives XF86Launch5 and nothing binds) while the window has the focus:
 *      each comes back from yscr_poll() with the X server's time, 3 ms
 *      before to 1 ms after the injection (SDL's own stamp is the next
 *      frame's pump, 8 to 17 ms later); one of 20 may be late (the server
 *      handled the request late in 2 of 10 runs). Skipped without libXtst
 *      or focus.
 *   5. With vblank_mode=0 in the environment, open() refuses.
 *   6. In an 800 x 600 window (the Present copy path): depth 1, so 60
 *      frames each show on the next vblank, tier 2.
 *   7. desc.x11_override_redirect: the fullscreen window is
 *      override-redirect, its flips are Present flips, and it has the
 *      keyboard focus (20 of 20 XTEST keys; no window manager gives it).
 *   8. The panic (PANIC), in a child that runs this program again: the
 *      parent sets a ramp that is not identity on X screen 0's CRTC; the
 *      child opens fullscreen with desc.panic and, if the display lists
 *      one, a 59.94 Hz mode (which resets the ramp), and stops calling begin();
 *      the parent sends Shift+Esc three times through XTEST. The child
 *      must end with YSCR_PANIC_EXIT_CODE and leave the parent's ramp and
 *      the CRTC's mode as they were. The parent puts the identity back.
 *  10. desc.x_display ":0.1", in a child (it sets DISPLAY before SDL's
 *      video starts): SDL's window is on X screen 1 and its 120 flips are
 *      tier 1; a second screen asking for ":0.0" in that process is
 *      refused (SDL 3 drives one X screen per process). Skipped with one
 *      X screen.
 *   9. A clean close with a ramp that is not identity: fullscreen in the
 *      desktop mode says os_gamma=set and puts the ramp back at close; in
 *      a 59.94 Hz mode (the switch resets the ramp), close puts back the
 *      mode and then the ramp.
 *  11. Kernel key times: two uinput keyboards send keycode 192 (KEY_F14),
 *      one made before open() (found in /dev/input), one while the screen
 *      is open (found by inotify). Each key comes back from yscr_poll()
 *      with the kernel's time, inside the write() to uinput (the X
 *      server's time is 0.3 to 2 ms before it). Skipped when /dev/uinput
 *      cannot be opened or this process cannot read the new keyboard's
 *      node (no uaccess rule; docs/screen.md, "Kernel key times").
 *  12. desc.raw_mice on Linux: a uinput mouse (made before open, so X has
 *      added it) sends 20 clicks with a move while the window has the
 *      focus: each press comes from yscr_event_input() with the kernel's
 *      time (inside the write()), SDL's id of that mouse (not UNLISTED),
 *      the left button, and the move. With the focus moved to the root
 *      window, 5 more clicks give nothing. Skipped like part 11.
 *
 * Exit 0 on pass, 1 on a failed check; 0 with "SKIP" when there is no X
 * display or the backend does not open (no SDL X11 driver, no DRI3). With
 * YSCR_GLX_TEST_REQUIRE=1 (the mutation list sets it), a backend that does
 * not open is a failure: open() refuses on purpose when Present's events
 * or clock are not what it measured, and a fault there looks the same.
 */
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/uinput.h>

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "screen_glx_test: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

typedef struct seen {
    int n, observed, on_plan, early, independent, tier1, on_clock;
} seen;

static void on_flip(void* ctx, const yscr_record* r, const yscr_trigger_result* trig, int n_trig) {
    seen* s = (seen*)ctx;
    int64_t now = (int64_t)yrt_now_ns();
    (void)trig; (void)n_trig;
    if (r->index < 0) return;
    s->n++;
    if (!(r->flags & (YSCR_FLIP_ESTIMATED | YSCR_FLIP_OCCLUDED | YSCR_FLIP_SKIPPED))) s->observed++;
    if (r->dropped == 0 && !(r->flags & YSCR_FLIP_EARLY)) s->on_plan++;
    if (r->flags & YSCR_FLIP_EARLY) s->early++;
    if (r->path == YSCR_PATH_INDEPENDENT) s->independent++;
    if (r->tier == YSCR_TIER_1) s->tier1++;
    if (r->onset > now - 1000000000LL && r->onset <= now + 2000000LL) s->on_clock++;
}

static int open_glx(yscr_screen* s) {
    yscr_desc d;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_GLX_OML;
    return yscr_open(s, &d);
}

static void clear(yscr_screen* s) {
    typedef void (*clear_color_fn)(float, float, float, float);
    typedef void (*clear_fn)(unsigned int);
    clear_color_fn cc = (clear_color_fn)yscr_gl_proc(s, "glClearColor");
    clear_fn cl = (clear_fn)yscr_gl_proc(s, "glClear");
    if (cc && cl) { cc(0.2f, 0.2f, 0.2f, 1.0f); cl(0x4000u); }
}

/* Part 4. Returns the keys that came back within the window, or -1 when
 * XTEST or the focus is missing. */
static int xtest_keys(yscr_screen* s, int* received) {
    void* (*open_display)(const char*) = NULL;
    int (*close_display)(void*) = NULL;
    int (*flush)(void*) = NULL;
    int (*fake_key)(void*, unsigned int, int, unsigned long) = NULL;
    void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL), *xtst = dlopen("libXtst.so.6", RTLD_NOW), *p, *dpy;
    int injected = 0, ok = 0, waiting = 0;
    int64_t t_inj = 0, give_up = 0, end = (int64_t)yrt_now_ns() + 10000000000LL;
    *received = 0;
    if (!x11 || !xtst) return -1;
    p = dlsym(x11, "XOpenDisplay");  memcpy(&open_display, &p, sizeof p);
    p = dlsym(x11, "XCloseDisplay"); memcpy(&close_display, &p, sizeof p);
    p = dlsym(x11, "XFlush");        memcpy(&flush, &p, sizeof p);
    p = dlsym(xtst, "XTestFakeKeyEvent"); memcpy(&fake_key, &p, sizeof p);
    if (!open_display || !close_display || !flush || !fake_key || !(dpy = open_display(NULL))) return -1;
    while ((injected < 20 || waiting) && (int64_t)yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev;
        if (yscr_begin(s, &f) != YSCR_OK) break;
        while (yscr_poll(s, &ev, &t_ev))
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.raw == 192 && waiting) {
                (*received)++;
                if (t_ev >= t_inj - 3000000 && t_ev <= t_inj + 1000000) ok++;
                else printf("  key %d: stamp %+.3f ms from the injection, outside -3 to +1\n", *received,
                            (double)(t_ev - t_inj) / 1e6);
                waiting = 0;
            }
        if (waiting && (int64_t)yrt_now_ns() > give_up) waiting = 0;
        if (!waiting && injected < 20 && SDL_GetKeyboardFocus() == yscr_window(s)) {
            yrt_sleep_until(yrt_now_ns() + 2000000u * (uint64_t)(injected % 4), 0);
            t_inj = (int64_t)yrt_now_ns();
            fake_key(dpy, 192, 1, 0);
            fake_key(dpy, 192, 0, 0);
            flush(dpy);
            give_up = t_inj + 500000000;
            waiting = 1;
            injected++;
        }
        clear(s);
        yscr_flip(s);
    }
    close_display(dpy);
    return injected == 0 ? -1 : ok;
}


/* Part 11: a keyboard made through uinput, or -1. *readable says whether
 * this process can read its event node (udev's ACL; waits up to 1 s). */
static int uinput_kbd(int* readable) {
    struct uinput_setup us;
    int k, fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    /* udev calls it a keyboard, which the uaccess rule needs, only with
     * all of KEY_ESC to KEY_S */
    for (k = KEY_ESC; k < 32; k++) ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_KEYBIT, KEY_F14);
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1209;
    us.id.product = 0x0001;
    snprintf(us.name, sizeof us.name, "ysp screen_glx_test");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    *readable = 0;
    {
        char sys[64] = {0}, path[160];
        int t, e;
        ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys);
        for (t = 0; t < 100 && !*readable; t++) {
            for (e = 0; e < 256 && !*readable; e++) {
                snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sys, e);
                if (access(path, F_OK) == 0) {
                    snprintf(path, sizeof path, "/dev/input/event%d", e);
                    *readable = access(path, R_OK) == 0;
                }
            }
            if (!*readable) yrt_sleep_ns(YRT_MS(10));
        }
    }
    return fd;
}

static void uinput_key(int fd, int down) {
    struct input_event e[2];
    memset(e, 0, sizeof e);
    e[0].type = EV_KEY;
    e[0].code = KEY_F14;
    e[0].value = down;
    e[1].type = EV_SYN;
    e[1].code = SYN_REPORT;
    if (write(fd, e, sizeof e) != (ssize_t)sizeof e) perror("uinput");
}

/* Sends n keys after the first that comes back (Xorg adds a new keyboard
 * about a second after it appears). Returns the keys whose press came back
 * with a time inside the write(), or -1 when none came back. */
static int uinput_keys(yscr_screen* s, int fd, int n, int* received) {
    int injected = 0, ok = 0, waiting = 0, warm = 0;
    int64_t w0 = 0, w1 = 0, give_up = 0, end = (int64_t)yrt_now_ns() + 10000000000LL;
    *received = 0;
    while ((injected < n || waiting) && (int64_t)yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev;
        if (yscr_begin(s, &f) != YSCR_OK) break;
        while (yscr_poll(s, &ev, &t_ev))
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.raw == 192 && waiting) {
                waiting = 0;
                if (!warm) { warm = 1; continue; }
                (*received)++;
                /* the round trip through SDL's ticks rounds to 1 us */
                if (t_ev >= w0 - 2000 && t_ev <= w1 + 2000) ok++;
                else printf("  key %d: stamp %+.3f ms from the write\n", *received, (double)(t_ev - w0) / 1e6);
            }
        if (waiting && (int64_t)yrt_now_ns() > give_up) waiting = 0;
        if (!waiting && injected < n && SDL_GetKeyboardFocus() == yscr_window(s)) {
            yrt_sleep_until(yrt_now_ns() + 2000000u * (uint64_t)(injected % 4), 0);
            w0 = (int64_t)yrt_now_ns();
            uinput_key(fd, 1);
            w1 = (int64_t)yrt_now_ns();
            uinput_key(fd, 0);
            give_up = w0 + (warm ? 500000000 : 200000000);
            waiting = 1;
            if (warm) injected++;
        }
        clear(s);
        yscr_flip(s);
    }
    return *received == 0 ? -1 : ok;
}

static void part11(void) {
    static yscr_screen scr;
    char line[1024];
    int readable = 0, a = uinput_kbd(&readable), b, got_a = 0, got_b = 0, ok_a, ok_b;
    if (a < 0) { printf("kernel key times: skipped (/dev/uinput: %s)\n", strerror(errno)); return; }
    if (!readable) {
        printf("kernel key times: skipped (the keyboard's node is not readable; the uaccess rule)\n");
        close(a);
        return;
    }
    if (!open_glx(&scr)) { CHECK(0); close(a); return; }
    ok_a = uinput_keys(&scr, a, 10, &got_a);
    b = uinput_kbd(&readable);
    CHECK(b >= 0 && readable);
    ok_b = b < 0 ? 0 : uinput_keys(&scr, b, 10, &got_b);
    yscr_describe(&scr, line, sizeof line);
    yscr_close(&scr);
    ioctl(a, UI_DEV_DESTROY);
    close(a);
    if (b >= 0) { ioctl(b, UI_DEV_DESTROY); close(b); }
    printf("kernel key times: before open %d of %d, plugged in while open %d of %d; %s\n", ok_a, got_a, ok_b, got_b,
           strstr(line, "key_time=") ? strstr(line, "key_time=") : "?");
    CHECK(got_a == 10 && ok_a == 10);
    CHECK(got_b == 10 && ok_b == 10);
}

/* Part 12: a mouse made through uinput, or -1; *readable as uinput_kbd. */
static int uinput_mouse(int* readable) {
    struct uinput_setup us;
    char sys[64] = {0}, path[160];
    int t, e, fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1209;
    us.id.product = 0x0002;
    snprintf(us.name, sizeof us.name, "ysp screen_glx_test mouse");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    *readable = 0;
    ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys);
    for (t = 0; t < 100 && !*readable; t++) {
        for (e = 0; e < 256 && !*readable; e++) {
            snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sys, e);
            if (access(path, F_OK) == 0) {
                snprintf(path, sizeof path, "/dev/input/event%d", e);
                *readable = access(path, R_OK) == 0;
            }
        }
        if (!*readable) yrt_sleep_ns(YRT_MS(10));
    }
    return fd;
}

static void uinput_ev(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("uinput");
}

/* The X input focus to the window (or PointerRoot with win 0). */
static void x_focus(unsigned long win) {
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL), *p, *dpy;
    void* (*open_display)(const char*) = NULL;
    int (*close_display)(void*) = NULL;
    int (*set_focus)(void*, unsigned long, int, unsigned long) = NULL;
    int (*sync)(void*, int) = NULL;
    if (!x11) return;
    p = dlsym(x11, "XOpenDisplay");   memcpy(&open_display, &p, sizeof p);
    p = dlsym(x11, "XCloseDisplay");  memcpy(&close_display, &p, sizeof p);
    p = dlsym(x11, "XSetInputFocus"); memcpy(&set_focus, &p, sizeof p);
    p = dlsym(x11, "XSync");          memcpy(&sync, &p, sizeof p);
    if (!open_display || !close_display || !set_focus || !sync || !(dpy = open_display(NULL))) return;
    set_focus(dpy, win ? win : 1ul /* PointerRoot */, 2 /* RevertToParent */, 0);
    sync(dpy, 0);
    close_display(dpy);
}

static void part12(void) {
    static yscr_screen scr;
    yscr_desc d;
    SDL_MouseID want = 0;
    int readable = 0, m = uinput_mouse(&readable), i, injected = 0, got = 0, ok_t = 0, ok_id = 0, ok_move = 0;
    int unlisted = 0, phase = 0, stray = 0, n_ids = 0;
    int64_t w0 = 0, w1 = 0, end;
    SDL_MouseID* ids;
    {   /* refused with no window: SDL's mouse ids and the focus gate need one */
        static yscr_screen sim;
        memset(&d, 0, sizeof d);
        d.backend = YSCR_BACKEND_SIM;
        d.raw_mice = true;
        CHECK(!yscr_open(&sim, &d));
        CHECK(strstr(yscr_error(&sim), "raw_mice") != NULL);
    }
    if (m < 0) { printf("raw mice: skipped (/dev/uinput: %s)\n", strerror(errno)); return; }
    if (!readable) {
        printf("raw mice: skipped (the mouse's node is not readable; the uaccess rule's mouse line)\n");
        close(m);
        return;
    }
    yrt_sleep_ns(YRT_MS(1500));   /* Xorg adds the device (and SDL lists it) */
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_GLX_OML;
    d.raw_mice = true;
    if (!yscr_open(&scr, &d)) {
        printf("raw mice: open: %s\n", yscr_error(&scr));
        CHECK(0);
        ioctl(m, UI_DEV_DESTROY);
        close(m);
        return;
    }
    ids = SDL_GetMice(&n_ids);
    for (i = 0; ids && i < n_ids; i++)
        if (SDL_GetMouseNameForID(ids[i]) && !strcmp(SDL_GetMouseNameForID(ids[i]), "ysp screen_glx_test mouse")) want = ids[i];
    SDL_free(ids);
    end = (int64_t)yrt_now_ns() + 15000000000LL;
    while ((int64_t)yrt_now_ns() < end && (phase < 3 || phase == 15)) {
        yscr_frame f;
        SDL_Event ev;
        int64_t t_ev;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        while (yscr_poll(&scr, &ev, &t_ev)) {
            yin_event in;
            if (!yscr_event_input(&scr, &ev, &in) || in.kind != YIN_KIND_MOUSE) continue;
            if (in.device != (uint32_t)want && !(in.flags & YIN_UNLISTED)) continue;   /* another mouse */
            if (phase == 2) {
                stray++;
                if (getenv("GLX_TEST_DEBUG"))
                    printf("  stray: sdl type 0x%x (doorbell 0x%x) device %u flags 0x%x type %d control %u\n", (unsigned)ev.type,
                           (unsigned)yscr_input_event_type(), (unsigned)in.device, (unsigned)in.flags, (int)in.type, (unsigned)in.control);
                continue;
            }
            if (in.type == YIN_PRESS && in.control == 1) {
                got++;
                if (in.t >= w0 - 2000 && in.t <= w1 + 2000) ok_t++;
                else printf("  click %d: stamp %+.3f ms from the write\n", got, (double)(in.t - w0) / 1e6);
                if (in.device == (uint32_t)want && want) ok_id++;
                if (in.flags & YIN_UNLISTED) unlisted++;
            } else if (in.type == YIN_SAMPLE && in.control == YIN_AXIS_DELTA && in.x == 3.0f && in.y == -2.0f) {
                ok_move++;
            }
        }
        if (phase == 0 && SDL_GetKeyboardFocus() == yscr_window(&scr)) {
            if (injected < 20) {
                yrt_sleep_until(yrt_now_ns() + 2000000u * (uint64_t)(injected % 4), 0);
                w0 = (int64_t)yrt_now_ns();
                uinput_ev(m, EV_REL, REL_X, 3);
                uinput_ev(m, EV_REL, REL_Y, -2);
                uinput_ev(m, EV_KEY, BTN_LEFT, 1);
                uinput_ev(m, EV_SYN, SYN_REPORT, 0);
                w1 = (int64_t)yrt_now_ns();
                uinput_ev(m, EV_KEY, BTN_LEFT, 0);
                uinput_ev(m, EV_SYN, SYN_REPORT, 0);
                injected++;
            } else if (got >= 20 || (int64_t)yrt_now_ns() > end - 8000000000LL) {
                phase = 1;
                x_focus(0);
            }
        } else if (phase == 1 && SDL_GetKeyboardFocus() == NULL) {
            phase = 15;   /* the next frame's begin() tells the reader */
        } else if (phase == 15) {
            int k;
            for (k = 0; k < 5; k++) {
                uinput_ev(m, EV_KEY, BTN_LEFT, 1);
                uinput_ev(m, EV_SYN, SYN_REPORT, 0);
                uinput_ev(m, EV_KEY, BTN_LEFT, 0);
                uinput_ev(m, EV_SYN, SYN_REPORT, 0);
                yrt_sleep_ns(YRT_MS(10));
            }
            phase = 2;
            end = (int64_t)yrt_now_ns() + 300000000LL;   /* time for strays to arrive */
        }
        clear(&scr);
        yscr_flip(&scr);
    }
    {
        unsigned long win = (unsigned long)SDL_GetNumberProperty(SDL_GetWindowProperties(yscr_window(&scr)),
                                                                SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (win) x_focus(win);
    }
    yscr_close(&scr);
    ioctl(m, UI_DEV_DESTROY);
    close(m);
    printf("raw mice: %d clicks injected, %d presses, %d with the kernel's time, %d with SDL's id %u, %d moves, "
           "%d unlisted; %d strays without the focus\n", injected, got, ok_t, ok_id, (unsigned)want, ok_move, unlisted, stray);
    CHECK(want != 0);
    CHECK(injected == 20 && got == 20);
    CHECK(ok_t == 20);
    CHECK(ok_id == 20);
    CHECK(ok_move == 20);
    CHECK(stray == 0);
}

/* Part 10's child: exit 0 when every check holds. */
static int screen1_child(void) {
    static yscr_screen a, b;
    yscr_desc d;
    seen run;
    yscr_frame f;
    int i, scr_no, fail = 0;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_GLX_OML;
    d.x_display = ":0.1";
    if (!yscr_open(&a, &d)) { fprintf(stderr, "screen 1 child: %s\n", yscr_error(&a)); return 2; }
    scr_no = (int)SDL_GetNumberProperty(SDL_GetWindowProperties(yscr_window(&a)), SDL_PROP_WINDOW_X11_SCREEN_NUMBER, -1);
    memset(&run, 0, sizeof run);
    yscr_on_flip(&a, on_flip, &run);
    for (i = 0; i < 120; i++) { if (yscr_begin(&a, &f) != YSCR_OK) break; clear(&a); yscr_flip(&a); }
    yscr_wait_flip(&a, NULL);
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_GLX_OML;
    d.windowed = true;
    d.x_display = ":0.0";
    fail |= yscr_open(&b, &d) ? 4 : 0;
    fail |= strstr(yscr_error(&b), "one X screen per process") ? 0 : 8;
    if (yscr_is_open(&b)) yscr_close(&b);
    yscr_close(&a);
    printf("screen 1 child: X screen %d, %d records, tier 1 %d; second display %s\n", scr_no, run.n, run.tier1,
           (fail & 12) ? "NOT refused" : "refused");
    fflush(stdout);
    if (scr_no != 1) fail |= 16;
    if (run.n != 120 || run.tier1 != 120) fail |= 32;
    return fail;
}

static void part10(const char* self) {
    void* xc = yscr__x_load() ? yscr__x.connect(":0", NULL) : NULL;
    int screens = 0, status = 0;
    pid_t pid;
    if (xc && yscr__xx_load()) {
        yscr__xcb_screen_it it = yscr__xx.roots(yscr__xx.get_setup(xc));
        for (; it.rem > 0; yscr__xx.screen_next(&it)) screens++;
    }
    if (xc) yscr__x.disconnect(xc);
    if (screens < 2) { printf("x_display: skipped (%d X screen)\n", screens); return; }
    pid = fork();
    if (pid == 0) { execl(self, self, "--screen1-child", (char*)NULL); _exit(5); }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Part 8's child: open, report, hang. */
static int panic_child(int fd) {
    static yscr_screen sc;
    yscr_desc d;
    yscr_frame f;
    yscr_mode m[64];
    char line[1024], ok;
    int i, n;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_GLX_OML;
    d.panic = true;
    n = yscr_modes(0, m, 64);
    for (i = 0; i < n; i++)
        if (m[i].w == 1920 && m[i].h == 1080 && m[i].period_ns > 16683000 && m[i].period_ns < 16684500) { d.mode = m[i]; break; }
    if (!yscr_open(&sc, &d)) { fprintf(stderr, "panic child: %s\n", yscr_error(&sc)); return 3; }
    for (i = 0; i < 30; i++) { if (yscr_begin(&sc, &f) != YSCR_OK) break; clear(&sc); yscr_flip(&sc); }
    yscr_describe(&sc, line, sizeof line);
    /* the mode switch put the ramp back to identity, so os_gamma is
     * identity here; the mode snapshot holds the parent's ramp */
    ok = (char)(strstr(line, "panic=armed") ? 'y' : 'n');
    if (write(fd, &ok, 1) != 1) return 4;
    printf("panic child: %s\n", strstr(line, "os_gamma="));
    fflush(stdout);
    for (;;) yrt_sleep_ns(YRT_S(1));   /* hung: no more begin() */
}

/* The ramp and mode of X screen 0's first active CRTC. */
static int crtc0(void* xc, uint32_t* crtc, uint32_t* mode, uint16_t* n) {
    uint32_t root = yscr__xx_root(xc, 0);
    uint8_t* res = root ? yscr__xx.res_current_reply(xc, yscr__xx.res_current(xc, root), NULL) : NULL;
    int k, nc, found = 0;
    if (!res) return 0;
    nc = yscr__xx.res_current_crtcs_length(res);
    for (k = 0; k < nc && !found; k++) {
        uint32_t c = yscr__xx.res_current_crtcs(res)[k], md = 0;
        uint8_t* ci = yscr__xx.crtc_info_reply(xc, yscr__xx.crtc_info(xc, c, 0), NULL);
        if (ci) { memcpy(&md, ci + 20, 4); free(ci); }
        if (md) {
            uint8_t* g = yscr__xx.crtc_gamma_reply(xc, yscr__xx.crtc_gamma(xc, c), NULL);
            if (g) { memcpy(n, g + 8, 2); free(g); }
            *crtc = c; *mode = md; found = 1;
        }
    }
    free(res);
    return found;
}

/* The CRTC as it was, to put back after a part whatever happened in it, so
 * a failed run (a mutant's) does not leave the next run in another mode. */
typedef struct crtc_cfg { uint32_t crtc, mode; int16_t x, y; uint16_t rot, n_out; uint32_t out[4]; } crtc_cfg;

static void cfg_take(void* xc, uint32_t crtc, crtc_cfg* c) {
    uint8_t* ci = yscr__xx.crtc_info_reply(xc, yscr__xx.crtc_info(xc, crtc, 0), NULL);
    int j, no;
    memset(c, 0, sizeof *c);
    c->crtc = crtc;
    if (!ci) return;
    memcpy(&c->x, ci + 12, 2); memcpy(&c->y, ci + 14, 2); memcpy(&c->mode, ci + 20, 4); memcpy(&c->rot, ci + 24, 2);
    no = yscr__xx.crtc_info_outputs_length(ci);
    if (no > 4) no = 4;
    for (j = 0; j < no; j++) c->out[j] = yscr__xx.crtc_info_outputs(ci)[j];
    c->n_out = (uint16_t)no;
    free(ci);
}

static void cfg_put_back(void* xc, const crtc_cfg* c, const uint16_t* id, uint16_t n) {
    uint32_t root = yscr__xx_root(xc, 0), cts = 0, mode = 0;
    uint8_t* res = root ? yscr__xx.res_current_reply(xc, yscr__xx.res_current(xc, root), NULL) : NULL;
    uint8_t* ci;
    if (res) { memcpy(&cts, res + 12, 4); free(res); }
    ci = yscr__xx.crtc_info_reply(xc, yscr__xx.crtc_info(xc, c->crtc, cts), NULL);
    if (ci) { memcpy(&mode, ci + 20, 4); free(ci); }
    if (mode != c->mode && c->mode) {
        free(yscr__xx.set_crtc_config_reply(xc, yscr__xx.set_crtc_config(xc, c->crtc, 0, cts, c->x, c->y, c->mode, c->rot,
                                                                          c->n_out, c->out), NULL));
        yrt_sleep_ns(YRT_MS(150));
    }
    yscr__xg_set(xc, c->crtc, n, id, id, id);
}

static int ramp_is(void* xc, uint32_t crtc, const uint16_t* want, uint16_t n) {
    uint8_t* g = yscr__xx.crtc_gamma_reply(xc, yscr__xx.crtc_gamma(xc, crtc), NULL);
    int same = 0;
    if (g) {
        same = !memcmp(yscr__xx.crtc_gamma_red(g), want, n * 2u) && !memcmp(yscr__xx.crtc_gamma_green(g), want, n * 2u) &&
               !memcmp(yscr__xx.crtc_gamma_blue(g), want, n * 2u);
        free(g);
    }
    return same;
}

static void part8_run(const char* self, void* xc, void* dpy, void* (*open_display)(const char*), int (*flush)(void*),
                      int (*fake_key)(void*, unsigned int, int, unsigned long), unsigned char (*kc_of)(void*, unsigned long),
                      uint32_t crtc, uint32_t mode, uint16_t n, const uint16_t* dim);

static void part8(const char* self) {
    static uint16_t dim[4096], id[4096];
    void *xc, *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL), *xtst = dlopen("libXtst.so.6", RTLD_NOW), *p, *dpy;
    void* (*open_display)(const char*) = NULL;
    int (*flush)(void*) = NULL, (*fake_key)(void*, unsigned int, int, unsigned long) = NULL;
    unsigned char (*kc_of)(void*, unsigned long) = NULL;
    uint32_t crtc = 0, mode = 0;
    uint16_t n = 0;
    int i;
    if (!yscr__xx_load() || !x11 || !xtst) { printf("panic: skipped (%s)\n", yscr__xx.why); return; }
    p = dlsym(x11, "XOpenDisplay"); memcpy(&open_display, &p, sizeof p);
    p = dlsym(x11, "XFlush"); memcpy(&flush, &p, sizeof p);
    p = dlsym(x11, "XKeysymToKeycode"); memcpy(&kc_of, &p, sizeof p);
    p = dlsym(xtst, "XTestFakeKeyEvent"); memcpy(&fake_key, &p, sizeof p);
    xc = yscr__x.connect(NULL, NULL);
    dpy = open_display ? open_display(NULL) : NULL;
    if (!xc || !dpy || !fake_key || !kc_of || !crtc0(xc, &crtc, &mode, &n) || n < 2 || n > 4096) {
        printf("panic: skipped (no X connection, XTEST or CRTC)\n");
        return;
    }
    for (i = 0; i < n; i++) { id[i] = (uint16_t)((uint64_t)i * 65535u / (n - 1u)); dim[i] = (uint16_t)(id[i] * 9u / 10u); }
    {
        crtc_cfg cfg;
        cfg_take(xc, crtc, &cfg);
        part8_run(self, xc, dpy, open_display, flush, fake_key, kc_of, crtc, mode, n, dim);
        cfg_put_back(xc, &cfg, id, n);
    }
    yscr__x.disconnect(xc);
}

static void part8_run(const char* self, void* xc, void* dpy, void* (*open_display)(const char*), int (*flush)(void*),
                      int (*fake_key)(void*, unsigned int, int, unsigned long), unsigned char (*kc_of)(void*, unsigned long),
                      uint32_t crtc, uint32_t mode, uint16_t n, const uint16_t* dim) {
    uint32_t mode_after = 0;
    uint16_t n2 = 0;
    int fds[2], status = 0, i, code = -1;
    pid_t pid;
    char ok = 0;
    int64_t t3 = 0, t_end = 0;
    (void)open_display;
    yscr__xg_set(xc, crtc, n, dim, dim, dim);
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        char fdarg[16];
        snprintf(fdarg, sizeof fdarg, "%d", fds[1]);
        execl(self, self, "--panic-child", fdarg, (char*)NULL);
        _exit(5);
    }
    close(fds[1]);
    CHECK(read(fds[0], &ok, 1) == 1 && ok == 'y');
    if (ok == 'y') {
        unsigned int shift = kc_of(dpy, 0xffe1u), esc = kc_of(dpy, 0xff1bu);
        fake_key(dpy, shift, 1, 0);
        for (i = 0; i < 3; i++) {
            fake_key(dpy, esc, 1, 0);
            fake_key(dpy, esc, 0, 0);
            flush(dpy);
            t3 = (int64_t)yrt_now_ns();
            yrt_sleep_ns(YRT_MS(150));
        }
        fake_key(dpy, shift, 0, 0);
        flush(dpy);
    }
    for (i = 0; i < 500 && waitpid(pid, &status, WNOHANG) == 0; i++) yrt_sleep_ns(YRT_MS(10));
    t_end = (int64_t)yrt_now_ns();
    if (i == 500) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    yrt_sleep_ns(YRT_MS(500));   /* a mode set settles */
    crtc0(xc, &crtc, &mode_after, &n2);
    printf("panic: child exit %d, %.0f ms after the third press; ramp %s; mode %s\n", code, (double)(t_end - t3) / 1e6,
           ramp_is(xc, crtc, dim, n) ? "the parent's" : "NOT the parent's", mode_after == mode ? "as before" : "CHANGED");
    CHECK(code == YSCR_PANIC_EXIT_CODE);
    CHECK(ramp_is(xc, crtc, dim, n));
    CHECK(mode_after == mode);
}

static void part9(void) {
    static uint16_t dim[4096], id[4096];
    static yscr_screen sc;
    crtc_cfg cfg;
    void* xc;
    uint32_t crtc = 0, mode = 0, mode_after = 0;
    uint16_t n = 0, n2 = 0;
    int i, pass;
    char line[1024];
    if (!yscr__xx_load() || !(xc = yscr__x.connect(NULL, NULL)) || !crtc0(xc, &crtc, &mode, &n) || n < 2 || n > 4096) {
        printf("close: skipped\n");
        return;
    }
    for (i = 0; i < n; i++) { id[i] = (uint16_t)((uint64_t)i * 65535u / (n - 1u)); dim[i] = (uint16_t)(id[i] * 9u / 10u); }
    cfg_take(xc, crtc, &cfg);
    for (pass = 0; pass < 2; pass++) {
        yscr_desc d;
        yscr_frame f;
        yscr_mode m[64];
        int k, nm = yscr_modes(0, m, 64), switched = 0;
        memset(&d, 0, sizeof d);
        d.backend = YSCR_BACKEND_GLX_OML;
        if (pass == 1)
            for (k = 0; k < nm; k++)
                if (m[k].w == 1920 && m[k].h == 1080 && m[k].period_ns > 16683000 && m[k].period_ns < 16684500) {
                    d.mode = m[k];
                    switched = 1;
                    break;
                }
        if (pass == 1 && !switched) { printf("close: no 59.94 Hz mode, pass 2 skipped\n"); break; }
        yscr__xg_set(xc, crtc, n, dim, dim, dim);
        memset(&sc, 0, sizeof sc);
        CHECK(yscr_open(&sc, &d));
        if (!yscr_is_open(&sc)) continue;
        for (k = 0; k < 30; k++) { if (yscr_begin(&sc, &f) != YSCR_OK) break; clear(&sc); yscr_flip(&sc); }
        yscr_describe(&sc, line, sizeof line);
        yscr_close(&sc);
        yrt_sleep_ns(YRT_MS(300));
        crtc0(xc, &crtc, &mode_after, &n2);
        printf("close %s: %s; after close ramp %s, mode %s\n", pass ? "after a mode switch" : "in the desktop mode",
               strstr(line, "os_gamma=") ? strstr(line, "os_gamma=") : "?", ramp_is(xc, crtc, dim, n) ? "the user's" : "NOT the user's",
               mode_after == mode ? "as before" : "CHANGED");
        if (pass == 0) CHECK(strstr(line, "os_gamma=set") != NULL);
        CHECK(ramp_is(xc, crtc, dim, n));
        CHECK(mode_after == mode);
    }
    cfg_put_back(xc, &cfg, id, n);
    yscr__x.disconnect(xc);
}

int main(int argc, char** argv) {
    static yscr_screen scr;
    yscr_frame f;
    seen run1, run2;
    char line[1024];
    int i;
    const char* dsp = getenv("DISPLAY");
    if (argc == 3 && !strcmp(argv[1], "--panic-child")) return panic_child(atoi(argv[2]));
    if (argc == 2 && !strcmp(argv[1], "--screen1-child")) return screen1_child();
    if (!dsp || !dsp[0]) { printf("screen_glx_test: SKIP (no DISPLAY)\n"); return 0; }
    if (!open_glx(&scr)) {
        const char* req = getenv("YSCR_GLX_TEST_REQUIRE");
        if (req && req[0] == '1') {
            fprintf(stderr, "screen_glx_test: FAIL line %d: open: %s\n", __LINE__, yscr_error(&scr));
            return 1;
        }
        printf("screen_glx_test: SKIP (%s)\n", yscr_error(&scr));
        return 0;
    }

    memset(&run1, 0, sizeof run1);
    yscr_on_flip(&scr, on_flip, &run1);
    for (i = 0; i < 120; i++) {
        CHECK(yscr_begin(&scr, &f) == YSCR_OK);
        clear(&scr);
        CHECK(yscr_flip(&scr) == YSCR_OK);
    }
    CHECK(yscr_wait_flip(&scr, NULL) == YSCR_OK);

    memset(&run2, 0, sizeof run2);
    yscr_on_flip(&scr, on_flip, &run2);
    for (i = 0; i < 120; i++) {
        CHECK(yscr_begin(&scr, &f) == YSCR_OK);
        clear(&scr);
        CHECK(yscr_flip_at(&scr, f.onset + (int64_t)(i % 4) * f.period, NULL) == YSCR_OK);
    }
    CHECK(yscr_wait_flip(&scr, NULL) == YSCR_OK);
    yscr_on_flip(&scr, NULL, NULL);
    {
        int got = 0, ok = xtest_keys(&scr, &got);
        if (ok < 0) printf("keys: skipped (no libXtst, or the window never had the focus)\n");
        else {
            printf("keys: 20 injected, %d received, %d with the X server's time\n", got, ok);
            CHECK(got == 20);
            /* one key of 20 was late in 2 of 10 runs (7.3 ms, the one
             * printed): the server handled the XTEST request late */
            CHECK(ok >= 19);
        }
    }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    yscr_close(&scr);

    printf("run 1: %d records, observed %d, on plan %d, independent %d, tier 1 %d, on the clock %d\n",
           run1.n, run1.observed, run1.on_plan, run1.independent, run1.tier1, run1.on_clock);
    printf("run 2 (held 0 to 3): %d records, observed %d, on plan %d, early %d\n",
           run2.n, run2.observed, run2.on_plan, run2.early);
    CHECK(run1.n == 120);
    CHECK(run1.observed == run1.n);
    CHECK(run1.on_plan == run1.n);
    CHECK(run1.independent == run1.n);
    CHECK(run1.tier1 == run1.n);
    CHECK(run1.on_clock == run1.n);
    CHECK(run2.n == 120);
    CHECK(run2.observed == run2.n);
    CHECK(run2.on_plan == run2.n);
    CHECK(run2.early == 0);
    CHECK(strstr(line, " vrr_prop=0") != NULL);

    {   /* part 6: a window */
        yscr_desc d;
        seen run3;
        memset(&d, 0, sizeof d);
        d.backend = YSCR_BACKEND_GLX_OML;
        d.windowed = true;
        memset(&scr, 0, sizeof scr);
        CHECK(yscr_open(&scr, &d));
        if (yscr_is_open(&scr)) {
            memset(&run3, 0, sizeof run3);
            yscr_on_flip(&scr, on_flip, &run3);
            for (i = 0; i < 60; i++) {
                CHECK(yscr_begin(&scr, &f) == YSCR_OK);
                clear(&scr);
                CHECK(yscr_flip(&scr) == YSCR_OK);
            }
            CHECK(yscr_wait_flip(&scr, NULL) == YSCR_OK);
            yscr_describe(&scr, line, sizeof line);
            yscr_close(&scr);
            printf("run 3 (window): %d records, on plan %d, tier 1 %d; %s\n", run3.n, run3.on_plan, run3.tier1,
                   strstr(line, " depth=1") ? "depth 1" : "depth not 1");
            CHECK(run3.n == 60);
            CHECK(run3.on_plan == 60);
            CHECK(strstr(line, " depth=1") != NULL);
            CHECK(strstr(line, " path=composed") != NULL);
        }
    }

    {   /* part 7: override-redirect fullscreen */
        yscr_desc d;
        memset(&d, 0, sizeof d);
        d.backend = YSCR_BACKEND_GLX_OML;
        d.x11_override_redirect = true;
        memset(&scr, 0, sizeof scr);
        CHECK(yscr_open(&scr, &d));
        if (yscr_is_open(&scr)) {
            int got = 0, ok = xtest_keys(&scr, &got);
            yscr_describe(&scr, line, sizeof line);
            yscr_close(&scr);
            printf("override-redirect: %s, keys %d received, %d with the X server's time\n",
                   strstr(line, " window=override-redirect") ? "yes" : "no", got, ok < 0 ? 0 : ok);
            CHECK(strstr(line, " window=override-redirect") != NULL);
            CHECK(strstr(line, " path=independent") != NULL);
            CHECK(ok >= 19);   /* as part 4 */
        }
    }

    part8(argv[0]);
    part9();
    part10(argv[0]);
    part11();
    part12();

    /* Mesa reads vblank_mode when a display's driver screen is made, and
     * open() makes its own display connection. */
    setenv("vblank_mode", "0", 1);
    memset(&scr, 0, sizeof scr);
    CHECK(!open_glx(&scr));
    CHECK(strstr(yscr_error(&scr), "swap interval") != NULL);
    if (yscr_is_open(&scr)) yscr_close(&scr);
    unsetenv("vblank_mode");

    if (g_failures) return 1;
    printf("screen_glx_test: all checks passed\n");
    return 0;
}
