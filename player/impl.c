/* player/impl.c - every header's implementation, once for the player.
 * Order matters: ysp/gfx.h implements ysp/screen.h and ysp/color.h; ysp/
 * response.h needs ysp/trials.h before it for YRSP_DATA_COLS; ysp/device.h
 * comes first where a glue needs it. */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"
#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
