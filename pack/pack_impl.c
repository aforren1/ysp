/* The implementations of the ysp headers that the pack tool shares with the
 * player, compiled once for the pack tool alone (ysp_pack_impl, linked by
 * ysp_pack_tool). ysp/gfx.h without SDL: the tool needs its shader wrapper
 * and curve set check, never a window; its implementation brings
 * ysp/screen.h's, ysp/color.h's and ysp/rt.h's. ysp/timeline.h for
 * ysp/video.h. ysp/audio.h without miniaudio: the tool needs the WAV probe
 * only. ysp/json.h reads and writes every description and manifest.
 *
 * Every header here is also in player/impl.c, which a program with the
 * player and the tool links in place of this file (docs/designer.md). A
 * header that the player does not implement goes in tool_impl.c. */
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
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_TABLE_IMPLEMENTATION
#include "ysp/table.h"
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
