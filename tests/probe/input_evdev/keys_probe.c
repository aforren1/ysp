/* keys_probe: where a Linux key's time comes from, measured.
 *
 * Creates a uinput keyboard and sends N taps of one key at random gaps.
 * For each press it records:
 *   w0, w1   CLOCK_MONOTONIC before and after the write() to uinput
 *   k        the kernel's evdev timestamp (EVIOCSCLOCKID CLOCK_MONOTONIC)
 *   r        when a reader thread's read() returned the event
 *   x        the X server's time of the XI2 raw key event (ms)
 *   xr       when Xlib returned that event
 * and prints min / mean / p99 / max of the differences.
 *
 *   cc -O2 -o keys_probe keys_probe.c -lX11 -lXi -lpthread
 *   ./keys_probe [--n 300] [--fifo] [--keyboard /dev/input/eventN --hand]
 *
 * --fifo puts the evdev reader on SCHED_FIFO 1 (needs an rtprio grant).
 * --hand reads a real keyboard instead of uinput: press keys by hand; it
 * compares the kernel's time with the X server's for each press.
 * Needs read access to the evdev node and write access to /dev/uinput.
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAXN 4096
#define KEY KEY_F24 /* evdev 194; X keycode 202 */

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

typedef struct {
    int64_t w0, w1, k, r, x, xr;
} sample;

static sample S[MAXN];
static volatile int n_k, n_x;
static int evfd = -1;
static int stop_pipe[2];
static int hand;

static void* ev_reader(void* arg) {
    (void)arg;
    struct pollfd p[2] = {{evfd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
    for (;;) {
        if (poll(p, 2, -1) < 0 && errno != EINTR) break;
        if (p[1].revents) break;
        struct input_event ev[64];
        ssize_t got = read(evfd, ev, sizeof ev);
        int64_t r = now_ns();
        if (got <= 0) continue;
        for (int i = 0; i < (int)(got / (ssize_t)sizeof ev[0]); i++) {
            if (ev[i].type != EV_KEY || ev[i].value != 1) continue;
            if (!hand && ev[i].code != KEY) continue;
            int j = __atomic_load_n(&n_k, __ATOMIC_ACQUIRE);
            if (j >= MAXN) continue;
            S[j].k = (int64_t)ev[i].input_event_sec * 1000000000 +
                     (int64_t)ev[i].input_event_usec * 1000;
            S[j].r = r;
            __atomic_store_n(&n_k, j + 1, __ATOMIC_RELEASE);
        }
    }
    return NULL;
}

static Display* dpy;
static int xi_op;

static void* x_reader(void* arg) {
    (void)arg;
    int xfd = ConnectionNumber(dpy);
    struct pollfd p[2] = {{xfd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            int64_t xr = now_ns();
            XGenericEventCookie* c = &e.xcookie;
            if (c->type != GenericEvent || c->extension != xi_op) continue;
            if (!XGetEventData(dpy, c)) continue;
            if (c->evtype == XI_RawKeyPress) {
                XIRawEvent* re = c->data;
                if (hand || re->detail == KEY + 8) {
                    int j = n_x;
                    if (j < MAXN) {
                        S[j].x = (int64_t)re->time * 1000000;
                        S[j].xr = xr;
                        n_x = j + 1;
                    }
                }
            }
            XFreeEventData(dpy, c);
        }
        if (poll(p, 2, -1) < 0 && errno != EINTR) break;
        if (p[1].revents) break;
    }
    return NULL;
}

static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void stat(const char* name, int n, int64_t (*f)(const sample*)) {
    static int64_t v[MAXN];
    double sum = 0;
    for (int i = 0; i < n; i++) {
        v[i] = f(&S[i]);
        sum += (double)v[i];
    }
    qsort(v, (size_t)n, sizeof v[0], cmp64);
    printf("%-26s %5d  min %9.3f  mean %9.3f  p50 %9.3f  p99 %9.3f  max %9.3f ms\n", name, n,
           v[0] / 1e6, sum / n / 1e6, v[n / 2] / 1e6, v[(n * 99) / 100] / 1e6, v[n - 1] / 1e6);
}

static int64_t d_k_w0(const sample* s) { return s->k - s->w0; }
static int64_t d_w1_w0(const sample* s) { return s->w1 - s->w0; }
static int64_t d_r_k(const sample* s) { return s->r - s->k; }
static int64_t d_x_k(const sample* s) { return s->x - s->k; }
static int64_t d_x_w0(const sample* s) { return s->x - s->w0; }
static int64_t d_xr_k(const sample* s) { return s->xr - s->k; }

static void emit(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("uinput write");
}

int main(int argc, char** argv) {
    int n = 300, fifo = 0;
    const char* kbd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fifo")) fifo = 1;
        else if (!strcmp(argv[i], "--hand")) hand = 1;
        else if (!strcmp(argv[i], "--keyboard") && i + 1 < argc) kbd = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--n N] [--fifo] [--keyboard DEV --hand]\n", argv[0]);
            return 2;
        }
    }
    if (n > MAXN) n = MAXN;
    if (hand && !kbd) {
        fprintf(stderr, "--hand needs --keyboard\n");
        return 2;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "no X display\n");
        return 1;
    }
    int ev0, er0;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_op, &ev0, &er0)) {
        fprintf(stderr, "no XInput\n");
        return 1;
    }
    unsigned char mb[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mb, XI_RawKeyPress);
    XIEventMask m = {XIAllMasterDevices, sizeof mb, mb};
    XISelectEvents(dpy, DefaultRootWindow(dpy), &m, 1);
    XSync(dpy, False);

    int ui = -1;
    char node[64];
    if (!kbd) {
        ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (ui < 0) {
            perror("/dev/uinput");
            return 1;
        }
        ioctl(ui, UI_SET_EVBIT, EV_KEY);
        ioctl(ui, UI_SET_KEYBIT, KEY);
        /* udev calls it a keyboard (ID_INPUT_KEYBOARD, which the uaccess
         * rule needs) only with all of KEY_ESC..KEY_S */
        for (int k = KEY_ESC; k < 32; k++) ioctl(ui, UI_SET_KEYBIT, k);
        struct uinput_setup us;
        memset(&us, 0, sizeof us);
        us.id.bustype = BUS_USB;
        us.id.vendor = 0x1209;
        us.id.product = 0x0001;
        snprintf(us.name, sizeof us.name, "ysp keys_probe");
        if (ioctl(ui, UI_DEV_SETUP, &us) < 0 || ioctl(ui, UI_DEV_CREATE) < 0) {
            perror("uinput setup");
            return 1;
        }
        char sys[64] = {0};
        ioctl(ui, UI_GET_SYSNAME(sizeof sys), sys);
        /* the event node shows up and gets its ACL from udev a moment later */
        node[0] = 0;
        for (int t = 0; t < 200 && evfd < 0; t++) {
            char path[160];
            for (int e = 0; e < 64 && evfd < 0; e++) {
                snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sys, e);
                if (access(path, F_OK) == 0) {
                    snprintf(node, sizeof node, "/dev/input/event%d", e);
                    evfd = open(node, O_RDONLY | O_NONBLOCK);
                    if (evfd < 0) node[0] = 0;
                }
            }
            if (evfd < 0) usleep(10000);
        }
        if (evfd < 0) {
            fprintf(stderr, "uinput's event node %s: not readable\n", node);
            return 1;
        }
        /* Xorg's udev hotplug adds the device after a while */
        usleep(1500000);
    } else {
        snprintf(node, sizeof node, "%s", kbd);
        evfd = open(kbd, O_RDONLY | O_NONBLOCK);
        if (evfd < 0) {
            perror(kbd);
            return 1;
        }
    }
    int clk = CLOCK_MONOTONIC;
    if (ioctl(evfd, EVIOCSCLOCKID, &clk) < 0) perror("EVIOCSCLOCKID");
    printf("evdev %s, reader %s\n", node, fifo ? "SCHED_FIFO 1" : "SCHED_OTHER");

    if (pipe(stop_pipe)) return 1;
    pthread_t te, tx;
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (fifo) {
        struct sched_param sp = {1};
        pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&a, SCHED_FIFO);
        pthread_attr_setschedparam(&a, &sp);
    }
    if (pthread_create(&te, &a, ev_reader, NULL)) {
        fprintf(stderr, "reader thread failed (FIFO needs an rtprio grant)\n");
        return 1;
    }
    pthread_create(&tx, NULL, x_reader, NULL);

    if (hand) {
        printf("press keys on %s; %d presses ends it\n", kbd, n);
        while (n_k < n || n_x < n) usleep(20000);
    } else {
        srand(1);
        for (int i = 0; i < n; i++) {
            usleep(20000 + (unsigned)(rand() % 30000));
            S[i].w0 = now_ns();
            emit(ui, EV_KEY, KEY, 1);
            emit(ui, EV_SYN, SYN_REPORT, 0);
            S[i].w1 = now_ns();
            usleep(5000);
            emit(ui, EV_KEY, KEY, 0);
            emit(ui, EV_SYN, SYN_REPORT, 0);
        }
        int64_t until = now_ns() + 500000000;
        while ((n_k < n || n_x < n) && now_ns() < until) usleep(5000);
    }
    if (write(stop_pipe[1], "x", 1) != 1) return 1;
    pthread_join(te, NULL);
    pthread_join(tx, NULL);
    if (ui >= 0) {
        ioctl(ui, UI_DEV_DESTROY);
        close(ui);
    }
    printf("evdev presses %d, X presses %d, of %d\n", n_k, n_x, n);
    if (n_k != n || n_x != n) {
        fprintf(stderr, "counts differ; the pairs below would be wrong\n");
        return 1;
    }
    if (!hand) {
        stat("write call (w1 - w0)", n, d_w1_w0);
        stat("kernel - write start", n, d_k_w0);
    }
    stat("evdev read - kernel", n, d_r_k);
    stat("X server - kernel", n, d_x_k);
    if (!hand) stat("X server - write start", n, d_x_w0);
    stat("Xlib returned - kernel", n, d_xr_k);
    return 0;
}
