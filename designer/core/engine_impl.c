/* engine_impl.c - every header's implementation, once, for the player and the
 * pack tool linked into one program.
 *
 * Each library compiles its own implementations (player/impl.c,
 * pack/pack_impl.c), so the two libraries cannot both be linked: every
 * ysp/json.h, ysp/pack.h and ysp/experiment.h function would be defined
 * twice. The designer instead compiles the libraries' other sources (see
 * designer/CMakeLists.txt) with this file: the player's implementations,
 * unchanged, and the three that only the pack tool needs. They come in this
 * translation unit because ysp/video.h's implementation also implements
 * ysp/gfx.h and ysp/timeline.h unless their guards are already set here. The
 * pack tool compiles ysp/gfx.h without SDL and ysp/audio.h without miniaudio;
 * those macros change only the implementations, not a declaration, and the
 * tool calls neither backend. */
#include "impl.c"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"
