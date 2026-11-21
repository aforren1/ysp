/* player/player.h - the ysp player's internal interface.
 *
 * The player loads a pack, checks its definition (ysp/experiment.h) against
 * the pack and the rig, lowers it onto the ysp headers and runs it: in a
 * real window with the rig's devices, or on the simulated display (--sim).
 * docs/player.md describes what is built and the decisions lowering forced.
 *
 * The code is split so that one test can reach each part: eval.c (values),
 * host.c (the loader's checks that need the pack and the rig), lower.c (the
 * definition to desc structs and op tables), run.c (the tree, the trial
 * state, the data set, the frame), main.c (the command line). Everything
 * the frame path touches is sized and allocated at load.
 */
#ifndef YSP_PLAYER_H_INCLUDED
#define YSP_PLAYER_H_INCLUDED

/* yrsp_feed_screen() and yrsp_sdl_source() exist only when SDL's event
 * header comes before ysp/input.h and ysp/response.h */
#if !defined(YSCR_NO_SDL)
#include <SDL3/SDL_events.h>
#endif
#include "ysp/experiment.h"
#include "ysp/gfx.h"
#include "ysp/timeline.h"
#include "ysp/trials.h"
#include "ysp/stair.h"
#include "ysp/quest.h"
#include "ysp/aep.h"
#include "ysp/audio.h"
#include "ysp/response.h"
#include "ysp/rdk.h"
#include "ysp/rigfile.h"
#include "ysp/dataset.h"
#include "ysp/pack.h"
#include "ysp/sig.h"
/* again after ysp/device.h (through ysp/rigfile.h): the response rule's
 * helpers (ysp/response.h, RESPONSE TRIGGERS) */
#include "ysp/response.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_VERSION_STRING "0.16.0"
/* The newest format version this player lowers (2: format part 3, in
 * v2.c). session.json's format_version is the definition's. */
#define PL_FORMAT_VERSION 2

enum {
    PL_BASE_TRIAL     = 1,   /* the timeline base a trial runs on            */
    PL_FIRED_CAP      = 512,
    PL_MAX_PATH       = 2048,
    PL_ROW_TEXT       = 65536,
    PL_PENDING_ROWS   = 16,  /* trials can end faster than flips complete    */
    PL_MAX_FIELDS     = 64,
    PL_FRAME_RING     = 256, /* frame number to trial index, until its record */
    PL_REC_DELAY_CAP  = 64,  /* tests: flip records held back             */
    PL_MAX_DEPTH_STACK = 34,
    PL_MAX_TEXTURES   = 512,
    PL_EVENT_QUEUE    = 4096,
    PL_RDK_STEPS      = 64,    /* an RDK's step buffer (about 1 s at 60 Hz); a full
                              * one goes to its stream mid-trial, so a
                              * trial has no length limit                    */
    PL_SEED_LOG       = 65536, /* leaf runs whose seeds session.json lists;
                              * later ones are counted, and every seed
                              * re-derives from the session seed (D8)        */
    PL_MAX_CONTAINER_VISITS = 10000000, /* container starts a tree can make:
                              * each costs a walk step, 4096 a frame, so
                              * this is at most about 41 s of blank frames  */
    PL_MAX_TEXT       = 4096,  /* bytes of one value's text: ysp/table.h's
                              * longest field, so the files read back         */
    PL_WALK_PER_FRAME = 4096,  /* tree-walk steps before the frame goes on    */
    PL_OUT_PENDING    = 64,    /* a channel's codes between the fire and its
                              * device record (a real run)                  */
    PL_RULE_ARMS      = 8,     /* a channel's last rule arms: a rule's record
                              * goes to the arm its press falls in          */
    PL_DEV_RING       = 4096,  /* device records between two frames          */
    PL_DEV_DRAIN      = 256,   /* records read per drain call                */

    PL_WRITE_QUEUE    = 8192   /* writes the writer may lag behind: about a
                              * minute of frames and their events at 60 Hz  */
};
/* A row's rsp_flags bit, the player's own, in the bits ysp/response.h
 * leaves to its caller (15 to 31; YRSP_R_* keep 0 to 14): `after` was
 * built from a live result that named another response than the row's
 * final one. */
#define PL_RSP_AFTER_LIVE 0x8000u
/* An out row's flags bit, the player's own (ysp/device.h's YDEV_OUT_* end at
 * 0x0200): the record came after its trial's row was written, so that row
 * counted the code failed. */
#define PL_OUT_AFTER_ROW 0x4000u
enum {
    PL_WAIT_FINAL_NS = 1000000000, /* the safety net of a response's wait for
                                    * the landing frame's flip record: every
                                    * record comes 1 to 2 frames after its
                                    * vblank on GLX_OML, so 1 s is 60 frames
                                    * past any record the screen delivers; it
                                    * fires only when records stop (docs/
                                    * player.md) */
    /* Spans of time, counted in frames at the display's rate
     * (pl_frames_of()); each is its 0.10.0 frame count at 60 Hz, so a 60 Hz
     * run is unchanged */
    PL_OUT_WAIT_NS = 500000000,     /* a row waits this long after its last
                                     * frame for its codes' device records,
                                     * then is written with the missing ones
                                     * as failed (30 frames at 60 Hz)        */
    PL_LEAD_IN_NS = 500000000,      /* a real run's blank frames before the
                                     * data set opens (30 at 60 Hz)          */
    PL_END_DRAIN_NS = 133333336,    /* the session's last flips that bring in
                                     * the last records (8 at 60 Hz)          */
    PL_RECORD_LAG_NS = 33333334,    /* a flip record comes this long after its
                                     * flip at most (2 frames at 60 Hz on
                                     * GLX_OML, docs/player.md)               */
    PL_SOUND_LEAD_NS = 40000000,    /* a sound's yau_play_at() this long ahead
                                     * of its onset at least: the shortest safe
                                     * lead docs/audio.md measured at the
                                     * defaults (WASAPI shared queue 2, ALSA
                                     * exclusive 480-frame periods)          */
    PL_WORK_BIN_NS = 100,           /* --measure's histogram: 100 ns bins */
    PL_WORK_BINS = 200000,          /* up to 20 ms; more goes in the last */
    PL_WRITER_PERIOD_NS = 2000000   /* the writer looks at its queue this often:
                                     * a row is on disk 2 ms after it is final */
};
/* pl_val.flags beyond the loader's: a trial seed (text is "x" and 16 hex digits,
 * the spelling of the trial_seed column); a session field that was not given
 * (text is empty, a number reads as 0). */
#define PL_VF_SEED   0x40u
#define PL_VF_ABSENT 0x80u
#define PL_MARK_END ((int32_t)0x7fffffff)  /* the internal end of a trial; not
                                            * a mark index, so no definition
                                            * name can collide with it */
/* Version 2's own marks (v2.c), above every mark index too: a flag step
 * (+ flag x 2 + value) and a recenter step (+ its index in the template's
 * list) act when they land. */
#define PL_MARK_FLAG     ((int32_t)0x7f000000)
#define PL_MARK_RECENTER ((int32_t)0x7e000000)

typedef struct pl_v2 pl_v2;          /* a template's version 2 runtime (v2.c) */
typedef struct pl_v2_row pl_v2_row;  /* a row's version 2 cells (v2.c)        */

typedef struct pl_player pl_player;

/* --- values (2.2) ----------------------------------------------------- */

typedef struct pl_val {
    uint8_t  kind;      /* yexp_vkind: NONE, BOOL, NUMBER, STRING or ARRAY   */
    uint8_t  flags;     /* YEXP_VF_INT: i is exact; YEXP_VF_DURATION: i is ns */
    uint32_t n;         /* ARRAY: items                                      */
    double   num;
    int64_t  i;
    const char* str;
    const yexp_value* items;
} pl_val;

/* The variable environment of one evaluation. */
typedef struct pl_env {
    const yexp_node*     node;     /* the node in scope (a leaf during a trial) */
    const yexp_template* t;        /* the template during a trial; else NULL */
    const ytr_trials*    trials;   /* open during a trial; else NULL         */
    const ytr_trial_info* ti;      /* the current trial's; else NULL         */
    int      phase;                /* PL_PH_*: which built-ins are known     */
    const char* method;            /* during a track trial; else NULL        */
    double   level, estimate, contrast_shown;
    bool     have_shown;           /* a michelson stimulus is in the trial   */
    const char* response;          /* the response's choice key, or NULL     */
    int64_t  rt_ns;                /* YRSP_NO_TIME: none                     */
    bool     responded, correct, has_correct;
    const char* outcome;           /* "correct", "wrong", "timeout", or NULL */
    bool     key_answer_known;
    int      extreme;              /* the check at load: 0 off, 1 low, 2 high */
} pl_env;

enum { PL_PH_SESSION = 0, PL_PH_TRIAL = 1, PL_PH_AFTER = 2, PL_PH_ROW = 3 };

int  pl_eval(pl_player* p, const pl_env* env, const yexp_value* v, pl_val* out);
int  pl_eval_ns(pl_player* p, const pl_env* env, const yexp_value* v, int64_t* ns);
int  pl_eval_num(pl_player* p, const pl_env* env, const yexp_value* v, double* out);
int  pl_eval_int(pl_player* p, const pl_env* env, const yexp_value* v, int64_t* out);
int  pl_eval_bool(pl_player* p, const pl_env* env, const yexp_value* v, bool* out);
/* The value's text, as a lookup key or a data cell: a bool is true or false,
 * a number its canonical text. */
int  pl_val_text(const pl_val* v, char* buf, size_t cap);
int  pl_var(pl_player* p, const pl_env* env, const char* name, pl_val* out);
int  pl_eval_text(pl_player* p, const pl_env* env, const yexp_value* v, char* buf, size_t cap);

/* --- channels and stimuli --------------------------------------------- */

/* A property a step drives. */
typedef struct pl_chan {
    const yexp_stim* stim;
    const char* prop;           /* NULL: the visible channel                 */
    int  stim_index;            /* into pl_player.stim, by the template pass  */
} pl_chan;

/* A key of a stimulus that the player writes straight into a float or a
 * small integer field of ygfx_stim. */
typedef struct pl_write {
    const yexp_prop* prop;
    uint8_t  kind;              /* PL_W_* */
    uint8_t  width;             /* bytes of an enum or int field             */
    uint16_t offset;
    const yexp_value* value;
    float*   target;            /* a float field outside ygfx_stim: NULL or  */
} pl_write;

enum { PL_W_FLOAT, PL_W_ENUM, PL_W_ANCHOR, PL_W_COLOR, PL_W_SPECIAL };

typedef struct pl_stim {
    const yexp_stim* def;
    int   ch_visible;           /* -1: no step shows it; channel index       */
    ygfx_stim gs;               /* what ygfx_draw() draws                    */
    pl_write* writes; int n_writes;   /* variable keys, applied at trial start */
    /* text */
    ygfx_stim* text_runs; int max_runs; int n_runs;  /* this trial's runs     */
    ypak_text_style style;
    const yexp_value* text_value;
    float text_box[2];
    float color[3];
    /* rdk */
    yrdk_field field;
    ygfx_buf buf;
    float* xy;
    int rdk_count;
    bool rdk_moving;
    int64_t rdk_t0;
    uint64_t rdk_seed;
    yrdk_step* steps; int n_steps; int stream; int max_steps;
    int n_flushed;              /* this trial's steps already in the stream  */
    /* michelson */
    const yexp_value* michelson;
    bool  text_after;           /* its text depends on the response         */
    /* sound */
    yau_buf sound;
    yau_id sound_id;
    int64_t sound_scheduled;   /* the RT time of the onset last played, -1  */
    int     sound_first;       /* -1 until sound_id holds the first onset's id */
    float last_color[3];
} pl_stim;

/* --- methods (2.8) ---------------------------------------------------- */

typedef struct pl_method {
    const yexp_method* def;
    yst_stair* stair;
    yqst_quest* quest;
    yaep_gp* aep;
    double (*next_level)(struct pl_method* m);
    int (*update)(struct pl_method* m, double value, int correct);
    double (*estimate)(struct pl_method* m);
    double (*done)(struct pl_method* m);
    bool log_domain;        /* method_value is floored before its log       */
    double aep_x[8];
} pl_method;

/* --- templates --------------------------------------------------------- */

typedef struct pl_marks {
    const char** names; int n, cap;
} pl_marks;

typedef struct pl_tmpl {
    const yexp_template* def;
    yrsp_collector* collector;  /* NULL: no response                         */
    int   n_choices;
    const char** choice_keys;
    uint32_t* choice_codes;     /* response.trigger's per-choice code        */
    int   from_ch;              /* the visible channel of `from`, or -1       */
    int   role;                 /* the response role, into pl_player.roles    */
    int   from_sound;
    ytl_op* ops;  int ops_cap;  /* big enough for events + after + the rest  */
    /* record list */
    int*  record_stims; int n_record;
    bool* stim_used;            /* the stimuli this template shows or changes */
    pl_v2* v2;                  /* format 2's states, graph and forms; NULL:
                                 * the version 1 path                        */
} pl_tmpl;

/* A role of the definition (2.9) and what it is bound to. In --sim no role
 * has a device: the participant and the sink stand in. */
typedef struct pl_role {
    const yexp_role* def;
    const yrig_role* rig;      /* the rig's binding, or NULL                   */
    bool        keyboard;      /* an input role that lists "keyboard" and that
                                * the rig binds to no device (a real run)     */
    int         trigger;       /* its trigger channel; -1 for an input role   */
    ydev_device* dev;          /* open in a real run when the rig binds it    */
    ydev_rule*  rule;          /* an input device's response rule (2.6)      */
    int         rule_out;      /* the rule's output: a trigger channel        */
    yin_source  source;        /* its events' tier and bounds                 */
    bool        below;         /* its tier is worse than its min_tier          */
    int         tier;          /* the rig's tier for it (pl_roles_tier())      */
} pl_role;

/* A code a trigger channel fired, until the device's record of the write
 * (a real run): records of one channel come in fire order. */
typedef struct pl_out_fire { int trial; uint32_t code; int64_t deadline; } pl_out_fire;
/* A rule's window opened (a real run): its records go to the trial whose
 * arm their press follows. */
typedef struct pl_rule_arm { int trial; int64_t from; } pl_rule_arm;

/* --- the tree ---------------------------------------------------------- */

typedef struct pl_table_ref {
    const char* name;
    ytb_table*  table;
    void*       arena;
} pl_table_ref;

/* A leaf's prepared desc. */
typedef struct pl_leaf {
    const yexp_node* node;
    pl_tmpl*  tmpl;
    ytr_desc  desc;              /* from ytr_rules(), seed and rng set per run */
    const ytb_table* table;
    void*     arena;
    size_t    arena_size;
    int       n_tracks;
    pl_method* tracks[YTR_MAX_TRACKS];
    int       n_vars;
    const yexp_rule_var* rule_vars;
    const char* var_names[YTR_MAX_VARS];
} pl_leaf;

typedef struct pl_stack {
    const yexp_node* node;
    const yexp_node* children;   /* the array being walked                  */
    int n_children, child;
    int rep, n_reps;
    int row, n_rows;             /* a container's rows; 1 with no table     */
    const ytb_table* table;
    int iteration;               /* of this node within its parent's run    */
    int child_iters[256];        /* starts so far of each child, for D8     */
    int loop_pass;
    int n_correct, n_counted;    /* loop's accuracy over this iteration      */
    bool active;                 /* skipped by if/unless                     */
    bool opened;
} pl_stack;

/* --- data rows ---------------------------------------------------------- */

enum { PL_COL_FIXED, PL_COL_SET };

typedef struct pl_col {
    char     name[YTB_MAX_NAME + 1];
    int      type;       /* ytr_col_type                                     */
    int      idx;        /* in the writer                                    */
} pl_col;

/* One recorded stimulus over a trial (D10): first onset, last offset, and
 * frames, tier, drops and flags over every frame it was shown. */
typedef struct pl_stim_stats {
    int     stim;
    int64_t onset_frame, offset_frame;
    int64_t onset_ns, offset_ns, residual_ns;
    int     frames;
    int     tier;       /* worst */
    int     dropped;
    int     flags;
} pl_stim_stats;

typedef struct pl_out_stats {
    int      n, failed;
    int64_t  lag_max;
    int      owed;      /* a real run: codes asked for whose device record
                         * has not come; a row waits for them (a bound) */
} pl_out_stats;

typedef struct pl_row {
    bool    used, final;
    bool    last_in;         /* its last frame's flip record has come     */
    int     trial_index;
    int64_t first_frame, last_frame;
    int     tier_worst, frames_dropped;
    int     flip_flags;
    pl_stim_stats* stats;     /* one per recorded stimulus of the template */
    int     n_stats;
    pl_out_stats* out;
    bool    ended, write_row;
    int64_t elapsed_ns;
    /* the row's values; written when its frames' records are in */
    struct { int col; int type; int64_t i; double d; const char* s; } cells[YTR_DATA_MAX_COLS];
    int     n_cells;
    char*   text;            /* row_text_cap bytes, sized at load           */
    size_t  text_len;
    yrsp_result res;
    bool    has_res;
    pl_v2_row* v2;           /* version 2's cells known at the flush        */
    int     queued;          /* handed to the writer and not yet written:
                              * the slot is not free (read and written
                              * atomically: the writer thread clears it) */
} pl_row;

/* A write for the data set's writer (pl_wq_*, run.c): a real run's frame
 * thread never calls write() itself, because on ext4 one append blocked a
 * frame for 18 ms while the kernel wrote the file back (docs/player.md). */
enum { PL_WJ_FRAME = 1, PL_WJ_EVENT, PL_WJ_STREAM, PL_WJ_ROW, PL_WJ_ROWBUF };
/* A version 2 trace row (v2.c) is too wide for the job: it waits in the
 * queue's side buffer (pl_player.wq_rows), in its job's slot, so the job
 * stays 160 bytes and the side buffer exists only for a session with traces. */
enum { PL_WQ_ROW_BYTES = 296 };
typedef struct pl_rdk_step_row { int trial_index, update; int64_t t_ns; double coherence, direction, speed; } pl_rdk_step_row;
typedef struct pl_wjob {
    int type;
    int id;                  /* STREAM: the stream; ROW: the row's slot   */
    union { yds_frame frame; yds_event event; pl_rdk_step_row step; } u;
} pl_wjob;

/* --- arguments ---------------------------------------------------------- */

typedef struct pl_field_arg { const char* name, *value; } pl_field_arg;

typedef struct pl_args {
    const char* pack_path;
    const char* out_dir;
    bool  sim;
    bool  have_seed;
    uint64_t seed;
    const char* rig_path;
    const char* cal_path;
    int   n_fields;
    pl_field_arg fields[PL_MAX_FIELDS];
    const char* participant;   /* "correct" (default), "silent", "p=0.8" or
                                * "script:FILE"                               */
    double rt_s;               /* the participant's RT; default 0.3 s        */
    int   max_trials;          /* a budget for the fuzz target; 0: none      */
    int64_t max_seconds;       /* virtual seconds before giving up; default 2 days */
    bool  quiet;
    bool  measure;             /* real-clock cost of each frame's work      */
    bool  windowed;            /* a real run in a window (development)      */
    const char* allow_below;   /* ROLE[,ROLE...]: these roles start below
                                * their min_tier; NULL: none                */
    int64_t record_late_ns;    /* tests (--sim): every flip record's onset this much late */
    int record_delay;          /* tests (--sim): records handed on this many frames late */
    int live_cap;              /* tests (a real run): raw mouse samples one poll holds; 0 the default */
    int64_t plan_early_ns;     /* tests: a response's plan onset this much
                                * before the timeline's (--sim)             */
    int   lose_code;           /* tests: the screen never writes the Nth code
                                * at a flip (1 = the first); 0: none        */
    int   hang_trial;          /* tests: the frame loop stops in session
                                * trial hang_trial - 1 (the panic watchdog's
                                * case); 0: never                          */
    const char* trace_path;    /* a test's view of every channel per frame  */
    int   rdk_buffer;          /* tests: an RDK's step buffer, to make it
                                * flush often; 0 is PL_RDK_STEPS             */
    const char* def_text;      /* the fuzz target: use these bytes for the
                                * experiment entry, keeping the pack's tables */
    size_t def_len;
    /* version 2, --sim: the scripted participant of signal roles */
    const char* samples_path;  /* t_s,role,x,y: device counts per sample    */
    int   n_gains;
    pl_field_arg gains[16];    /* --sim-gain ROLE=UNITS: the --sim adapter's
                                * scale per count (G18 has no rig block)     */
    const char* draws_path;    /* tests: every leaf's trials and variables,
                                * written without running (--test-draws)    */
    bool  profile;             /* --profile: the trace ring's zones to
                                * DIR/profile.csv (a YSP_TRACE_RING build)  */
    int64_t sim_period_ns;     /* --sim-hz: the simulated display's period;
                                * 0 is screen.h's default (60 Hz)           */
    int   n_lates;
    pl_field_arg lates[16];    /* tests (--sim): --test-sample-late ROLE=S,
                                * a role's samples reach the player S
                                * seconds after their time                  */
    int   n_bounds;
    pl_field_arg bounds[16];   /* tests (--sim): --test-sample-bound ROLE=S,
                                * the lateness bound the player waits for
                                * (default: the role's --test-sample-late),
                                * so a report can come later than its bound */
    bool  feed_each;           /* tests (--sim): --test-feed-each, each sample
                                * in its own ysig_feed() and its hits taken
                                * before the next (3.11 rule 4's reading), not
                                * a poll's in one ysig_feed_n()             */
} pl_args;

typedef struct pl_script_row { int trial; char* resp; bool silent; double rt_s, hold_s; } pl_script_row;

/* --- the stepped session (sim.c, 0.16.0) ---------------------------------- */

/* The stepped session draws on a GL context through ysp/gfx.h 0.23.0's
 * deferred open and output texture (docs/designer.md, "The GL context",
 * prerequisite P3). Before 0.23.0 it draws on the null backend only. */
#if YGFX_VERSION_MAJOR > 0 || YGFX_VERSION_MINOR >= 23
#define PL_GFX_DEFER 1
#else
#define PL_GFX_DEFER 0
#endif

enum { PL_SIM_EVENTS = 0, PL_SIM_RESPONSE, PL_SIM_AFTER, PL_SIM_BETWEEN };
enum {
    PL_SIM_INPUT_CAP = 256,    /* keys and choices between two frames       */
    PL_SIM_LIVE_CAP  = 8192,   /* samples between two frames, as a real run's
                                * poll holds                              */
    PL_SIM_TEXT      = 128     /* the last trial's response and outcome, as
                                * pl_sim_where_now() gives them (cut)      */
};

typedef struct pl_sim_where {
    int         trial;          /* the session trial; -1 before the first  */
    const char* leaf;           /* the leaf node's path, or NULL            */
    int         condition;      /* ytr_trial_info.condition: the table row,
                                 * -1 for a track trial                     */
    const char* tmpl;           /* the template, or NULL between trials     */
    const char* state;          /* version 2: the current state; NULL: the
                                 * end, or an events template               */
    int         phase;          /* PL_SIM_EVENTS, _RESPONSE, _AFTER, _BETWEEN */
    int64_t     frame;          /* the next frame's index                   */
    int64_t     t_ns;           /* its onset from the session's first frame */
    const char* outcome;        /* the last trial's, or NULL                */
    const char* response; int64_t rt_ns;   /* the last trial's; NULL and
                                 * YRSP_NO_TIME with none                  */
} pl_sim_where;

typedef struct pl_sim_desc {
    ypak_desc   pack;           /* in memory; the caller keeps it until pl_close() */
    const char* def_text; size_t def_len;   /* NULL: the pack's experiment */
    uint64_t    seed;
    const yrig_profile* rig;    /* NULL: the built-in simulated rig          */
    const ycol_cal* cal;        /* the display's calibration, or NULL: the
                                 * nominal one for a definition that requires
                                 * one (a need), identity for any other      */
    int         n_fields; pl_field_arg fields[PL_MAX_FIELDS];
    const char* out_dir;        /* the data set's folder; must not exist    */
    const char* participant;    /* the scripted participant of the trials
                                 * that live() leaves to it: "correct",
                                 * "p=0.8", "silent"; NULL: "correct"       */
    double      rt_s;           /* its rt; 0: 0.3 s                          */
    int32_t     width, height;  /* the display in px; 0: 1920 x 1080         */
    int64_t     period_ns;      /* the frame period; 0: 60 Hz                */
    /* the gfx (ysp/gfx.h 0.23.0, P3): recorded at the load, made at
     * pl_sim_attach() on the context that gl_proc reaches, with the caps
     * of a gfx that the GL thread opened. gl_caps NULL: the null backend */
    const struct ygfx_gl_caps* gl_caps;
    const ygfx_cache* cache;    /* compiled programs, kept across sessions  */
    yscr_proc (*gl_proc)(void* ctx, const char* name);
    void*       gl_ctx;
    /* hooks, each optional, on the thread that steps the session */
    bool (*live)(void* ctx, const pl_sim_where* w);   /* at a trial's start:
                                 * true, the caller answers this trial;
                                 * false, the scripted participant does    */
    void (*on_code)(void* ctx, const char* role, uint32_t code, int64_t t_ns);
    void*       ctx;
} pl_sim_desc;

/* The stepped session's state in pl_player (sim.c). */
typedef struct pl_sim {
    bool        on;             /* pl_sim_load() made this player            */
    bool        draw;           /* this frame draws (pl_sim_frame())         */
    bool        live_trial;     /* live() took the running trial             */
    bool        attached, warmed, stopped;
    int         stage;          /* 0 loaded, 1 frames, 2 the end's frames, 3 over */
    int         drain_i;        /* the end's frames so far                   */
    int32_t     width, height;
    int64_t     period;
    int64_t     t0;             /* frame 0's onset                          */
    int64_t     frame;          /* the next frame's index                    */
    yscr_record rec;            /* the last frame's record, given with the next */
    bool        has_rec;
    yrsp_input  in[PL_SIM_INPUT_CAP];   /* keys and choices for the next frame */
    int         n_in;
    char        last_outcome[PL_SIM_TEXT], last_response[PL_SIM_TEXT];
    bool        last_has_outcome, last_has_response;
    int64_t     last_rt;
    const struct ygfx_gl_caps* gl_caps;
    const ygfx_cache* cache;
    yscr_proc (*gl_proc)(void* ctx, const char* name);
    void*       gl_ctx;
    const ycol_cal* cal;
    const yrig_profile* rig;
    bool (*live)(void* ctx, const pl_sim_where* w);
    void (*on_code)(void* ctx, const char* role, uint32_t code, int64_t t_ns);
    void*       ctx;
    /* sim.c's live() call, through a pointer: run.c then links without
     * sim.c, as the mutation tests compile only the sources they list */
    bool (*ask)(pl_player* p, int trial);
    void (*end)(pl_player* p);   /* pl_close()'s: the end of a session that
                                  * did not reach it, and session.json    */
} pl_sim;

typedef struct pl_node_seed_rec { const char* path; int iteration; uint64_t seed; } pl_node_seed_rec;

typedef struct pl_csrc pl_csrc;

/* --- the check (lower.c): the load without side effects ------------------- */

/* pl_check()'s answer: the definition loaded and lowered, the loader refused
 * it (tier 1), or the player refused it after the loader (tier 2). */
enum { PL_CHECK_OK = 0, PL_CHECK_DEFINITION = 1, PL_CHECK_REFUSED = 2 };
/* The object a refusal names, for a link to it. */
enum { PL_OBJ_NONE = 0, PL_OBJ_NODE, PL_OBJ_STIMULUS, PL_OBJ_TEMPLATE, PL_OBJ_METHOD, PL_OBJ_SIGNAL_ROLE, PL_OBJ_FIELD };
/* What a real run asks of the rig or the session (tier 3). The check goes
 * on with a stand-in for each (docs/player.md, "Reference: pl_check()"). */
enum {
    PL_NEED_FIELD = 1,      /* a required session field: --field NAME=VALUE  */
    PL_NEED_CALIBRATION,    /* display.calibration "required"                */
    PL_NEED_VIEW,           /* display.units deg (value 1) or cm (2): a view */
    PL_NEED_DISPLAY_TIER,   /* display.min_tier (value)                      */
    PL_NEED_DEVICE,         /* a required role that the keyboard cannot take:
                             * a device in the rig profile (value: direction) */
    PL_NEED_ROLE_TIER,      /* a role's min_tier (value)                     */
    PL_NEED_SIGNAL,         /* a signal role: its adapter in the rig profile
                             * (value 1: required)                          */
    PL_NEED_SIM_GAIN        /* a relative signal role: --sim needs --sim-gain
                             * or the rig's signal                          */
};

typedef struct pl_need {
    int         kind;       /* PL_NEED_*                                     */
    const char* name;       /* the field or role; NULL for the display       */
    int         value;      /* by kind, above; FIELD: 1 text, 0 integer       */
} pl_need;

typedef struct pl_check_desc {
    ypak_desc   pack;       /* data and size, or a reader; a path is mapped
                             * read-only                                     */
    const char* def_text;   /* the definition; NULL: the pack's experiment   */
    size_t      def_len;
    uint64_t    seed;       /* the session seed (D8)                          */
    const ycol_cal* cal;    /* the stand-in for a required calibration; NULL:
                             * pl_check_nominal_cal()'s, made on each call   */
} pl_check_desc;

/* Every pointer stays valid until pl_close(). */
typedef struct pl_check_result {
    int                status;      /* PL_CHECK_*                            */
    const yexp_report* report;      /* tier 1: yexp_load()'s diagnostics,
                                     * errors and warnings; NULL when the
                                     * loader did not run                   */
    const char*        message;     /* tier 2: the refusal; "" with none     */
    int                object;      /* PL_OBJ_*: what the refusal names      */
    const char*        object_name; /* a node's path, or the object's name   */
    const pl_need*     needs;       /* tier 3, in the order the load met them */
    int                n_needs;
} pl_check_result;

/* --- the player --------------------------------------------------------- */

struct pl_player {
    pl_args   args;
    uint64_t  seed;
    uint64_t  participant_rng;
    char      error[1024];
    int       failed;
    bool      bad_arg;   /* the failure is a bad argument: exit 2 (main.c) */
    void*     blocks;
    int       c_tables;
    bool      channels_assigned;
    float**   chan_src;
    ytr_desc  extreme_desc;
    int64_t   participant;
    ygfx_tex  tex[PL_MAX_TEXTURES];
    const char* tex_names[PL_MAX_TEXTURES];
    int       n_tex;
    const yexp_node** def_leaves;
    ypak_runs runs;
    bool      runs_open;
    ygfx_cset* csets;
    int*      col_family;
    pl_csrc*  csrc;
    pl_node_seed_rec* node_seeds;
    int       n_node_seeds;
    int       leaf_iter, leaf_loop_pass, leaf_n_correct, leaf_n_counted, iter_counter;
    int       leaf_pass_trials;  /* trials this loop pass of the leaf gave    */
    int       leaf_requeues;     /* requeues since the leaf opened           */
    char*     key_answer;      /* max_text + 1 bytes, sized at load          */
    size_t    max_text;        /* the longest value text the session can make */
    char*     text_buf;        /* max_text + 1: a cell, a lookup's key        */
    char*     runs_key;        /* a glyph-run block's key                     */
    size_t    row_text_cap;    /* bytes of a row's text arena                 */
    long long n_seeds_unlogged; /* leaf runs past PL_SEED_LOG               */
    pl_stim_stats* stat_pool[PL_PENDING_ROWS];
    pl_out_stats*  out_pool[PL_PENDING_ROWS];
    pl_stim_stats* stats_now;
    pl_out_stats*  out_now;
    int       pool_stats;
    uint8_t*  frame_vis;      /* PL_FRAME_RING x pool_stats: was record k shown on that frame */
    float*    chan_base;      /* a literal key's value: every trial starts from it */
    bool*     chan_dyn;       /* the key holds a variable: read it per trial      */
    int64_t   ops_latest;     /* the latest time pl_build_ops() scheduled         */
    int64_t   ops_origin;     /* what at_s counts from: a version 2 state's entry
                               * (3.11 rule 6), else the trial's start (0)      */
    int       walk_steps, walk_frame_steps;
    void*     trace_file;
    int       now_tier, now_dropped, now_flags;
    bool      pending_stop;
    bool      release_due;
    yrsp_collector* release_to;
    yrsp_input release_ev;
    int       n_leaves_tmp;

    /* the pack and the definition */
    ypak_pack pack;
    bool      pack_open;
    yjs_arena arena;
    bool      arena_open;
    yexp_def* def;
    yexp_report report;
    const char* canon; size_t canon_n;
    char      experiment_id[65], stored_id[65];
    char      pack_id[72];
    int       n_field_values;
    pl_val*   field_values;

    /* the rig and the color path */
    yrig_profile* rig;
    bool      have_rig;
    char      rig_hash[65];
    char      rig_path[PL_MAX_PATH];   /* the profile's file ("" without one) */
    ycol_cal  cal;
    bool      have_cal;
    ycol_ctx  cx;
    ycol_shown gray_sh;

    /* the screen */
    yscr_screen scr;
    bool        scr_open;
    ygfx_gfx    gfx;
    bool        gfx_open;
    yscr_caps   caps;
    yau_audio   audio;
    bool        audio_open;
    yrt_ring    ring;   /* device records (a real run) */
    unsigned char* ring_mem;
    bool        ring_open;
    yrt_event*  dev_recs;          /* PL_DEV_DRAIN, for the drain        */
    ygfx_file_cache pcache;        /* compiled programs (a real run)     */
    ygfx_view   view;              /* deg: from the mode the screen got  */

    /* roles (2.9), in the definition's order */
    pl_role*    roles;
    ydev_roles  dev_roles;         /* ysp/device.h's role table          */
    pl_out_fire* out_fires;        /* 2 x PL_OUT_PENDING per trigger channel:
                                    * the flips' codes, then the frame
                                    * loop's (a response's on a keyboard role) */
    int*        out_head;          /* per queue: the next record's fire   */
    int*        out_tail;
    pl_rule_arm rule_arms[YSCR_MAX_TRIGGERS][PL_RULE_ARMS];   /* per output channel */
    int         n_rule_arms[YSCR_MAX_TRIGGERS];
    int         n_flip_codes;      /* codes at a flip so far (--test-lose-code) */
    yrsp_collector* collectors;    /* one per template with a response, in
                                    * one array: every one hears every input */
    int         n_collectors;

    /* the abort (ysp/screen.h, ABORT): the operator stopped the session */
    bool        aborted;
    uint32_t    abort_reasons;
    int         abort_presses;
    int64_t     abort_ns;
    int         abort_trial;       /* the trial that was running, or -1  */
    char        panic_path[PL_MAX_PATH];   /* DIR/panic.json             */

    /* the data set's writer (a real run; --sim writes in place) */
    yrt_worker  writer;
    bool        writer_open;
    pl_wjob*    wq;                /* PL_WRITE_QUEUE                     */
    unsigned char* wq_rows;        /* PL_WRITE_QUEUE x PL_WQ_ROW_BYTES, or NULL */
    int         wq_head, wq_tail;  /* the writer's, the frame thread's;
                                    * read across threads atomically     */
    int         wq_stop;           /* the writer's job runs once more    */
    int         wfail;             /* the writer failed: werror says why */
    char        werror[512];

    /* channels: one per stimulus visibility and per driven property */
    ytl_timeline tl;
    ytl_desc   tl_desc;
    float*     tl_initial;
    ytl_event* tl_events;
    ytl_key*   tl_keys;
    int        n_channels;
    pl_chan*   channels;
    ygfx_bind* binds; int n_binds;
    ytl_event* peek;
    ytl_event* fired;
    int        fired_cap;
    pl_marks   marks;

    /* stimuli (sorted by name, as in the definition) */
    pl_stim*  stims; int n_stims;
    int*      draw_order;

    /* trigger roles, in the order of the role names (2.9) */
    int       n_triggers;
    const yexp_role** trigger_roles;
    volatile int trigger_fired;  /* the worker's count; a check at the end  */

    pl_method* methods; int n_methods;
    pl_tmpl*   tmpls; int n_tmpls;
    pl_table_ref* tables; int n_tables;
    pl_leaf*   leaves; int n_leaves;

    /* the tree walk */
    pl_stack  stack[PL_MAX_DEPTH_STACK];
    int       sp;
    bool      walk_done;
    ytr_trials trials;
    bool      trials_open;
    pl_leaf*  leaf;
    uint64_t  leaf_rng;

    /* the data set */
    yds_dataset ds;
    bool        ds_open;
    pl_col*   cols; int n_cols;
    ytr_col*  ytr_cols;
    pl_row    rows[PL_PENDING_ROWS];
    int       row_head, row_tail;
    int       frame_trial[PL_FRAME_RING];
    int       last_outcome_val; /* tests: the last trial's ytr_update() value */
    yscr_record rec_q[PL_REC_DELAY_CAP];   /* tests: held records (--test-record-*) */
    int       rec_head, rec_n;
    yds_event* events; int n_events;
    int64_t   t0_session;   /* the first frame's onset, for time_elapsed_s */

    /* the trial */
    pl_env    env;
    pl_tmpl*  tmpl;
    ytr_trial_info ti;
    ytl_seq   q;
    int       global_trial;     /* over the whole session; joins the tables */
    bool      in_trial, responding, after_built;
    int64_t   events_end;       /* base time after the events table         */
    int64_t   trial_first_frame;
    int       cur_row;          /* index into rows[]                        */
    bool      from_landed;
    int64_t   from_frame;
    int64_t   trial_start_onset;
    pl_method* track;
    yrsp_result live_res;       /* the result `after` was built from        */
    bool      have_live;
    /* the participant */
    bool      press_planned;
    bool      press_pending;
    int64_t   press_at, release_at;
    int       press_choice;
    double    press_rt;
    double    press_hold;
    bool      silent;
    pl_script_row* script; int n_script;
    int       script_k;       /* the script row of the planned press, -1 none */
    int64_t   press_onset;    /* the onset the trial's presses count from     */
    int       trials_done;
    /* measured frame work: real ns, no clock read on the frame path when
     * measure is false */
    bool      measure;
    int64_t   work_max_ns, work_trial_max_ns, work_n, work_over_16ms;
    int64_t   work_max_frame;
    int64_t   work_over_period;   /* frames whose work took more than a period */
    int       out_wait_frames;    /* PL_OUT_WAIT_NS, PL_END_DRAIN_NS and   */
    int       end_frames;         /* PL_RECORD_LAG_NS in frames of the     */
    int       rec_lag_frames;     /* display's period (pl_frames_of())     */
    uint32_t* work_hist;          /* per-frame work in PL_WORK_BIN_NS bins     */
    int64_t   work_input_max_ns, work_input_max_frame;   /* a real run's input: SDL's pump */
    /* --sim writes in place on the frame thread, where a real run queues
     * the write to its writer thread: under --measure that time is kept
     * out of the frame work (an ext4 write-back held one for 17 ms) */
    int64_t   work_wr_ns;                                /* in-place writes so far */
    int64_t   work_wr_max_ns, work_wr_max_frame;         /* the most in one frame */
    /* version 2 (v2.c) */
    struct pl_v2_session* v2s;
    /* --profile */
    yrt_ring  profile_ring;
    void*     profile_mem;
    yrt_event* profile_buf;
    void*     profile_file;
    /* pl_check() and the stepped session */
    bool      stand_in;       /* pl_check() and pl_sim_load(): no rig file
                               * and no devices, an audio device that never
                               * runs, the tier 3 stand-ins (r->needs)     */
    bool      checking;       /* pl_check() only: the null gfx, no warm-up
                               * draw, no data set, the device-button
                               * check; no stderr                           */
    bool      pack_reader;    /* entries come whole through a cursor       */
    const ycol_cal* check_cal; /* pl_check_desc.cal                        */
    pl_need*  needs; int n_needs, c_needs;
    int       at_obj;         /* the object the load is at (PL_OBJ_*)      */
    const char* at_name;
    int       fail_obj;       /* at_obj and at_name at the first pl_fail() */
    const char* fail_name;
    const char** check_streams; /* the streams named so far, for          */
    int       n_check_streams;    /* yds_stream_check()                     */
    ytr_data* check_scratch;  /* the column checks' writer, which discards */
    /* each load's own, so two loads on two threads share nothing (statics
     * through 0.15.0) */
    size_t    row_cap;        /* a row's text arena (pl_run_prepare())       */
    ytr_trials*   rules_scratch;   /* h_rules()'s dry open (host.c), 253 KB  */
    ytl_timeline* ops_scratch;     /* h_check_ops()'s timeline (host.c)      */
    float     v2_sink;        /* version 2's selector and value channels'
                               * bind target: written, never read          */
    pl_sim    sim;            /* the stepped session (sim.c)                */
};

/* --- load (lower.c) ------------------------------------------------------ */

int  pl_load(pl_player* p, const pl_args* a);
/* pl_load()'s load on a p that its caller set up (sim.c): p->args, the
 * stand-ins and the stepped session's fields; no memset. */
int  pl_load_from(pl_player* p, const ypak_desc* pd);
/* The load of pl_load() as --sim does it, with no side effects outside
 * memory: no file is written or read but the pack (and its path), no rig
 * profile, device, window, GL, audio device, thread or clock change. Returns
 * r->status. Call pl_close() after reading r, before p's next pl_check(). */
int  pl_check(pl_player* p, const pl_check_desc* d, pl_check_result* r);
/* pl_check()'s stand-in calibration, player_make_rig's nominal one. Making
 * it is most of a check's time (docs/player.md, "Reference: pl_check()"):
 * a caller that checks often makes it once and gives it in d->cal. 0, or < 0. */
int  pl_check_nominal_cal(ycol_cal* out);
void pl_close(pl_player* p);
void pl_fail(pl_player* p, const char* fmt, ...);
/* The object a refusal from here on names (PL_OBJ_NONE: none). */
void pl_at(pl_player* p, int obj, const char* name);
/* The text programs' first draws and ygfx_prime() (lower.c): at the load,
 * or at a stepped session's first drawn frame. */
void pl_gfx_warm(pl_player* p);
/* A gfx on ysp/gfx.h's null backend, with no screen and no GL (impl.c). */
bool pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d);
void* pl_alloc(pl_player* p, size_t n);
/* A span of time in whole frames of the display's period, rounded up. */
int pl_frames_of(const pl_player* p, int64_t ns);
const ytb_table* pl_table_get(pl_player* p, const char* name);
pl_stim* pl_stim_by_name(pl_player* p, const char* name);
/* The channel of a stimulus's property, or of its visibility when prop is
 * NULL; -1 when no step drives it. */
int  pl_channel(pl_player* p, const char* stim, const char* prop);
int  pl_chan_add(pl_player* p, const char* stim, const char* prop);
void pl_chan_steps(pl_player* p, const yexp_step* st, int n);
int  pl_trigger_channel(pl_player* p, const char* role);
/* The role of the definition by name, or NULL. */
pl_role* pl_role_get(pl_player* p, const char* name);
/* The node's seed by D8 for its path and the iterations of the node's chain;
 * iters[k] is node k of the chain's iteration, k = 0 the outermost. */
uint64_t pl_node_seed(uint64_t session_seed, const char* path, const int* iters, int n);

/* Op tables (lower.c): the ops of steps[0..n), filled from env. Returns the
 * count written, < 0 on an error. cursor (the base time at the start) moves
 * to the end time of what was emitted. ops has room for pl_ops_bound(). */
int  pl_build_ops(pl_player* p, const yexp_step* steps, int n, ytl_op* ops, int cap, int64_t* cursor);
int  pl_ops_bound(const yexp_step* steps, int n);
int  pl_mark_index(pl_player* p, const char* name);

/* The host for yexp_load() (host.c). */
void pl_host_init(pl_player* p, yexp_host* h);

/* --- format version 2 (v2.c) --------------------------------------------- */

/* Load: the steps' channels (from pl_channels_assign()), then the graphs,
 * the forms' binds and the samples after the stimuli and the timeline. */
int  pl_v2_channels(pl_player* p);
int  pl_v2_load(pl_player* p);
void pl_v2_close(pl_player* p);
int  pl_v2_ops_bound(const yexp_template* t);
/* A leaf's run: its `uniform` draws (D29). */
int  pl_v2_leaf_open(pl_player* p, uint64_t leaf_seed);
/* A walk2d or uniform variable's value, or 0 when name is none of them. */
int  pl_v2_var(pl_player* p, const pl_env* e, const char* name, pl_val* out);
/* A template's flag by name: 1 and *out, or 0. */
int  pl_v2_flag(pl_player* p, const char* name, bool* out);
/* Version 2's row cells, before the frame path. */
int  pl_v2_rows(pl_player* p);
/* A template's runtime-state steps: the count (k -1), or state k's. */
int  pl_v2_rs_steps(pl_player* p, int ti, int k, const yexp_step** st, int* n);
/* The trial: its start, each frame's parts, its end and its row. */
int  pl_v2_trial_start(pl_player* p, const yscr_frame* f);
int  pl_v2_frame_input(pl_player* p, const yscr_frame* f);
int  pl_v2_frame_eval(pl_player* p, const yscr_frame* f);
int  pl_v2_on_mark(pl_player* p, const ytl_event* e);
int  pl_v2_trial_end(pl_player* p, const yscr_frame* f, int choice, int* outcome_val, bool* requeue);
int  pl_v2_row_fill(pl_player* p, pl_row* r);
int  pl_v2_column(pl_player* p, pl_row* r, int col, const yexp_value* f, pl_val* out, bool* deferred);
const char* pl_v2_rsp_role(pl_player* p, const yrsp_result* res);
int  pl_v2_trial_rows(pl_player* p);
int  pl_v2_barrier_wait(pl_player* p);
/* G18: a real run's signal roles from the rig profile (v2.c). polls: the
 * frame polls the screen once (pl_v2_poll_screen()) and feeds its inputs
 * to the collectors later (pl_v2_feed_keys()); bind_wait: 1 while a role
 * waits for a click, so no trial starts; sigrole_check: the host's
 * sigrole() */
bool pl_v2_polls(const pl_player* p);
#if !defined(YSCR_NO_SDL)
int  pl_v2_poll_screen(pl_player* p, const yscr_frame* f);
#endif
void pl_v2_feed_keys(pl_player* p);
int  pl_v2_bind_wait(pl_player* p);
int  pl_v2_sigrole_check(pl_player* p, const yexp_sigrole* r, char* msg, size_t cap);
int  pl_v2_response(pl_player* p, const yrsp_result* r, const yscr_frame* f);
void pl_v2_on_onset(pl_player* p, const ytl_event* e, int stim);
int  pl_v2_step_ops(pl_player* p, const yexp_step* s, ytl_op* ops, int c, int cap);
int  pl_v2_clear_ops(pl_player* p, const char* stim, const char* prop, ytl_op* ops, int c, int cap);
bool pl_v2_reads(pl_player* p, const yexp_template* t, const char* name);
int  pl_on_fired(pl_player* p, const yscr_frame* f, int n);
int  pl_v2_row_flush(pl_player* p, pl_row* r, ytr_data* w);
bool pl_v2_row_waits(const pl_row* r);
int  pl_row_cell_value(pl_player* p, pl_row* r, int col, const pl_val* x);
int  pl_v2_on_record(pl_player* p, const yscr_record* rec);
int  pl_v2_columns(pl_player* p);
/* --test-draws: every leaf's trials and variables, no frames. */
int  pl_v2_draws(pl_player* p);
const float* pl_v2_values(pl_player* p, const float* tl_values);
const ygfx_bind* pl_v2_binds(pl_player* p, int* n);
int  pl_v2_session_json(pl_player* p, yjs_arena* a, yjs_value* s);
int  pl_v2_session_end(pl_player* p, yjs_arena* a);
/* ysp/sig.h's arm counts summed over the session's graphs: caught up, past
 * off, far, full, hits superseded, retracted, rows stale, derived stale.
 * Returns the number of graphs. */
#define PL_ARM_COUNTS 19
int  pl_v2_arm_counts(pl_player* p, int64_t c[PL_ARM_COUNTS]);
int  pl_v2_late_frames(pl_player* p, int64_t* n);
int  pl_v2_exit_lag_max(pl_player* p, int64_t* ns);
int64_t pl_v2_collector_now(pl_player* p, int64_t onset);
bool pl_v2_any_graph(pl_player* p);
int  pl_v2_streams(pl_player* p);
/* The running template's version 2 runtime, or none (a version 1 template). */
void pl_v2_select(pl_player* p);
/* The stepped session (sim.c): the running trial's state, or NULL; a
 * sample for the next frame (the queue of a real run's raw mice). */
const char* pl_v2_state_now(const pl_player* p);
int  pl_v2_sample_push(pl_player* p, int64_t t, int role, float x, float y);

/* --- run (run.c) --------------------------------------------------------- */

int  pl_run(pl_player* p);
/* pl_run()'s parts, which a stepped session (sim.c) calls around the frames
 * it makes: the allocations; one frame (the stats' reset, pl_frame(), a
 * blank frame at the session's end); whether the end needs frame i, and its
 * work; the session's last writes and session.json. */
int  pl_run_prepare(pl_player* p);
int  pl_run_frame(pl_player* p, const yscr_frame* f);
bool pl_run_drain_more(const pl_player* p, int i);
int  pl_run_drain(pl_player* p, const yscr_frame* f, int i);
int  pl_run_finish(pl_player* p);
/* One frame: the frame loop is this call between yscr_begin() and
 * yscr_flip(), so the callback form of rig_spec 4.2.1 can take it over.
 * 0 to go on, 1 when the session is over, < 0 on an error. */
int  pl_frame(pl_player* p, const yscr_frame* f);
void pl_stim_apply(pl_player* p);
enum { PL_KEY_LOAD, PL_KEY_TRIAL, PL_KEY_AFTER };
bool pl_value_after(const yexp_value* v);
int  pl_apply_writes(pl_player* p, pl_stim* s, int pass);
int  pl_apply_prop(pl_player* p, pl_stim* s, const yexp_prop* pr);
int  pl_stim_set_text(pl_player* p, pl_stim* s);
int  pl_walk_next_leaf(pl_player* p);
int  pl_start_trial(pl_player* p, const yscr_frame* f);
int  pl_end_trial(pl_player* p, const yscr_frame* f);
int  pl_extreme_ns(pl_player* p, const yexp_value* v, int64_t* ns);
/* The longest a trial of t can run, over the leaves that run it at their
 * extremes; -1 in *ns when nothing bounds it (a response with no deadline). */
int  pl_eval_var_bool(pl_player* p, const pl_env* env, const char* name, bool* out);
int  pl_channels_assign(pl_player* p);
int  pl_columns_build(pl_player* p);
/* A stream of the data set: its id, or < 0 with pl_fail(). pl_check() runs
 * yds_stream()'s checks with no data set (yds_stream_check()). */
int  pl_stream(pl_player* p, const char* name, const ytr_col* cols, int n);
pl_leaf* pl_leaf_of(pl_player* p, const yexp_node* n);
bool pl_track_done(void* ctx);
void pl_row_write_oldest(pl_player* p);
int  pl_event_queue(pl_player* p, const yds_event* ev);
int  pl_events_flush(pl_player* p, bool all);
void pl_trigger_sink(void* ctx, const yscr_trigger_info* info);
/* A code queued on trigger channel ch (queue q: 0 a flip's, 1 the frame
 * loop's) until its device record comes (run.c; a test reaches it). */
void pl_out_push(pl_player* p, int ch, int q, int trial, int64_t deadline, uint32_t code);
/* The data set's writer thread (a real run). */
int  pl_writer_open(pl_player* p);
int  pl_wq_push_row(pl_player* p, int stream, const void* row, size_t n);
int  pl_after_build(pl_player* p, int64_t start);
int  pl_v2_events_response(pl_player* p, int64_t bt);
void pl_v2_record_times(pl_player* p, pl_row* r, const yscr_record* rec);
int  pl_v2_finish(pl_player* p);
int  pl_v2_frame_timed(pl_player* p, const yscr_frame* f);
int  pl_window_check(pl_player* p, const yexp_template* d, int64_t dur, int64_t minrt);
int  pl_writer_close(pl_player* p);
extern volatile int pl_loop_active;   /* the frame loop is running: a test counts allocations against it */

/* --- the stepped session (sim.c) ------------------------------------------
 * The --sim session, stepped by its caller (the designer's preview):
 * docs/player.md, "Reference: the stepped session". pl_sim_load() runs on
 * any thread and makes no GL call. pl_sim_attach(), pl_sim_frame() with
 * draw, pl_sim_texture() and pl_sim_detach() run on the thread whose GL
 * context is current. Every other call runs on the thread that owns the
 * session at the time; no two threads use one session at once. No call
 * changes the clock, elevates a thread or reads a file but the pack. */

/* The load and lower: pl_check()'s, with the rig, the display and the data
 * set (its writer thread starts here). r as pl_check()'s; 0 is
 * PL_CHECK_OK. Call pl_close() after any result. No GL call. */
int  pl_sim_load(pl_player* p, const pl_sim_desc* d, pl_check_result* r);
/* Makes the recorded GL objects on the current context, for at most
 * budget_ns (0: all). 1 when done, 0 when more is left, < 0 an error.
 * With the null backend, 1 at once. */
int  pl_sim_attach(pl_player* p, int64_t budget_ns);
/* One frame: its onset is the last one's plus the period. draw false: the
 * frame's logic and no GL call (a fast forward, before or after attach).
 * draw true: after attach only. 0 to go on, 1 the session is over (its
 * data set is complete), < 0 a refusal: p->error, and the object in
 * p->fail_obj and p->fail_name. Allocates nothing. */
int  pl_sim_frame(pl_player* p, bool draw);
/* Input at t_ns on the session's frame grid (pl_sim_next_onset()), after
 * the last frame's onset and at or before the next one's; it reaches the
 * session at the next pl_sim_frame(). A key goes to every collector as an
 * SDL scancode; a choice is the running template's choice by index, with
 * its own kind and control; a sample is a signal role's, by its index in
 * the definition, in the role's units (a relative role's: counts, at the
 * stand-in gain of 1). 0, or < 0 refused (a time out of range, a full
 * queue, no such choice or role). */
int  pl_sim_key(pl_player* p, int64_t t_ns, int scancode, bool down);
int  pl_sim_choice(pl_player* p, int64_t t_ns, int choice, bool down);
int  pl_sim_sample(pl_player* p, int64_t t_ns, int role, float x, float y);
/* The next frame's onset on the session's grid. */
int64_t pl_sim_next_onset(const pl_player* p);
void pl_sim_where_now(const pl_player* p, pl_sim_where* w);
/* The display's texture: its GL name (GL_RGBA8, rows bottom-up). false
 * before attach and on the null backend. */
bool pl_sim_texture(const pl_player* p, ygfx_native* out);
/* Stops a session before its end: the data set records it as stopped
 * ("aborted", reason "request"), and pl_sim_frame() then gives 1. */
void pl_sim_stop(pl_player* p);
/* Frees the GL objects and closes the gfx (the GL thread). After it,
 * pl_close() makes no GL call and may run on any thread. */
void pl_sim_detach(pl_player* p);

#ifdef __cplusplus
}
#endif
#endif
