/* ysp_gfx.c - MEX ysp_gfx: ysp/gfx.h: phase A's draw path (open on a screen, begin, rect, end)
 *
 * A thin file: the commands are the core's (ysp_vis.c), which this file
 * loads from its own directory and calls (ysp_vis_mex.h, LIFETIME). Build
 * with build.m; ysp_vis.<mexext> and ANGLE's two DLLs must sit beside it.
 */
#define YV_THIN YV_WHICH_GFX
#define YV_THIN_NAME "ysp_gfx"
#include "ysp_vis_mex.h"
