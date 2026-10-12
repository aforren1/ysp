// Compile check: ysp/audio.h's own declaration of libpulse's pa_timing_info
// (the PulseAudio position source), against <pulse/def.h>: the size and
// every field's offset. Without PulseAudio's headers, or off Linux, or
// without miniaudio's PulseAudio support, it returns 0.
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <cstddef>
#include <cstdio>

#if defined(__has_include)
#if __has_include(<pulse/def.h>)
#include <pulse/def.h>
#define HAVE_PULSE_DEF 1
#endif
#endif

#if defined(HAVE_PULSE_DEF) && !defined(YAU_NO_MINIAUDIO) && defined(MA_SUPPORT_PULSEAUDIO)
static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "audio_pulse: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)
#define SAME(f) CHECK(offsetof(yau__pa_timing_info, f) == offsetof(pa_timing_info, f))

int main() {
    CHECK(sizeof(yau__pa_timing_info) == sizeof(pa_timing_info));
    SAME(timestamp);
    SAME(synchronized_clocks);
    SAME(sink_usec);
    SAME(source_usec);
    SAME(transport_usec);
    SAME(playing);
    SAME(write_index_corrupt);
    SAME(write_index);
    SAME(read_index_corrupt);
    SAME(read_index);
    SAME(configured_sink_usec);
    SAME(configured_source_usec);
    SAME(since_underrun);
    if (g_failures) return 1;
    std::printf("audio_pulse: yau__pa_timing_info matches <pulse/def.h>\n");
    return 0;
}
#else
int main() { return 0; }
#endif
