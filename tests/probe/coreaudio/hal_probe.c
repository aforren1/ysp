/* hal_probe.c - what the HAL says about an output, and its render time
 * stamps, through miniaudio's AudioUnit as ysp/audio.h opens it. Plays
 * silence. docs/audio.md, "CoreAudio", came from it.
 *
 *     clang -O2 -I<miniaudio dir> tests/probe/coreaudio/hal_probe.c -o hal_probe \
 *         -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
 *     ./hal_probe [NAME] [SECONDS] [STALL_MS]
 *
 * NAME: a part of the output's name (default: the default output).
 * Prints the device's properties (UID, transport, rates, latency, safety
 * offset, IO buffer, streams and their latency and formats, hog mode), the
 * AudioUnit's client and device formats, once a second the device's
 * IsAlive, the AudioUnit's current device and the listeners' counts, and
 * per render (the first two, and every one whose call came over 15 ms
 * after the last or whose sample time jumped): the stamp's sample time, its
 * host time minus the call's, and the device's current time. STALL_MS:
 * the 100th callback sleeps that long. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#include "miniaudio.h"
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static mach_timebase_info_data_t tb;
static double tk2ns(int64_t t) { return (double)t * tb.numer / tb.denom; }

#define N 16384
typedef struct { uint64_t entry, pre_host, now_host; double pre_sample, now_sample, rate; uint32_t flags, frames, aflags; } rec;
static rec R[N];
static volatile int nrec = 0, ncb = 0, nnote = 0;
static AudioTimeStamp last_ts;
static UInt32 last_aflags;
static AudioObjectID g_dev;
static int stall_ms = 0;
static volatile int overloads = 0, alive_notes = 0, devs_notes = 0;
static OSStatus on_alive(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress* a, void* ud) { (void)id; (void)n; (void)a; (void)ud; alive_notes++; return noErr; }
static OSStatus on_devs(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress* a, void* ud) { (void)id; (void)n; (void)a; (void)ud; devs_notes++; return noErr; }
static OSStatus on_over(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress* a, void* ud) { (void)id; (void)n; (void)a; (void)ud; overloads++; return noErr; }

typedef OSStatus (*addnotify_fn)(AudioUnit, AURenderCallback, void*);
static OSStatus notify(void* ud, AudioUnitRenderActionFlags* f, const AudioTimeStamp* ts, UInt32 bus, UInt32 n, AudioBufferList* b) {
    (void)ud; (void)bus; (void)n; (void)b;
    if (*f & kAudioUnitRenderAction_PreRender) { last_ts = *ts; last_aflags = *f; nnote++; }
    return noErr;
}

static void data(ma_device* d, void* out, const void* in, ma_uint32 n) {
    (void)in;
    uint64_t e = mach_absolute_time();
    memset(out, 0, n * ma_get_bytes_per_frame(d->playback.format, d->playback.channels));
    AudioTimeStamp now; memset(&now, 0, sizeof now);
    AudioDeviceGetCurrentTime(g_dev, &now);
    if (nrec < N) {
        rec* r = &R[nrec++];
        r->entry = e; r->pre_host = last_ts.mHostTime; r->pre_sample = last_ts.mSampleTime; r->rate = last_ts.mRateScalar;
        r->flags = last_ts.mFlags; r->frames = n; r->aflags = last_aflags; r->now_host = now.mHostTime; r->now_sample = now.mSampleTime;
    }
    ncb++;
    if (stall_ms && ncb == 100) usleep(stall_ms * 1000);
}

static void getu32(AudioObjectID id, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, const char* name) {
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain };
    UInt32 v = 0, sz = sizeof v;
    OSStatus st = AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &v);
    if (st) printf("  %-28s err %d\n", name, (int)st);
    else printf("  %-28s %u (0x%08x '%c%c%c%c')\n", name, v, v, (v >> 24) & 0xff ? (v >> 24) & 0xff : '.', (v >> 16) & 0xff ? (v >> 16) & 0xff : '.', (v >> 8) & 0xff ? (v >> 8) & 0xff : '.', v & 0xff ? v & 0xff : '.');
}
static void getf64(AudioObjectID id, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, const char* name) {
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain };
    Float64 v = 0; UInt32 sz = sizeof v;
    OSStatus st = AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &v);
    if (st) printf("  %-28s err %d\n", name, (int)st); else printf("  %-28s %.6f\n", name, v);
}
static void getstr(AudioObjectID id, AudioObjectPropertySelector s, const char* name) {
    AudioObjectPropertyAddress a = { s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef v = NULL; UInt32 sz = sizeof v; char buf[256] = "";
    OSStatus st = AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &v);
    if (!st && v) { CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(v); }
    printf("  %-28s %s%s\n", name, st ? "err " : "", buf);
}
static void fmt(const char* name, AudioStreamBasicDescription* f) {
    printf("    %s: %.0f Hz fmt '%c%c%c%c' flags 0x%x bpf %u ch %u bits %u\n", name, f->mSampleRate,
           (f->mFormatID >> 24) & 0xff, (f->mFormatID >> 16) & 0xff, (f->mFormatID >> 8) & 0xff, f->mFormatID & 0xff,
           f->mFormatFlags, f->mBytesPerFrame, f->mChannelsPerFrame, f->mBitsPerChannel);
}

static void props(AudioObjectID id) {
    AudioObjectPropertyScope o = kAudioObjectPropertyScopeOutput, g = kAudioObjectPropertyScopeGlobal;
    printf("device %u\n", id);
    getstr(id, kAudioObjectPropertyName, "name");
    getstr(id, kAudioDevicePropertyDeviceUID, "uid");
    getstr(id, kAudioDevicePropertyModelUID, "model uid");
    getstr(id, kAudioObjectPropertyManufacturer, "manufacturer");
    getu32(id, kAudioDevicePropertyTransportType, g, "transport");
    getf64(id, kAudioDevicePropertyNominalSampleRate, g, "nominal rate");
    getf64(id, kAudioDevicePropertyActualSampleRate, g, "actual rate");
    getu32(id, kAudioDevicePropertyLatency, o, "latency (out)");
    getu32(id, kAudioDevicePropertySafetyOffset, o, "safety offset (out)");
    getu32(id, kAudioDevicePropertyBufferFrameSize, o, "buffer frame size");
    getu32(id, kAudioDevicePropertyUsesVariableBufferFrameSizes, o, "variable buffer sizes");
    getu32(id, kAudioDevicePropertyClockDomain, g, "clock domain");
    getu32(id, kAudioDevicePropertyDeviceIsAlive, g, "alive");
    getu32(id, kAudioDevicePropertyDeviceIsRunningSomewhere, g, "running somewhere");
    getu32(id, kAudioDevicePropertyHogMode, g, "hog mode pid");
    getu32(id, kAudioDevicePropertyIsHidden, g, "hidden");
    getu32(id, kAudioDevicePropertyClockSource, o, "clock source");
    {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSizeRange, o, kAudioObjectPropertyElementMain };
        AudioValueRange r; UInt32 sz = sizeof r;
        if (!AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &r)) printf("  %-28s %.0f..%.0f\n", "buffer size range", r.mMinimum, r.mMaximum);
    }
    {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, o, kAudioObjectPropertyElementMain };
        AudioStreamID s[16]; UInt32 sz = sizeof s, i;
        if (!AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, s))
            for (i = 0; i < sz / sizeof s[0]; i++) {
                printf("  stream %u\n", s[i]);
                getu32(s[i], kAudioStreamPropertyLatency, g, "  stream latency");
                getu32(s[i], kAudioStreamPropertyTerminalType, g, "  terminal type");
                getu32(s[i], kAudioStreamPropertyStartingChannel, g, "  starting channel");
                AudioStreamBasicDescription f; UInt32 fz = sizeof f;
                AudioObjectPropertyAddress b = { kAudioStreamPropertyVirtualFormat, g, kAudioObjectPropertyElementMain };
                if (!AudioObjectGetPropertyData(s[i], &b, 0, NULL, &fz, &f)) fmt("virtual", &f);
                b.mSelector = kAudioStreamPropertyPhysicalFormat; fz = sizeof f;
                if (!AudioObjectGetPropertyData(s[i], &b, 0, NULL, &fz, &f)) fmt("physical", &f);
            }
    }
}

int main(int argc, char** argv) {
    const char* want = argc > 1 ? argv[1] : NULL;
    int secs = argc > 2 ? atoi(argv[2]) : 2;
    stall_ms = argc > 3 ? atoi(argv[3]) : 0;
    mach_timebase_info(&tb);
    ma_context ctx; ma_backend be = ma_backend_coreaudio;
    if (ma_context_init(&be, 1, NULL, &ctx)) return 1;
    ma_device_info* inf; ma_uint32 n, i; ma_device_id* pick = NULL;
    ma_context_get_devices(&ctx, &inf, &n, NULL, NULL);
    for (i = 0; i < n; i++) { if (want && strstr(inf[i].name, want)) pick = &inf[i].id; }
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.pDeviceID = pick;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noFixedSizedCallback = MA_TRUE;
    dc.dataCallback = data;
    ma_device dev;
    if (ma_device_init(&ctx, &dc, &dev)) return 1;
    g_dev = (AudioObjectID)dev.coreaudio.deviceObjectIDPlayback;
    printf("miniaudio: %s rate %u ch %u fmt %d period %u periods %u passthrough %d\n", dev.playback.name, dev.playback.internalSampleRate,
           dev.playback.internalChannels, dev.playback.internalFormat, dev.playback.internalPeriodSizeInFrames, dev.playback.internalPeriods,
           dev.playback.converter.isPassthrough);
    {
        AudioStreamBasicDescription f; UInt32 fz = sizeof f;
        AudioUnit au = (AudioUnit)dev.coreaudio.audioUnitPlayback;
        if (!AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, &fz)) fmt("AU input scope (client)", &f);
        fz = sizeof f;
        if (!AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, &fz)) fmt("AU output scope (device)", &f);
        addnotify_fn add = (addnotify_fn)dlsym(ctx.coreaudio.hAudioUnit, "AudioUnitAddRenderNotify");
        printf("AudioUnitAddRenderNotify via dlsym: %p\n", (void*)add);
        if (add) printf("add notify: %d\n", (int)add(au, notify, NULL));
    }
    props(g_dev);
    { AudioObjectPropertyAddress a = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
      printf("overload listener: %d\n", (int)AudioObjectAddPropertyListener(g_dev, &a, on_over, NULL));
      a.mSelector = kAudioDevicePropertyDeviceIsAlive;
      printf("alive listener: %d\n", (int)AudioObjectAddPropertyListener(g_dev, &a, on_alive, NULL));
      a.mSelector = kAudioHardwarePropertyDevices;
      printf("devices listener: %d\n", (int)AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, on_devs, NULL)); }
    ma_device_start(&dev);
    for (int q = 0; q < secs; q++) {
        sleep(1);
        AudioObjectPropertyAddress a = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 v = 9, sz = sizeof v; OSStatus st = AudioObjectGetPropertyData(g_dev, &a, 0, NULL, &sz, &v);
        UInt32 run = 9; sz = sizeof run; AudioObjectPropertyAddress b = { kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(g_dev, &b, 0, NULL, &sz, &run);
        AudioObjectID cur = 0; UInt32 csz = sizeof cur;
        AudioUnitGetProperty((AudioUnit)dev.coreaudio.audioUnitPlayback, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &cur, &csz);
        printf("t %d s: callbacks %d alive %u (st %d) running %u state %d overloads %d alive_notes %d devs_notes %d AU device %u\n", q + 1, ncb, v, (int)st, run, (int)ma_device_get_state(&dev), overloads, alive_notes, devs_notes, cur);
        fflush(stdout);
    }
    ma_device_stop(&dev);
    printf("callbacks %d notes %d overloads %d\n", ncb, nnote, overloads);
    {
        int k; double prev_s = 0; uint64_t prev_h = 0;
        for (k = 0; k < nrec; k++) {
            rec* r = &R[k];
            if (k < 2 || (k > 0 && tk2ns((int64_t)(r->entry - R[k-1].entry)) > 15e6) || (k > 0 && r->pre_sample - prev_s != R[k - 1].frames))
                printf("%4d n %u ts.sample %.0f dS %.0f ts.host-entry %.3f ms dH %.3f ms rate %.6f flags 0x%x | now.sample %.1f ts-now %.1f fr, now.host-entry %.3f ms\n",
                       k, r->frames, r->pre_sample, k ? r->pre_sample - prev_s : 0, tk2ns((int64_t)(r->pre_host - r->entry)) / 1e6,
                       k ? tk2ns((int64_t)(r->pre_host - prev_h)) / 1e6 : 0, r->rate, r->flags, r->now_sample, r->pre_sample - r->now_sample,
                       tk2ns((int64_t)(r->now_host - r->entry)) / 1e6);
            prev_s = r->pre_sample; prev_h = r->pre_host;
        }
    }
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    return 0;
}
