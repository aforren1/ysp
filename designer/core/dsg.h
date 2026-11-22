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
 * safe: pl_check() keeps static scratch, so call dsg_check() from one thread
 * at a time. The designer calls it only on its check thread (worker.c).
 */
#ifndef YSP_DESIGNER_DSG_H
#define YSP_DESIGNER_DSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSG_VERSION_STRING "0.4.0"

/* The designer's folder inside a project, and the files in it. */
#define DSG_DIR            ".designer"
#define DSG_VIEW_FILE      ".designer/view.json"
#define DSG_CHECK_SOURCE   ".designer/check/experiment.json"
#define DSG_CHECK_PACK     ".designer/check/check.ysppak"
/* The description's file name when the caller names a folder. */
#define DSG_DESC_FILE      "pack.json"
/* Step 6: the entry cache that the check pack and Build share (decision 17,
 * part K's ypt_options.cache), and Build's folder (decision 1). */
#define DSG_CACHE_DIR      ".designer/cache/entries"
#define DSG_BUILD_DIR      "build"

#define DSG_PATH_MAX 4096
/* Step 6: a rig profile's name, its NUL included ("profile" for
 * profile.json; ysp/rigfile.h's names are shorter). */
#define DSG_RIG_NAME 64

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

/* Removes root/rel and everything in it (the preview's old data sets,
 * DSG_DIR/preview/run-N, on the check thread). rel is relative: names
 * joined by '/', none of them empty, "." or "..", and no ':' or '\\' (a
 * drive or a stream on Windows). No name on the way, the last one too, may
 * be a symbolic link or a junction (on Windows a name surrogate; a cloud
 * folder's placeholder is not one). Inside the tree, a link is removed and
 * never followed, so nothing outside root/rel goes.
 * A rel that is not there is not an error. -1 with the reason in err when
 * rel is refused, or when something could not be removed (the rest is
 * gone). On POSIX the walk opens each folder with O_NOFOLLOW from the one
 * above it, so a folder swapped for a link during the walk is refused; on
 * Windows a link made during the walk is not seen. */
int  dsg_remove_tree(const char* root, const char* rel, char* err, size_t cap);

/* An atomic add on a counter that two threads share; the new value. */
int32_t dsg_atomic_add(volatile int32_t* v, int32_t d);
int32_t dsg_atomic_load(const volatile int32_t* v);

/* --- the process (platform.c; docs/designer.md, "Part X as built") --------
 *
 * One child program (the player), its standard output and error in a file
 * of their own, and a watch thread that wakes the UI. The child gets
 * SCHED_OTHER, each signal at its default action, an empty signal mask,
 * /dev/null (Windows: NUL) as standard input, and no descriptor or handle
 * of the designer's but the log's. POSIX: it has a process group of its
 * own, so a Ctrl+C in the designer's terminal does not stop a session.
 *
 * The watch thread is SCHED_OTHER, never elevated, and blocks. It wakes at
 * the child's exit (Linux 5.3 and later: at once, through a pidfd; else
 * within 250 ms) and every 250 ms, when it reads the sizes of out_path and
 * of each watch file. It calls wake when a size changed or the child
 * ended. wake may call dsg_proc_poll(). Call the other functions from one
 * thread.
 * WebAssembly: dsg_proc_start() fails with "no processes in a browser". */
typedef struct dsg_proc dsg_proc;
typedef struct dsg_proc_desc {
    const char* const* argv;      /* argv[0]: the program's path; NULL-ended.
                                     A relative path is from the designer's
                                     working folder, not from cwd           */
    const char* cwd;              /* NULL: the designer's                 */
    const char* out_path;         /* stdout and stderr; made, must not exist */
    const char* const* watch;     /* files whose growth wakes (NULL-ended) */
    void (*wake)(void* ctx);      /* on the watch thread                  */
    void* ctx;
} dsg_proc_desc;
int  dsg_proc_start(dsg_proc** out, const dsg_proc_desc* d, char* err, size_t cap);
/* SIGTERM; Windows WM_CLOSE to each top-level window of the child. 1 when
 * the request went out; 0 when the child has ended, or (Windows) has no
 * top-level window, as `ysp_player --sim` has none: then only Kill ends it. */
int  dsg_proc_stop(dsg_proc* p);
void dsg_proc_kill(dsg_proc* p);           /* SIGKILL; TerminateProcess */
/* 1 ended (status: exit code >= 0, or -signal), 0 running. Windows: the
 * exit code as a signed int; Kill's is -9, as SIGKILL's on POSIX. */
int  dsg_proc_poll(dsg_proc* p, int* status);
/* Joins the watch thread. A process still running is left running; on
 * POSIX the watch thread then stays only to reap it (no zombie) and frees
 * itself. wake is not called after this returns. */
void dsg_proc_free(dsg_proc* p);

/* --- view state ------------------------------------------------------------ */

/* A view's own state: one top-level key of the view file whose value is an
 * object ("chart", "tables", "panels"; docs/designer.md, "Plan of step 4"),
 * kept as compact JSON. The view that owns the key reads and writes it; the
 * file keeps keys that no view of this designer reads. */
#define DSG_VIEW_PARTS 8
typedef struct dsg_view_part {
    char   key[32];
    char*  json;                         /* compact JSON of an object, NUL-ended */
    size_t n;
} dsg_view_part;

/* What the designer shows, never part of the definition (format D13). */
typedef struct dsg_view {
    uint32_t caret_line, caret_column;   /* 1-based; 0: none                  */
    float    scroll_y;                   /* the editor's scroll, px           */
    char*    imgui_ini;                  /* ImGui's window settings; NULL: none */
    size_t   imgui_ini_len;
    char     main_view[32];              /* the main area's view ("text", "chart",
                                          * "tables"); "": the text            */
    char     selection[512];             /* the selected value's JSON Pointer;
                                          * "": none                          */
    dsg_view_part parts[DSG_VIEW_PARTS];
    int      n_parts;
} dsg_view;

void dsg_view_free(dsg_view* v);
/* The part's JSON, or NULL when the view file has none. */
const char* dsg_view_part_get(const dsg_view* v, const char* key, size_t* n);
/* Sets a part from the JSON of an object (checked), or removes it (json
 * NULL). -1 when it is not an object, the key is reserved or too long, or
 * the parts are full. */
int  dsg_view_part_set(dsg_view* v, const char* key, const char* json, size_t n);
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
    DSG_FROM_DESIGNER,     /* the designer itself (a file it cannot write)    */
    DSG_FROM_PREVIEW,      /* the preview: a session's refusal at its load or
                            * at run time (preview.h)                         */
    /* Step 6 (dsg.h 0.4.0). */
    DSG_FROM_BUILD,        /* Build (build.c): the pack tool's error line and
                            * each "warning:" line                            */
    DSG_FROM_PLAYER_RUN,   /* Run (run.c): a run that the player refused or
                            * that failed, its log's last line                */
    DSG_FROM_RIG           /* the rig profile itself (rig.c, check.c): a file
                            * that does not load, a check note of yrig_load() */
};

typedef struct dsg_issue {
    int         severity;      /* DSG_SEV_*                                    */
    int         from;          /* DSG_FROM_*                                   */
    dsg_place   place;         /* line 0: no place                             */
    int         object;        /* tier 2: PL_OBJ_*; tier 3: PL_NEED_*          */
    const char* name;          /* what object names; NULL: none                */
    const char* message;
    /* Step 6: the rig profile that the check ran with causes it, so a check
     * with no profile would not say it (a role that the profile binds to a
     * family that the role does not list). The report's rig names the
     * profile; the Issues panel says so. Always true for DSG_FROM_RIG. */
    bool        rig;
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
    /* Step 6: the rig profile that the check ran with (pl_check_desc.rig and
     * .cal, decision 13); "": none. */
    char             rig[DSG_RIG_NAME];
} dsg_report;

typedef struct dsg_checker dsg_checker;

/* NULL when out of memory: it holds pl_player (1.6 MB) and the stand-in
 * calibration, made once (docs/player.md, "Reference: pl_check()"). */
dsg_checker* dsg_checker_new(void);
void dsg_checker_free(dsg_checker* c);
/* Forgets the check pack, so the next check builds one (another project, or
 * the project's sources changed on disk). */
void dsg_checker_forget(dsg_checker* c);
/* Step 6 (decision 13): the rig profile of the next checks by its name
 * (dsg_rig_load()), "" or NULL for none. The checker loads it at its next
 * check and keeps it; it loads it again when the name changes or
 * dsg_rig_changed() says so. pl_check_desc.rig is the profile and .cal its
 * calibration when dsg_rig_cal() gives one (else the stand-in). A profile
 * that does not load is a DSG_FROM_RIG error and the check runs with none.
 * The report's rig is the profile that the check ran with. Part G fills it
 * in check.c; part S's stub keeps the name and runs with none. */
void dsg_checker_set_rig(dsg_checker* c, const char* name);

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
 * pack tool's message and yexp_load()'s diagnostics with no host.
 *
 * tables: the document (doc.h) whose table buffers hold the tables as
 * edited, or NULL for the tables on disk. Each dirty table's bytes are
 * written under DSG_DIR/check/ and the check description's source points at
 * them, so the check sees a table edit before Save; a buffer's name is its
 * "source" in the description. The pack is built again when the set of
 * dirty tables or the revision of one changes. */
struct dsg_doc;
int dsg_check(dsg_checker* c, const dsg_project* p, const char* text, size_t n, const struct dsg_doc* tables, dsg_report* r);

/* One unsaved table, as dsg_check() takes it from a document: the check
 * thread gets a copy, because the UI edits the document meanwhile. */
typedef struct dsg_table_bytes {
    int         buf;     /* the document's buffer: names the copy's file      */
    const char* name;    /* its "source" in the description                    */
    uint64_t    rev;     /* dsg_doc_rev(): a new one rebuilds the pack         */
    const char* bytes;
    size_t      n;
} dsg_table_bytes;
/* dsg_check() with the dirty tables given as bytes (n_tables 0: none). Only
 * folder, desc_path, desc and desc_len of p are read. */
int dsg_check_tables(dsg_checker* c, const dsg_project* p, const char* text, size_t n, const dsg_table_bytes* tables, int n_tables,
                     dsg_report* r);

/* --- the check pack's bytes, shared (docs/designer.md, "Plan of step 5") ----
 *
 * A session points into the pack it loaded, so a rebuild must not free the
 * bytes under it. The checker holds one reference; each holder adds its own.
 * Retain and release are atomic: any thread may call them. */
typedef struct dsg_pack dsg_pack;
/* The checker's pack, borrowed (NULL: none). Retain it to keep it. */
dsg_pack*   dsg_checker_pack(dsg_checker* c);
dsg_pack*   dsg_pack_retain(dsg_pack* pk);       /* gives pk; NULL is a no-op */
void        dsg_pack_release(dsg_pack* pk);      /* frees at the last; NULL ok */
const void* dsg_pack_bytes(const dsg_pack* pk, size_t* size);
/* Packs made and not yet freed, in the process: for the tests. */
int32_t     dsg_pack_live(void);

/* --- the check thread (worker.c; docs/designer.md, "Plan of step 5") --------
 *
 * One thread runs two kinds of job. For each kind the newest job wins: a job
 * posted while one of its kind waits replaces it. A waiting check runs before
 * a waiting session job, and sessions handed back to close run between the
 * two. Each result carries the revision its job was posted with. The thread
 * is a yrt_pump (ysp/rt.h): it starts SCHED_OTHER with explicit attributes,
 * never elevates and blocks when idle.
 *
 * The UI thread posts and takes; the post copies what the job reads (the
 * text, the dirty tables, the project's description) into buffers that the
 * worker reuses, so a post allocates only when a buffer grows. */
typedef struct dsg_worker dsg_worker;

/* What a check job gives. Every pointer stays valid until the next
 * dsg_worker_take_check() that returns true. */
typedef struct dsg_check_result {
    uint64_t   revision;     /* the job's                                   */
    dsg_report report;
    dsg_pack*  pack;         /* the pack the check ran against, held by the
                              * result; NULL: none. Retain it to keep it.    */
} dsg_check_result;

/* The preview's settings that a session load reads (part A fills them). */
#define DSG_SESSION_FIELDS 16
typedef struct dsg_session_field { char name[64]; char value[256]; } dsg_session_field;
typedef struct dsg_session_settings {
    int32_t  width, height, hz;   /* the simulated display; 0: the built-in rig's */
    uint64_t seed;
    char     participant[32];     /* the scripted participant: "correct", ... */
    double   rt_s;
    char     pin_leaf[512];       /* the pin's leaf path; "": none           */
    int32_t  pin_row;             /* the pin's table row; -1: any            */
    int32_t  n_fields;
    dsg_session_field fields[DSG_SESSION_FIELDS];
    /* Step 6 (decision 14): the preview's rig profile by its name, which
     * the session's load reads (dsg_rig_load(), pl_sim_desc.rig and .cal);
     * "": the built-in simulated rig. */
    char     rig[DSG_RIG_NAME];
    char     out_dir[DSG_PATH_MAX];   /* the data set's folder               */
} dsg_session_settings;

/* A loaded session. The worker allocates and frees it; between its take
 * and dsg_worker_close() the UI thread owns it. */
typedef struct dsg_session {
    uint64_t revision;            /* the job's                               */
    dsg_session_settings settings;
    dsg_pack* pack;               /* the session's reference: released after
                                   * the close                               */
    void*    impl;                /* the load's own state (the player's
                                   * session); NULL for the stub             */
    struct dsg_session* next;     /* the worker's close list                 */
} dsg_session;

/* The seam to the player's stepped session (pl_sim_load(), part A). Both
 * run on the check thread. load: 0, or -1 with the reason in err (the
 * worker then calls close). close: frees what load made; the worker then
 * releases the pack and frees s. A NULL load is the stub: it loads
 * nothing and succeeds. */
typedef struct dsg_session_ops {
    int  (*load)(void* ctx, dsg_session* s, const char* text, size_t n, char* err, size_t cap);
    void (*close)(void* ctx, dsg_session* s);
    void* ctx;
} dsg_session_ops;

typedef struct dsg_session_result {
    uint64_t     revision;
    dsg_session* session;         /* the caller's now; NULL: the load failed */
    char         error[512];      /* why, when session is NULL               */
} dsg_session_result;

typedef struct dsg_worker_desc {
    /* On the check thread, after a result is ready: main.cpp pushes an SDL
     * event, which wakes the UI's wait. NULL: none. */
    void (*wake)(void* ctx);
    void* ctx;
    dsg_session_ops session;
    /* No thread: the jobs run in dsg_worker_wait(), on its caller's thread
     * (the preview's tests; a build without threads). */
    bool  no_thread;
} dsg_worker_desc;

/* Starts the thread. Builds gfx's NOISE table first, on the calling thread
 * (ygfx_noise_value(): not thread safe before its first call), so start the
 * worker before any other thread uses ysp/gfx.h. NULL with the reason in err. */
dsg_worker* dsg_worker_start(const dsg_worker_desc* d, char* err, size_t cap);
/* Drops the waiting jobs, joins the thread after the job that runs, closes
 * the sessions the worker holds and frees it. Sessions that the caller took
 * must be handed back with dsg_worker_close() first. */
void dsg_worker_stop(dsg_worker* w);

/* Posts a check job: text as project p's definition, with the dirty tables
 * of the document (NULL: the tables on disk) and the rig profile by its name
 * (step 6; "" or NULL: none), which the thread gives to
 * dsg_checker_set_rig() before the check. 0, or -1 when out of memory. */
int  dsg_worker_check(dsg_worker* w, const dsg_project* p, const char* text, size_t n, const struct dsg_doc* tables,
                      uint64_t revision, const char* rig);
/* The checker forgets its pack before the next check (dsg_checker_forget()). */
void dsg_worker_forget(dsg_worker* w);
/* Posts a session job: an accepted text, the pack its check ran against
 * (the worker adds its own reference) and the settings. 0, or -1. */
int  dsg_worker_session(dsg_worker* w, const char* text, size_t n, dsg_pack* pack, uint64_t revision,
                        const dsg_session_settings* s);
/* A session that the UI thread is done with (detached from GL): closed on
 * the check thread. */
void dsg_worker_close(dsg_worker* w, dsg_session* s);

/* On the UI thread: the newest check result since the last take. */
bool dsg_worker_take_check(dsg_worker* w, const dsg_check_result** out);
/* On the UI thread: the newest session job's outcome since the last take. */
bool dsg_worker_take_session(dsg_worker* w, dsg_session_result* out);
/* A session job waits or runs: a session that arrives now is stale. */
bool dsg_worker_session_queued(dsg_worker* w);

/* Blocks until every job posted so far has run and posted its result, for
 * at most timeout_ns. 0, or -1 on the timeout. With no_thread, runs them. */
int  dsg_worker_wait(dsg_worker* w, int64_t timeout_ns);
/* false: no thread (desc.no_thread, or a build without threads). */
bool dsg_worker_threaded(const dsg_worker* w);

typedef struct dsg_worker_stats {
    int64_t checks;               /* dsg_check() runs                         */
    int64_t checks_replaced;      /* check jobs replaced while they waited    */
    int64_t sessions_loaded;      /* loads that succeeded                     */
    int64_t sessions_failed;
    int64_t sessions_replaced;    /* session jobs replaced while they waited  */
    int64_t sessions_dropped;     /* loaded, then closed by the thread: stale */
    int64_t sessions_closed;      /* every close on the thread                */
} dsg_worker_stats;
void dsg_worker_get_stats(dsg_worker* w, dsg_worker_stats* out);

/* --- step 6: rig profiles (rig.c, part G; docs/designer.md, "Rig profiles") --
 *
 * FROZEN BY PART S (docs/designer.md, "Part S as built" of step 6), as are
 * the Build and Run sections below. A part that needs a change stops and
 * reports it.
 *
 * A dsg_rig is a profile of the user's rig folder (yrig_dir()) and its
 * calibration file, loaded together. The calls take no lock: a dsg_rig is
 * one thread's (the checker keeps its own on the check thread, a session's
 * load its own, the Run view its own on the UI thread). A load costs about
 * 176 us (yrig_load_file() with a 62,528-byte calibration, "Measured"), so
 * load once per choice or change, never per frame or per check. */
struct yrig_profile;
struct ycol_cal;
typedef struct dsg_rig dsg_rig;

/* The profiles in yrig_dir(): "profile" (profile.json) first, then each
 * other NAME.json's NAME by name, at most cap. The count, or -1 with the
 * reason in err: no user folder, or one that others can write (yrig_dir()'s
 * refusal, as the player gives it). */
int  dsg_rig_list(char (*names)[DSG_RIG_NAME], int cap, char* err, size_t err_cap);
/* yrig_load() of name ("profile": profile.json), and the calibration file
 * that it names when the file's check is OK (ycol_cal_load()). 0 and *out,
 * or -1 with the reason in err. */
int  dsg_rig_load(const char* name, dsg_rig** out, char* err, size_t cap);
void dsg_rig_free(dsg_rig* r);                   /* NULL is a no-op */
const char* dsg_rig_name(const dsg_rig* r);
/* The profile's file, for "Show" (its folder) and the Run view. */
const char* dsg_rig_path(const dsg_rig* r);
/* pl_check_desc.rig and pl_sim_desc.rig. Never NULL for a loaded rig. */
const struct yrig_profile* dsg_rig_profile(const dsg_rig* r);
/* pl_check_desc.cal and pl_sim_desc.cal: NULL when the profile names no
 * calibration, or its file's check is not OK. The calibration need goes
 * only when the profile's check passed and this is given (Part P). */
const struct ycol_cal* dsg_rig_cal(const dsg_rig* r);
/* The profile's file or its calibration file changed size or modification
 * time since the load, or went (a stat each: at most once a second). */
bool dsg_rig_changed(const dsg_rig* r);
/* One line for the preview's banner: "Rig: NAME (view 570 mm, 1920 px over
 * 518.4 mm)", or "Rig: NAME (no view)". snprintf's return. */
int  dsg_rig_summary(const dsg_rig* r, char* out, size_t cap);
/* The preview's pointer for a relative signal role (Part P, P2: with a
 * profile that has the role's signal, pl_sim_sample() takes a raw mouse's
 * counts through the profile's adapter, not role units). out: counts per
 * role unit for x and y, 1 / (per_count x yrig_units_factor(the signal's
 * units, role_units)), with its sign. role_units: YRIG_UNITS_*. false when
 * the profile has no signal for the role or the units do not convert: then
 * a sample is in role units at gain 1, as with no profile. */
bool dsg_rig_counts_per_unit(const dsg_rig* r, const char* role, int role_units, double out[2]);

/* --- step 6: Build (build.c, part B; docs/designer.md, "Build") ----------- */

enum { DSG_BUILD_NONE = 0, DSG_BUILD_OK, DSG_BUILD_FAILED, DSG_BUILD_CANCELED };

/* One source of a build's stamp: the description, the definition and each
 * resource's source as the description names them. */
typedef struct dsg_build_source {
    const char* path;             /* absolute                                */
    int64_t     size;             /* -1: missing at the build                */
    int64_t     mtime_ns;
} dsg_build_source;

/* What a Build gives. Every pointer stays valid until the next
 * dsg_build_take() that returns true, or dsg_builder_stop(). */
typedef struct dsg_build_result {
    uint64_t    revision;         /* the post's: the document as saved       */
    int         status;           /* DSG_BUILD_*                             */
    /* DSG_FROM_BUILD issues: the error line and each "warning:" line, with
     * the definition's place ("line N, column M", as the check's) or a
     * table's (dsg_csv_place(), which the tables view reads). The order of
     * dsg_report: errors first. status is DSG_CHECK_NONE. */
    dsg_report  report;
    const char* path;             /* the pack (decision 1)                   */
    int64_t     size;             /* its bytes; 0 unless OK                  */
    char        pack_id[72];      /* ypak_id(): "sha256:..."; "" unless OK   */
    double      ms;               /* the build's time                        */
    int64_t     done_unix_s;      /* when it ended                           */
    const dsg_build_source* sources;   /* the stamp                          */
    int         n_sources;
} dsg_build_result;

typedef struct dsg_builder dsg_builder;
typedef struct dsg_builder_desc {
    /* On the build thread when a result is ready: main.cpp pushes the check
     * thread's event (UiPlatform.wake). NULL: none. */
    void (*wake)(void* ctx);
    void* ctx;
    /* No thread: a job runs in dsg_build_wait() (tests; WebAssembly). */
    bool  no_thread;
} dsg_builder_desc;

/* The build thread: a yrt_pump as the check thread's (SCHED_OTHER with
 * explicit attributes, never elevated, blocks when idle). NULL with the
 * reason in err. */
dsg_builder* dsg_builder_start(const dsg_builder_desc* d, char* err, size_t cap);
/* Cancels the build that runs, drops a waiting one, joins, frees. NULL is a
 * no-op. */
void dsg_builder_stop(dsg_builder* b);
/* PROJECT/build/NAME.ysppak, NAME the project folder's name (decision 1).
 * 0, or -1 when it does not fit. */
int  dsg_build_path(const dsg_project* p, char* out, size_t cap);
/* Posts a Build of p's description from the files (decision 2: the caller
 * saved first) to out_path, with the entry cache in DSG_CACHE_DIR. One job
 * at a time; a post while one waits replaces it (newest wins). Copies what
 * it reads. 0, or -1 with the reason in err. */
int  dsg_build_post(dsg_builder* b, const dsg_project* p, const char* out_path, uint64_t revision, char* err, size_t cap);
/* The running build stops before its next entry (ypt_options.cancel,
 * YPT_ERR_CANCELED: no pack); a waiting one is dropped. */
void dsg_build_cancel(dsg_builder* b);
/* A job waits or runs. */
bool dsg_build_busy(const dsg_builder* b);
/* On the UI thread: the newest result since the last take. */
bool dsg_build_take(dsg_builder* b, const dsg_build_result** out);
/* Blocks until every job posted so far has posted its result, for at most
 * timeout_ns. 0, or -1 on the timeout. With no_thread, runs them. */
int  dsg_build_wait(dsg_builder* b, int64_t timeout_ns);
/* Run builds first when this is true: no result, one not OK, another
 * revision, or a source whose size or time is not the stamp's (a stat
 * each). */
bool dsg_build_stale(const dsg_project* p, const dsg_build_result* r, uint64_t revision);
/* The check pack's build, on the check thread: check.c's build() calls it
 * in place of its own ypt_build_text() of desc (the check's description,
 * whose sources are in p->folder) into out_path. Part B owns it: the entry
 * cache (DSG_CACHE_DIR, its bound) and any skip of a build whose inputs did
 * not change. Only folder, desc_path, desc and desc_len of p are read (the
 * check job's copy). Part S's stub is check.c's call as before step 6. 0
 * with the pack at out_path, or -1 with the reason in err. */
int  dsg_build_check_pack(const dsg_project* p, const char* desc, size_t desc_len, const char* out_path, char* err, size_t cap);

/* --- step 6: Run (run.c and run_part.c; docs/designer.md, "Run", "Stop") --- */

#define DSG_RUN_ROLES       16
#define DSG_RUN_MAX_SECONDS 600.0   /* --max-seconds with --sim (decision 10) */
#define DSG_RUN_REDRAW_HZ   2       /* the UI's frames a second for a run's progress */
#define DSG_RUN_KILL_AFTER_NS 5000000000LL   /* Kill shows 5 s after a Stop  */
#define DSG_RUN_LOG_LINES   200     /* the log's last lines that the view shows */

typedef struct dsg_run_gain {
    char   role[32];
    double units;                 /* role units a count                       */
} dsg_run_gain;

/* The Run view's settings ("The command line"); the view file's part "run"
 * keeps all but out_dir. */
typedef struct dsg_run_settings {
    char     rig[DSG_RIG_NAME];   /* --rig NAME, and the check's profile;
                                   * "": no --rig (a real run then takes
                                   * profile.json if there is one)           */
    bool     sim;                 /* --sim                                   */
    char     participant[32];     /* --participant SPEC with --sim; "": none */
    int32_t  n_fields;
    dsg_session_field fields[DSG_SESSION_FIELDS];   /* --field NAME=VALUE    */
    char     seed[24];            /* --seed N; "": the player draws one      */
    int32_t  n_below;
    char     allow_below_tier[DSG_RUN_ROLES][32];   /* --allow-below-tier R,R */
    bool     windowed;            /* --windowed                              */
    double   max_seconds;         /* --max-seconds with --sim                */
    int32_t  n_gains;
    dsg_run_gain gains[DSG_RUN_ROLES];  /* --sim-gain ROLE=UNITS with --sim  */
    char     out_dir[DSG_PATH_MAX];     /* "": dsg_run_out_dir()'s           */
} dsg_run_settings;

/* run_part.c (part S): not simulated, no rig, no fields, max_seconds
 * DSG_RUN_MAX_SECONDS. */
void dsg_run_settings_default(dsg_run_settings* s);
/* The view file's part "run": {"open": bool, "rig": NAME, "sim": bool,
 * "participant": SPEC, "fields": {NAME: "VALUE"}, "seed": "N",
 * "allow_below_tier": [ROLE], "windowed": bool, "max_seconds": s,
 * "gains": {ROLE: units}}. A key that is missing, of another type or out of
 * range takes its default, and so does every key when the part is missing
 * or does not parse. open: the Run view shows in the AUX pane. */
void dsg_run_part_read(const dsg_view* v, dsg_run_settings* s, bool* open);
/* Writes every key, defaults too. 0, or -1 when the parts are full. */
int  dsg_run_part_write(dsg_view* v, const dsg_run_settings* s, bool open);

/* The player's command line: argv[0] the player, argv[argc] NULL. */
#define DSG_RUN_ARGV 96
typedef struct dsg_run_argv {
    const char* argv[DSG_RUN_ARGV];
    int         argc;
    char        buf[32768];       /* the strings that argv points at         */
} dsg_run_argv;
/* "Run", "The command line": player PACK --out DIR [--rig NAME] [--seed N]
 * [--field NAME=VALUE]... [--allow-below-tier R,R] [--windowed] [--sim
 * --max-seconds S [--sim-gain ROLE=UNITS]... [--participant SPEC]]. A pure
 * function. 0, or -1 with the reason in err: a value does not fit, or the
 * environment variable YSP_DESIGNER_TEST is set and s->sim is false (no test
 * starts a real run). */
int  dsg_run_args(const dsg_run_settings* s, const char* player, const char* pack, const char* out_dir, dsg_run_argv* out,
                  char* err, size_t cap);
/* folder/data/PARTICIPANT_YYYYMMDD-HHMMSS (decision 11): the session field
 * "participant" (s->fields), UTC. 0, or -1 when it does not fit. */
int  dsg_run_out_dir(const char* folder, const dsg_run_settings* s, int64_t now_unix_s, char* out, size_t cap);
/* What an exit status means ("What the Run view shows"): 0 done, 1 refused
 * or failed, 2 a bad argument, 3 stopped, 99 the panic watchdog, -S a
 * signal. Into buf; gives buf. */
const char* dsg_run_exit_text(int status, char* buf, size_t cap);
/* `player --version`'s version ("0.18.0") into out, its output in a file
 * under tmp_dir. 0, or -1 with the reason in err. */
int  dsg_player_version(const char* player, const char* tmp_dir, char* out, size_t cap, char* err, size_t err_cap);

enum { DSG_RUN_STARTING = 1, DSG_RUN_RUNNING, DSG_RUN_STOPPING, DSG_RUN_ENDED };

typedef struct dsg_run dsg_run;
typedef struct dsg_run_desc {
    const char* const* argv;      /* dsg_run_args()'s; argv[0] absolute       */
    const char* cwd;              /* the project's folder                     */
    const char* out_dir;          /* --out's DIR: its trials.csv is read; the
                                   * output goes to DIR.log beside it         */
    const char* tmp_dir;          /* dsg_player_version()'s: PROJECT/.designer */
    bool        sim;              /* --sim: on Windows Stop cannot reach it   */
    void (*wake)(void* ctx);      /* dsg_proc_desc.wake: on the watch thread  */
    void* ctx;
    int64_t     now_ns;           /* the UI's clock at the start              */
} dsg_run_desc;
/* Refuses a player of another version than the linked player library's
 * (dsg_player_version(), decision 5), makes out_dir's parent, and starts
 * the player (dsg_proc_start(), its output in DIR.log, trials.csv watched).
 * Copies what it reads of d. 0, or -1 with the reason in err, and then
 * nothing runs. */
int  dsg_run_start(dsg_run** out, const dsg_run_desc* d, char* err, size_t cap);
/* Stop (decision 8): 1 the request went out; 0 it did not (the player had
 * ended, or a --sim run on Windows, Part P): Kill is offered at once. */
int  dsg_run_stop(dsg_run* r, int64_t now_ns);
/* Kill: only after a Stop (the view warns that the display's mode and
 * gamma ramp may stay changed). */
void dsg_run_kill(dsg_run* r);
/* On the UI thread, once a frame: polls the process, and reads the log's
 * and trials.csv's new bytes when their sizes changed. true when the info
 * changed. Allocates only when a buffer grows; never blocks. */
bool dsg_run_tick(dsg_run* r, int64_t now_ns);

typedef struct dsg_run_info {
    int         state;            /* DSG_RUN_*                                */
    int64_t     elapsed_ns;       /* the start to now, or to the end          */
    int         trials;           /* trials.csv's rows                        */
    const char* last_response;    /* the last row's (by column name); ""      */
    const char* last_outcome;
    double      last_rt_s;        /* < 0: none                                */
    const char* log;              /* the log's last DSG_RUN_LOG_LINES lines   */
    int         exit_status;      /* ENDED: the code >= 0, or -signal         */
    const char* exit_text;        /* dsg_run_exit_text(), with the log's last
                                   * line for 1; "" until the end             */
    bool        kill_offered;     /* DSG_RUN_KILL_AFTER_NS after a Stop that
                                   * went out, at once after one that did not */
    const char* out_dir;
    const char* log_path;
    const char* command;          /* the command line, quoted (for exit 2)    */
} dsg_run_info;
/* Pointers valid until the next dsg_run_tick(), _stop(), _kill() or
 * _free(). */
void dsg_run_get_info(const dsg_run* r, dsg_run_info* out);
/* A failed run's issue (exit 1: the log's last line; DSG_FROM_PLAYER_RUN,
 * an error, rig true when the run had --rig and the line names the rig).
 * false: none. Pointers as dsg_run_get_info()'s. */
bool dsg_run_issue(const dsg_run* r, dsg_issue* out);
/* Frees the run. A player that still runs is left running ("Leave it
 * running"): its output goes to its log (decision 6). NULL is a no-op. */
void dsg_run_free(dsg_run* r);

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
