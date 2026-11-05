/* ysp_screen.c - MEX ysp_screen: ysp/screen.h: open, the frame loop (begin, flip, flip_at, wait_flip), flip records, input events, window calls
 *
 * A thin file: the commands are the core's (ysp_vis.c), which this file
 * loads from its own directory and calls (ysp_vis_mex.h, LIFETIME). Build
 * with build.m; ysp_vis.<mexext> and ANGLE's two DLLs must sit beside it.
 */
#define YV_THIN YV_WHICH_SCREEN
#define YV_THIN_NAME "ysp_screen"
#include "ysp_vis_mex.h"
