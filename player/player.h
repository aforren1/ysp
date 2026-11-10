/* player/player.h - the ysp player's internal interface.
 *
 * The player loads a pack, checks its definition (ysp/experiment.h) against
 * the pack and the rig, lowers it onto the ysp headers and runs it. This
 * first version runs only on the simulated display (--sim). docs/player.md
 * describes what is built and the decisions lowering forced.
 *
 * The code is split so that one test can reach each part: eval.c (values),
 * host.c (the loader's checks that need the pack and the rig), lower.c (the
 * definition to desc structs and op tables), run.c (the tree, the trial
 * state, the data set, the frame), main.c (the command line). Everything
 * the frame path touches is sized and allocated at load.
 */
#ifndef YSP_PLAYER_H_INCLUDED
#define YSP_PLAYER_H_INCLUDED

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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_VERSION_STRING "0.1.0"

enum {
    PL_BASE_TRIAL     = 1,   /* the timeline base a trial runs on            */
    PL_FIRED_CAP      = 512,
    PL_MAX_PATH       = 2048,
    PL_ROW_TEXT       = 65536,
    PL_PENDING_ROWS   = 16,  /* trials can end faster than flips complete    */
    PL_MAX_FIELDS     = 64,
    PL_FRAME_RING     = 256, /* frame number to trial index, until its record */
    PL_MAX_DEPTH_STACK = 34,
    PL_MAX_TEXTURES   = 512,
    PL_EVENT_QUEUE    = 4096,
    PL_MAX_RDK_TRIAL_S = 7200, /* the longest trial an RDK's stream is sized
                              * for at load; a longer one is refused there   */
    PL_MAX_LEAF_RUNS  = 10000000, /* leaf runs a tree can make (skipped ones
                              * too), sized at load: 24 bytes each          */
    PL_MAX_CONTAINER_VISITS = 10000000, /* container starts a tree can make:
                              * each costs a walk step, 4096 a frame, so
                              * this is at most about 41 s of blank frames  */
    PL_MAX_TEXT       = 4096,  /* bytes of one value's text: ysp/table.h's
                              * longest field, so the files read back         */
    PL_WALK_PER_FRAME = 4096   /* tree-walk steps before the frame goes on    */
};
/* pl_val.flags beyond the loader's: a trial seed (text is "x" and 16 hex digits,
 * the spelling of the trial_seed column); a session field that was not given
 * (text is empty, a number reads as 0). */
#define PL_VF_SEED   0x40u
#define PL_VF_ABSENT 0x80u
#define PL_MARK_END ((int32_t)0x7fffffff)  /* the internal end of a trial; not
                                            * a mark index, so no definition
                                            * name can collide with it */

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
    int   from_sound;
    ytl_op* ops;  int ops_cap;  /* big enough for events + after + the rest  */
    /* record list */
    int*  record_stims; int n_record;
    bool* stim_used;            /* the stimuli this template shows or changes */
} pl_tmpl;

typedef struct pl_response_rule {
    ydev_device* device;   /* unused in --sim */
} pl_response_rule;

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
} pl_out_stats;

typedef struct pl_row {
    bool    used, final;
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
} pl_row;

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
    const char* trace_path;    /* a test's view of every channel per frame  */
    const char* def_text;      /* the fuzz target: use these bytes for the
                                * experiment entry, keeping the pack's tables */
    size_t def_len;
} pl_args;

typedef struct pl_script_row { int trial; char* resp; bool silent; double rt_s, hold_s; } pl_script_row;

typedef struct pl_node_seed_rec { const char* path; int iteration; uint64_t seed; } pl_node_seed_rec;

typedef struct pl_rdk_step_row { int trial_index, update; int64_t t_ns; double coherence, direction, speed; } pl_rdk_step_row;

typedef struct pl_csrc pl_csrc;

/* --- the player --------------------------------------------------------- */

struct pl_player {
    pl_args   args;
    uint64_t  seed;
    uint64_t  participant_rng;
    char      error[1024];
    int       failed;
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
    char*     key_answer;      /* max_text + 1 bytes, sized at load          */
    size_t    max_text;        /* the longest value text the session can make */
    char*     text_buf;        /* max_text + 1: a cell, a lookup's key        */
    char*     runs_key;        /* a glyph-run block's key                     */
    size_t    row_text_cap;    /* bytes of a row's text arena                 */
    int       max_rdk_steps;
    int       n_leaf_runs_max;
    pl_stim_stats* stat_pool[PL_PENDING_ROWS];
    pl_out_stats*  out_pool[PL_PENDING_ROWS];
    pl_stim_stats* stats_now;
    pl_out_stats*  out_now;
    int       pool_stats;
    uint8_t*  frame_vis;      /* PL_FRAME_RING x pool_stats: was record k shown on that frame */
    float*    chan_base;      /* a literal key's value: every trial starts from it */
    bool*     chan_dyn;       /* the key holds a variable: read it per trial      */
    int64_t   ops_latest;     /* the latest time pl_build_ops() scheduled         */
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
    yrt_ring    ring;   /* flip and device records */
    unsigned char* ring_mem;

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
    /* the participant */
    bool      press_planned;
    bool      press_pending;
    int64_t   press_at, release_at;
    int       press_choice;
    double    press_rt;
    double    press_hold;
    bool      silent;
    pl_script_row* script; int n_script;
    int       trials_done;
    /* measured frame work: real ns, no clock read on the frame path when
     * measure is false */
    bool      measure;
    int64_t   work_max_ns, work_trial_max_ns, work_n, work_over_16ms;
};

/* --- load (lower.c) ------------------------------------------------------ */

int  pl_load(pl_player* p, const pl_args* a);
void pl_close(pl_player* p);
void pl_fail(pl_player* p, const char* fmt, ...);
void* pl_alloc(pl_player* p, size_t n);
const ytb_table* pl_table_get(pl_player* p, const char* name);
pl_stim* pl_stim_by_name(pl_player* p, const char* name);
/* The channel of a stimulus's property, or of its visibility when prop is
 * NULL; -1 when no step drives it. */
int  pl_channel(pl_player* p, const char* stim, const char* prop);
int  pl_trigger_channel(pl_player* p, const char* role);
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

/* --- run (run.c) --------------------------------------------------------- */

int  pl_run(pl_player* p);
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
int  pl_template_bound_ns(pl_player* p, const yexp_template* t, int64_t* ns);
int  pl_eval_var_bool(pl_player* p, const pl_env* env, const char* name, bool* out);
int  pl_channels_assign(pl_player* p);
int  pl_columns_build(pl_player* p);
pl_leaf* pl_leaf_of(pl_player* p, const yexp_node* n);
bool pl_track_done(void* ctx);
void pl_row_write_oldest(pl_player* p);
int  pl_event_queue(pl_player* p, const yds_event* ev);
int  pl_events_flush(pl_player* p, bool all);
void pl_trigger_sink(void* ctx, const yscr_trigger_info* info);
extern volatile int pl_loop_active;   /* the frame loop is running: a test counts allocations against it */

#ifdef __cplusplus
}
#endif
#endif
