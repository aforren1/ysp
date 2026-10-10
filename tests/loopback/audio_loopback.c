/* audio_loopback.c - ysp/audio.h's onsets against a capture of the output.
 *
 * Not a compile check and not run by ctest. Build it with
 * -DYSP_BUILD_LOOPBACK=ON and run it by hand.
 *
 *   audio_loopback --digital --device NAME [--onsets N] [--db DB]
 *                      [--mute | --unmute] [--volume V] [--exclusive] [--leads]
 *                      [--load N] [--csv FILE]
 *     Windows only. Plays N short bursts (default 500, 120 ms apart) through
 *     ysp/audio.h in shared mode, and captures the same endpoint with WASAPI
 *     loopback, raw, with each packet's device position and QPC time. For
 *     each burst it finds the first sample of the burst in the capture and
 *     reports:
 *       - placement: the capture's frame distance between bursts against
 *         the distance ysp/audio.h planned. Equal means no frame was
 *         inserted, dropped or resampled between the mixer and the engine's
 *         output;
 *       - bit-exactness of the captured burst against the buffer;
 *       - time: the capture's time of the burst minus the record's onset.
 *         Its spread is the scheduling accuracy at the engine's output; its
 *         median is the offset between that tap and the position point. It
 *         is NOT a latency: the tap is before the endpoint buffer, the
 *         driver, the DAC, the amplifier and the air.
 *     A burst is 64 frames of a fixed +-1 sequence at --db (default -60
 *     dBFS; values above -30 are refused). --mute and --unmute set the
 *     endpoint's mute for the run, --volume V its volume scalar (at most
 *     0.1); both are restored at the end. On the test laptop the tap was all
 *     zeros while the endpoint was muted or at volume 0 (docs/audio.md),
 *     so the tap is after the endpoint volume there. --leads plays a second set
 *     with the lead falling from 60 ms to 2 ms and reports the shortest lead
 *     with no LATE record. --exclusive opens the output in exclusive mode
 *     (at most 30 s) to see what the loopback capture gets then.
 *     --load N spins N threads at normal priority for the run.
 *     It cannot see an exclusive or an offloaded stream.
 *
 *   audio_loopback --line --device OUT --in IN [--ttl-ch C --ttl-dtr PORT
 *                      --ttl-latency-ns NS] [--onsets N] [--db DB]
 *     Needs hardware: line out of OUT wired to line in of IN (channel 0).
 *     Plays clicks and captures IN through miniaudio. With a TTL wired into
 *     capture channel C, raised by ysp/serial.h's DTR line on PORT at each
 *     target, both edges are on one ADC clock, so the capture latency
 *     cancels: output latency = TTL time + its own measured latency (from
 *     ysp_serial's loopback, --ttl-latency-ns) + (audio frame - TTL frame) /
 *     rate. It reports that against the onset record (the value
 *     desc.onset_offset_ns should take) and against the record plus the OS's
 *     claim (caps.os_latency_ns), so the first run shows how wrong the claim
 *     is. Without a TTL it reports the round trip, an upper bound on the
 *     output latency. Never run here: no hardware (docs/audio.md).
 *
 *   audio_loopback --line --stream ... (the same options)
 *     The clicks go out as ONE stream (STREAMS in ysp/audio.h) instead of N
 *     sounds: sample i x 0.3 s of the stream starts click i, the main thread
 *     is the producer, and each click's time is yau_stream_time_of() of
 *     its sample. Besides the latency it reports placement through the
 *     stream: the capture's frame distance between clicks minus the planned
 *     0.3 s. With the input and output on one converter clock that is 0 for
 *     every click when no frame was inserted or dropped anywhere in the
 *     stream; with two clocks it is the drift between them (about 0.7
 *     frames per click at 50 ppm). The stream's GAP count must be 0.
 *
 * Exit: 0 when the run completed and every check passed, 1 when a check
 * failed, 2 usage or setup error, 3 a mode this platform does not have.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <process.h>
#endif

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static yau_audio au;
static int g_onsets = 500;
static double g_db = -60;
static const char* g_dev = NULL;
static const char* g_csv = NULL;
static int g_stream = 0;
static float g_vol = -1;

#define BURST 64
static float g_burst[BURST];

static void make_burst(float a) {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    int i;
    for (i = 0; i < BURST; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        g_burst[i] = (x & 1) ? a : -a;
    }
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static double pctl(double* v, int n, double p) {
    if (n <= 0) return 0;
    return v[(int)(p * (n - 1) + 0.5)];
}

/* ===================================================================== */
#if defined(_WIN32)

/* The GUIDs, here, so the program links no uuid library. */
static const GUID lb_CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID lb_IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID lb_IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID lb_IID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const GUID lb_IID_IAudioEndpointVolume = { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };

typedef struct cap_pkt { int64_t frame; uint64_t devpos, qpc; uint32_t flags, n; } cap_pkt;

#define CAP_MAX_FRAMES (48000 * 140)
#define CAP_MAX_PKTS   20000
static float*   g_cap;          /* channel 0 of the loopback capture         */
static int64_t  g_cap_n = 0;
static cap_pkt  g_pkt[CAP_MAX_PKTS];
static int      g_npkt = 0;
static uint32_t g_cap_ch = 0, g_cap_rate = 0;
static volatile LONG g_cap_stop = 0;
static uint32_t g_disc = 0, g_silent_pkts = 0;

typedef struct cap_ctx { IAudioClient* ac; IAudioCaptureClient* cc; } cap_ctx;

static unsigned __stdcall cap_thread(void* p) {
    cap_ctx* c = (cap_ctx*)p;
    (void)yrt_thread_elevate(NULL);
    while (!g_cap_stop) {
        UINT32 next = 0;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(c->cc, &next)) && next) {
            BYTE* data; UINT32 n; DWORD fl; UINT64 dp, qpc;
            if (FAILED(IAudioCaptureClient_GetBuffer(c->cc, &data, &n, &fl, &dp, &qpc))) break;
            if (g_npkt < CAP_MAX_PKTS && g_cap_n + n <= CAP_MAX_FRAMES) {
                UINT32 i;
                g_pkt[g_npkt].frame = g_cap_n; g_pkt[g_npkt].devpos = dp; g_pkt[g_npkt].qpc = qpc;
                g_pkt[g_npkt].flags = fl; g_pkt[g_npkt].n = n; g_npkt++;
                for (i = 0; i < n; i++)
                    g_cap[g_cap_n + i] = (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : ((const float*)(const void*)data)[i * g_cap_ch];
                g_cap_n += n;
                if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) g_disc++;
                if (fl & AUDCLNT_BUFFERFLAGS_SILENT) g_silent_pkts++;
            }
            IAudioCaptureClient_ReleaseBuffer(c->cc, n);
        }
        yrt_sleep_until(yrt_now_ns() + 2000000, 0);
    }
    return 0;
}

/* The ysp_rt time of capture frame j, from the packet that holds it. */
static int64_t cap_time(int64_t j) {
    int lo = 0, hi = g_npkt - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (g_pkt[mid].frame <= j) lo = mid; else hi = mid - 1; }
    return (int64_t)g_pkt[lo].qpc * 100 + (int64_t)((double)(j - g_pkt[lo].frame) * 1e9 / g_cap_rate + 0.5);
}

/* The capture's own clock at frame j: least squares of packet time on
 * packet frame over the packets within 5 s of j that no discontinuity
 * separates from it. A packet's stamp alone moves by a few hundred
 * microseconds, and a capture that lost packets loses frames, so one line
 * over the whole run would not do (measured, docs/audio.md). */
static double cap_time_fit(double j) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, mx, my, b;
    int q, lo, hi, n, k;
    int64_t f0, t0;
    lo = 0; hi = g_npkt - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if ((double)g_pkt[mid].frame <= j) lo = mid; else hi = mid - 1; }
    k = lo;
    for (lo = k; lo > 0 && !(g_pkt[lo].flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                && (double)g_pkt[lo - 1].frame > j - 240000.0; lo--) { }
    for (hi = k; hi + 1 < g_npkt && !(g_pkt[hi + 1].flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                && (double)g_pkt[hi + 1].frame < j + 240000.0; hi++) { }
    n = hi - lo + 1;
    f0 = g_pkt[lo].frame;
    t0 = (int64_t)g_pkt[lo].qpc * 100;
    if (n < 2) return (double)t0 + (j - (double)f0) * 1e9 / g_cap_rate;
    for (q = lo; q <= hi; q++) { sx += (double)(g_pkt[q].frame - f0); sy += (double)((int64_t)g_pkt[q].qpc * 100 - t0); }
    mx = sx / n; my = sy / n;
    for (q = lo; q <= hi; q++) {
        double dx = (double)(g_pkt[q].frame - f0) - mx, dy = (double)((int64_t)g_pkt[q].qpc * 100 - t0) - my;
        sxx += dx * dx; sxy += dx * dy;
    }
    b = sxy / sxx;
    return (double)t0 + my + b * (j - (double)f0 - mx);
}

/* Whether the capture flagged a discontinuity between frames a and b. */
static int cap_gap_between(int64_t a, int64_t b) {
    int q;
    for (q = 0; q < g_npkt; q++)
        if ((g_pkt[q].flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && g_pkt[q].frame > a && g_pkt[q].frame <= b) return 1;
    return 0;
}

/* The capture frame nearest ysp_rt time t. */
static int64_t cap_frame_at(int64_t t) {
    int lo = 0, hi = g_npkt - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if ((int64_t)g_pkt[mid].qpc * 100 <= t) lo = mid; else hi = mid - 1; }
    return g_pkt[lo].frame + (int64_t)((double)(t - (int64_t)g_pkt[lo].qpc * 100) * g_cap_rate / 1e9);
}

/* The burst in the capture by cross-correlation, because the tap need not
 * hold the samples as written: on the test laptop it is after the endpoint
 * volume and a filter (docs/audio.md). Returns the lag of the largest
 * correlation in [from, from + span), or -1 when that peak is not 8 times
 * the window's rms correlation; *frac is the parabolic sub-frame offset,
 * *scale the burst's gain in the tap, *exact whether the samples there are
 * the buffer's bit for bit. */
static int64_t find_burst(int64_t from, int64_t span, double* scale, int* exact, double* frac) {
    int64_t j, end = from + span, best = -1;
    double bc = 0, ss = 0, e = 0;
    long cnt = 0;
    int i;
    if (from < 1) from = 1;
    if (end > g_cap_n - BURST - 1) end = g_cap_n - BURST - 1;
    for (i = 0; i < BURST; i++) e += (double)g_burst[i] * g_burst[i];
    for (j = from; j < end; j++) {
        double c = 0;
        for (i = 0; i < BURST; i++) c += (double)g_cap[j + i] * g_burst[i];
        ss += c * c; cnt++;
        if (c > bc) { bc = c; best = j; }
    }
    *exact = 0; *scale = 0; *frac = 0;
    if (best < 0 || cnt == 0 || bc * bc < 64.0 * ss / (double)cnt) return -1;
    {
        double cm = 0, cp = 0, den;
        for (i = 0; i < BURST; i++) { cm += (double)g_cap[best - 1 + i] * g_burst[i]; cp += (double)g_cap[best + 1 + i] * g_burst[i]; }
        den = cm - 2 * bc + cp;
        if (den < 0) *frac = 0.5 * (cm - cp) / den;
    }
    *scale = bc / e;
    *exact = 1;
    for (i = 0; i < BURST; i++) if (g_cap[best + i] != g_burst[i]) { *exact = 0; break; }
    return best;
}

static int g_load = 0;
static unsigned char g_ring_mem[YRT_RING_BYTES(16384)];
static yrt_ring g_ring;
static FILE* g_ringf = NULL;
static void drain_ring(void) {
    yrt_event ev[256];
    int k, j;
    while ((k = yrt_ring_drain(&g_ring, ev, 256)) > 0)
        for (j = 0; j < k && g_ringf; j++)
            if (ev[j].source == YRT_SRC_AUDIO && ev[j].kind != YAU_EV_ONSET && ev[j].kind != YAU_EV_END)
                fprintf(g_ringf, "%u,%llu,%u,%lld,%lld,%.6f,%.0f\n", (unsigned)ev[j].kind, (unsigned long long)ev[j].t_ns,
                        (unsigned)ev[j].aux, (long long)ev[j].u.i64[0], (long long)ev[j].u.i64[1], ev[j].u.f64[1], ev[j].u.f64[2]);
}
static volatile LONG g_load_stop = 0;
static volatile unsigned long g_sink = 0;
static unsigned __stdcall load_thread(void* p) { (void)p; while (!g_load_stop) g_sink++; return 0; }

static int digital(int mute, int exclusive, int leads) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* md = NULL;
    IAudioClient* ac = NULL;
    IAudioCaptureClient* cc = NULL;
    IAudioEndpointVolume* vol = NULL;
    WAVEFORMATEX* wf = NULL;
    BOOL was_muted = FALSE;
    float level = -1;
    cap_ctx cx;
    HANDLE th;
    yau_desc d;
    yau_buf b;
    yau_id* ids;
    yau_onset* recs;
    int i, n = g_onsets, nlead = leads ? 200 : 0, failures = 0;
    ma_device_info* infos; ma_uint32 ninfo, k;
    char line[512];
    ma_context ctx;
    const ma_device_id* pick = NULL;

    g_cap = (float*)calloc(CAP_MAX_FRAMES, sizeof(float));
    ids = (yau_id*)calloc((size_t)(n + nlead), sizeof *ids);
    recs = (yau_onset*)calloc((size_t)(n + nlead), sizeof *recs);
    if (!g_cap || !ids || !recs) return 2;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* the endpoint by name, through miniaudio's list, which holds WASAPI ids */
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 2;
    if (ma_context_get_devices(&ctx, &infos, &ninfo, NULL, NULL) == MA_SUCCESS)
        for (k = 0; k < ninfo; k++) if (strstr(infos[k].name, g_dev)) { pick = &infos[k].id; printf("endpoint: %s\n", infos[k].name); break; }
    if (!pick) { fprintf(stderr, "no playback device named like '%s'\n", g_dev); return 2; }
    if (FAILED(CoCreateInstance(&lb_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &lb_IID_IMMDeviceEnumerator, (void**)&en))
        || FAILED(IMMDeviceEnumerator_GetDevice(en, pick->wasapi, &md))
        || FAILED(IMMDevice_Activate(md, &lb_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&ac))
        || FAILED(IAudioClient_GetMixFormat(ac, &wf))
        || FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, NULL))
        || FAILED(IAudioClient_GetService(ac, &lb_IID_IAudioCaptureClient, (void**)&cc))) {
        fprintf(stderr, "the loopback capture did not open\n");
        return 2;
    }
    g_cap_ch = wf->nChannels;
    g_cap_rate = wf->nSamplesPerSec;
    if (SUCCEEDED(IMMDevice_Activate(md, &lb_IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void**)&vol))) {
        IAudioEndpointVolume_GetMute(vol, &was_muted);
        IAudioEndpointVolume_GetMasterVolumeLevelScalar(vol, &level);
        if (mute) IAudioEndpointVolume_SetMute(vol, mute == 1, NULL);
        if (g_vol >= 0) IAudioEndpointVolume_SetMasterVolumeLevelScalar(vol, g_vol, NULL);
    }
    printf("loopback: %u Hz, %u channels, volume scalar %.3f (was %.3f), muted %s (was %s)\n", (unsigned)g_cap_rate,
           (unsigned)g_cap_ch, (double)(g_vol >= 0 ? g_vol : level), (double)level,
           mute ? (mute == 1 ? "yes" : "no") : (was_muted ? "yes" : "no"), was_muted ? "yes" : "no");

    memset(&d, 0, sizeof d);
    yrt_ring_open(&g_ring, &(yrt_ring_desc){ .memory = g_ring_mem, .bytes = sizeof g_ring_mem });
    d.ring = &g_ring;
    d.device = g_dev;
    d.exclusive = exclusive != 0;
    d.arena_bytes = 1 << 16;
    if (g_csv) {
        char path[600];
        snprintf(path, sizeof path, "%s.ring", g_csv);
        g_ringf = fopen(path, "w");
        if (g_ringf) fprintf(g_ringf, "kind,t_ns,aux,i0,i1,rate,spread\n");
    }
    if (!yau_open(&au, &d)) {
        fprintf(stderr, "%s\n", yau_error(&au));
        if (vol) { IAudioEndpointVolume_SetMute(vol, was_muted, NULL); if (g_vol >= 0) IAudioEndpointVolume_SetMasterVolumeLevelScalar(vol, level, NULL); }
        return 2;
    }
    yau_describe(&au, line, sizeof line);
    puts(line);
    if (g_cap_rate != au.dcaps.rate) { fprintf(stderr, "capture rate differs from the device rate\n"); }
    IAudioClient_Start(ac);
    cx.ac = ac; cx.cc = cc;
    th = (HANDLE)_beginthreadex(NULL, 0, cap_thread, &cx, 0, NULL);
    for (i = 0; i < g_load; i++) CloseHandle((HANDLE)_beginthreadex(NULL, 0, load_thread, NULL, 0, NULL));
    yrt_sleep_until(yrt_now_ns() + 300000000, 0);

    memset(&b, 0, sizeof b);
    b.frames = g_burst; b.n = BURST; b.channels = 1;
    {
        /* targets at random sub-frame phases, 50 to 110 ms ahead, every 120 ms */
        uint64_t x = 12345;
        int64_t t0 = yrt_now_ns();
        for (i = 0; i < n + nlead; i++) {
            int64_t slot = t0 + (int64_t)i * 120000000LL, t, lead;
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            if (i < n) lead = 50000000 + (int64_t)((x >> 33) % 60000000);
            else lead = 60000000 - (int64_t)(i - n) * 290000;   /* 60 ms down to 2 ms */
            t = slot + 120000000 + (int64_t)((x >> 20) % 20833);
            yrt_sleep_until((int64_t)(t - lead), 0);
            ids[i] = yau_play_at(&au, b, t);
            yau_update(&au);
            drain_ring();
            /* the handle keeps the last 256 records: collect each one about
             * a second after it was handed over */
            if (i >= 8 && (ids[i - 8] <= 0 || yau_wait(&au, ids[i - 8], 500000000, &recs[i - 8]) != YAU_OK))
                recs[i - 8].start_frame = -1;
        }
        yrt_sleep_until(yrt_now_ns() + 400000000, 0);
    }
    for (i = n + nlead - 8 < 0 ? 0 : n + nlead - 8; i < n + nlead; i++) {
        if (ids[i] <= 0 || yau_wait(&au, ids[i], 1000000000, &recs[i]) != YAU_OK) { recs[i].start_frame = -1; }
    }
    yau_describe(&au, line, sizeof line);
    InterlockedExchange(&g_cap_stop, 1);
    InterlockedExchange(&g_load_stop, 1);
    WaitForSingleObject(th, INFINITE);
    IAudioClient_Stop(ac);
    yau_close(&au);
    drain_ring();
    if (g_ringf) fclose(g_ringf);
    if (vol) {
        IAudioEndpointVolume_SetMute(vol, was_muted, NULL);
        if (g_vol >= 0) IAudioEndpointVolume_SetMasterVolumeLevelScalar(vol, level, NULL);
    }
    puts(line);
    {
        int64_t j, nz = 0;
        float mx = 0;
        for (j = 0; j < g_cap_n; j++) { float a = (float)fabs(g_cap[j]); if (a > 0) nz++; if (a > mx) mx = a; }
        printf("capture: %lld frames in %d packets, %u discontinuities, %u silent packets, %lld nonzero, peak %g\n",
               (long long)g_cap_n, g_npkt, (unsigned)g_disc, (unsigned)g_silent_pkts, (long long)nz, (double)mx);
    }

    {
        /* find every burst; map the first one by its plan, the rest by the
         * planned spacing, so a frame inserted or dropped shows */
        double* dt = (double*)calloc((size_t)(n + nlead), sizeof(double));
        double* dts = (double*)calloc((size_t)(n + nlead), sizeof(double));
        int found = 0, placed = 0, exact_n = 0, nd = 0, late_n = 0;
        int64_t L0 = -1, Lp = 0, Fp = 0, lag0 = 0, moved[8];
        int gaps = 0, moved_n = 0;
        double scale0 = 0;
        int64_t shortest_ok = -1;
        FILE* f = g_csv ? fopen(g_csv, "w") : NULL;
        if (f) fprintf(f, "i,target,onset,start_frame,flags,tier,cap_frame,cap_time,scale,exact\n");
        for (i = 0; i < n + nlead; i++) {
            yau_onset* r = &recs[i];
            int64_t L, guess;
            double sc = 0, fr = 0;
            int ex = 0;
            if (r->start_frame < 0) continue;
            if (r->flags & YAU_ONSET_LATE) late_n += i >= n;
            if (L0 < 0) {
                /* the first one by time: within 50 ms of its record, where
                 * no other burst is (they are 120 ms apart) */
                L = find_burst(cap_frame_at(r->onset) - 2400, 4800, &sc, &ex, &fr);
                if (L >= 0) lag0 = L - cap_frame_at(r->onset);
            } else {
                /* each next one by its own time and the first one's lag, so a
                 * capture that lost packets does not lose the rest */
                guess = cap_frame_at(r->onset) + lag0;
                L = find_burst(guess - 1200, 2400, &sc, &ex, &fr);
            }
            if (L < 0) { if (f) fprintf(f, "%d,%lld,%lld,%lld,%u,%u,-1,0,0,0\n", i, (long long)r->target, (long long)r->onset, (long long)r->start_frame, r->flags, r->tier); continue; }
            found++;
            /* placement: the frame distance to the previous burst found, as
             * planned, unless the capture lost packets between the two */
            if (L0 < 0) { L0 = L; scale0 = sc; placed++; }
            else if (cap_gap_between(Lp, L)) gaps++;
            else if (L - Lp == r->start_frame - Fp) placed++;
            else if (moved_n < 8) moved[moved_n++] = (L - Lp) - (r->start_frame - Fp);
            Lp = L; Fp = r->start_frame;
            exact_n += ex;
            if (i < n) {
                dts[nd] = (double)(cap_time(L) - r->onset) + fr * 1e9 / g_cap_rate;
                dt[nd++] = cap_time_fit((double)L + fr) - (double)r->onset;
            }
            if (f) fprintf(f, "%d,%lld,%lld,%lld,%u,%u,%lld,%lld,%.9g,%d\n", i, (long long)r->target, (long long)r->onset,
                           (long long)r->start_frame, r->flags, r->tier, (long long)L, (long long)cap_time(L), sc, ex);
        }
        if (f) fclose(f);
        printf("bursts: %d played, %d found, %d at the planned spacing from the one before, %d across a capture gap, "
               "%d bit-exact, scale %.6f\n", n + nlead, found, placed, gaps, exact_n, scale0);
        if (moved_n) {
            int q;
            printf("placement off by (frames):");
            for (q = 0; q < moved_n; q++) printf(" %lld", (long long)moved[q]);
            printf("\n");
        }
        if (nd > 0) {
            static double dev[4000];
            double med;
            int j;
            qsort(dt, (size_t)nd, sizeof dt[0], cmp_d);
            med = pctl(dt, nd, 0.5);
            for (j = 0; j < nd; j++) dev[j] = fabs(dt[j] - med);
            qsort(dev, (size_t)nd, sizeof dev[0], cmp_d);
            {
                double ms;
                int jj;
                static double ds[4000];
                qsort(dts, (size_t)nd, sizeof dts[0], cmp_d);
                ms = pctl(dts, nd, 0.5);
                for (jj = 0; jj < nd; jj++) ds[jj] = fabs(dts[jj] - ms);
                qsort(ds, (size_t)nd, sizeof ds[0], cmp_d);
                printf("by each packet's own stamp: |deviation from median| p50 %.1f p99 %.1f max %.1f us\n",
                       pctl(ds, nd, 0.5) / 1e3, pctl(ds, nd, 0.99) / 1e3, ds[nd - 1] / 1e3);
            }
            printf("capture time (local fit of the capture clock) - onset record, us: p1 %.1f p50 %.1f p99 %.1f; |deviation from median| p50 %.1f p99 %.1f max %.1f\n",
                   pctl(dt, nd, 0.01) / 1e3, med / 1e3, pctl(dt, nd, 0.99) / 1e3,
                   pctl(dev, nd, 0.5) / 1e3, pctl(dev, nd, 0.99) / 1e3, dev[nd - 1] / 1e3);
            if (pctl(dev, nd, 0.99) > 20833.0) failures++;
        }
        if (nlead) {
            for (i = n; i < n + nlead; i++) if (!(recs[i].flags & YAU_ONSET_LATE) && recs[i].start_frame >= 0) shortest_ok = 60000000 - (int64_t)(i - n) * 290000;
            printf("leads: %d of %d LATE; the shortest lead with no LATE record before the first LATE: ", late_n, nlead);
            for (i = n; i < n + nlead; i++) if (recs[i].flags & YAU_ONSET_LATE) break;
            printf("%.2f ms\n", i > n ? (60000000 - (double)(i - 1 - n) * 290000) / 1e6 : -1.0);
            (void)shortest_ok;
        }
        if (placed + gaps != found || found < n) failures++;
        free(dt);
        free(dts);
    }
    if (g_csv) {
        /* the raw capture (channel 0, float) beside the CSV, for analysis */
        char path[600];
        FILE* fb;
        snprintf(path, sizeof path, "%s.f32", g_csv);
        fb = fopen(path, "wb");
        if (fb) { fwrite(g_cap, sizeof(float), (size_t)g_cap_n, fb); fclose(fb); }
        snprintf(path, sizeof path, "%s.pkt", g_csv);
        fb = fopen(path, "w");
        if (fb) {
            int q;
            fprintf(fb, "frame,devpos,qpc100,flags,n\n");
            for (q = 0; q < g_npkt; q++)
                fprintf(fb, "%lld,%llu,%llu,%u,%u\n", (long long)g_pkt[q].frame, (unsigned long long)g_pkt[q].devpos,
                        (unsigned long long)g_pkt[q].qpc, (unsigned)g_pkt[q].flags, (unsigned)g_pkt[q].n);
            fclose(fb);
        }
    }
    CoTaskMemFree(wf);
    if (vol) IAudioEndpointVolume_Release(vol);
    IAudioCaptureClient_Release(cc);
    IAudioClient_Release(ac);
    IMMDevice_Release(md);
    IMMDeviceEnumerator_Release(en);
    ma_context_uninit(&ctx);
    free(g_cap); free(ids); free(recs);
    return failures ? 1 : 0;
}
#endif /* _WIN32 */

/* ===================================================================== */
/* --line: line out to line in, with an optional TTL on a second channel.  */

/* --line --stream: the clicks as one stream, the main thread its producer.
 * Click i starts on stream sample i x 0.3 s; its time is the fit's time of
 * that sample, read once the stream has started (yau_stream_time_of). */
static int64_t* g_click_t;
static uint32_t g_stream_gaps = 0;
static void stream_clicks(int n, yser_port* ttl, int64_t* t_ttl, yau_id* ids, uint32_t rate) {
    static yau_stream st;
    yau_stream_desc sd;
    yau_stream_info in;
    yau_onset r;
    int64_t spacing = (int64_t)rate * 3 / 10, total = spacing * n, next = 0;
    int i_t = 0, i;
    g_click_t = (int64_t*)calloc((size_t)n, sizeof *g_click_t);
    memset(&sd, 0, sizeof sd);
    sd.channels = 1;
    if (!g_click_t || yau_stream_init(&au, &st, &sd) != 0) { fprintf(stderr, "stream: %s\n", yau_error(&au)); return; }
    for (;;) {
        int64_t k;
        float* p;
        while (next < total && (p = yau_stream_acquire(&st, &k)) != NULL) {
            if (k > total - next) k = total - next;
            for (i = 0; i < k; i++) {
                int64_t q = (next + i) % spacing;
                p[i] = q < BURST ? g_burst[q] : 0.0f;
            }
            yau_stream_commit(&st, k);
            next += k;
        }
        if (next >= total) yau_stream_end(&st);
        if (!ids[0]) ids[0] = yau_play_stream(&au, &st, yrt_now_ns() + 300000000);
        yau_update(&au);
        yau_stream_get_info(&st, &in);
        if (in.started) {
            for (i = 0; i < n; i++)
                if (!g_click_t[i]) (void)yau_stream_time_of(&au, &st, (int64_t)i * spacing, &g_click_t[i]);
            if (ttl && i_t < n && yrt_now_ns() >= g_click_t[i_t] - 2000000) {
                yrt_sleep_until(g_click_t[i_t], YRT_DEFAULT_SPIN_NS);
                yser_set_dtr(ttl, true);
                t_ttl[i_t] = yrt_now_ns();
                yrt_sleep_until(yrt_now_ns() + 5000000, 0);
                yser_set_dtr(ttl, false);
                i_t++;
            }
        }
        if (in.state == YAU_STREAM_ENDED) break;
        yrt_sleep_until(yrt_now_ns() + 1000000, 0);
    }
    if (yau_result(&au, ids[0], &r) == YAU_OK)
        printf("line: stream onset tier %d, flags 0x%x, %lld silent frames\n", r.tier, (unsigned)r.flags, (long long)r.gap_frames);
    g_stream_gaps = in.gaps;
    (void)yau_stream_release(&au, &st);
}

#define LINE_MAX_FRAMES (48000 * 120)
static float*   g_in;           /* the capture, channel 0 and the TTL channel */
static float*   g_in_ttl;
static int64_t  g_in_n = 0;
static int      g_ttl_ch = -1;
static int64_t  g_in_t[200000]; /* callback entry per capture block           */
static int64_t  g_in_w[200000];
static int      g_in_blocks = 0;

static void line_cb(ma_device* dv, void* out, const void* in, ma_uint32 n) {
    const float* x = (const float*)in;
    ma_uint32 i, ch = dv->capture.channels;
    (void)out;
    if (g_in_blocks < 200000) { g_in_t[g_in_blocks] = yrt_now_ns(); g_in_w[g_in_blocks] = g_in_n; g_in_blocks++; }
    for (i = 0; i < n && g_in_n < LINE_MAX_FRAMES; i++, g_in_n++) {
        g_in[g_in_n] = x[i * ch];
        if (g_ttl_ch >= 0 && (ma_uint32)g_ttl_ch < ch) g_in_ttl[g_in_n] = x[i * ch + (ma_uint32)g_ttl_ch];
    }
}

static int line_mode(const char* in_name, const char* dtr_port, int64_t ttl_lat) {
    ma_context ctx;
    ma_device_config dc;
    ma_device dv;
    ma_device_info* infos; ma_uint32 ninfo, k;
    const ma_device_id* pick = NULL;
    yau_desc d;
    yau_caps caps;
    yau_buf b;
    yser_port port;
    int i, n = g_onsets, have_ttl = 0;
    int64_t* t_ttl;
    yau_id* ids;
    char line[512];
    double* lat;
    int nl = 0;
    g_in = (float*)calloc(LINE_MAX_FRAMES, sizeof(float));
    g_in_ttl = (float*)calloc(LINE_MAX_FRAMES, sizeof(float));
    t_ttl = (int64_t*)calloc((size_t)n, sizeof *t_ttl);
    ids = (yau_id*)calloc((size_t)n, sizeof *ids);
    lat = (double*)calloc((size_t)n, sizeof *lat);
    if (!g_in || !g_in_ttl || !t_ttl || !ids || !lat) return 2;
    memset(&port, 0, sizeof port);
    if (dtr_port && g_ttl_ch >= 0) {
        yser_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.device = dtr_port;
        if (!yser_open(&port, &sd)) { fprintf(stderr, "%s\n", yser_error(&port)); return 2; }
        yser_set_dtr(&port, false);
        have_ttl = 1;
    }
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 2;
    if (ma_context_get_devices(&ctx, NULL, NULL, &infos, &ninfo) == MA_SUCCESS)
        for (k = 0; k < ninfo; k++) if (strstr(infos[k].name, in_name)) { pick = &infos[k].id; break; }
    if (!pick) { fprintf(stderr, "no capture device named like '%s'\n", in_name); return 2; }
    dc = ma_device_config_init(ma_device_type_capture);
    dc.capture.pDeviceID = pick;
    dc.capture.format = ma_format_f32;
    dc.dataCallback = line_cb;
    if (ma_device_init(&ctx, &dc, &dv) != MA_SUCCESS) { fprintf(stderr, "the capture device did not open\n"); return 2; }
    memset(&d, 0, sizeof d);
    d.device = g_dev;
    d.arena_bytes = g_stream ? 1 << 20 : 1 << 16;
    if (!yau_open(&au, &d)) { fprintf(stderr, "%s\n", yau_error(&au)); return 2; }
    yau_get_caps(&au, &caps);
    yau_describe(&au, line, sizeof line);
    puts(line);
    ma_device_start(&dv);
    yrt_sleep_until(yrt_now_ns() + 500000000, 0);
    memset(&b, 0, sizeof b);
    b.frames = g_burst; b.n = BURST; b.channels = 1;
    if (g_stream) {
        stream_clicks(n, have_ttl ? &port : NULL, t_ttl, ids, caps.rate);
        n = 0;   /* the clicks are out; the loop below has nothing to play */
    }
    for (i = 0; i < n; i++) {
        int64_t t = yrt_now_ns() + 150000000;
        ids[i] = yau_play_at(&au, b, t);
        if (have_ttl) {
            yrt_sleep_until(t, YRT_DEFAULT_SPIN_NS);
            yser_set_dtr(&port, true);
            t_ttl[i] = yrt_now_ns();
            yrt_sleep_until(yrt_now_ns() + 5000000, 0);
            yser_set_dtr(&port, false);
        }
        yrt_sleep_until(yrt_now_ns() + 150000000, 0);
    }
    yrt_sleep_until(yrt_now_ns() + 500000000, 0);
    ma_device_uninit(&dv);
    if (g_stream) n = g_onsets;
    {
        /* find each click and TTL edge in the capture, in order */
        int64_t pos = 0, tpos = 0, prev = -1, dmin = INT64_MAX, dmax = INT64_MIN;
        double thr = 0.5 * fabs(g_burst[0]);
        for (i = 0; i < n; i++) {
            yau_onset r;
            int64_t a = -1, e = -1, j;
            if (g_stream) {
                /* the click's time: the fit's time of its stream sample */
                memset(&r, 0, sizeof r);
                r.onset = g_click_t[i];
                if (r.onset <= 0) continue;
            } else if (yau_wait(&au, ids[i], 1000000000, &r) != YAU_OK) continue;
            for (j = pos; j < g_in_n; j++) if (fabs(g_in[j]) > thr) { a = j; break; }
            if (a < 0) break;
            pos = a + 4800;
            if (g_stream && prev >= 0) {
                int64_t dev = (a - prev) - (int64_t)caps.rate * 3 / 10;
                if (dev < dmin) dmin = dev;
                if (dev > dmax) dmax = dev;
            }
            prev = a;
            if (have_ttl) {
                for (j = tpos; j < g_in_n; j++) if (g_in_ttl[j] > 0.25f) { e = j; break; }
                if (e < 0) break;
                tpos = e + 4800;
                /* both edges on one ADC clock: the capture latency cancels */
                lat[nl++] = (double)(t_ttl[i] + ttl_lat + (int64_t)((double)(a - e) * 1e9 / caps.rate) - r.onset);
            } else {
                /* round trip: the capture block's entry time, earliest */
                int blk = 0;
                while (blk + 1 < g_in_blocks && g_in_w[blk + 1] <= a) blk++;
                lat[nl++] = (double)(g_in_t[blk] - r.onset);
            }
        }
        if (g_stream && prev >= 0 && dmin <= dmax)
            printf("line: stream placement, capture frame distance minus the planned 0.3 s: min %lld, max %lld frames; gaps %u\n",
                   (long long)dmin, (long long)dmax, (unsigned)g_stream_gaps);
    }
    yau_close(&au);
    if (have_ttl) yser_close(&port);
    ma_context_uninit(&ctx);
    if (nl == 0) { printf("line: no click found in the capture\n"); return 1; }
    qsort(lat, (size_t)nl, sizeof lat[0], cmp_d);
    printf("line: %d clicks; %s minus the onset record, us: p1 %.1f p50 %.1f p99 %.1f\n", nl,
           have_ttl ? "measured output time" : "round trip (capture block time)",
           pctl(lat, nl, 0.01) / 1e3, pctl(lat, nl, 0.5) / 1e3, pctl(lat, nl, 0.99) / 1e3);
    if (have_ttl) {
        printf("line: desc.onset_offset_ns should be %.0f\n", pctl(lat, nl, 0.5));
        if (caps.os_latency_ns >= 0)
            printf("line: measured minus the OS's claim (%s, %.3f ms): %.1f us\n", caps.os_latency_src,
                   (double)caps.os_latency_ns / 1e6, (pctl(lat, nl, 0.5) - (double)caps.os_latency_ns) / 1e3);
        else
            printf("line: the OS makes no latency claim on this backend\n");
    }
    free(g_in); free(g_in_ttl); free(t_ttl); free(ids); free(lat);
    return 0;
}

int main(int argc, char** argv) {
    int i, mode = 0, mute = 0, exclusive = 0, leads = 0;
    const char* in_name = NULL;
    const char* dtr = NULL;
    int64_t ttl_lat = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--digital")) mode = 1;
        else if (!strcmp(argv[i], "--line")) mode = 2;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) g_dev = argv[++i];
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) in_name = argv[++i];
        else if (!strcmp(argv[i], "--onsets") && i + 1 < argc) g_onsets = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) g_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--unmute")) mute = 2;
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) g_vol = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--exclusive")) exclusive = 1;
        else if (!strcmp(argv[i], "--leads")) leads = 1;
        else if (!strcmp(argv[i], "--stream")) g_stream = 1;
#if defined(_WIN32)
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) g_load = atoi(argv[++i]);
#endif
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) g_csv = argv[++i];
        else if (!strcmp(argv[i], "--ttl-ch") && i + 1 < argc) g_ttl_ch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ttl-dtr") && i + 1 < argc) dtr = argv[++i];
        else if (!strcmp(argv[i], "--ttl-latency-ns") && i + 1 < argc) ttl_lat = atoll(argv[++i]);
        else { fprintf(stderr, "audio_loopback: unknown argument %s (see the top of the file)\n", argv[i]); return 2; }
    }
    if (g_vol > 0.1f) { fprintf(stderr, "--volume: at most 0.1\n"); return 2; }
    if (!mode || !g_dev || g_onsets < 2 || g_onsets > 2000 || g_db > -30 || (mode == 2 && !in_name)) {
        fprintf(stderr, "usage: audio_loopback --digital --device NAME [...] | --line --device OUT --in IN [...]\n");
        return 2;
    }
    if (g_stream && mode != 2) { fprintf(stderr, "--stream: with --line only\n"); return 2; }
    if (exclusive && (double)g_onsets * 0.12 > 25) { fprintf(stderr, "--exclusive: at most 200 onsets (30 s)\n"); return 2; }
    make_burst(yau_db((float)g_db));
    if (mode == 1) {
#if defined(_WIN32)
        return digital(mute, exclusive, leads);
#else
        (void)mute; (void)exclusive; (void)leads;
        fprintf(stderr, "--digital needs WASAPI loopback capture (Windows)\n");
        return 3;
#endif
    }
    return line_mode(in_name, dtr, ttl_lat);
}
