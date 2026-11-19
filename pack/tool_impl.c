/* The implementations of the ysp headers that only the pack tool uses, in
 * ysp_pack_tool_code with the tool's own sources. ysp/video.h for
 * yvid_index_make() and the frame sequence format, with pl_mpeg
 * (third_party/pl_mpeg) for MPEG-1 and Media Foundation for MP4 on Windows.
 * ysp/outline.h for curve sets from fonts and SVG artwork.
 *
 * ysp/video.h's implementation also implements ysp/gfx.h and ysp/timeline.h
 * unless their guards are set. They are set here: those two come from
 * pack_impl.c (ypak) or player/impl.c (a program with the player), and a
 * second copy would not link. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION_GUARD
#define YSP_TIMELINE_IMPLEMENTATION_GUARD
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
