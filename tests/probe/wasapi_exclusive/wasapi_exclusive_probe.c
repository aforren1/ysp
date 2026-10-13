/* wasapi_exclusive_probe.c - WASAPI exclusive-mode render timing, one
 * setting at a time. Plays silence unless --tone. Public domain.
 *
 * docs/audio.md, "Exclusive mode compared with cave-audio": ysp/audio.h
 * (through miniaudio) and a raw loop both gave bursts of events and 30 to
 * 110 ms of driver queue on the development laptop. cave-audio's WASAPI
 * sink opens exclusive mode with other choices. This probe makes each
 * choice a switch, so one run changes one setting. No code is taken from
 * cave-audio (GPL-3.0); the choices come from reading it.
 *
 *   wasapi_exclusive_probe [--preset cave|raw] [--seconds S] [--device NAME]
 *       [--format device|walk|f32|i32|i24|i16|i32v24]
 *       [--period min|default|FRAMES] [--write zeros|silent]
 *       [--pos inside|after] [--order thread|start] [--mmcss NAME|none]
 *       [--no-prefill] [--timer] [--no-thread-com] [--raw]
 *       [--poll BUFFER_FRAMES KEEP_FRAMES] [--load N]
 *       [--tone DB [--gate MS]] [--csv FILE]
 *
 *   --preset cave   cave-audio's sink (the default): the format walk, the
 *                   minimum device period, buffer = period, event-driven,
 *                   a silent prefill, the thread created before Start, COM
 *                   and MMCSS "Pro Audio" on the thread, the whole buffer
 *                   written with zeros at each event, GetPosition between
 *                   GetBuffer and ReleaseBuffer, no timer resolution change
 *   --preset raw    the raw loop of docs/audio.md (v0.3.0): the device
 *                   format, AUDCLNT_BUFFERFLAGS_SILENT, GetPosition after
 *                   ReleaseBuffer, Start before the thread, timeBeginPeriod(1),
 *                   no COM call on the thread
 *   Options after --preset change one setting of it.
 *   --format        device: PKEY_AudioEngine_DeviceFormat; walk: the first of
 *                   float32, int32, int24 (packed), int16 that
 *                   IsFormatSupported(EXCLUSIVE) takes; or one of those
 *                   (i32v24: 32-bit containers with 24 valid bits)
 *   --period        the device period from GetDevicePeriod, or FRAMES; the
 *                   AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED retry is done
 *   --raw           IAudioClient2::SetClientProperties with
 *                   AUDCLNT_STREAMOPTIONS_RAW before Initialize
 *   --poll B K      no event: a buffer of B frames (the driver can round
 *                   it), the thread calls Sleep(1) and keeps K frames in
 *                   it. Give --timer too: without it Sleep(1) takes 15.6 ms
 *   --load N        N threads spin at normal priority during the run
 *   --tone DB       write a 1 kHz sine at DB dBFS (-30 at most) instead of
 *                   silence; --gate MS turns it on and off every MS ms
 *   --csv FILE      one row per wake: t_ns, pos_frames, posqpc_ns, written
 *
 * Statistics skip the first second. "Lead needed" is, for each wake, the
 * time from the previous wake to the play time (from a line fitted to the
 * GetPosition pairs) of the first frame this wake wrote: a sound handed over
 * just after one wake waits for the next, so its maximum is the shortest
 * safe lead of a client that writes at each wake and takes no time.
 *
 * Exit: 0, 1 when the stream did not open or run, 2 usage, 3 not Windows.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>

#if !defined(_WIN32)
int main(void) {
    printf("wasapi_exclusive_probe: WASAPI is Windows only\n");
    return 3;
}
#else

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <avrt.h>
#include <mmsystem.h>
#include <process.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Spelled out so the program links no uuid library. */
static const GUID xp_CLSID_En = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID xp_IID_En = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID xp_IID_AC = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID xp_IID_AC2 = { 0x726778CD, 0xF60A, 0x4EDA, { 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA } };
static const GUID xp_IID_RC = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID xp_IID_CLK = { 0xCD63314F, 0x3FBA, 0x4A1B, { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };
static const GUID xp_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID xp_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const PROPERTYKEY xp_PK_Name = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const PROPERTYKEY xp_PK_DevFmt = { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };

#define XP_E_NOT_ALIGNED ((HRESULT)0x88890019L)

typedef struct xp_rec { int64_t t, pos_cost; uint64_t pos, posqpc, written; } xp_rec;

static struct {
    /* settings */
    const char* format; const char* period; const char* device; const char* mmcss; const char* csv;
    int zeros, pos_inside, thread_first, prefill, timer, thread_com, raw, poll_buf, poll_keep, load, gate_frames;
    double seconds, tone_amp, tone_phase, gate_ms;
    /* stream */
    IAudioClient* ac; IAudioRenderClient* rc; IAudioClock* clk; HANDLE ev;
    WAVEFORMATEXTENSIBLE wf; UINT32 bufsz, period_frames; UINT64 clk_freq;
    volatile LONG stop; uint64_t written; long timeouts, getbuf_fail;
    xp_rec* r; long nr, maxr; int mmcss_ok;
    LARGE_INTEGER qf;
} g;

static int64_t now_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / g.qf.QuadPart) * 1000000000LL + (int64_t)(c.QuadPart % g.qf.QuadPart) * 1000000000LL / g.qf.QuadPart;
}

static void preset(const char* p) {
    g.format = "walk"; g.period = "min"; g.zeros = 1; g.pos_inside = 1; g.thread_first = 1;
    g.prefill = 1; g.timer = 0; g.thread_com = 1; g.mmcss = "Pro Audio";
    if (!strcmp(p, "raw")) {
        g.format = "device"; g.zeros = 0; g.pos_inside = 0; g.thread_first = 0; g.timer = 1; g.thread_com = 0;
    }
}

static void make_fmt(WAVEFORMATEXTENSIBLE* w, DWORD rate, WORD ch, const char* kind) {
    WORD bits = 32, valid = 32; int fl = 0;
    if (!strcmp(kind, "f32")) fl = 1;
    else if (!strcmp(kind, "i24")) bits = valid = 24;
    else if (!strcmp(kind, "i16")) bits = valid = 16;
    else if (!strcmp(kind, "i32v24")) valid = 24;
    memset(w, 0, sizeof *w);
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = ch;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = bits;
    w->Format.nBlockAlign = (WORD)(ch * bits / 8);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    w->Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    w->Samples.wValidBitsPerSample = valid;
    w->dwChannelMask = ch == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    w->SubFormat = fl ? xp_FLOAT : xp_PCM;
}

static void print_fmt(const char* label, const WAVEFORMATEXTENSIBLE* w) {
    int fl = w->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
             (w->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&w->SubFormat, &xp_FLOAT));
    unsigned valid = w->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? w->Samples.wValidBitsPerSample : w->Format.wBitsPerSample;
    printf("%s %s %u bits (%u valid), %lu Hz, %u ch\n", label, fl ? "float" : "int", (unsigned)w->Format.wBitsPerSample,
           valid, (unsigned long)w->Format.nSamplesPerSec, (unsigned)w->Format.nChannels);
}

static IMMDevice* find_device(IMMDeviceEnumerator* en, char* name, int cap) {
    IMMDeviceCollection* col = NULL; IMMDevice* found = NULL; UINT n = 0, i;
    if (!g.device) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &found))) return NULL;
    }
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &n);
        for (i = 0; i < n; i++) {
            IMMDevice* d = NULL; IPropertyStore* ps = NULL; PROPVARIANT pv; int hit = 0;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            PropVariantInit(&pv);
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && SUCCEEDED(IPropertyStore_GetValue(ps, &xp_PK_Name, &pv)) && pv.vt == VT_LPWSTR) {
                char buf[256];
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, buf, (int)sizeof buf, NULL, NULL);
                if (found) {
                    LPWSTR a = NULL, b = NULL;
                    IMMDevice_GetId(d, &a); IMMDevice_GetId(found, &b);
                    if (a && b && !wcscmp(a, b)) { snprintf(name, (size_t)cap, "%s", buf); }
                    CoTaskMemFree(a); CoTaskMemFree(b);
                } else if (strstr(buf, g.device)) {
                    snprintf(name, (size_t)cap, "%s", buf); hit = 1;
                }
            }
            PropVariantClear(&pv);
            if (ps) IPropertyStore_Release(ps);
            if (hit) { found = d; break; }
            IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
    }
    return found;
}

static int choose_format(IMMDevice* md) {
    IPropertyStore* ps = NULL; PROPVARIANT pv; WAVEFORMATEXTENSIBLE dev; HRESULT hr;
    memset(&dev, 0, sizeof dev);
    PropVariantInit(&pv);
    if (FAILED(IMMDevice_OpenPropertyStore(md, STGM_READ, &ps)) || FAILED(IPropertyStore_GetValue(ps, &xp_PK_DevFmt, &pv)) || !pv.blob.pBlobData) {
        printf("no device format\n");
        return 0;
    }
    memcpy(&dev, pv.blob.pBlobData, pv.blob.cbSize < sizeof dev ? pv.blob.cbSize : sizeof dev);
    PropVariantClear(&pv);
    IPropertyStore_Release(ps);
    print_fmt("device format:", &dev);
    if (!strcmp(g.format, "device")) {
        g.wf = dev;
    } else if (!strcmp(g.format, "walk")) {
        static const char* order[] = { "f32", "i32", "i24", "i16" };
        int i;
        for (i = 0; i < 4; i++) {
            make_fmt(&g.wf, dev.Format.nSamplesPerSec, dev.Format.nChannels, order[i]);
            hr = IAudioClient_IsFormatSupported(g.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, &g.wf.Format, NULL);
            printf("walk: %s 0x%08lx\n", order[i], (unsigned long)hr);
            if (hr == S_OK) break;
        }
        if (i == 4) return 0;
    } else {
        make_fmt(&g.wf, dev.Format.nSamplesPerSec, dev.Format.nChannels, g.format);
    }
    hr = IAudioClient_IsFormatSupported(g.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, &g.wf.Format, NULL);
    print_fmt("stream format:", &g.wf);
    if (hr != S_OK) { printf("IsFormatSupported(EXCLUSIVE) 0x%08lx\n", (unsigned long)hr); return 0; }
    return 1;
}

static HRESULT init_client(IMMDevice* md, REFERENCE_TIME hns, int* retried) {
    DWORD flags = g.poll_buf ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    REFERENCE_TIME buf = g.poll_buf ? (REFERENCE_TIME)(1e7 * g.poll_buf / g.wf.Format.nSamplesPerSec + 0.5) : hns;
    HRESULT hr;
    if (g.raw) {
        IAudioClient2* a2 = NULL;
        if (SUCCEEDED(IAudioClient_QueryInterface(g.ac, &xp_IID_AC2, (void**)&a2))) {
            AudioClientProperties p;
            memset(&p, 0, sizeof p);
            p.cbSize = sizeof p; p.eCategory = AudioCategory_Other; p.Options = AUDCLNT_STREAMOPTIONS_RAW;
            hr = IAudioClient2_SetClientProperties(a2, &p);
            printf("SetClientProperties(RAW) 0x%08lx\n", (unsigned long)hr);
            IAudioClient2_Release(a2);
        }
    }
    hr = IAudioClient_Initialize(g.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, flags, buf, hns, &g.wf.Format, NULL);
    if (hr == XP_E_NOT_ALIGNED && !g.poll_buf) {
        UINT32 n = 0;
        *retried = 1;
        IAudioClient_GetBufferSize(g.ac, &n);
        IAudioClient_Release(g.ac); g.ac = NULL;
        if (!n || FAILED(IMMDevice_Activate(md, &xp_IID_AC, CLSCTX_ALL, NULL, (void**)&g.ac))) return hr;
        hns = (REFERENCE_TIME)(1e7 * n / g.wf.Format.nSamplesPerSec + 0.5);
        hr = IAudioClient_Initialize(g.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, flags, hns, hns, &g.wf.Format, NULL);
    }
    g.period_frames = (UINT32)((hns * g.wf.Format.nSamplesPerSec + 5000000) / 10000000);
    return hr;
}

static void record(int64_t t) {
    UINT64 pos = 0, pq = 0; int64_t t1 = now_ns(), c;
    IAudioClock_GetPosition(g.clk, &pos, &pq);
    c = now_ns() - t1;
    if (g.nr < g.maxr) {
        xp_rec* x = &g.r[g.nr++];
        x->t = t; x->pos_cost = c;
        x->pos = (pos / g.clk_freq) * g.wf.Format.nSamplesPerSec + (pos % g.clk_freq) * g.wf.Format.nSamplesPerSec / g.clk_freq;
        x->posqpc = pq * 100;
        x->written = g.written; /* fixed after the write when --pos inside */
    }
}

/* A 1 kHz sine in every channel, for drivers that treat digital silence
 * differently from sound. */
static void write_tone(BYTE* p, UINT32 n) {
    UINT32 i, c, ch = g.wf.Format.nChannels, bits = g.wf.Format.wBitsPerSample;
    int fl = g.wf.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
             (g.wf.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&g.wf.SubFormat, &xp_FLOAT));
    double step = 2.0 * 3.14159265358979 * 1000.0 / g.wf.Format.nSamplesPerSec;
    for (i = 0; i < n; i++) {
        double s = g.tone_amp * sin(g.tone_phase);
        if (g.gate_frames && ((g.written + i) / (uint64_t)g.gate_frames) % 2) s = 0;
        g.tone_phase += step;
        if (g.tone_phase > 6.283185307179586) g.tone_phase -= 6.283185307179586;
        for (c = 0; c < ch; c++) {
            if (fl) { float f = (float)s; memcpy(p, &f, 4); p += 4; }
            else if (bits == 32) { int32_t v = (int32_t)(s * 2147483647.0); memcpy(p, &v, 4); p += 4; }
            else if (bits == 24) { int32_t v = (int32_t)(s * 8388607.0); p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p += 3; }
            else { int16_t v = (int16_t)(s * 32767.0); memcpy(p, &v, 2); p += 2; }
        }
    }
}

static unsigned __stdcall render(void* arg) {
    DWORD idx = 0; HANDLE mm = NULL; int com = 0;
    UINT32 blk = g.wf.Format.nBlockAlign;
    (void)arg;
    if (g.thread_com) com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (g.mmcss) { mm = AvSetMmThreadCharacteristicsA(g.mmcss, &idx); g.mmcss_ok = mm != NULL; }
    while (!g.stop) {
        int64_t t; UINT32 n; BYTE* p = NULL; long before = g.nr;
        if (g.poll_buf) {
            UINT32 pad = 0;
            Sleep(1);
            if (g.stop) break;
            t = now_ns();
            IAudioClient_GetCurrentPadding(g.ac, &pad);
            n = pad < (UINT32)g.poll_keep ? (UINT32)g.poll_keep - pad : 0;
            if (n > g.bufsz - pad) n = g.bufsz - pad;
        } else {
            DWORD w = WaitForSingleObject(g.ev, 2000);
            if (g.stop) break;
            t = now_ns();
            if (w != WAIT_OBJECT_0) { g.timeouts++; continue; }
            n = g.bufsz;
        }
        if (n && SUCCEEDED(IAudioRenderClient_GetBuffer(g.rc, n, &p)) && p) {
            if (g.pos_inside) record(t);
            if (g.tone_amp > 0) write_tone(p, n);
            else if (g.zeros) memset(p, 0, (size_t)n * blk);
            IAudioRenderClient_ReleaseBuffer(g.rc, n, g.zeros || g.tone_amp > 0 ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
            g.written += n;
            if (g.pos_inside && g.nr > before) g.r[g.nr - 1].written = g.written;
        } else if (n) {
            g.getbuf_fail++;
        }
        if (!g.pos_inside) record(t);
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (com) CoUninitialize();
    return 0;
}

static unsigned __stdcall spin(void* arg) {
    volatile unsigned long long x = 0;
    (void)arg;
    while (!g.stop) x++;
    return (unsigned)(x & 1);
}

static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
static double pct(const int64_t* v, long n, double p) { return n ? (double)v[(long)(p * (n - 1) + 0.5)] : 0; }

static void analyze(void) {
    long i, k0 = 0, n, m, bursts = 0, in_burst = 0, b2b = 0, under = 0, still = 0;
    double rate = g.wf.Format.nSamplesPerSec, period_ns = 1e9 * g.period_frames / rate;
    int64_t* v = (int64_t*)malloc(sizeof(int64_t) * (size_t)(g.nr + 1));
    double sx = 0, sy = 0, sxx = 0, sxy = 0, a, b, x0, y0;
    if (!v || g.nr < 10) { printf("too few wakes: %ld\n", g.nr); free(v); return; }
    while (k0 < g.nr && g.r[k0].t - g.r[0].t < 1000000000LL) k0++;
    n = g.nr - k0;
    printf("wakes %ld (%ld after the first second), expected %.0f, timeouts %ld, GetBuffer failures %ld\n", g.nr, n,
           g.seconds * rate / (g.poll_buf ? rate / 1000.0 : g.period_frames), g.timeouts, g.getbuf_fail);
    /* intervals and bursts: a back-to-back wake comes under a quarter period after the last */
    m = 0;
    for (i = k0 + 1; i < g.nr; i++) {
        int64_t d = g.r[i].t - g.r[i - 1].t;
        v[m++] = d;
        if (!g.poll_buf && d < period_ns / 4) { b2b++; if (!in_burst) { bursts++; in_burst = 1; } } else in_burst = 0;
    }
    qsort(v, m, sizeof *v, cmp64);
    printf("interval ms p1 %.3f p50 %.3f p99 %.3f p99.9 %.3f max %.3f; back-to-back %ld of %ld in %ld bursts\n", pct(v, m, .01) / 1e6,
           pct(v, m, .5) / 1e6, pct(v, m, .99) / 1e6, pct(v, m, .999) / 1e6, v[m - 1] / 1e6, b2b, m, bursts);
    if (!g.poll_buf) {
        long gm = 0;
        for (i = k0 + 1; i < g.nr; i++) { int64_t d = g.r[i].t - g.r[i - 1].t; if (d > 2 * period_ns) v[gm++] = d; }
        qsort(v, gm, sizeof *v, cmp64);
        if (gm) printf("gaps over 2 periods: %ld, ms p50 %.2f max %.2f\n", gm, pct(v, gm, .5) / 1e6, v[gm - 1] / 1e6);
    }
    m = 0;
    for (i = k0; i < g.nr; i++) v[m++] = g.r[i].pos_cost;
    qsort(v, m, sizeof *v, cmp64);
    printf("GetPosition cost us p50 %.2f p99 %.2f max %.2f\n", pct(v, m, .5) / 1e3, pct(v, m, .99) / 1e3, v[m - 1] / 1e3);
    /* the position's QPC stamp against the wake time */
    m = 0;
    for (i = k0; i < g.nr; i++) v[m++] = (int64_t)g.r[i].posqpc - g.r[i].t;
    qsort(v, m, sizeof *v, cmp64);
    printf("position stamp minus wake us p1 %.1f p50 %.1f p99 %.1f\n", pct(v, m, .01) / 1e3, pct(v, m, .5) / 1e3, pct(v, m, .99) / 1e3);
    m = 0;
    for (i = k0 + 1; i < g.nr; i++) {
        if (g.r[i].pos == g.r[i - 1].pos) still++;
        else v[m++] = (int64_t)(g.r[i].pos - g.r[i - 1].pos);
        if (g.r[i].pos > g.r[i].written) under++;
    }
    qsort(v, m, sizeof *v, cmp64);
    printf("position step frames p1 %.0f p50 %.0f p99 %.0f; unmoved %ld; past written %ld\n", pct(v, m, .01), pct(v, m, .5), pct(v, m, .99), still, under);
    m = 0;
    for (i = k0; i < g.nr; i++) v[m++] = (int64_t)g.r[i].written - (int64_t)g.r[i].pos;
    qsort(v, m, sizeof *v, cmp64);
    printf("written minus position frames min %lld p50 %.0f p99 %.0f max %lld (p50 %.1f ms)\n", (long long)v[0], pct(v, m, .5),
           pct(v, m, .99), (long long)v[m - 1], pct(v, m, .5) * 1000.0 / rate);
    /* line: stamp time (ns) against position (frames) */
    x0 = (double)g.r[k0].pos; y0 = (double)g.r[k0].posqpc;
    for (i = k0; i < g.nr; i++) {
        double x = (double)g.r[i].pos - x0, y = (double)g.r[i].posqpc - y0;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    a = (sy - b * sx) / n;
    m = 0;
    for (i = k0; i < g.nr; i++) {
        double x = (double)g.r[i].pos - x0, y = (double)g.r[i].posqpc - y0;
        v[m++] = (int64_t)fabs(y - (a + b * x));
    }
    qsort(v, m, sizeof *v, cmp64);
    printf("position line: %.3f frames/s (%+.0f ppm), |residual| us p50 %.1f p99 %.1f max %.1f\n", 1e9 / b, (1e9 / b / rate - 1) * 1e6,
           pct(v, m, .5) / 1e3, pct(v, m, .99) / 1e3, v[m - 1] / 1e3);
    /* lead needed: play time of the first frame wake i wrote minus wake i-1 */
    m = 0;
    for (i = k0 + 1; i < g.nr; i++) {
        double f = (double)g.r[i - 1].written - x0;
        double play = y0 + a + b * f;
        v[m++] = (int64_t)(play - (double)g.r[i - 1].t);
    }
    qsort(v, m, sizeof *v, cmp64);
    printf("lead needed ms p50 %.2f p99 %.2f p99.9 %.2f max %.2f\n", pct(v, m, .5) / 1e6, pct(v, m, .99) / 1e6, pct(v, m, .999) / 1e6,
           v[m - 1] / 1e6);
    free(v);
}

static int usage(void) {
    fprintf(stderr, "usage: wasapi_exclusive_probe [--preset cave|raw] [--seconds S] [--device NAME] [--format F] [--period P]\n"
                    "  [--write zeros|silent] [--pos inside|after] [--order thread|start] [--mmcss NAME|none] [--no-prefill]\n"
                    "  [--timer] [--no-thread-com] [--raw] [--poll BUFFER KEEP] [--load N] [--tone DB [--gate MS]] [--csv FILE]\n");
    return 2;
}

int main(int argc, char** argv) {
    IMMDeviceEnumerator* en = NULL; IMMDevice* md = NULL; HRESULT hr;
    REFERENCE_TIME def = 0, mn = 0, hns, lat = 0; HANDLE th; int i, retried = 0; char name[256] = "?";
    QueryPerformanceFrequency(&g.qf);
    g.seconds = 30;
    preset("cave");
    for (i = 1; i < argc; i++) {
        const char* s = argv[i]; const char* nx = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(s, "--preset") && nx) { preset(nx); i++; }
        else if (!strcmp(s, "--seconds") && nx) { g.seconds = atof(nx); i++; }
        else if (!strcmp(s, "--device") && nx) { g.device = nx; i++; }
        else if (!strcmp(s, "--format") && nx) { g.format = nx; i++; }
        else if (!strcmp(s, "--period") && nx) { g.period = nx; i++; }
        else if (!strcmp(s, "--write") && nx) { g.zeros = !strcmp(nx, "zeros"); i++; }
        else if (!strcmp(s, "--pos") && nx) { g.pos_inside = !strcmp(nx, "inside"); i++; }
        else if (!strcmp(s, "--order") && nx) { g.thread_first = !strcmp(nx, "thread"); i++; }
        else if (!strcmp(s, "--mmcss") && nx) { g.mmcss = strcmp(nx, "none") ? nx : NULL; i++; }
        else if (!strcmp(s, "--csv") && nx) { g.csv = nx; i++; }
        else if (!strcmp(s, "--poll") && i + 2 < argc) { g.poll_buf = atoi(argv[i + 1]); g.poll_keep = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(s, "--no-prefill")) g.prefill = 0;
        else if (!strcmp(s, "--timer")) g.timer = 1;
        else if (!strcmp(s, "--no-thread-com")) g.thread_com = 0;
        else if (!strcmp(s, "--raw")) g.raw = 1;
        else if (!strcmp(s, "--load") && nx) { g.load = atoi(nx); i++; }
        else if (!strcmp(s, "--tone") && nx) {
            double db = atof(nx);
            if (db > -30) { fprintf(stderr, "--tone: above -30 dBFS is refused\n"); return 2; }
            g.tone_amp = pow(10.0, db / 20.0); i++;
        }
        else if (!strcmp(s, "--gate") && nx) { g.gate_ms = atof(nx); i++; }
        else return usage();
    }
    if (g.timer) timeBeginPeriod(1);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&xp_CLSID_En, NULL, CLSCTX_ALL, &xp_IID_En, (void**)&en)) || !(md = find_device(en, name, sizeof name))) {
        printf("no render endpoint\n");
        return 1;
    }
    printf("endpoint: %s\n", name);
    if (FAILED(IMMDevice_Activate(md, &xp_IID_AC, CLSCTX_ALL, NULL, (void**)&g.ac)) || !choose_format(md)) return 1;
    IAudioClient_GetDevicePeriod(g.ac, &def, &mn);
    if (!strcmp(g.period, "min")) hns = mn;
    else if (!strcmp(g.period, "default")) hns = def;
    else hns = (REFERENCE_TIME)(1e7 * atof(g.period) / g.wf.Format.nSamplesPerSec + 0.5);
    hr = init_client(md, hns, &retried);
    if (FAILED(hr)) { printf("Initialize(EXCLUSIVE) 0x%08lx\n", (unsigned long)hr); return 1; }
    g.gate_frames = (int)(g.gate_ms * g.wf.Format.nSamplesPerSec / 1000.0 + 0.5);
    g.ev =CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g.poll_buf) IAudioClient_SetEventHandle(g.ac, g.ev);
    IAudioClient_GetBufferSize(g.ac, &g.bufsz);
    IAudioClient_GetStreamLatency(g.ac, &lat);
    IAudioClient_GetService(g.ac, &xp_IID_RC, (void**)&g.rc);
    IAudioClient_GetService(g.ac, &xp_IID_CLK, (void**)&g.clk);
    IAudioClock_GetFrequency(g.clk, &g.clk_freq);
    if (g.poll_buf) g.period_frames = g.bufsz;
    printf("device period default %.1f ms, min %.1f ms; %s; buffer %u frames, stream latency %.2f ms, clock %llu Hz%s\n", def / 1e4, mn / 1e4,
           g.poll_buf ? "polled every 1 ms" : "event-driven", g.bufsz, lat / 1e4, (unsigned long long)g.clk_freq,
           retried ? ", realigned" : "");
    printf("settings: write %s, GetPosition %s, %s, MMCSS %s, prefill %d, timeBeginPeriod %d, COM on thread %d, raw %d",
           g.tone_amp > 0 ? "tone" : g.zeros ? "zeros" : "silent flag", g.pos_inside ? "inside" : "after", g.thread_first ? "thread then Start" : "Start then thread",
           g.mmcss ? g.mmcss : "none", g.prefill, g.timer, g.thread_com, g.raw);
    if (g.poll_buf) printf(", keep %d frames", g.poll_keep);
    if (g.load) printf(", %d spinning threads", g.load);
    if (g.tone_amp > 0) printf(", tone %.1f dBFS", 20.0 * log10(g.tone_amp));
    if (g.tone_amp > 0 && g.gate_ms > 0) printf(" gated %.0f ms on, %.0f ms off", g.gate_ms, g.gate_ms);
    printf("\n");
    g.maxr = (long)(g.seconds * 2000) + 2000;
    g.r = (xp_rec*)calloc((size_t)g.maxr, sizeof *g.r);
    if (!g.r) return 1;
    if (g.prefill) {
        BYTE* p; UINT32 n = g.poll_buf ? (UINT32)g.poll_keep : g.bufsz;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(g.rc, n, &p))) { IAudioRenderClient_ReleaseBuffer(g.rc, n, AUDCLNT_BUFFERFLAGS_SILENT); g.written = n; }
    }
    for (i = 0; i < g.load; i++) CloseHandle((HANDLE)_beginthreadex(NULL, 0, spin, NULL, 0, NULL));
    if (g.thread_first) {
        th = (HANDLE)_beginthreadex(NULL, 0, render, NULL, 0, NULL);
        hr = IAudioClient_Start(g.ac);
    } else {
        hr = IAudioClient_Start(g.ac);
        th = (HANDLE)_beginthreadex(NULL, 0, render, NULL, 0, NULL);
    }
    if (FAILED(hr)) printf("Start 0x%08lx\n", (unsigned long)hr);
    Sleep((DWORD)(g.seconds * 1000));
    InterlockedExchange(&g.stop, 1);
    SetEvent(g.ev);
    WaitForSingleObject(th, INFINITE);
    IAudioClient_Stop(g.ac);
    printf("MMCSS joined: %d\n", g.mmcss_ok);
    analyze();
    if (g.csv) {
        FILE* f = fopen(g.csv, "wb");
        long k;
        if (f) {
            fprintf(f, "t_ns,pos_frames,posqpc_ns,written\n");
            for (k = 0; k < g.nr; k++)
                fprintf(f, "%lld,%llu,%llu,%llu\n", (long long)g.r[k].t, (unsigned long long)g.r[k].pos, (unsigned long long)g.r[k].posqpc,
                        (unsigned long long)g.r[k].written);
            fclose(f);
        }
    }
    IAudioRenderClient_Release(g.rc);
    IAudioClock_Release(g.clk);
    IAudioClient_Release(g.ac);
    IMMDevice_Release(md);
    IMMDeviceEnumerator_Release(en);
    if (g.timer) timeEndPeriod(1);
    CoUninitialize();
    return g.nr > 10 ? 0 : 1;
}
#endif
