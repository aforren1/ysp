// Compile check: ysp/video.h's own copies of the CoreMedia, CoreVideo,
// CoreGraphics and VideoToolbox types and constants its AVFoundation backend
// uses, against the macOS SDK's.
//
// The header loads those frameworks at run time and includes none of their
// headers (CoreVideo's would bring ApplicationServices into the caller's
// translation unit), so its struct layouts and constants are copies. A wrong
// one would fail only at run time, on a rig, as a wrong time or a refused
// frame; here it is a compile error. At run time it also loads the
// frameworks and checks that every symbol the header looks up resolves to
// the SDK's own function or constant. AVAssetReader's status values
// (Reading 1, Completed 2) are in an Objective-C header and are not checked
// here; the tests read them at every open and end. Off Apple, or with
// YVID_NO_AVF, it does nothing and returns 0.
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG   // not about MPEG-1
#endif
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>

#if YVID__AVF
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stddef.h>

#define SAME_LAYOUT(mine, sdk) static_assert(sizeof(mine) == sizeof(sdk) && alignof(mine) == alignof(sdk), #mine " is not " #sdk)
#define SAME_OFFSET(mine, mf, sdk, sf) static_assert(offsetof(mine, mf) == offsetof(sdk, sf), #mine "." #mf " is not " #sdk "." #sf)

SAME_LAYOUT(yvid__cmtime, CMTime);
SAME_OFFSET(yvid__cmtime, value, CMTime, value);
SAME_OFFSET(yvid__cmtime, timescale, CMTime, timescale);
SAME_OFFSET(yvid__cmtime, flags, CMTime, flags);
SAME_OFFSET(yvid__cmtime, epoch, CMTime, epoch);
SAME_LAYOUT(yvid__cmrange, CMTimeRange);
SAME_OFFSET(yvid__cmrange, duration, CMTimeRange, duration);
SAME_LAYOUT(yvid__cgrect, CGRect);
static_assert(offsetof(yvid__cgrect, w) == offsetof(CGRect, size), "CGRect.size");
SAME_LAYOUT(yvid__affine, CGAffineTransform);
SAME_OFFSET(yvid__affine, b, CGAffineTransform, b);
SAME_OFFSET(yvid__affine, c, CGAffineTransform, c);
SAME_OFFSET(yvid__affine, d, CGAffineTransform, d);
SAME_LAYOUT(yvid__dims, CMVideoDimensions);
SAME_OFFSET(yvid__dims, h, CMVideoDimensions, height);
SAME_LAYOUT(yvid__vtcb, VTDecompressionOutputCallbackRecord);
SAME_OFFSET(yvid__vtcb, ref, VTDecompressionOutputCallbackRecord, decompressionOutputRefCon);
static_assert(sizeof(CGFloat) == sizeof(double), "CGFloat is double");
static_assert(sizeof(CFIndex) == sizeof(long) && sizeof(CFTypeID) == sizeof(unsigned long), "CFIndex, CFTypeID");
static_assert(sizeof(OSStatus) == 4 && sizeof(CVOptionFlags) == 8 && sizeof(VTDecodeFrameFlags) == 4, "status and flag widths");

static_assert(YVID__CMT_VALID == kCMTimeFlags_Valid, "kCMTimeFlags_Valid");
static_assert(YVID__CMT_POS_INF == kCMTimeFlags_PositiveInfinity, "kCMTimeFlags_PositiveInfinity");
static_assert((kCMTimeFlags_Valid | kCMTimeFlags_ImpliedValueFlagsMask) == 29u, "the numeric test in the index maker");
static_assert(YVID__CMT_ROUND == kCMTimeRoundingMethod_RoundHalfAwayFromZero, "rounding");
static_assert(YVID__CF_SINT32 == kCFNumberSInt32Type && YVID__CF_SINT64 == kCFNumberSInt64Type, "CFNumber types");
static_assert(YVID__CF_UTF8 == kCFStringEncodingUTF8, "UTF-8");
static_assert(YVID__CV_LOCK_RO == kCVPixelBufferLock_ReadOnly, "read-only lock");
static_assert(YVID__VT_NO_OUTPUT == kVTDecodeFrame_DoNotOutputFrame, "kVTDecodeFrame_DoNotOutputFrame");
static_assert(YVID__PF_420V == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, "420v");
static_assert(YVID__PF_420F == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, "420f");
static_assert(YVID__CC_H264 == kCMVideoCodecType_H264 && YVID__CC_HEVC == kCMVideoCodecType_HEVC, "codec types");
static_assert(YVID__CC_AVC1 == 'avc1' && YVID__CC_AVC3 == 'avc3' && YVID__CC_HVC1 == 'hvc1' && YVID__CC_HEV1 == 'hev1', "sample entries");
static_assert(DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM == 1, "the loader queue's autorelease frequency");

static int g_bad;
#define SAME_FN(slot, fn) do { if ((const void*)yvid__av.slot != (const void*)&fn) { fprintf(stderr, "video_avf: %s is not the SDK's\n", #slot); g_bad++; } } while (0)
#define SAME_K(slot, k) do { if (yvid__av.slot != (const void*)(k)) { fprintf(stderr, "video_avf: %s is not %s\n", #slot, #k); g_bad++; } } while (0)

int main() {
    char err[256];
    if (yvid__avf_load(err, sizeof err) < 0) { fprintf(stderr, "video_avf: %s\n", err); return 1; }
    SAME_FN(CFRelease, CFRelease); SAME_FN(CMTimeConvertScale, CMTimeConvertScale);
    SAME_FN(CMSampleBufferGetPresentationTimeStamp, CMSampleBufferGetPresentationTimeStamp);
    SAME_FN(CMVideoFormatDescriptionGetCleanAperture, CMVideoFormatDescriptionGetCleanAperture);
    SAME_FN(CVPixelBufferGetBaseAddressOfPlane, CVPixelBufferGetBaseAddressOfPlane);
    SAME_FN(VTDecompressionSessionDecodeFrame, VTDecompressionSessionDecodeFrame);
    SAME_FN(VTIsHardwareDecodeSupported, VTIsHardwareDecodeSupported);
    SAME_K(kCFBooleanTrue, kCFBooleanTrue); SAME_K(kCFAllocatorMalloc, kCFAllocatorMalloc);
    SAME_K(keyCB, &kCFTypeDictionaryKeyCallBacks); SAME_K(valueCB, &kCFTypeDictionaryValueCallBacks);
    SAME_K(NotSync, kCMSampleAttachmentKey_NotSync);
    SAME_K(FullRange, kCMFormatDescriptionExtension_FullRangeVideo);
    SAME_K(Atoms, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    SAME_K(Matrix, kCMFormatDescriptionExtension_YCbCrMatrix);
    SAME_K(Transfer, kCMFormatDescriptionExtension_TransferFunction);
    SAME_K(Primaries, kCMFormatDescriptionExtension_ColorPrimaries);
    SAME_K(Chroma, kCMFormatDescriptionExtension_ChromaLocationTopField);
    SAME_K(FieldCount, kCMFormatDescriptionExtension_FieldCount);
    SAME_K(PAR, kCMFormatDescriptionExtension_PixelAspectRatio);
    SAME_K(PARh, kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing);
    SAME_K(M601, kCVImageBufferYCbCrMatrix_ITU_R_601_4); SAME_K(TsRGB, kCVImageBufferTransferFunction_sRGB);
    SAME_K(PEBU, kCVImageBufferColorPrimaries_EBU_3213); SAME_K(CLeft, kCVImageBufferChromaLocation_Left);
    SAME_K(RequireHW, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder);
    SAME_K(UsingHW, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder);
    SAME_FN(dispatch_async_f, dispatch_async_f); SAME_FN(dispatch_sync_f, dispatch_sync_f);
    SAME_FN(dispatch_queue_create, dispatch_queue_create);
    if (DISPATCH_QUEUE_SERIAL != NULL) { fprintf(stderr, "video_avf: DISPATCH_QUEUE_SERIAL is not NULL\n"); g_bad++; }
    if (g_bad) return 1;
    printf("video_avf: layouts, constants and symbols match the SDK\n");
    return 0;
}
#else
int main() { return 0; }
#endif
