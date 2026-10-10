/* Compile check: ysp/audio.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors when
 * miniaudio.h is found; audio.cpp builds the same source as C++17. It
 * opens miniaudio's null device, which needs no sound hardware, plays a
 * tone and reads its record, and touches each miniaudio field the strict
 * open reads, so a miniaudio release that moves one fails here. */
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#if !defined(YAU_NO_MINIAUDIO)
static int touch_fields(ma_device* d) {
    int n = (int)d->playback.internalSampleRate + (int)d->playback.internalChannels
          + (int)d->playback.internalFormat + (int)d->playback.converter.isPassthrough
          + (int)d->playback.internalChannelMap[0] + (int)d->playback.internalPeriodSizeInFrames
          + (int)d->playback.internalPeriods;
#if defined(MA_SUPPORT_WASAPI)
    n += d->wasapi.pAudioClientPlayback != NULL;
    n += (int)d->wasapi.actualBufferSizeInFramesPlayback;
#endif
    return n;
}
#endif

/* The lifecycle calls v0.4.0 changed, by type. */
typedef struct au_api {
    bool (*stream_open)(yau_audio*, yau_stream*, const yau_stream_desc*);
    void (*stream_close)(yau_audio*, yau_stream*);
    bool (*wav_open)(yau_audio*, yau_wav*, const yau_wav_desc*);
    void (*wav_close)(yau_audio*, yau_wav*);
    int (*sample_at)(yau_audio*, const yau_stream*, int64_t, int64_t*);
} au_api;

static const au_api g_api = { yau_stream_open, yau_stream_close, yau_wav_open, yau_wav_close, yau_stream_sample_at };

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
    free(au);
    return yau_params(&n) && n > 0 ? 0 : 7;
#endif
}
