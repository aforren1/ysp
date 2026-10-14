/* The implementations of the ysp headers the pack tool uses, compiled once
 * for the library (pack/ysp/pack_tool.h). ysp/gfx.h without SDL: the tool
 * needs its shader wrapper and curve set check, never a window; its
 * implementation brings ysp/screen.h's, ysp/color.h's and ysp/rt.h's.
 * ysp/video.h for yvid_index_make() and the frame sequence format, with
 * pl_mpeg (third_party/pl_mpeg) for MPEG-1 and Media Foundation for MP4 on
 * Windows; it brings ysp/timeline.h's.
 * ysp/audio.h without miniaudio: the tool needs the WAV probe only.
 * ysp/json.h reads and writes every description and manifest. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_TABLE_IMPLEMENTATION
#include "ysp/table.h"
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
