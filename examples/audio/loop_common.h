/* loop_common.h - shared by audio_loopback_latency and audio_first_sound:
 * a simulated loop (an output and an input on one timer thread, the
 * input hearing the output a set delay later), Windows endpoint volume
 * and mute for an acoustic run, and the capture of a whole run into
 * memory. Included once, after ysp/audio.h's implementation. Not API. */
#ifndef YSP_EXAMPLES_LOOP_COMMON_H
#define YSP_EXAMPLES_LOOP_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <pthread.h>
#endif

/* --- the simulated loop ---------------------------------------------------- */

/* One thread, a 10 ms tick: the output renders a block, its channel 0
 * goes into a delay line of delay frames, and the input gets the block
 * that left the line, with a noise floor at -80 dBFS. A gate, when set,
 * mutes the first gate_frames after gate_quiet frames of digital silence:
 * a sink that wakes up late (rig_spec 13). */
typedef struct lp_sim {
    int64_t  delay, gate_frames, gate_quiet;
    /* a participant (audio_voice_rt): rt_ms[i] after the i-th sound that
     * follows 300 ms of silence, a word goes into the line */
    double   rt_ms[64];
    int      nrt, heard;
    int64_t  resp_at, quiet_in;
    float*   line;              /* delay + 480 frames, a ring                 */
    int64_t  w;                 /* frames rendered                            */
    int64_t  quiet, muting;
    uint64_t rng;
    yau_render_fn render, block;
    void*    out_host;
    void*    in_host;
    volatile int run, started;
    int      out_init, in_init, timer;
    float    out[480 * 2];
    float    in[480];
} lp_sim;

static lp_sim g_lp;

static int lp_out_open(void* ctx, const yau_device_open* in, yau_device_caps* caps, char* err, size_t cap) {
    (void)ctx; (void)err; (void)cap;
    memset(caps, 0, sizeof *caps);
    caps->rate = in->format->rate;
    caps->channels = in->format->channels;
    caps->out = YAU_OUT_F32;
    caps->period = 480;
    caps->buffer = 960;
    caps->pos_source = YAU_POS_CALLBACK;
    caps->tier = YAU_TIER_SIM;
    snprintf(caps->name, sizeof caps->name, "simulated output");
    return 0;
}
static int lp_in_open(void* ctx, const yau_device_open* in, yau_device_caps* caps, char* err, size_t cap) {
    (void)ctx; (void)err; (void)cap;
    memset(caps, 0, sizeof *caps);
    caps->rate = in->format->rate;
    caps->channels = 1;
    caps->out = YAU_OUT_F32;
    caps->period = 480;
    caps->buffer = 960;
    caps->pos_source = YAU_POS_CALLBACK;
    caps->tier = YAU_TIER_SIM;
    snprintf(caps->name, sizeof caps->name, "simulated input");
    return 0;
}

/* One 10 ms tick of the loop, entered at t_entry. */
static void lp_step(lp_sim* s, int64_t t_entry) {
    yau_tick tk;
    int i;
    memset(&tk, 0, sizeof tk);
    tk.t_entry = t_entry;
    tk.pos = -1;
    if (s->render && s->out_host) {
        if (!s->out_init) { yau_rt_thread_init(s->out_host); s->out_init = 1; }
        s->render(s->out_host, s->out, 480, &tk);
    } else {
        memset(s->out, 0, sizeof s->out);
    }
    for (i = 0; i < 480; i++) {
        float x = s->out[i * 2];
        int64_t at = (s->w + i) % (s->delay + 480), f = s->w + i;
        if (s->nrt > 0) {
            if (fabs((double)x) > 0.01 && s->quiet_in >= 14400 && s->heard < s->nrt)
                s->resp_at = f + (int64_t)(s->rt_ms[s->heard++] * 48.0 + 0.5);
            s->quiet_in = fabs((double)x) > 0.01 ? 0 : s->quiet_in + 1;
            if (s->resp_at > 0 && f >= s->resp_at && f < s->resp_at + 14400) {
                /* a word: 120 Hz and 4 harmonics, 5 ms attack, -24 dBFS rms */
                double t = (double)(f - s->resp_at) / 48.0, env = t < 5 ? 0.5 * (1 - cos(3.14159265358979 * t / 5)) : 1, v = 0;
                int h;
                for (h = 1; h <= 5; h++) v += sin(2 * 3.14159265358979 * 120.0 * h * t / 1000.0) / h;
                x += (float)(0.2 * env * v / 2.28);
            }
        }
        if (s->gate_frames > 0) {
            if (x == 0.0f) s->quiet++;
            else {
                if (s->quiet >= s->gate_quiet) s->muting = s->gate_frames;
                s->quiet = 0;
            }
            if (s->muting > 0) { s->muting--; x = 0.0f; }
        }
        s->line[at] = x;
    }
    for (i = 0; i < 480; i++) {
        int64_t at = (s->w + i + 480) % (s->delay + 480);   /* delay frames ago */
        double u;
        s->rng = s->rng * 6364136223846793005ull + 1442695040888963407ull;
        u = ((double)(s->rng >> 11) / 9007199254740992.0) * 2.0 - 1.0;
        s->in[i] = s->line[at] + (float)(u * 1.7e-4);   /* -80 dBFS rms */
    }
    s->w += 480;
    if (s->block && s->in_host) {
        if (!s->in_init) { yau_capture_thread_init(s->in_host); s->in_init = 1; }
        s->block(s->in_host, s->in, 480, &tk);
    }
}

/* On ysp/rt.h's virtual clock (--sim) the tick is a virtual timer on the
 * frame thread, so the loop's blocks land at the same times on any host. */
static int64_t lp_tick(void* p, int64_t now) {
    lp_sim* s = (lp_sim*)p;
    if (!s->run) return 0;
    lp_step(s, now);
    return now + 10000000;
}

#if defined(_WIN32)
static unsigned __stdcall lp_thread(void* p)
#else
static void* lp_thread(void* p)
#endif
{
    lp_sim* s = (lp_sim*)p;
    int64_t next = yrt_now_ns();
    (void)yrt_thread_elevate(NULL);
    while (s->run) {
        next += 10000000;
        yrt_sleep_until(next, YRT_DEFAULT_SPIN_NS);
        lp_step(s, yrt_now_ns());
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void lp_go(lp_sim* s) {
    if (s->started) return;
    s->started = 1;
    s->run = 1;
    if (yrt_is_virtual()) {
        s->timer = yrt_virtual_timer(lp_tick, s, yrt_now_ns() + 10000000);
        return;
    }
#if defined(_WIN32)
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, lp_thread, s, 0, NULL));
#else
    { pthread_t th; pthread_create(&th, NULL, lp_thread, s); pthread_detach(th); }
#endif
}
static int lp_out_start(void* ctx, yau_render_fn r, void* host) {
    lp_sim* s = (lp_sim*)ctx;
    s->out_host = host;
    s->render = r;
    lp_go(s);
    return 0;
}
static int lp_in_start(void* ctx, yau_render_fn b, void* host) {
    lp_sim* s = (lp_sim*)ctx;
    s->in_host = host;
    s->block = b;
    lp_go(s);
    return 0;
}
/* Stopping either side ends the thread; it is the same device. */
static void lp_stop(void* ctx) {
    lp_sim* s = (lp_sim*)ctx;
    if (!s->run) return;
    s->run = 0;
    if (yrt_is_virtual()) { yrt_virtual_cancel(s->timer); return; }
    yrt_sleep_until(yrt_now_ns() + 30000000, 0);
}
static void lp_close(void* ctx) { (void)ctx; }
static const yau_device g_lp_out = { YAU_DEVICE_VERSION, "simulated loop", lp_out_open, lp_out_start, lp_stop, lp_close, NULL };
static const yau_device g_lp_in = { YAU_DEVICE_VERSION, "simulated loop", lp_in_open, lp_in_start, lp_stop, lp_close, NULL };

static int lp_sim_init(lp_sim* s, double delay_ms, double gate_ms, double gate_quiet_ms) {
    memset(s, 0, sizeof *s);
    s->delay = (int64_t)(delay_ms * 48.0 + 0.5);
    if (s->delay < 480) s->delay = 480;
    s->gate_frames = (int64_t)(gate_ms * 48.0 + 0.5);
    s->gate_quiet = (int64_t)(gate_quiet_ms * 48.0 + 0.5);
    s->rng = 99;
    s->line = (float*)calloc((size_t)(s->delay + 480), sizeof(float));
    return s->line ? 0 : -1;
}

/* --- the run in memory ----------------------------------------------------- */

/* Channel ch of every captured frame, indexed by capture frame; a gap is
 * left at 0. */
typedef struct lp_rec {
    float*  x;
    int64_t cap, n;             /* frames held; the highest frame + 1         */
    int     ch;
} lp_rec;

static inline int64_t lp_drain(yau_capture* c, lp_rec* r) {
    int64_t n, f, got = 0;
    const float* p;
    int C = c->dcaps.channels;
    while ((p = yau_capture_acquire(c, 0, &n, &f, NULL)) != NULL) {
        int64_t i;
        for (i = 0; i < n && f + i < r->cap; i++) r->x[f + i] = p[i * C + r->ch];
        if (f + n > r->n) r->n = f + n < r->cap ? f + n : r->cap;
        (void)yau_capture_release(c, 0, n);
        got += n;
    }
    return got;
}

/* RMS of x[a, b) in dBFS. */
static inline double lp_rms_db(const lp_rec* r, int64_t a, int64_t b) {
    double s = 0;
    int64_t i;
    if (a < 0) a = 0;
    if (b > r->n) b = r->n;
    if (b <= a) return -999;
    for (i = a; i < b; i++) s += (double)r->x[i] * r->x[i];
    s /= (double)(b - a);
    return s > 0 ? 10 * log10(s) : -999;
}

static inline int lp_cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static inline double lp_pct(double* v, int n, double p) { return n > 0 ? v[(int)(p * (n - 1) + 0.5)] : 0; }

/* --- endpoint volume and mute (Windows) -------------------------------------- */

#if defined(_WIN32)
typedef struct lp_ep {
    IAudioEndpointVolume* v;
    BOOL  was_muted;
    float was_level;
    int   touched;
    char  name[64];
} lp_ep;

static const GUID lp_CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID lp_IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID lp_IID_IAudioEndpointVolume = { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };

/* The endpoint (render or capture) whose name contains name, or the
 * default one; its mute and volume are read and kept for the restore. */
static inline int lp_ep_open(lp_ep* e, int capture, const char* name) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* md = NULL;
    HRESULT hr;
    memset(e, 0, sizeof *e);
    (void)CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&lp_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &lp_IID_IMMDeviceEnumerator, (void**)&en)))
        return -1;
    if (name && name[0]) {
        ma_context ctx;
        ma_device_info *pi, *ci;
        ma_uint32 np, nc, k, n;
        ma_device_info* list;
        hr = E_FAIL;
        if (ma_context_init(NULL, 0, NULL, &ctx) == MA_SUCCESS) {
            if (ma_context_get_devices(&ctx, &pi, &np, &ci, &nc) == MA_SUCCESS) {
                list = capture ? ci : pi;
                n = capture ? nc : np;
                for (k = 0; k < n; k++)
                    if (strstr(list[k].name, name)) { hr = IMMDeviceEnumerator_GetDevice(en, list[k].id.wasapi, &md); break; }
            }
            ma_context_uninit(&ctx);
        }
    } else {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, capture ? eCapture : eRender, eConsole, &md);
    }
    if (FAILED(hr) || !md || FAILED(IMMDevice_Activate(md, &lp_IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void**)&e->v))) {
        if (md) IMMDevice_Release(md);
        IMMDeviceEnumerator_Release(en);
        return -1;
    }
    IAudioEndpointVolume_GetMute(e->v, &e->was_muted);
    IAudioEndpointVolume_GetMasterVolumeLevelScalar(e->v, &e->was_level);
    IMMDevice_Release(md);
    IMMDeviceEnumerator_Release(en);
    return 0;
}
static inline void lp_ep_set(lp_ep* e, int unmute, float level) {
    if (!e->v) return;
    if (unmute) { IAudioEndpointVolume_SetMute(e->v, FALSE, NULL); e->touched = 1; }
    if (level >= 0) { IAudioEndpointVolume_SetMasterVolumeLevelScalar(e->v, level, NULL); e->touched = 1; }
}
static inline void lp_ep_restore(lp_ep* e) {
    if (!e->v) return;
    if (e->touched) {
        IAudioEndpointVolume_SetMasterVolumeLevelScalar(e->v, e->was_level, NULL);
        IAudioEndpointVolume_SetMute(e->v, e->was_muted, NULL);
    }
    IAudioEndpointVolume_Release(e->v);
    e->v = NULL;
}
static inline void lp_ep_print(const lp_ep* e, const char* what) {
    BOOL m = FALSE;
    float l = -1;
    if (!e->v) { printf("%s endpoint: volume not readable\n", what); return; }
    IAudioEndpointVolume_GetMute(e->v, &m);
    IAudioEndpointVolume_GetMasterVolumeLevelScalar(e->v, &l);
    printf("%s endpoint: volume scalar %.3f (was %.3f), muted %s (was %s)\n", what, (double)l, (double)e->was_level,
           m ? "yes" : "no", e->was_muted ? "yes" : "no");
}
#endif

#endif
