/* pulse_record_probe.c - what PulseAudio's record stream timing info is
 * worth as a capture position for ysp/audio.h, measured.
 *
 * Opens miniaudio's PulseAudio backend for capture (16-bit, the source's
 * rate, stereo) on a source by name (a sink's monitor needs no cable), and
 * in every data callback asks the server for timing info and reads the
 * last reply. Each new reply is one CSV line:
 *   cb_ns,ts_ns,read_index,write_index,source_usec,transport_usec,read,n,rate
 * cb_ns the callback's CLOCK_MONOTONIC time; ts_ns the reply's timestamp
 * (CLOCK_REALTIME, mapped to CLOCK_MONOTONIC with the offset read beside
 * it); indexes in bytes; read the frames handed to the callbacks before
 * this one; n this block.
 *
 *   cc -O2 -I<miniaudio dir> -o pulse_record_probe pulse_record_probe.c -ldl -lpthread -lm
 *   ./pulse_record_probe SECONDS out.csv SOURCE [period_frames] [load]
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <dlfcn.h>
#include <pthread.h>
#include <pulse/def.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void* (*update_fn)(void* s, void* cb, void* ud);
typedef const pa_timing_info* (*get_fn)(void* s);
typedef void (*unref_fn)(void* op);

static update_fn g_update;
static get_fn g_get;
static unref_fn g_unref;
static FILE* g_f;
static long long g_read, g_last_sec = -1, g_last_usec = -1, g_rows;
static volatile int g_stop;

static long long ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void on_data(ma_device* d, void* out, const void* in, ma_uint32 n) {
    long long cb = ns(CLOCK_MONOTONIC);
    void* s = d->pulse.pStreamCapture;
    (void)in; (void)out;
    if (s) {
        void* op = g_update(s, NULL, NULL);
        const pa_timing_info* ti;
        if (op) g_unref(op);
        ti = g_get(s);
        if (ti && (ti->timestamp.tv_sec != g_last_sec || ti->timestamp.tv_usec != g_last_usec)) {
            long long off = ns(CLOCK_REALTIME) - ns(CLOCK_MONOTONIC);
            long long ts = (long long)ti->timestamp.tv_sec * 1000000000LL + (long long)ti->timestamp.tv_usec * 1000LL - off;
            g_last_sec = ti->timestamp.tv_sec;
            g_last_usec = ti->timestamp.tv_usec;
            fprintf(g_f, "%lld,%lld,%lld,%lld,%llu,%llu,%lld,%u,%u\n", cb, ts, (long long)ti->read_index,
                    (long long)ti->write_index, (unsigned long long)ti->source_usec,
                    (unsigned long long)ti->transport_usec, g_read, n, d->capture.internalSampleRate);
            g_rows++;
        }
    }
    g_read += n;
}

static void* spin(void* p) {
    volatile unsigned long long x = 0;
    (void)p;
    while (!g_stop) x++;
    return NULL;
}

int main(int argc, char** argv) {
    ma_backend be = ma_backend_pulseaudio;
    ma_context ctx;
    ma_device_config dc;
    ma_device_id id;
    ma_device dev;
    pthread_t th[64];
    int nload = argc > 5 ? atoi(argv[5]) : 0, i;
    if (argc < 4) { fprintf(stderr, "usage: pulse_record_probe SECONDS out.csv SOURCE [period_frames] [load]\n"); return 2; }
    if (nload > 64) nload = 64;
    if (ma_context_init(&be, 1, NULL, &ctx) != MA_SUCCESS) { fprintf(stderr, "no PulseAudio\n"); return 1; }
    g_update = (update_fn)dlsym(ctx.pulse.pulseSO, "pa_stream_update_timing_info");
    g_get = (get_fn)dlsym(ctx.pulse.pulseSO, "pa_stream_get_timing_info");
    g_unref = (unref_fn)dlsym(ctx.pulse.pulseSO, "pa_operation_unref");
    if (!g_update || !g_get || !g_unref) { fprintf(stderr, "libpulse symbols missing\n"); return 1; }
    g_f = fopen(argv[2], "w");
    if (!g_f) { perror(argv[2]); return 1; }
    fprintf(g_f, "cb_ns,ts_ns,read_index,write_index,source_usec,transport_usec,read,n,rate\n");
    dc = ma_device_config_init(ma_device_type_capture);
    memset(&id, 0, sizeof id);
    snprintf(id.pulse, sizeof id.pulse, "%s", argv[3]);
    dc.capture.pDeviceID = &id;
    dc.capture.format = ma_format_s16;
    dc.capture.channels = 2;
    dc.sampleRate = 0;
    dc.periodSizeInFrames = argc > 4 ? (ma_uint32)atoi(argv[4]) : 480;
    dc.noFixedSizedCallback = MA_TRUE;
    dc.dataCallback = on_data;
    if (ma_device_init(&ctx, &dc, &dev) != MA_SUCCESS) { fprintf(stderr, "device init failed\n"); return 1; }
    fprintf(stderr, "source %s, %u Hz, period %u\n", dev.capture.name, dev.capture.internalSampleRate,
            dev.capture.internalPeriodSizeInFrames);
    for (i = 0; i < nload; i++) pthread_create(&th[i], NULL, spin, NULL);
    ma_device_start(&dev);
    {
        struct timespec t = { atoi(argv[1]), 0 };
        nanosleep(&t, NULL);
    }
    ma_device_uninit(&dev);
    g_stop = 1;
    for (i = 0; i < nload; i++) pthread_join(th[i], NULL);
    ma_context_uninit(&ctx);
    fclose(g_f);
    fprintf(stderr, "pulse_record_probe: %lld replies, %lld frames read\n", g_rows, g_read);
    return 0;
}
