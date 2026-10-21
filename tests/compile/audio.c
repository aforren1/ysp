/* Compile check: ysp/audio.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors when
 * miniaudio.h is found; audio.cpp builds the same source as C++17. It
 * opens miniaudio's null device, which needs no sound hardware, plays a
 * tone and reads its record, records from the null input alone and as
 * one duplex device, and touches each miniaudio field the strict open
 * reads, so a miniaudio release that moves one fails here. */
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#if !defined(YAU_NO_MINIAUDIO)
static int touch_fields(ma_device* d) {
    int n = (int)d->playback.internalSampleRate + (int)d->playback.internalChannels
          + (int)d->playback.internalFormat + (int)d->playback.converter.isPassthrough
          + (int)d->playback.internalChannelMap[0] + (int)d->playback.internalPeriodSizeInFrames
          + (int)d->playback.internalPeriods;
    n += (int)d->capture.internalSampleRate + (int)d->capture.internalChannels
       + (int)d->capture.internalFormat + (int)d->capture.converter.isPassthrough
       + (int)d->capture.internalChannelMap[0] + (int)d->capture.internalPeriodSizeInFrames
       + (int)d->capture.internalPeriods + (int)d->capture.shareMode + (int)d->sampleRate;
#if defined(MA_SUPPORT_WASAPI)
    n += d->wasapi.pAudioClientPlayback != NULL;
    n += (int)d->wasapi.actualBufferSizeInFramesPlayback;
    n += d->wasapi.pAudioClientCapture != NULL;
    n += (int)d->wasapi.actualBufferSizeInFramesCapture;
#endif
    return n;
}
#endif

/* v0.4.1: the web tier is the lowest, as ysp/screen.h's, and fits the
 * ring's 3-bit tier field; checked at compile time. */
typedef char web_tier_fits[(YAU_TIER_WEB == 5 && YAU_TIER_WEB > YAU_TIER_SIM && YAU_TIER_WEB <= 7) ? 1 : -1];

/* The lifecycle calls v0.4.0 changed, and v0.5.0's capture, by type. */
typedef struct au_api {
    bool (*stream_open)(yau_audio*, yau_stream*, const yau_stream_desc*);
    void (*stream_close)(yau_audio*, yau_stream*);
    bool (*wav_open)(yau_audio*, yau_wav*, const yau_wav_desc*);
    void (*wav_close)(yau_audio*, yau_wav*);
    int (*sample_at)(yau_audio*, const yau_stream*, int64_t, int64_t*);
    bool (*capture_open)(yau_capture*, const yau_capture_desc*);
    void (*capture_close)(yau_capture*);
    const float* (*capture_acquire)(yau_capture*, int, int64_t*, int64_t*, int*);
    int64_t (*capture_save)(yau_capture*, int, yau_wavw*, int64_t);
    bool (*wavw_open)(yau_wavw*, const yau_wavw_desc*);
    void (*wavw_close)(yau_wavw*);
    int (*vkey_feed)(yau_vkey*, const float*, int64_t, uint16_t, int64_t, yau_vkey_event*, int);
    int (*edge_feed)(yau_edge*, const float*, int64_t, uint16_t, int64_t, double*, int);
} au_api;

static const au_api g_api = { yau_stream_open, yau_stream_close, yau_wav_open, yau_wav_close, yau_stream_sample_at,
                              yau_capture_open, yau_capture_close, yau_capture_acquire, yau_capture_save,
                              yau_wavw_open, yau_wavw_close, yau_vkey_feed, yau_edge_feed };

#if !defined(YAU_NO_MINIAUDIO)
/* Records 50 ms from c (open): returns 0 when frames came with a fit. */
static int read_some(yau_capture* c) {
    static float buf[48000 * 2];
    int64_t got = 0, first = -1, end = yrt_now_ns() + YRT_MS(300);
    yau_capture_info in;
    while (got < 2400 && yrt_now_ns() < end) {
        int64_t n = yau_capture_read(c, 0, buf, 24000, &first);
        if (n > 0) got += n;
        yrt_sleep_until(yrt_now_ns() + YRT_MS(5), 0);
    }
    yau_capture_get_info(c, &in);
    return got >= 2400 && in.fit_ready && in.tier == YAU_TIER_SIM && yau_capture_time_of(c, first) > 0 ? 0 : 1;
}
#endif

int main(void) {
    yau_audio* au = (yau_audio*)calloc(1, sizeof(yau_audio));
    yau_desc d;
    int n = 0, rc;
    if (!au) return 1;
    memset(&d, 0, sizeof d);
#if defined(YAU_NO_MINIAUDIO)
    rc = yau_open(au, &d) ? 1 : 0;   /* must refuse: no backend */
    free(au);
    if (!g_api.stream_open || !g_api.wav_close || !g_api.sample_at || !g_api.wav_open) return 11;
    return rc == 0 && yau_params(&n) && n > 0 ? 0 : 2;
#else
    yau_tone_desc td;
    yau_buf b;
    yau_onset r;
    yau_id id;
    {
        ma_device dummy;
        memset(&dummy, 0, sizeof dummy);
        (void)touch_fields(&dummy);
    }
    d.backend = YAU_BACKEND_NULL;
    d.arena_bytes = 1 << 20;
    if (!yau_open(au, &d)) { fprintf(stderr, "%s\n", yau_error(au)); free(au); return 3; }
    memset(&td, 0, sizeof td);
    td.hz = 1000; td.dur_ns = YRT_MS(20); td.peak = 0.01f; td.ramp_ns = 0;
    b = yau_tone(au, &td);
    if (!b.frames) { yau_close(au); free(au); return 4; }
    id = yau_play_at(au, b, 0);
    if (id <= 0) { yau_close(au); free(au); return 5; }
    rc = yau_wait(au, id, 2000000000LL, &r);
    if (rc != YAU_OK || r.tier != YAU_TIER_SIM) { yau_close(au); free(au); return 6; }
    {
        /* the same tone through a stream */
        static yau_stream st;
        yau_stream_desc sd;
        yau_stream_info in;
        memset(&sd, 0, sizeof sd);
        sd.channels = 1;
        if (!yau_stream_open(au, &st, &sd)) { yau_close(au); free(au); return 8; }
        if (yau_stream_write(&st, b.frames, b.n) != b.n || yau_stream_end(&st) != 0) { yau_close(au); free(au); return 9; }
        id = yau_play_stream(au, &st, 0);
        rc = id > 0 ? yau_wait(au, id, 2000000000LL, &r) : (int)id;
        if (rc != YAU_OK || r.sample != 0 || yau_stream_get_info(&st, &in) != 0 || !in.started) {
            yau_close(au); free(au); return 10;
        }
        g_api.stream_close(au, &st);
        g_api.stream_close(au, &st);
    }
    yau_close(au);
    {
        /* the null input alone, then as a duplex device with the output */
        static yau_capture cap;
        yau_capture_desc cd;
        memset(&cd, 0, sizeof cd);
        cd.backend = YAU_BACKEND_NULL;
        if (!g_api.capture_open(&cap, &cd)) { fprintf(stderr, "%s\n", yau_capture_error(&cap)); free(au); return 12; }
        rc = read_some(&cap);
        g_api.capture_close(&cap);
        g_api.capture_close(&cap);
        if (rc) { free(au); return 13; }
        d.capture = &cap;
        d.capture_desc = &cd;
        if (!yau_open(au, &d)) { fprintf(stderr, "%s\n", yau_error(au)); free(au); return 14; }
        rc = read_some(&cap);
        yau_capture_close(&cap);   /* the duplex device's: nothing */
        if (rc || !yau_capture_is_open(&cap)) { yau_close(au); free(au); return 15; }
        yau_close(au);
        if (yau_capture_is_open(&cap)) { free(au); return 16; }
    }
    free(au);
    {   /* v0.7.0: values, the last field; set on the enum rows only */
        const yau_param* t = yau_params(&n);
        int i, k = 0;
        for (i = 0; t && i < n; i++) k += (t[i].values != NULL) == !strcmp(t[i].type, "enum");
        if (!t || k != n) return 17;
    }
    return yau_params(&n) && n > 0 ? 0 : 7;
#endif
}
