/* uinput_keys.c - a keyboard made through uinput that sends taps of F14 and
 * logs when, for comparing key stamps (Psychtoolbox's KbQueue, ysp/screen.h)
 * with the kernel's evdev time and the write() call. README.md here.
 *
 *   uinput_keys N DELAY_S out.csv
 *
 * Makes the device ("ysp uinput_keys"), waits DELAY_S seconds (Xorg adds a
 * new keyboard about a second after it appears; the reader sets up its queue
 * meanwhile), then sends N press-release taps 20 to 50 ms apart, the release
 * 5 ms after the press. One line per press:
 *   i,w0_mono_ns,w1_mono_ns,w0_real_ns,kernel_mono_ns
 * w0 and w1 bracket the write(); kernel_mono_ns is the event's evdev time
 * (CLOCK_MONOTONIC), read back from the device's own node (needs the uaccess
 * rule, tools/linux/70-ysp-input.rules; 0 when unreadable).
 *
 * Build: cc -O2 -Wall -o uinput_keys uinput_keys.c
 *
 * Public domain / MIT-0, as the rest of ysp. */
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define KEY KEY_F14   /* X keycode 192: nothing binds it on Xfce */

static int64_t ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void sleep_ns(int64_t d) {
    struct timespec t = { (time_t)(d / 1000000000), (long)(d % 1000000000) };
    while (nanosleep(&t, &t) != 0) { }
}

static void tap(int fd, int down) {
    struct input_event e[2];
    memset(e, 0, sizeof e);
    e[0].type = EV_KEY;
    e[0].code = KEY;
    e[0].value = down;
    e[1].type = EV_SYN;
    e[1].code = SYN_REPORT;
    if (write(fd, e, sizeof e) != (ssize_t)sizeof e) perror("uinput write");
}

/* The kernel's time of the next press on the device's node, or 0. */
static int64_t kernel_press(int ev) {
    struct input_event b[16];
    int64_t until = ns(CLOCK_MONOTONIC) + 200000000;
    if (ev < 0) return 0;
    while (ns(CLOCK_MONOTONIC) < until) {
        ssize_t r = read(ev, b, sizeof b);
        for (int i = 0; i < (int)(r > 0 ? r / (ssize_t)sizeof b[0] : 0); i++)
            if (b[i].type == EV_KEY && b[i].code == KEY && b[i].value == 1)
                return (int64_t)b[i].input_event_sec * 1000000000 + (int64_t)b[i].input_event_usec * 1000;
        sleep_ns(100000);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: uinput_keys N DELAY_S out.csv\n"); return 2; }
    int n = atoi(argv[1]);
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    /* udev calls it a keyboard (ID_INPUT_KEYBOARD, which the uaccess rule
     * and Xorg's keyboard class need) only with all of KEY_ESC..KEY_S */
    for (int k = KEY_ESC; k < 32; k++) ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_KEYBIT, KEY);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1209;
    us.id.product = 0x0001;
    snprintf(us.name, sizeof us.name, "ysp uinput_keys");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { perror("uinput setup"); return 1; }
    char sys[64] = { 0 }, path[160];
    int ev = -1;
    ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys);
    for (int t = 0; t < 100 && ev < 0; t++) {
        for (int e = 0; e < 256 && ev < 0; e++) {
            snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sys, e);
            if (access(path, F_OK) == 0) {
                snprintf(path, sizeof path, "/dev/input/event%d", e);
                ev = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            }
        }
        if (ev < 0) sleep_ns(10000000);
    }
    if (ev >= 0) {
        int clk = CLOCK_MONOTONIC;
        ioctl(ev, EVIOCSCLOCKID, &clk);
    }
    sleep_ns((int64_t)(atof(argv[2]) * 1e9));
    FILE* f = fopen(argv[3], "w");
    if (!f) { perror(argv[3]); return 1; }
    fprintf(f, "i,w0_mono_ns,w1_mono_ns,w0_real_ns,kernel_mono_ns\n");
    srand(1);
    for (int i = 0; i < n; i++) {
        sleep_ns(20000000 + (int64_t)(rand() % 30000) * 1000);
        int64_t r0 = ns(CLOCK_REALTIME), w0 = ns(CLOCK_MONOTONIC);
        tap(fd, 1);
        int64_t w1 = ns(CLOCK_MONOTONIC);
        sleep_ns(5000000);
        tap(fd, 0);
        fprintf(f, "%d,%lld,%lld,%lld,%lld\n", i, (long long)w0, (long long)w1, (long long)r0,
                (long long)kernel_press(ev));
    }
    fclose(f);
    sleep_ns(500000000);   /* the reader drains before the device goes */
    ioctl(fd, UI_DEV_DESTROY);
    return 0;
}
