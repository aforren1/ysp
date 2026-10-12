/* pulse_timing_probe.c - what PulseAudio's stream timing info is worth as a
 * device position for ysp/audio.h, measured.
 *
 * Opens miniaudio's PulseAudio backend (16-bit, 48 kHz, stereo, silence),
 * and in every data callback asks the server for timing info
 * (pa_stream_update_timing_info) and reads the last reply
 * (pa_stream_get_timing_info). Each new reply is one CSV line:
 *   cb_ns,ts_ns,read_index,write_index,sink_usec,transport_usec,playing,written
 * cb_ns the callback's CLOCK_MONOTONIC time; ts_ns the reply's timestamp
 * (CLOCK_REALTIME, mapped to CLOCK_MONOTONIC with the offset read beside
 * it); indexes in bytes; written the frames handed to miniaudio so far.
 *
 *   cc -O2 -I<miniaudio dir> -o pulse_timing_probe pulse_timing_probe.c -ldl -lpthread -lm
 *   ./pulse_timing_probe SECONDS out.csv [period_frames]
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <dlfcn.h>
#include <pulse/def.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef void* (*update_fn)(void* s, void* cb, void* ud);
typedef const pa_timing_info* (*get_fn)(void* s);
typedef void (*unref_fn)(void* op);

static update_fn g_update;
static get_fn g_get;
static unref_fn g_unref;
static FILE* g_f;
static long long g_written, g_last_sec = -1, g_last_usec = -1, g_rows;

static long long ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void on_data(ma_device* d, void* out, const void* in, ma_uint32 n) {
    long long cb = ns(CLOCK_MONOTONIC);
    void* s = d->pulse.pStreamPlayback;
    (void)in;
    memset(out, 0, (size_t)n * 4);
    if (s && g_update && g_get) {
        void* op = g_update(s, NULL, NULL);
        const pa_timing_info* ti;
        if (op) g_unref(op);
        ti = g_get(s);
        if (ti && (ti->timestamp.tv_sec != g_last_sec || ti->timestamp.tv_usec != g_last_usec)) {
            long long off = ns(CLOCK_REALTIME) - ns(CLOCK_MONOTONIC);
            long long ts = (long long)ti->timestamp.tv_sec * 1000000000LL + (long long)ti->timestamp.tv_usec * 1000LL - off;
            g_last_sec = ti->timestamp.tv_sec;
            g_last_usec = ti->timestamp.tv_usec;
            fprintf(g_f, "%lld,%lld,%lld,%lld,%llu,%llu,%d,%lld\n", cb, ts, (long long)ti->read_index,
                    (long long)ti->write_index, (unsigned long long)ti->sink_usec,
                    (unsigned long long)ti->transport_usec, ti->playing, g_written);
            g_rows++;
        }
    }
    g_written += n;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: pulse_timing_probe SECONDS out.csv [period_frames]\n"); return 2; }
    ma_backend be = ma_backend_pulseaudio;
    ma_context ctx;
    if (ma_context_init(&be, 1, NULL, &ctx) != MA_SUCCESS) { fprintf(stderr, "no PulseAudio\n"); return 1; }
    g_update = (update_fn)dlsym(ctx.pulse.pulseSO, "pa_stream_update_timing_info");
    g_get = (get_fn)dlsym(ctx.pulse.pulseSO, "pa_stream_get_timing_info");
    g_unref = (unref_fn)dlsym(ctx.pulse.pulseSO, "pa_operation_unref");
    if (!g_update || !g_get || !g_unref) { fprintf(stderr, "libpulse symbols missing\n"); return 1; }
    g_f = fopen(argv[2], "w");
    if (!g_f) { perror(argv[2]); return 1; }
    fprintf(g_f, "cb_ns,ts_ns,read_index,write_index,sink_usec,transport_usec,playing,written\n");
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_s16;
    dc.playback.channels = 2;
    dc.sampleRate = 48000;
    dc.periodSizeInFrames = argc > 3 ? (ma_uint32)atoi(argv[3]) : 480;
    dc.dataCallback = on_data;
    ma_device dev;
    if (ma_device_init(&ctx, &dc, &dev) != MA_SUCCESS) { fprintf(stderr, "device init failed\n"); return 1; }
    ma_device_start(&dev);
    struct timespec t = { atoi(argv[1]), 0 };
    nanosleep(&t, NULL);
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    fclose(g_f);
    fprintf(stderr, "pulse_timing_probe: %lld replies, %lld frames written, period %u\n", g_rows, g_written,
            dc.periodSizeInFrames);
    return 0;
}
