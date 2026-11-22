/* run_view_double.h - a test double of Build (designer/core/build.c), Run
 * (run.c) and the rig profiles (rig.c) for the Run view's Test Engine tests
 * (te_run.cpp). OWNED BY PART V (designer step 6).
 *
 * Why: parts B, R and G were built at the same time as the Run view, so its
 * tests ran on this double first. Part S's build_run_double.c gives no
 * trials, no log, no exit text, no table place and no rig, which the Run
 * view's tests read. run_view_double.c defines every external symbol of
 * build.c, run.c and rig.c, and is in test_designer_te's sources, so the
 * linker takes none of the three files' objects from ysp_designer_core.
 *
 * Take it out at the integration (docs/designer.md, "Part V as built"):
 * define DSG_RUN_VIEW_DOUBLE 0 at the top of te_run.cpp and remove
 * run_view_double.c from test_designer_te's sources. With 0 this file and
 * run_view_double.c compile to nothing.
 *
 * Build: a result as scripted, at the take after its post (no thread). Run:
 * no process; the progress, the end and what Stop does are scripted, and
 * each step calls the run's wake as the watch thread would. The exit texts
 * are the plan's ("What the Run view shows"). Rig: the profiles of
 * $XDG_CONFIG_HOME/ysp/rig (Windows: %APPDATA%\ysp\rig), loaded with
 * yrig_load_file(). Unlike yrig_dir(), the double does not refuse a folder
 * under one that all users can write, so it runs from a build tree under
 * /tmp; the real rig.c does refuse one. */
#ifndef YSP_DESIGNER_RUN_VIEW_DOUBLE_H
#define YSP_DESIGNER_RUN_VIEW_DOUBLE_H

#ifndef DSG_RUN_VIEW_DOUBLE
#define DSG_RUN_VIEW_DOUBLE 1
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clears the script and the counters: Builds succeed, Stop reaches the
 * player and does not end it. */
void rvd_reset(void);
/* The next Builds' result: status (DSG_BUILD_*), an error with its message
 * (NULL: none) at the definition's line and column (line 0: no place, as a
 * table's message has), and a warning (NULL: none). */
void rvd_build_script(int status, const char* error, int line, int column, const char* warning);
int  rvd_build_posts(void);
int  rvd_run_starts(void);
/* The last start's argv[i]; NULL past its end. */
const char* rvd_run_arg(int i);
int  rvd_run_stops(void);
int  rvd_run_kills(void);
/* At the run's next tick: trials rows written, the last row's response,
 * outcome and rt (s), and one line added to the log (NULL: none). Wakes. */
void rvd_run_progress(int trials, const char* response, const char* outcome, double rt_s, const char* log_line);
/* The run ends at its next tick with this status. Wakes. */
void rvd_run_end(int status);
/* false: Stop gives 0 at once and Kill is offered, as a --sim run on
 * Windows (Part P). true: Stop goes out. */
void rvd_run_stop_reaches(bool reaches);
/* true: a Stop that went out ends the run with 3 at the next tick, as
 * `ysp_player --sim` does on SIGTERM. */
void rvd_run_stop_ends(bool ends);

#ifdef __cplusplus
}
#endif
#endif
