/* preview_double.h - a test double of the preview (designer/core/preview.h)
 * for the pane's tests (part B of designer step 5). OWNED BY PART B.
 *
 * Why: parts A (preview.c) and B (the pane) are built at the same time, and
 * S's stub never shows a session, so the pane's states cannot be reached
 * through it. preview_double.c defines every call of preview.h. It is linked
 * into test_designer_ui and test_designer_te before ysp_designer_core, so
 * the linker never takes preview.c's object from the library.
 *
 * Off (the default), the double acts as S's stub, so the tests of the other
 * parts see no change. On, it is a small preview: it posts real session
 * jobs to the worker (whose stub load succeeds) after the settle, adopts
 * them through ui.cpp, steps a frame grid at the display's rate and speed,
 * and turns a key or a choice into a trial's response.
 *
 * After part A merges, test_designer_te runs on A's preview.c: take
 * preview_double.c out of its sources in designer/CMakeLists.txt and define
 * DSG_PREVIEW_DOUBLE 0 at the top of te_preview.cpp. test_designer_ui keeps
 * the double: its parts 9 and 10 drive states that A's preview reaches only
 * through a fault (a refusal at run time, a texture with no GL). Defined 0
 * for preview_double.c too, the double compiles to nothing and the linker
 * takes preview.c (docs/designer.md, "Part B as built"). */
#ifndef YSP_DESIGNER_PREVIEW_DOUBLE_H
#define YSP_DESIGNER_PREVIEW_DOUBLE_H

#ifndef DSG_PREVIEW_DOUBLE
#define DSG_PREVIEW_DOUBLE 1
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On: a session after each accepted check. Off: S's stub. Each enable
 * resets the script below and the counters. */
void pvd_enable(bool on);

/* The script: what A's preview would read from the definition. */
void pvd_set_choices(int n, const char* const* labels);
void pvd_set_undriven(int n, const char* const* roles, const char* const* why);
void pvd_set_leaves(int n, const char* const* paths, const char* const* tmpls, const int* rows);
/* The role of the code that each response sends (on_code); NULL: none. */
void pvd_set_output_role(const char* role);
/* A GL texture name for dsg_preview_texture() (0: none). */
void pvd_set_texture(uint32_t name);
/* At the next tick the shown session refuses at run time with this text. */
void pvd_fail(const char* message);
/* At the next tick the shown session ends. */
void pvd_end(void);

/* What the pane gave the preview. */
int  pvd_posts(void);              /* session jobs posted                     */
int  pvd_adopted(void);            /* sessions shown                          */
int  pvd_steps(void);              /* frames stepped                          */
int  pvd_pointer_calls(void);
void pvd_pointer(float* x, float* y, bool* inside);
int  pvd_choice_last(void);        /* the last choice pressed; -1 none        */
int  pvd_restarts(void);           /* dsg_preview_restart() calls             */

#ifdef __cplusplus
}
#endif
#endif
