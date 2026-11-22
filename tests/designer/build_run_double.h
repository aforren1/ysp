/* build_run_double.h - a test double of Build (designer/core/build.c) and
 * Run (run.c) for test_designer_ui's step 6 part, which part S wrote. OWNED
 * BY PART S.
 *
 * Why: ui.cpp's Build and Run (the toolbar, the merged report, Run's Build
 * first, the run's end, the quit question, the redraw rate) are frozen
 * after part S, and the stubs never give a result. build_run_double.c
 * defines every external symbol of build.c and run.c, and is linked into
 * test_designer_ui before ysp_designer_core, so the linker takes neither
 * file's object from the library: the real B and R must define nothing
 * else that another file uses. test_designer_ui_real does not link it
 * (DSG_BUILD_RUN_DOUBLE 0) and runs on the library's build.c and run.c.
 *
 * A Build's result is the script's, given at the take after its post; a run
 * is a record of its argv that ends when the test says. dsg_build_path(),
 * dsg_build_stale() and dsg_build_check_pack() are the stub's. */
#ifndef YSP_DESIGNER_BUILD_RUN_DOUBLE_H
#define YSP_DESIGNER_BUILD_RUN_DOUBLE_H

#ifndef DSG_BUILD_RUN_DOUBLE
#define DSG_BUILD_RUN_DOUBLE 1
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clears the script and the counters. */
void brd_reset(void);
/* The next Builds' result: status (DSG_BUILD_*) with n_errors issues "build
 * error K" and then n_warnings "warning: build warning K", in that order,
 * the errors with a place ("line 2, column 3"). */
void brd_build_script(int status, int n_errors, int n_warnings);
int  brd_build_posts(void);
uint64_t brd_build_revision(void);       /* the last post's                  */
const char* brd_build_out(void);         /* the last post's out_path         */
/* Runs started; the last one's argv (i < 0 or past its end: NULL). */
int  brd_run_starts(void);
const char* brd_run_arg(int i);
bool brd_run_sim(void);                  /* the last start's desc.sim        */
int  brd_run_stops(void);
int  brd_run_kills(void);
/* The last run ends at its next tick with this status; issue: its message
 * (NULL: none). */
void brd_run_end(int status, const char* issue);
/* As the watch thread: calls the last run's desc.wake. */
void brd_run_wake(void);
/* The run's issue says rig true. */
void brd_run_issue_rig(bool rig);

#ifdef __cplusplus
}
#endif
#endif
