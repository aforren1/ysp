/* alsa_status_probe.c - what ALSA's PCM status is worth as a device position
 * for ysp/audio.h in exclusive mode (hw:), measured.
 *
 * Opens miniaudio's ALSA backend exclusively on a hw device (16-bit,
 * 48 kHz, stereo, silence) and in every data callback, before rendering,
 * reads snd_pcm_status(): the delay (frames from the one playing to the
 * last one written) and the status timestamp. One CSV line per callback:
 *   cb_ns,ts_ns,delay,avail,written,state,ts_real
 * cb_ns CLOCK_MONOTONIC at the callback; ts_ns the status timestamp mapped
 * to CLOCK_MONOTONIC (ts_real 1: it was CLOCK_REALTIME, the default
 * timestamp type); written the frames handed to ALSA before this callback.
 * PulseAudio holds the card, so run it as `pasuspender -- ./alsa_status_probe`.
 *
 *   cc -O2 -I<miniaudio dir> -o alsa_status_probe alsa_status_probe.c -ldl -lpthread -lm
 *   pasuspender -- ./alsa_status_probe SECONDS out.csv [period_frames] [device]
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef int (*status_fn)(void* pcm, void* st);
typedef size_t (*sizeof_fn)(void);
typedef long (*get_delay_fn)(const void* st);
typedef unsigned long (*get_avail_fn)(const void* st);
typedef void (*get_ts_fn)(const void* st, struct timespec* ts);
typedef int (*get_state_fn)(const void* st);
typedef int (*sw_cur_fn)(void* pcm, void* sw);
typedef int (*sw_tstamp_type_fn)(void* pcm, void* sw, int type);
typedef int (*sw_tstamp_mode_fn)(void* pcm, void* sw, int mode);
typedef int (*sw_apply_fn)(void* pcm, void* sw);
typedef size_t (*sw_sizeof_fn)(void);

static status_fn g_status;
static get_delay_fn g_delay;
static get_avail_fn g_avail;
static get_ts_fn g_ts;
static get_state_fn g_state;
static unsigned char g_st[1024];
static FILE* g_f;
static long long g_written, g_rows;
static int g_ts_real = 1;

static long long ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void on_data(ma_device* d, void* out, const void* in, ma_uint32 n) {
    long long cb = ns(CLOCK_MONOTONIC);
    (void)in;
    memset(out, 0, (size_t)n * 4);
    if (g_status && g_status(d->alsa.pPCMPlayback, g_st) == 0) {
        struct timespec ts;
        long long t, off = g_ts_real ? ns(CLOCK_REALTIME) - ns(CLOCK_MONOTONIC) : 0;
        g_ts(g_st, &ts);
        t = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec - off;
        fprintf(g_f, "%lld,%lld,%ld,%lu,%lld,%d,%d\n", cb, t, g_delay(g_st), g_avail(g_st), g_written, g_state(g_st), g_ts_real);
        g_rows++;
    }
    g_written += n;
}

#define SYM(h, name, var) do { *(void**)(&(var)) = dlsym((h), (name)); if (!(var)) { fprintf(stderr, "no %s\n", name); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: alsa_status_probe SECONDS out.csv [period_frames] [device]\n"); return 2; }
    ma_backend be = ma_backend_alsa;
    ma_context ctx;
    if (ma_context_init(&be, 1, NULL, &ctx) != MA_SUCCESS) { fprintf(stderr, "no ALSA\n"); return 1; }
    void* h = ctx.alsa.asoundSO;
    sizeof_fn st_sizeof;
    SYM(h, "snd_pcm_status", g_status);
    SYM(h, "snd_pcm_status_sizeof", st_sizeof);
    SYM(h, "snd_pcm_status_get_delay", g_delay);
    SYM(h, "snd_pcm_status_get_avail", g_avail);
    SYM(h, "snd_pcm_status_get_htstamp", g_ts);
    SYM(h, "snd_pcm_status_get_state", g_state);
    if (st_sizeof() > sizeof g_st) { fprintf(stderr, "status too big\n"); return 1; }
    g_f = fopen(argv[2], "w");
    if (!g_f) { perror(argv[2]); return 1; }
    fprintf(g_f, "cb_ns,ts_ns,delay,avail,written,state,ts_real\n");
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    ma_device_id id;
    memset(&id, 0, sizeof id);
    snprintf(id.alsa, sizeof id.alsa, "%s", argc > 4 ? argv[4] : "hw:0,0");
    dc.playback.pDeviceID = &id;
    dc.playback.shareMode = ma_share_mode_exclusive;
    dc.playback.format = ma_format_s16;
    dc.playback.channels = 2;
    dc.sampleRate = 48000;
    dc.periodSizeInFrames = argc > 3 ? (ma_uint32)atoi(argv[3]) : 240;
    dc.dataCallback = on_data;
    ma_device dev;
    if (ma_device_init(&ctx, &dc, &dev) != MA_SUCCESS) { fprintf(stderr, "device init failed (PulseAudio holding the card? run under pasuspender)\n"); return 1; }
    {
        /* status timestamps are 0 unless the mode is ENABLE (1); ask for
         * CLOCK_MONOTONIC ones: SND_PCM_TSTAMP_TYPE_MONOTONIC = 1 */
        sw_cur_fn cur; sw_tstamp_type_fn tt; sw_tstamp_mode_fn tm; sw_apply_fn apply; sw_sizeof_fn sws;
        static unsigned char sw[512];
        SYM(h, "snd_pcm_sw_params_current", cur);
        SYM(h, "snd_pcm_sw_params_set_tstamp_type", tt);
        SYM(h, "snd_pcm_sw_params_set_tstamp_mode", tm);
        SYM(h, "snd_pcm_sw_params", apply);
        SYM(h, "snd_pcm_sw_params_sizeof", sws);
        if (sws() <= sizeof sw && cur(dev.alsa.pPCMPlayback, sw) == 0 && tm(dev.alsa.pPCMPlayback, sw, 1) == 0 &&
            tt(dev.alsa.pPCMPlayback, sw, 1) == 0 && apply(dev.alsa.pPCMPlayback, sw) == 0)
            g_ts_real = 0;
    }
    fprintf(stderr, "device %s, period %u x %u, %s stamps\n", dev.playback.name, dev.playback.internalPeriodSizeInFrames,
            dev.playback.internalPeriods, g_ts_real ? "realtime" : "monotonic");
    ma_device_start(&dev);
    struct timespec t = { atoi(argv[1]), 0 };
    nanosleep(&t, NULL);
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    fclose(g_f);
    fprintf(stderr, "alsa_status_probe: %lld rows, %lld frames written\n", g_rows, g_written);
    return 0;
}
