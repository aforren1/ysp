/* preview.h - the designer's preview (docs/designer.md, "Plan of step 5"):
 * a template's states on the player's simulated display, in a pane beside
 * the main view. Faithful in logic, not in timing.
 *
 * FROZEN BY PART S. preview.c (part A) implements every call; the pane
 * (view_preview.cpp, part B) and the chart (view_chart.cpp, part C) call
 * them. A part that needs a change stops and reports it.
 *
 * C, no SDL. The GL calls are the player's (pl_sim_attach(), pl_sim_frame()
 * with draw, pl_sim_detach()), on the UI thread, inside ui_frame(), with
 * the window's context current.
 *
 * Threads. Every call here runs on the UI thread, except the session ops
 * of dsg_preview_session_ops(), which run on the check thread (worker.c).
 * The two share only what dsg_preview_gl fixes before the worker starts and
 * what travels in a dsg_session: the UI thread posts a session job with
 * dsg_worker_session(); the check thread loads it (pl_sim_load()) and fast
 * forwards it to the pin; the UI thread takes it (dsg_worker_take_session())
 * and gives it to dsg_preview_adopt(), which attaches it over frames and
 * shows it; a session that is done with is detached here and handed back
 * with dsg_worker_close(), which closes it (pl_close()) on the check
 * thread.
 *
 * Pointers. A string or struct that a call gives stays valid until the next
 * call that changes the preview: dsg_preview_tick(), _adopt(), _checked(),
 * _open(), _close(), _set(), a control, or dsg_preview_free(). */
#ifndef YSP_DESIGNER_PREVIEW_H
#define YSP_DESIGNER_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsg.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dsg_doc;
struct ygfx_gl_caps;
struct ygfx_gfx;
struct ygfx_cache;

/* --- the GL context and the seam to the check thread ------------------------ */

/* yscr_proc's type (ysp/screen.h), so this header needs no ysp header. */
typedef void (*dsg_gl_fn)(void);

/* The GL context that the sessions draw on: main.cpp's first gfx, open on
 * the window's context before the worker starts (docs/designer.md, "Part S
 * as built"). The caller keeps it, unchanged, until dsg_worker_stop(): the
 * session ops read it on the check thread. caps NULL (the headless tests):
 * the sessions draw on gfx's null backend, and dsg_preview_texture() is
 * false. */
typedef struct dsg_preview_gl {
    const struct ygfx_gl_caps* caps;   /* pl_sim_desc.gl_caps                */
    struct ygfx_gfx* share;            /* pl_sim_desc.share: the first gfx   */
    const struct ygfx_cache* cache;    /* pl_sim_desc.cache; NULL: none      */
    dsg_gl_fn (*proc)(void* ctx, const char* name);   /* pl_sim_desc.gl_proc */
    void* proc_ctx;                    /* pl_sim_desc.gl_ctx                 */
} dsg_preview_gl;

/* The session job's load and close, for dsg_worker_desc.session. Both run
 * on the check thread. load: pl_sim_load() of the text against
 * dsg_pack_bytes(s->pack) with s->settings (its out_dir is the run's
 * folder, made by the load's data set), then the fast forward to the pin;
 * it puts the session in s->impl. close: pl_close() and what load made,
 * and the removal of old run folders (dsg_remove_tree()). ctx is gl, which
 * the caller keeps until dsg_worker_stop(). */
void dsg_preview_session_ops(const dsg_preview_gl* gl, dsg_session_ops* out);

/* --- the settings: the view file's part "preview" --------------------------- */

enum { DSG_PV_SCALE_FIT = 0, DSG_PV_SCALE_1TO1 = 1 };

/* What the person chooses. session.out_dir is not a setting: the preview
 * fills it for each run. */
typedef struct dsg_preview_settings {
    dsg_session_settings session;   /* display (width, height, hz), seed,
                                     * participant, rt_s, pin (pin_leaf,
                                     * pin_row; -1 any), fields              */
    double speed;                   /* 0.25, 0.5, 1 or 2                      */
    int    scale;                   /* DSG_PV_SCALE_*                          */
} dsg_preview_settings;

/* 1920 x 1080 at 60 Hz, seed 1, "correct" at 0.3 s, no pin, no fields,
 * speed 1, Fit (decisions 1 and 6). */
void dsg_preview_settings_default(dsg_preview_settings* s);
/* The view file's part "preview" (preview_part.c, part S): {"open": bool,
 * "display": [w, h], "hz": n, "speed": s, "seed": n, "participant": "...",
 * "rt_s": s, "pin": {"leaf": PATH, "row": n}, "fields": {NAME: "VALUE"},
 * "scale": "fit" or "1:1", "rig": NAME (step 6; "": the built-in rig)}. A key that is missing, of another type or out of
 * range takes its default, and so does every key when the part is missing
 * or does not parse. open: the pane is shown. */
void dsg_preview_part_read(const dsg_view* v, dsg_preview_settings* s, bool* open);
/* Writes every key, defaults too. 0, or -1 when the view file's parts are
 * full or out of memory. */
int  dsg_preview_part_write(dsg_view* v, const dsg_preview_settings* s, bool open);

/* --- the preview ------------------------------------------------------------- */

typedef struct dsg_preview dsg_preview;

/* gl, w and doc are the caller's and outlive the preview. NULL when out of
 * memory. */
dsg_preview* dsg_preview_new(const dsg_preview_gl* gl, dsg_worker* w, struct dsg_doc* doc);
/* Stops and detaches the sessions it holds and hands them to the worker
 * (dsg_worker_close()), so call it before dsg_worker_stop(), with the GL
 * context current. NULL is a no-op. */
void dsg_preview_free(dsg_preview* pv);

/* A project opened: its folder holds the runs (DSG_DIR/preview/run-N). The
 * document's revision now is the floor: a session of a lower revision is
 * another project's and adopt drops it. Settings stay as they are; the
 * caller sets the project's with dsg_preview_set(). */
void dsg_preview_open(dsg_preview* pv, const dsg_project* p);
/* The project closes: stops, detaches and hands back every session; drops
 * the issue, the outputs and the last check. */
void dsg_preview_close(dsg_preview* pv);

/* A check result that the UI took (ui.cpp, after its own use). The preview
 * keeps the revision, the status, the error count and a reference to the
 * pack when the status is DSG_CHECK_OK; it keeps no pointer into r. */
void dsg_preview_checked(dsg_preview* pv, const dsg_check_result* r, int64_t now_ns);
/* A session job's outcome that the UI took. A session that is stale (its
 * revision or settings are not the newest, or dsg_worker_session_queued())
 * is handed back at once and never attached; a newer one is attached over
 * the next frames while the shown one runs. A failed load becomes the
 * preview issue; the shown session stays. The preview owns r->session from
 * here on. */
void dsg_preview_adopt(dsg_preview* pv, dsg_session_result* r, int64_t now_ns);
/* Once a UI frame, before the views draw, with the GL context current.
 * shown: the pane is open and the window is not hidden; while false the
 * preview pauses, posts no session job and attaches nothing. Posts a
 * session job when the text has not changed for PV_SETTLE_MS and the check
 * accepted the current revision; attaches for at most PV_ATTACH_BUDGET_MS;
 * steps the frames due (at most PV_MAX_STEPS, the last one drawn). The
 * three knobs are preview.c's (300 ms, 4 ms, 16 in the plan). */
void dsg_preview_tick(dsg_preview* pv, int64_t now_ns, bool shown);
/* Draw every frame: a session runs and is shown. */
bool dsg_preview_animating(const dsg_preview* pv);
/* A frame is wanted though nothing animates: an attach in progress, a
 * session job to post after the settle, a step asked for. */
bool dsg_preview_work(const dsg_preview* pv);

/* --- controls ------------------------------------------------------------------ */

void dsg_preview_run(dsg_preview* pv);
void dsg_preview_pause(dsg_preview* pv);
/* One frame, drawn; then paused. */
void dsg_preview_step(dsg_preview* pv);
/* A session job with the same settings now, without the settle. */
void dsg_preview_restart(dsg_preview* pv);
/* New settings. A change of the display, seed, participant, rt, pin or
 * fields restarts; speed and scale take effect at once. Step 6: a change of
 * session.rig restarts too (part G, in preview.c's same_settings()). */
void dsg_preview_set(dsg_preview* pv, const dsg_preview_settings* s);
void dsg_preview_get(const dsg_preview* pv, dsg_preview_settings* out);
/* Pins the first leaf of the shown definition's timeline that runs template
 * tmpl, row any, and restarts ("Preview this template", the chart). false
 * when no leaf runs it. */
bool dsg_preview_pin_template(dsg_preview* pv, const char* tmpl);

/* --- input ----------------------------------------------------------------------- */

/* SDL_SCANCODE_ESCAPE: it gives the keyboard back to the designer. */
#define DSG_SCANCODE_ESCAPE 41

/* The pane claims or gives back the keyboard (a click on the display; the
 * pane losing focus). */
void dsg_preview_set_keyboard(dsg_preview* pv, bool on);
bool dsg_preview_keyboard(const dsg_preview* pv);
/* A key event with its SDL scancode (main.cpp, through ui_preview_key()).
 * true: the preview took it, so the designer does not see it. While the
 * keyboard is claimed every key is taken: Escape gives it back and does not
 * reach the session; another goes to the session when it runs (input while
 * paused is dropped). A key up whose down the session took goes to the
 * session even after the keyboard went back. */
bool dsg_preview_key(dsg_preview* pv, int scancode, bool down);
/* Choice i of the running template (the choice strip). */
void dsg_preview_choice(dsg_preview* pv, int i, bool down);
/* The cursor in participant px from the display's top-left corner, x
 * right, y down; inside false: it left the display. Each position2d role
 * that the preview drives gets a sample in its units. */
void dsg_preview_pointer(dsg_preview* pv, float x_px, float y_px, bool inside);

/* --- state ------------------------------------------------------------------------ */

enum {
    DSG_PV_NONE = 0,    /* no session: no project, nothing accepted yet       */
    DSG_PV_WAITING,     /* a session job waits or runs; none shown yet        */
    DSG_PV_ATTACHING,   /* the first session's GL objects are being made      */
    DSG_PV_RUNNING,
    DSG_PV_PAUSED,
    DSG_PV_ENDED,       /* the session's end: "Session over: N trials"        */
    DSG_PV_FAILED       /* refused at run time, or the first load failed: the
                         * preview issue says why                              */
};

typedef struct dsg_preview_info {
    int         status;        /* DSG_PV_* of the session shown              */
    bool        loading;       /* a session job waits or runs                 */
    bool        attaching;     /* a newer session is being attached           */
    bool        stale;         /* the text changed since the shown session's
                                * load and the newest check refused it         */
    int         stale_errors;  /* that check's errors                          */
    int64_t     shown_unix_s;  /* when the shown session's job was posted, for
                                * "Shows the definition as of HH:MM:SS"; 0:
                                * none                                         */
    bool        slow;          /* the last tick dropped frames: "slower than
                                * real time"                                  */
    int32_t     width, height, hz;   /* the shown session's display           */
    int         trials;        /* trials done                                  */
    const char* out_dir;       /* the shown session's data set; "" none        */
    const char* note;          /* the fast forward's stop, such as "row 5 was
                                * not drawn in this run of the leaf: seed 7";
                                * "" none                                      */
    bool        keyboard;      /* dsg_preview_keyboard()                       */
} dsg_preview_info;
void dsg_preview_get_info(const dsg_preview* pv, dsg_preview_info* out);

/* pl_sim_where's, without player.h. phase: PL_SIM_EVENTS, _RESPONSE,
 * _AFTER, _BETWEEN, which these equal. */
enum { DSG_PV_PHASE_EVENTS = 0, DSG_PV_PHASE_RESPONSE, DSG_PV_PHASE_AFTER, DSG_PV_PHASE_BETWEEN };
typedef struct dsg_preview_where {
    int         trial;         /* -1 before the first                         */
    const char* leaf;          /* NULL: none                                   */
    int         condition;     /* the table row; -1 a track trial              */
    const char* tmpl;          /* NULL between trials                          */
    const char* state;         /* version 2's state; NULL: the end, events     */
    int         phase;         /* DSG_PV_PHASE_*                               */
    int64_t     frame;
    int64_t     t_ns;
    const char* outcome;       /* the last trial's; NULL none                  */
    const char* response;
    int64_t     rt_ns;
} dsg_preview_where;
/* Where the shown session is; false with none (the chart's ring, Follow). */
bool dsg_preview_where_now(const dsg_preview* pv, dsg_preview_where* out);

/* The shown session's display texture: its GL name (GL_RGBA8, rows bottom
 * up, alpha 255) and size. false before the first attach and on the null
 * backend. */
bool dsg_preview_texture(const dsg_preview* pv, uint32_t* gl_name, int32_t* width, int32_t* height);

/* The preview issue: a session's refusal at run time, or a load's refusal
 * that the check did not give. from is DSG_FROM_PREVIEW; place is in the
 * text that the session loaded, as a check's issue's is in the text the
 * check read. false: none. */
bool dsg_preview_issue(const dsg_preview* pv, dsg_issue* out);

/* The codes the session sent (on_code), oldest first; the list keeps the
 * last DSG_PV_OUTPUTS. */
#define DSG_PV_OUTPUTS 64
typedef struct dsg_preview_output {
    const char* role;
    uint32_t    code;
    int64_t     t_ns;          /* on the session's frame grid                  */
} dsg_preview_output;
int  dsg_preview_outputs(const dsg_preview* pv);
bool dsg_preview_output_at(const dsg_preview* pv, int i, dsg_preview_output* out);

/* The input roles that the preview does not drive (decision 3, "Input"):
 * position2d_relative, allow_relative, position3d, scalar, gaze, units
 * workspace_cm or device, a sync line. why: a short reason. */
int  dsg_preview_undriven(const dsg_preview* pv);
const char* dsg_preview_undriven_role(const dsg_preview* pv, int i, const char** why);

/* The running template's choices, for the choice strip: their labels (the
 * choice's response name). 0: none, or no session. */
int  dsg_preview_choices(const dsg_preview* pv);
const char* dsg_preview_choice_label(const dsg_preview* pv, int i);

/* The leaves of the last accepted definition's timeline, in walk order,
 * for the pin: the leaf's path, its template and its table's rows (0: no
 * table). */
typedef struct dsg_preview_leaf {
    const char* path;
    const char* tmpl;
    int         rows;
} dsg_preview_leaf;
int  dsg_preview_leaves(const dsg_preview* pv);
bool dsg_preview_leaf_at(const dsg_preview* pv, int i, dsg_preview_leaf* out);

#ifdef __cplusplus
}
#endif
#endif
