/* ysp/input.h - v0.4.0 - public domain single-header input event
 *
 *   One timestamped input event for every device a rig reads: keyboards,
 *   mice, touch, pens, gamepads, response and button boxes, eye trackers.
 *   Producers (ysp/screen.h's SDL events and raw mice, a serial box reader,
 *   an eye tracker) make these events; consumers (ysp/response.h's
 *   collector, a data log, the player's input.key) read them, and neither
 *   side needs the other's header. Also: the timing quality of a source
 *   (the tier scale of ysp/screen.h and ysp/audio.h), and adapters from
 *   SDL3 events and ysp/screen.h's raw mouse reports.
 *
 *   Types and inline functions only: nothing to implement, nothing to
 *   link, no heap, no OS calls. C99 is the floor: it builds as C99, C11
 *   and C++17, and in the C dialect MSVC compiles by default. The SDL3
 *   adapter appears when SDL3's SDL_events.h was included first (or when
 *   this header is included again after it).
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Include it; there is no implementation to define. A producer:
 *
 *       yin_event e = { 0 };
 *       e.t = yrt_now_ns();
 *       e.kind = YIN_KIND_BOX;
 *       e.device = port;
 *       e.control = button;
 *       e.type = YIN_PRESS;
 *       e.value = 1.0f;
 *
 *   From SDL3 (include SDL3 or ysp/screen.h first):
 *
 *       while (yscr_poll(&scr, &ev, &t))
 *           if (yin_from_sdl(&ev, t, NULL, &e)) yrsp_feed(&rsp, &e);
 *
 *   ---------------------------------------------------------------------
 *   THE EVENT (yin_event)
 *   ---------------------------------------------------------------------
 *   One 48-byte struct for every device:
 *     t        ysp_rt ns
 *     kind     YIN_KIND_KEYBOARD, _MOUSE, _TOUCH, _PEN, _GAMEPAD, _BOX
 *              (response box, button box, pedal, an Arduino-style device),
 *              _EYE, _SYNC (a timing event that is not a response: a
 *              scanner pulse, a TTL input, a photodiode or sound-key edge;
 *              control = the line or channel), _USER + 0..7
 *     device   the instance within the kind (SDL's `which`, a port index);
 *              0 = unknown
 *     type     PRESS, RELEASE, SAMPLE (a position or an axis value),
 *              PROXIMITY (value 1 in range, 0 out)
 *     control  keyboard: the SDL scancode (the USB HID usage: the physical
 *              key); mouse, pen, gamepad, box: the button; touch: the
 *              contact (finger) id, low 32 bits; a SAMPLE: an axis
 *              (YIN_AXIS_POSITION, YIN_AXIS_PEN + SDL_PenAxis,
 *              YIN_AXIS_GAMEPAD + SDL_GamepadAxis), except touch, whose
 *              SAMPLE control is the contact
 *     code     keyboard: the SDL keycode (the layout's key); else yours
 *     mods     keyboard: SDL_Keymod
 *     flags    YIN_REPEAT (the OS's auto-repeat of a held key),
 *              YIN_SYNTHETIC (made by the OS or SDL from another
 *              device), YIN_ERASER (the pen's eraser end),
 *              YIN_UNLISTED (a raw mouse SDL does not list),
 *              YIN_DEVTICKS (ticks is set)
 *     stamp    the stamp's tier (SOURCES); 0 = the source table's
 *     unc_us   this event's own uncertainty in us: a device clock fit's
 *              spread at this time, a bracket's width, a stamp's known
 *              quantizer; 0 = the source entry's bounds; 65535 = 65.5 ms
 *              or more. The one time in ysp that is not int64_t ns: a
 *              uint16_t in the 2 bytes that were free, because an int64_t
 *              would make the event 56 bytes and ysp/screen.h's input
 *              slot (16 bytes of state and the event) 72, past one cache
 *              line. Its name says the unit
 *     x, y     position: window coordinates for mouse and pen, 0..1 for
 *              touch; pen tilt
 *     value    an axis value, pressure; 1 for a press, 0 for a release
 *     aux      yours
 *     ticks    with YIN_DEVTICKS: the low 32 bits of the device's own
 *              clock for this event, in the source's unit, so the analysis
 *              can map the event again with an offline fit of the clock
 *              pairs in the log (ysp/rt.h DEVICE CLOCK FIT). 32 bits are
 *              enough: the pairs carry the whole count
 *   Eye trackers (kind EYE): control YIN_EYE_GAZE with type SAMPLE (x, y
 *   window pixels, value the pupil); YIN_EYE_FIXATION, _SACCADE and
 *   _BLINK with PRESS at the start and RELEASE at the end; PROXIMITY with
 *   value 0 when the tracker loses the eye and 1 when it finds it again;
 *   code the eye (YIN_EYE_LEFT, _RIGHT, _BOTH).
 *   Consumers take them in arrival order. Their times need not rise:
 *   sources stamp differently (a response box's device clock, SDL's raw
 *   and message paths), so a consumer reads t, not the order
 *   (ysp/response.h does).
 *
 *   ---------------------------------------------------------------------
 *   SOURCES AND TIMING QUALITY (yin_source)
 *   ---------------------------------------------------------------------
 *   Each source states how its stamps relate to the physical event, on
 *   the tier scale of ysp/screen.h and ysp/audio.h, for the whole chain
 *   from the finger to the stamp:
 *     YIN_TIER_UNKNOWN (0) not measured end to end
 *     YIN_TIER_1  a device clock mapped to ysp_rt by a fit that a
 *                    loopback test checked
 *     YIN_TIER_2  good under stated conditions, measured
 *     YIN_TIER_3  development only: a tick-quantized OS time, a poll
 *     YIN_TIER_SIM synthetic input, exact by construction
 *   An entry also gives measured bounds (lo_ns, hi_ns: stamp minus event)
 *   and `partial` when they cover the host part only (the OS saw it ->
 *   the stamp), not the device's scan, debounce and USB poll. The entry of
 *   a kind with device 0 covers every device of that kind; an entry with
 *   the device's id wins. event.stamp, when set, overrides the tier.
 *   ysp/response.h's result copies the responding source's tier, bounds
 *   and partial flag next to the onset's tier; it computes no combined
 *   uncertainty, because the two rest on different evidence.
 *   Measured on the one Windows 11 machine of ysp/screen.h's STATUS
 *   (docs/response.md and docs/screen.md have the tables): SDL
 *   3.4's raw keyboard path stamped keys sent with SendInput 0.16 to 0.67
 *   ms after the call; the message path 6.2 ms before to 12.1 ms after. Mouse, touch and pen
 *   events are stamped with the message time (read in SDL 3.4's source),
 *   not measured. yin_sdl_source() returns these as entries.
 *
 *   ---------------------------------------------------------------------
 *   SDL3 (yin_from_sdl, defined when SDL_events.h came first)
 *   ---------------------------------------------------------------------
 *   Converts keyboard, mouse button and motion, touch, pen and gamepad
 *   events. It passes SDL 3.4's second key reports (a key with no scan
 *   code comes from two paths); ysp/screen.h's yscr_event_input() converts
 *   through it and drops them (ysp/screen.h INPUT, "Second key reports").
 *   It reads fields only and calls no SDL function. It drops the
 *   mouse events SDL makes from pen and touch input (which =
 *   SDL_PEN_MOUSEID, SDL_TOUCH_MOUSEID) and the touch events it makes from
 *   a pen (SDL_PEN_TOUCHID) and a mouse, unless ctx.keep_synthetic.
 *   Keys come on the message path, stamped later and coarser, while SDL
 *   text input is on for the window, which another library can start (Dear
 *   ImGui's SDL3 backend does): set ctx.raw_keyboard from ysp/screen.h's
 *   caps.raw_keyboard each frame, and those keys get tier 3. Mouse events
 *   with which 0 (SDL's absolute mode, message-timed) get tier 3
 *   (yin_sdl_mouse_stamp). Gamepad axes become -1..1 (triggers 0..1).
 *   Mouse and pen positions are SDL's window coordinates; touch positions
 *   are 0..1. ysp/screen.h starts SDL's video subsystem only: for gamepads
 *   set its desc.gamepads, or call SDL_InitSubSystem(SDL_INIT_GAMEPAD) and
 *   open each gamepad (SDL_OpenGamepad) yourself.
 *
 *   ---------------------------------------------------------------------
 *   RAW MICE (yin_mouse_report, yin_from_mouse)
 *   ---------------------------------------------------------------------
 *   ysp/screen.h's desc.raw_mice (Windows) reads each mouse's Raw Input on
 *   a thread into yin_mouse_reports. yin_from_mouse() turns one report
 *   into up to 12 events of YIN_KIND_MOUSE with the
 *   mouse's device id, in this order: presses, releases (buttons numbered
 *   as SDL's: 1 left, 2 middle, 3 right, 4 and 5 the side buttons), a
 *   YIN_AXIS_DELTA sample (x, y in counts, unaccelerated) or a
 *   YIN_AXIS_ABSOLUTE sample (a tablet, remote desktop: x, y 0..1), a
 *   YIN_AXIS_WHEEL sample (value in notches, x the horizontal wheel). A
 *   mouse SDL does not list has YIN_UNLISTED. yin_raw_mouse_source() is
 *   its source entry (stamped at read; host part measured with injected
 *   input only).
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.4.0 - the repository conventions (README.md). Breaks the API:
 *          yin_source's bounds are int64_t ns, lo_ns and hi_ns (were
 *          int32_t us, lo_us and hi_us). yin_event.unc_us stays a uint16_t
 *          in us, the one exception to "every time is int64_t ns": an
 *          int64_t would make the event 56 bytes and ysp/screen.h's input
 *          slot 72, past one cache line (THE EVENT). USAGE builds as C99
 *          and C++17; the CHANGELOG moved to the end of this comment.
 *   v0.3.0 - For specialized devices (docs/devices_spec.md): YIN_KIND_SYNC
 *          (7) for timing events that are not a participant's response
 *          (scanner pulses, TTL inputs, photodiode and sound-key edges);
 *          unc_us, the event's own uncertainty, in the 2 bytes that were
 *          reserved_ (offset 26); ticks, the device clock's raw count, in
 *          the 4 bytes of tail padding (offset 44), valid with the new flag
 *          YIN_DEVTICKS; the eye controls YIN_EYE_* and eye codes. The
 *          size (48) and every existing offset and value are unchanged.
 *   v0.2.0 - The raw mouse report (yin_mouse_report, YIN_MOUSE_*) moved
 *          here from ysp/screen.h, so yin_from_mouse() needs no other
 *          header; ysp/screen.h's yscr_mouse_event and YSCR_MOUSE_*
 *          are its aliases. The SDL3 adapter is defined when this header
 *          is included again after SDL3, so ysp/screen.h's implementation
 *          uses it for yscr_event_input().
 *   v0.1.0 - split out of ysp/response.h v0.1.2: the event, its kinds,
 *          types, flags, controls and axes, the source entry and tiers,
 *          the SDL3 adapter and the raw mouse adapter, renamed from
 *          yrsp_ to yin_ (ysp/response.h keeps its names as aliases).
 *
 *   STATUS: v0.4.0, 2026-10-10: v0.3.0's checks, and yin_source's new
 *   layout (int64_t bounds at 8 and 16, 32 bytes); the event is still 48
 *   bytes. Measured with MinGW-w64 gcc 16.1 (x64): an int64_t unc_ns at
 *   the end would make the event 56 bytes and a ysp/screen.h input slot
 *   72 (64 now). The USAGE block builds as C99 and C++17.
 *   v0.3.0, 2026-10-08 (v0.2.0's checks, and the new fields' offsets
 *   and constants). Built with MinGW-w64 gcc 16.1 as C11, C99
 *   and C++17 under -Wall -Wextra -Wpedantic -Wshadow -Werror and with
 *   MSVC 19.44 under /W4 /WX as C11 and C++17. tests/adapt/input_test.c
 *   checks the layout (48 bytes, the field offsets producers write), the
 *   constants' values (they are in data files), and both adapters on
 *   events filled by hand; ysp/response.h's test runs every adapter case
 *   again through its aliases.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_INPUT_H_INCLUDED
#define YSP_INPUT_H_INCLUDED

#define YIN_VERSION_MAJOR 0
#define YIN_VERSION_MINOR 4
#define YIN_VERSION_PATCH 0
#define YIN_VERSION_STRING "0.4.0"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>   /* memset in the inline adapters */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum yin_kind {
    YIN_KIND_KEYBOARD = 0,  /* zero, so a zeroed choice is a key        */
    YIN_KIND_MOUSE    = 1,
    YIN_KIND_TOUCH    = 2,
    YIN_KIND_PEN      = 3,  /* drawing tablet, stylus                   */
    YIN_KIND_GAMEPAD  = 4,
    YIN_KIND_BOX      = 5,  /* response box, button box, pedal, device  */
    YIN_KIND_EYE      = 6,  /* eye tracker                              */
    YIN_KIND_SYNC     = 7,  /* a timing event, not a response: scanner
                               * pulse, TTL input, photodiode edge        */
    YIN_KIND_USER     = 8   /* 8..15: yours                             */
} yin_kind;

#define YIN_KIND_BIT(k) ((uint16_t)(1u << (k)))
#define YIN_KINDS_ALL   ((uint16_t)0xFFFFu)

typedef enum yin_type {
    YIN_NONE      = 0,      /* refused: a zeroed input is no event      */
    YIN_PRESS     = 1,      /* key, button, pen tip, contact down       */
    YIN_RELEASE   = 2,
    YIN_SAMPLE    = 3,      /* a position or an axis value              */
    YIN_PROXIMITY = 4       /* value 1 in range, 0 out                  */
} yin_type;

#define YIN_REPEAT    0x01u  /* the OS's auto-repeat of a held key   */
#define YIN_SYNTHETIC 0x02u  /* made from another device's input     */
#define YIN_ERASER    0x04u  /* pen: the eraser end                  */
#define YIN_UNLISTED  0x08u  /* a raw mouse SDL does not list        */
#define YIN_DEVTICKS  0x10u  /* ticks holds the device clock's count */

/* SAMPLE controls. Pen axes follow SDL_PenAxis (pressure 0, x tilt 1, y
 * tilt 2, distance 3, rotation 4, slider 5, tangential pressure 6);
 * gamepad axes SDL_GamepadAxis (left x 0, left y 1, right x 2, right y 3,
 * left trigger 4, right trigger 5). */
#define YIN_AXIS_POSITION     0u
#define YIN_AXIS_PEN          16u
#define YIN_AXIS_PEN_PRESSURE 16u
#define YIN_AXIS_GAMEPAD      32u
/* ysp/screen.h's raw mice (desc.raw_mice): x, y the movement in counts,
 * unaccelerated; x, y a position 0..1 on the display or the virtual
 * desktop (tablets, remote desktop); value the wheel in notches, x the
 * horizontal wheel. */
#define YIN_AXIS_DELTA        1u
#define YIN_AXIS_ABSOLUTE     2u
#define YIN_AXIS_WHEEL        3u

/* Eye trackers (kind EYE): controls, and the eye in `code`. GAZE is a
 * position, so it is YIN_AXIS_POSITION's value. */
#define YIN_EYE_GAZE          0u
#define YIN_EYE_FIXATION      1u
#define YIN_EYE_SACCADE       2u
#define YIN_EYE_BLINK         3u
#define YIN_EYE_LEFT          0u
#define YIN_EYE_RIGHT         1u
#define YIN_EYE_BOTH          2u

/* unc_us when the uncertainty is 65.5 ms or more. */
#define YIN_UNC_MAX           65535u

typedef enum yin_tier {
    YIN_TIER_UNKNOWN = 0,
    YIN_TIER_1       = 1,
    YIN_TIER_2       = 2,
    YIN_TIER_3       = 3,
    YIN_TIER_SIM     = 4
} yin_tier;

/* One timestamped input event. 48 bytes. */
typedef struct yin_event {
    int64_t  t;          /* ysp_rt ns                                      */
    uint32_t device;     /* instance within the kind; 0 = unknown          */
    uint32_t control;    /* scancode, button, contact, axis (THE EVENT)    */
    uint32_t code;       /* keyboard: SDL keycode; else yours              */
    uint16_t mods;       /* keyboard: SDL_Keymod                           */
    uint8_t  kind;       /* yin_kind                                    */
    uint8_t  type;       /* yin_type                                    */
    uint8_t  flags;      /* YIN_*                                    */
    uint8_t  stamp;      /* yin_tier; 0 = the source table's            */
    uint16_t unc_us;     /* this event's uncertainty, us (not ns: THE
                          * EVENT); 0 = the source's                     */
    float    x, y;
    float    value;
    float    aux;
    uint32_t ticks;      /* YIN_DEVTICKS: the device clock, low 32 bits  */
} yin_event;

/* How a kind (or one device of it) is stamped. Zero: UNKNOWN, no bounds. */
typedef struct yin_source {
    uint8_t     kind;
    uint8_t     tier;          /* yin_tier                              */
    uint8_t     partial;       /* 1: the bounds cover the host part only   */
    uint8_t     reserved_;
    uint32_t    device;        /* 0 = every device of the kind             */
    int64_t     lo_ns, hi_ns;  /* stamp minus event time; both 0 = unknown */
    const char* note;          /* one line for the data file               */
} yin_source;

/* The stamp tier of an SDL mouse event by its `which`: 0 is SDL's absolute
 * mode, stamped with the window message's time (tier 3); another id is a
 * Raw Input device (relative mode), the source table's. */
static inline uint8_t yin_sdl_mouse_stamp(uint32_t which) {
    return (uint8_t)(which ? 0 : YIN_TIER_3);
}

/* One raw mouse report: what a Raw Input reader (ysp/screen.h's
 * desc.raw_mice) reads for one mouse at once. 32 bytes. */
#define YIN_MOUSE_LEFT   0x01u   /* .down and .up: buttons                    */
#define YIN_MOUSE_RIGHT  0x02u
#define YIN_MOUSE_MIDDLE 0x04u
#define YIN_MOUSE_X1     0x08u
#define YIN_MOUSE_X2     0x10u
#define YIN_MOUSE_ABSOLUTE        0x01u  /* .flags: dx, dy are a position, 0..65535 */
#define YIN_MOUSE_VIRTUAL_DESKTOP 0x02u  /* ... over the virtual desktop         */
#define YIN_MOUSE_UNLISTED        0x04u  /* a device SDL does not list           */
typedef struct yin_mouse_report {
    int64_t  t;          /* ysp_rt ns: when the reader read the report      */
    uint32_t device;     /* the Raw Input handle: SDL's mouse id; 0 = injected */
    int32_t  dx, dy;     /* counts, unaccelerated; with ABSOLUTE a position */
    int16_t  wheel;      /* 120 per notch, away from the user positive      */
    int16_t  hwheel;     /* 120 per notch, right positive                   */
    uint8_t  down, up;   /* YIN_MOUSE_* pressed, released in this report  */
    uint8_t  flags;      /* YIN_MOUSE_ABSOLUTE, _VIRTUAL_DESKTOP, _UNLISTED */
    uint8_t  reserved_;
    uint32_t reserved2_;
} yin_mouse_report;

/* One raw mouse report (ysp/screen.h's desc.raw_mice) into up to 12
 * inputs, in this order: presses, releases, the movement (a DELTA or
 * ABSOLUTE sample), the wheel (a WHEEL sample). Buttons are numbered as
 * SDL's: left 1, middle 2, right 3, X1 4, X2 5. Returns how many, at most
 * cap. The stamp tier comes from the source table (yin_raw_mouse_source). */
static inline int yin_from_mouse(const yin_mouse_report* m, yin_event* out, int cap) {
    static const uint8_t bit[5] = { YIN_MOUSE_LEFT, YIN_MOUSE_MIDDLE, YIN_MOUSE_RIGHT,
                                    YIN_MOUSE_X1, YIN_MOUSE_X2 };
    yin_event in;
    int n = 0, pass, i;
    memset(&in, 0, sizeof in);
    in.t = m->t;
    in.kind = (uint8_t)YIN_KIND_MOUSE;
    in.device = m->device;
    in.flags = (uint8_t)((m->flags & YIN_MOUSE_UNLISTED) ? YIN_UNLISTED : 0u);
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < 5; i++)
            if ((pass ? m->up : m->down) & bit[i]) {
                if (n >= cap) return n;
                out[n] = in;
                out[n].type = (uint8_t)(pass ? YIN_RELEASE : YIN_PRESS);
                out[n].control = (uint32_t)(i + 1);
                out[n].value = pass ? 0.0f : 1.0f;
                n++;
            }
    if ((m->dx || m->dy || (m->flags & YIN_MOUSE_ABSOLUTE)) && n < cap) {
        out[n] = in;
        out[n].type = (uint8_t)YIN_SAMPLE;
        if (m->flags & YIN_MOUSE_ABSOLUTE) {
            out[n].control = YIN_AXIS_ABSOLUTE;
            out[n].x = (float)m->dx / 65535.0f;
            out[n].y = (float)m->dy / 65535.0f;
        } else {
            out[n].control = YIN_AXIS_DELTA;
            out[n].x = (float)m->dx;
            out[n].y = (float)m->dy;
        }
        n++;
    }
    if ((m->wheel || m->hwheel) && n < cap) {
        out[n] = in;
        out[n].type = (uint8_t)YIN_SAMPLE;
        out[n].control = YIN_AXIS_WHEEL;
        out[n].value = (float)m->wheel / 120.0f;
        out[n].x = (float)m->hwheel / 120.0f;
        n++;
    }
    return n;
}

/* The source table entry for raw mice: stamped like SDL's raw keyboard,
 * when the reader read the report; bounds not stated (measured means of
 * 0.15 to 0.40 ms after SendInput, injected input only). */
static inline yin_source yin_raw_mouse_source(void) {
    yin_source s;
    memset(&s, 0, sizeof s);
    s.kind = (uint8_t)YIN_KIND_MOUSE;
    s.tier = (uint8_t)YIN_TIER_UNKNOWN;
    s.partial = 1;
    s.note = "ysp_screen raw mice: stamped at read; host part 0.15 to 0.40 ms (means, injected); USB poll not measured";
    return s;
}




#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_INPUT_H_INCLUDED */

/* The SDL3 adapter, when SDL_events.h came first. Outside the include
 * guard: a file that includes SDL3 after this header includes it again
 * and gets the adapter then (ysp/screen.h's implementation does). */
#if defined(SDL_events_h_) && !defined(YSP_INPUT_SDL_INCLUDED)
#define YSP_INPUT_SDL_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yin_sdl_ctx {
    bool raw_keyboard;     /* SDL's raw keyboard path is on (ysp/screen.h's
                            * caps.raw_keyboard, read each frame: another
                            * library can start text input): keys get the
                            * source table's tier; off, the message path's
                            * tier 3                                      */
    bool keep_synthetic;   /* keep pen- and touch-made mouse events and
                            * pen- and mouse-made touch events, flagged   */
} yin_sdl_ctx;

/* One SDL event at t_rt (yscr_poll()'s time) into *out. Returns 1 for
 * keyboard, mouse button and motion, touch, pen and gamepad events; 0 for
 * the rest and for dropped synthetic ones. ctx may be NULL (raw keyboard
 * path, synthetic events dropped). Calls no SDL function. */
static inline int yin_from_sdl(const SDL_Event* ev, int64_t t_rt, const yin_sdl_ctx* ctx,
                                  yin_event* out) {
    yin_event in;
    bool keep = ctx && ctx->keep_synthetic;
    memset(&in, 0, sizeof in);
    in.t = t_rt;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        in.kind = (uint8_t)YIN_KIND_KEYBOARD;
        in.type = (uint8_t)(ev->key.down ? YIN_PRESS : YIN_RELEASE);
        in.device = (uint32_t)ev->key.which;
        in.control = (uint32_t)ev->key.scancode;
        in.code = (uint32_t)ev->key.key;
        in.mods = (uint16_t)ev->key.mod;
        in.flags = (uint8_t)(ev->key.repeat ? YIN_REPEAT : 0u);
        in.stamp = (uint8_t)(ctx && !ctx->raw_keyboard ? YIN_TIER_3 : 0);
        in.value = ev->key.down ? 1.0f : 0.0f;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.which == SDL_TOUCH_MOUSEID || ev->button.which == SDL_PEN_MOUSEID) {
            if (!keep) return 0;
            in.flags = (uint8_t)YIN_SYNTHETIC;
        }
        in.kind = (uint8_t)YIN_KIND_MOUSE;
        in.type = (uint8_t)(ev->button.down ? YIN_PRESS : YIN_RELEASE);
        in.device = (uint32_t)ev->button.which;
        in.stamp = yin_sdl_mouse_stamp((uint32_t)ev->button.which);
        in.control = ev->button.button;
        in.x = ev->button.x;
        in.y = ev->button.y;
        in.value = ev->button.down ? 1.0f : 0.0f;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (ev->motion.which == SDL_TOUCH_MOUSEID || ev->motion.which == SDL_PEN_MOUSEID) {
            if (!keep) return 0;
            in.flags = (uint8_t)YIN_SYNTHETIC;
        }
        in.kind = (uint8_t)YIN_KIND_MOUSE;
        in.type = (uint8_t)YIN_SAMPLE;
        in.device = (uint32_t)ev->motion.which;
        in.stamp = yin_sdl_mouse_stamp((uint32_t)ev->motion.which);
        in.control = YIN_AXIS_POSITION;
        in.x = ev->motion.x;
        in.y = ev->motion.y;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        if (ev->tfinger.touchID == SDL_PEN_TOUCHID || ev->tfinger.touchID == SDL_MOUSE_TOUCHID) {
            if (!keep) return 0;
            in.flags = (uint8_t)YIN_SYNTHETIC;
        }
        in.kind = (uint8_t)YIN_KIND_TOUCH;
        in.type = (uint8_t)(ev->type == SDL_EVENT_FINGER_DOWN ? YIN_PRESS
                            : ev->type == SDL_EVENT_FINGER_MOTION ? YIN_SAMPLE : YIN_RELEASE);
        in.device = (uint32_t)ev->tfinger.touchID;
        in.control = (uint32_t)ev->tfinger.fingerID;
        in.x = ev->tfinger.x;
        in.y = ev->tfinger.y;
        in.value = ev->tfinger.pressure;
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        in.kind = (uint8_t)YIN_KIND_PEN;
        in.type = (uint8_t)(ev->ptouch.down ? YIN_PRESS : YIN_RELEASE);
        in.device = (uint32_t)ev->ptouch.which;
        in.control = 0;
        in.flags = (uint8_t)(ev->ptouch.eraser ? YIN_ERASER : 0u);
        in.x = ev->ptouch.x;
        in.y = ev->ptouch.y;
        in.value = ev->ptouch.down ? 1.0f : 0.0f;
        break;
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
        in.kind = (uint8_t)YIN_KIND_PEN;
        in.type = (uint8_t)(ev->pbutton.down ? YIN_PRESS : YIN_RELEASE);
        in.device = (uint32_t)ev->pbutton.which;
        in.control = ev->pbutton.button;
        in.x = ev->pbutton.x;
        in.y = ev->pbutton.y;
        in.value = ev->pbutton.down ? 1.0f : 0.0f;
        break;
    case SDL_EVENT_PEN_MOTION:
        in.kind = (uint8_t)YIN_KIND_PEN;
        in.type = (uint8_t)YIN_SAMPLE;
        in.device = (uint32_t)ev->pmotion.which;
        in.control = YIN_AXIS_POSITION;
        in.x = ev->pmotion.x;
        in.y = ev->pmotion.y;
        break;
    case SDL_EVENT_PEN_AXIS:
        in.kind = (uint8_t)YIN_KIND_PEN;
        in.type = (uint8_t)YIN_SAMPLE;
        in.device = (uint32_t)ev->paxis.which;
        in.control = YIN_AXIS_PEN + (uint32_t)ev->paxis.axis;
        in.x = ev->paxis.x;
        in.y = ev->paxis.y;
        in.value = ev->paxis.value;
        break;
    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        in.kind = (uint8_t)YIN_KIND_PEN;
        in.type = (uint8_t)YIN_PROXIMITY;
        in.device = (uint32_t)ev->pproximity.which;
        in.value = ev->type == SDL_EVENT_PEN_PROXIMITY_IN ? 1.0f : 0.0f;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        in.kind = (uint8_t)YIN_KIND_GAMEPAD;
        in.type = (uint8_t)(ev->gbutton.down ? YIN_PRESS : YIN_RELEASE);
        in.device = (uint32_t)ev->gbutton.which;
        in.control = ev->gbutton.button;
        in.value = ev->gbutton.down ? 1.0f : 0.0f;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        in.kind = (uint8_t)YIN_KIND_GAMEPAD;
        in.type = (uint8_t)YIN_SAMPLE;
        in.device = (uint32_t)ev->gaxis.which;
        in.control = YIN_AXIS_GAMEPAD + ev->gaxis.axis;
        /* -32768 would map past -1; SDL's triggers run 0..32767 */
        in.value = ev->gaxis.value <= -32767 ? -1.0f : (float)ev->gaxis.value / 32767.0f;
        break;
    default:
        return 0;
    }
    *out = in;
    return 1;
}

/* The source table entry for an SDL event kind, from what was measured or
 * read (SOURCES). Windows only; elsewhere UNKNOWN with no bounds. */
static inline yin_source yin_sdl_source(const yin_sdl_ctx* ctx, int kind) {
    yin_source s;
    memset(&s, 0, sizeof s);
    s.kind = (uint8_t)kind;
#if defined(_WIN32)
    if (kind == YIN_KIND_KEYBOARD && (!ctx || ctx->raw_keyboard)) {
        s.tier = (uint8_t)YIN_TIER_UNKNOWN;
        s.partial = 1;
        s.lo_ns = 160000;
        s.hi_ns = 670000;
        s.note = "SDL 3.4 raw keyboard: host part measured with SendInput; scan and USB poll not measured";
    } else if (kind == YIN_KIND_KEYBOARD) {
        s.tier = (uint8_t)YIN_TIER_3;
        s.partial = 1;
        s.lo_ns = -6200000;
        s.hi_ns = 12100000;
        s.note = "SDL 3.4 keyboard message path: message tick, measured with SendInput";
    } else if (kind == YIN_KIND_MOUSE || kind == YIN_KIND_TOUCH || kind == YIN_KIND_PEN) {
        s.tier = (uint8_t)YIN_TIER_3;
        s.note = "SDL 3.4 window message time (ms tick), not measured";
    } else {
        s.note = "not measured";
    }
#else
    (void)ctx;
    s.note = "not measured on this platform";
#endif
    return s;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SDL_events_h_ */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
