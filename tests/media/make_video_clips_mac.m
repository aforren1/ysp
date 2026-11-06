/* make_video_clips_mac.m - the MP4 clips of make_video_clips.sh, made on
 * macOS without ffmpeg: VideoToolbox encodes, AVAssetWriter writes the
 * compressed samples as they are (passthrough). Nothing is downloaded.
 *
 *   clang -fobjc-arc -O2 -o make_video_clips_mac tests/media/make_video_clips_mac.m \
 *       -framework Foundation -framework AVFoundation -framework CoreMedia \
 *       -framework CoreVideo -framework VideoToolbox
 *   ./make_video_clips_mac OUTDIR [--long | --av]
 *
 * tests/media/make_video_clips_mac.sh does both. Point YVID_TEST_MEDIA at
 * OUTDIR. The names and frame counts are make_video_clips.sh's, so
 * tests/adapt/video_test.c reads either set. Each frame carries its own
 * index in 16 bars across the top 64 rows (bit k white when bit k of the
 * frame number is set). VideoToolbox puts the color in the stream's VUI
 * (H.264 and HEVC) and AVAssetWriter writes a colr box.
 *
 * Not made here, because VideoToolbox does not encode them: r_444 (4:4:4),
 * r_interlace and r_mpeg4 (MPEG-4 Part 2). r_jpeg.mov (Motion JPEG in a
 * QuickTime file) stands in for "not H.264 or HEVC"; r_10bit is HEVC Main
 * 10 (VideoToolbox has no H.264 High 10). No .nv12 references: there is no
 * second decoder here; the test compares VideoToolbox's hardware and
 * software decoders with the index instead.
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    F_BFRAMES = 1, F_VFR = 2, F_KEY17 = 4, F_SAR = 8, F_ROT = 16, F_OFFSET = 32,
    F_AUDIO = 64, F_MIXCOLOR = 128, F_10BIT = 256, F_NOISE = 512, F_NOBARS = 1024
};

typedef struct clip {
    const char* name;
    int w, h, num, den, frames, gop, ts;
    CMVideoCodecType codec;
    int flags;
    const char* ext;
} clip;

static NSMutableArray* g_out;   /* encoded samples, in decode order */
static NSLock* g_lock;

static void on_encoded(void* ref, void* fref, OSStatus st, VTEncodeInfoFlags info, CMSampleBufferRef sb) {
    (void)ref; (void)fref; (void)info;
    if (st != noErr || !sb) { fprintf(stderr, "encode failed: %d\n", (int)st); exit(1); }
    [g_lock lock];
    [g_out addObject:(__bridge id)sb];
    [g_lock unlock];
}

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 32); }

/* One frame: a moving diagonal pattern and chroma ramps (testsrc2's role),
 * the bars in the top 64 rows, neutral chroma in the top 32. */
static void fill(CVPixelBufferRef pb, const clip* c, int i) {
    int ten = (c->flags & F_10BIT) != 0;
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    uint8_t* uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (int r = 0; r < c->h; r++) {
        for (int x = 0; x < c->w; x++) {
            int v;
            if (r < 64 && !(c->flags & F_NOBARS)) v = ((i >> (x * 16 / c->w)) & 1) ? 235 : 16;
            else {
                v = 16 + ((x + r * 2 + i * 4) % 220);
                if (c->flags & F_NOISE) { v += (int)(rnd() % 13) - 6; v = v < 16 ? 16 : v > 235 ? 235 : v; }
            }
            if (ten) ((uint16_t*)(void*)(y + (size_t)r * ys))[x] = (uint16_t)((v << 2) << 6);
            else y[(size_t)r * ys + (size_t)x] = (uint8_t)v;
        }
    }
    for (int r = 0; r < (c->h + 1) / 2; r++) {
        for (int x = 0; x < (c->w + 1) / 2; x++) {
            int cb = 128, cr = 128;
            if (r * 2 >= 32 || (c->flags & F_NOBARS)) { cb = 16 + (x * 224 / ((c->w + 1) / 2)); cr = 16 + ((r * 2 + i) % 224); }
            if (ten) {
                uint16_t* p = (uint16_t*)(void*)(uv + (size_t)r * uvs);
                p[2 * x] = (uint16_t)((cb << 2) << 6); p[2 * x + 1] = (uint16_t)((cr << 2) << 6);
            } else {
                uv[(size_t)r * uvs + 2 * (size_t)x] = (uint8_t)cb;
                uv[(size_t)r * uvs + 2 * (size_t)x + 1] = (uint8_t)cr;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

/* A CMSampleBuffer of 16-bit stereo PCM, `n` frames from frame `at`. */
static CMSampleBufferRef pcm(CMAudioFormatDescriptionRef fd, int64_t at, int n) {
    int16_t* s = malloc((size_t)n * 4);
    for (int k = 0; k < n; k++) {
        int16_t v = (int16_t)lrint(8000.0 * sin(2.0 * M_PI * 440.0 * (double)(at + k) / 48000.0));
        s[2 * k] = v; s[2 * k + 1] = v;
    }
    CMBlockBufferRef bb = NULL;
    CMBlockBufferCreateWithMemoryBlock(NULL, NULL, (size_t)n * 4, NULL, NULL, 0, (size_t)n * 4, kCMBlockBufferAssureMemoryNowFlag, &bb);
    CMBlockBufferReplaceDataBytes(s, bb, 0, (size_t)n * 4);
    free(s);
    CMSampleBufferRef sb = NULL;
    CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, fd, n, CMTimeMake(at, 48000), NULL, &sb);
    CFRelease(bb);
    return sb;
}

static int make(NSString* dir, const clip* c) {
    int ten = (c->flags & F_10BIT) != 0;
    OSStatus st;
    VTCompressionSessionRef vt = NULL;
    g_out = [NSMutableArray array];
    NSDictionary* src = @{ (id)kCVPixelBufferPixelFormatTypeKey:
                               @(ten ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                           (id)kCVPixelBufferWidthKey: @(c->w), (id)kCVPixelBufferHeightKey: @(c->h) };
    st = VTCompressionSessionCreate(NULL, c->w, c->h, c->codec, NULL, (__bridge CFDictionaryRef)src, NULL, on_encoded, NULL, &vt);
    if (st != noErr) { fprintf(stderr, "%s: VTCompressionSessionCreate %d\n", c->name, (int)st); return 1; }
    CFStringRef prim = kCVImageBufferColorPrimaries_ITU_R_709_2, trc = kCVImageBufferTransferFunction_ITU_R_709_2,
                mat = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    if (c->flags & F_MIXCOLOR) { trc = kCVImageBufferTransferFunction_sRGB; mat = kCVImageBufferYCbCrMatrix_ITU_R_601_4; }
    if (c->codec == kCMVideoCodecType_H264)
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    else if (c->codec == kCMVideoCodecType_HEVC)
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ProfileLevel, ten ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel);
    if (c->codec != kCMVideoCodecType_JPEG) {
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_AllowFrameReordering, (c->flags & F_BFRAMES) ? kCFBooleanTrue : kCFBooleanFalse);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef)@(c->gop + 1));
        /* no interval by duration: VideoToolbox keyed c_23976 every 23
         * frames by it, and r_vfr and r_ts1000 off their GOP. Keyframes are
         * forced by sample count below, as ffmpeg's -g -keyint_min does. */
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFNumberRef)@((double)c->num / c->den));
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef)@((double)c->w * c->h * c->num / c->den * ((c->flags & F_NOISE) ? 0.6 : 0.15)));
    }
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_ColorPrimaries, prim);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_TransferFunction, trc);
    VTSessionSetProperty(vt, kVTCompressionPropertyKey_YCbCrMatrix, mat);
    if (c->flags & F_SAR)
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_PixelAspectRatio, (__bridge CFDictionaryRef)@{
            (id)kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing: @2, (id)kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing: @1 });
    VTCompressionSessionPrepareToEncodeFrames(vt);
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(vt);
    int64_t step = (int64_t)c->den * c->ts;   /* frame i at i * den * ts / num ticks */
    int written = 0;
    for (int i = 0; i < c->frames; i++) {
        if ((c->flags & F_VFR) && i % 7 == 3) continue;   /* every 7th dropped, times kept */
        CVPixelBufferRef pb = NULL;
        if (CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess) { fprintf(stderr, "pool\n"); return 1; }
        CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, prim, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, trc, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, mat, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocation_Left, kCVAttachmentMode_ShouldPropagate);
        fill(pb, c, i);
        int64_t v = ((int64_t)i * step * 2 + c->num) / (2 * (int64_t)c->num);   /* rounded: r_ts1000 */
        if (c->flags & F_OFFSET) v += c->ts / 10;
        CMTime pts = CMTimeMake(v, c->ts);
        int key = written % c->gop == 0 || ((c->flags & F_KEY17) && i == 17);
        NSDictionary* fp = key ? @{ (id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES } : nil;
        st = VTCompressionSessionEncodeFrame(vt, pb, pts, CMTimeMake(step / c->num > 0 ? step / c->num : 1, c->ts), (__bridge CFDictionaryRef)fp, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st != noErr) { fprintf(stderr, "%s: encode %d: %d\n", c->name, i, (int)st); return 1; }
        written++;
    }
    VTCompressionSessionCompleteFrames(vt, kCMTimeInvalid);
    VTCompressionSessionInvalidate(vt);
    CFRelease(vt);
    if ((int)g_out.count != written) { fprintf(stderr, "%s: %d samples for %d frames\n", c->name, (int)g_out.count, written); return 1; }

    NSString* path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.%s", c->name, c->ext ? c->ext : "mp4"]];
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    NSError* err = nil;
    AVAssetWriter* w = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path]
                                                fileType:(c->ext && !strcmp(c->ext, "mov")) ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4 error:&err];
    if (!w) { fprintf(stderr, "%s: %s\n", c->name, err.description.UTF8String); return 1; }
    w.shouldOptimizeForNetworkUse = YES;
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription((__bridge CMSampleBufferRef)g_out[0]);
    AVAssetWriterInput* vin = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:nil sourceFormatHint:fd];
    vin.expectsMediaDataInRealTime = NO;
    vin.mediaTimeScale = c->ts;
    if (c->flags & F_ROT) vin.transform = (CGAffineTransform){ 0, 1, -1, 0, 0, 0 };   /* 90 degrees */
    [w addInput:vin];
    AVAssetWriterInput* ain = nil;
    CMAudioFormatDescriptionRef afd = NULL;
    if (c->flags & F_AUDIO) {
        AudioStreamBasicDescription asbd = { 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 4, 1, 4, 2, 16, 0 };
        CMAudioFormatDescriptionCreate(NULL, &asbd, 0, NULL, 0, NULL, NULL, &afd);
        ain = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
            AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @48000, AVNumberOfChannelsKey: @2, AVEncoderBitRateKey: @128000 }];
        ain.expectsMediaDataInRealTime = NO;
        [w addInput:ain];
    }
    if (![w startWriting]) { fprintf(stderr, "%s: startWriting: %s\n", c->name, w.error.description.UTF8String); return 1; }
    [w startSessionAtSourceTime:kCMTimeZero];
    int vi = 0;
    int64_t ai = 0, an = (int64_t)c->frames * c->den * 48000 / c->num;
    while (vi < (int)g_out.count || (ain && ai < an)) {
        int did = 0;
        if (vi < (int)g_out.count && vin.readyForMoreMediaData) {
            if (![vin appendSampleBuffer:(__bridge CMSampleBufferRef)g_out[vi]]) { fprintf(stderr, "%s: append %d: %s\n", c->name, vi, w.error.description.UTF8String); return 1; }
            if (++vi == (int)g_out.count) [vin markAsFinished];
            did = 1;
        }
        if (ain && ai < an && ain.readyForMoreMediaData) {
            int n = (int)(an - ai < 1024 ? an - ai : 1024);
            CMSampleBufferRef sb = pcm(afd, ai, n);
            if (![ain appendSampleBuffer:sb]) { fprintf(stderr, "%s: audio: %s\n", c->name, w.error.description.UTF8String); return 1; }
            CFRelease(sb);
            ai += n;
            if (ai == an) [ain markAsFinished];
            did = 1;
        }
        if (!did) [NSThread sleepForTimeInterval:0.001];
    }
    if (afd) CFRelease(afd);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (w.status != AVAssetWriterStatusCompleted) { fprintf(stderr, "%s: %s\n", c->name, w.error.description.UTF8String); return 1; }
    printf("%s: %d frames\n", path.UTF8String, written);
    g_out = nil;
    return 0;
}

/* A WAV of `frames` stereo 16-bit frames at 48 kHz: a 440 Hz sine. */
static int wav(NSString* dir, const char* name, int64_t frames, double amp) {
    NSString* path = [dir stringByAppendingPathComponent:[NSString stringWithUTF8String:name]];
    FILE* f = fopen(path.fileSystemRepresentation, "wb");
    if (!f) return 1;
    uint32_t data = (uint32_t)(frames * 4), riff = 36 + data;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                      0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    memcpy(h + 4, &riff, 4); memcpy(h + 40, &data, 4);
    fwrite(h, 1, 44, f);
    for (int64_t k = 0; k < frames; k++) {
        int16_t v = (int16_t)lrint(amp * 32767.0 * sin(2.0 * M_PI * 440.0 * (double)k / 48000.0));
        int16_t s[2] = { v, v };
        fwrite(s, 2, 2, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: make_video_clips_mac OUTDIR [--long | --av]\n"); return 2; }
        NSString* dir = [NSString stringWithUTF8String:argv[1]];
        const char* opt = argc > 2 ? argv[2] : "";
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        g_lock = [NSLock new];
        const CMVideoCodecType H = kCMVideoCodecType_H264, V = kCMVideoCodecType_HEVC;
        static const clip base[] = {
            { "c_720p30",   1280, 720,  30, 1, 300, 30, 30000, 0, 0, NULL },
            { "c_1080p30",  1920, 1080, 30, 1, 300, 30, 30000, 0, 0, NULL },
            { "c_1080p60",  1920, 1080, 60, 1, 600, 60, 60000, 0, 0, NULL },
            { "c_2997",     1280, 720,  30000, 1001, 150, 15, 30000, 0, 0, NULL },
            { "c_23976",    1280, 720,  24000, 1001, 96, 24, 24000, 0, 0, NULL },
            { "c_crop",     1280, 718,  30, 1, 60, 30, 30000, 0, 0, NULL },
            { "c_mixcolor", 320,  240,  30, 1, 30, 30, 30000, 0, F_MIXCOLOR | F_NOBARS, NULL },
            { "c_hevc",     1280, 720,  30, 1, 120, 30, 30000, 1, 0, NULL },
            { "c_audio",    1280, 720,  30, 1, 90, 30, 30000, 0, F_AUDIO, NULL },
            { "r_bframes",  1280, 720,  30, 1, 90, 30, 30000, 0, F_BFRAMES, NULL },
            { "r_vfr",      1280, 720,  30, 1, 90, 30, 30000, 0, F_VFR, NULL },
            { "r_gop",      1280, 720,  30, 1, 90, 30, 30000, 0, F_KEY17, NULL },
            { "r_10bit",    1280, 720,  30, 1, 60, 30, 30000, 1, F_10BIT, NULL },
            { "r_sar",      1280, 720,  30, 1, 60, 30, 30000, 0, F_SAR, NULL },
            { "r_jpeg",     1280, 720,  30, 1, 30, 30, 30000, 2, 0, "mov" },
            { "r_rotation", 1280, 720,  30, 1, 60, 30, 30000, 0, F_ROT, NULL },
            { "r_ts1000",   1280, 720,  30000, 1001, 150, 15, 1000, 0, 0, NULL },
            { "r_offset",   1280, 720,  30, 1, 60, 30, 30000, 0, F_OFFSET, NULL },
        };
        static const clip lng[] = {
            { "m_720p30",  1280, 720,  30, 1, 1800, 30, 30000, 0, F_NOISE, NULL },
            { "m_1080p30", 1920, 1080, 30, 1, 1800, 30, 30000, 0, F_NOISE, NULL },
            { "m_1080p60", 1920, 1080, 60, 1, 3600, 60, 60000, 0, F_NOISE, NULL },
        };
        static const clip av[] = {
            { "m_av10min", 1920, 1080, 30, 1, 18000, 30, 30000, 0, F_NOISE, NULL },
        };
        const CMVideoCodecType codecs[] = { H, V, kCMVideoCodecType_JPEG };
        for (size_t i = 0; i < sizeof base / sizeof base[0]; i++) {
            clip c = base[i];
            c.codec = codecs[c.codec];
            if (make(dir, &c)) return 1;
        }
        if (wav(dir, "c_720p30.wav", 480000, 0.25)) return 1;
        if (!strcmp(opt, "--long"))
            for (size_t i = 0; i < sizeof lng / sizeof lng[0]; i++) { clip c = lng[i]; c.codec = H; if (make(dir, &c)) return 1; }
        if (!strcmp(opt, "--av")) {
            clip c = av[0]; c.codec = H;
            if (make(dir, &c) || wav(dir, "m_av10min.wav", 28800000, 0.03)) return 1;
        }
        printf("clips in %s\n", argv[1]);
    }
    return 0;
}
