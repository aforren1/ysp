/* Compile check: ysp/input.h as a C translation unit. CMake and CI build
 * this as C11 with warnings as errors; input.cpp builds the same
 * source as C++17. The header has no implementation and no threads. It
 * fills one event and returns 0 to prove it builds and runs. */
#include "ysp/input.h"

#include <string.h>

int main(void) {
    yin_event e;
    yin_source s;
    memset(&e, 0, sizeof(e));
    e.kind = YIN_KIND_BOX;
    e.type = YIN_PRESS;
    e.control = 1;
    e.value = 1.0f;
    s = yin_raw_mouse_source();
    s.lo_ns = -160000;          /* v0.4.0: int64_t ns bounds */
    s.hi_ns = 670000;
    e.unc_us = 1;               /* the one time in us: THE EVENT */
    return sizeof(e) == 48 && s.hi_ns - s.lo_ns == 830000 && yin_sdl_mouse_stamp(0) == YIN_TIER_3 ? 0 : 1;
}
