/* Compile check: ysp/timeline.h's op table macros (SEQUENCES) as C++20.
 * They are designated initializers, which C++ has from C++20 on, and only
 * in declaration order. CMake builds this one target as C++20, and with
 * -Wno-missing-field-initializers on g++, which warns about every field a
 * designated initializer leaves out even though zero is the default. Built
 * as an earlier C++, it compiles to an empty program. */
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
static const ytl_op trial[] = {
    YTL_ON(0),
    YTL_WAIT(YRT_MS(500)),
    YTL_OFF(0), YTL_SET_VALUE(1, 2.0f), YTL_TRIGGER(12), YTL_TRIGGER_CH(1, 13), YTL_MARK(3, 7),
    YTL_TO(2, .to = 0.5f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE),
    YTL_WAIT(YRT_MS(600)),
    YTL_THEN(2, .to = 0.0f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE),
    YTL_AT(0),
};

int main() {
    static ytl_event ev[16];
    static ytl_key keys[32];
    static ytl_timeline tl;
    ytl_desc d = {};
    d.events = ev;
    d.event_capacity = 16;
    d.n_channels = 3;
    d.keys = keys;
    d.key_capacity = 32;
    if (!ytl_open(&tl, &d)) return 1;
    ytl_seq q = ytl_seq_on(&tl, 1);
    return ytl_run(&q, trial, YTL_COUNT(trial)) != 0;
}
#else
int main() { return 0; }
#endif
