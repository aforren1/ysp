/* alsa_capture_probe.c - what ALSA's PCM status is worth as a capture
 * position for ysp/audio.h on a hw: device, measured.
 *
 * Opens miniaudio's ALSA backend exclusively on a hw: capture device
 * (32-bit, 48 kHz, stereo) and in every data callback reads
 * snd_pcm_status(). One CSV line per callback:
 *   cb_ns,ts_ns,trig_ns,delay,avail,read,n,state,audio_ns,audio_type
 * cb_ns CLOCK_MONOTONIC at the callback; ts_ns the status timestamp; trig_ns
 * the trigger timestamp (the last start); read the frames handed to the
 * callbacks before this one; n this block. With "link" as the mode, the
 * timestamps are CLOCK_MONOTONIC_RAW (the only types the kernel allows with
 * a link-synchronized audio stamp) mapped here to CLOCK_MONOTONIC, and
 * audio_ns is the audio timestamp the driver gives with them (audio_type
 * the type it reports: 5 is LINK_SYNCHRONIZED). Otherwise audio_ns is 0.
 * With --load N, N threads spin at normal priority.
 *
 *   cc -O2 -I<miniaudio dir> -o alsa_capture_probe alsa_capture_probe.c -ldl -lpthread -lm
 *   ./alsa_capture_probe SECONDS out.csv [period] [periods] [device] [mode] [load]
 *
 * mode: "status" (default) or "link". The card must be free: PulseAudio
 * holds a capture PCM only in an input or duplex profile.
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int (*status_fn)(void* pcm, void* st);
typedef size_t (*sizeof_fn)(void);
typedef long (*get_delay_fn)(const void* st);
typedef unsigned long (*get_avail_fn)(const void* st);
typedef void (*get_ts_fn)(const void* st, struct timespec* ts);
typedef int (*get_state_fn)(const void* st);
typedef int (*sw_cur_fn)(void* pcm, void* sw);
typedef int (*sw_set_fn)(void* pcm, void* sw, int v);
typedef int (*sw_apply_fn)(void* pcm, void* sw);
typedef void (*set_cfg_fn)(void* st, void* cfg);
typedef void (*get_rep_fn)(const void* st, void* rep);

/* snd_pcm_audio_tstamp_config_t and _report_t: bit fields in one word */
typedef struct { unsigned int type_requested : 4; unsigned int report_delay : 1; } cfg_t;
typedef struct { unsigned int valid : 1; unsigned int actual_type : 4; unsigned int accuracy_report : 1;
                 unsigned int accuracy; } rep_t;

static status_fn g_status;
static get_delay_fn g_delay;
static get_avail_fn g_avail;
static get_ts_fn g_ts, g_trig, g_audio;
static get_state_fn g_state;
static set_cfg_fn g_set_cfg;
static get_rep_fn g_rep;
static unsigned char g_st[1024];
static FILE* g_f;
static long long g_read, g_rows;
static int g_link;
static volatile int g_stop;

static long long ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static long long tsns(const struct timespec* t) { return (long long)t->tv_sec * 1000000000LL + t->tv_nsec; }

static void on_data(ma_device* d, void* out, const void* in, ma_uint32 n) {
    long long cb = ns(CLOCK_MONOTONIC);
    (void)out; (void)in;
    if (g_link) {
        cfg_t cfg = { 5, 0 };   /* SND_PCM_AUDIO_TSTAMP_TYPE_LINK_SYNCHRONIZED */
        g_set_cfg(g_st, &cfg);
    }
    if (g_status(d->alsa.pPCMCapture, g_st) == 0) {
        struct timespec ts, tr, au;
        long long off = 0, t, trig;
        rep_t rep;
        memset(&rep, 0, sizeof rep);
        if (g_link) {
            /* RAW less MONOTONIC, from a bracketed read */
            long long m0 = ns(CLOCK_MONOTONIC), r = ns(CLOCK_MONOTONIC_RAW), m1 = ns(CLOCK_MONOTONIC);
            off = r - (m0 + m1) / 2;
        }
        g_ts(g_st, &ts);
        g_trig(g_st, &tr);
        g_audio(g_st, &au);
        if (g_rep) g_rep(g_st, &rep);
        t = tsns(&ts) - off;
        trig = tsns(&tr) - off;
        fprintf(g_f, "%lld,%lld,%lld,%ld,%lu,%lld,%u,%d,%lld,%u\n", cb, t, trig, g_delay(g_st), g_avail(g_st), g_read, n,
                g_state(g_st), g_link ? tsns(&au) : 0LL, g_link ? rep.actual_type : 0u);
        g_rows++;
    }
    g_read += n;
}

static void* spin(void* p) {
    volatile unsigned long long x = 0;
    (void)p;
    while (!g_stop) x++;
    return NULL;
}

#define SYM(h, name, var) do { *(void**)(&(var)) = dlsym((h), (name)); if (!(var)) { fprintf(stderr, "no %s\n", name); return 1; } } while (0)

int main(int argc, char** argv) {
    ma_backend be = ma_backend_alsa;
    ma_context ctx;
    ma_device_config dc;
    ma_device_id id;
    ma_device dev;
    sizeof_fn st_sizeof;
    pthread_t th[64];
    int nload = argc > 7 ? atoi(argv[7]) : 0, i;
    void* h;
    if (argc < 3) {
        fprintf(stderr, "usage: alsa_capture_probe SECONDS out.csv [period] [periods] [device] [status|link] [load]\n");
        return 2;
    }
    g_link = argc > 6 && strcmp(argv[6], "link") == 0;
    if (nload > 64) nload = 64;
    if (ma_context_init(&be, 1, NULL, &ctx) != MA_SUCCESS) { fprintf(stderr, "no ALSA\n"); return 1; }
    h = ctx.alsa.asoundSO;
    SYM(h, "snd_pcm_status", g_status);
    SYM(h, "snd_pcm_status_sizeof", st_sizeof);
    SYM(h, "snd_pcm_status_get_delay", g_delay);
    SYM(h, "snd_pcm_status_get_avail", g_avail);
    SYM(h, "snd_pcm_status_get_htstamp", g_ts);
    SYM(h, "snd_pcm_status_get_trigger_htstamp", g_trig);
    SYM(h, "snd_pcm_status_get_audio_htstamp", g_audio);
    SYM(h, "snd_pcm_status_get_state", g_state);
    SYM(h, "snd_pcm_status_set_audio_htstamp_config", g_set_cfg);
    SYM(h, "snd_pcm_status_get_audio_htstamp_report", g_rep);
    if (st_sizeof() > sizeof g_st) { fprintf(stderr, "status too big\n"); return 1; }
    g_f = fopen(argv[2], "w");
    if (!g_f) { perror(argv[2]); return 1; }
    fprintf(g_f, "cb_ns,ts_ns,trig_ns,delay,avail,read,n,state,audio_ns,audio_type\n");
    dc = ma_device_config_init(ma_device_type_capture);
    memset(&id, 0, sizeof id);
    snprintf(id.alsa, sizeof id.alsa, "%s", argc > 5 ? argv[5] : "hw:0,0");
    dc.capture.pDeviceID = &id;
    dc.capture.shareMode = ma_share_mode_exclusive;
    dc.capture.format = ma_format_s32;
    dc.capture.channels = 2;
    dc.sampleRate = 48000;
    dc.periodSizeInFrames = argc > 3 ? (ma_uint32)atoi(argv[3]) : 240;
    dc.periods = argc > 4 ? (ma_uint32)atoi(argv[4]) : 3;
    dc.noFixedSizedCallback = MA_TRUE;
    dc.dataCallback = on_data;
    if (ma_device_init(&ctx, &dc, &dev) != MA_SUCCESS) { fprintf(stderr, "device init failed (card busy?)\n"); return 1; }
    {
        /* SND_PCM_TSTAMP_ENABLE 1; type MONOTONIC 1, MONOTONIC_RAW 2 */
        sw_cur_fn cur; sw_set_fn tt, tm; sw_apply_fn apply; sizeof_fn sws;
        static unsigned char sw[512];
        SYM(h, "snd_pcm_sw_params_current", cur);
        SYM(h, "snd_pcm_sw_params_set_tstamp_type", tt);
        SYM(h, "snd_pcm_sw_params_set_tstamp_mode", tm);
        SYM(h, "snd_pcm_sw_params", apply);
        SYM(h, "snd_pcm_sw_params_sizeof", sws);
        if (sws() > sizeof sw || cur(dev.alsa.pPCMCapture, sw) != 0 || tm(dev.alsa.pPCMCapture, sw, 1) != 0 ||
            tt(dev.alsa.pPCMCapture, sw, g_link ? 2 : 1) != 0 || apply(dev.alsa.pPCMCapture, sw) != 0) {
            fprintf(stderr, "timestamp mode refused\n");
            return 1;
        }
    }
    fprintf(stderr, "device %s, %s %u Hz, period %u x %u, %s stamps\n", dev.capture.name,
            ma_get_format_name(dev.capture.internalFormat), dev.capture.internalSampleRate,
            dev.capture.internalPeriodSizeInFrames, dev.capture.internalPeriods, g_link ? "raw+link" : "monotonic");
    /* spinners before the device starts: miniaudio's thread may be raised */
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
    fprintf(stderr, "alsa_capture_probe: %lld rows, %lld frames read\n", g_rows, g_read);
    return 0;
}
