// Compile check: ysp/screen.h's own declarations for the GLX_OML backend,
// against the system's Xlib, GLX and xcb headers.
//
// The header declares the few constants, struct offsets and entry point
// types it uses, so it compiles without the X11 and GL development
// packages. Here, where those packages exist, each constant is compared
// with the header's, the PresentCompleteNotify field offsets with
// xcb_present_complete_notify_event_t (a packed struct, read by offset),
// the reply offsets with xcb's reply structs, the cookie with xcb's, and
// the XKeyEvent fields the X11 key-time hook reads.
// A wrong value is a failure here, not a wrong onset on a rig.
//
// Off Linux, or without the headers, it does nothing and returns 0.
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stddef.h>

#if defined(YSCR__GLX) && __has_include(<GL/glx.h>) && __has_include(<GL/glxext.h>) && \
    __has_include(<xcb/present.h>) && __has_include(<X11/Xlib.h>)
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <xcb/xcb.h>
#include <xcb/present.h>

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "screen_glx: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

int main() {
    CHECK(YSCR__GLX_DOUBLEBUFFER == GLX_DOUBLEBUFFER);
    CHECK(YSCR__GLX_RED_SIZE == GLX_RED_SIZE);
    CHECK(YSCR__GLX_GREEN_SIZE == GLX_GREEN_SIZE);
    CHECK(YSCR__GLX_BLUE_SIZE == GLX_BLUE_SIZE);
    CHECK(YSCR__GLX_DEPTH_SIZE == GLX_DEPTH_SIZE);
    CHECK(YSCR__GLX_STENCIL_SIZE == GLX_STENCIL_SIZE);
    CHECK(YSCR__GLX_SAMPLES == GLX_SAMPLES);
    CHECK(YSCR__GLX_VISUAL_ID == GLX_VISUAL_ID);
    CHECK(YSCR__GLX_DRAWABLE_TYPE == GLX_DRAWABLE_TYPE);
    CHECK(YSCR__GLX_RENDER_TYPE == GLX_RENDER_TYPE);
    CHECK(YSCR__GLX_WINDOW_BIT == GLX_WINDOW_BIT);
    CHECK(YSCR__GLX_RGBA_BIT == GLX_RGBA_BIT);
    CHECK(YSCR__GLX_CONTEXT_MAJOR_VERSION == GLX_CONTEXT_MAJOR_VERSION_ARB);
    CHECK(YSCR__GLX_CONTEXT_MINOR_VERSION == GLX_CONTEXT_MINOR_VERSION_ARB);
    CHECK(YSCR__GLX_CONTEXT_PROFILE_MASK == GLX_CONTEXT_PROFILE_MASK_ARB);
    CHECK(YSCR__GLX_CONTEXT_ES2_PROFILE == GLX_CONTEXT_ES2_PROFILE_BIT_EXT);
    CHECK(YSCR__GLX_SWAP_INTERVAL == GLX_SWAP_INTERVAL_EXT);
    CHECK(YSCR__GL_RENDERER == GL_RENDERER);

    CHECK(YSCR__PRESENT_MASK_COMPLETE == XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    CHECK(YSCR__PRESENT_COMPLETE_NOTIFY == XCB_PRESENT_COMPLETE_NOTIFY);
    CHECK(YSCR__PRESENT_KIND_PIXMAP == XCB_PRESENT_COMPLETE_KIND_PIXMAP);
    CHECK(YSCR__PRESENT_MODE_COPY == XCB_PRESENT_COMPLETE_MODE_COPY);
    CHECK(YSCR__PRESENT_MODE_FLIP == XCB_PRESENT_COMPLETE_MODE_FLIP);
    CHECK(YSCR__PRESENT_MODE_SKIP == XCB_PRESENT_COMPLETE_MODE_SKIP);
    CHECK(XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY == 3);   // n_mode[3] in the describe line
    CHECK(YSCR__XCB_ATOM_CARDINAL == XCB_ATOM_CARDINAL);

    CHECK(YSCR__PEV_TYPE == offsetof(xcb_present_complete_notify_event_t, event_type));
    CHECK(YSCR__PEV_KIND == offsetof(xcb_present_complete_notify_event_t, kind));
    CHECK(YSCR__PEV_MODE == offsetof(xcb_present_complete_notify_event_t, mode));
    CHECK(YSCR__PEV_SERIAL == offsetof(xcb_present_complete_notify_event_t, serial));
    CHECK(YSCR__PEV_UST == offsetof(xcb_present_complete_notify_event_t, ust));
    CHECK(YSCR__PEV_MSC == offsetof(xcb_present_complete_notify_event_t, msc));

    // the reply fields the presenter reads at offset 8, 16 and 32
    CHECK(offsetof(xcb_intern_atom_reply_t, atom) == 8);
    CHECK(offsetof(xcb_get_selection_owner_reply_t, owner) == 8);
    CHECK(offsetof(xcb_get_window_attributes_reply_t, visual) == 8);
    CHECK(offsetof(xcb_get_window_attributes_reply_t, override_redirect) == 27);
    CHECK(offsetof(xcb_query_extension_reply_t, present) == 8);
    CHECK(offsetof(xcb_get_property_reply_t, value_len) == 16);
    CHECK(sizeof(xcb_get_property_reply_t) == 32);   // the value follows the reply

    // the cookie is returned by value: same size and member as xcb's
    CHECK(sizeof(yscr__xcb_cookie) == sizeof(xcb_void_cookie_t));
    CHECK(sizeof(yscr__xcb_cookie) == sizeof(xcb_intern_atom_cookie_t));
    CHECK(offsetof(yscr__xcb_cookie, sequence) == offsetof(xcb_void_cookie_t, sequence));
    CHECK(sizeof(yscr__XID) == sizeof(XID));

    // the key fields yscr__xk_hook reads from SDL's XEvent
    CHECK(KeyPress == 2 && KeyRelease == 3);
    CHECK(offsetof(XKeyEvent, type) == 0);
    CHECK(offsetof(XKeyEvent, time) == 56 && sizeof(Time) == sizeof(unsigned long));
    CHECK(offsetof(XKeyEvent, keycode) == 84 && sizeof(((XKeyEvent*)0)->keycode) == sizeof(unsigned int));

    if (g_failures) return 1;
    printf("screen_glx: all GLX, Present and xcb declarations match the system headers\n");
    return 0;
}

#else
int main() { return 0; }
#endif
