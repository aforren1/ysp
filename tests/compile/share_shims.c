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
 * Build (repo root), for example:
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

/* HEADER LIMIT (video.h 0.5.0, line 6044, yvid__snd_close): the soundtrack
 * part calls YAU__NOW() and YAU__SLEEP_UNTIL(), private macros of audio.h's
 * IMPLEMENTATION block, so it does not compile beside audio.h's
 * declarations alone. These are audio.h's own definitions off the web
 * (audio.h 2403, 2420); remove them when video.h stops using the macros. */
#ifndef YAU__NOW
#define YAU__NOW() (yrt_now_ns())
#define YAU__SLEEP_UNTIL(t) yrt_sleep_until((int64_t)(t), 0)
#endif

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
