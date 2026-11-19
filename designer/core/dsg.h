/* dsg.h - the designer's core (docs/designer.md, "Order of work", step 3).
 *
 * The parts of the designer that need no window: a project folder (open,
 * save, view state), the check of a definition with the player's own load
 * (pl_check(), docs/player.md), the place in the definition's text that a
 * refusal names, and the redraw rule. No SDL, no GL, no C++: the UI
 * (designer/app) is a thin layer on top, and tests/designer/core_test.c runs
 * all of this headless.
 *
 * A project is a folder with a pack source description (docs/pack.md 5.2,
 * normally pack.json), the definition that its "experiment" names, and the
 * other sources it lists. The designer adds one folder, DSG_DIR, for its view
 * state and its check pack (docs/designer.md, "Project folder").
 *
 * Every function returns 0 on success or -1 with one line in err. Not thread
 * safe: pl_check() keeps static scratch, so call dsg_check() from one thread.
 */
#ifndef YSP_DESIGNER_DSG_H
#define YSP_DESIGNER_DSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSG_VERSION_STRING "0.1.0"

/* The designer's folder inside a project, and the files in it. */
#define DSG_DIR            ".designer"
#define DSG_VIEW_FILE      ".designer/view.json"
#define DSG_CHECK_SOURCE   ".designer/check/experiment.json"
#define DSG_CHECK_PACK     ".designer/check/check.ysppak"
/* The description's file name when the caller names a folder. */
#define DSG_DESC_FILE      "pack.json"

#define DSG_PATH_MAX 4096

/* --- the platform (platform.c: the only file with OS calls) ---------------- */

/* The whole file, with a NUL after its n bytes (not counted). Free with free(). */
int  dsg_read_file(const char* path, char** data, size_t* n, char* err, size_t cap);
/* Writes path.tmp, then puts it in place of path, so a failed write leaves
 * the old file whole. */
int  dsg_write_file(const char* path, const void* data, size_t n, char* err, size_t cap);
/* Makes a folder; an existing folder is not an error. */
int  dsg_make_dir(const char* path, char* err, size_t cap);
bool dsg_is_dir(const char* path);
bool dsg_is_file(const char* path);

/* folder/rel into out; -1 when it does not fit. */
int  dsg_join(char* out, size_t cap, const char* folder, const char* rel);

/* --- view state ------------------------------------------------------------ */

/* What the designer shows, never part of the definition (format D13). */
typedef struct dsg_view {
    uint32_t caret_line, caret_column;   /* 1-based; 0: none                  */
    float    scroll_y;                   /* the editor's scroll, px           */
    char*    imgui_ini;                  /* ImGui's window settings; NULL: none */
    size_t   imgui_ini_len;
} dsg_view;

void dsg_view_free(dsg_view* v);
/* Reads DSG_VIEW_FILE. A missing file gives an empty view and 0; a file that
 * does not parse gives an empty view and -1 with the reason (the caller
 * notes it: view state is not worth refusing a project for). */
int  dsg_view_read(const char* folder, dsg_view* v, char* err, size_t cap);
/* Writes DSG_VIEW_FILE (canonical JSON), making DSG_DIR when needed. */
int  dsg_view_write(const char* folder, const dsg_view* v, char* err, size_t cap);

/* --- the project ----------------------------------------------------------- */

typedef struct dsg_project {
    char   folder[DSG_PATH_MAX];      /* the sources' folder                  */
    char   desc_path[DSG_PATH_MAX];   /* the pack source description          */
    char*  desc;                      /* its bytes                            */
    size_t desc_len;
    char   def_source[512];           /* experiment.source, relative          */
    char   def_path[DSG_PATH_MAX];    /* folder/def_source                    */
    char*  saved;                     /* the definition's bytes on disk, as
                                       * last read or written                 */
    size_t saved_len;
    char*  text;                      /* the text to edit after open: the
                                       * file's bytes, or the migrated ones   */
    size_t text_len;
    int    version;                   /* the file's format version; 0 when the
                                       * loader refused it at open            */
    bool   migrated;                  /* version 1 shown as version 2 (D12)   */
    dsg_view view;
    char   note[512];                 /* what open noted (a migration, a bad
                                       * view file); "" when nothing          */
} dsg_project;

/* path is the project's folder or its description file. */
int  dsg_project_open(dsg_project* p, const char* path, char* err, size_t cap);
void dsg_project_close(dsg_project* p);
/* A new project in folder (made when needed): DSG_DESC_FILE and
 * experiment.json, a version 2 definition with one trial. Refuses a folder
 * that already has a DSG_DESC_FILE. */
int  dsg_project_create(const char* folder, char* err, size_t cap);
/* The text differs from the definition's bytes on disk. */
bool dsg_project_dirty(const dsg_project* p, const char* text, size_t n);

typedef struct dsg_saved {
    bool   canonical;   /* the loader read the text, so the file holds its
                         * canonical bytes under version 2 (format D2, D12);
                         * false: the text as typed                           */
    char*  bytes;       /* what was written (malloc; dsg_saved_free())        */
    size_t n;
} dsg_saved;
int  dsg_project_save(dsg_project* p, const char* text, size_t n, dsg_saved* out, char* err, size_t cap);
void dsg_saved_free(dsg_saved* s);

/* --- places in the text ------------------------------------------------------ */

typedef struct dsg_place {
    uint32_t offset;          /* bytes from the text's start                   */
    uint32_t line, column;    /* 1-based, the column in bytes (ysp/json.h's)
                               * line 0: no place                              */
} dsg_place;

/* The place of what a tier 2 refusal names (PL_OBJ_* and its name, from
 * pl_check_result). Falls back to the set that holds it ("stimuli"), then
 * to no place. A text that does not parse has no place. */
bool dsg_locate_object(const char* text, size_t n, int object, const char* name, dsg_place* out);
/* The place of a tier 3 need (PL_NEED_* and its name). */
bool dsg_locate_need(const char* text, size_t n, int need, const char* name, dsg_place* out);
/* Line and column (1-based, bytes) to a byte offset, clamped to the line's
 * end and the text's end. */
uint32_t dsg_offset_of(const char* text, size_t n, uint32_t line, uint32_t column);
/* A byte offset to line and column (1-based, bytes). */
void dsg_line_col(const char* text, size_t n, uint32_t offset, uint32_t* line, uint32_t* column);
/* Codepoints in text[0, byte_offset): an editor's text position. */
uint32_t dsg_codepoints_before(const char* text, size_t n, uint32_t byte_offset);
/* The byte offset of codepoint cp (clamped to n). */
uint32_t dsg_byte_of_codepoint(const char* text, size_t n, uint32_t cp);

/* --- the check ------------------------------------------------------------- */

enum { DSG_SEV_ERROR = 0, DSG_SEV_WARNING, DSG_SEV_NEED, DSG_SEV_NOTE };
/* Who said it. */
enum {
    DSG_FROM_LOADER = 1,   /* tier 1: yexp_load() in pl_check()               */
    DSG_FROM_PLAYER,       /* tier 2: the player's refusal                    */
    DSG_FROM_RUN,          /* tier 3: what a real run needs                   */
    DSG_FROM_PACK,         /* the pack tool, building the check pack          */
    DSG_FROM_DESIGNER      /* the designer itself (a file it cannot write)    */
};

typedef struct dsg_issue {
    int         severity;      /* DSG_SEV_*                                    */
    int         from;          /* DSG_FROM_*                                   */
    dsg_place   place;         /* line 0: no place                             */
    int         object;        /* tier 2: PL_OBJ_*; tier 3: PL_NEED_*          */
    const char* name;          /* what object names; NULL: none                */
    const char* message;
} dsg_issue;

/* What a check gives. Every pointer stays valid until the checker's next
 * dsg_check() or dsg_checker_free(). */
/* dsg_report.status: pl_check()'s PL_CHECK_* values, for callers that do
 * not include player.h; -1 when pl_check() did not run. */
enum { DSG_CHECK_NONE = -1, DSG_CHECK_OK = 0, DSG_CHECK_DEFINITION = 1, DSG_CHECK_REFUSED = 2 };

typedef struct dsg_report {
    int              status;     /* DSG_CHECK_*                              */
    const dsg_issue* issues;     /* errors first, then warnings, needs, notes */
    int              n_issues;
    int              n_errors;
    bool             built;      /* this check (re)built the check pack       */
    bool             have_pack;  /* a check pack exists after this check      */
} dsg_report;

typedef struct dsg_checker dsg_checker;

/* NULL when out of memory: it holds pl_player (1.6 MB) and the stand-in
 * calibration, made once (docs/player.md, "Reference: pl_check()"). */
dsg_checker* dsg_checker_new(void);
void dsg_checker_free(dsg_checker* c);
/* Forgets the check pack, so the next check builds one (another project, or
 * the project's sources changed on disk). */
void dsg_checker_forget(dsg_checker* c);

/* Checks text as the definition of project p with the player's load.
 *
 * The check needs a pack with the project's tables, fonts and other
 * entries: the check pack, DSG_CHECK_PACK, built by the linked pack tool
 * from the project's description with the definition replaced by text
 * (written to DSG_CHECK_SOURCE). It is built at the first check whose text
 * the loader reads, and again when the set of texts the definition's text
 * stimuli can show changes (ypt_derive_texts()), since the glyph runs are
 * the only entries the definition decides. Every other check runs
 * pl_check() on text against the check pack in memory.
 *
 * Without a check pack (none yet, or its build failed), the issues are the
 * pack tool's message and yexp_load()'s diagnostics with no host. */
int dsg_check(dsg_checker* c, const dsg_project* p, const char* text, size_t n, dsg_report* r);

/* --- the redraw rule ----------------------------------------------------- */

/* Frames drawn after the last event: ImGui settles hover and focus changes
 * over two frames (docs/designer_ui_probe.md, part 1). */
#define DSG_SETTLE_FRAMES 3

typedef struct dsg_redraw {
    int  settle;        /* frames still to draw after the last event        */
    bool work;          /* a check or a save waits for a frame               */
} dsg_redraw;

/* An event arrived (input, a resize, a dropped file, a dialog's answer). */
void dsg_redraw_event(dsg_redraw* r);
/* Draw a frame now? queued: ImGui's input queue holds events (it applies one
 * key or button change a frame); animating: a panel animates. */
bool dsg_redraw_due(const dsg_redraw* r, int queued, bool animating);
/* After each frame drawn. */
void dsg_redraw_drawn(dsg_redraw* r);
/* How long the loop may block for the next event, ms: -1 forever, 0 not. */
int  dsg_redraw_wait_ms(const dsg_redraw* r, int queued, bool animating);

#ifdef __cplusplus
}
#endif
#endif
