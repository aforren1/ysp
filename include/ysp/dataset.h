/* ysp/dataset.h - v0.1.0 - public domain single-header session data set writer
 *
 *   The folder a session's data goes into (docs/experiment_format.md D9 and
 *   2.10): session.json (canonical JSON, format "ysp-session" 1, written at
 *   the start and again at the end with "closed": true), trials.csv (one
 *   row per trial, columns of the caller's), frames.csv (one row per flip
 *   record), events.csv (landings, marks, responses, device events,
 *   outputs), clock.csv (clock pairs and fits) and streams/<name>.csv (one
 *   table per stream). Every CSV is written by ysp/trials.h's data writer,
 *   so each row reaches the OS when the write call returns, and a crash
 *   keeps every row written; session.json says "closed": false until the
 *   session ends, so a session cut short is visible. For the web, a stored
 *   zip of the finished folder (yds_zip()).
 *
 *   REQUIRES ysp/trials.h (and so ysp/table.h) and ysp/json.h beside it;
 *   this header includes them, and its implementation implements them
 *   unless the translation unit already does. Uses the C library's stdio
 *   and, for the folder and the atomic rewrite of session.json, the OS:
 *   CreateDirectoryW and MoveFileExW on Windows, mkdir, rename and fsync
 *   on POSIX and under Emscripten (its in-memory file system). C99 is the
 *   floor: it builds as C99, C11 and C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_DATASET_IMPLEMENTATION
 *       #include "ysp/dataset.h"
 *
 *       typedef struct row { const char* response; bool correct; int64_t rt_ns; } row;
 *       static const ytr_col cols[] = {
 *           YTR_COL_TEXT("participant", "P07"),
 *           YTR_COL(row, response, YTR_COL_STRING),
 *           YTR_COL(row, correct, YTR_COL_BOOL),
 *           YTR_COL(row, rt_ns, YTR_COL_TIME_NS),         // column rt_s
 *       };
 *       yjs_arena a;
 *       yjs_value* s;
 *       yjs_arena_init_heap(&a, 0);
 *       s = yjs_new(&a, YJS_OBJECT);                     // the player's keys
 *       yjs_set(&a, s, "experiment", yjs_new_stringz(&a, experiment_id));
 *       yjs_set(&a, s, "seed", yjs_new_int(&a, seed));
 *
 *       static yds_dataset ds;                           // about 770 KB, zero pages
 *       yds_desc d = { 0 };
 *       d.dir = "data/P07_20261021T140312Z";             // must not exist
 *       d.session = s;                                   // copied
 *       d.trial_cols = cols;
 *       d.n_trial_cols = 4;
 *       if (!yds_open(&ds, &d)) { fputs(yds_error(&ds), stderr); return 1; }
 *       yjs_arena_free(&a);
 *
 *       // between trials, or from the frame loop:
 *       yds_frame f;
 *       yds_frame_from_screen(&f, &frame.done[i], trial);   // ysp/screen.h first
 *       yds_write_frame(&ds, &f);
 *       yds_write_trial(&ds, &r);
 *
 *       if (yds_finish(&ds) < 0) fputs(yds_error(&ds), stderr);
 *       yds_close(&ds);
 *
 *   ---------------------------------------------------------------------
 *   THE FOLDER
 *   ---------------------------------------------------------------------
 *   desc.dir is UTF-8, at most YDS_MAX_DIR bytes, and must not exist: the
 *   open makes it (its parent must exist) and refuses a folder that is
 *   there already (YDS_ERR_EXISTS), so a data set never overwrites
 *   another. It holds, from the open on:
 *     session.json        canonical JSON (SESSION.JSON)
 *     trials.csv          desc.trial_cols, one row per yds_write_trial()
 *     frames.csv          YDS_FRAME_COLS, one row per yds_write_frame()
 *     events.csv          YDS_EVENT_COLS, one row per yds_write_event()
 *     clock.csv           YDS_CLOCK_COLS, one row per yds_write_clock()
 *     streams/            one <name>.csv per yds_stream()
 *   Each CSV is a ysp/trials.h data file with no session (ytr_data_desc
 *   .trials NULL): its header line at open, LF line ends, UTF-8, no
 *   comment line, missing values as empty fields, times as seconds in
 *   columns whose names end in "_s" (the exact decimal of the
 *   nanoseconds, 9 decimals), the decimal point '.' in every locale.
 *   ysp/trials.h's DATA FILE section is the reference for the text of
 *   each type. ysp/table.h reads each file back (with allow_empty) up to
 *   its YTB_MAX_ROWS (32767) rows; pandas and R read any length.
 *   CRASHES. A row is handed to the OS in one write before the write call
 *   returns (YTR_FLUSH_ROW, the default), so a crash, an abort() or a
 *   killed process keeps every row written; YTR_FLUSH_SYNC (desc.flush)
 *   also waits for the disk on every row of every table, so a power cut
 *   keeps them too, at milliseconds a row (STATUS). A failed open removes
 *   what it made.
 *
 *   ---------------------------------------------------------------------
 *   SESSION.JSON
 *   ---------------------------------------------------------------------
 *   One JSON object in ysp/json.h's canonical form (keys in bytewise
 *   order, two spaces, LF) and one LF after it. The writer's keys:
 *     "format"        "ysp-session"
 *     "version"       1
 *     "closed"        false from the open; true when yds_finish() ran
 *     "start_utc"     "YYYY-MM-DDTHH:MM:SSZ" at the open, from time(),
 *                     unless desc.session has the key already
 *     "rows"          at the finish: {"clock": n, "events": n, "frames":
 *                     n, "streams": {name: n, ...}, "trials": n}
 *     "write_errors"  at the finish, only when a write failed: one
 *                     string per table that failed, the file and the
 *                     message
 *   Every other key is the caller's: desc.session's members (copied at
 *   the open), yds_session_set() and yds_session_push() later. The keys
 *   the player puts there are in docs/experiment_format.md 2.10 (the
 *   experiment and pack IDs, the rig profile's hash, device rows,
 *   describe lines, session fields, the seed, versions, live changes).
 *   desc.session and yds_session_set() refuse the writer's keys but
 *   "start_utc" (YDS_ERR_ARG).
 *   ATOMIC REWRITE. Each write of session.json (the open, every
 *   yds_session_set() and yds_session_push(), the finish) writes
 *   session.json.tmp, hands it to the disk (_commit() on Windows,
 *   fsync() elsewhere), and renames it over session.json, so a reader or a
 *   crash finds the old file or the new one, never a part. On POSIX
 *   rename() replaces the file atomically and the folder is synced after
 *   it. On Windows the C library's rename() refuses an existing target,
 *   so the writer calls MoveFileExW(MOVEFILE_REPLACE_EXISTING |
 *   MOVEFILE_WRITE_THROUGH): one rename on the volume (NTFS logs it as
 *   one metadata change). It fails while another program holds
 *   session.json open without FILE_SHARE_DELETE (a virus scanner, an
 *   editor that locks); the writer tries 5 times, 10 ms apart, then
 *   returns YDS_ERR_IO, keeps the old session.json and removes the
 *   temporary file. The values stay in the handle, so the next rewrite
 *   writes them.
 *
 *   ---------------------------------------------------------------------
 *   THE TABLES
 *   ---------------------------------------------------------------------
 *   frames.csv, events.csv and clock.csv have fixed columns, from the
 *   structs below (YDS_FRAME_COLS, YDS_EVENT_COLS, YDS_CLOCK_COLS name
 *   them in order). Fill a row with its _init() first, which sets every
 *   field to missing (YTR_MISSING_NS, YTR_MISSING_I64, YTR_MISSING_INT,
 *   NaN, NULL), then set what the row has. A missing field is an empty
 *   cell. trial_index joins a row to trials.csv's trial_index (the
 *   player's column); frame joins events.csv to frames.csv.
 *   frames.csv   frame, trial_index, target_s, planned_s, onset_s,
 *                residual_s, dropped, path, tier, flags, code_risk,
 *                evaluate_s, script_s, draw_s, upload_s, swap_s, gpu_s:
 *                ysp/screen.h's yscr_record (yds_frame_from_screen()).
 *   events.csv   t_s, trial_index, kind, role, stimulus, frame,
 *                planned_s, residual_s, code, control, type, value, x, y,
 *                deadline_s, end_s, tier, flags, ticks, text. kind says
 *                what the row is: a timeline landing ("mark", "trigger",
 *                "onset", "offset", "set", ...; yds_event_from_timeline()),
 *                an input ("keyboard", "mouse", "touch", "pen", "gamepad",
 *                "box", "eye", "sync", "user"; yds_event_from_input()), or
 *                the player's ("response", "out" for an output write with
 *                t_s its start and end_s its end, "visibility").
 *   clock.csv    role, kind ("pair" or "fit"), t_s, ticks, width_s,
 *                ns_per_tick, ppm, spread_s, span_s, points, epoch: a
 *                pair is the host time t_s and the device's ticks; a fit
 *                (yds_clock_from_fit()) maps ticks to host time as
 *                t_s + (ticks' - ticks) x ns_per_tick.
 *   trials.csv and streams/<name>.csv take ytr_col lists as
 *   ytr_data_desc.cols does. yds_trials_writer() and yds_stream_writer()
 *   give the ytr_data for ytr_data_col(), the ytr_data_set_*() setters and
 *   yrsp_data_set(). trials.csv has no session part (ytr_data_desc.trials
 *   NULL, as experiment_format.md 2.10 says: a tree has several
 *   ytr_trials), so the player writes ysp/trials.h v0.6's data-line
 *   columns as its own: each variable as a STRING column of
 *   ytr_var_text(), and trial_seed as a YTR_COL_CHARS field of
 *   YDS_SEED_TEXT bytes filled by yds_seed_text(ytr_trial_seed()), the
 *   same "x" and 16 hex digits as trials' own line, which ysp/table.h,
 *   pandas and R read as text with all 64 bits. A stream's name follows the column rule
 *   ([A-Za-z_][A-Za-z0-9_]*, at most 63 bytes) and differs from every
 *   other stream's in more than case (Windows folders ignore case).
 *   ADAPTERS. Inline, when their header was included before this one:
 *   yds_frame_from_screen() (ysp/screen.h), yds_event_from_input()
 *   (ysp/input.h), yds_event_from_timeline() (ysp/timeline.h),
 *   yds_clock_from_fit() (ysp/rt.h).
 *
 *   ---------------------------------------------------------------------
 *   THE ZIP (the web)
 *   ---------------------------------------------------------------------
 *   yds_zip() writes the finished folder as one zip (store only, no
 *   compression, CRC-32 per entry, UTF-8 names, every date 1980-01-01 so
 *   the same files give the same bytes): <base>/session.json,
 *   <base>/trials.csv, frames.csv, events.csv, clock.csv, <base>/streams/
 *   and each stream's file, <base> the folder's last name. Under
 *   Emscripten the folder is in the in-memory file system, and the zip
 *   goes to a write function (posted or downloaded) or a path. It reads
 *   each file twice (the CRC, then the copy) through a 64 KB buffer from
 *   malloc. A file or an offset of 4 GiB or more is YDS_ERR_RANGE: no
 *   zip64.
 *
 *   ---------------------------------------------------------------------
 *   ERRORS AND RETURN VALUES
 *   ---------------------------------------------------------------------
 *   yds_open() returns bool, clears yds_error() on entry and fills it on
 *   failure; it refuses a handle that holds an open data set (close it
 *   first), so a second open cannot lose the first one's files. Every
 *   other call that can fail returns 0 (or a stream id)
 *   or a negative YDS_ERR_* and fills yds_error(): a write's message
 *   names the file and holds ysp/trials.h's message. After a write
 *   failure a table refuses every later row (YDS_ERR_IO), so a file never
 *   has a row after a hole; the other tables go on. yds_finish() returns
 *   YDS_ERR_IO when any write failed at any time, and still writes
 *   session.json with "closed": true and "write_errors". yds_close()
 *   returns nothing, finishes a data set that is open, and is safe on a
 *   zeroed handle, after a failed open and twice.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   The handle holds a ytr_data per table (about 38 KB each with
 *   ysp/trials.h v0.6's 256 columns: 4 plus YDS_MAX_STREAMS, 16 by
 *   default) and the stream names: about 770 KB on a 64-bit ABI. Give it
 *   static storage or calloc(): the open zeroes the four tables and the
 *   rest, and a stream's writer only when yds_stream() makes it, so the
 *   pages of unused streams are never touched (MinGW, Windows 11: the
 *   working set grows 340 to 352 KB at the open and about 40 KB per
 *   stream; 936 to 948 KB when the open zeroed the whole handle). Define
 *   YDS_MAX_STREAMS smaller before the include to shrink the address
 *   space too. Do not define YTR_DATA_MAX_COLS for this header: it is
 *   ysp/trials.h's, and must match every include of it. session.json's values live in a ysp/json.h heap
 *   arena from the open to the close (a few KB; each set copies its value
 *   in). A row write allocates nothing and makes one write call; the C
 *   library allocates each FILE at its open. yds_zip() allocates its
 *   buffer. One thread writes one data set.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.1.0 - first version (docs/experiment_format.md gap G13): the
 *          folder, session.json with the atomic rewrite, the four tables
 *          and the streams on ysp/trials.h's writer, the adapters, the
 *          stored zip. On ysp/trials.h v0.6 (256 columns, a 38 KB writer):
 *          yds_seed_text() for a trial_seed column in trials' spelling.
 *
 *   STATUS: v0.1.0, 2026-10-21: MSVC 19.44, MinGW-w64 gcc 16.1, WSL2 gcc
 *   11.4 (ASan, UBSan), emcc. On an i7-1360P laptop's SSD (NTFS, MSVC
 *   /O2), median (p99): a frame or event row 12 us (25), a row with
 *   YTR_FLUSH_SYNC 2.9 ms (7.8), an atomic rewrite of session.json 8.2 ms
 *   (13.1): between trials, never in a trial's frames. docs/dataset.md has
 *   the decisions, the test counts and the costs.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_DATASET_H_INCLUDED
#define YSP_DATASET_H_INCLUDED

#define YDS_VERSION_MAJOR 0
#define YDS_VERSION_MINOR 1
#define YDS_VERSION_PATCH 0
#define YDS_VERSION_STRING "0.1.0"

/* mkdir(), fsync() and fileno() under -std=c11: the feature-test macro of
 * ysp/rt.h and ysp/trials.h, set before the first system header (Linux and
 * Emscripten only). */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) && \
    !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif

#include "ysp/trials.h"
#include "ysp/json.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YDS_API
#define YDS_API extern
#endif

#define YDS_FORMAT         "ysp-session"
#define YDS_FORMAT_VERSION 1

#ifndef YDS_MAX_STREAMS
#define YDS_MAX_STREAMS 16       /* streams/<name>.csv files per data set   */
#endif
#define YDS_MAX_DIR     768      /* bytes of desc.dir                       */
#define YDS_MAX_NAME    63       /* bytes of a stream's name                */

/* Results */
#define YDS_OK          0
#define YDS_ERR_ARG    -1   /* a NULL, a bad name, a writer's key, a bad type */
#define YDS_ERR_IO     -2   /* a file could not be made, written or renamed   */
#define YDS_ERR_EXISTS -3   /* the folder exists                              */
#define YDS_ERR_CLOSED -4   /* the data set is not open                       */
#define YDS_ERR_ORDER  -5   /* yds_zip() before yds_finish()                  */
#define YDS_ERR_FULL   -6   /* YDS_MAX_STREAMS streams already                */
#define YDS_ERR_NOMEM  -7   /* the arena or the zip's buffer                  */
#define YDS_ERR_RANGE  -8   /* yds_zip(): a file or an offset of 4 GiB or more */

/* One row of frames.csv: a flip record (yds_frame_from_screen()). */
typedef struct yds_frame {
    int64_t frame;        /* yscr_record.index                               */
    int     trial_index;  /* the trial it belongs to; YTR_MISSING_INT: none  */
    int64_t target_ns;    /* RT ns, as the record                            */
    int64_t planned_ns;
    int64_t onset_ns;
    int64_t residual_ns;
    int     dropped;
    int     path;         /* yscr_path                                       */
    int     tier;         /* yscr_tier                                       */
    int     flags;        /* YSCR_FLIP_*                                     */
    int     code_risk;    /* YSCR_CODE_RISK_*                                */
    int64_t evaluate_ns;  /* phase durations; missing when unknown           */
    int64_t script_ns;
    int64_t draw_ns;
    int64_t upload_ns;
    int64_t swap_ns;
    int64_t gpu_ns;
} yds_frame;

/* One row of events.csv. */
typedef struct yds_event {
    int64_t     t_ns;        /* the event's time, RT ns: an onset, a stamp, a
                              * write's start                               */
    int         trial_index;
    const char* kind;        /* THE TABLES                                  */
    const char* role;        /* the device role; NULL for a landing         */
    const char* stimulus;    /* a landing's stimulus                        */
    int64_t     frame;       /* the frame it landed on                      */
    int64_t     planned_ns;  /* the plan in RT ns                           */
    int64_t     residual_ns; /* a landing's or an output's error            */
    int64_t     code;        /* a trigger or mark code, a key code          */
    int64_t     control;     /* scancode, button, contact, axis             */
    int         type;        /* yin_type                                    */
    double      value, x, y;
    int64_t     deadline_ns; /* an output's deadline                        */
    int64_t     end_ns;      /* an output write's end                       */
    int         tier;        /* stamp tier                                  */
    int         flags;
    int64_t     ticks;       /* the device clock                            */
    const char* text;        /* a response's key name, a state              */
} yds_event;

/* One row of clock.csv. */
typedef struct yds_clock {
    const char* role;
    const char* kind;        /* "pair" or "fit"                             */
    int64_t     t_ns;        /* pair: host time; fit: the map's anchor      */
    int64_t     ticks;       /* pair: the device's ticks; fit: at the anchor */
    int64_t     width_ns;    /* pair: its bracket width                     */
    double      ns_per_tick; /* fit                                         */
    double      ppm;
    int64_t     spread_ns;
    int64_t     span_ns;
    int         points;
    int         epoch;
} yds_clock;

#define YDS_FRAME_COLS 17
#define YDS_EVENT_COLS 20
#define YDS_CLOCK_COLS 11

/* Zero-initialize and set what you need. */
typedef struct yds_desc {
    const char*      dir;          /* UTF-8; made here; must not exist      */
    const yjs_value* session;      /* an object of the caller's keys, or
                                    * NULL; copied at the open              */
    const ytr_col*   trial_cols;   /* trials.csv's columns, as ytr_data_desc
                                    * .cols; required                       */
    int              n_trial_cols;
    ytr_flush        flush;        /* every table                           */
} yds_desc;

/* The data set. The caller allocates it (static storage) and treats every
 * field as opaque. */
typedef struct yds_dataset {
    bool       open;
    bool       finished;    /* yds_finish() ran: yds_zip() may read it      */
    bool       arena_live;
    int        n_streams;
    ytr_flush  flush;
    yjs_value* root;        /* session.json                                 */
    yjs_arena  arena;
    char       dir[YDS_MAX_DIR + 1];
    char       stream_name[YDS_MAX_STREAMS][YDS_MAX_NAME + 1];
    ytr_data   trials, frames, events, clock;
    ytr_data   stream[YDS_MAX_STREAMS];
    char       error[384];
} yds_dataset;

YDS_API const char* yds_version(void);
YDS_API const char* yds_strerror(int code);

/* Make the folder, write session.json ("closed": false) and open the four
 * tables. Returns false with yds_error() set; a failed open removes what it
 * made. */
YDS_API bool yds_open(yds_dataset* ds, const yds_desc* desc);
/* The last failure's message ("" when there is none). */
YDS_API const char* yds_error(const yds_dataset* ds);

/* Set every field to missing. */
YDS_API void yds_frame_init(yds_frame* f);
YDS_API void yds_event_init(yds_event* e);
YDS_API void yds_clock_init(yds_clock* c);

/* One row each, in the OS when the call returns. 0, or YDS_ERR_ARG,
 * YDS_ERR_CLOSED, YDS_ERR_IO with yds_error() set. */
YDS_API int yds_write_trial(yds_dataset* ds, const void* row);
YDS_API int yds_write_frame(yds_dataset* ds, const yds_frame* f);
YDS_API int yds_write_event(yds_dataset* ds, const yds_event* e);
YDS_API int yds_write_clock(yds_dataset* ds, const yds_clock* c);

/* Make streams/<name>.csv with these columns (as ytr_data_desc.cols). The
 * stream's id (0 or more), or YDS_ERR_ARG, YDS_ERR_FULL, YDS_ERR_CLOSED,
 * YDS_ERR_IO. */
YDS_API int yds_stream(yds_dataset* ds, const char* name, const ytr_col* cols, int n_cols);
YDS_API int yds_write_stream(yds_dataset* ds, int id, const void* row);

/* The writers, for ytr_data_col(), ytr_data_set_*() and yrsp_data_set().
 * NULL when the data set is not open or id is not a stream. */
YDS_API ytr_data* yds_trials_writer(yds_dataset* ds);
YDS_API ytr_data* yds_stream_writer(yds_dataset* ds, int id);

/* session.json's key `key` := a copy of v, then the atomic rewrite.
 * yds_session_push() appends a copy of v to the array at `key` (made when
 * absent). 0, or YDS_ERR_ARG (a writer's key, a non-array at the key),
 * YDS_ERR_NOMEM, YDS_ERR_CLOSED, YDS_ERR_IO. */
YDS_API int yds_session_set(yds_dataset* ds, const char* key, const yjs_value* v);
YDS_API int yds_session_push(yds_dataset* ds, const char* key, const yjs_value* v);
/* session.json as it is now, for a read; NULL when not open. */
YDS_API const yjs_value* yds_session(const yds_dataset* ds);

/* A trial seed (ytr_trial_seed()) in ysp/trials.h's data-line spelling,
 * "x" and 16 lowercase hex digits, for a trial_seed column of
 * YTR_COL_CHARS[YDS_SEED_TEXT] in trials.csv (THE TABLES). */
#define YDS_SEED_TEXT 18
YDS_API void yds_seed_text(uint64_t seed, char out[YDS_SEED_TEXT]);

/* Rows written to a table: 0 trials, 1 frames, 2 events, 3 clock, 4 + id a
 * stream; 0 when not open. */
YDS_API int64_t yds_rows(const yds_dataset* ds, int table);
#define YDS_TABLE_TRIALS 0
#define YDS_TABLE_FRAMES 1
#define YDS_TABLE_EVENTS 2
#define YDS_TABLE_CLOCK  3
#define YDS_TABLE_STREAM 4

/* Close every table and rewrite session.json with "closed": true and
 * "rows". 0, or YDS_ERR_IO (a write failed, now or before), YDS_ERR_CLOSED. */
YDS_API int yds_finish(yds_dataset* ds);
/* Finish a data set that is open; free the arena. Safe on a zeroed handle,
 * after a failed open and twice. */
YDS_API void yds_close(yds_dataset* ds);

/* The finished folder as one stored zip (THE ZIP): to `path` (created or
 * truncated) or, with path NULL, to write(ctx, bytes, n). Works after
 * yds_finish(), also after yds_close(). 0, or YDS_ERR_ORDER (not
 * finished), YDS_ERR_ARG, YDS_ERR_IO, YDS_ERR_NOMEM, YDS_ERR_RANGE. */
YDS_API int yds_zip(yds_dataset* ds, const char* path, ytr_write_fn write, void* ctx);

/* --- helpers for the other headers, inline, when they came first ----- */

#if defined(YSP_SCREEN_H_INCLUDED)
/* A flip record as a frames.csv row. */
static inline int64_t yds__phase(uint32_t ns) {
    return ns == YSCR_PHASE_UNKNOWN ? YTR_MISSING_NS : (int64_t)ns;
}
static inline void yds_frame_from_screen(yds_frame* f, const yscr_record* r, int trial_index) {
    yds_frame_init(f);
    f->frame = r->index;
    f->trial_index = trial_index;
    f->target_ns = r->target;
    f->planned_ns = r->planned;
    f->onset_ns = r->onset ? r->onset : YTR_MISSING_NS;   /* 0: never shown */
    f->residual_ns = r->onset ? r->residual : YTR_MISSING_NS;
    f->dropped = (int)r->dropped;
    f->path = r->path;
    f->tier = r->tier;
    f->flags = r->flags;
    f->code_risk = r->code_risk;
    f->evaluate_ns = yds__phase(r->phase_ns[YSCR_PHASE_EVALUATE]);
    f->script_ns = yds__phase(r->phase_ns[YSCR_PHASE_SCRIPT]);
    f->draw_ns = yds__phase(r->phase_ns[YSCR_PHASE_DRAW]);
    f->upload_ns = yds__phase(r->phase_ns[YSCR_PHASE_UPLOAD]);
    f->swap_ns = yds__phase(r->phase_ns[YSCR_PHASE_SWAP]);
    f->gpu_ns = yds__phase(r->phase_ns[YSCR_PHASE_GPU]);
}
#endif

#if defined(YSP_INPUT_H_INCLUDED)
/* An input event of `role` as an events.csv row; kind is the device kind's
 * name. Device ticks only when the event carries them (YIN_DEVTICKS). */
static inline void yds_event_from_input(yds_event* e, const yin_event* in, const char* role, int trial_index) {
    static const char* const kinds[] = { "keyboard", "mouse", "touch", "pen", "gamepad", "box", "eye", "sync" };
    yds_event_init(e);
    e->t_ns = in->t;
    e->trial_index = trial_index;
    e->kind = in->kind < 8 ? kinds[in->kind] : "user";
    e->role = role;
    e->code = in->code;
    e->control = in->control;
    e->type = in->type;
    e->value = in->value;
    e->x = in->x;
    e->y = in->y;
    e->tier = in->stamp;
    e->flags = in->flags;
    if (in->flags & YIN_DEVTICKS) e->ticks = in->ticks;
}
#endif

#if defined(YSP_TIMELINE_H_INCLUDED)
/* A fired timeline event as an events.csv row: t is the frame's predicted
 * onset (RT ns); planned is left to the caller (the base maps it). */
static inline void yds_event_from_timeline(yds_event* e, const ytl_event* ev, const char* stimulus, int trial_index) {
    static const char* const kinds[] = { "mark", "trigger", "onset", "offset", "set", "call_start",
                                         "call_complete", "target_set" };
    yds_event_init(e);
    e->t_ns = ev->frame >= 0 ? ev->onset : YTR_MISSING_NS;
    e->trial_index = trial_index;
    e->kind = ev->kind < 8 ? kinds[ev->kind] : "user";
    e->stimulus = stimulus;
    e->frame = ev->frame >= 0 ? ev->frame : YTR_MISSING_I64;
    e->residual_ns = ev->frame >= 0 ? ev->residual : YTR_MISSING_NS;
    e->code = ev->code;
    e->control = ev->target;
    e->flags = ev->flags;
}
#endif

#if defined(YSP_RT_H_INCLUDED)
/* A clock fit of `role` as a clock.csv row. */
static inline void yds_clock_from_fit(yds_clock* c, const yrt_fit_info* fi, const char* role) {
    yds_clock_init(c);
    c->role = role;
    c->kind = "fit";
    c->t_ns = fi->t0_ns;
    c->ticks = (int64_t)fi->ticks0;
    c->width_ns = fi->width_ns;
    c->ns_per_tick = fi->ns_per_tick;
    c->ppm = fi->ppm;
    c->spread_ns = fi->spread_ns;
    c->span_ns = fi->span_ns;
    c->points = (int)fi->points;
    c->epoch = (int)fi->epoch;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* YSP_DATASET_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_DATASET_IMPLEMENTATION
#ifndef YSP_DATASET_IMPLEMENTATION_GUARD
#define YSP_DATASET_IMPLEMENTATION_GUARD

#ifndef YSP_TRIALS_IMPLEMENTATION_GUARD
    #define YSP_TRIALS_IMPLEMENTATION
    #include "ysp/trials.h"
#endif
#ifndef YSP_JSON_IMPLEMENTATION_GUARD
    #define YSP_JSON_IMPLEMENTATION
    #include "ysp/json.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>        /* _commit */
    #include <share.h>     /* _SH_DENYWR */
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

#ifdef NAN
    #define YDS__NAN ((double)NAN)
#else
    #define YDS__NAN (HUGE_VAL - HUGE_VAL)
#endif

/* The writer's keys in session.json, but "start_utc": the caller may give
 * that one (a test, a resumed session). */
static const char* const yds__reserved[] = { "format", "version", "closed", "rows", "write_errors", NULL };

/* frames.csv, events.csv, clock.csv: the structs above, field by field. */
static const ytr_col yds__frame_cols[YDS_FRAME_COLS] = {
    YTR_COL(yds_frame, frame, YTR_COL_INT64),
    YTR_COL(yds_frame, trial_index, YTR_COL_INT),
    YTR_COL(yds_frame, target_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, planned_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, onset_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, residual_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, dropped, YTR_COL_INT),
    YTR_COL(yds_frame, path, YTR_COL_INT),
    YTR_COL(yds_frame, tier, YTR_COL_INT),
    YTR_COL(yds_frame, flags, YTR_COL_INT),
    YTR_COL(yds_frame, code_risk, YTR_COL_INT),
    YTR_COL(yds_frame, evaluate_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, script_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, draw_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, upload_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, swap_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_frame, gpu_ns, YTR_COL_TIME_NS),
};

static const ytr_col yds__event_cols[YDS_EVENT_COLS] = {
    YTR_COL(yds_event, t_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_event, trial_index, YTR_COL_INT),
    YTR_COL(yds_event, kind, YTR_COL_STRING),
    YTR_COL(yds_event, role, YTR_COL_STRING),
    YTR_COL(yds_event, stimulus, YTR_COL_STRING),
    YTR_COL(yds_event, frame, YTR_COL_INT64),
    YTR_COL(yds_event, planned_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_event, residual_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_event, code, YTR_COL_INT64),
    YTR_COL(yds_event, control, YTR_COL_INT64),
    YTR_COL(yds_event, type, YTR_COL_INT),
    YTR_COL(yds_event, value, YTR_COL_DOUBLE),
    YTR_COL(yds_event, x, YTR_COL_DOUBLE),
    YTR_COL(yds_event, y, YTR_COL_DOUBLE),
    YTR_COL(yds_event, deadline_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_event, end_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_event, tier, YTR_COL_INT),
    YTR_COL(yds_event, flags, YTR_COL_INT),
    YTR_COL(yds_event, ticks, YTR_COL_INT64),
    YTR_COL(yds_event, text, YTR_COL_STRING),
};

static const ytr_col yds__clock_cols[YDS_CLOCK_COLS] = {
    YTR_COL(yds_clock, role, YTR_COL_STRING),
    YTR_COL(yds_clock, kind, YTR_COL_STRING),
    YTR_COL(yds_clock, t_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_clock, ticks, YTR_COL_INT64),
    YTR_COL(yds_clock, width_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_clock, ns_per_tick, YTR_COL_DOUBLE),
    YTR_COL(yds_clock, ppm, YTR_COL_DOUBLE),
    YTR_COL(yds_clock, spread_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_clock, span_ns, YTR_COL_TIME_NS),
    YTR_COL(yds_clock, points, YTR_COL_INT),
    YTR_COL(yds_clock, epoch, YTR_COL_INT),
};

YDS_API const char* yds_version(void) { return YDS_VERSION_STRING; }

YDS_API const char* yds_strerror(int code) {
    switch (code) {
    case YDS_OK:         return "ok";
    case YDS_ERR_ARG:    return "bad argument";
    case YDS_ERR_IO:     return "a file could not be made, written or renamed";
    case YDS_ERR_EXISTS: return "the folder exists";
    case YDS_ERR_CLOSED: return "the data set is not open";
    case YDS_ERR_ORDER:  return "the data set is not finished";
    case YDS_ERR_FULL:   return "no room for another stream";
    case YDS_ERR_NOMEM:  return "out of memory";
    case YDS_ERR_RANGE:  return "a file of 4 GiB or more (no zip64)";
    default:             return "unknown error";
    }
}

static void yds__vfail(yds_dataset* ds, const char* fmt, va_list ap) {
    int n = snprintf(ds->error, sizeof ds->error, "ysp_dataset: ");
    if (n < 0) n = 0;
    vsnprintf(ds->error + n, sizeof ds->error - (size_t)n, fmt, ap);
}

static int yds__fail(yds_dataset* ds, int code, const char* fmt, ...) {
    va_list ap;
    if (!ds) return code;
    va_start(ap, fmt);
    yds__vfail(ds, fmt, ap);
    va_end(ap);
    return code;
}

/* For open(), which returns bool. */
static bool yds__ofail(yds_dataset* ds, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    yds__vfail(ds, fmt, ap);
    va_end(ap);
    return false;
}

YDS_API const char* yds_error(const yds_dataset* ds) { return ds ? ds->error : ""; }

YDS_API void yds_frame_init(yds_frame* f) {
    if (!f) return;
    memset(f, 0, sizeof *f);
    f->frame = YTR_MISSING_I64;
    f->trial_index = f->dropped = f->path = f->tier = f->flags = f->code_risk = YTR_MISSING_INT;
    f->target_ns = f->planned_ns = f->onset_ns = f->residual_ns = YTR_MISSING_NS;
    f->evaluate_ns = f->script_ns = f->draw_ns = f->upload_ns = f->swap_ns = f->gpu_ns = YTR_MISSING_NS;
}

YDS_API void yds_event_init(yds_event* e) {
    if (!e) return;
    memset(e, 0, sizeof *e);   /* the strings NULL */
    e->t_ns = e->planned_ns = e->residual_ns = e->deadline_ns = e->end_ns = YTR_MISSING_NS;
    e->frame = e->code = e->control = e->ticks = YTR_MISSING_I64;
    e->trial_index = e->type = e->tier = e->flags = YTR_MISSING_INT;
    e->value = e->x = e->y = YDS__NAN;
}

YDS_API void yds_clock_init(yds_clock* c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->t_ns = c->width_ns = c->spread_ns = c->span_ns = YTR_MISSING_NS;
    c->ticks = YTR_MISSING_I64;
    c->ns_per_tick = c->ppm = YDS__NAN;
    c->points = c->epoch = YTR_MISSING_INT;
}

/* ======================================================================= *
 *  FILES
 * ======================================================================= */

#if defined(_WIN32)
static bool yds__wide(const char* s, wchar_t* w, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) > 0;
}
#endif

static FILE* yds__fopen(const char* path, const char* mode) {
#if defined(_WIN32)
    wchar_t w[1100], m[8];
    int i;
    if (!yds__wide(path, w, 1100)) return NULL;
    for (i = 0; mode[i] && i < 7; i++) m[i] = (wchar_t)mode[i];
    m[i] = 0;
    /* Others may read a file while it is written (a monitor, a copy). */
    return _wfsopen(w, m, mode[0] == 'r' ? _SH_DENYNO : _SH_DENYWR);
#else
    return fopen(path, mode);
#endif
}

/* 0, or the errno (POSIX) or GetLastError() (Windows) of the failure, and
 * *exists when the path was there already. */
static int yds__mkdir(const char* path, bool* exists) {
    *exists = false;
#if defined(_WIN32)
    {
        wchar_t w[1100];
        DWORD e;
        if (!yds__wide(path, w, 1100)) return (int)ERROR_INVALID_NAME;
        if (CreateDirectoryW(w, NULL)) return 0;
        e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) *exists = true;
        return (int)e;
    }
#else
    if (mkdir(path, 0777) == 0) return 0;
    if (errno == EEXIST) *exists = true;
    return errno;
#endif
}

static void yds__remove(const char* path) {
#if defined(_WIN32)
    wchar_t w[1100];
    if (yds__wide(path, w, 1100)) DeleteFileW(w);
#else
    remove(path);
#endif
}

static void yds__rmdir(const char* path) {
#if defined(_WIN32)
    wchar_t w[1100];
    if (yds__wide(path, w, 1100)) RemoveDirectoryW(w);
#else
    rmdir(path);
#endif
}

static bool yds__sync(FILE* f) {
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

/* The rename of tmp over path; 0 or the OS's error number. */
static int yds__replace(const char* tmp, const char* path, const char* dir) {
#if defined(_WIN32)
    wchar_t wt[1100], w[1100];
    int tries;
    DWORD e = 0;
    (void)dir;
    if (!yds__wide(tmp, wt, 1100) || !yds__wide(path, w, 1100)) return (int)ERROR_INVALID_NAME;
    /* A scanner or an indexer may hold the old file for a moment; the
     * rename then fails with a sharing or an access error. */
    for (tries = 0; tries < 5; tries++) {
        if (MoveFileExW(wt, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
        e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) break;
        Sleep(10);
    }
    return e ? (int)e : -1;
#else
    int fd;
    if (rename(tmp, path) != 0) return errno ? errno : -1;
    /* The rename is in the folder's entries: sync them, so a power cut
     * after this keeps the new file. A file system that cannot sync a
     * folder (EINVAL) has nothing to sync. */
    fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        if (fsync(fd) != 0) { /* EINVAL: nothing to sync; else the rename stands */ }
        close(fd);
    }
    return 0;
#endif
}

static int yds__jput(void* ctx, const char* bytes, size_t n) {
    return fwrite(bytes, 1, n, (FILE*)ctx) == n ? 0 : 1;
}

/* dir/name, cut to cap; every caller's buffer holds the longest. A copy,
 * not snprintf: gcc's truncation warning cannot see the bounds. */
static void yds__path(char* out, size_t cap, const char* dir, const char* name) {
    size_t a = strlen(dir), b = strlen(name);
    if (cap == 0) return;
    if (a > cap - 1) a = cap - 1;
    memcpy(out, dir, a);
    if (a + 1 < cap) out[a++] = '/';
    if (b > cap - 1 - a) b = cap - 1 - a;
    memcpy(out + a, name, b);
    out[a + b] = '\0';
}

/* session.json through session.json.tmp: written, on the disk, renamed. */
static int yds__write_session(yds_dataset* ds, const char* what) {
    char tmp[YDS_MAX_DIR + 32], path[YDS_MAX_DIR + 32];
    FILE* f;
    bool ok;
    int e;
    yds__path(tmp, sizeof tmp, ds->dir, "session.json.tmp");
    yds__path(path, sizeof path, ds->dir, "session.json");
    f = yds__fopen(tmp, "wb");
    if (!f) return yds__fail(ds, YDS_ERR_IO, "%s: cannot create '%s' (errno %d)", what, tmp, errno);
    ok = yjs_write(ds->root, YJS_WRITE_PRETTY, yds__jput, f) == YJS_OK;
    ok = ok && fputc('\n', f) != EOF;   /* one LF after the value: a text file */
    ok = ok && fflush(f) == 0;
    ok = ok && yds__sync(f);
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        yds__remove(tmp);
        return yds__fail(ds, YDS_ERR_IO, "%s: cannot write '%s' (errno %d)", what, tmp, errno);
    }
    if ((e = yds__replace(tmp, path, ds->dir)) != 0) {
        yds__remove(tmp);
        return yds__fail(ds, YDS_ERR_IO, "%s: cannot rename '%s' over session.json (error %d); "
                         "the old session.json stays", what, tmp, e);
    }
    return YDS_OK;
}

static bool yds__is_name(const char* s) {
    size_t i, n = s ? strlen(s) : 0;
    if (n == 0 || n > YDS_MAX_NAME) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!letter && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

static bool yds__same_ci(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
}

static bool yds__reserved_key(const char* key) {
    int i;
    for (i = 0; yds__reserved[i]; i++)
        if (strcmp(key, yds__reserved[i]) == 0) return true;
    return false;
}

static bool yds__discard(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    (void)bytes;
    (void)n;
    return true;
}

/* Columns checked with no file, so a refused column makes nothing. */
static int yds__check_cols(yds_dataset* ds, ytr_data* w, const ytr_col* cols, int n, const char* what) {
    ytr_data_desc dd;
    int rc = YDS_OK;
    memset(&dd, 0, sizeof dd);
    dd.write = yds__discard;
    dd.cols = cols;
    dd.n_cols = n;
    if (!ytr_data_open(w, &dd)) rc = yds__fail(ds, YDS_ERR_ARG, "%s: %s", what, ytr_data_error(w));
    ytr_data_close(w);
    memset(w, 0, sizeof *w);
    return rc;
}

static int yds__open_table(yds_dataset* ds, ytr_data* w, const char* name, const ytr_col* cols, int n,
                           const char* what) {
    char path[YDS_MAX_DIR + 96];
    ytr_data_desc dd;
    yds__path(path, sizeof path, ds->dir, name);
    memset(&dd, 0, sizeof dd);
    dd.path = path;
    dd.cols = cols;
    dd.n_cols = n;
    dd.flush = ds->flush;
    if (!ytr_data_open(w, &dd)) return yds__fail(ds, YDS_ERR_IO, "%s: %s: %s", what, name, ytr_data_error(w));
    return YDS_OK;
}

/* A failed open leaves nothing behind: the folder held no data yet. */
static void yds__undo(yds_dataset* ds, bool made_dir) {
    static const char* const files[] = { "trials.csv", "frames.csv", "events.csv", "clock.csv",
                                         "session.json", "session.json.tmp", NULL };
    char p[YDS_MAX_DIR + 32];
    int i;
    ytr_data_close(&ds->trials);
    ytr_data_close(&ds->frames);
    ytr_data_close(&ds->events);
    ytr_data_close(&ds->clock);
    if (made_dir) {
        for (i = 0; files[i]; i++) {
            yds__path(p, sizeof p, ds->dir, files[i]);
            yds__remove(p);
        }
        yds__path(p, sizeof p, ds->dir, "streams");
        yds__rmdir(p);
        yds__rmdir(ds->dir);
    }
    if (ds->arena_live) yjs_arena_free(&ds->arena);
    ds->arena_live = false;
    ds->root = NULL;
}

static bool yds__utc(char out[80]) {   /* 80: room for any int, so no truncation warning */
    time_t t = time(NULL);
    struct tm tm;
#if defined(_MSC_VER)
    if (gmtime_s(&tm, &t) != 0) return false;
#else
    {
        struct tm* p = gmtime(&t);   /* the open runs on one thread */
        if (!p) return false;
        tm = *p;
    }
#endif
    snprintf(out, 80, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

YDS_API bool yds_open(yds_dataset* ds, const yds_desc* desc) {
    size_t n;
    uint32_t i;
    int e, rc;
    bool exists;
    char p[YDS_MAX_DIR + 32];
    if (!ds) return false;
    /* An open data set's files would be lost to the memset below. */
    if (ds->open) return yds__ofail(ds, "yds_open: this handle holds an open data set; close it first");
    if (ds->arena_live) yjs_arena_free(&ds->arena);
    /* Not the streams: each is zeroed when yds_stream() makes it, so a
     * static or calloc'd handle keeps the pages of unused streams
     * untouched (16 x 38 KB with ysp/trials.h v0.6). */
    memset(ds, 0, offsetof(yds_dataset, stream));
    memset(ds->error, 0, sizeof ds->error);
    if (!desc) return yds__ofail(ds, "yds_open: null desc");
    if (!desc->dir || !desc->dir[0]) return yds__ofail(ds, "yds_open: no dir");
    n = strlen(desc->dir);
    if (n > YDS_MAX_DIR) return yds__ofail(ds, "yds_open: dir is longer than %d bytes", YDS_MAX_DIR);
    memcpy(ds->dir, desc->dir, n + 1);
    /* A trailing separator would double in every path and empty the zip's
     * folder name. */
    while (n > 1 && (ds->dir[n - 1] == '/' || ds->dir[n - 1] == '\\')) ds->dir[--n] = '\0';
    if (!yjs_utf8_ok(ds->dir, n)) return yds__ofail(ds, "yds_open: dir is not UTF-8");
    if ((int)desc->flush < (int)YTR_FLUSH_ROW || (int)desc->flush > (int)YTR_FLUSH_SYNC)
        return yds__ofail(ds, "yds_open: flush %d is not a ytr_flush", (int)desc->flush);
    ds->flush = desc->flush;
    if (!desc->trial_cols || desc->n_trial_cols <= 0)
        return yds__ofail(ds, "yds_open: no trial_cols (trials.csv needs a column)");
    if (desc->session) {
        if (desc->session->type != YJS_OBJECT) return yds__ofail(ds, "yds_open: session is not an object");
        for (i = 0; i < desc->session->n; i++)
            if (yds__reserved_key(desc->session->u.members[i].key))
                return yds__ofail(ds, "yds_open: session has '%s', which the writer sets",
                                  desc->session->u.members[i].key);
    }
    if (yds__check_cols(ds, &ds->trials, desc->trial_cols, desc->n_trial_cols, "yds_open: trial_cols") < 0)
        return false;

    yjs_arena_init_heap(&ds->arena, 0);
    ds->arena_live = true;
    ds->root = desc->session ? yjs_copy(&ds->arena, desc->session) : yjs_new(&ds->arena, YJS_OBJECT);
    rc = ds->root ? YJS_OK : YJS_ERR_NOMEM;
    if (rc == YJS_OK) rc = yjs_set(&ds->arena, ds->root, "format", yjs_new_stringz(&ds->arena, YDS_FORMAT));
    if (rc == YJS_OK) rc = yjs_set(&ds->arena, ds->root, "version", yjs_new_int(&ds->arena, YDS_FORMAT_VERSION));
    if (rc == YJS_OK) rc = yjs_set(&ds->arena, ds->root, "closed", yjs_new(&ds->arena, YJS_FALSE));
    if (rc == YJS_OK && !yjs_get(ds->root, "start_utc")) {
        char utc[80];
        rc = yds__utc(utc) ? yjs_set(&ds->arena, ds->root, "start_utc", yjs_new_stringz(&ds->arena, utc))
                           : YJS_ERR_ARG;
    }
    if (rc != YJS_OK) {
        yds__undo(ds, false);
        return yds__ofail(ds, "yds_open: session.json's values: %s",
                          rc == YJS_ERR_NOMEM ? "out of memory" : "no UTC time from the C library");
    }

    if ((e = yds__mkdir(ds->dir, &exists)) != 0) {
        yds__undo(ds, false);
        if (exists)
            return yds__ofail(ds, "yds_open: '%s' exists; a data set never goes into a folder that is "
                              "there already", ds->dir);
        return yds__ofail(ds, "yds_open: cannot make '%s' (error %d; does its parent exist?)", ds->dir, e);
    }
    yds__path(p, sizeof p, ds->dir, "streams");
    if ((e = yds__mkdir(p, &exists)) != 0) {
        yds__undo(ds, true);
        return yds__ofail(ds, "yds_open: cannot make '%s' (error %d)", p, e);
    }
    rc = yds__write_session(ds, "yds_open");
    if (rc == YDS_OK)
        rc = yds__open_table(ds, &ds->trials, "trials.csv", desc->trial_cols, desc->n_trial_cols, "yds_open");
    if (rc == YDS_OK) rc = yds__open_table(ds, &ds->frames, "frames.csv", yds__frame_cols, YDS_FRAME_COLS, "yds_open");
    if (rc == YDS_OK) rc = yds__open_table(ds, &ds->events, "events.csv", yds__event_cols, YDS_EVENT_COLS, "yds_open");
    if (rc == YDS_OK) rc = yds__open_table(ds, &ds->clock, "clock.csv", yds__clock_cols, YDS_CLOCK_COLS, "yds_open");
    if (rc != YDS_OK) {
        /* The undo closes tables, which may write their own messages. */
        char msg[sizeof ds->error];
        memcpy(msg, ds->error, sizeof msg);
        yds__undo(ds, true);
        memcpy(ds->error, msg, sizeof msg);
        return false;
    }
    ds->error[0] = '\0';
    ds->open = true;
    return true;
}

/* A table's write, with its message under the file's name. */
static int yds__write(yds_dataset* ds, ytr_data* w, const char* name, const void* row, const char* what) {
    int rc;
    if (!ds) return YDS_ERR_ARG;
    if (!ds->open) return yds__fail(ds, YDS_ERR_CLOSED, "%s: the data set is not open", what);
    rc = ytr_data_write(w, row);
    if (rc == 0) return YDS_OK;
    return yds__fail(ds, rc == YTR_ERR_IO ? YDS_ERR_IO : rc == YTR_ERR_CLOSED ? YDS_ERR_CLOSED : YDS_ERR_ARG,
                     "%s: %s: %s", what, name, ytr_data_error(w));
}

YDS_API int yds_write_trial(yds_dataset* ds, const void* row) {
    return yds__write(ds, ds ? &ds->trials : NULL, "trials.csv", row, "yds_write_trial");
}

YDS_API int yds_write_frame(yds_dataset* ds, const yds_frame* f) {
    if (ds && ds->open && !f) return yds__fail(ds, YDS_ERR_ARG, "yds_write_frame: null row");
    return yds__write(ds, ds ? &ds->frames : NULL, "frames.csv", f, "yds_write_frame");
}

YDS_API int yds_write_event(yds_dataset* ds, const yds_event* e) {
    if (ds && ds->open && !e) return yds__fail(ds, YDS_ERR_ARG, "yds_write_event: null row");
    return yds__write(ds, ds ? &ds->events : NULL, "events.csv", e, "yds_write_event");
}

YDS_API int yds_write_clock(yds_dataset* ds, const yds_clock* c) {
    if (ds && ds->open && !c) return yds__fail(ds, YDS_ERR_ARG, "yds_write_clock: null row");
    return yds__write(ds, ds ? &ds->clock : NULL, "clock.csv", c, "yds_write_clock");
}

YDS_API int yds_stream(yds_dataset* ds, const char* name, const ytr_col* cols, int n_cols) {
    char file[YDS_MAX_NAME + 16];
    int i, id, rc;
    if (!ds) return YDS_ERR_ARG;
    if (!ds->open) return yds__fail(ds, YDS_ERR_CLOSED, "yds_stream: the data set is not open");
    if (!yds__is_name(name))
        return yds__fail(ds, YDS_ERR_ARG, "yds_stream: '%s' is not a name ([A-Za-z_][A-Za-z0-9_]*, at most %d "
                         "bytes)", name ? name : "(null)", YDS_MAX_NAME);
    for (i = 0; i < ds->n_streams; i++)
        if (yds__same_ci(ds->stream_name[i], name))
            return yds__fail(ds, YDS_ERR_ARG, "yds_stream: '%s' is taken by '%s' (names differ in more than "
                             "case)", name, ds->stream_name[i]);
    if (ds->n_streams >= YDS_MAX_STREAMS)
        return yds__fail(ds, YDS_ERR_FULL, "yds_stream: %d streams already (YDS_MAX_STREAMS)", YDS_MAX_STREAMS);
    id = ds->n_streams;
    if ((rc = yds__check_cols(ds, &ds->stream[id], cols, n_cols, "yds_stream")) < 0) return rc;
    snprintf(file, sizeof file, "streams/%s.csv", name);
    if ((rc = yds__open_table(ds, &ds->stream[id], file, cols, n_cols, "yds_stream")) < 0) return rc;
    memcpy(ds->stream_name[id], name, strlen(name) + 1);
    ds->n_streams++;
    return id;
}

YDS_API int yds_write_stream(yds_dataset* ds, int id, const void* row) {
    char file[YDS_MAX_NAME + 16];
    if (!ds) return YDS_ERR_ARG;
    if (!ds->open) return yds__fail(ds, YDS_ERR_CLOSED, "yds_write_stream: the data set is not open");
    if (id < 0 || id >= ds->n_streams) return yds__fail(ds, YDS_ERR_ARG, "yds_write_stream: no stream %d", id);
    snprintf(file, sizeof file, "streams/%s.csv", ds->stream_name[id]);
    return yds__write(ds, &ds->stream[id], file, row, "yds_write_stream");
}

YDS_API ytr_data* yds_trials_writer(yds_dataset* ds) { return ds && ds->open ? &ds->trials : NULL; }

YDS_API ytr_data* yds_stream_writer(yds_dataset* ds, int id) {
    return ds && ds->open && id >= 0 && id < ds->n_streams ? &ds->stream[id] : NULL;
}

YDS_API const yjs_value* yds_session(const yds_dataset* ds) { return ds && ds->open ? ds->root : NULL; }

YDS_API void yds_seed_text(uint64_t seed, char out[YDS_SEED_TEXT]) {
    static const char hx[] = "0123456789abcdef";
    int i;
    out[0] = 'x';
    for (i = 0; i < 16; i++) out[1 + i] = hx[(seed >> (60 - 4 * i)) & 15u];
    out[17] = '\0';
}

YDS_API int64_t yds_rows(const yds_dataset* ds, int table) {
    if (!ds || !ds->open) return 0;
    switch (table) {
    case YDS_TABLE_TRIALS: return ytr_data_rows(&ds->trials);
    case YDS_TABLE_FRAMES: return ytr_data_rows(&ds->frames);
    case YDS_TABLE_EVENTS: return ytr_data_rows(&ds->events);
    case YDS_TABLE_CLOCK:  return ytr_data_rows(&ds->clock);
    default:
        if (table >= YDS_TABLE_STREAM && table - YDS_TABLE_STREAM < ds->n_streams)
            return ytr_data_rows(&ds->stream[table - YDS_TABLE_STREAM]);
        return 0;
    }
}

static int yds__session_key(yds_dataset* ds, const char* key, const yjs_value* v, const char* what) {
    if (!ds) return YDS_ERR_ARG;
    if (!ds->open) return yds__fail(ds, YDS_ERR_CLOSED, "%s: the data set is not open", what);
    if (!key || !v) return yds__fail(ds, YDS_ERR_ARG, "%s: null key or value", what);
    if (yds__reserved_key(key)) return yds__fail(ds, YDS_ERR_ARG, "%s: '%s' is the writer's key", what, key);
    return YDS_OK;
}

YDS_API int yds_session_set(yds_dataset* ds, const char* key, const yjs_value* v) {
    yjs_value* c;
    int rc = yds__session_key(ds, key, v, "yds_session_set");
    if (rc < 0) return rc;
    c = yjs_copy(&ds->arena, v);
    if (!c) return yds__fail(ds, YDS_ERR_NOMEM, "yds_session_set: '%s': out of memory", key);
    if ((rc = yjs_set(&ds->arena, ds->root, key, c)) != YJS_OK)
        return yds__fail(ds, rc == YJS_ERR_NOMEM ? YDS_ERR_NOMEM : YDS_ERR_ARG,
                         "yds_session_set: '%s': the key is not UTF-8, or out of memory", key);
    return yds__write_session(ds, "yds_session_set");
}

YDS_API int yds_session_push(yds_dataset* ds, const char* key, const yjs_value* v) {
    yjs_value *c, *arr;
    int rc = yds__session_key(ds, key, v, "yds_session_push");
    if (rc < 0) return rc;
    arr = yjs_get(ds->root, key);
    if (arr && arr->type != YJS_ARRAY)
        return yds__fail(ds, YDS_ERR_ARG, "yds_session_push: '%s' holds a value that is not an array", key);
    c = yjs_copy(&ds->arena, v);
    if (!c) return yds__fail(ds, YDS_ERR_NOMEM, "yds_session_push: '%s': out of memory", key);
    if (!arr) {
        arr = yjs_new(&ds->arena, YJS_ARRAY);
        if (!arr || (rc = yjs_set(&ds->arena, ds->root, key, arr)) != YJS_OK)
            return yds__fail(ds, YDS_ERR_NOMEM, "yds_session_push: '%s': the key is not UTF-8, or out of memory",
                             key);
    }
    if (yjs_push(&ds->arena, arr, c) != YJS_OK)
        return yds__fail(ds, YDS_ERR_NOMEM, "yds_session_push: '%s': out of memory", key);
    return yds__write_session(ds, "yds_session_push");
}

/* One table's end: its rows, and its failure in write_errors. */
static bool yds__end_table(yds_dataset* ds, ytr_data* w, const char* name, yjs_value* rows, const char* key,
                           yjs_value* errs) {
    int64_t n = ytr_data_rows(w);
    bool ok = ytr_data_finish(w) == 0;
    yjs_set(&ds->arena, rows, key, yjs_new_int(&ds->arena, n));
    if (!ok) {
        char m[sizeof w->error + YDS_MAX_NAME + 16];
        snprintf(m, sizeof m, "%s: %s", name, ytr_data_error(w));
        yjs_push(&ds->arena, errs, yjs_new_stringz(&ds->arena, m));
    }
    return ok;
}

YDS_API int yds_finish(yds_dataset* ds) {
    yjs_value *rows, *streams, *errs;
    bool ok = true;
    int i, rc;
    char name[YDS_MAX_NAME + 16];
    if (!ds) return YDS_ERR_ARG;
    if (!ds->open) return yds__fail(ds, YDS_ERR_CLOSED, "yds_finish: the data set is not open");
    rows = yjs_new(&ds->arena, YJS_OBJECT);
    streams = yjs_new(&ds->arena, YJS_OBJECT);
    errs = yjs_new(&ds->arena, YJS_ARRAY);
    if (!rows || !streams || !errs) {
        /* Still close the files: their rows are on disk either way. */
        ytr_data_close(&ds->trials);
        ytr_data_close(&ds->frames);
        ytr_data_close(&ds->events);
        ytr_data_close(&ds->clock);
        for (i = 0; i < ds->n_streams; i++) ytr_data_close(&ds->stream[i]);
        ds->open = false;
        ds->finished = true;
        return yds__fail(ds, YDS_ERR_NOMEM, "yds_finish: out of memory; session.json still says closed false");
    }
    ok = yds__end_table(ds, &ds->trials, "trials.csv", rows, "trials", errs) && ok;
    ok = yds__end_table(ds, &ds->frames, "frames.csv", rows, "frames", errs) && ok;
    ok = yds__end_table(ds, &ds->events, "events.csv", rows, "events", errs) && ok;
    ok = yds__end_table(ds, &ds->clock, "clock.csv", rows, "clock", errs) && ok;
    for (i = 0; i < ds->n_streams; i++) {
        snprintf(name, sizeof name, "streams/%s.csv", ds->stream_name[i]);
        ok = yds__end_table(ds, &ds->stream[i], name, streams, ds->stream_name[i], errs) && ok;
    }
    yjs_set(&ds->arena, rows, "streams", streams);
    yjs_set(&ds->arena, ds->root, "rows", rows);
    if (errs->n) yjs_set(&ds->arena, ds->root, "write_errors", errs);
    yjs_set(&ds->arena, ds->root, "closed", yjs_new(&ds->arena, YJS_TRUE));
    ds->open = false;
    ds->finished = true;
    rc = yds__write_session(ds, "yds_finish");
    if (rc < 0) return rc;
    if (!ok)
        return yds__fail(ds, YDS_ERR_IO, "yds_finish: %s", errs->n ? errs->u.items[0]->u.s : "a write failed");
    return YDS_OK;
}

YDS_API void yds_close(yds_dataset* ds) {
    if (!ds) return;
    if (ds->open) (void)yds_finish(ds);
    if (ds->arena_live) yjs_arena_free(&ds->arena);
    ds->arena_live = false;
    ds->root = NULL;
}

/* ======================================================================= *
 *  ZIP
 * ======================================================================= */

typedef struct yds__zip {
    FILE*        out;
    ytr_write_fn write;
    void*        ctx;
    bool         bad;
    uint64_t     at;          /* bytes written                             */
    uint32_t     crc_table[256];
} yds__zip;

static void yds__zput(yds__zip* z, const void* b, size_t n) {
    if (z->bad || n == 0) return;
    if (z->out) z->bad = fwrite(b, 1, n, z->out) != n;
    else z->bad = !z->write(z->ctx, b, n);
    z->at += n;
}

static void yds__w16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void yds__w32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* A file's CRC-32 and size, read through buf; false when it cannot be read. */
static bool yds__crc_file(yds__zip* z, const char* path, unsigned char* buf, size_t cap, uint32_t* crc, uint64_t* size) {
    FILE* f = yds__fopen(path, "rb");
    uint32_t c = 0xFFFFFFFFu;
    uint64_t total = 0;
    size_t k, i;
    if (!f) return false;
    while ((k = fread(buf, 1, cap, f)) > 0) {
        for (i = 0; i < k; i++) c = z->crc_table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
        total += k;
    }
    k = (size_t)ferror(f);
    fclose(f);
    *crc = ~c;
    *size = total;
    return k == 0;
}

YDS_API int yds_zip(yds_dataset* ds, const char* path, ytr_write_fn write, void* ctx) {
    /* The entries: session.json, the four tables, the streams' folder, each
     * stream. */
    enum { MAXE = 6 + YDS_MAX_STREAMS };
    char rel[MAXE][YDS_MAX_NAME + 16];
    uint32_t crc[MAXE];
    uint64_t size[MAXE], off[MAXE];
    char full[YDS_MAX_DIR + YDS_MAX_NAME + 32], name[YDS_MAX_DIR + YDS_MAX_NAME + 32];
    const char* base;
    size_t bl;
    int n = 0, i, rc = YDS_OK;
    unsigned char* buf;
    const size_t cap = (size_t)64 << 10;
    yds__zip z;
    uint64_t cd_start;
    unsigned char h[64];
    if (!ds) return YDS_ERR_ARG;
    if (ds->open || !ds->finished)
        return yds__fail(ds, YDS_ERR_ORDER, "yds_zip: the data set is %s; finish it first",
                         ds->open ? "still open" : "not finished");
    if (!path && !write) return yds__fail(ds, YDS_ERR_ARG, "yds_zip: no path and no write function");
    memset(&z, 0, sizeof z);
    for (i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)i;
        int k;
        for (k = 0; k < 8; k++) v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1u)));
        z.crc_table[i] = v;
    }
    base = ds->dir + strlen(ds->dir);
    while (base > ds->dir && base[-1] != '/' && base[-1] != '\\') base--;
    bl = strlen(base);
    if (bl == 0 || bl > 255) return yds__fail(ds, YDS_ERR_ARG, "yds_zip: the folder's name is empty or too long");
    {
        static const char* const fixed[] = { "session.json", "trials.csv", "frames.csv", "events.csv",
                                             "clock.csv", "streams/" };
        for (i = 0; i < 6; i++) snprintf(rel[n++], sizeof rel[0], "%s", fixed[i]);
    }
    for (i = 0; i < ds->n_streams; i++) snprintf(rel[n++], sizeof rel[0], "streams/%s.csv", ds->stream_name[i]);

    buf = (unsigned char*)malloc(cap);
    if (!buf) return yds__fail(ds, YDS_ERR_NOMEM, "yds_zip: out of memory (%lu bytes)", (unsigned long)cap);
    for (i = 0; i < n; i++) {
        size_t rl = strlen(rel[i]);
        crc[i] = 0;
        size[i] = 0;
        if (rel[i][rl - 1] == '/') continue;
        yds__path(full, sizeof full, ds->dir, rel[i]);
        if (!yds__crc_file(&z, full, buf, cap, &crc[i], &size[i])) {
            rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: cannot read '%s' (errno %d)", full, errno);
            break;
        }
        if (size[i] >= 0xFFFFFFFFu) {
            rc = yds__fail(ds, YDS_ERR_RANGE, "yds_zip: '%s' has %llu bytes; a zip without zip64 holds less "
                           "than 4 GiB", full, (unsigned long long)size[i]);
            break;
        }
    }
    if (rc == YDS_OK && path) {
        z.out = yds__fopen(path, "wb");
        if (!z.out) rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: cannot create '%s' (errno %d)", path, errno);
    } else {
        z.write = write;
        z.ctx = ctx;
    }
    for (i = 0; rc == YDS_OK && i < n; i++) {
        size_t nl = (size_t)snprintf(name, sizeof name, "%s/%s", base, rel[i]);
        bool dir = rel[i][strlen(rel[i]) - 1] == '/';
        off[i] = z.at;
        if (z.at >= 0xFFFFFFFFu) {
            rc = yds__fail(ds, YDS_ERR_RANGE, "yds_zip: the zip passes 4 GiB at '%s' (no zip64)", rel[i]);
            break;
        }
        /* The local header: stored, UTF-8 names (bit 11), 1980-01-01 00:00. */
        yds__w32(h, 0x04034B50u);
        yds__w16(h + 4, 20);
        yds__w16(h + 6, 0x0800u);
        yds__w16(h + 8, 0);
        yds__w16(h + 10, 0);
        yds__w16(h + 12, 0x21);
        yds__w32(h + 14, crc[i]);
        yds__w32(h + 18, (uint32_t)size[i]);
        yds__w32(h + 22, (uint32_t)size[i]);
        yds__w16(h + 26, (uint32_t)nl);
        yds__w16(h + 28, 0);
        yds__zput(&z, h, 30);
        yds__zput(&z, name, nl);
        if (!dir) {
            FILE* f;
            uint64_t got = 0;
            size_t k;
            yds__path(full, sizeof full, ds->dir, rel[i]);
            f = yds__fopen(full, "rb");
            if (!f) {
                rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: cannot read '%s' (errno %d)", full, errno);
                break;
            }
            while ((k = fread(buf, 1, cap, f)) > 0 && got + k <= size[i]) {
                yds__zput(&z, buf, k);
                got += k;
            }
            fclose(f);
            /* The CRC pass and the copy must see one file. */
            if (got != size[i] || k != 0) {
                rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: '%s' changed while it was zipped", full);
                break;
            }
        }
        if (z.bad) rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: the write failed at byte %llu", (unsigned long long)z.at);
    }
    cd_start = z.at;
    for (i = 0; rc == YDS_OK && i < n; i++) {
        size_t nl = (size_t)snprintf(name, sizeof name, "%s/%s", base, rel[i]);
        bool dir = rel[i][strlen(rel[i]) - 1] == '/';
        yds__w32(h, 0x02014B50u);
        yds__w16(h + 4, 0x0314u);      /* made by: Unix, 2.0, so the modes below count */
        yds__w16(h + 6, 20);
        yds__w16(h + 8, 0x0800u);
        yds__w16(h + 10, 0);
        yds__w16(h + 12, 0);
        yds__w16(h + 14, 0x21);
        yds__w32(h + 16, crc[i]);
        yds__w32(h + 20, (uint32_t)size[i]);
        yds__w32(h + 24, (uint32_t)size[i]);
        yds__w16(h + 28, (uint32_t)nl);
        yds__w16(h + 30, 0);
        yds__w16(h + 32, 0);
        yds__w16(h + 34, 0);
        yds__w16(h + 36, 0);
        yds__w32(h + 38, dir ? (040755u << 16) | 0x10u : 0100644u << 16);
        yds__w32(h + 42, (uint32_t)off[i]);
        yds__zput(&z, h, 46);
        yds__zput(&z, name, nl);
    }
    if (rc == YDS_OK) {
        if (z.at >= 0xFFFFFFFFu) {
            rc = yds__fail(ds, YDS_ERR_RANGE, "yds_zip: the zip passes 4 GiB (no zip64)");
        } else {
            yds__w32(h, 0x06054B50u);
            yds__w16(h + 4, 0);
            yds__w16(h + 6, 0);
            yds__w16(h + 8, (uint32_t)n);
            yds__w16(h + 10, (uint32_t)n);
            yds__w32(h + 12, (uint32_t)(z.at - cd_start));
            yds__w32(h + 16, (uint32_t)cd_start);
            yds__w16(h + 20, 0);
            yds__zput(&z, h, 22);
            if (z.bad) rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: the write failed at byte %llu", (unsigned long long)z.at);
        }
    }
    if (z.out && fclose(z.out) != 0 && rc == YDS_OK)
        rc = yds__fail(ds, YDS_ERR_IO, "yds_zip: closing '%s' failed (errno %d)", path, errno);
    free(buf);
    return rc;
}

#endif /* YSP_DATASET_IMPLEMENTATION_GUARD */
#endif /* YSP_DATASET_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
