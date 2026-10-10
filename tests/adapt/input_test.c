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
}

#if defined(YIN_TEST_SDL)
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
}
#endif

int main(void) {
    printf("ysp_input %s\n", YIN_VERSION_STRING);
    check_layout();
    check_constants();
    check_raw_mouse();
#if defined(YIN_TEST_SDL)
    check_sdl();
#endif
    if (g_failures) {
        fprintf(stderr, "input_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("input_test: all %d checks passed\n", g_checks);
    return 0;
}
