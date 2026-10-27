/* Link check: ysp/video.h's and ysp/screen.h's implementations with the
 * shims of bindings/share/ instead of audio.h's and timeline.h's.
 *
 * ysp-vis compiles screen, gfx and video, and takes audio and timeline from
 * ysp.audio and ysp.timeline through their tables (docs/bindings.md,
 * "Sharing one implementation across modules"). This unit is that build
 * without the host: audio.h and timeline.h are declarations only, and the
 * shims define every yau_* and ytl_* function the table lists. The link
 * fails when video.h, gfx.h or screen.h calls a function the tables do not
 * forward, and the compile fails when a shim's signature differs from the
 * header's. main() does not call a forwarder: no table is loaded.
 *
 * CMake builds it as compile_share_shims_c and _cxx (with SDL3). By hand,
 * from the repo root, for example:
 *   gcc -std=c11 -Wall -Wextra -Werror -Iinclude -Ibindings/share -DYSCR_NO_SDL \
 *       tests/compile/share_shims.c -o share_shims -lm -lpthread -ldl
 *   cl /W4 /WX /Iinclude /Ibindings\share /DYSCR_NO_SDL tests\compile\share_shims.c
 */
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG   /* not about MPEG-1 */
#endif
#define YTL_MAX_CHANNELS 1024           /* as ysp.timeline and ysp_timeline */

/* Declarations only: the owners' modules hold the implementations. */
#include "ysp/audio.h"
#include "ysp/timeline.h"
#define YSP_TIMELINE_IMPLEMENTATION_GUARD   /* video.h must not pull timeline.h's in */
#define YSP_AUDIO_API_SHIMS
#include "ysp_audio_api.h"
#define YSP_TIMELINE_API_SHIMS
#include "ysp_timeline_api.h"

/* No YAU__* macro here: video.h (from 0.5.1) calls audio.h's public
 * functions only. A use of a private name of audio.h's implementation
 * fails this compile. */

#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>

int main(void) {
    /* The soundtrack and the timeline follow are the parts that call the
     * forwarders; take their addresses so the linker keeps them. */
    int (*snd)(yvid_movie*, yau_audio*, const yvid_soundtrack_desc*) = yvid_soundtrack;
    int (*follow)(yvid_movie*, yau_audio*, const yscr_frame*) = yvid_follow_audio;
    bool (*vid)(yvid_movie*, ygfx_gfx*, const yvid_desc*) = yvid_open;
    if (!snd || !follow || !vid || ysp_audio_api_ptr || ysp_timeline_api_ptr) return 1;
    printf("share_shims: video.h, gfx.h and screen.h link against the audio and timeline shims\n");
    return 0;
}
