/* ysp_video.c - MEX ysp_video: movies (ysp/video.h), the index maker and frame sequences
 *
 * A thin file: the commands are the core's (ysp_vis.c), which this file
 * loads from its own directory and calls (ysp_vis_mex.h, LIFETIME). Build
 * with build.m; ysp_vis.<mexext> and ANGLE's two DLLs must sit beside it.
 */
#define YV_THIN YV_WHICH_VIDEO
#define YV_THIN_NAME "ysp_video"
#include "ysp_vis_mex.h"
