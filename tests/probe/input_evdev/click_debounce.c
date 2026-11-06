/* click_debounce: where fast mouse clicks get lost on Linux X11, measured.
 *
 * A uinput mouse clicks n times (press and release written together, or
 * the press held hold_ms) with gap_ms from each release to the next
 * press. Each click is counted at the kernel (an evdev reader), at
 * libinput (its own path context on the node, as Xorg's libinput driver
 * has), at the X server (XI2 raw button events, through SDL's X11 hook)
 * and in SDL's event stream. A fullscreen ysp/screen.h GLX_OML window
 * takes the clicks. docs/screen.md, "Fast clicks" has the results.
 *
 *   cc -O2 -std=c11 -D_GNU_SOURCE -Iinclude -I$SDL3/include \
 *      tests/probe/input_evdev/click_debounce.c -o click_debounce \
 *      -Wl,-rpath,$SDL3/lib -L$SDL3/lib -lSDL3 -ldl -lm
 *   ./click_debounce gap_ms [n] [hold_ms]
 *
 * Needs write access to /dev/uinput, read access to the new mouse's node
 * (tools/linux/70-ysp-input.rules) and libinput.so.10 (no headers: it is
 * loaded with dlopen). It moves no pointer: run it with nothing else on
 * the display.
 */
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

static int xi_raw_press, xi_raw_release;
static bool SDLCALL hook(void* ud, XEvent* xev) {
    const unsigned char* e = (const unsigned char*)xev;
    int type, evtype;
    (void)ud;
    memcpy(&type, e, sizeof type);
    if (type == 35) {
        memcpy(&evtype, e + 36, sizeof evtype);
        if (evtype == 15) xi_raw_press++;
        if (evtype == 16) xi_raw_release++;
    }
    return true;
}

static void uev(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = value;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("uinput");
}

/* libinput through dlopen (no headers installed) */
struct li_iface { int (*open_restricted)(const char*, int, void*); void (*close_restricted)(int, void*); };
static int li_open(const char* p, int flags, void* ud) { (void)ud; int fd = open(p, flags); return fd < 0 ? -errno : fd; }
static void li_close(int fd, void* ud) { (void)ud; close(fd); }
static void* (*li_create)(const struct li_iface*, void*);
static void* (*li_add)(void*, const char*);
static int (*li_dispatch)(void*);
static void* (*li_get)(void*);
static int (*li_type)(void*);
static void* (*li_ptr)(void*);
static int (*li_bstate)(void*);
static unsigned (*li_button)(void*);
static void (*li_destroy)(void*);
static int li_press, li_release;
static void li_drain(void* li) {
    void* ev;
    li_dispatch(li);
    while ((ev = li_get(li)) != NULL) {
        if (li_type(ev) == 402) {   /* LIBINPUT_EVENT_POINTER_BUTTON */
            void* p = li_ptr(ev);
            if (li_button(p) == BTN_LEFT) { if (li_bstate(p)) li_press++; else li_release++; }
        }
        li_destroy(ev);
    }
}
#define LOAD(v, name) do { void* q = dlsym(h, name); if (!q) { printf("no %s\n", name); return 1; } memcpy(&v, &q, sizeof q); } while (0)

int main(int argc, char** argv) {
    static yscr_screen scr;
    yscr_desc d;
    struct uinput_setup us;
    struct li_iface iface = { li_open, li_close };
    int gap = argc > 1 ? atoi(argv[1]) : 16, n = argc > 2 ? atoi(argv[2]) : 20, hold = argc > 3 ? atoi(argv[3]) : 0;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC), i, e, kfd = -1, k_press = 0, k_release = 0;
    int down = 0, up = 0;
    char sys[64] = {0}, path[160], node[64] = {0};
    void* h = dlopen("libinput.so.10", RTLD_NOW), *li;
    int64_t end;
    if (!h) { printf("no libinput\n"); return 1; }
    LOAD(li_create, "libinput_path_create_context"); LOAD(li_add, "libinput_path_add_device");
    LOAD(li_dispatch, "libinput_dispatch"); LOAD(li_get, "libinput_get_event"); LOAD(li_type, "libinput_event_get_type");
    LOAD(li_ptr, "libinput_event_get_pointer_event"); LOAD(li_bstate, "libinput_event_pointer_get_button_state");
    LOAD(li_button, "libinput_event_pointer_get_button"); LOAD(li_destroy, "libinput_event_destroy");
    if (fd < 0) { perror("uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT); ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT); ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_RELBIT, REL_X); ioctl(fd, UI_SET_RELBIT, REL_Y); ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB; us.id.vendor = 0x1209; us.id.product = 0x0002;
    snprintf(us.name, sizeof us.name, "ysp b8 mouse");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { perror("create"); return 1; }
    ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys);
    for (i = 0; i < 100 && !node[0]; i++) {
        for (e = 0; e < 256 && !node[0]; e++) {
            snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sys, e);
            if (access(path, F_OK) == 0) snprintf(node, sizeof node, "/dev/input/event%d", e);
        }
        if (!node[0]) yrt_sleep_ns(YRT_MS(10));
    }
    yrt_sleep_ns(YRT_MS(1500));
    kfd = open(node, O_RDONLY | O_NONBLOCK);
    li = li_create(&iface, NULL);
    if (kfd < 0 || !li || !li_add(li, node)) { printf("cannot read %s\n", node); return 1; }
    li_drain(li);
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_GLX_OML;
    if (!yscr_open(&scr, &d)) { printf("open: %s\n", yscr_error(&scr)); return 1; }
    SDL_SetX11EventHook(hook, NULL);
    end = (int64_t)yrt_now_ns() + 3000000000LL;
    while ((int64_t)yrt_now_ns() < end && SDL_GetKeyboardFocus() != yscr_window(&scr)) {
        yscr_frame f;
        SDL_Event ev;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        while (yscr_poll(&scr, &ev, NULL)) {}
        yscr_flip(&scr);
    }
    xi_raw_press = xi_raw_release = 0;
    for (i = 0; i < n; i++) {
        uev(fd, EV_KEY, BTN_LEFT, 1); uev(fd, EV_SYN, SYN_REPORT, 0);
        if (hold) yrt_sleep_ns(YRT_MS(hold));
        uev(fd, EV_KEY, BTN_LEFT, 0); uev(fd, EV_SYN, SYN_REPORT, 0);
        yrt_sleep_ns((uint64_t)gap * 1000000u);
        li_drain(li);
    }
    end = (int64_t)yrt_now_ns() + 500000000LL;
    while ((int64_t)yrt_now_ns() < end) {
        yscr_frame f;
        SDL_Event ev;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        while (yscr_poll(&scr, &ev, NULL)) {
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == 1) down++;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == 1) up++;
        }
        li_drain(li);
        yscr_flip(&scr);
    }
    {
        struct input_event b[64];
        ssize_t r;
        while ((r = read(kfd, b, sizeof b)) > 0)
            for (e = 0; e < (int)(r / (ssize_t)sizeof b[0]); e++)
                if (b[e].type == EV_KEY && b[e].code == BTN_LEFT) { if (b[e].value) k_press++; else k_release++; }
    }
    yscr_close(&scr);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    printf("hold %2d ms, release to press %2d ms, %d clicks: kernel %d/%d, libinput %d/%d, X raw %d/%d, SDL %d/%d (press/release)\n",
           hold, gap, n, k_press, k_release, li_press, li_release, xi_raw_press, xi_raw_release, down, up);
    return 0;
}
