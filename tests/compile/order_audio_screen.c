/* Compile check: the audio, display and video implementations in one
 * translation unit, in both orders, with warnings as errors.
 *
 * On Windows each header brings its own system headers: miniaudio under
 * WIN32_LEAN_AND_MEAN, ysp/screen.h's D3D11 and DXGI, ysp/video.h's Media
 * Foundation. A macro one of them defines and a later one redefines is a
 * warning (MSVC C4005) and fails a /WX build; audio.h before screen.h did
 * that with CLSCTX_ALL up to audio.h 0.7.0. Each header's own compile check
 * cannot see it, because the conflict needs two headers in one unit.
 *
 * YSP_TEST_SCREEN_FIRST picks the order: unset, ysp/audio.h first (the
 * order ysp/video.h asks for); set, ysp/screen.h first. ysp/video.h comes
 * last in both, after ysp/audio.h, so its soundtrack part is compiled too.
 * order_audio_screen.cpp builds the same source as C++17. */
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG   /* not about MPEG-1 */
#endif

#ifdef YSP_TEST_SCREEN_FIRST
    #define YSP_SCREEN_IMPLEMENTATION
    #include "ysp/screen.h"
    #define YSP_AUDIO_IMPLEMENTATION
    #include "ysp/audio.h"
#else
    #define YSP_AUDIO_IMPLEMENTATION
    #include "ysp/audio.h"
    #define YSP_SCREEN_IMPLEMENTATION
    #include "ysp/screen.h"
#endif
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>

int main(void) {
    /* One function from each part, so the linker needs every one. */
    bool (*au)(yau_audio*, const yau_desc*) = yau_open;
    bool (*scr)(yscr_screen*, const yscr_desc*) = yscr_open;
    bool (*vid)(yvid_movie*, ygfx_gfx*, const yvid_desc*) = yvid_open;
    int (*follow)(yvid_movie*, yau_audio*, const yscr_frame*) = yvid_follow_audio;
    if (!au || !scr || !vid || !follow) return 1;
#ifdef YSP_TEST_SCREEN_FIRST
    printf("order_audio_screen: screen.h, audio.h, video.h in one unit\n");
#else
    printf("order_audio_screen: audio.h, screen.h, video.h in one unit\n");
#endif
    return 0;
}
