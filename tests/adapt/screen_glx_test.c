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
 *      frame's pump, 8 to 17 ms later). Skipped without libXtst or focus.
 *   5. With vblank_mode=0 in the environment, open() refuses.
 *   6. In an 800 x 600 window (the Present copy path): depth 1, so 60
 *      frames each show on the next vblank, tier 2.
 *   7. desc.x11_override_redirect: the fullscreen window is
 *      override-redirect, its flips are Present flips, and it has the
 *      keyboard focus (20 of 20 XTEST keys; no window manager gives it).
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

int main(void) {
    static yscr_screen scr;
    yscr_frame f;
    seen run1, run2;
    char line[1024];
    int i;
    const char* dsp = getenv("DISPLAY");
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
            CHECK(ok == 20);
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
            CHECK(ok == 20);
        }
    }

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
