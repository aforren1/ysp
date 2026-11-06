/* input_test.c - self-checking test for ysp/input.h. No framework: it
 * returns 0 when every check passed and 1 after printing each failure.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o input_test tests/adapt/input_test.c
 *
 * The layout and the constants are in data files and in producers built
 * against older copies, so their values are pinned here. The adapters are
 * checked on events filled by hand: with YIN_TEST_SDL defined and SDL3's
 * headers on the path, the SDL adapter (nothing is linked); always the
 * raw mouse adapter, with ysp/screen.h's declarations. ysp/response.h's
 * test runs every adapter case again through its aliases.
 */
#if defined(YIN_TEST_SDL)
#include <SDL3/SDL_events.h>
#endif
#include "ysp/screen.h"
#include "ysp/input.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "input_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)

static void check_layout(void) {
    CHECK_I(sizeof(yin_event), 48);
    CHECK_I(offsetof(yin_event, t), 0);
    CHECK_I(offsetof(yin_event, device), 8);
    CHECK_I(offsetof(yin_event, control), 12);
    CHECK_I(offsetof(yin_event, code), 16);
    CHECK_I(offsetof(yin_event, mods), 20);
    CHECK_I(offsetof(yin_event, kind), 22);
    CHECK_I(offsetof(yin_event, type), 23);
    CHECK_I(offsetof(yin_event, flags), 24);
    CHECK_I(offsetof(yin_event, stamp), 25);
    CHECK_I(offsetof(yin_event, x), 28);
    CHECK_I(offsetof(yin_event, y), 32);
    CHECK_I(offsetof(yin_event, value), 36);
    CHECK_I(offsetof(yin_event, aux), 40);
    /* v0.3.0: the two spare places, so every older offset stays */
    CHECK_I(offsetof(yin_event, unc_us), 26);
    CHECK_I(sizeof(((yin_event*)0)->unc_us), 2);
    CHECK_I(offsetof(yin_event, ticks), 44);
    CHECK_I(sizeof(((yin_event*)0)->ticks), 4);
    /* v0.4.0: the source's bounds are int64_t ns */
    CHECK_I(sizeof(((yin_source*)0)->lo_ns), 8);
    CHECK_I(offsetof(yin_source, lo_ns), 8);
    CHECK_I(offsetof(yin_source, hi_ns), 16);
    CHECK_I(offsetof(yin_source, note), 24);
    CHECK_I(sizeof(yin_source), 32);
}

static void check_constants(void) {
    CHECK_I(YIN_KIND_KEYBOARD, 0);
    CHECK_I(YIN_KIND_MOUSE, 1);
    CHECK_I(YIN_KIND_TOUCH, 2);
    CHECK_I(YIN_KIND_PEN, 3);
    CHECK_I(YIN_KIND_GAMEPAD, 4);
    CHECK_I(YIN_KIND_BOX, 5);
    CHECK_I(YIN_KIND_EYE, 6);
    CHECK_I(YIN_KIND_SYNC, 7);
    CHECK_I(YIN_KIND_USER, 8);
    CHECK_I(YIN_KIND_BIT(YIN_KIND_BOX), 32);
    CHECK_I(YIN_KINDS_ALL, 0xFFFF);
    CHECK_I(YIN_NONE, 0);
    CHECK_I(YIN_PRESS, 1);
    CHECK_I(YIN_RELEASE, 2);
    CHECK_I(YIN_SAMPLE, 3);
    CHECK_I(YIN_PROXIMITY, 4);
    CHECK_I(YIN_REPEAT, 1);
    CHECK_I(YIN_SYNTHETIC, 2);
    CHECK_I(YIN_ERASER, 4);
    CHECK_I(YIN_UNLISTED, 8);
    CHECK_I(YIN_DEVTICKS, 16);
    CHECK_I(YIN_EYE_GAZE, 0);
    CHECK_I(YIN_EYE_GAZE, YIN_AXIS_POSITION);
    CHECK_I(YIN_EYE_FIXATION, 1);
    CHECK_I(YIN_EYE_SACCADE, 2);
    CHECK_I(YIN_EYE_BLINK, 3);
    CHECK_I(YIN_EYE_LEFT, 0);
    CHECK_I(YIN_EYE_RIGHT, 1);
    CHECK_I(YIN_EYE_BOTH, 2);
    CHECK_I(YIN_UNC_MAX, 65535);
    CHECK_I(YIN_AXIS_POSITION, 0);
    CHECK_I(YIN_AXIS_DELTA, 1);
    CHECK_I(YIN_AXIS_ABSOLUTE, 2);
    CHECK_I(YIN_AXIS_WHEEL, 3);
    CHECK_I(YIN_AXIS_PEN, 16);
    CHECK_I(YIN_AXIS_PEN_PRESSURE, 16);
    CHECK_I(YIN_AXIS_GAMEPAD, 32);
    CHECK_I(YIN_TIER_UNKNOWN, 0);
    CHECK_I(YIN_TIER_1, 1);
    CHECK_I(YIN_TIER_2, 2);
    CHECK_I(YIN_TIER_3, 3);
    CHECK_I(YIN_TIER_SIM, 4);
    CHECK_I(yin_sdl_mouse_stamp(0), YIN_TIER_3);
    CHECK_I(yin_sdl_mouse_stamp(65661), 0);
}

static void check_raw_mouse(void) {
    yscr_mouse_event m;
    yin_event e[12];
    yin_source s;
    int n;
    memset(&m, 0, sizeof m);
    m.t = 123;
    m.device = 65661;
    m.down = YSCR_MOUSE_RIGHT;
    m.dx = -7;
    memset(e, 0xA5, sizeof e);   /* the adapter must write every byte it owns */
    n = yin_from_mouse(&m, e, 12);
    CHECK_I(n, 2);
    CHECK_I(e[0].unc_us, 0);
    CHECK_I(e[0].ticks, 0);
    CHECK_I(e[0].flags & YIN_DEVTICKS, 0);
    CHECK_I(e[1].ticks, 0);
    CHECK_I(e[0].kind, YIN_KIND_MOUSE);
    CHECK_I(e[0].type, YIN_PRESS);
    CHECK_I(e[0].control, 3);
    CHECK_I(e[0].device, 65661);
    CHECK_I(e[0].t, 123);
    CHECK_I(e[1].control, YIN_AXIS_DELTA);
    CHECK_I((long long)e[1].x, -7);
    CHECK_I(yin_from_mouse(&m, e, 1), 1);
    s = yin_raw_mouse_source();
    CHECK_I(s.kind, YIN_KIND_MOUSE);
    CHECK_I(s.tier, YIN_TIER_UNKNOWN);
    /* v0.5.1: the note is the platform's own measurement */
#if defined(__APPLE__)
    CHECK_I(s.note && strstr(s.note, "macOS") && strstr(s.note, "55 us") && strstr(s.note, "122 us") &&
            strstr(s.note, "125 Hz"), 1);
#elif defined(__linux__)
    /* v0.6.0: the kernel's stamp, not Windows' read time */
    CHECK_I(s.note && strstr(s.note, "Linux") && strstr(s.note, "evdev") && !strstr(s.note, "0.15 to 0.40"), 1);
    CHECK_I(s.partial, 1);
#else
    CHECK_I(s.note && strstr(s.note, "0.15 to 0.40 ms") && !strstr(s.note, "macOS"), 1);
#endif
}

#if defined(YIN_TEST_SDL)
/* v0.6.0: every pen event carries SDL's pen_state in code and the eraser
 * flag from it; tilt is an axis sample's value, the position its x, y */
static void check_pen(void) {
    SDL_Event ev;
    yin_event e;
    const uint32_t st = SDL_PEN_INPUT_DOWN | SDL_PEN_INPUT_BUTTON_1 | SDL_PEN_INPUT_ERASER_TIP;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_PEN_MOTION;
    ev.pmotion.which = 21;
    ev.pmotion.pen_state = st;
    ev.pmotion.x = 100.5f;
    ev.pmotion.y = 200.25f;
    memset(&e, 0xA5, sizeof e);
    CHECK_I(yin_from_sdl(&ev, 1000, NULL, &e), 1);
    CHECK_I(e.kind, YIN_KIND_PEN);
    CHECK_I(e.type, YIN_SAMPLE);
    CHECK_I(e.control, YIN_AXIS_POSITION);
    CHECK_I(e.device, 21);
    CHECK_I(e.t, 1000);
    CHECK_I(e.code, st);
    CHECK_I(e.flags, YIN_ERASER);
    CHECK_I(e.x == 100.5f && e.y == 200.25f, 1);
    CHECK_I(e.value == 0.0f, 1);
    CHECK_I(e.stamp, 0);
    CHECK_I(e.unc_us, 0);
    ev.pmotion.pen_state = SDL_PEN_INPUT_DOWN;   /* the tip */
    CHECK_I(yin_from_sdl(&ev, 1000, NULL, &e), 1);
    CHECK_I(e.code, SDL_PEN_INPUT_DOWN);
    CHECK_I(e.flags, 0);

    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_PEN_AXIS;
    ev.paxis.which = 21;
    ev.paxis.axis = SDL_PEN_AXIS_XTILT;
    ev.paxis.value = -42.5f;
    ev.paxis.x = 100.5f;
    ev.paxis.y = 200.25f;
    ev.paxis.pen_state = SDL_PEN_INPUT_ERASER_TIP;
    CHECK_I(yin_from_sdl(&ev, 1001, NULL, &e), 1);
    CHECK_I(e.type, YIN_SAMPLE);
    CHECK_I(e.control, YIN_AXIS_PEN + 1);
    CHECK_I(e.value == -42.5f, 1);   /* the tilt, in degrees */
    CHECK_I(e.x == 100.5f && e.y == 200.25f, 1);   /* the pen's position, not the tilt */
    CHECK_I(e.code, SDL_PEN_INPUT_ERASER_TIP);
    CHECK_I(e.flags, YIN_ERASER);
    ev.paxis.axis = SDL_PEN_AXIS_YTILT;
    CHECK_I(yin_from_sdl(&ev, 1001, NULL, &e), 1);
    CHECK_I(e.control, YIN_AXIS_PEN + 2);
    ev.paxis.axis = SDL_PEN_AXIS_PRESSURE;
    ev.paxis.value = 0.5f;
    CHECK_I(yin_from_sdl(&ev, 1001, NULL, &e), 1);
    CHECK_I(e.control, YIN_AXIS_PEN_PRESSURE);

    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_PEN_UP;   /* the eraser lifted: the state has it, the flag too */
    ev.ptouch.which = 21;
    ev.ptouch.eraser = true;
    ev.ptouch.down = false;
    ev.ptouch.pen_state = SDL_PEN_INPUT_ERASER_TIP;
    ev.ptouch.x = 3.0f;
    CHECK_I(yin_from_sdl(&ev, 1002, NULL, &e), 1);
    CHECK_I(e.type, YIN_RELEASE);
    CHECK_I(e.control, 0);
    CHECK_I(e.code, SDL_PEN_INPUT_ERASER_TIP);
    CHECK_I(e.flags, YIN_ERASER);
    CHECK_I(e.value == 0.0f, 1);
    CHECK_I(e.x == 3.0f, 1);
    ev.ptouch.eraser = false;   /* the tip */
    ev.ptouch.pen_state = 0;
    CHECK_I(yin_from_sdl(&ev, 1002, NULL, &e), 1);
    CHECK_I(e.flags, 0);
    ev.type = SDL_EVENT_PEN_DOWN;
    ev.ptouch.down = true;
    ev.ptouch.pen_state = SDL_PEN_INPUT_DOWN;
    CHECK_I(yin_from_sdl(&ev, 1003, NULL, &e), 1);
    CHECK_I(e.type, YIN_PRESS);
    CHECK_I(e.code, SDL_PEN_INPUT_DOWN);
    CHECK_I(e.value == 1.0f, 1);

    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_PEN_BUTTON_DOWN;
    ev.pbutton.which = 21;
    ev.pbutton.button = 2;
    ev.pbutton.down = true;
    ev.pbutton.pen_state = SDL_PEN_INPUT_BUTTON_2 | SDL_PEN_INPUT_ERASER_TIP;
    CHECK_I(yin_from_sdl(&ev, 1004, NULL, &e), 1);
    CHECK_I(e.type, YIN_PRESS);
    CHECK_I(e.control, 2);
    CHECK_I(e.code, SDL_PEN_INPUT_BUTTON_2 | SDL_PEN_INPUT_ERASER_TIP);
    CHECK_I(e.flags, YIN_ERASER);

    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_PEN_PROXIMITY_IN;
    ev.pproximity.which = 21;
#if YIN__SDL_PROXIMITY_STATE
    ev.pproximity.pen_state = SDL_PEN_INPUT_IN_PROXIMITY | SDL_PEN_INPUT_ERASER_TIP;
#endif
    memset(&e, 0xA5, sizeof e);
    CHECK_I(yin_from_sdl(&ev, 1005, NULL, &e), 1);
    CHECK_I(e.kind, YIN_KIND_PEN);
    CHECK_I(e.type, YIN_PROXIMITY);
    CHECK_I(e.value == 1.0f, 1);
    CHECK_I(e.x == 0.0f && e.y == 0.0f, 1);
#if YIN__SDL_PROXIMITY_STATE
    CHECK_I(e.code, SDL_PEN_INPUT_IN_PROXIMITY | SDL_PEN_INPUT_ERASER_TIP);
    CHECK_I(e.flags, YIN_ERASER);
#else
    CHECK_I(e.code, 0);
    CHECK_I(e.flags, 0);
#endif
    ev.type = SDL_EVENT_PEN_PROXIMITY_OUT;
#if YIN__SDL_PROXIMITY_STATE
    ev.pproximity.pen_state = 0;
#endif
    CHECK_I(yin_from_sdl(&ev, 1006, NULL, &e), 1);
    CHECK_I(e.type, YIN_PROXIMITY);
    CHECK_I(e.value == 0.0f, 1);
    CHECK_I(e.code, 0);
    CHECK_I(e.flags, 0);

    /* the mouse events SDL makes from a pen: dropped unless kept, flagged */
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.which = SDL_PEN_MOUSEID;
    CHECK_I(yin_from_sdl(&ev, 1007, NULL, &e), 0);
    {
        yin_sdl_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.keep_synthetic = true;
        CHECK_I(yin_from_sdl(&ev, 1007, &ctx, &e), 1);
        CHECK_I(e.flags, YIN_SYNTHETIC);
    }
}

static void check_sdl(void) {
    SDL_Event ev;
    yin_event e;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.down = true;
    ev.key.scancode = SDL_SCANCODE_J;
    ev.key.key = SDLK_J;
    ev.key.which = 9;
    CHECK_I(yin_from_sdl(&ev, 55, NULL, &e), 1);
    CHECK_I(e.kind, YIN_KIND_KEYBOARD);
    CHECK_I(e.control, 13);
    CHECK_I(e.code, 'j');
    CHECK_I(e.device, 9);
    CHECK_I(e.t, 55);
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ev.button.down = true;
    ev.button.button = 1;
    CHECK_I(yin_from_sdl(&ev, 1, NULL, &e), 1);
    CHECK_I(e.stamp, YIN_TIER_3);
    ev.type = SDL_EVENT_WINDOW_SHOWN;
    CHECK_I(yin_from_sdl(&ev, 1, NULL, &e), 0);
    {   /* v0.5.0: the OS's own time (ysp/screen.h on macOS: NSEvent.timestamp) */
        yin_sdl_ctx ctx;
        yin_source src;
        memset(&ctx, 0, sizeof ctx);
        ctx.os_stamps = true;
        ctx.t_os = 1000;
        ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;   /* which 0: tier 3 without OS stamps */
        CHECK_I(yin_from_sdl(&ev, 1500, &ctx, &e), 1);
        CHECK_I(e.t, 1000);
        CHECK_I(e.stamp, 0);
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_EVENT_KEY_UP;
        ev.key.scancode = SDL_SCANCODE_J;
        CHECK_I(yin_from_sdl(&ev, 1500, &ctx, &e), 1);   /* raw_keyboard off: tier 3 without */
        CHECK_I(e.t, 1000);
        CHECK_I(e.stamp, 0);
        ctx.t_os = 0;   /* no NSEvent found: SDL's stamp, flagged */
        CHECK_I(yin_from_sdl(&ev, 1500, &ctx, &e), 1);
        CHECK_I(e.t, 1500);
        CHECK_I(e.stamp, YIN_TIER_3);
        ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
        CHECK_I(yin_from_sdl(&ev, 1500, &ctx, &e), 1);
        CHECK_I(e.stamp, YIN_TIER_3);
        ctx.os_stamps = false;   /* as before v0.5.0 */
        ev.type = SDL_EVENT_KEY_DOWN;
        ev.key.down = true;
        CHECK_I(yin_from_sdl(&ev, 1500, &ctx, &e), 1);
        CHECK_I(e.t, 1500);
        CHECK_I(e.stamp, YIN_TIER_3);
        ctx.raw_keyboard = true;
        CHECK_I(yin_from_sdl(&ev, 1500, &ctx, &e), 1);
        CHECK_I(e.stamp, 0);
        ctx.os_stamps = true;
        src = yin_sdl_source(&ctx, YIN_KIND_KEYBOARD);
#if defined(__APPLE__)
        CHECK_I(src.tier, YIN_TIER_UNKNOWN);
        CHECK_I(src.partial, 1);
        CHECK_I(src.lo_ns, 49300);
        CHECK_I(src.hi_ns, 46591000);
        src = yin_sdl_source(&ctx, YIN_KIND_MOUSE);
        CHECK_I(src.lo_ns, 49300);
        ctx.os_stamps = false;
        src = yin_sdl_source(&ctx, YIN_KIND_KEYBOARD);
        CHECK_I(src.tier, YIN_TIER_3);
        CHECK_I(src.lo_ns, 262000);
        CHECK_I(src.hi_ns, 67962000);
#elif defined(__linux__)
        /* v0.6.0: SDL's X11 pump time, tier 3; buttons measured with XTEST */
        src = yin_sdl_source(&ctx, YIN_KIND_MOUSE);
        CHECK_I(src.tier, YIN_TIER_3);
        CHECK_I(src.partial, 1);
        CHECK_I(src.lo_ns, 1498000);
        CHECK_I(src.hi_ns, 16646000);
        src = yin_sdl_source(NULL, YIN_KIND_PEN);
        CHECK_I(src.tier, YIN_TIER_3);
        CHECK_I(src.lo_ns == 0 && src.hi_ns == 0, 1);
        src = yin_sdl_source(NULL, YIN_KIND_TOUCH);
        CHECK_I(src.tier, YIN_TIER_3);
        src = yin_sdl_source(NULL, YIN_KIND_KEYBOARD);
        CHECK_I(src.tier, YIN_TIER_3);
        CHECK_I(src.note && strstr(src.note, "kernel"), 1);
        src = yin_sdl_source(NULL, YIN_KIND_GAMEPAD);
        CHECK_I(src.tier, YIN_TIER_UNKNOWN);
#else
        CHECK_I(src.note != NULL, 1);
#endif
    }
}
#endif

int main(void) {
    printf("ysp_input %s\n", YIN_VERSION_STRING);
    check_layout();
    check_constants();
    check_raw_mouse();
#if defined(YIN_TEST_SDL)
    check_sdl();
    check_pen();
#endif
    if (g_failures) {
        fprintf(stderr, "input_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("input_test: all %d checks passed\n", g_checks);
    return 0;
}
