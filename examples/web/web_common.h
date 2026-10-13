/* web_common.h - what the web demos share (docs/web.md): the signal that
 * the stimuli are ready, the two halves of the screen as the touch and
 * mouse targets of a two-choice task, the response's input kind for the
 * data file, and the stimulus scale.
 *
 * Include it after ysp/screen.h (or ysp/gfx.h) and ysp/response.h. The
 * same code builds natively, where web_ready() does nothing.
 */
#ifndef WEB_COMMON_H
#define WEB_COMMON_H

#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
/* The page shows the stage when this comes: setup, the stimuli and their
 * prime are done, so the first frame the participant sees is a frame of
 * the loop, not of setup (docs/web.md, "Priming"). */
EM_JS(void, web_ready, (void), {
    if (Module['onYspReady']) Module['onYspReady']();
})
#else
static inline void web_ready(void) {}
#endif

/* Touch and mouse presses and releases become choices by where they
 * landed: code 1 on the left half of the window, 2 on the right half, so a
 * choice { kind TOUCH or MOUSE, match KEYCODE, code 1 or 2 } takes them,
 * beside the keys. Only the primary mouse button counts. The event keeps
 * its time, kind, device and control (the finger), so ysp/response.h pairs
 * each release with its press as it does for keys. Touch x is 0..1 of the
 * window (SDL); mouse x is in window coordinates. */
static inline void web_side(yscr_screen* s, yin_event* in) {
    float x = -1;
    if (in->type != YIN_PRESS && in->type != YIN_RELEASE) return;
    if (in->kind == YIN_KIND_TOUCH) {
        x = in->x;
    } else if (in->kind == YIN_KIND_MOUSE && in->control == 1) {
        int w = 0, h = 0;
        if (!yscr_window(s) || !SDL_GetWindowSize(yscr_window(s), &w, &h) || w <= 0) return;
        x = in->x / (float)w;
    }
    if (x >= 0) in->code = x < 0.5f ? 1u : 2u;
}

/* The input kind of a response, by name, for the data file. */
static inline const char* web_kind_name(int kind) {
    switch (kind) {
    case YIN_KIND_KEYBOARD: return "keyboard";
    case YIN_KIND_MOUSE:    return "mouse";
    case YIN_KIND_TOUCH:    return "touch";
    case YIN_KIND_PEN:      return "pen";
    default:                return "other";
    }
}

/* "WxH" into *w and *h; 0 when it is not two positive numbers. */
static inline int web_size_arg(const char* a, int* w, int* h) {
    char* end;
    long x = strtol(a, &end, 10), y;
    if (end == a || *end != 'x') return 0;
    a = end + 1;
    y = strtol(a, &end, 10);
    if (end == a || *end || x < 64 || y < 64 || x > 8192 || y > 8192) return 0;
    *w = (int)x;
    *h = (int)y;
    return 1;
}

#endif /* WEB_COMMON_H */
