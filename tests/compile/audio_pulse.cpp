// Compile check: ysp/audio.h's own declarations of libpulse's structs,
// against PulseAudio's headers: pa_timing_info (the PulseAudio position
// source), its size and every field's offset, and the leading fields of
// pa_sink_info and pa_source_info that yau__pa_dev_info mirrors (the card
// of a sink or source, and whether a source is a monitor). Without
// PulseAudio's headers, or off Linux, or without miniaudio's PulseAudio
// support, it returns 0.
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <cstddef>
#include <cstdio>

#if defined(__has_include)
#if __has_include(<pulse/def.h>) && __has_include(<pulse/introspect.h>)
#include <pulse/def.h>
#include <pulse/introspect.h>
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
#define DEV(t, f, g) CHECK(offsetof(yau__pa_dev_info, f) == offsetof(t, g))
    DEV(pa_sink_info, name, name);
    DEV(pa_sink_info, index, index);
    DEV(pa_sink_info, description, description);
    DEV(pa_sink_info, sample_spec, sample_spec);
    DEV(pa_sink_info, channel_map, channel_map);
    DEV(pa_sink_info, owner_module, owner_module);
    DEV(pa_sink_info, volume, volume);
    DEV(pa_sink_info, mute, mute);
    DEV(pa_sink_info, monitor, monitor_source);
    DEV(pa_sink_info, monitor_name, monitor_source_name);
    DEV(pa_sink_info, latency, latency);
    DEV(pa_sink_info, driver, driver);
    DEV(pa_sink_info, flags, flags);
    DEV(pa_sink_info, proplist, proplist);
    DEV(pa_source_info, index, index);
    DEV(pa_source_info, monitor, monitor_of_sink);
    DEV(pa_source_info, monitor_name, monitor_of_sink_name);
    DEV(pa_source_info, proplist, proplist);
    CHECK(sizeof(((yau__pa_dev_info*)0)->sample_spec) == sizeof(pa_sample_spec));
    CHECK(sizeof(((yau__pa_dev_info*)0)->channel_map) == sizeof(pa_channel_map));
    CHECK(sizeof(((yau__pa_dev_info*)0)->volume) == sizeof(pa_cvolume));
    if (g_failures) return 1;
    std::printf("audio_pulse: yau__pa_timing_info and yau__pa_dev_info match PulseAudio's headers\n");
    return 0;
}
#else
int main() { return 0; }
#endif
