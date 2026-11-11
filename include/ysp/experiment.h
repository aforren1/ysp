/* ysp/experiment.h - v0.3.0 - public domain single-header loader for experiment definitions
 *
 *   Reads a ysp experiment definition of format version 1 or 2
 *   (docs/experiment_format.md, the pack's EXPERIMENT entry) into a checked,
 *   fully defaulted form in a caller-given arena: session, display, devices,
 *   stimuli, methods, trial templates and the tree of nodes, and for version
 *   2 the signal roles, the controls, the states with their exits, the flags
 *   and the value forms (part 3). Every refusal names the element and
 *   the key, with the line and column of the source. It writes the
 *   definition's canonical bytes with defaults left out and gives the
 *   experiment ID (D2). A version 1 file keeps its version 1 meaning, form,
 *   canonical bytes and ID; yexp_migrated() gives its bytes under version 2
 *   (3.2).
 *
 *   The header lowers nothing: it does not know ysp/trials.h, ysp/timeline.h,
 *   ysp/gfx.h or ysp/response.h. A check that needs something outside the
 *   file goes through a caller-supplied table of functions (THE HOST); a
 *   check whose function is NULL is skipped and recorded as skipped.
 *
 *   Needs ysp/json.h and the C standard library. No OS calls, no threads, no
 *   heap (the arena decides). C99 is the floor: it builds as C99, C11 and
 *   C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_JSON_IMPLEMENTATION
 *       #include "ysp/json.h"
 *       #define YSP_EXPERIMENT_IMPLEMENTATION
 *       #include "ysp/experiment.h"
 *
 *       yjs_arena a;
 *       yexp_def* d;
 *       static yexp_report r;                       // 11 KB: not on a small stack
 *       yjs_arena_init_heap(&a, 0);                 // or yjs_arena_init(&a, buf, cap)
 *       if (yexp_load(&a, text, len, NULL, &d, &r) != YEXP_OK) {
 *           for (int i = 0; i < r.n_diags; i++)
 *               printf("%u:%u: %s\n", r.diags[i].line, r.diags[i].column, r.diags[i].msg);
 *       } else {
 *           const char* bytes; size_t n; uint8_t id[32];
 *           yexp_canonical(d, &a, &bytes, &n, id);  // the canonical bytes and the ID
 *       }
 *       yjs_arena_free(&a);                         // the form and everything else at once
 *
 *   ---------------------------------------------------------------------
 *   THE CHECKED FORM
 *   ---------------------------------------------------------------------
 *   yexp_def has plain arrays: session fields, roles, stimuli, methods and
 *   templates sorted by name (JSON objects are sorted), and the tree of
 *   nodes in file order. Every key that has a default holds it, so a reader
 *   tests no key for presence: the defaults come from one table of the
 *   format's (yexp_defaults(), "listed per version", D2), never from a
 *   header's desc, so a header that changes a default does not change an
 *   experiment. A key with no default that the definition left out is
 *   "none": a NULL pointer, or a value of kind YEXP_V_NONE.
 *   A value (yexp_value) is a literal, a reference "$name", a lookup, a
 *   linspace, a gamut, a michelson or a choice (format section 2.2); a
 *   duration has YEXP_VF_DURATION and its nanoseconds in i. Stimuli and
 *   methods keep their keys as a list (yexp_prop), found with
 *   yexp_prop_get().
 *
 *   Version 2 (format part 3). d->sigroles are the signal roles (3.5);
 *   d->controls the top-level controls and t->controls a template's own
 *   (3.7), each with its keys as a yexp_prop list, as a stimulus has, and its
 *   names resolved into yexp_ref: what the name is (a role, a control, a
 *   state, a stimulus, `response`, `end`), its index, and a lane (-1: every
 *   lane). t->states (NULL for a template with `events`), t->exits (the
 *   shared exits, with the indices of the states in `in`), t->flags and
 *   t->requeue_outcomes (3.11). Steps gain `until`, `recenter` and `flag`
 *   (3.12). The value forms of 3.10 and 3.15 are values of kind
 *   YEXP_V_SIGNAL, YEXP_V_TEST, YEXP_V_WHEN, YEXP_V_HIT and YEXP_V_TIME; a
 *   cursor's transform is YEXP_V_TRANSFORM or the string "identity"; vars
 *   take YEXP_V_UNIFORM and YEXP_V_WALK2D (3.14). The form says what was
 *   written and checked; it lowers nothing (no ysp/sig.h node exists here).
 *
 *   ---------------------------------------------------------------------
 *   REFUSALS
 *   ---------------------------------------------------------------------
 *   yexp_load() fills a yexp_report: at most YEXP_MAX_DIAGS diagnostics,
 *   each with its severity, line, column and message; n_errors and
 *   n_warnings count all of them, kept or not. The first pass (JSON shape,
 *   keys, types, names, the steps of a template) collects every error it
 *   can, element by element. A second pass (references, scopes, lookups,
 *   the host's checks) runs only when the first found none, so one mistake
 *   does not report twice; it collects too. A warning does not refuse: a
 *   duration above 60 s (D4), a lookup key that no level reaches (2.2), a
 *   template or stimulus that no node reaches, so its variables were not
 *   checked.
 *
 *   ---------------------------------------------------------------------
 *   THE HOST
 *   ---------------------------------------------------------------------
 *   yexp_host is a table of optional functions. entry() says whether a pack
 *   entry exists; column() and level() read a table's columns and levels;
 *   role() checks a role against the rig; param() checks a value against a
 *   header's parameter table; rules() runs ytr_rules() and a dry open;
 *   check_ops() runs ytl_check_ops() on a template; sigrole() checks a
 *   signal role (version 2) against the rig: an adapter for it, its type and
 *   units as ysig_open() takes them, its device. The rig profile has no
 *   adapter block yet (format G18), so no host supplies sigrole() today and
 *   a definition with signal roles records YEXP_CHECK_SIGROLES as skipped; a
 *   definition without any records neither bit. The pack tool, the
 *   player and the designer each supply what they have. report.run and
 *   report.skipped say which checks ran (YEXP_CHECK_*). When column() is
 *   NULL and a table is in scope, a variable that names no other thing
 *   is taken to be a column of that table. A column's type (yexp_column)
 *   decides what a variable may be used for; 0 and 1 are a table's only
 *   booleans. A level of 256 bytes or more is read again at its own length,
 *   and a level or level list past the limits is recorded in
 *   report.skipped, not as run.
 *
 *   ---------------------------------------------------------------------
 *   CANONICAL BYTES
 *   ---------------------------------------------------------------------
 *   yexp_canonical() writes ysp/json.h's canonical form with one LF after
 *   the value, as the pack tool does (pack.md 16), and with every key at its
 *   default left out; the SHA-256 of those bytes is the experiment ID. A
 *   duration is the exact decimal of its nanoseconds. Another real goes
 *   through its double (yjs_double(), then yjs_new_double()), so
 *   0.1 and 0.1000000000000000055 are one ID. A number whose exact value is
 *   an integer within int64 is that integer, however it is spelled. One
 *   target of a step and a choice with only its key are written in their
 *   shorter spelling. A number that yjs_double() cannot read (more than 19 significant
 *   digits, or out of range) is refused at load. docs/experiment.md gives
 *   the reasons. A version 1 file's bytes are its version 1 bytes, so its
 *   ID does not change with this header. yexp_migrated() gives the bytes of
 *   format 3.2's migration step: a version 1 file's canonical bytes with
 *   "version" 2 and no stimulus `visible` key (D12, D41), which load as
 *   version 2 to the same form; for a version 2 file, its canonical bytes.
 *
 *   ---------------------------------------------------------------------
 *   BOUNDS AND MEMORY
 *   ---------------------------------------------------------------------
 *   Everything lands in the caller's arena (yjs_arena_init() for a fixed
 *   buffer, yjs_arena_init_heap() for blocks with a limit): the parsed JSON,
 *   the form, the canonical tree and the work space. About 60 KB of work
 *   space, plus about 21 bytes per byte of source text for the worked
 *   examples, and about 530 bytes per template (docs/experiment.md). A full
 *   arena is YEXP_ERR_NOMEM; report.arena_need is the request that failed,
 *   not the total. Limits: 1024 stimuli, methods, roles, signal roles,
 *   controls (top-level, and per template) and templates, 4096 steps,
 *   states, exits or flags in one list, 256
 *   children of a node and 32 levels of nodes (the JSON depth of 64 stops a
 *   deeper file first), 5,000,000 work steps over both passes
 *   (YEXP_WORK_STEPS). Not for a
 *   frame loop: a definition is read once per session.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - format version 2 (docs/experiment_format.md part 3, D16 to D41):
 *          YEXP_FORMAT_VERSION is 2. Signal roles, top-level and template
 *          controls (the eight kinds of 3.7), states, exits, shared exits,
 *          max_visits and over, flags, requeue_outcomes, the steps until,
 *          recenter and flag, the value forms signal and when (3.10), the
 *          column forms hit, time, signal and when (3.15), signal choices in
 *          a response (3.13), the vars uniform and walk2d (3.14),
 *          display.units "cm"; the checks and refusals of 3.9, 3.11 and 3.16
 *          that need only the file; yexp_defaults(2) (3.17); the canonical
 *          bytes and ID of version 2; yexp_migrated() (3.2). A version 1 file
 *          loads as before: the same form, bytes and ID. Struct changes:
 *          new value kinds, step ops and fields, yexp_template's and
 *          yexp_def's version 2 parts, yexp_host.sigrole and
 *          YEXP_CHECK_SIGROLES. Nothing lowers version 2 yet.
 *   v0.2.0 - the AEP method's keys hyper.*, hyper_min.* and hyper_max.* are
 *          hyperparams.*, hyperparams_min.* and hyperparams_max.*, as the
 *          fields of ysp/aep.h 0.17.0. The loader refuses the old keys
 *          (unknown key). The format stays version 1
 *          (docs/experiment_format.md, 2.11). No struct changes.
 *   v0.1.3 - the levels of $outcome follow key_answer: "correct" and
 *          "wrong" with one, "responded" without one, and "timeout" when
 *          the response can time out. A lookup by $outcome on a template
 *          with no key_answer needs a "responded" key; that entry is read
 *          only after a press. No struct changes.
 *   v0.1.2 - valid definitions that v0.1.1 refused: a value read only on a
 *          trial with a response (outcome, method_value, the steps of
 *          {"if": "$responded"} or {"if": "$correct"}, a lookup's entry
 *          for a pressed key, "true" of $responded or $correct, "correct"
 *          or "wrong" of $outcome) can read $rt_s in a number key, and a
 *          lookup by $response, $responded or $outcome there needs no key
 *          for a timeout. $rt_s is valid in a number key when the response
 *          has no deadline. A value read only on a track trial
 *          (method_value, a lookup by $method's entry for a method's name)
 *          can read $level in a number key, and a lookup by $method there
 *          needs no "" key. No struct changes.
 *   v0.1.1 - refusals the player's lowering needs (docs/experiment.md, gaps):
 *          a leaf with tracks on a template with no response or no
 *          key_answer; a non-integer outcome; $rt_s, and $level on a leaf
 *          with both tracks and a table, in a numeric key; a bare $response
 *          as a trigger code. Levels: the empty string is a level of
 *          $response when the response can time out, and of $method on a
 *          leaf with both tracks and a table; a lookup by $response needs
 *          choices. No struct changes.
 *   v0.1.0 - first version: the loader for format version 1, its defaults
 *          table, the host interface and the canonical writer.
 *
 *   STATUS: v0.3.0 (2026-11-11). Tested; the first player (docs/player.md)
 *   uses it for version 1 and refuses version 2 by name until its lowering
 *   exists.
 *   docs/experiment.md has the decisions and the test results.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0; the text is at the end of the file.
 */
#ifndef YSP_EXPERIMENT_H_INCLUDED
#define YSP_EXPERIMENT_H_INCLUDED

#define YEXP_VERSION_MAJOR 0
#define YEXP_VERSION_MINOR 3
#define YEXP_VERSION_PATCH 0
#define YEXP_VERSION_STRING "0.3.0"

/* The newest format version this header reads (D12). */
#define YEXP_FORMAT_VERSION 2

#include "ysp/json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YEXP_API
#define YEXP_API extern
#endif

/* Results */
#define YEXP_OK           0
#define YEXP_ERR_REFUSED -1   /* the definition is refused: see the report  */
#define YEXP_ERR_NOMEM   -2   /* the arena is full: see the report          */
#define YEXP_ERR_ARG     -3   /* NULL or a bad argument                     */

/* Severity of a diagnostic */
#define YEXP_ERROR   0
#define YEXP_WARNING 1

/* Checks, for yexp_report.run and yexp_report.skipped */
#define YEXP_CHECK_ENTRIES 0x01u   /* entry(): pack entries exist           */
#define YEXP_CHECK_COLUMNS 0x02u   /* column(): tables and their columns    */
#define YEXP_CHECK_LEVELS  0x04u   /* level(): a lookup's map against levels*/
#define YEXP_CHECK_ROLES   0x08u   /* role(): roles against the rig         */
#define YEXP_CHECK_PARAMS  0x10u   /* param(): values against headers' tables */
#define YEXP_CHECK_RULES   0x20u   /* rules(): ytr_rules() and a dry open   */
#define YEXP_CHECK_OPS     0x40u   /* check_ops(): ytl_check_ops()          */
#define YEXP_CHECK_SIGROLES 0x80u  /* sigrole(): signal roles against the rig;
                                    * set only when the file has signal roles */

#define YEXP_MAX_DIAGS 32
#define YEXP_MSG_BYTES 320

typedef struct yexp_diag {
    int      severity;            /* YEXP_ERROR or YEXP_WARNING              */
    uint32_t line, column;        /* from 1; 0 when no place applies         */
    char     msg[YEXP_MSG_BYTES]; /* "stimulus 'fix': 'w': ..."              */
} yexp_diag;

typedef struct yexp_report {
    int       n_errors, n_warnings;   /* counted all, kept or not            */
    int       n_diags;                /* kept in diags[]                     */
    unsigned  run, skipped;           /* YEXP_CHECK_* bits                   */
    size_t    arena_need;             /* after YEXP_ERR_NOMEM: the failed request's
                                       * bytes, not the total the load wants */
    yexp_diag diags[YEXP_MAX_DIAGS];
} yexp_report;

/* --- values (format 2.2) ----------------------------------------------- */

typedef enum yexp_vkind {
    YEXP_V_NONE = 0,    /* absent, no default                                */
    YEXP_V_BOOL,        /* i: 0 or 1                                         */
    YEXP_V_NUMBER,      /* num; i with YEXP_VF_INT or YEXP_VF_DURATION       */
    YEXP_V_STRING,      /* str, "$$x" already read as "$x"                   */
    YEXP_V_ARRAY,       /* items[n]                                          */
    YEXP_V_VAR,         /* str: the name, without '$'                        */
    YEXP_V_LOOKUP,      /* by; map[n], sorted by key                         */
    YEXP_V_LINSPACE,    /* items[3]: lo, hi, n                               */
    YEXP_V_GAMUT,       /* str: "stimulus.property"                          */
    YEXP_V_MICHELSON,   /* by: the contrast's value                          */
    YEXP_V_CHOICE,      /* a node's vars: items[n], drawn per trial          */
    /* format version 2 */
    YEXP_V_SIGNAL,      /* {"signal": N}: str the name, i its lane (-1 every
                         * lane), n the lanes it gives; by the level of
                         * {"signal": N, "above": L} (a test), else NULL     */
    YEXP_V_TEST,        /* a when's test by name: str, a control with a test */
    YEXP_V_WHEN,        /* by: the test (TEST or SIGNAL with by); items[0]
                         * then, items[1] else; n 2                          */
    YEXP_V_HIT,         /* column {"hit": "C.x"}: str C, i the lane of C's of
                         * (-1: the one lane)                                */
    YEXP_V_TIME,        /* column {"time": N, "from": M}: str N, by a string
                         * value holding M                                   */
    YEXP_V_TRANSFORM,   /* a cursor's {"rotation_deg": A} or {"mirror_deg": A}:
                         * str the key, by A; "identity" is a string value   */
    YEXP_V_UNIFORM,     /* vars {"uniform": [lo, hi], "n": N, "per": "node"}:
                         * items[0] lo, items[1] hi, n 2, i N, str the per   */
    YEXP_V_WALK2D       /* vars {"walk2d": {...}}: items[7] = step, box x0,
                         * y0, x1, y1, start x, y; n 7                       */
} yexp_vkind;

#define YEXP_VF_INT      0x01u    /* an integer: i                           */
#define YEXP_VF_DURATION 0x02u    /* seconds: i is its nanoseconds           */

typedef struct yexp_value yexp_value;
typedef struct yexp_kv {
    const char*       key;
    const yexp_value* value;
} yexp_kv;

struct yexp_value {
    uint8_t           kind;       /* yexp_vkind                              */
    uint8_t           flags;      /* YEXP_VF_*                               */
    uint16_t          reserved_;
    uint32_t          n;          /* items, map entries                      */
    uint32_t          pos;        /* byte offset in the source               */
    double            num;
    int64_t           i;
    const char*       str;
    const yexp_value* items;
    const yexp_kv*    map;
    const yexp_value* by;
    uint8_t*          reached_;   /* which map keys a level reached          */
};

/* A key's value of a stimulus or a method. */
typedef struct yexp_prop {
    const char*       key;
    const yexp_value* value;
    bool              given;      /* false: from the format's defaults       */
} yexp_prop;

/* --- the form ---------------------------------------------------------- */

typedef struct yexp_field {
    const char* name;
    bool        is_string;        /* false: integer                          */
    bool        required;
} yexp_field;

typedef struct yexp_role {
    const char*        name;
    int                direction; /* 0 in, 1 out, 2 both                     */
    const char* const* family;
    int                n_family;
    bool               required;
    int                min_tier;  /* 0: none                                 */
    int64_t            pulse_ns;
    const char*        kind;
    uint32_t           pos_;      /* in the source, for a host's refusal    */
    const yexp_kv*     buttons;   /* string values                           */
    int                n_buttons;
    const yexp_kv*     map;       /* string values                           */
    int                n_map;
    const char* const* sorted_buttons_;
    bool               failed_;   /* refused in the first pass               */
} yexp_role;

typedef enum yexp_stim_kind {
    YEXP_SHAPE = 0, YEXP_GRATING, YEXP_GABOR, YEXP_NOISE, YEXP_IMAGE,
    YEXP_TEXT, YEXP_ARTWORK, YEXP_RDK, YEXP_SOUND, YEXP_VIDEO
} yexp_stim_kind;

typedef struct yexp_stim {
    const char*      name;
    yexp_stim_kind   kind;
    int              layer;
    const yexp_prop* props;       /* every key the kind has                  */
    int              n_props;
    const int*       dyn_;        /* indices of props that hold a variable   */
    int              n_dyn_;
    uint32_t         pos_;
    bool             failed_;     /* refused in the first pass               */
} yexp_stim;

typedef enum yexp_method_type { YEXP_STAIR = 0, YEXP_QUEST, YEXP_AEP } yexp_method_type;

typedef struct yexp_method {
    const char*        name;
    yexp_method_type   type;
    bool               log10;     /* "scale": "log10"                        */
    const char*        estimate;  /* "reversals", "trials", "mean", "mode"   */
    const char* const* dims;      /* aep only: names that become variables   */
    int                n_dims;
    const yexp_prop*   props;
    int                n_props;
} yexp_method;

typedef enum yexp_step_op {
    YEXP_ON = 0, YEXP_OFF, YEXP_WAIT, YEXP_AT, YEXP_SET, YEXP_TO, YEXP_THEN,
    YEXP_TRIGGER, YEXP_MARK, YEXP_IF, YEXP_UNLESS,
    YEXP_UNTIL, YEXP_RECENTER, YEXP_FLAG      /* format version 2 (3.12) */
} yexp_step_op;

/* What a name in a version 2 key is (format 3.9). */
typedef enum yexp_ref_kind {
    YEXP_REF_NONE = 0,  /* the key is not given                               */
    YEXP_REF_ROLE,      /* index into d->sigroles                             */
    YEXP_REF_CONTROL,   /* control                                            */
    YEXP_REF_STATE,     /* index into the template's states                   */
    YEXP_REF_STIMULUS,  /* index into d->stimuli                              */
    YEXP_REF_FLAG,      /* index into the template's flags                    */
    YEXP_REF_RESPONSE,  /* `response`: the template's response                */
    YEXP_REF_END        /* `end`: the trial's end                             */
} yexp_ref_kind;

typedef struct yexp_control yexp_control;

/* A name, resolved: "hand", "hand.x", "past_target", "response". */
typedef struct yexp_ref {
    const char*         name;     /* without the lane; NULL: not given       */
    int                 kind;     /* yexp_ref_kind                           */
    int                 index;    /* role, state, stimulus, flag; else -1    */
    int                 lane;     /* -1 every lane; 0 x, 1 y, 2 z, 3 w        */
    int                 lanes;    /* lanes it reads (1 with a lane); 0: none */
    const yexp_control* control;  /* YEXP_REF_CONTROL                        */
    uint32_t            pos_;
} yexp_ref;

typedef struct yexp_step yexp_step;
struct yexp_step {
    yexp_step_op       op;
    const char* const* stims;     /* on, off: names; set, to, then: one      */
    int                n_stims;
    const yexp_value*  t;         /* wait_s, at_s                            */
    const yexp_value*  stagger;   /* on, off                                 */
    const yexp_value*  duration;  /* to, then                                */
    const yexp_value*  delay;     /* to, then                                */
    const char*        ease;      /* to, then                                */
    const yexp_prop*   props;     /* set, to, then                           */
    int                n_props;
    const yexp_value*  code;      /* trigger                                 */
    const char*        role;      /* trigger                                 */
    const char*        mark;      /* mark                                    */
    const char*        cond;      /* if, unless: the variable's name         */
    uint32_t           cond_pos_;
    const yexp_step*   steps;     /* if, unless                              */
    int                n_steps;
    /* format version 2 */
    yexp_ref           target;    /* until: the control; recenter: the role  */
    const yexp_value*  timeout;   /* until's timeout_s; NULL: none           */
    const yexp_value*  point;     /* recenter's to: a point                  */
    const char*        flag;      /* flag                                    */
    bool               flag_value;
};

typedef struct yexp_choice {
    const char* key;
    const char* name;             /* the key when not given                  */
    const yexp_control* signal;   /* a signal choice's movement (3.13): key and
                                   * name are its name; NULL for a key choice */
} yexp_choice;

typedef struct yexp_response {
    const char*        role;      /* NULL with signal choices                */
    const char*        from;
    const yexp_choice* choices;   /* NULL: none listed                       */
    int                n_choices;
    const yexp_value*  duration;       /* seconds; 0: no deadline            */
    const yexp_value*  min_rt;         /* minimum_valid_rt_s                 */
    const yexp_value*  key_answer;     /* NULL when not given                */
    bool               ends_trial;
    int                max_responses;
    bool               allow_held_key;
    bool               wait_for_key_release;
    const char*        trigger_role;   /* NULL: no trigger                   */
    const yexp_value*  trigger_code;
    bool               signal_choices; /* version 2: choices are movements    */
} yexp_response;

/* --- format version 2 (part 3) ------------------------------------------ */

/* A signal role (3.5). */
typedef enum yexp_sig_type {
    YEXP_SIG_POSITION2D = 0, YEXP_SIG_POSITION2D_RELATIVE, YEXP_SIG_POSITION3D, YEXP_SIG_SCALAR, YEXP_SIG_GAZE
} yexp_sig_type;
typedef enum yexp_sig_units {
    YEXP_UNITS_DISPLAY_PX = 0, YEXP_UNITS_DISPLAY_CM, YEXP_UNITS_DISPLAY_DEG, YEXP_UNITS_WORKSPACE_CM, YEXP_UNITS_DEVICE
} yexp_sig_units;

typedef struct yexp_sigrole {
    const char* name;
    int         type;             /* yexp_sig_type                           */
    int         units;            /* yexp_sig_units                          */
    int         lanes;            /* from the type: 1, 2 or 3                */
    bool        allow_relative;
    bool        required;
    uint32_t    pos_;
    bool        failed_;
} yexp_sigrole;

/* The kinds of control (3.7). */
typedef enum yexp_control_kind {
    YEXP_CURSOR = 0, YEXP_REGION, YEXP_DISTANCE, YEXP_MOVEMENT, YEXP_PEAK, YEXP_MOVING_TARGET, YEXP_TRACE, YEXP_SIGNAL_LOST
} yexp_control_kind;

/* One lane of a moving target: n components (3.7). */
typedef struct yexp_sines {
    const yexp_value* amplitudes; /* an array of n numbers                   */
    const yexp_value* freqs_hz;   /* an array of n numbers                   */
    const yexp_value* phases;     /* an array of n numbers, or a variable    */
    int               n;
} yexp_sines;

struct yexp_control {
    const char*       name;
    yexp_control_kind kind;
    bool              shared;     /* a top-level control (D25)               */
    const yexp_prop*  props;      /* every key of the kind, defaults filled   */
    int               n_props;
    /* names, resolved (3.9) */
    yexp_ref          of;         /* region, distance, movement, peak,
                                   * signal_lost; a cursor's role            */
    const yexp_ref*   items;      /* a trace's of; a cursor's mean           */
    int               n_items;
    yexp_ref          x, y;       /* a cursor's two lanes                    */
    yexp_ref          from, to, freeze_at, stimulus;
    yexp_ref          center;     /* a region's {"signal": N} center         */
    yexp_sines        sx, sy;     /* a moving target's lanes                 */
    /* what it gives (3.7): lanes of its value (0: none), a test, hits */
    int               lanes;
    bool              has_test, has_hits;
    uint32_t          pos_;
    bool              failed_;
    bool              named_;     /* a response names this movement          */
};

/* An exit (3.11): `on` a hit, `after_s`, or instant (neither). */
typedef struct yexp_exit {
    yexp_ref          on;         /* kind NONE: not `on`                     */
    const yexp_value* after;      /* after_s; NULL: none                     */
    const char*       if_var;     /* the variable or flag, without '$'       */
    bool              if_flag;    /* if_var names one of the template's flags */
    yexp_ref          to;         /* a state or `end`                        */
    const yexp_value* outcome;    /* NULL: none                              */
    const int*        in;         /* a shared exit's states (indices)        */
    int               n_in;
    uint32_t          pos_, if_pos_;
} yexp_exit;

typedef struct yexp_state {
    const char*       name;
    const yexp_step*  steps;   int n_steps;
    const yexp_exit*  exits;   int n_exits;   /* its own, in order           */
    int               max_visits;              /* 0: none                     */
    yexp_ref          over_to;                 /* kind NONE: no over          */
    const yexp_value* over_outcome;            /* NULL: none                  */
    uint32_t          pos_;
} yexp_state;

typedef struct yexp_template {
    const char*          name;
    const yexp_step*     events;   int n_events;
    const yexp_step*     after;    int n_after;
    const yexp_response* response;  /* NULL: none                            */
    const char* const*   record;   int n_record;  /* given, or the default   */
    bool                 record_given;
    const yexp_kv*       columns;  int n_columns;
    const yexp_value*    outcome;
    const yexp_value*    method_value;  /* NULL: no leaf with tracks runs it */
    uint32_t             pos_;
    bool                 has_tracks_;   /* some leaf with tracks runs it     */
    bool                 mv_written_;
    uint32_t             mv_pos_;
    void*                canon_;        /* its object in the canonical tree  */
    void*                mv_canon_;     /* a written method_value not at its default */
    /* format version 2 */
    const yexp_control*  controls;  int n_controls;  /* its own, by name    */
    const yexp_state*    states;    int n_states;    /* NULL: `events`      */
    const yexp_exit*     exits;     int n_exits;     /* shared exits        */
    const char* const*   flags;     int n_flags;
    const yexp_value*    requeue_outcomes;  /* an array; NULL: none         */
    uint32_t             states_pos_;
    uint8_t*             uses_top;  /* per top-level control: 1 when the template
                                     * names it, so its graph has a copy (3.6) */
} yexp_template;

/* A rules text's `var NAME choice V V ...`. */
typedef struct yexp_rule_var {
    const char*        name;
    const char* const* values;    /* NULL: too many to keep (above 62)       */
    int                n_values;
} yexp_rule_var;

typedef struct yexp_node yexp_node;
struct yexp_node {
    const char*          name;
    const char*          path;        /* "a/b"                               */
    const yexp_node*     parent;
    const yexp_template* trial;       /* NULL: a container                   */
    const yexp_node*     children;    int n_children;
    const char*          table;       /* timeline_variables; NULL: none      */
    const char*          rules;       /* joined with LF; "" when none        */
    const char* const*   rule_items;  int n_rules;
    const char* const*   jitters;     int n_jitters;  /* names from the rules */
    const yexp_rule_var* rule_vars;   int n_rule_vars;
    const yexp_method* const* tracks; int n_tracks;
    int                  interleave;  /* 0 random, 1 round_robin             */
    double               track_rate;
    const yexp_kv*       vars;        int n_vars;  /* a value or a choice    */
    const yexp_kv*       data;        int n_data;
    int                  repetitions;
    const char*          if_var;      /* NULL: none                          */
    const char*          unless_var;
    bool                 has_loop;
    double               loop_until;  /* until_accuracy                      */
    int                  loop_max;
    bool                 requeue_no_response;
    bool                 record_data;
    uint32_t             pos_, if_pos_, unless_pos_;
};

typedef struct yexp_def {
    int                  format_version;
    const char*          name;        /* "" when not given                   */
    const yexp_field*    fields;      int n_fields;
    int                  units_deg;   /* 0: px, 1: deg                       */
    double               background[3];
    bool                 calibration_required;
    int                  min_tier;    /* 0: none                             */
    const yexp_role*     roles;       int n_roles;
    const yexp_stim*     stimuli;     int n_stimuli;
    const yexp_method*   methods;     int n_methods;
    const yexp_template* templates;   int n_templates;
    const yexp_node*     timeline;    int n_timeline;
    bool                 web_allowed;
    const yjs_value*     web_exclusion;  /* NULL when not given; recorded only */
    yjs_value*           canon_;
    /* format version 2 */
    int                  units;       /* 0 px, 1 deg, 2 cm (units_deg is 1 for deg) */
    const yexp_sigrole*  sigroles;    int n_sigroles;   /* by name         */
    const yexp_control*  controls;    int n_controls;   /* top-level, by name */
} yexp_def;

/* --- the host: checks that need something outside the file ------------- */

/* Kinds of pack entry for entry(). */
#define YEXP_ENTRY_TABLE   0
#define YEXP_ENTRY_TEXTURE 1
#define YEXP_ENTRY_FONT    2
#define YEXP_ENTRY_ARTWORK 3
#define YEXP_ENTRY_AUDIO   4
#define YEXP_ENTRY_VIDEO   5

/* Types of a column. */
#define YEXP_COL_STRING  0
#define YEXP_COL_NUMBER  1
#define YEXP_COL_INTEGER 2

typedef struct yexp_column {
    const char* name;
    int         type;          /* YEXP_COL_*                                 */
    int         n_levels;
} yexp_column;

/* Sections for param(). */
#define YEXP_PARAM_STIMULUS 0  /* kind: the stimulus's kind name             */
#define YEXP_PARAM_METHOD   1  /* kind: "stair", "quest", "aep"              */

typedef struct yexp_host {
    void* ctx;
    /* A pack entry: 1 exists, 0 does not. */
    int (*entry)(void* ctx, int kind, const char* name);
    /* Column i of a table: 1 and *out; 0 past the last; < 0 no such table. */
    int (*column)(void* ctx, const char* table, int i, yexp_column* out);
    /* Level l of column col as its canonical text, in buf: its length, or
     * < 0 past the last. A number's text is ysp/json.h's canonical text. */
    int (*level)(void* ctx, const char* table, int col, int l, char* buf, size_t cap);
    /* A role against the rig: 0, or nonzero with a message in msg. */
    int (*role)(void* ctx, const yexp_role* role, char* msg, size_t cap);
    /* A given key of a stimulus or method: 0, or nonzero with a message. */
    int (*param)(void* ctx, int section, const char* kind, const yexp_prop* p, char* msg, size_t cap);
    /* A leaf's rules (ytr_rules(), a dry ytr_open()): 0, or nonzero. */
    int (*rules)(void* ctx, const yexp_node* leaf, char* msg, size_t cap);
    /* A template (ytl_check_ops() at the extremes): 0, or nonzero. */
    int (*check_ops)(void* ctx, const yexp_def* d, const yexp_template* t, char* msg, size_t cap);
    /* Version 2: a signal role against the rig profile (format 3.5, "Checks
     * with a rig"): the profile has an adapter for it, ysig_open() takes its
     * type and units (a relative adapter under position2d needs
     * allow_relative), and its device opens. 0, or nonzero with a message.
     * A required role without an adapter is a refusal; an optional one is
     * the host's to accept. Called once per signal role, after the first
     * pass. The rig profile has no adapter block yet (G18), so no host has
     * one; NULL records YEXP_CHECK_SIGROLES as skipped. */
    int (*sigrole)(void* ctx, const yexp_sigrole* role, char* msg, size_t cap);
} yexp_host;

/* --- the format's table of keys and defaults (D2) ---------------------- */

typedef struct yexp_row {
    uint8_t     section;       /* internal section number                    */
    uint16_t    kinds;         /* stimuli: bit 1 << kind; steps: bit 1 << op */
    uint8_t     type;          /* internal value type                        */
    uint8_t     flags;
    const char* key;
    const char* def;           /* canonical JSON text; NULL: no default      */
    const char* values;        /* an enum's "name=value,..."; NULL otherwise */
} yexp_row;

/* The table for format version `version`; NULL for one this header does not
 * read. *n gets the row count. */
YEXP_API const yexp_row* yexp_defaults(int version, int* n);
/* The name of a row's section ("stimulus", "node", ...). */
YEXP_API const char* yexp_section_name(int section);
/* Whether a row is required (no default). */
YEXP_API bool yexp_row_required(const yexp_row* r);

/* --- the calls --------------------------------------------------------- */

YEXP_API const char* yexp_version(void);

/* Parses and checks text[0..n) (UTF-8 JSON) into a form in the arena.
 * YEXP_OK and *out, or YEXP_ERR_REFUSED or YEXP_ERR_NOMEM with the report
 * filled and *out NULL. host may be NULL (every host check is skipped). */
YEXP_API int yexp_load(yjs_arena* a, const char* text, size_t n, const yexp_host* host, yexp_def** out, yexp_report* report);

/* The canonical bytes (with one LF after the value) and the experiment ID,
 * the SHA-256 of those bytes. The bytes live in the arena a; *n has their
 * length. id may be NULL. YEXP_OK, YEXP_ERR_NOMEM or YEXP_ERR_ARG. */
YEXP_API int yexp_canonical(const yexp_def* d, yjs_arena* a, const char** bytes, size_t* n, uint8_t id[32]);

/* The migrated canonical bytes and their SHA-256 (format 3.2, D12): for a
 * version 1 definition, its canonical bytes with "version" 2 and no
 * stimulus `visible` key; for a version 2 definition, its canonical bytes.
 * Arguments and results as yexp_canonical(). */
YEXP_API int yexp_migrated(const yexp_def* d, yjs_arena* a, const char** bytes, size_t* n, uint8_t id[32]);

/* Version 2 lookups by name; NULL when there is none. A template's control
 * is found in t (NULL: top-level only), then among the top-level ones. */
YEXP_API const yexp_sigrole* yexp_sigrole_get(const yexp_def* d, const char* name);
YEXP_API const yexp_control* yexp_control_get(const yexp_def* d, const yexp_template* t, const char* name);

/* A stimulus's or a method's key: the prop or NULL. */
YEXP_API const yexp_prop* yexp_prop_get(const yexp_prop* props, int n, const char* key);
YEXP_API const yexp_stim* yexp_stim_get(const yexp_def* d, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* YSP_EXPERIMENT_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_EXPERIMENT_IMPLEMENTATION
#ifndef YSP_EXPERIMENT_IMPLEMENTATION_GUARD
#define YSP_EXPERIMENT_IMPLEMENTATION_GUARD

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

YEXP_API const char* yexp_version(void) { return YEXP_VERSION_STRING; }

/* Sections of the table. */
enum {
    YEXP__S_ROOT, YEXP__S_DISPLAY, YEXP__S_SESSION, YEXP__S_SFIELD, YEXP__S_WEB, YEXP__S_ROLE,
    YEXP__S_STIM, YEXP__S_STAIR, YEXP__S_QUEST, YEXP__S_AEP, YEXP__S_TEMPLATE, YEXP__S_RESPONSE,
    YEXP__S_RTRIGGER, YEXP__S_CHOICE, YEXP__S_NODE, YEXP__S_LOOP, YEXP__S_STEP,
    /* format version 2 */
    YEXP__S_SIGROLE, YEXP__S_CONTROL, YEXP__S_STATE, YEXP__S_OVER, YEXP__S_EXIT, YEXP__S_SRESPONSE,
    YEXP__S_UNIFORM, YEXP__S_WALK2D, YEXP__S_COUNT
};

/* Value types of a row. */
enum {
    YEXP__T_NUM, YEXP__T_INT, YEXP__T_DUR, YEXP__T_BOOL, YEXP__T_STR, YEXP__T_ENUM, YEXP__T_ENUMS,
    YEXP__T_TEXT, YEXP__T_RES, YEXP__T_VEC3, YEXP__T_NUMS, YEXP__T_GRID, YEXP__T_VAL, YEXP__T_CODE,
    YEXP__T_VAR, YEXP__T_NAME, YEXP__T_NAMEZ, YEXP__T_NAMES, YEXP__T_STRS, YEXP__T_TARGETS,
    YEXP__T_SMAP, YEXP__T_ANY, YEXP__T_OBJ, YEXP__T_SET, YEXP__T_LIST,
    /* format version 2 */
    YEXP__T_POINT,      /* [x, y]: numbers or variables; a variable; a lookup */
    YEXP__T_CENTER,     /* a point, or {"signal": NAME} (a region's center)   */
    YEXP__T_TRANSFORM,  /* "identity", {"rotation_deg": A}, {"mirror_deg": A}, a lookup */
    YEXP__T_REF,        /* a name with an optional lane: "hand", "hand.x"     */
    YEXP__T_REFS,       /* an array of them                                   */
    YEXP__T_SINES,      /* a moving target's lane: its builder reads it       */
    YEXP__T_BOX         /* walk2d's [[x0, y0], [x1, y1]]: literal numbers     */
};

/* Flags of a row. */
#define YEXP__R_REQ  0x01u   /* required                                     */
#define YEXP__R_LIT  0x02u   /* a literal, never a variable or lookup        */
#define YEXP__R_NONE 0x04u   /* optional, with no default                    */
#define YEXP__R_SELF 0x08u   /* the section's builder writes the canonical key */
#define YEXP__R_STEP 0x10u   /* version 2: a step's property, no stimulus key (D41) */

/* Blocks that both versions' tables share. */
#define YEXP__ROWS_SESSION_ROLE \
    { YEXP__S_SESSION, 0xffff, YEXP__T_SET, YEXP__R_REQ, "fields", NULL, NULL }, \
    { YEXP__S_SFIELD, 0xffff, YEXP__T_BOOL, YEXP__R_REQ, "required", NULL, NULL }, \
    { YEXP__S_SFIELD, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "integer=0,string=1" }, \
    { YEXP__S_WEB, 0xffff, YEXP__T_BOOL, 0, "allowed", "false", NULL }, \
    { YEXP__S_WEB, 0xffff, YEXP__T_ANY, YEXP__R_NONE, "exclusion", NULL, NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_SMAP, 0, "buttons", "{}", NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "direction", NULL, "in=0,out=1,both=2" }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_STRS, YEXP__R_REQ, "family", NULL, NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_STR, 0, "kind", "\"\"", NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_SMAP, 0, "map", "{}", NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_INT, YEXP__R_LIT, "min_tier", "0", NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_DUR, 0, "pulse_s", "0", NULL }, \
    { YEXP__S_ROLE, 0xffff, YEXP__T_BOOL, 0, "required", "true", NULL },
#define YEXP__ROWS_STIM_A \
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "add", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "ahead", "6", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "algorithm", "\"custom\"", "custom=0,wn=1,mn=2,ll=3,bm=4" }, \
    { YEXP__S_STIM, 0x0060, YEXP__T_INT, 0, "aliased", "0", NULL }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_ENUM, YEXP__R_LIT, "align", "\"start\"", "start=0,end=1,center=2" }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_ENUM, 0, "anchor", "\"center\"", "center=0,top_left=1,top=2,top_right=3,left=4,right=5,bottom_left=6,bottom=7,bottom_right=8" }, \
    { YEXP__S_STIM, 0x000a, YEXP__T_ENUM, 0, "aperture", "\"rect\"", "rect=0,circle=1,annulus=2,line=3,polygon=4,cross=5,no_aperture=6,mask_tex=7" }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "aperture", "\"circle\"", "circle=0,rect=1" }, \
    { YEXP__S_STIM, 0x0040, YEXP__T_RES, YEXP__R_REQ, "artwork", NULL, NULL }, \
    { YEXP__S_STIM, 0x0004, YEXP__T_NUM, 0, "aspect", "1", NULL }, \
    { YEXP__S_STIM, 0x0100, YEXP__T_RES, YEXP__R_REQ, "audio", NULL, NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "ax", "0.5", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "ay", "0.5", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "base", "0", NULL }, \
    { YEXP__S_STIM, 0x0061, YEXP__T_ENUM, 0, "blend", "\"over\"", "over=0,add=1,multiply=2" }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_ENUM, 0, "cap", "\"butt\"", "butt=0,round=1,square=2" }, \
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "channels", "0", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "check", "1", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "chroma", "\"sited\"", "sited=0,nearest=1" }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "clock", "\"onset\"", "onset=0,frames=1" }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "coherence", "0", NULL }, \
    { YEXP__S_STIM, 0x02f1, YEXP__T_VEC3, 0, "color", "[0,0,0]", NULL }, \
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "contrast", "1", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_INT, 0, "count", "0", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "coverage", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "dash", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "dash_offset", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_INT, 0, "dash_snap", "0", NULL }, \
    { YEXP__S_STIM, 0x0100, YEXP__T_NUM, 0, "db", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "decode_threads", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "density", "0", NULL }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_ENUM, YEXP__R_LIT, "dir", "\"auto\"", "auto=0,ltr=1,rtl=2" }, \
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "dir_b", "0", NULL }, \
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "dir_g", "0", NULL }, \
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "dir_r", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "direction", "0", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_ENUM, 0, "dist", "\"uniform\"", "uniform=0,binary=1,gaussian=2,simplex=3" }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "dot_size", "0", NULL }, \
    { YEXP__S_STIM, 0x000b, YEXP__T_ENUM, 0, "edge", "\"hard\"", "hard=0,cosine=1,gaussian=2" }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "edge", "\"wrap\"", "wrap=0,replot=1" }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "edge_width", "0", NULL }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_STR, YEXP__R_REQ | YEXP__R_LIT, "font", NULL, NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_DUR, 0, "frame_s", "0", NULL }, \
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "frames", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "gap", "0", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "gate", "1", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "gpu_path", "\"auto\"", "auto=0,upload=1,shared=2,gpu=3" }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "h", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "hw_decode", "\"auto\"", "auto=0,off=1,dxva=2,videotoolbox=3,vaapi=4" }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_ENUM, 0, "join", "\"round\"", "round=0,miter=1,bevel=2" }, \
    { YEXP__S_STIM, 0x03ff, YEXP__T_ENUM, YEXP__R_REQ, "kind", NULL, "shape=0,grating=1,gabor=2,noise=3,image=4,text=5,artwork=6,rdk=7,sound=8,video=9" }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_STR, YEXP__R_LIT, "lang", "\"\"", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_INT, 0, "layer", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_NUM, 0, "lead", "0.5", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_INT, 0, "lifetime_frames", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_DUR, 0, "lifetime_s", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "light", "\"auto\"", "auto=0,codes=1,eotf=2" }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_NUM, YEXP__R_LIT, "line_height", "0", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "linear", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_BOOL, 0, "loop", "false", NULL }, \
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "loops", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "min_tier", "0", NULL }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "miter_limit", "4", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "modulation", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "movie_index", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "noise", "\"direction\"", "direction=0,position=1,walk=2" }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_gain", "0.5", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_lacunarity", "2", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_scale", "0", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_z", "0", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_INT, 0, "octaves", "1", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "offset", "0", NULL }, \
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "offset", "0", NULL }, \
    { YEXP__S_STIM, 0x02f1, YEXP__T_NUM, 0, "opacity", "1", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "ori", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p0", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p1", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p10", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p11", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p12", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p13", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p14", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p15", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p16", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p17", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p18", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p19", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p2", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p20", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p21", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p22", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p23", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p24", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p25", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p26", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p27", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p28", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p29", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p3", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p30", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p31", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p4", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p5", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p6", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p7", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p8", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p9", "0", NULL }, \
    { YEXP__S_STIM, 0x0006, YEXP__T_NUM, 0, "phase", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "pin_cpu", "0", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_ENUM, 0, "place", "\"center\"", "center=0,top_left=1,top=2,top_right=3,left=4,right=5,bottom_left=6,bottom=7,bottom_right=8" }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "premultiplied", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "preroll", "6", NULL }, \
    { YEXP__S_STIM, 0x0060, YEXP__T_INT, 0, "rays", "0", NULL }, \
    { YEXP__S_STIM, 0x0008, YEXP__T_INT, 0, "seed", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "select", "\"exact\"", "exact=0,bernoulli=1" }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_INT, 0, "sets", "1", NULL }, \
    { YEXP__S_STIM, 0x0006, YEXP__T_NUM, 0, "sf", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_ENUM, 0, "shape", "\"rect\"", "rect=0,circle=1,annulus=2,line=3,polygon=4,cross=5,no_aperture=6,mask_tex=7,rrect=8,arc=9,pie=10,capsule=11,ngon=12,star=13,ellipse=14,qbezier=15,polyline=16,compound=17" }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p0", "0", NULL }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p1", "0", NULL }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p2", "0", NULL }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p3", "0", NULL }, \
    { YEXP__S_STIM, 0x0004, YEXP__T_NUM, 0, "sigma", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "signal", "\"same\"", "same=0,different=1,least_recent=2" }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_NUM, YEXP__R_REQ | YEXP__R_LIT, "size", NULL, NULL }, \
    { YEXP__S_STIM, 0x0040, YEXP__T_NUM, 0, "size", "0", NULL }, \
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "speed", "0", NULL }, \
    { YEXP__S_STIM, 0x0002, YEXP__T_INT, 0, "square", "0", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_h", "0", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_w", "0", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_x", "0", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_y", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_BOOL, 0, "strict_cadence", "false", NULL }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "stroke", "0", NULL }, \
    { YEXP__S_STIM, 0x008b, YEXP__T_ENUM, 0, "stroke_align", "\"center\"", "center=0,inside=1,outside=2" }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_TEXT, YEXP__R_REQ, "text", NULL, NULL }, \
    { YEXP__S_STIM, 0x0010, YEXP__T_RES, YEXP__R_REQ, "texture", NULL, NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_a", "1", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_b", "1", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_g", "1", NULL }, \
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_r", "1", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "trim_end", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_INT, 0, "trim_set", "0", NULL }, \
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "trim_start", "0", NULL }, \
    { YEXP__S_STIM, 0x0200, YEXP__T_RES, YEXP__R_REQ, "video", NULL, NULL },
#define YEXP__ROWS_STIM_B \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "w", "0", NULL }, \
    { YEXP__S_STIM, 0x0020, YEXP__T_NUM, YEXP__R_LIT, "width", "0", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "x", "0", NULL }, \
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "y", "0", NULL },
#define YEXP__ROWS_METHODS \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "est_reversals", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "est_trials", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "estimate", "\"reversals\"", "reversals=0,trials=1" }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_BOOL, 0, "harder_is_up", "false", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_BOOL, 0, "initial_rule", "false", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "max", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "min", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "n_down", "1", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "n_up", "1", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "rule", "\"updown\"", "updown=0,asa=1" }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "scale", "\"linear\"", "linear=0,log10=1" }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "start", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "step_down_scale", "1", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "step_type", "\"lin\"", "lin=0,log=1,db=2" }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "steps", NULL, NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "stop_at_limit", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "stop_reversals", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "stop_trials", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "target_p", "0", NULL }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "stair=0,quest=1,aep=2" }, \
    { YEXP__S_STAIR, 0xffff, YEXP__T_BOOL, 0, "use_limits", "false", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "estimate", "\"mean\"", "mean=0,mode=1" }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUMS, 0, "joint_prior", "[]", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "n_outcomes", "2", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_BOOL, 0, "no_table", "false", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_GRID, YEXP__R_REQ, "param", NULL, NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "pf", "\"gumbel\"", "gumbel=0,weibull=1,logistic=2,normal=3,hypsec=4,custom=5" }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "scale", "\"linear\"", "linear=0,log10=1" }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "select", "\"entropy\"", "entropy=0,quantile=1,mean=2,mode=3" }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "select_param", "0", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "select_quantile", "0.5", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_GRID, YEXP__R_REQ, "stim", NULL, NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "stop_entropy", "0", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "stop_sd", "0", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "stop_sd_param", "0", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "stop_trials", "0", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "subset_size", "0", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "tie_tolerance", "1e-09", NULL }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "tiebreak", "\"lowest\"", "lowest=0,nearest=1,alternate=2,random=3" }, \
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "stair=0,quest=1,aep=2" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "acq", "\"lse\"", "lse=0,eavc=1,localmi=2,balv=3,bald=4,random=5,ucb=6,ei=7,thompson=8" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "acq_beta", "1.96", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "candidates", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUMS, 0, "dim_kind", "[]", "continuous=0,integer=1,categorical=2" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "dim_levels", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NAMES, YEXP__R_NONE, "dims", NULL, NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "estimate", "\"mean\"", "mean=0,mode=1" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "fit", "false", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "fit_every", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "fit_max_evals", "40", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "fit_pcg", "false", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "fit_tol", "1e-08", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "grid", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "guess", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "hi", NULL, NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams.cutpoint", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams.lengthscale", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams.lengthscale_b", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams.lengthscale_g", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams.mean", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams.mean_g", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams.noise_sd", "0.1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams.outputscale", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams.outputscale_b", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams.outputscale_g", "0.3", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_max.cutpoint", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_max.lengthscale", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_max.lengthscale_b", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_max.lengthscale_g", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_max.mean", "5", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_max.mean_g", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_max.noise_sd", "10", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_max.outputscale", "10", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_max.outputscale_b", "10", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_max.outputscale_g", "10", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_min.cutpoint", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_min.lengthscale", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_min.lengthscale_b", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyperparams_min.lengthscale_g", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_min.mean", "-5", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_min.mean_g", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_min.noise_sd", "0.001", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_min.outputscale", "0.1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_min.outputscale_b", "0.1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyperparams_min.outputscale_g", "0.01", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "intensity_dim", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "jitter", "1e-06", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "kernel", "\"rbf\"", "rbf=0,semip=1" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "lapse", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "lik", "\"bernoulli\"", "bernoulli=0,ordinal=1,categorical=2,gaussian=3,pairwise=4" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "link", "\"probit\"", "probit=0,logit=1" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "lo", NULL, NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "max_trials", "512", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "minimize", "false", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "model", "\"gp\"", "gp=0,psychometric=1" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "monotone_dims", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "n_candidates", "512", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "n_init", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "n_outcomes", "2", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "no_hyper_prior", "false", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "pcg_threshold", "512", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.lengthscale.center", "0.25", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.lengthscale.sd", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean.center", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean.sd", "0.7", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean_g.ceiling", "0.00390625", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean_g.center", "0.25", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean_g.sd", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.noise_sd.center", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.noise_sd.sd", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale.center", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale.sd", "0.7", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_b.center", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_b.sd", "0.7", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_g.center", "0.3", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_g.sd", "0.7", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "refine_steps", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "refit_every", "1", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "scale", "\"linear\"", "linear=0,log10=1" }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "stop_context", "[]", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "stop_threshold_sd", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "stop_trials", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "target_outcome", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "target_p", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "target_value", "0", NULL }, \
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "stair=0,quest=1,aep=2" },
#define YEXP__ROWS_RESPONSE_LOOP \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "allow_held_key", "false", NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_LIST, YEXP__R_NONE, "choices", NULL, NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_DUR, 0, "duration_s", "0", NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "from", NULL, NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_VAL, YEXP__R_NONE, "key_answer", NULL, NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_INT, YEXP__R_LIT, "max_responses", "0", NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_DUR, 0, "minimum_valid_rt_s", "0", NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "response_ends_trial", "true", NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "role", NULL, NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "trigger", NULL, NULL }, \
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "wait_for_key_release", "false", NULL }, \
    { YEXP__S_RTRIGGER, 0xffff, YEXP__T_CODE, YEXP__R_REQ, "code", NULL, NULL }, \
    { YEXP__S_RTRIGGER, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "role", NULL, NULL }, \
    { YEXP__S_CHOICE, 0xffff, YEXP__T_STR, YEXP__R_REQ, "key", NULL, NULL }, \
    { YEXP__S_CHOICE, 0xffff, YEXP__T_STR, YEXP__R_NONE, "name", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_SET, 0, "data", "{}", NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_VAR, YEXP__R_NONE, "if", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_ENUM, 0, "interleave", "\"random\"", "random=0,round_robin=1" }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "loop", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "name", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_BOOL, 0, "record_data", "true", NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_INT, 0, "repetitions", "1", NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_ENUM, YEXP__R_NONE, "requeue", NULL, "no_response=0" }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_STRS, 0, "rules", "[]", NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_LIST, YEXP__R_NONE, "timeline", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_STR, YEXP__R_NONE, "timeline_variables", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_NUM, 0, "track_rate", "0", NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_NAMES, 0, "tracks", "[]", NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_NAME, YEXP__R_NONE, "trial", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_VAR, YEXP__R_NONE, "unless", NULL, NULL }, \
    { YEXP__S_NODE, 0xffff, YEXP__T_SET, 0, "vars", "{}", NULL }, \
    { YEXP__S_LOOP, 0xffff, YEXP__T_INT, YEXP__R_REQ, "max", NULL, NULL }, \
    { YEXP__S_LOOP, 0xffff, YEXP__T_NUM, YEXP__R_REQ, "until_accuracy", NULL, NULL },

/* The format's keys and defaults, version 1 (D2). Sorted by section, then key.
 * The stimulus, method and step rows came from the parameter tables of the
 * headers as they stood when version 1 was fixed; a later change to a header
 * does not change a row, since these are the format's defaults. */
static const yexp_row yexp__rows[] = {
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "devices", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_OBJ, 0, "display", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_STR, YEXP__R_REQ, "format", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "methods", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_NAMEZ, 0, "name", "\"\"", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_OBJ, 0, "session", "{\"fields\":{\"participant\":{\"required\":true,\"type\":\"integer\"}}}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "stimuli", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_LIST, YEXP__R_REQ, "timeline", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, YEXP__R_REQ, "trials", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_INT, YEXP__R_REQ, "version", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_OBJ, 0, "web", "{}", NULL },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_VEC3, 0, "background", "[0.5,0.5,0.5]", NULL },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_ENUM, 0, "calibration", "\"optional\"", "required=0,optional=1" },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_INT, 0, "min_tier", "0", NULL },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_ENUM, 0, "units", "\"px\"", "px=0,deg=1" },
    YEXP__ROWS_SESSION_ROLE
    YEXP__ROWS_STIM_A
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "visible", "1", NULL },
    YEXP__ROWS_STIM_B
    YEXP__ROWS_METHODS
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "after", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_SET, 0, "columns", "{}", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "events", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_VAL, YEXP__R_SELF, "method_value", "\"$level\"", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_VAL, YEXP__R_SELF, "outcome", "\"correct\"", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_NAMES, YEXP__R_NONE | YEXP__R_SELF, "record", NULL, NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "response", NULL, NULL },
    YEXP__ROWS_RESPONSE_LOOP
    { YEXP__S_STEP, 0x0008, YEXP__T_DUR, YEXP__R_REQ, "at_s", NULL, NULL },
    { YEXP__S_STEP, 0x0060, YEXP__T_DUR, 0, "delay_s", "0", NULL },
    { YEXP__S_STEP, 0x0060, YEXP__T_DUR, 0, "duration_s", "0", NULL },
    { YEXP__S_STEP, 0x0060, YEXP__T_ENUM, 0, "ease", "\"linear\"", "linear=0,step=1,cosine=2,quad_in=3,quad_out=4,log=5,bezier=6,quad_in_out=7,cubic_in=8,cubic_out=9,cubic_in_out=10" },
    { YEXP__S_STEP, 0x0200, YEXP__T_VAR, YEXP__R_REQ, "if", NULL, NULL },
    { YEXP__S_STEP, 0x0100, YEXP__T_STR, YEXP__R_REQ, "mark", NULL, NULL },
    { YEXP__S_STEP, 0x0002, YEXP__T_TARGETS, YEXP__R_REQ, "off", NULL, NULL },
    { YEXP__S_STEP, 0x0001, YEXP__T_TARGETS, YEXP__R_REQ, "on", NULL, NULL },
    { YEXP__S_STEP, 0x0070, YEXP__T_OBJ, YEXP__R_REQ, "props", NULL, NULL },
    { YEXP__S_STEP, 0x0080, YEXP__T_NAME, YEXP__R_REQ, "role", NULL, NULL },
    { YEXP__S_STEP, 0x0010, YEXP__T_NAME, YEXP__R_REQ, "set", NULL, NULL },
    { YEXP__S_STEP, 0x0003, YEXP__T_DUR, YEXP__R_SELF, "stagger_s", "0", NULL },
    { YEXP__S_STEP, 0x0600, YEXP__T_LIST, YEXP__R_REQ, "steps", NULL, NULL },
    { YEXP__S_STEP, 0x0040, YEXP__T_NAME, YEXP__R_REQ, "then", NULL, NULL },
    { YEXP__S_STEP, 0x0020, YEXP__T_NAME, YEXP__R_REQ, "to", NULL, NULL },
    { YEXP__S_STEP, 0x0080, YEXP__T_CODE, YEXP__R_REQ, "trigger", NULL, NULL },
    { YEXP__S_STEP, 0x0400, YEXP__T_VAR, YEXP__R_REQ, "unless", NULL, NULL },
    { YEXP__S_STEP, 0x0004, YEXP__T_DUR, YEXP__R_REQ, "wait_s", NULL, NULL },
};
#define YEXP__N_ROWS ((int)(sizeof yexp__rows / sizeof yexp__rows[0]))

/* The format's keys and defaults, version 2 (format 3.17, D41): version
 * 1's rows with the changes of part 3 (a stimulus's `visible` is a step's
 * property only; display.units has "cm"; the template's outcome has a
 * second default, 1, for a template with states), then the sections that
 * version 2 adds. Sorted by section, then key. */
static const yexp_row yexp__rows2[] = {
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "controls", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "devices", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_OBJ, 0, "display", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_STR, YEXP__R_REQ, "format", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "methods", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_NAMEZ, 0, "name", "\"\"", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "roles", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_OBJ, 0, "session", "{\"fields\":{\"participant\":{\"required\":true,\"type\":\"integer\"}}}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, 0, "stimuli", "{}", NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_LIST, YEXP__R_REQ, "timeline", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_SET, YEXP__R_REQ, "trials", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_INT, YEXP__R_REQ, "version", NULL, NULL },
    { YEXP__S_ROOT, 0xffff, YEXP__T_OBJ, 0, "web", "{}", NULL },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_VEC3, 0, "background", "[0.5,0.5,0.5]", NULL },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_ENUM, 0, "calibration", "\"optional\"", "required=0,optional=1" },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_INT, 0, "min_tier", "0", NULL },
    { YEXP__S_DISPLAY, 0xffff, YEXP__T_ENUM, 0, "units", "\"px\"", "px=0,deg=1,cm=2" },
    YEXP__ROWS_SESSION_ROLE
    YEXP__ROWS_STIM_A
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, YEXP__R_STEP, "visible", "1", NULL },
    YEXP__ROWS_STIM_B
    YEXP__ROWS_METHODS
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "after", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_SET, 0, "columns", "{}", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_SET, 0, "controls", "{}", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "events", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "exits", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_NAMES, 0, "flags", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_VAL, YEXP__R_SELF, "method_value", "\"$level\"", NULL },
    { YEXP__S_TEMPLATE, 0x0001, YEXP__T_VAL, YEXP__R_SELF, "outcome", "\"correct\"", NULL },
    { YEXP__S_TEMPLATE, 0x0002, YEXP__T_VAL, YEXP__R_SELF, "outcome", "1", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_NAMES, YEXP__R_NONE | YEXP__R_SELF, "record", NULL, NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "requeue_outcomes", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "response", NULL, NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, YEXP__R_NONE, "states", NULL, NULL },
    YEXP__ROWS_RESPONSE_LOOP
    { YEXP__S_STEP, 0x0008, YEXP__T_DUR, YEXP__R_REQ, "at_s", NULL, NULL },
    { YEXP__S_STEP, 0x0060, YEXP__T_DUR, 0, "delay_s", "0", NULL },
    { YEXP__S_STEP, 0x0060, YEXP__T_DUR, 0, "duration_s", "0", NULL },
    { YEXP__S_STEP, 0x0060, YEXP__T_ENUM, 0, "ease", "\"linear\"", "linear=0,step=1,cosine=2,quad_in=3,quad_out=4,log=5,bezier=6,quad_in_out=7,cubic_in=8,cubic_out=9,cubic_in_out=10" },
    { YEXP__S_STEP, 0x2000, YEXP__T_NAME, YEXP__R_REQ, "flag", NULL, NULL },
    { YEXP__S_STEP, 0x0200, YEXP__T_VAR, YEXP__R_REQ, "if", NULL, NULL },
    { YEXP__S_STEP, 0x0100, YEXP__T_STR, YEXP__R_REQ, "mark", NULL, NULL },
    { YEXP__S_STEP, 0x0002, YEXP__T_TARGETS, YEXP__R_REQ, "off", NULL, NULL },
    { YEXP__S_STEP, 0x0001, YEXP__T_TARGETS, YEXP__R_REQ, "on", NULL, NULL },
    { YEXP__S_STEP, 0x0070, YEXP__T_OBJ, YEXP__R_REQ, "props", NULL, NULL },
    { YEXP__S_STEP, 0x1000, YEXP__T_NAME, YEXP__R_REQ, "recenter", NULL, NULL },
    { YEXP__S_STEP, 0x0080, YEXP__T_NAME, YEXP__R_REQ, "role", NULL, NULL },
    { YEXP__S_STEP, 0x0010, YEXP__T_NAME, YEXP__R_REQ, "set", NULL, NULL },
    { YEXP__S_STEP, 0x0003, YEXP__T_DUR, YEXP__R_SELF, "stagger_s", "0", NULL },
    { YEXP__S_STEP, 0x0600, YEXP__T_LIST, YEXP__R_REQ, "steps", NULL, NULL },
    { YEXP__S_STEP, 0x0040, YEXP__T_NAME, YEXP__R_REQ, "then", NULL, NULL },
    { YEXP__S_STEP, 0x0800, YEXP__T_DUR, YEXP__R_NONE, "timeout_s", NULL, NULL },
    { YEXP__S_STEP, 0x0020, YEXP__T_NAME, YEXP__R_REQ, "to", NULL, NULL },
    { YEXP__S_STEP, 0x1000, YEXP__T_POINT, YEXP__R_REQ, "to", NULL, NULL },
    { YEXP__S_STEP, 0x0080, YEXP__T_CODE, YEXP__R_REQ, "trigger", NULL, NULL },
    { YEXP__S_STEP, 0x0400, YEXP__T_VAR, YEXP__R_REQ, "unless", NULL, NULL },
    { YEXP__S_STEP, 0x0800, YEXP__T_NAME, YEXP__R_REQ, "until", NULL, NULL },
    { YEXP__S_STEP, 0x2000, YEXP__T_BOOL, YEXP__R_REQ, "value", NULL, NULL },
    { YEXP__S_STEP, 0x0004, YEXP__T_DUR, YEXP__R_REQ, "wait_s", NULL, NULL },
    { YEXP__S_SIGROLE, 0xffff, YEXP__T_BOOL, 0, "allow_relative", "false", NULL },
    { YEXP__S_SIGROLE, 0xffff, YEXP__T_BOOL, 0, "required", "true", NULL },
    { YEXP__S_SIGROLE, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "position2d=0,position2d_relative=1,position3d=2,scalar=3,gaze=4" },
    { YEXP__S_SIGROLE, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "units", NULL, "display_px=0,display_cm=1,display_deg=2,workspace_cm=3,device=4" },
    { YEXP__S_CONTROL, 0x0001, YEXP__T_POINT, 0, "about", "[0,0]", NULL },
    { YEXP__S_CONTROL, 0x0080, YEXP__T_DUR, YEXP__R_REQ, "after_s", NULL, NULL },
    { YEXP__S_CONTROL, 0x0002, YEXP__T_CENTER, 0, "center", "[0,0]", NULL },
    { YEXP__S_CONTROL, 0x0004, YEXP__T_POINT, 0, "center", "[0,0]", NULL },
    { YEXP__S_CONTROL, 0x00ff, YEXP__T_ENUM, YEXP__R_REQ, "control", NULL, "cursor=0,region=1,distance=2,movement=3,peak=4,moving_target=5,trace=6,signal_lost=7" },
    { YEXP__S_CONTROL, 0x0011, YEXP__T_NAME, YEXP__R_NONE, "freeze_at", NULL, NULL },
    { YEXP__S_CONTROL, 0x0078, YEXP__T_NAME, YEXP__R_NONE, "from", NULL, NULL },
    { YEXP__S_CONTROL, 0x0010, YEXP__T_DUR, 0, "lag_s", "0.01", NULL },
    { YEXP__S_CONTROL, 0x0001, YEXP__T_NAMES, YEXP__R_NONE, "mean", NULL, NULL },
    { YEXP__S_CONTROL, 0x0010, YEXP__T_ENUM, 0, "measure", "\"speed\"", "speed=0" },
    { YEXP__S_CONTROL, 0x0040, YEXP__T_REFS, YEXP__R_REQ, "of", NULL, NULL },
    { YEXP__S_CONTROL, 0x009e, YEXP__T_REF, YEXP__R_REQ, "of", NULL, NULL },
    { YEXP__S_CONTROL, 0x0008, YEXP__T_NUM, YEXP__R_REQ, "onset", NULL, NULL },
    { YEXP__S_CONTROL, 0x0008, YEXP__T_POINT, YEXP__R_NONE, "origin", NULL, NULL },
    { YEXP__S_CONTROL, 0x0002, YEXP__T_BOOL, 0, "outside", "false", NULL },
    { YEXP__S_CONTROL, 0x0002, YEXP__T_NUM, YEXP__R_REQ, "radius", NULL, NULL },
    { YEXP__S_CONTROL, 0x0020, YEXP__T_DUR, 0, "ramp_s", "0", NULL },
    { YEXP__S_CONTROL, 0x0042, YEXP__T_ENUM, 0, "rate", "\"sample\"", "sample=0,frame=1" },
    { YEXP__S_CONTROL, 0x0001, YEXP__T_NAME, YEXP__R_NONE, "role", NULL, NULL },
    { YEXP__S_CONTROL, 0x0004, YEXP__T_NUM, 0, "scale", "1", NULL },
    { YEXP__S_CONTROL, 0x0002, YEXP__T_NUM, YEXP__R_NONE, "speed_below", NULL, NULL },
    { YEXP__S_CONTROL, 0x0002, YEXP__T_DUR, 0, "speed_lag_s", "0.01", NULL },
    { YEXP__S_CONTROL, 0x0002, YEXP__T_DUR, 0, "stay_s", "0", NULL },
    { YEXP__S_CONTROL, 0x0021, YEXP__T_NAME, YEXP__R_NONE, "stimulus", NULL, NULL },
    { YEXP__S_CONTROL, 0x0040, YEXP__T_NAME, 0, "to", "\"end\"", NULL },
    { YEXP__S_CONTROL, 0x0001, YEXP__T_TRANSFORM, 0, "transform", "\"identity\"", NULL },
    { YEXP__S_CONTROL, 0x0001, YEXP__T_REF, YEXP__R_NONE, "x", NULL, NULL },
    { YEXP__S_CONTROL, 0x0020, YEXP__T_SINES, YEXP__R_REQ, "x", NULL, NULL },
    { YEXP__S_CONTROL, 0x0001, YEXP__T_REF, YEXP__R_NONE, "y", NULL, NULL },
    { YEXP__S_CONTROL, 0x0020, YEXP__T_SINES, YEXP__R_REQ, "y", NULL, NULL },
    { YEXP__S_STATE, 0xffff, YEXP__T_LIST, 0, "exits", "[]", NULL },
    { YEXP__S_STATE, 0xffff, YEXP__T_INT, YEXP__R_NONE | YEXP__R_LIT, "max_visits", NULL, NULL },
    { YEXP__S_STATE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "name", NULL, NULL },
    { YEXP__S_STATE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "over", NULL, NULL },
    { YEXP__S_STATE, 0xffff, YEXP__T_LIST, 0, "steps", "[]", NULL },
    { YEXP__S_OVER, 0xffff, YEXP__T_VAL, YEXP__R_NONE, "outcome", NULL, NULL },
    { YEXP__S_OVER, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "to", NULL, NULL },
    { YEXP__S_EXIT, 0x0003, YEXP__T_DUR, YEXP__R_NONE, "after_s", NULL, NULL },
    { YEXP__S_EXIT, 0x0003, YEXP__T_VAR, YEXP__R_NONE, "if", NULL, NULL },
    { YEXP__S_EXIT, 0x0002, YEXP__T_NAMES, YEXP__R_REQ, "in", NULL, NULL },
    { YEXP__S_EXIT, 0x0003, YEXP__T_NAME, YEXP__R_NONE, "on", NULL, NULL },
    { YEXP__S_EXIT, 0x0003, YEXP__T_VAL, YEXP__R_NONE, "outcome", NULL, NULL },
    { YEXP__S_EXIT, 0x0003, YEXP__T_NAME, YEXP__R_REQ, "to", NULL, NULL },
    { YEXP__S_SRESPONSE, 0xffff, YEXP__T_LIST, YEXP__R_REQ, "choices", NULL, NULL },
    { YEXP__S_SRESPONSE, 0xffff, YEXP__T_DUR, 0, "duration_s", "0", NULL },
    { YEXP__S_SRESPONSE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "from", NULL, NULL },
    { YEXP__S_SRESPONSE, 0xffff, YEXP__T_INT, YEXP__R_LIT, "max_responses", "0", NULL },
    { YEXP__S_SRESPONSE, 0xffff, YEXP__T_DUR, 0, "minimum_valid_rt_s", "0", NULL },
    { YEXP__S_SRESPONSE, 0xffff, YEXP__T_BOOL, 0, "response_ends_trial", "false", NULL },
    { YEXP__S_UNIFORM, 0xffff, YEXP__T_INT, YEXP__R_LIT, "n", "1", NULL },
    { YEXP__S_UNIFORM, 0xffff, YEXP__T_ENUM, 0, "per", "\"node\"", "node=0" },
    { YEXP__S_UNIFORM, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "uniform", NULL, NULL },
    { YEXP__S_WALK2D, 0xffff, YEXP__T_BOX, YEXP__R_REQ, "box", NULL, NULL },
    { YEXP__S_WALK2D, 0xffff, YEXP__T_POINT, YEXP__R_LIT, "start", "[0,0]", NULL },
    { YEXP__S_WALK2D, 0xffff, YEXP__T_NUM, YEXP__R_REQ | YEXP__R_LIT, "step", NULL, NULL },
};
#define YEXP__N_ROWS2 ((int)(sizeof yexp__rows2 / sizeof yexp__rows2[0]))

static const char* const yexp__section_names[YEXP__S_COUNT] = {
    "file", "display", "session", "session field", "web", "role", "stimulus", "stair method",
    "quest method", "aep method", "template", "response", "response trigger", "choice", "node",
    "loop", "step", "signal role", "control", "state", "over", "exit", "response with signal choices",
    "uniform", "walk2d"
};

YEXP_API const yexp_row* yexp_defaults(int version, int* n) {
    if (version == 1) { if (n) *n = YEXP__N_ROWS; return yexp__rows; }
    if (version == 2) { if (n) *n = YEXP__N_ROWS2; return yexp__rows2; }
    if (n) *n = 0;
    return NULL;
}

YEXP_API const char* yexp_section_name(int section) {
    return section >= 0 && section < YEXP__S_COUNT ? yexp__section_names[section] : "";
}

YEXP_API bool yexp_row_required(const yexp_row* r) { return r && (r->flags & YEXP__R_REQ) != 0; }


/* --- work space --------------------------------------------------------- */

#define YEXP__MAX_NAME     63
#define YEXP__MAX_SET      1024
#define YEXP__MAX_LIST     4096
#define YEXP__MAX_CHILDREN 256
#define YEXP__MAX_DEPTH    32
#define YEXP__SLOT_STACK   2048
#ifndef YEXP_WORK_STEPS
#define YEXP_WORK_STEPS    5000000L   /* a fuzz target lowers it, to stay quick */
#endif
#define YEXP__MAX_LEVELS   65536
#define YEXP__VAR_CHAIN    64      /* one variable naming another, deep */
#define YEXP__LEVEL_BYTES  65536   /* the longest level asked for again */

#define YEXP__BAD_ROLES   1u
#define YEXP__BAD_STIMULI 2u
#define YEXP__BAD_METHODS 4u
#define YEXP__BAD_TRIALS  8u
#define YEXP__BAD_SIGROLES 16u
#define YEXP__BAD_CONTROLS 32u

/* A step that gives x or y of a stimulus: checked against the controls'
 * stimuli once the template's names are all known (3.7, finding F12). */
typedef struct yexp__xy { int stim; uint32_t pos; const char* key; char elem[128]; struct yexp__xy* next; } yexp__xy;

/* A name and its index, sorted for a search. */
typedef struct yexp__keyed { const char* key; int index; } yexp__keyed;

/* A key's state while one object is read. */
typedef struct yexp__slot {
    const yjs_value*  src;
    const yexp_value* v;
    uint32_t          pos;
    bool              given;
} yexp__slot;

/* A lookup and where it is, for the warnings after the second pass. */
typedef struct yexp__lookup {
    yexp_value*          v;
    char                 elem[96];
    char                 key[64];
    struct yexp__lookup* next;
} yexp__lookup;

/* A gamut value and where it is: checked once the stimuli are all read. */
typedef struct yexp__gamut {
    yexp_value*         v;
    char                elem[96];
    char                key[64];
    struct yexp__gamut* next;
} yexp__gamut;

typedef struct yexp__ctx {
    yjs_arena*         a;
    const char*        text;
    size_t             len;
    yexp_report*       r;
    const yexp_host*   h;
    yexp_def*          d;
    int                first[YEXP__S_COUNT];
    int                count[YEXP__S_COUNT];
    const yexp_value*  defs[YEXP__N_ROWS2];
    const yjs_value*   cdefs[YEXP__N_ROWS2];
    yexp__slot*        slots;
    int                top;
    bool               nomem;
    yexp__lookup*      lookups;
    yexp__lookup*      lookups_tail;
    yexp__gamut*       gamuts;
    /* the second pass */
    long               steps;
    bool               over;
    const char**       lv;
    int                n_lv, c_lv;
    uint8_t*           stim_use;
    uint8_t*           stim_seen;     /* shown by some leaf                  */
    uint8_t*           tmpl_seen;     /* run by some leaf                    */
    bool               lv_over;       /* the level list was cut short        */
    int*               used;          /* the stimuli this leaf marked        */
    int                n_used;
    uint32_t*          stim_stamp;    /* seen in the list numbered gen       */
    uint32_t*          method_stamp;
    uint32_t           gen;
    unsigned           bad_sets;      /* a set refused whole: no name of it is dangling */
    /* the table of the file's version */
    const yexp_row*    rows;
    int                n_rows;
    int                version;
    /* version 2: the template being read, for the names of 3.9 */
    yexp_template*     tp;
    const yjs_value*   trials_src;    /* the templates' names                */
    yexp__keyed*     st_idx;        /* its states by name: key, index      */
    int                n_st;
    yexp__keyed*     fl_idx;        /* its flags by name                   */
    int                n_fl;
    bool               t_response;    /* it has a response                   */
    bool               t_states;      /* it has states                       */
    int                place_lanes;   /* lanes of the place a value form fills */
    unsigned           extra_allow;   /* value forms a set step's props take */
    int                step_place;    /* 0 events, 1 after, 2 a state's steps */
    int                until_seen;    /* an until came before in this list   */
    struct yexp__xy*   xy;            /* steps that give x or y of a stimulus */
    uint8_t*           mv_named;      /* top-level movements a response names */
} yexp__ctx;

static bool yexp__streq(const char* a, const char* b) { return strcmp(a, b) == 0; }

static bool yexp__ends_s(const char* s) {
    size_t n = strlen(s);
    return n >= 2 && s[n - 2] == '_' && s[n - 1] == 's';
}

static void yexp__flat(char* s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20) *s = '?';
}

static void yexp__diagv(yexp__ctx* c, int sev, uint32_t pos, const char* fmt, va_list ap) {
    yexp_report* r = c->r;
    yexp_diag* d;
    if (sev == YEXP_ERROR) r->n_errors++;
    else r->n_warnings++;
    if (r->n_diags >= YEXP_MAX_DIAGS) return;
    d = &r->diags[r->n_diags++];
    d->severity = sev;
    d->line = d->column = 0;
    if (pos != UINT32_MAX) yjs_line_col(c->text, c->len, pos, &d->line, &d->column);
    vsnprintf(d->msg, sizeof d->msg, fmt, ap);
    yexp__flat(d->msg);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
static void yexp__err(yexp__ctx* c, uint32_t pos, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    yexp__diagv(c, YEXP_ERROR, pos, fmt, ap);
    va_end(ap);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
static void yexp__warn(yexp__ctx* c, uint32_t pos, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    yexp__diagv(c, YEXP_WARNING, pos, fmt, ap);
    va_end(ap);
}

/* Both passes share one limit: a hostile file must not buy quadratic time. */
static bool yexp__work(yexp__ctx* c) {
    if (c->over) return false;
    if (++c->steps <= YEXP_WORK_STEPS) return true;
    c->over = true;
    yexp__err(c, UINT32_MAX, "the definition takes more than %ld steps to check; do many leaves share large templates, or do 'vars' choices refer to each other?", (long)YEXP_WORK_STEPS);
    return false;
}

static void* yexp__alloc(yexp__ctx* c, size_t n) {
    void* p;
    if (c->nomem) return NULL;
    p = yjs_alloc(c->a, n ? n : 1);
    if (!p) c->nomem = true;
    return p;
}

/* The table of the file's version: its sections' first rows and counts. */
static void yexp__use_rows(yexp__ctx* c, int version) {
    int i, last = -1;
    c->version = version;
    c->rows = version >= 2 ? yexp__rows2 : yexp__rows;
    c->n_rows = version >= 2 ? YEXP__N_ROWS2 : YEXP__N_ROWS;
    memset(c->first, 0, sizeof c->first);
    memset(c->count, 0, sizeof c->count);
    for (i = 0; i < c->n_rows; i++) {
        int sect = c->rows[i].section;
        if (sect != last) { c->first[sect] = i; last = sect; }
        c->count[sect]++;
    }
}

static yexp_value* yexp__vnew(yexp__ctx* c, int kind, uint32_t pos) {
    yexp_value* v = (yexp_value*)yexp__alloc(c, sizeof(yexp_value));
    if (v) { v->kind = (uint8_t)kind; v->pos = pos; }
    return v;
}

/* Name rule of format 2.1. */
static bool yexp__name_ok(const char* s) {
    size_t i;
    if (!s || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
    for (i = 1; s[i]; i++) {
        char ch = s[i];
        if (i >= YEXP__MAX_NAME) return false;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}

static const char* const yexp__builtins[] = {
    "trial_index", "block", "rep", "condition", "practice", "warmup", "first_in_block", "after_break",
    "trial_seed", "method", "level", "estimate", "response", "rt_s", "responded", "correct", "outcome",
    "contrast_shown", NULL
};

static bool yexp__is_builtin(const char* s) {
    int i;
    for (i = 0; yexp__builtins[i]; i++)
        if (yexp__streq(s, yexp__builtins[i])) return true;
    return false;
}

/* A key of a later format version: named, since it is a version question
 * and not a typo. */
static bool yexp__is_v2(const char* key) {
    static const char* const v2[] = { "until", "states", "exits", "flags", "flag", "recenter", "controls",
                                      "freeze_at", "max_visits", "signal", "when", "signals", NULL };
    int i;
    for (i = 0; v2[i]; i++)
        if (yexp__streq(key, v2[i])) return true;
    return false;
}

/* --- rows and defaults -------------------------------------------------- */

static int yexp__find(const yexp__ctx* c, int sect, unsigned mask, const char* key) {
    int i, n = c->count[sect], f = c->first[sect];
    for (i = 0; i < n; i++) {
        const yexp_row* r = &c->rows[f + i];
        if ((r->kinds & mask) && yexp__streq(r->key, key)) return f + i;
    }
    return -1;
}

static const yexp_value* yexp__literal_from_json(yexp__ctx* c, const yexp_row* row, const yjs_value* j);

/* A row's default as a value, parsed once per load. */
static const yexp_value* yexp__default(yexp__ctx* c, int ri) {
    const yexp_row* r = &c->rows[ri];
    yjs_value* j;
    yjs_error e;
    if (c->defs[ri]) return c->defs[ri];
    if (!r->def) return NULL;
    if (yjs_parse(c->a, r->def, strlen(r->def), NULL, &j, &e) != YJS_OK) {
        if (e.code == YJS_ERR_NOMEM) c->nomem = true;
        return NULL;
    }
    c->defs[ri] = yexp__literal_from_json(c, r, j);
    return c->defs[ri];
}

static const yexp_value* yexp__literal_from_json(yexp__ctx* c, const yexp_row* row, const yjs_value* j) {
    yexp_value* v = yexp__vnew(c, YEXP_V_NONE, UINT32_MAX);
    size_t i;
    if (!v) return NULL;
    switch (j->type) {
    case YJS_TRUE: v->kind = YEXP_V_BOOL; v->i = 1; break;
    case YJS_FALSE: v->kind = YEXP_V_BOOL; v->i = 0; break;
    case YJS_NUMBER: {
        double x = 0;
        yjs_double(j, &x);
        v->kind = YEXP_V_NUMBER;
        v->num = x;
        if (row->type == YEXP__T_DUR) { v->flags = YEXP_VF_DURATION; yjs_fixed(j, 9, &v->i); }
        else if (j->flags & YJS_NUM_I64) { v->flags = YEXP_VF_INT; v->i = j->i; }
        break;
    }
    case YJS_STRING:
        if (j->u.s[0] == '$') { v->kind = YEXP_V_VAR; v->str = j->u.s + 1; }
        else { v->kind = YEXP_V_STRING; v->str = j->u.s; }
        break;
    case YJS_ARRAY: {
        yexp_value* it = (yexp_value*)yexp__alloc(c, (j->n ? j->n : 1) * sizeof(yexp_value));
        if (!it) return NULL;
        for (i = 0; i < j->n; i++) {
            const yexp_value* e = yexp__literal_from_json(c, row, j->u.items[i]);
            if (!e) return NULL;
            it[i] = *e;
        }
        v->kind = YEXP_V_ARRAY;
        v->items = it;
        v->n = j->n;
        break;
    }
    default: break;   /* objects: a composite row has no value */
    }
    return v;
}

static bool yexp__same(const yexp_value* a, const yexp_value* b) {
    uint32_t i;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case YEXP_V_BOOL: return a->i == b->i;
    case YEXP_V_NUMBER:
        if ((a->flags & YEXP_VF_DURATION) || (b->flags & YEXP_VF_DURATION)) return a->i == b->i;
        return a->num == b->num;
    case YEXP_V_STRING: case YEXP_V_VAR: return yexp__streq(a->str, b->str);
    case YEXP_V_ARRAY:
        if (a->n != b->n) return false;
        for (i = 0; i < a->n; i++)
            if (!yexp__same(&a->items[i], &b->items[i])) return false;
        return true;
    default: return false;
    }
}

/* Two trees of canonical values: the composite rows' defaults. */
static bool yexp__json_same(const yjs_value* a, const yjs_value* b) {
    size_t i;
    if (a->type != b->type || a->n != b->n) return a->type == b->type && a->type < YJS_NUMBER;
    switch (a->type) {
    case YJS_NULL: case YJS_TRUE: case YJS_FALSE: return true;
    case YJS_NUMBER: case YJS_STRING: return memcmp(a->u.s, b->u.s, a->n) == 0;
    case YJS_ARRAY:
        for (i = 0; i < a->n; i++)
            if (!yexp__json_same(a->u.items[i], b->u.items[i])) return false;
        return true;
    default:
        for (i = 0; i < a->n; i++)
            if (a->u.members[i].key_len != b->u.members[i].key_len ||
                memcmp(a->u.members[i].key, b->u.members[i].key, a->u.members[i].key_len) != 0 ||
                !yexp__json_same(a->u.members[i].value, b->u.members[i].value))
                return false;
        return true;
    }
}

/* Whether a composite's canonical value is its row's default. */
static bool yexp__at_default(yexp__ctx* c, int ri, const yjs_value* canon) {
    const yexp_row* r = &c->rows[ri];
    yjs_value* j;
    yjs_error e;
    if (!r->def) return false;
    if (!c->cdefs[ri]) {
        if (yjs_parse(c->a, r->def, strlen(r->def), NULL, &j, &e) != YJS_OK) { c->nomem = c->nomem || e.code == YJS_ERR_NOMEM; return false; }
        c->cdefs[ri] = j;
    }
    return yexp__json_same(canon, c->cdefs[ri]);
}

static void yexp__put(yexp__ctx* c, yjs_value* obj, const char* key, yjs_value* v) {
    if (!obj || !v || c->nomem) return;
    if (yjs_set(c->a, obj, key, v) != YJS_OK) c->nomem = true;
}

static yjs_value* yexp__cnew(yexp__ctx* c, int type) {
    yjs_value* v;
    if (c->nomem) return NULL;
    v = yjs_new(c->a, type);
    if (!v) c->nomem = true;
    return v;
}

static yjs_value* yexp__cstr(yexp__ctx* c, const char* s) {
    yjs_value* v;
    if (c->nomem) return NULL;
    v = yjs_new_stringz(c->a, s);
    if (!v) c->nomem = true;
    return v;
}

static void yexp__cpush(yexp__ctx* c, yjs_value* arr, yjs_value* v) {
    if (!arr || !v || c->nomem) return;
    if (yjs_push(c->a, arr, v) != YJS_OK) c->nomem = true;
}

/* D1: a note is text. The one check, for every object that can hold one. */
static bool yexp__note_ok(yexp__ctx* c, const yjs_value* obj, const char* elem) {
    const yjs_member* m = yjs_member_n(obj, "note", 4);
    if (m && m->value->type != YJS_STRING) {
        yexp__err(c, m->value->pos, "%s: 'note' is a string", elem);
        return false;
    }
    return true;
}

/* --- names (version 2, format 3.9) -------------------------------------- */

static const char* const yexp__ctl_kinds[] = { "cursor", "region", "distance", "movement", "peak", "moving_target", "trace", "signal_lost" };

/* What each kind of control gives (3.7): lanes of its value, a test, hits. */
static void yexp__ctl_gives(yexp_control_kind k, int* lanes, bool* test, bool* hits) {
    static const int L[] = { 2, 1, 1, 0, 1, 2, 0, 1 };
    *lanes = L[(int)k];
    *test = k == YEXP_REGION || k == YEXP_SIGNAL_LOST;
    *hits = *test;
}

static int yexp__kfind(const yexp__keyed* k, int n, const char* name) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(k[mid].key, name);
        if (cmp == 0) return k[mid].index;
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

static const yexp_control* yexp__ctl_find(const yexp_control* v, int n, const char* name) {
    int lo = 0, hi = n;
    if (!v) return NULL;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(v[mid].name, name);
        if (cmp == 0) return &v[mid];
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

YEXP_API const yexp_sigrole* yexp_sigrole_get(const yexp_def* d, const char* name) {
    int lo = 0, hi;
    if (!d || !name || !d->sigroles) return NULL;
    hi = d->n_sigroles;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(d->sigroles[mid].name, name);
        if (cmp == 0) return &d->sigroles[mid];
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

YEXP_API const yexp_control* yexp_control_get(const yexp_def* d, const yexp_template* t, const char* name) {
    const yexp_control* x = NULL;
    if (!d || !name) return NULL;
    if (t) x = yexp__ctl_find(t->controls, t->n_controls, name);
    return x ? x : yexp__ctl_find(d->controls, d->n_controls, name);
}

/* A name with an optional lane, "hand" or "hand.x", into r (unresolved).
 * false after a diagnostic. */
static bool yexp__ref_text(yexp__ctx* c, const char* text, uint32_t pos, bool lane_ok, yexp_ref* r, const char* elem, const char* key) {
    const char* dot = strchr(text, '.');
    memset(r, 0, sizeof *r);
    r->index = -1;
    r->lane = -1;
    r->pos_ = pos;
    if (dot) {
        size_t n = (size_t)(dot - text);
        char* nm;
        if (!lane_ok) {
            yexp__err(c, pos, "%s: '%s': '%s': a lane goes only where a value is read (3.9); this key takes a name", elem, key, text);
            return false;
        }
        if (!dot[1] || dot[2] || !strchr("xyzw", dot[1])) {
            yexp__err(c, pos, "%s: '%s': '%s': a lane is .x, .y, .z or .w (3.9)", elem, key, text);
            return false;
        }
        nm = (char*)yexp__alloc(c, n + 1);
        if (!nm) return false;
        memcpy(nm, text, n);
        nm[n] = 0;
        if (!yexp__name_ok(nm)) {
            yexp__err(c, pos, "%s: '%s': '%s': a name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes", elem, key, text);
            return false;
        }
        r->name = nm;
        r->lane = dot[1] == 'x' ? 0 : dot[1] == 'y' ? 1 : dot[1] == 'z' ? 2 : 3;
    } else {
        if (!yexp__name_ok(text)) {
            yexp__err(c, pos, "%s: '%s': '%s': a name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes", elem, key, text);
            return false;
        }
        r->name = text;
    }
    return true;
}

/* What r->name is in the template being read, or, outside a template (a
 * top-level control), among the names every template has: signal roles,
 * stimuli and top-level controls (format finding F1). false: nothing has
 * the name. */
static bool yexp__resolve_name(yexp__ctx* c, yexp_ref* r) {
    const yexp_def* d = c->d;
    const char* nm = r->name;
    const yexp_control* ct;
    const yexp_stim* st;
    const yexp_sigrole* sr;
    int k;
    r->kind = YEXP_REF_NONE;
    r->index = -1;
    r->control = NULL;
    r->lanes = 0;
    if (c->tp) {
        ct = yexp__ctl_find(c->tp->controls, c->tp->n_controls, nm);
        if (ct) { r->kind = YEXP_REF_CONTROL; r->control = ct; return true; }
        if ((k = yexp__kfind(c->st_idx, c->n_st, nm)) >= 0) { r->kind = YEXP_REF_STATE; r->index = k; return true; }
        if ((k = yexp__kfind(c->fl_idx, c->n_fl, nm)) >= 0) { r->kind = YEXP_REF_FLAG; r->index = k; return true; }
        if (c->t_response && yexp__streq(nm, "response")) { r->kind = YEXP_REF_RESPONSE; return true; }
    }
    if (yexp__streq(nm, "end")) { r->kind = YEXP_REF_END; return true; }
    ct = yexp__ctl_find(d->controls, d->n_controls, nm);
    if (ct) { r->kind = YEXP_REF_CONTROL; r->control = ct; return true; }
    st = yexp_stim_get(d, nm);
    if (st) { r->kind = YEXP_REF_STIMULUS; r->index = (int)(st - d->stimuli); return true; }
    sr = yexp_sigrole_get(d, nm);
    if (sr) { r->kind = YEXP_REF_ROLE; r->index = (int)(sr - d->sigroles); return true; }
    return false;
}

/* What a key wants of a name (3.9). */
#define YEXP__N_VALUE    0x001u   /* a role or a control with a value        */
#define YEXP__N_TEST     0x002u   /* a control with a test                   */
#define YEXP__N_HITS     0x004u   /* a control with hits                     */
#define YEXP__N_RESPONSE 0x008u   /* `response` (with HITS or TIME)          */
#define YEXP__N_TIME     0x010u   /* a stimulus, a control with hits, a state, response */
#define YEXP__N_END      0x020u   /* `end`                                   */
#define YEXP__N_STATE    0x040u   /* a state                                 */
#define YEXP__N_STIM     0x080u   /* a stimulus                              */
#define YEXP__N_ROLE     0x100u   /* a signal role                           */

static const char* yexp__want_text(unsigned want) {
    if (want & YEXP__N_VALUE) return "a signal role or a control with a value";
    if (want & YEXP__N_TEST) return "a control with a test";
    if ((want & YEXP__N_HITS) && (want & YEXP__N_RESPONSE)) return "a control with hits, or response";
    if (want & YEXP__N_HITS) return "a control with hits";
    if ((want & YEXP__N_TIME) && (want & YEXP__N_END)) return "a stimulus, a control with hits, a state, response or end";
    if (want & YEXP__N_TIME) return "a stimulus, a control with hits, a state or response";
    if ((want & YEXP__N_STATE) && (want & YEXP__N_END)) return "a state or end";
    if (want & YEXP__N_STATE) return "a state";
    if (want & YEXP__N_STIM) return "a stimulus";
    if (want & YEXP__N_ROLE) return "a signal role";
    return "a name";
}

static void yexp__ref_what(const yexp_ref* r, char* out, size_t cap) {
    switch (r->kind) {
    case YEXP_REF_ROLE: snprintf(out, cap, "a signal role"); break;
    case YEXP_REF_CONTROL: snprintf(out, cap, "a %s control", yexp__ctl_kinds[(int)r->control->kind]); break;
    case YEXP_REF_STATE: snprintf(out, cap, "a state"); break;
    case YEXP_REF_STIMULUS: snprintf(out, cap, "a stimulus"); break;
    case YEXP_REF_FLAG: snprintf(out, cap, "a flag"); break;
    case YEXP_REF_RESPONSE: snprintf(out, cap, "the template's response"); break;
    case YEXP_REF_END: snprintf(out, cap, "the trial's end"); break;
    default: snprintf(out, cap, "nothing"); break;
    }
}

/* Resolves r and checks it against what the key wants. false after a
 * diagnostic, or quietly when the name is of an element refused already. */
static bool yexp__name_use(yexp__ctx* c, yexp_ref* r, unsigned want, const char* elem, const char* key) {
    char what[64];
    bool ok = false;
    if (!r->name) return false;
    if (!yexp__resolve_name(c, r)) {
        if (c->bad_sets & (YEXP__BAD_STIMULI | YEXP__BAD_SIGROLES | YEXP__BAD_CONTROLS)) return false;
        if (!c->tp)
            yexp__err(c, r->pos_, "%s: '%s': no signal role, stimulus or top-level control is named '%s'; a top-level control names only those (3.9)", elem, key, r->name);
        else if (yexp__streq(r->name, "response") && (want & YEXP__N_RESPONSE))
            yexp__err(c, r->pos_, "%s: '%s': 'response': the template has no response", elem, key);
        else
            yexp__err(c, r->pos_, "%s: '%s': nothing is named '%s'; this key takes %s (3.9)", elem, key, r->name, yexp__want_text(want));
        return false;
    }
    if (r->kind == YEXP_REF_CONTROL && r->control->failed_) return false;
    /* a template that names a top-level control has its copy (3.6, D25) */
    if (r->kind == YEXP_REF_CONTROL && r->control->shared && c->tp && c->tp->uses_top)
        c->tp->uses_top[r->control - c->d->controls] = 1;
    switch (r->kind) {
    case YEXP_REF_ROLE:
        ok = (want & (YEXP__N_VALUE | YEXP__N_ROLE)) != 0;
        if (ok) r->lanes = c->d->sigroles[r->index].lanes;
        break;
    case YEXP_REF_CONTROL: {
        const yexp_control* x = r->control;
        ok = ((want & YEXP__N_VALUE) && x->lanes > 0) || ((want & YEXP__N_TEST) && x->has_test) ||
             ((want & (YEXP__N_HITS | YEXP__N_TIME)) && x->has_hits);
        if (ok && (want & YEXP__N_VALUE)) r->lanes = x->lanes;
        if (!ok && x->kind == YEXP_MOVEMENT && (want & (YEXP__N_HITS | YEXP__N_TIME))) {
            yexp__err(c, r->pos_, "%s: '%s': '%s' is a movement; its hit is the response, so name 'response' (3.7)", elem, key, r->name);
            return false;
        }
        break;
    }
    case YEXP_REF_STATE: ok = (want & (YEXP__N_TIME | YEXP__N_STATE)) != 0; break;
    case YEXP_REF_STIMULUS: ok = (want & (YEXP__N_TIME | YEXP__N_STIM)) != 0; break;
    case YEXP_REF_RESPONSE: ok = (want & YEXP__N_RESPONSE) != 0; break;
    case YEXP_REF_END: ok = (want & YEXP__N_END) != 0; break;
    default: ok = false; break;
    }
    if (!ok) {
        if (r->kind == YEXP_REF_END && !(want & YEXP__N_END)) {
            yexp__err(c, r->pos_, "%s: '%s': 'end' is the trial's end, which goes only in a 'to'; this key takes %s (3.9)", elem, key, yexp__want_text(want));
            return false;
        }
        yexp__ref_what(r, what, sizeof what);
        yexp__err(c, r->pos_, "%s: '%s': '%s' is %s; this key takes %s (3.9)", elem, key, r->name, what, yexp__want_text(want));
        return false;
    }
    if (r->lane >= 0) {
        if (r->lane >= r->lanes) {
            yexp__err(c, r->pos_, "%s: '%s': '%s' has %d lane%s; '.%c' is not one of them (3.9)", elem, key, r->name, r->lanes, r->lanes == 1 ? "" : "s", "xyzw"[r->lane]);
            return false;
        }
        r->lanes = 1;
    }
    return true;
}

/* A name in a one-lane place: a name alone is refused when its value has
 * more than one lane (3.9). */
static bool yexp__one_lane(yexp__ctx* c, const yexp_ref* r, const char* elem, const char* key) {
    if (r->lanes > 1) {
        yexp__err(c, r->pos_, "%s: '%s': '%s' has %d lanes and this place takes one; name a lane, '%s.x' (3.9)", elem, key, r->name, r->lanes, r->name);
        return false;
    }
    return true;
}

/* --- values (format 2.2) ------------------------------------------------ */

#define YEXP__AV_NUM      0x001u
#define YEXP__AV_INT      0x002u   /* a number literal must be an integer */
#define YEXP__AV_DUR      0x004u   /* a number is a duration              */
#define YEXP__AV_STR      0x008u
#define YEXP__AV_BOOL     0x010u
#define YEXP__AV_ARRAY    0x020u
#define YEXP__AV_VAR      0x040u
#define YEXP__AV_LOOKUP   0x080u
#define YEXP__AV_LINSPACE 0x100u
#define YEXP__AV_GAMUT    0x200u
#define YEXP__AV_MICH     0x400u
#define YEXP__AV_CHOICE   0x800u
#define YEXP__AV_NONNEG   0x1000u  /* a number literal is at least 0         */
#define YEXP__AV_INT32    0x2000u  /* an integer literal fits 32 bits        */
#define YEXP__AV_VALUE    (YEXP__AV_NUM | YEXP__AV_STR | YEXP__AV_BOOL | YEXP__AV_ARRAY | YEXP__AV_VAR | YEXP__AV_LOOKUP)
/* format version 2 */
#define YEXP__AV_SIG       0x4000u   /* {"signal": N} (3.10)                   */
#define YEXP__AV_WHEN      0x8000u   /* {"when": T, "then": V, "else": V}      */
#define YEXP__AV_COL       0x10000u  /* {"hit": ...}, {"time": ...} (3.15)     */
#define YEXP__AV_TRANSFORM 0x20000u  /* {"rotation_deg": A}, {"mirror_deg": A} */
#define YEXP__AV_VARFORM   0x40000u  /* {"uniform": ...}, {"walk2d": ...} (3.14) */
/* a lookup's entries and a choice's values take none of the forms: one
 * form per place, so the lowering has one node per form */
#define YEXP__AV_NESTED    (YEXP__AV_SIG | YEXP__AV_WHEN | YEXP__AV_COL | YEXP__AV_VARFORM)

static yexp_value* yexp__value(yexp__ctx* c, const yjs_value* src, unsigned allow, unsigned iallow, const char* elem,
                               const char* key, yjs_value** canon);

static const char* yexp__wants(unsigned allow) {
    if (allow & YEXP__AV_DUR) return "a duration: a number of seconds, a \"$variable\" that holds seconds, or a lookup";
    if (allow & YEXP__AV_ARRAY) return "an array of numbers, a \"$variable\" or a lookup";
    if ((allow & YEXP__AV_INT) && (allow & YEXP__AV_VAR)) return "an integer, a \"$variable\" or a lookup";
    if (allow & YEXP__AV_INT) return "an integer";
    if ((allow & YEXP__AV_NUM) && (allow & YEXP__AV_VAR)) return "a number, a \"$variable\" or a lookup";
    if (allow & YEXP__AV_NUM) return "a number";
    if ((allow & YEXP__AV_STR) && (allow & YEXP__AV_VAR)) return "a string, a \"$variable\" or a lookup";
    if (allow & YEXP__AV_STR) return "a string";
    return "another type";
}

/* --- value forms of version 2 (format 3.10, 3.14, 3.15) -----------------
 *
 * These read a form's shape and its names' spelling only. Its names are
 * resolved by yexp__forms_resolve() once the template's controls exist. */

static yexp_value* yexp__signal_form(yexp__ctx* c, const yjs_value* src, bool test, const char* elem, const char* key, yjs_value** canon) {
    const yjs_value* nv = yjs_get(src, "signal");
    const yjs_value* lv = yjs_get(src, "above");
    const yjs_value* note = yjs_get(src, "note");
    yexp_value* v;
    yexp_ref r;
    yjs_value* cl = NULL;
    size_t i;
    for (i = 0; i < src->n; i++) {
        const char* k = src->u.members[i].key;
        if (yexp__streq(k, "above") && !test) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': 'above' goes only in a when's test, {\"signal\": NAME, \"above\": LEVEL} (3.10)", elem, key);
            return NULL;
        }
        if (!yexp__streq(k, "signal") && !yexp__streq(k, "above") && !yexp__streq(k, "note")) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': a signal form is {\"signal\": NAME}; '%s' is an unknown key", elem, key, k);
            return NULL;
        }
    }
    if (nv->type != YJS_STRING) {
        yexp__err(c, nv->pos, "%s: '%s': a signal form names a role or a control: {\"signal\": \"hand.x\"}", elem, key);
        return NULL;
    }
    if (!yexp__ref_text(c, nv->u.s, nv->pos, true, &r, elem, key)) return NULL;
    v = yexp__vnew(c, YEXP_V_SIGNAL, src->pos);
    if (!v) return NULL;
    v->str = r.name;
    v->i = r.lane;
    if (test) {
        if (!lv) {
            yexp__err(c, src->pos, "%s: '%s': a when's test is a control's name or {\"signal\": NAME, \"above\": LEVEL} (3.10)", elem, key);
            return NULL;
        }
        /* format finding F2: the level is a number, a variable or a lookup */
        v->by = yexp__value(c, lv, YEXP__AV_NUM | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, key, &cl);
        if (!v->by) return NULL;
    }
    if (canon) {
        *canon = yexp__cnew(c, YJS_OBJECT);
        yexp__put(c, *canon, "signal", yexp__cstr(c, nv->u.s));
        if (cl) yexp__put(c, *canon, "above", cl);
        if (note) yexp__put(c, *canon, "note", yexp__cstr(c, note->u.s));
    }
    return v;
}

static yexp_value* yexp__when_form(yexp__ctx* c, const yjs_value* src, unsigned allow, unsigned iallow, const char* elem, const char* key, yjs_value** canon) {
    static const char* const parts[2] = { "then", "else" };
    const yjs_value* tv = yjs_get(src, "when");
    const yjs_value* note = yjs_get(src, "note");
    yexp_value* v;
    yexp_value* it;
    yjs_value* ct = NULL;
    yjs_value* cp[2] = { NULL, NULL };
    const yexp_value* test = NULL;
    size_t i;
    int k;
    for (i = 0; i < src->n; i++) {
        const char* m = src->u.members[i].key;
        if (!yexp__streq(m, "when") && !yexp__streq(m, "then") && !yexp__streq(m, "else") && !yexp__streq(m, "note")) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': a when is {\"when\": TEST, \"then\": V, \"else\": V}; '%s' is an unknown key", elem, key, m);
            return NULL;
        }
    }
    if (!yjs_get(src, "then") || !yjs_get(src, "else")) {
        yexp__err(c, src->pos, "%s: '%s': a when has 'when', 'then' and 'else' (3.10)", elem, key);
        return NULL;
    }
    if (tv->type == YJS_STRING) {
        yexp_ref r;
        yexp_value* t;
        if (!yexp__ref_text(c, tv->u.s, tv->pos, false, &r, elem, key)) return NULL;
        t = yexp__vnew(c, YEXP_V_TEST, tv->pos);
        if (!t) return NULL;
        t->str = r.name;
        test = t;
        ct = yexp__cstr(c, tv->u.s);
    } else if (tv->type == YJS_OBJECT && yjs_get(tv, "signal")) {
        if (!yexp__note_ok(c, tv, elem)) return NULL;
        test = yexp__signal_form(c, tv, true, elem, key, &ct);
        if (!test) return NULL;
    } else {
        yexp__err(c, tv->pos, "%s: '%s': a when's test is a control's name or {\"signal\": NAME, \"above\": LEVEL} (3.10)", elem, key);
        return NULL;
    }
    v = yexp__vnew(c, YEXP_V_WHEN, src->pos);
    it = (yexp_value*)yexp__alloc(c, 2 * sizeof *it);
    if (!v || !it) return NULL;
    for (k = 0; k < 2; k++) {
        /* then and else: a literal, a variable, a lookup or a when */
        const yexp_value* e = yexp__value(c, yjs_get(src, parts[k]), (allow & ~(unsigned)(YEXP__AV_SIG | YEXP__AV_COL | YEXP__AV_VARFORM | YEXP__AV_TRANSFORM)) | YEXP__AV_WHEN,
                                          iallow, elem, key, &cp[k]);
        if (!e) return NULL;
        it[k] = *e;
    }
    v->by = test;
    v->items = it;
    v->n = 2;
    if (canon) {
        *canon = yexp__cnew(c, YJS_OBJECT);
        yexp__put(c, *canon, "when", ct);
        yexp__put(c, *canon, "then", cp[0]);
        yexp__put(c, *canon, "else", cp[1]);
        if (note) yexp__put(c, *canon, "note", yexp__cstr(c, note->u.s));
    }
    return v;
}

/* {"hit": "C.x"} and {"time": N, "from": M} (3.15). */
static yexp_value* yexp__col_form(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key, yjs_value** canon) {
    bool hit = yjs_get(src, "hit") != NULL;
    const yjs_value* note = yjs_get(src, "note");
    yexp_value* v;
    yexp_ref r;
    size_t i;
    for (i = 0; i < src->n; i++) {
        const char* m = src->u.members[i].key;
        if (!yexp__streq(m, "note") && !(hit ? yexp__streq(m, "hit") : (yexp__streq(m, "time") || yexp__streq(m, "from")))) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': a column is {\"hit\": \"C.x\"} or {\"time\": N, \"from\": M}; '%s' is an unknown key here", elem, key, m);
            return NULL;
        }
    }
    if (hit) {
        const yjs_value* h = yjs_get(src, "hit");
        if (h->type != YJS_STRING) { yexp__err(c, h->pos, "%s: '%s': 'hit' names a control with hits and a lane of its of: \"C.x\"", elem, key); return NULL; }
        if (!yexp__ref_text(c, h->u.s, h->pos, true, &r, elem, key)) return NULL;
        v = yexp__vnew(c, YEXP_V_HIT, src->pos);
        if (!v) return NULL;
        v->str = r.name;
        v->i = r.lane;
        if (canon) { *canon = yexp__cnew(c, YJS_OBJECT); yexp__put(c, *canon, "hit", yexp__cstr(c, h->u.s)); }
    } else {
        const yjs_value* t = yjs_get(src, "time");
        const yjs_value* f = yjs_get(src, "from");
        yexp_value* m;
        yexp_ref r2;
        if (!f) { yexp__err(c, src->pos, "%s: '%s': a time column is {\"time\": N, \"from\": M} (3.15)", elem, key); return NULL; }
        if (t->type != YJS_STRING || f->type != YJS_STRING) { yexp__err(c, src->pos, "%s: '%s': 'time' and 'from' are names (3.15)", elem, key); return NULL; }
        if (!yexp__ref_text(c, t->u.s, t->pos, false, &r, elem, key) || !yexp__ref_text(c, f->u.s, f->pos, false, &r2, elem, key)) return NULL;
        v = yexp__vnew(c, YEXP_V_TIME, t->pos);
        m = yexp__vnew(c, YEXP_V_STRING, f->pos);
        if (!v || !m) return NULL;
        v->str = r.name;
        m->str = r2.name;
        v->by = m;
        if (canon) {
            *canon = yexp__cnew(c, YJS_OBJECT);
            yexp__put(c, *canon, "time", yexp__cstr(c, t->u.s));
            yexp__put(c, *canon, "from", yexp__cstr(c, f->u.s));
        }
    }
    if (canon && note) yexp__put(c, *canon, "note", yexp__cstr(c, note->u.s));
    return v;
}

/* A cursor's {"rotation_deg": A} or {"mirror_deg": A} (3.7). */
static yexp_value* yexp__transform_form(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key, yjs_value** canon) {
    const char* which = yjs_get(src, "rotation_deg") ? "rotation_deg" : "mirror_deg";
    const yjs_value* note = yjs_get(src, "note");
    yexp_value* v;
    yjs_value* ca = NULL;
    size_t i;
    for (i = 0; i < src->n; i++) {
        const char* m = src->u.members[i].key;
        if (!yexp__streq(m, which) && !yexp__streq(m, "note")) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': a transform is \"identity\", {\"rotation_deg\": A} or {\"mirror_deg\": A}; '%s' does not go with '%s'", elem, key, m, which);
            return NULL;
        }
    }
    v = yexp__vnew(c, YEXP_V_TRANSFORM, src->pos);
    if (!v) return NULL;
    v->str = which;
    v->by = yexp__value(c, yjs_get(src, which), YEXP__AV_NUM | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, which, &ca);
    if (!v->by) return NULL;
    if (canon) {
        *canon = yexp__cnew(c, YJS_OBJECT);
        yexp__put(c, *canon, which, ca);
        if (note) yexp__put(c, *canon, "note", yexp__cstr(c, note->u.s));
    }
    return v;
}

static yexp__slot* yexp__object(yexp__ctx* c, int sect, unsigned mask, const yjs_value* src, const char* elem, yjs_value** canon);
static void yexp__release(yexp__ctx* c, int sect);
static const yexp_value* yexp__slotv(const yexp__ctx* c, yexp__slot* slots, int sect, const char* key);

/* {"uniform": [lo, hi], "n": N, "per": "node"} (3.14, D29). */
static yexp_value* yexp__uniform_form(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key, yjs_value** canon) {
    char sub[160];
    yexp__slot* s;
    yjs_value* cu = NULL;
    const yexp_value* lh;
    const yexp_value* nv;
    yexp_value* v;
    yexp_value* it;
    int before = c->r->n_errors;
    snprintf(sub, sizeof sub, "%s: '%s': uniform", elem, key);
    s = yexp__object(c, YEXP__S_UNIFORM, 0xffffu, src, sub, &cu);
    if (!s) return NULL;
    lh = yexp__slotv(c, s, YEXP__S_UNIFORM, "uniform");
    nv = yexp__slotv(c, s, YEXP__S_UNIFORM, "n");
    v = yexp__vnew(c, YEXP_V_UNIFORM, src->pos);
    it = (yexp_value*)yexp__alloc(c, 2 * sizeof *it);
    if (v && it && lh) {
        if (lh->n != 2 || lh->items[0].kind != YEXP_V_NUMBER || lh->items[1].kind != YEXP_V_NUMBER) {
            yexp__err(c, lh->pos, "%s: 'uniform' is [lo, hi], two numbers", sub);
        } else if (!(lh->items[0].num < lh->items[1].num)) {
            yexp__err(c, lh->pos, "%s: 'uniform': lo is below hi; [lo, hi) is empty otherwise", sub);
        } else {
            it[0] = lh->items[0];
            it[1] = lh->items[1];
        }
    }
    if (v && nv && (nv->kind != YEXP_V_NUMBER || nv->i < 1 || nv->i > 64))
        yexp__err(c, nv->pos, "%s: 'n' is an integer from 1 to 64", sub);
    yexp__release(c, YEXP__S_UNIFORM);
    if (!v || !it || c->r->n_errors != before) return NULL;
    v->items = it;
    v->n = 2;
    v->i = nv ? nv->i : 1;
    v->str = "node";
    if (canon) *canon = cu;
    return v;
}

static yexp_value* yexp__walk2d_form(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key, yjs_value** canon) {
    char sub[160];
    const yjs_value* w = yjs_get(src, "walk2d");
    const yjs_value* note = yjs_get(src, "note");
    yexp__slot* s;
    yjs_value* cw = NULL;
    const yexp_value* step;
    const yexp_value* box;
    const yexp_value* start;
    yexp_value* v;
    yexp_value* it;
    size_t i;
    int before = c->r->n_errors;
    snprintf(sub, sizeof sub, "%s: '%s': walk2d", elem, key);
    for (i = 0; i < src->n; i++)
        if (!yexp__streq(src->u.members[i].key, "walk2d") && !yexp__streq(src->u.members[i].key, "note")) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': a walk is {\"walk2d\": {\"step\": s, \"box\": [[x0, y0], [x1, y1]], \"start\": [x, y]}}; '%s' is an unknown key", elem, key, src->u.members[i].key);
            return NULL;
        }
    s = yexp__object(c, YEXP__S_WALK2D, 0xffffu, w, sub, &cw);
    if (!s) return NULL;
    step = yexp__slotv(c, s, YEXP__S_WALK2D, "step");
    box = yexp__slotv(c, s, YEXP__S_WALK2D, "box");
    start = yexp__slotv(c, s, YEXP__S_WALK2D, "start");
    v = yexp__vnew(c, YEXP_V_WALK2D, src->pos);
    it = (yexp_value*)yexp__alloc(c, 7 * sizeof *it);
    if (v && it && step && box && start && c->r->n_errors == before) {
        double x0 = box->items[0].items[0].num, y0 = box->items[0].items[1].num;
        double x1 = box->items[1].items[0].num, y1 = box->items[1].items[1].num;
        double sx = start->items[0].num, sy = start->items[1].num;
        double dx = x1 - x0, dy = y1 - y0;
        if (!(sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1)) {
            yexp__err(c, start->pos == UINT32_MAX ? w->pos : start->pos, "%s: the box does not hold 'start' (3.14)", sub);
        } else if (!(step->num > 0) || !(4.0 * step->num * step->num < dx * dx + dy * dy)) {
            /* every point of the box has a step that stays in it */
            yexp__err(c, step->pos, "%s: 'step' is above 0 and below half the box's diagonal (3.14)", sub);
        } else {
            it[0] = *step;
            it[1] = box->items[0].items[0]; it[2] = box->items[0].items[1];
            it[3] = box->items[1].items[0]; it[4] = box->items[1].items[1];
            it[5] = start->items[0]; it[6] = start->items[1];
        }
    }
    yexp__release(c, YEXP__S_WALK2D);
    if (!v || !it || c->r->n_errors != before) return NULL;
    v->items = it;
    v->n = 7;
    if (canon) {
        *canon = yexp__cnew(c, YJS_OBJECT);
        yexp__put(c, *canon, "walk2d", cw);
        if (note) yexp__put(c, *canon, "note", yexp__cstr(c, note->u.s));
    }
    return v;
}

/* The version 2 object forms, or NULL with *handled false when src is
 * none of them. */
static yexp_value* yexp__v2_form(yexp__ctx* c, const yjs_value* src, unsigned allow, unsigned iallow, const char* elem, const char* key, yjs_value** canon, bool* handled) {
    bool sig = yjs_get(src, "signal") != NULL, when = yjs_get(src, "when") != NULL;
    bool col = yjs_get(src, "hit") != NULL || yjs_get(src, "time") != NULL;
    bool tr = yjs_get(src, "rotation_deg") != NULL || yjs_get(src, "mirror_deg") != NULL;
    bool uni = yjs_get(src, "uniform") != NULL, walk = yjs_get(src, "walk2d") != NULL;
    *handled = true;
    if (when && (allow & YEXP__AV_WHEN)) return yexp__when_form(c, src, allow, iallow, elem, key, canon);
    if (sig && (allow & YEXP__AV_SIG)) return yexp__signal_form(c, src, false, elem, key, canon);
    if (col && (allow & YEXP__AV_COL)) return yexp__col_form(c, src, elem, key, canon);
    if (tr && (allow & YEXP__AV_TRANSFORM)) return yexp__transform_form(c, src, elem, key, canon);
    if (uni && (allow & YEXP__AV_VARFORM)) return yexp__uniform_form(c, src, elem, key, canon);
    if (walk && (allow & YEXP__AV_VARFORM)) return yexp__walk2d_form(c, src, elem, key, canon);
    if (sig || when) {
        yexp__err(c, src->pos, "%s: '%s': a {\"%s\": ...} value form is not allowed here; it goes in a set step's props or a column, a signal also in a region's center, and then and else take a literal, a variable, a lookup or a when (3.10)",
                  elem, key, when ? "when" : "signal");
        return NULL;
    }
    if (col) { yexp__err(c, src->pos, "%s: '%s': {\"hit\": ...} and {\"time\": ...} go only in a template's columns (3.15)", elem, key); return NULL; }
    if (uni || walk) { yexp__err(c, src->pos, "%s: '%s': {\"%s\": ...} goes only as a node's vars value, not inside another value (3.14)", elem, key, uni ? "uniform" : "walk2d"); return NULL; }
    if (tr) { yexp__err(c, src->pos, "%s: '%s': a transform goes only in a cursor's 'transform' (3.7)", elem, key); return NULL; }
    *handled = false;
    return NULL;
}

/* The integer that a number's exact decimal value is, when it is one within
 * int64, however it is spelled: one value, one form, one ID (D2). */
static bool yexp__exact_int(const yjs_value* src, int64_t* out) {
    char buf[32];
    size_t n;
    unsigned fl = 0;
    if (src->flags & YJS_NUM_I64) { *out = src->i; return true; }
    n = yjs_write_mem(src, YJS_WRITE_COMPACT, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf || !yjs_number_ok(buf, n, &fl) || !(fl & YJS_NUM_INT)) return false;
    return yjs_parse_fixed(buf, n, 0, out) == YJS_OK;
}

static void yexp__number(yexp__ctx* c, const yjs_value* src, unsigned allow, const char* elem, const char* key, yexp_value* v, yjs_value** canon) {
    double x;
    int64_t whole = 0;
    v->kind = YEXP_V_NUMBER;
    if (allow & YEXP__AV_DUR) {
        int64_t ns = 0;
        if (yjs_fixed(src, 9, &ns) != YJS_OK) {
            yexp__err(c, src->pos, "%s: '%s': a duration is a number of seconds that fits 64-bit nanoseconds; '%s' does not", elem, key, src->u.s);
            return;
        }
        if (ns < 0 && (allow & YEXP__AV_NONNEG)) {
            yexp__err(c, src->pos, "%s: '%s': a duration here is at least 0; '%s' is negative", elem, key, src->u.s);
            return;
        }
        v->flags = YEXP_VF_DURATION;
        v->i = ns;
        v->num = (double)ns / 1e9;
        if (ns > INT64_C(60000000000) || ns < -INT64_C(60000000000))
            yexp__warn(c, src->pos, "%s: '%s': a duration above 60 s (%s); seconds, not milliseconds?", elem, key, src->u.s);
        if (canon) { *canon = yjs_new_fixed(c->a, ns, 9); if (!*canon) c->nomem = true; }
        return;
    }
    if (yexp__exact_int(src, &whole)) {
        v->flags = YEXP_VF_INT;
        v->i = whole;
        v->num = (double)whole;
    } else {
        if (allow & YEXP__AV_INT) {
            yexp__err(c, src->pos, "%s: '%s': '%s' is not an integer within 64 bits", elem, key, src->u.s);
            return;
        }
        if (yjs_double(src, &x) != YJS_OK) {
            yexp__err(c, src->pos, "%s: '%s': the number '%s' has more than 19 significant digits or is out of range for a double", elem, key, src->u.s);
            return;
        }
        v->num = x;
        /* a real whose double is whole is written as that integer, so it
         * must read as one now, or the reload gives another form */
        if (x == floor(x) && x >= -9223372036854775808.0 && x < 9223372036854775808.0) {
            v->flags = YEXP_VF_INT;
            v->i = (int64_t)x;
        }
    }
    if ((allow & YEXP__AV_NONNEG) && v->num < 0) {
        yexp__err(c, src->pos, "%s: '%s': at least 0; '%s' is negative", elem, key, src->u.s);
        return;
    }
    if ((allow & YEXP__AV_INT32) && (v->i > INT32_MAX || v->i < INT32_MIN)) {
        yexp__err(c, src->pos, "%s: '%s': '%s' does not fit 32 bits", elem, key, src->u.s);
        return;
    }
    if (canon) {
        *canon = (v->flags & YEXP_VF_INT) ? yjs_new_int(c->a, v->i) : yjs_new_double(c->a, v->num);
        if (!*canon) c->nomem = true;
    }
}

static yexp_value* yexp__lookup_form(yexp__ctx* c, const yjs_value* src, unsigned allow, unsigned iallow, const char* elem, const char* key, yjs_value** canon) {
    const yjs_value* bys = yjs_get(src, "by");
    const yjs_value* maps = yjs_get(src, "map");
    const yjs_value* note = yjs_get(src, "note");
    yexp_value* v;
    yexp_kv* kv;
    yjs_value* cby = NULL;
    yjs_value* cmap = NULL;
    size_t i;
    yexp__lookup* meta;
    if (!(allow & YEXP__AV_LOOKUP)) {
        yexp__err(c, src->pos, "%s: '%s': a lookup is not allowed here; this key takes %s", elem, key, yexp__wants(allow));
        return NULL;
    }
    for (i = 0; i < src->n; i++) {
        const char* k = src->u.members[i].key;
        if (!yexp__streq(k, "by") && !yexp__streq(k, "map") && !yexp__streq(k, "note")) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': a lookup has 'by' and 'map'; '%s' is an unknown key", elem, key, k);
            return NULL;
        }
    }
    if (!bys || !maps || maps->type != YJS_OBJECT || maps->n == 0) {
        yexp__err(c, src->pos, "%s: '%s': a lookup is {\"by\": VALUE, \"map\": {KEY: VALUE, ...}} with at least one key", elem, key);
        return NULL;
    }
    v = yexp__vnew(c, YEXP_V_LOOKUP, src->pos);
    if (!v) return NULL;
    {
        const yexp_value* byv = yexp__value(c, bys, YEXP__AV_NUM | YEXP__AV_STR | YEXP__AV_BOOL | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, "by", &cby);
        if (!byv) return NULL;
        v->by = byv;
    }
    kv = (yexp_kv*)yexp__alloc(c, maps->n * sizeof(yexp_kv));
    v->reached_ = (uint8_t*)yexp__alloc(c, maps->n + 1u);
    cmap = yexp__cnew(c, YJS_OBJECT);
    if (!kv || !v->reached_ || !cmap) return NULL;
    for (i = 0; i < maps->n; i++) {
        yjs_value* cv = NULL;
        const yexp_value* mv = yexp__value(c, maps->u.members[i].value, allow & ~(unsigned)YEXP__AV_NESTED, iallow, elem, key, &cv);
        if (!mv) return NULL;
        kv[i].key = maps->u.members[i].key;
        kv[i].value = mv;
        yexp__put(c, cmap, kv[i].key, cv);
    }
    v->map = kv;
    v->n = maps->n;
    meta = (yexp__lookup*)yexp__alloc(c, sizeof(yexp__lookup));
    if (!meta) return NULL;
    meta->v = v;
    snprintf(meta->elem, sizeof meta->elem, "%s", elem);
    snprintf(meta->key, sizeof meta->key, "%s", key);
    if (c->lookups_tail) c->lookups_tail->next = meta;
    else c->lookups = meta;
    c->lookups_tail = meta;
    if (canon) {
        *canon = yexp__cnew(c, YJS_OBJECT);
        yexp__put(c, *canon, "by", cby);
        yexp__put(c, *canon, "map", cmap);
        if (note && note->type == YJS_STRING) yexp__put(c, *canon, "note", yexp__cstr(c, note->u.s));
    }
    return v;
}

static yexp_value* yexp__object_form(yexp__ctx* c, const yjs_value* src, unsigned allow, unsigned iallow, const char* elem, const char* key, yjs_value** canon) {
    if (!yexp__note_ok(c, src, elem)) return NULL;
    if (yjs_get(src, "lua")) {
        yexp__err(c, src->pos, "%s: '%s': {\"lua\": ...} needs a format version with scripts; version 1 refuses it (D6)", elem, key);
        return NULL;
    }
    if (c->version >= 2) {
        bool handled = false;
        yexp_value* v2 = yexp__v2_form(c, src, allow, iallow, elem, key, canon, &handled);
        if (handled) return v2;
    }
    if (yjs_get(src, "signal") || yjs_get(src, "when")) {
        yexp__err(c, src->pos, "%s: '%s': this value form is of format version 2; this definition is version 1", elem, key);
        return NULL;
    }
    if (yjs_get(src, "by") || yjs_get(src, "map")) return yexp__lookup_form(c, src, allow, iallow, elem, key, canon);
    if (yjs_get(src, "gamut")) {
        const yjs_value* g = yjs_get(src, "gamut");
        yexp_value* v;
        yexp__gamut* meta;
        if (!(allow & YEXP__AV_GAMUT)) {
            yexp__err(c, src->pos, "%s: '%s': a gamut value is not allowed here", elem, key);
            return NULL;
        }
        if (src->n > 1 + (yjs_get(src, "note") ? 1u : 0u) || g->type != YJS_STRING || !strchr(g->u.s, '.')) {
            yexp__err(c, src->pos, "%s: '%s': a gamut is {\"gamut\": \"stimulus.property\"}", elem, key);
            return NULL;
        }
        v = yexp__vnew(c, YEXP_V_GAMUT, src->pos);
        meta = (yexp__gamut*)yexp__alloc(c, sizeof(yexp__gamut));
        if (!v || !meta) return NULL;
        v->str = g->u.s;
        meta->v = v;
        snprintf(meta->elem, sizeof meta->elem, "%s", elem);
        snprintf(meta->key, sizeof meta->key, "%s", key);
        meta->next = c->gamuts;
        c->gamuts = meta;
        if (canon) {
            *canon = yexp__cnew(c, YJS_OBJECT);
            yexp__put(c, *canon, "gamut", yexp__cstr(c, v->str));
            if (yjs_get(src, "note")) yexp__put(c, *canon, "note", yexp__cstr(c, yjs_get(src, "note")->u.s));
        }
        return v;
    }
    if (yjs_get(src, "linspace")) {
        const yjs_value* ls = yjs_get(src, "linspace");
        yexp_value* v;
        yexp_value* it;
        yjs_value* carr;
        int k;
        if (!(allow & YEXP__AV_LINSPACE)) {
            yexp__err(c, src->pos, "%s: '%s': a linspace is not allowed here", elem, key);
            return NULL;
        }
        if (src->n > 1 + (yjs_get(src, "note") ? 1u : 0u) || ls->type != YJS_ARRAY || ls->n != 3) {
            yexp__err(c, src->pos, "%s: '%s': a linspace is {\"linspace\": [lo, hi, n]}", elem, key);
            return NULL;
        }
        v = yexp__vnew(c, YEXP_V_LINSPACE, src->pos);
        it = (yexp_value*)yexp__alloc(c, 3 * sizeof(yexp_value));
        carr = yexp__cnew(c, YJS_ARRAY);
        if (!v || !it || !carr) return NULL;
        for (k = 0; k < 3; k++) {
            yjs_value* ce = NULL;
            unsigned a2 = k < 2 ? (YEXP__AV_NUM | YEXP__AV_GAMUT | YEXP__AV_VAR) : (YEXP__AV_NUM | YEXP__AV_INT);
            const yexp_value* e = yexp__value(c, ls->u.items[k], a2, 0, elem, key, &ce);
            if (!e) return NULL;
            it[k] = *e;
            yexp__cpush(c, carr, ce);
        }
        if (it[2].kind == YEXP_V_NUMBER && it[2].i < 1) {
            yexp__err(c, src->pos, "%s: '%s': a linspace's n is at least 1", elem, key);
            return NULL;
        }
        v->items = it;
        v->n = 3;
        if (canon) {
            *canon = yexp__cnew(c, YJS_OBJECT);
            yexp__put(c, *canon, "linspace", carr);
            if (yjs_get(src, "note")) yexp__put(c, *canon, "note", yexp__cstr(c, yjs_get(src, "note")->u.s));
        }
        return v;
    }
    if (yjs_get(src, "michelson")) {
        yexp_value* v;
        const yexp_value* inner;
        yjs_value* ci = NULL;
        if (!(allow & YEXP__AV_MICH)) {
            yexp__err(c, src->pos, "%s: '%s': {\"michelson\": ...} is only for the property 'contrast'", elem, key);
            return NULL;
        }
        if (src->n > 1 + (yjs_get(src, "note") ? 1u : 0u)) {
            yexp__err(c, src->pos, "%s: '%s': a michelson is {\"michelson\": VALUE}", elem, key);
            return NULL;
        }
        v = yexp__vnew(c, YEXP_V_MICHELSON, src->pos);
        if (!v) return NULL;
        inner = yexp__value(c, yjs_get(src, "michelson"), YEXP__AV_NUM | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, key, &ci);
        if (!inner) return NULL;
        v->by = inner;
        if (canon) {
            *canon = yexp__cnew(c, YJS_OBJECT);
            yexp__put(c, *canon, "michelson", ci);
            if (yjs_get(src, "note")) yexp__put(c, *canon, "note", yexp__cstr(c, yjs_get(src, "note")->u.s));
        }
        return v;
    }
    if (yjs_get(src, "choice")) {
        const yjs_value* ch = yjs_get(src, "choice");
        yexp_value* v;
        yexp_value* it;
        yjs_value* carr;
        size_t i;
        if (!(allow & YEXP__AV_CHOICE)) {
            yexp__err(c, src->pos, "%s: '%s': {\"choice\": [...]} is only for a node's vars", elem, key);
            return NULL;
        }
        if (src->n > 1 + (yjs_get(src, "note") ? 1u : 0u) || ch->type != YJS_ARRAY || ch->n == 0) {
            yexp__err(c, src->pos, "%s: '%s': a choice is {\"choice\": [v, ...]} with at least one value", elem, key);
            return NULL;
        }
        v = yexp__vnew(c, YEXP_V_CHOICE, src->pos);
        it = (yexp_value*)yexp__alloc(c, ch->n * sizeof(yexp_value));
        carr = yexp__cnew(c, YJS_ARRAY);
        if (!v || !it || !carr) return NULL;
        for (i = 0; i < ch->n; i++) {
            yjs_value* ce = NULL;
            const yexp_value* e = yexp__value(c, ch->u.items[i], allow & ~(unsigned)(YEXP__AV_CHOICE | YEXP__AV_NESTED), iallow, elem, key, &ce);
            if (!e) return NULL;
            it[i] = *e;
            yexp__cpush(c, carr, ce);
        }
        v->items = it;
        v->n = ch->n;
        if (canon) {
            *canon = yexp__cnew(c, YJS_OBJECT);
            yexp__put(c, *canon, "choice", carr);
            if (yjs_get(src, "note")) yexp__put(c, *canon, "note", yexp__cstr(c, yjs_get(src, "note")->u.s));
        }
        return v;
    }
    if (allow & YEXP__AV_TRANSFORM) {
        yexp__err(c, src->pos, "%s: '%s': a transform is \"identity\", {\"rotation_deg\": A}, {\"mirror_deg\": A} or a lookup that gives one (3.7)", elem, key);
        return NULL;
    }
    yexp__err(c, src->pos, "%s: '%s': this object is not a value form (format 2.2); this key takes %s", elem, key, yexp__wants(allow));
    return NULL;
}

static yexp_value* yexp__value(yexp__ctx* c, const yjs_value* src, unsigned allow, unsigned iallow, const char* elem, const char* key, yjs_value** canon) {
    yexp_value* v;
    if (c->nomem) return NULL;
    if (canon) *canon = NULL;
    switch (src->type) {
    case YJS_NUMBER: {
        int before = c->r->n_errors;
        if (!(allow & YEXP__AV_NUM)) goto wrong;
        v = yexp__vnew(c, YEXP_V_NUMBER, src->pos);
        if (!v) return NULL;
        yexp__number(c, src, allow, elem, key, v, canon);
        return c->r->n_errors == before ? v : NULL;
    }
    case YJS_TRUE:
    case YJS_FALSE:
        if (!(allow & YEXP__AV_BOOL)) goto wrong;
        v = yexp__vnew(c, YEXP_V_BOOL, src->pos);
        if (!v) return NULL;
        v->i = src->type == YJS_TRUE;
        if (canon) *canon = yexp__cnew(c, src->type);
        return v;
    case YJS_STRING: {
        const char* s = src->u.s;
        if (s[0] == '$' && s[1] != '$') {
            if (!(allow & YEXP__AV_VAR)) {
                yexp__err(c, src->pos, "%s: '%s': '%s' is a variable; this key takes a literal", elem, key, s);
                return NULL;
            }
            if (!yexp__name_ok(s + 1)) {
                yexp__err(c, src->pos, "%s: '%s': '%s' is not a variable: a name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes; a literal '$' is written \"$$\"", elem, key, s);
                return NULL;
            }
            if ((allow & YEXP__AV_DUR) && !(allow & YEXP__AV_STR) && !yexp__ends_s(s + 1)) {
                yexp__err(c, src->pos, "%s: '%s': '%s' holds seconds, so its name ends in '_s' (D4)", elem, key, s);
                return NULL;
            }
            v = yexp__vnew(c, YEXP_V_VAR, src->pos);
            if (!v) return NULL;
            v->str = s + 1;
            if (canon) *canon = yexp__cstr(c, s);
            return v;
        }
        if (!(allow & YEXP__AV_STR)) {
            if (allow & YEXP__AV_DUR) {
                yexp__err(c, src->pos, "%s: '%s': a duration is a number of seconds or a \"$variable\" that holds seconds, not the text \"%s\" (D4)", elem, key, s);
                return NULL;
            }
            goto wrong;
        }
        v = yexp__vnew(c, YEXP_V_STRING, src->pos);
        if (!v) return NULL;
        v->str = s[0] == '$' ? s + 1 : s;   /* "$$x" is the text "$x" */
        if (canon) *canon = yexp__cstr(c, s);
        return v;
    }
    case YJS_ARRAY: {
        yexp_value* it;
        yjs_value* carr;
        size_t i;
        if (!(allow & YEXP__AV_ARRAY)) goto wrong;
        v = yexp__vnew(c, YEXP_V_ARRAY, src->pos);
        it = (yexp_value*)yexp__alloc(c, (src->n ? src->n : 1) * sizeof(yexp_value));
        carr = yexp__cnew(c, YJS_ARRAY);
        if (!v || !it || !carr) return NULL;
        for (i = 0; i < src->n; i++) {
            yjs_value* ce = NULL;
            const yexp_value* e = yexp__value(c, src->u.items[i], iallow, 0, elem, key, &ce);
            if (!e) return NULL;
            it[i] = *e;
            yexp__cpush(c, carr, ce);
        }
        v->items = it;
        v->n = src->n;
        if (canon) *canon = carr;
        return v;
    }
    case YJS_OBJECT:
        return yexp__object_form(c, src, allow, iallow, elem, key, canon);
    default:
        break;
    }
wrong:
    if (src->type == YJS_STRING && (allow & YEXP__AV_ARRAY))
        yexp__err(c, src->pos, "%s: '%s': a string is not allowed here; this key takes %s", elem, key, yexp__wants(allow));
    else
        yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes %s", elem, key, yexp__wants(allow));
    return NULL;
}

/* The numbers of an opaque object (web.exclusion) still get one spelling,
 * so a number's text does not move the ID; nothing else is read. */
static yjs_value* yexp__canon_any(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key) {
    yjs_value* out;
    size_t i;
    if (c->nomem) return NULL;
    switch (src->type) {
    case YJS_NUMBER: {
        yexp_value v;
        memset(&v, 0, sizeof v);
        out = NULL;
        yexp__number(c, src, YEXP__AV_NUM, elem, key, &v, &out);
        return out;
    }
    case YJS_STRING: return yexp__cstr(c, src->u.s);
    case YJS_ARRAY:
        out = yexp__cnew(c, YJS_ARRAY);
        for (i = 0; out && i < src->n; i++) yexp__cpush(c, out, yexp__canon_any(c, src->u.items[i], elem, key));
        return out;
    case YJS_OBJECT:
        out = yexp__cnew(c, YJS_OBJECT);
        for (i = 0; out && i < src->n; i++) yexp__put(c, out, src->u.members[i].key, yexp__canon_any(c, src->u.members[i].value, elem, key));
        return out;
    default:
        return yexp__cnew(c, src->type);
    }
}

/* Whether name is one of the "name=value" pairs of a row's values. */
static bool yexp__enum_ok(const char* values, const char* name) {
    size_t n = strlen(name);
    const char* p = values;
    while (p && *p) {
        const char* eq = strchr(p, '=');
        const char* end;
        if (!eq) return false;
        end = strchr(eq, ',');
        if ((size_t)(eq - p) == n && memcmp(p, name, n) == 0) return true;
        p = end ? end + 1 : NULL;
    }
    return false;
}

/* The names of a row's values for a message: "a, b, c". */
static void yexp__enum_list(const char* values, char* out, size_t cap) {
    size_t at = 0;
    const char* p = values;
    out[0] = 0;
    while (p && *p && at + 1 < cap) {
        const char* eq = strchr(p, '=');
        const char* end;
        if (!eq) break;
        end = strchr(eq, ',');
        at += (size_t)snprintf(out + at, cap - at, "%s%.*s", at ? ", " : "", (int)(eq - p), p);
        if (at >= cap) { out[cap - 1] = 0; break; }
        p = end ? end + 1 : NULL;
    }
}

/* --- objects by their section ------------------------------------------- */

static void yexp__unknown(yexp__ctx* c, int sect, unsigned mask, const yjs_member* m, const char* elem) {
    const char* key = m->key;
    char stem[80];
    int ri;
    if (yexp__streq(key, "lua") || yexp__streq(key, "script")) {
        yexp__err(c, m->key_pos, "%s: '%s': scripts need a later format version; version 1 refuses them (D6)", elem, key);
        return;
    }
    if (c->version < 2 && yexp__is_v2(key)) {
        yexp__err(c, m->key_pos, "%s: '%s' is a key of format version 2 (D16 to D20); this definition is version 1", elem, key);
        return;
    }
    if (c->version >= 2) {
        if (yexp__streq(key, "signals")) {
            yexp__err(c, m->key_pos, "%s: 'signals' (the graph as nodes) is of format version 3; version 2 refuses it (D26)", elem);
            return;
        }
        if (yexp__streq(key, "click_in")) {
            yexp__err(c, m->key_pos, "%s: 'click_in' is of format version 3; version 2 refuses it (D31)", elem);
            return;
        }
        if (yexp__streq(key, "assign_roles")) {
            yexp__err(c, m->key_pos, "%s: 'assign_roles' is of format version 3; version 2 binds a role in the rig profile (D32)", elem);
            return;
        }
        if (sect == YEXP__S_SRESPONSE && (yexp__streq(key, "role") || yexp__streq(key, "key_answer") || yexp__streq(key, "trigger") ||
                                          yexp__streq(key, "allow_held_key") || yexp__streq(key, "wait_for_key_release"))) {
            yexp__err(c, m->key_pos, "%s: '%s' is refused with signal choices: each movement's 'of' gives the input (3.13)", elem, key);
            return;
        }
    }
    snprintf(stem, sizeof stem, "%s_s", key);
    ri = yexp__find(c, sect, mask, stem);
    if (ri < 0) {
        size_t n = strlen(key);
        if (n > 3 && (memcmp(key + n - 3, "_ns", 3) == 0 || memcmp(key + n - 3, "_ms", 3) == 0)) {
            snprintf(stem, sizeof stem, "%.*s_s", (int)(n - 3), key);
            ri = yexp__find(c, sect, mask, stem);
        }
    }
    if (ri >= 0 && c->rows[ri].type == YEXP__T_DUR) {
        yexp__err(c, m->key_pos, "%s: '%s' is a duration; write the key '%s', in seconds (D4)", elem, key, c->rows[ri].key);
        return;
    }
    if (sect == YEXP__S_STIM && yexp__streq(key, "color_r")) {
        yexp__err(c, m->key_pos, "%s: '%s': write 'color' as [r, g, b]", elem, key);
        return;
    }
    if (sect == YEXP__S_RESPONSE && yexp__streq(key, "persist")) {
        yexp__err(c, m->key_pos, "%s: unknown key 'persist': 'response_ends_trial' is the one key for this (false: a response does not end the window)", elem);
        return;
    }
    ri = yexp__find(c, sect, 0xffffu, key);
    if (ri >= 0) {
        yexp__err(c, m->key_pos, "%s: key '%s' does not apply to this kind", elem, key);
        return;
    }
    if (sect == YEXP__S_STAIR || sect == YEXP__S_QUEST || sect == YEXP__S_AEP) {
        if (yexp__streq(key, "n_steps") || yexp__streq(key, "n_stim") || yexp__streq(key, "n_param") || yexp__streq(key, "n_dims")) {
            yexp__err(c, m->key_pos, "%s: '%s' is not a key: it comes from the length of the array it counts", elem, key);
            return;
        }
    }
    yexp__err(c, m->key_pos, "%s: unknown key '%s'", elem, key);
}

/* A string that is literal by the format: "$x" is refused and "$$x" is the
 * text "$x", the same in every key that takes one. */
static const yexp_value* yexp__literal_string(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key, yjs_value** canon) {
    const yexp_value* v;
    if (src->type != YJS_STRING) {
        yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes a string", elem, key);
        return NULL;
    }
    v = yexp__value(c, src, YEXP__AV_STR | YEXP__AV_VAR, 0, elem, key, canon);
    if (v && v->kind == YEXP_V_VAR) {
        yexp__err(c, src->pos, "%s: '%s': a literal string, not a variable", elem, key);
        return NULL;
    }
    return v;
}

/* [r, g, b], also as every value a lookup can give. */
static bool yexp__vec3_ok(yexp__ctx* c, const yexp_value* v, const char* elem, const char* key) {
    uint32_t k;
    if (v->kind == YEXP_V_ARRAY && v->n != 3) {
        yexp__err(c, v->pos, "%s: '%s': three numbers: [r, g, b]", elem, key);
        return false;
    }
    if (v->kind == YEXP_V_LOOKUP)
        for (k = 0; k < v->n; k++)
            if (!yexp__vec3_ok(c, v->map[k].value, elem, key)) return false;
    if (v->kind == YEXP_V_WHEN)
        for (k = 0; k < 2; k++)
            if (!yexp__vec3_ok(c, &v->items[k], elem, key)) return false;
    return true;
}

/* [x, y] (format finding F8: two elements), also as every value a lookup
 * gives; an element is a number or, unless literal, a variable. */
static bool yexp__point_ok(yexp__ctx* c, const yexp_value* v, const char* elem, const char* key) {
    uint32_t k;
    if (v->kind == YEXP_V_ARRAY && v->n != 2) {
        yexp__err(c, v->pos, "%s: '%s': a point is [x, y]", elem, key);
        return false;
    }
    if (v->kind == YEXP_V_LOOKUP)
        for (k = 0; k < v->n; k++)
            if (!yexp__point_ok(c, v->map[k].value, elem, key)) return false;
    return true;
}

/* A transform's text is "identity", also as a lookup's entry (3.7). */
static bool yexp__transform_ok(yexp__ctx* c, const yexp_value* v, const char* elem, const char* key) {
    uint32_t k;
    if (v->kind == YEXP_V_STRING && !yexp__streq(v->str, "identity")) {
        yexp__err(c, v->pos, "%s: '%s': a transform is \"identity\", {\"rotation_deg\": A}, {\"mirror_deg\": A} or a lookup that gives one; not \"%s\"", elem, key, v->str);
        return false;
    }
    if (v->kind == YEXP_V_LOOKUP)
        for (k = 0; k < v->n; k++)
            if (!yexp__transform_ok(c, v->map[k].value, elem, key)) return false;
    return true;
}

/* A row's scalar value. Returns false after a diagnostic. */
static bool yexp__row_value(yexp__ctx* c, int ri, const yjs_member* m, const char* elem, yexp__slot* slot, yjs_value** canon) {
    const yexp_row* r = &c->rows[ri];
    const yjs_value* src = m->value;
    unsigned lit = (r->flags & YEXP__R_LIT) ? ~(unsigned)(YEXP__AV_VAR | YEXP__AV_LOOKUP | YEXP__AV_GAMUT) : ~0u;
    const yexp_value* v = NULL;
    *canon = NULL;
    switch (r->type) {
    case YEXP__T_NUM: {
        unsigned al = (YEXP__AV_NUM | YEXP__AV_VAR | YEXP__AV_LOOKUP | YEXP__AV_GAMUT | (yexp__streq(r->key, "contrast") ? YEXP__AV_MICH : 0u) | c->extra_allow) & lit;
        v = yexp__value(c, src, al, 0, elem, r->key, canon);
        break;
    }
    case YEXP__T_INT:
        v = yexp__value(c, src, (YEXP__AV_NUM | YEXP__AV_INT | YEXP__AV_VAR | YEXP__AV_LOOKUP) & lit, 0, elem, r->key, canon);
        break;
    case YEXP__T_DUR:
        /* a negative minimum RT makes presses before the onset responses
         * (ysp/response.h 0.8.0); no other duration can run backward */
        v = yexp__value(c, src, (YEXP__AV_NUM | YEXP__AV_DUR | YEXP__AV_VAR | YEXP__AV_LOOKUP | (yexp__streq(r->key, "minimum_valid_rt_s") ? 0u : YEXP__AV_NONNEG)) & lit, 0, elem, r->key, canon);
        break;
    case YEXP__T_BOOL:
        if (src->type != YJS_TRUE && src->type != YJS_FALSE) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes true or false", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_BOOL, 0, elem, r->key, canon);
        break;
    case YEXP__T_STR:
        v = yexp__literal_string(c, src, elem, r->key, canon);
        if (!v) return false;
        break;
    case YEXP__T_ENUM:
        if (src->type != YJS_STRING) {
            char list[160];
            yexp__enum_list(r->values, list, sizeof list);
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes one of: %s", elem, r->key, list);
            return false;
        }
        if (!yexp__enum_ok(r->values, src->u.s)) {
            char list[160];
            yexp__enum_list(r->values, list, sizeof list);
            yexp__err(c, src->pos, "%s: '%s': \"%s\" is not one of: %s", elem, r->key, src->u.s, list);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_STR, 0, elem, r->key, canon);
        break;
    case YEXP__T_ENUMS: {
        size_t i;
        if (src->type != YJS_ARRAY) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes an array of names", elem, r->key);
            return false;
        }
        for (i = 0; i < src->n; i++)
            if (src->u.items[i]->type != YJS_STRING || !yexp__enum_ok(r->values, src->u.items[i]->u.s)) {
                char list[160];
                yexp__enum_list(r->values, list, sizeof list);
                yexp__err(c, src->u.items[i]->pos, "%s: '%s': every item is one of: %s", elem, r->key, list);
                return false;
            }
        v = yexp__value(c, src, YEXP__AV_ARRAY, YEXP__AV_STR, elem, r->key, canon);
        break;
    }
    case YEXP__T_TEXT:
        v = yexp__value(c, src, YEXP__AV_STR | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, r->key, canon);
        break;
    case YEXP__T_RES:
        if (src->type != YJS_STRING) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes a pack entry's name", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_STR | (yexp__streq(r->key, "texture") ? YEXP__AV_VAR : 0u), 0, elem, r->key, canon);
        break;
    case YEXP__T_VEC3:
        v = yexp__value(c, src, YEXP__AV_ARRAY | YEXP__AV_VAR | YEXP__AV_LOOKUP | c->extra_allow, YEXP__AV_NUM, elem, r->key, canon);
        if (v && !yexp__vec3_ok(c, v, elem, r->key)) return false;
        break;
    case YEXP__T_NUMS:
        v = yexp__value(c, src, YEXP__AV_ARRAY, YEXP__AV_NUM | YEXP__AV_GAMUT, elem, r->key, canon);
        break;
    case YEXP__T_GRID: {
        /* quest's axes: a fixed number, a list of points or a linspace */
        size_t i;
        yexp_value* it;
        yexp_value* vv;
        yjs_value* carr;
        if (src->type != YJS_ARRAY || src->n == 0) {
            yexp__err(c, src->pos, "%s: '%s': an array of axes: numbers, arrays of numbers or linspaces", elem, r->key);
            return false;
        }
        vv = yexp__vnew(c, YEXP_V_ARRAY, src->pos);
        it = (yexp_value*)yexp__alloc(c, src->n * sizeof(yexp_value));
        carr = yexp__cnew(c, YJS_ARRAY);
        if (!vv || !it || !carr) return false;
        for (i = 0; i < src->n; i++) {
            yjs_value* ce = NULL;
            const yexp_value* e = yexp__value(c, src->u.items[i], YEXP__AV_NUM | YEXP__AV_ARRAY | YEXP__AV_LINSPACE | YEXP__AV_GAMUT, YEXP__AV_NUM | YEXP__AV_GAMUT, elem, r->key, &ce);
            if (!e) return false;
            if (e->kind == YEXP_V_ARRAY && e->n == 0) {
                yexp__err(c, src->u.items[i]->pos, "%s: '%s': an axis has at least one point", elem, r->key);
                return false;
            }
            it[i] = *e;
            yexp__cpush(c, carr, ce);
        }
        vv->items = it;
        vv->n = src->n;
        v = vv;
        *canon = carr;
        break;
    }
    case YEXP__T_VAL:
        v = yexp__value(c, src, YEXP__AV_VALUE, YEXP__AV_NUM, elem, r->key, canon);
        break;
    case YEXP__T_CODE:
        v = yexp__value(c, src, YEXP__AV_NUM | YEXP__AV_INT | YEXP__AV_NONNEG | YEXP__AV_INT32 | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, r->key, canon);
        break;
    case YEXP__T_VAR:
        if (src->type != YJS_STRING || src->u.s[0] != '$' || src->u.s[1] == '$') {
            yexp__err(c, src->pos, "%s: '%s': a boolean variable: \"$name\"", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_VAR, 0, elem, r->key, canon);
        break;
    case YEXP__T_NAME:
    case YEXP__T_NAMEZ:
        if (c->version >= 2 && src->type == YJS_STRING && strchr(src->u.s, '.') && r->section >= YEXP__S_STEP) {
            yexp__err(c, src->pos, "%s: '%s': '%s': a lane goes only where a value is read (3.9); this key takes a name", elem, r->key, src->u.s);
            return false;
        }
        if (src->type != YJS_STRING || !(yexp__name_ok(src->u.s) || (r->type == YEXP__T_NAMEZ && !src->u.s[0]))) {
            yexp__err(c, src->pos, "%s: '%s': a name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_STR, 0, elem, r->key, canon);
        break;
    case YEXP__T_NAMES:
    case YEXP__T_STRS:
    case YEXP__T_TARGETS: {
        size_t i, n;
        yexp_value* vv;
        yexp_value* it;
        yjs_value* carr;
        const yjs_value* arr = src;
        bool one = false;
        if (r->type == YEXP__T_TARGETS && src->type == YJS_STRING) one = true;
        else if (src->type != YJS_ARRAY) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes %s", elem, r->key, r->type == YEXP__T_TARGETS ? "a name or an array of names" : "an array");
            return false;
        }
        n = one ? 1 : arr->n;
        if (n > YEXP__MAX_LIST) {
            yexp__err(c, src->pos, "%s: '%s': more than %d items", elem, r->key, YEXP__MAX_LIST);
            return false;
        }
        vv = yexp__vnew(c, YEXP_V_ARRAY, src->pos);
        it = (yexp_value*)yexp__alloc(c, (n ? n : 1) * sizeof(yexp_value));
        carr = one ? NULL : yexp__cnew(c, YJS_ARRAY);
        if (!vv || !it || (!one && !carr)) return false;
        for (i = 0; i < n; i++) {
            const yjs_value* e = one ? src : arr->u.items[i];
            yjs_value* ce = NULL;
            if (e->type != YJS_STRING || (r->type != YEXP__T_STRS && !yexp__name_ok(e->u.s))) {
                yexp__err(c, e->pos, "%s: '%s': %s", elem, r->key, r->type == YEXP__T_STRS ? "every item is a string" : "a name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes");
                return false;
            }
            {
                const yexp_value* ev = yexp__value(c, e, YEXP__AV_STR, 0, elem, r->key, &ce);
                if (!ev) return false;
                it[i] = *ev;
            }
            /* one target has one spelling: the shorter, the bare name */
            if (one || (r->type == YEXP__T_TARGETS && n == 1)) *canon = ce;
            else yexp__cpush(c, carr, ce);
        }
        vv->items = it;
        vv->n = (uint32_t)n;
        v = vv;
        if (!one && !(r->type == YEXP__T_TARGETS && n == 1)) *canon = carr;
        break;
    }
    case YEXP__T_SMAP: {
        size_t i;
        yexp_value* vv;
        yexp_kv* kv;
        yjs_value* cobj;
        if (src->type != YJS_OBJECT) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes an object of strings", elem, r->key);
            return false;
        }
        vv = yexp__vnew(c, YEXP_V_LOOKUP, src->pos);
        kv = (yexp_kv*)yexp__alloc(c, (src->n ? src->n : 1) * sizeof(yexp_kv));
        cobj = yexp__cnew(c, YJS_OBJECT);
        if (!vv || !kv || !cobj) return false;
        for (i = 0; i < src->n; i++) {
            yjs_value* ce = NULL;
            const yexp_value* e;
            if (src->u.members[i].value->type != YJS_STRING) {
                yexp__err(c, src->u.members[i].value->pos, "%s: '%s': every value is a string", elem, r->key);
                return false;
            }
            e = yexp__value(c, src->u.members[i].value, YEXP__AV_STR, 0, elem, r->key, &ce);
            if (!e) return false;
            kv[i].key = src->u.members[i].key;
            kv[i].value = e;
            yexp__put(c, cobj, kv[i].key, ce);
        }
        vv->map = kv;
        vv->n = src->n;
        v = vv;
        *canon = cobj;
        break;
    }
    case YEXP__T_POINT:
        if (r->flags & YEXP__R_LIT) v = yexp__value(c, src, YEXP__AV_ARRAY, YEXP__AV_NUM, elem, r->key, canon);
        else v = yexp__value(c, src, YEXP__AV_ARRAY | YEXP__AV_VAR | YEXP__AV_LOOKUP, YEXP__AV_NUM | YEXP__AV_VAR, elem, r->key, canon);
        if (v && !yexp__point_ok(c, v, elem, r->key)) return false;
        break;
    case YEXP__T_CENTER:
        v = yexp__value(c, src, YEXP__AV_ARRAY | YEXP__AV_VAR | YEXP__AV_LOOKUP | YEXP__AV_SIG, YEXP__AV_NUM | YEXP__AV_VAR, elem, r->key, canon);
        if (v && !yexp__point_ok(c, v, elem, r->key)) return false;
        break;
    case YEXP__T_TRANSFORM:
        v = yexp__value(c, src, YEXP__AV_STR | YEXP__AV_LOOKUP | YEXP__AV_TRANSFORM, 0, elem, r->key, canon);
        if (v && !yexp__transform_ok(c, v, elem, r->key)) return false;
        break;
    case YEXP__T_REF:
        if (src->type != YJS_STRING) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes a name, with a lane where one is read (\"hand.x\")", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_STR, 0, elem, r->key, canon);
        break;
    case YEXP__T_REFS: {
        size_t i;
        if (src->type != YJS_ARRAY || src->n == 0 || src->n > YEXP__MAX_LIST) {
            yexp__err(c, src->pos, "%s: '%s': an array of names, at least one", elem, r->key);
            return false;
        }
        for (i = 0; i < src->n; i++)
            if (src->u.items[i]->type != YJS_STRING) {
                yexp__err(c, src->u.items[i]->pos, "%s: '%s': every item is a name", elem, r->key);
                return false;
            }
        v = yexp__value(c, src, YEXP__AV_ARRAY, YEXP__AV_STR, elem, r->key, canon);
        break;
    }
    case YEXP__T_BOX: {
        /* walk2d's box: its checks need its numbers at load */
        yexp_value* vv;
        yexp_value* it;
        yjs_value* carr;
        int k;
        if (src->type != YJS_ARRAY || src->n != 2 || src->u.items[0]->type != YJS_ARRAY || src->u.items[1]->type != YJS_ARRAY ||
            src->u.items[0]->n != 2 || src->u.items[1]->n != 2) {
            yexp__err(c, src->pos, "%s: '%s': a box is [[x0, y0], [x1, y1]], numbers", elem, r->key);
            return false;
        }
        vv = yexp__vnew(c, YEXP_V_ARRAY, src->pos);
        it = (yexp_value*)yexp__alloc(c, 2 * sizeof *it);
        carr = yexp__cnew(c, YJS_ARRAY);
        if (!vv || !it || !carr) return false;
        for (k = 0; k < 2; k++) {
            yjs_value* ce = NULL;
            const yexp_value* e = yexp__value(c, src->u.items[k], YEXP__AV_ARRAY, YEXP__AV_NUM, elem, r->key, &ce);
            if (!e) return false;
            it[k] = *e;
            yexp__cpush(c, carr, ce);
        }
        vv->items = it;
        vv->n = 2;
        v = vv;
        *canon = carr;
        break;
    }
    default:
        return true;   /* a composite: the builder reads slot->src */
    }
    if (!v) return false;
    slot->v = v;
    return true;
}

/* One object of a section: every member against the rows, every default
 * filled, and the canonical object with the keys at their defaults left
 * out. Returns the slots (one per row of the section, in table order) or
 * NULL when src is no object. A composite row leaves its value in
 * slot->src and its canonical key to the caller. */
static yexp__slot* yexp__object(yexp__ctx* c, int sect, unsigned mask, const yjs_value* src, const char* elem, yjs_value** canon) {
    int n = c->count[sect], f = c->first[sect], i;
    size_t k;
    yexp__slot* slots;
    yjs_value* out;
    if (src->type != YJS_OBJECT) {
        yexp__err(c, src->pos, "%s: wrong type; expected an object", elem);
        return NULL;
    }
    if (!yexp__note_ok(c, src, elem)) return NULL;
    if (c->top + n > YEXP__SLOT_STACK) {
        yexp__err(c, src->pos, "%s: the definition nests too deep", elem);
        return NULL;
    }
    slots = c->slots + c->top;
    c->top += n;
    memset(slots, 0, (size_t)n * sizeof *slots);
    out = yexp__cnew(c, YJS_OBJECT);
    if (!out) return slots;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        int ri;
        yexp__slot* slot;
        yjs_value* cv = NULL;
        if (yexp__streq(m->key, "note")) {
            yexp__put(c, out, "note", yexp__cstr(c, m->value->u.s));
            continue;
        }
        ri = yexp__find(c, sect, mask, m->key);
        if (ri < 0) { yexp__unknown(c, sect, mask, m, elem); continue; }
        if (c->rows[ri].flags & YEXP__R_STEP) {
            yexp__err(c, m->key_pos, "%s: '%s' is not a stimulus key in version 2: the steps 'on' and 'off' drive it (D41)", elem, m->key);
            continue;
        }
        slot = &slots[ri - f];
        slot->given = true;
        slot->src = m->value;
        slot->pos = m->key_pos;
        if (!yexp__row_value(c, ri, m, elem, slot, &cv)) continue;
        if (slot->v && cv) {
            const yexp_row* r = &c->rows[ri];
            const yexp_value* d = r->def ? yexp__default(c, ri) : NULL;
            bool at_default = d && (yexp__same(slot->v, d) || (r->type == YEXP__T_SMAP && slot->v->n == 0));
            if (at_default && (r->flags & YEXP__R_SELF) == 0) {
                /* the same form and the same checks as when it is left out */
                slot->v = d;
                slot->given = false;
            } else if (r->flags & YEXP__R_SELF) {
                slot->src = (const yjs_value*)cv;
            } else {
                yexp__put(c, out, m->key, cv);
            }
        }
    }
    for (i = 0; i < n; i++) {
        const yexp_row* r = &c->rows[f + i];
        if (!(r->kinds & mask)) continue;
        if (!slots[i].given) {
            if (r->flags & YEXP__R_REQ) {
                yexp__err(c, src->pos, "%s: '%s' is required", elem, r->key);
            } else if (r->def && r->type != YEXP__T_OBJ && r->type != YEXP__T_SET && r->type != YEXP__T_LIST) {
                slots[i].v = yexp__default(c, f + i);
            }
        }
    }
    *canon = out;
    return slots;
}

static void yexp__release(yexp__ctx* c, int sect) { c->top -= c->count[sect]; }

static const yexp_value* yexp__slotv(const yexp__ctx* c, yexp__slot* slots, int sect, const char* key) {
    int ri = yexp__find(c, sect, 0xffffu, key);
    return ri >= 0 ? slots[ri - c->first[sect]].v : NULL;
}

static yexp__slot* yexp__slot_at(const yexp__ctx* c, yexp__slot* slots, int sect, const char* key) {
    int ri = yexp__find(c, sect, 0xffffu, key);
    return ri >= 0 ? &slots[ri - c->first[sect]] : NULL;
}

/* The slot of a key for one kind: a key that two kinds spell alike (a
 * tween's `to` and recenter's) has a row for each. */
static yexp__slot* yexp__slot_at_m(const yexp__ctx* c, yexp__slot* slots, int sect, unsigned mask, const char* key) {
    int ri = yexp__find(c, sect, mask, key);
    return ri >= 0 ? &slots[ri - c->first[sect]] : NULL;
}

static const yexp_value* yexp__slotv_m(const yexp__ctx* c, yexp__slot* slots, int sect, unsigned mask, const char* key) {
    yexp__slot* x = yexp__slot_at_m(c, slots, sect, mask, key);
    return x ? x->v : NULL;
}

/* A literal integer key of the form: out of its range is a refusal, never
 * the default. */
static int yexp__int_field(yexp__ctx* c, const yexp_value* v, const char* elem, const char* key, int lo, int hi, int dflt) {
    if (!v) return dflt;
    if (v->kind != YEXP_V_NUMBER || !(v->flags & YEXP_VF_INT) || v->i < lo || v->i > hi) {
        yexp__err(c, v->pos, "%s: '%s' is a literal integer from %d to %d", elem, key, lo, hi);
        return dflt;
    }
    return (int)v->i;
}

static bool yexp__v_bool(const yexp_value* v, bool dflt) {
    return v && v->kind == YEXP_V_BOOL ? v->i != 0 : dflt;
}

static const char* yexp__v_str(const yexp_value* v) {
    return v && (v->kind == YEXP_V_STRING || v->kind == YEXP_V_VAR) ? v->str : NULL;
}

/* A names array value as a C array of strings. */
static const char* const* yexp__str_array(yexp__ctx* c, const yexp_value* v, int* n) {
    const char** out;
    uint32_t i;
    *n = 0;
    if (!v || v->kind != YEXP_V_ARRAY || v->n == 0) return NULL;
    out = (const char**)yexp__alloc(c, v->n * sizeof(char*));
    if (!out) return NULL;
    for (i = 0; i < v->n; i++) out[i] = v->items[i].str;
    *n = (int)v->n;
    return out;
}

/* --- named sets --------------------------------------------------------- */

/* A set's own note is a note, not a member (docs/experiment.md: 'note' in a
 * named set). */
static bool yexp__is_set_note(const yjs_member* m) {
    return yexp__streq(m->key, "note") && m->value->type == YJS_STRING;
}

/* The number of members besides the note; false after a diagnostic. */
static bool yexp__set_begin(yexp__ctx* c, const yjs_value* src, const char* elem, int limit, int* n) {
    size_t k, count = 0;
    if (src->type != YJS_OBJECT) {
        yexp__err(c, src->pos, "%s: wrong type; expected an object of names", elem);
        return false;
    }
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        if (yexp__is_set_note(m)) continue;
        count++;
        if (!yexp__name_ok(m->key)) {
            yexp__err(c, m->key_pos, "%s: '%s': a name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes", elem, m->key);
        }
    }
    if (count > (size_t)limit) {
        yexp__err(c, src->pos, "%s: more than %d names", elem, limit);
        return false;
    }
    *n = (int)count;
    return true;
}

/* --- session, display, web, devices ------------------------------------- */

static void yexp__parse_session(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yjs_value* cs = NULL;
    yexp__slot* slots;
    yexp__slot* sf;
    const yjs_value* fields;
    yexp_field* out;
    yjs_value* cfields;
    int n = 0, i = 0;
    size_t k;
    bool has_participant = false;
    if (!src) {
        /* the format's default: participant, integer, required */
        out = (yexp_field*)yexp__alloc(c, sizeof *out);
        if (!out) return;
        out[0].name = "participant";
        out[0].is_string = false;
        out[0].required = true;
        c->d->fields = out;
        c->d->n_fields = 1;
        return;
    }
    slots = yexp__object(c, YEXP__S_SESSION, 0xffffu, src, "session", &cs);
    if (!slots) return;
    sf = yexp__slot_at(c, slots, YEXP__S_SESSION, "fields");
    fields = sf->src;
    if (!fields) { yexp__release(c, YEXP__S_SESSION); return; }
    if (!yexp__set_begin(c, fields, "session: 'fields'", YEXP__MAX_SET, &n)) { yexp__release(c, YEXP__S_SESSION); return; }
    out = (yexp_field*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cfields = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cfields) { yexp__release(c, YEXP__S_SESSION); return; }
    for (k = 0; k < fields->n; k++) {
        const yjs_member* m = &fields->u.members[k];
        char elem[96];
        yjs_value* cf = NULL;
        yexp__slot* s2;
        if (yexp__is_set_note(m)) { yexp__put(c, cfields, "note", yexp__cstr(c, m->value->u.s)); continue; }
        snprintf(elem, sizeof elem, "session field '%s'", m->key);
        if (yexp__is_builtin(m->key)) yexp__err(c, m->key_pos, "%s: the name is a built-in variable's", elem);
        s2 = yexp__object(c, YEXP__S_SFIELD, 0xffffu, m->value, elem, &cf);
        if (!s2) continue;
        out[i].name = m->key;
        {
            const char* t = yexp__v_str(yexp__slotv(c, s2, YEXP__S_SFIELD, "type"));
            out[i].is_string = t && yexp__streq(t, "string");
        }
        out[i].required = yexp__v_bool(yexp__slotv(c, s2, YEXP__S_SFIELD, "required"), false);
        if (yexp__streq(m->key, "participant")) has_participant = true;
        i++;
        yexp__put(c, cfields, m->key, cf);
        yexp__release(c, YEXP__S_SFIELD);
    }
    if (!has_participant)
        yexp__err(c, fields->pos, "session: 'fields' has no 'participant': rules take the participant number from it (2.4)");
    c->d->fields = out;
    c->d->n_fields = i;
    yexp__put(c, cs, "fields", cfields);
    yexp__release(c, YEXP__S_SESSION);
    *canon = cs;
}

static void yexp__parse_display(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yjs_value* cd = NULL;
    yexp__slot* s;
    yexp_def* d = c->d;
    const yexp_value* bg;
    int mt = 0;
    d->background[0] = d->background[1] = d->background[2] = 0.5;
    if (!src) return;
    s = yexp__object(c, YEXP__S_DISPLAY, 0xffffu, src, "display", &cd);
    if (!s) return;
    {
        const char* u = yexp__v_str(yexp__slotv(c, s, YEXP__S_DISPLAY, "units"));
        d->units_deg = u && yexp__streq(u, "deg");
        d->units = !u ? 0 : yexp__streq(u, "deg") ? 1 : yexp__streq(u, "cm") ? 2 : 0;
        u = yexp__v_str(yexp__slotv(c, s, YEXP__S_DISPLAY, "calibration"));
        d->calibration_required = u && yexp__streq(u, "required");
    }
    bg = yexp__slotv(c, s, YEXP__S_DISPLAY, "background");
    if (bg && bg->kind == YEXP_V_ARRAY && bg->n == 3) {
        int k;
        for (k = 0; k < 3; k++) {
            if (bg->items[k].kind != YEXP_V_NUMBER) {
                yexp__err(c, bg->items[k].pos, "display: 'background': a number per channel");
            } else {
                d->background[k] = bg->items[k].num;
            }
        }
    } else if (bg && bg->kind != YEXP_V_ARRAY) {
        yexp__err(c, bg->pos, "display: 'background': literal [r, g, b]");
    }
    mt = yexp__int_field(c, yexp__slotv(c, s, YEXP__S_DISPLAY, "min_tier"), "display", "min_tier", 0, 3, 0);
    d->min_tier = mt;
    yexp__release(c, YEXP__S_DISPLAY);
    *canon = cd;
}

static void yexp__parse_web(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yjs_value* cw = NULL;
    yexp__slot* s;
    if (!src) return;
    s = yexp__object(c, YEXP__S_WEB, 0xffffu, src, "web", &cw);
    if (!s) return;
    c->d->web_allowed = yexp__v_bool(yexp__slotv(c, s, YEXP__S_WEB, "allowed"), false);
    {
        yexp__slot* ex = yexp__slot_at(c, s, YEXP__S_WEB, "exclusion");
        if (ex && ex->src) {
            if (ex->src->type != YJS_OBJECT) {
                yexp__err(c, ex->src->pos, "web: 'exclusion' is an object");
            } else {
                c->d->web_exclusion = ex->src;
                yexp__put(c, cw, "exclusion", yexp__canon_any(c, ex->src, "web", "exclusion"));
            }
        }
    }
    yexp__release(c, YEXP__S_WEB);
    *canon = cw;
}

static int yexp__str_cmp(const void* a, const void* b);

static void yexp__parse_devices(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_role* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!src) return;
    if (!yexp__set_begin(c, src, "devices", YEXP__MAX_SET, &n)) { c->bad_sets |= YEXP__BAD_ROLES; return; }
    out = (yexp_role*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        char elem[96];
        yjs_value* cr = NULL;
        yexp__slot* s;
        yexp_role* r;
        const yexp_value* v;
        int before;
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        snprintf(elem, sizeof elem, "role '%s'", m->key);
        before = c->r->n_errors;
        s = yexp__object(c, YEXP__S_ROLE, 0xffffu, m->value, elem, &cr);
        r = &out[i++];
        /* a refused role keeps its name, so a reference to it is no second error */
        r->name = m->key;
        r->pos_ = m->key_pos;
        r->failed_ = !s;
        if (!s) continue;
        {
            const char* dir = yexp__v_str(yexp__slotv(c, s, YEXP__S_ROLE, "direction"));
            r->direction = !dir ? 0 : yexp__streq(dir, "out") ? 1 : yexp__streq(dir, "both") ? 2 : 0;
        }
        v = yexp__slotv(c, s, YEXP__S_ROLE, "family");
        r->family = yexp__str_array(c, v, &r->n_family);
        if (v && v->n == 0) yexp__err(c, v->pos, "%s: 'family' has at least one name", elem);
        r->required = yexp__v_bool(yexp__slotv(c, s, YEXP__S_ROLE, "required"), true);
        r->min_tier = yexp__int_field(c, yexp__slotv(c, s, YEXP__S_ROLE, "min_tier"), elem, "min_tier", 0, 3, 0);
        {
            const yexp_value* p = yexp__slotv(c, s, YEXP__S_ROLE, "pulse_s");
            r->pulse_ns = p && p->kind == YEXP_V_NUMBER ? p->i : 0;
            if (p && p->kind != YEXP_V_NUMBER) yexp__err(c, p->pos, "%s: 'pulse_s' is a number of seconds, not a variable: a role is read before any trial", elem);
        }
        r->kind = yexp__v_str(yexp__slotv(c, s, YEXP__S_ROLE, "kind"));
        v = yexp__slotv(c, s, YEXP__S_ROLE, "buttons");
        if (v) {
            r->buttons = v->map;
            r->n_buttons = (int)v->n;
            if (v->n) {
                /* sorted, so a long choices list against long buttons stays cheap */
                const char** bs = (const char**)yexp__alloc(c, v->n * sizeof(char*));
                uint32_t q;
                if (bs) {
                    for (q = 0; q < v->n; q++) bs[q] = v->map[q].value->str;
                    qsort((void*)bs, v->n, sizeof(char*), yexp__str_cmp);
                    r->sorted_buttons_ = bs;
                }
            }
        }
        v = yexp__slotv(c, s, YEXP__S_ROLE, "map");
        if (v) { r->map = v->map; r->n_map = (int)v->n; }
        r->failed_ = c->r->n_errors != before;
        yexp__put(c, cset, m->key, cr);
        yexp__release(c, YEXP__S_ROLE);
    }
    c->d->roles = out;
    c->d->n_roles = i;
    *canon = cset;
}

/* --- signal roles and controls (version 2, format 3.5, 3.7) ------------- */

static void yexp__parse_sigroles(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_sigrole* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!src) return;
    if (!yexp__set_begin(c, src, "roles", YEXP__MAX_SET, &n)) { c->bad_sets |= YEXP__BAD_SIGROLES; return; }
    out = (yexp_sigrole*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        static const int lanes[] = { 2, 2, 3, 1, 2 };
        const yjs_member* m = &src->u.members[k];
        char elem[96];
        yjs_value* cr = NULL;
        yexp__slot* s;
        yexp_sigrole* r;
        const char* t;
        const char* u;
        int before = c->r->n_errors;
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        snprintf(elem, sizeof elem, "signal role '%s'", m->key);
        r = &out[i++];
        r->name = m->key;
        r->pos_ = m->key_pos;
        s = yexp__object(c, YEXP__S_SIGROLE, 0xffffu, m->value, elem, &cr);
        if (!s) { r->failed_ = true; continue; }
        t = yexp__v_str(yexp__slotv(c, s, YEXP__S_SIGROLE, "type"));
        u = yexp__v_str(yexp__slotv(c, s, YEXP__S_SIGROLE, "units"));
        r->type = !t ? 0 : yexp__streq(t, "position2d_relative") ? 1 : yexp__streq(t, "position3d") ? 2 : yexp__streq(t, "scalar") ? 3 : yexp__streq(t, "gaze") ? 4 : 0;
        r->units = !u ? 0 : yexp__streq(u, "display_cm") ? 1 : yexp__streq(u, "display_deg") ? 2 : yexp__streq(u, "workspace_cm") ? 3 : yexp__streq(u, "device") ? 4 : 0;
        r->lanes = lanes[r->type];
        r->allow_relative = yexp__v_bool(yexp__slotv(c, s, YEXP__S_SIGROLE, "allow_relative"), false);
        r->required = yexp__v_bool(yexp__slotv(c, s, YEXP__S_SIGROLE, "required"), true);
        if (r->allow_relative && r->type != YEXP_SIG_POSITION2D) {
            const yexp__slot* ar = yexp__slot_at(c, s, YEXP__S_SIGROLE, "allow_relative");
            yexp__err(c, ar->pos, "%s: 'allow_relative' is only for the type position2d (3.5)", elem);
        }
        r->failed_ = c->r->n_errors != before;
        yexp__put(c, cset, m->key, cr);
        yexp__release(c, YEXP__S_SIGROLE);
    }
    c->d->sigroles = out;
    c->d->n_sigroles = i;
    *canon = cset;
}

/* A moving target's lane: {"amplitudes": [...], "freqs_hz": [...], "phases": [...]}. */
static bool yexp__parse_sines(yexp__ctx* c, const yjs_value* src, const char* elem, const char* key, yexp_sines* out, yjs_value** canon) {
    static const char* const keys[3] = { "amplitudes", "freqs_hz", "phases" };
    const yexp_value* v[3] = { NULL, NULL, NULL };
    yjs_value* cv[3] = { NULL, NULL, NULL };
    size_t i;
    int k;
    if (src->type != YJS_OBJECT) {
        yexp__err(c, src->pos, "%s: '%s' is {\"amplitudes\": [...], \"freqs_hz\": [...], \"phases\": [...]} (3.7)", elem, key);
        return false;
    }
    if (!yexp__note_ok(c, src, elem)) return false;
    for (i = 0; i < src->n; i++) {
        const char* m = src->u.members[i].key;
        if (!yexp__streq(m, keys[0]) && !yexp__streq(m, keys[1]) && !yexp__streq(m, keys[2]) && !yexp__streq(m, "note")) {
            yexp__err(c, src->u.members[i].key_pos, "%s: '%s': unknown key '%s'; a lane has amplitudes, freqs_hz and phases", elem, key, m);
            return false;
        }
    }
    for (k = 0; k < 3; k++) {
        const yjs_value* x = yjs_get(src, keys[k]);
        if (!x) { yexp__err(c, src->pos, "%s: '%s': '%s' is required (3.7)", elem, key, keys[k]); return false; }
        /* amplitudes and frequencies are literal; the phases may be an
         * array variable (a uniform of n, 3.14) */
        v[k] = yexp__value(c, x, k == 2 ? (YEXP__AV_ARRAY | YEXP__AV_VAR) : YEXP__AV_ARRAY, YEXP__AV_NUM, elem, keys[k], &cv[k]);
        if (!v[k]) return false;
    }
    out->n = (int)v[0]->n;
    if (v[0]->n < 1 || v[0]->n > 64) {
        yexp__err(c, v[0]->pos, "%s: '%s': 1 to 64 components; 'amplitudes' has %u (3.7)", elem, key, (unsigned)v[0]->n);
        return false;
    }
    for (k = 1; k < 3; k++)
        if (v[k]->kind == YEXP_V_ARRAY && v[k]->n != v[0]->n) {
            yexp__err(c, v[k]->pos, "%s: '%s': '%s' has %u numbers and 'amplitudes' %u; one length n (3.7)", elem, key, keys[k], (unsigned)v[k]->n, (unsigned)v[0]->n);
            return false;
        }
    out->amplitudes = v[0];
    out->freqs_hz = v[1];
    out->phases = v[2];
    if (canon) {
        *canon = yexp__cnew(c, YJS_OBJECT);
        for (k = 0; k < 3; k++) yexp__put(c, *canon, keys[k], cv[k]);
        if (yjs_get(src, "note")) yexp__put(c, *canon, "note", yexp__cstr(c, yjs_get(src, "note")->u.s));
    }
    return true;
}

/* A literal number that must be above 0: radius, onset, speed_below; or a
 * duration: lag_s, speed_lag_s, after_s (3.7, 3.16). */
static void yexp__above0(yexp__ctx* c, const yexp_value* v, const char* elem, const char* key) {
    if (!v || v->kind != YEXP_V_NUMBER) return;
    if ((v->flags & YEXP_VF_DURATION) ? v->i <= 0 : !(v->num > 0))
        yexp__err(c, v->pos, "%s: '%s' is above 0", elem, key);
}

/* A ref key's text into r, unresolved. */
static void yexp__ref_slot(yexp__ctx* c, yexp__slot* s, unsigned mask, const char* key, bool lane_ok, yexp_ref* r, const char* elem) {
    yexp__slot* x = yexp__slot_at_m(c, s, YEXP__S_CONTROL, mask, key);
    memset(r, 0, sizeof *r);
    r->index = -1;
    r->lane = -1;
    if (x && x->v && x->v->kind == YEXP_V_STRING) yexp__ref_text(c, x->v->str, x->v->pos, lane_ok, r, elem, key);
}

static void yexp__parse_control(yexp__ctx* c, const yjs_member* m, yexp_control* out, yjs_value* cset, bool shared, const char* prefix) {
    char elem[160];
    const yjs_value* kv;
    int kind = -1, i, f = c->first[YEXP__S_CONTROL], rows = c->count[YEXP__S_CONTROL], np = 0;
    unsigned mask;
    yexp__slot* s;
    yjs_value* cs = NULL;
    yexp_prop* props;
    snprintf(elem, sizeof elem, "%scontrol '%s'", prefix, m->key);
    out->name = m->key;
    out->pos_ = m->key_pos;
    out->shared = shared;
    out->failed_ = true;
    if (m->value->type != YJS_OBJECT) { yexp__err(c, m->value->pos, "%s: wrong type; expected an object", elem); return; }
    kv = yjs_get(m->value, "control");
    if (!kv) { yexp__err(c, m->value->pos, "%s: 'control' is required: the kind of control (3.7)", elem); return; }
    if (kv->type == YJS_STRING)
        for (i = 0; i < 8; i++)
            if (yexp__streq(kv->u.s, yexp__ctl_kinds[i])) kind = i;
    if (kind < 0) {
        yexp__err(c, kv->pos, "%s: 'control': not one of: cursor, region, distance, movement, peak, moving_target, trace, signal_lost", elem);
        return;
    }
    mask = 1u << kind;
    {
        int before = c->r->n_errors;
        s = yexp__object(c, YEXP__S_CONTROL, mask, m->value, elem, &cs);
        if (!s) return;
        out->kind = (yexp_control_kind)kind;
        yexp__ctl_gives(out->kind, &out->lanes, &out->has_test, &out->has_hits);
        for (i = 0; i < rows; i++)
            if ((c->rows[f + i].kinds & mask) && c->rows[f + i].type != YEXP__T_SINES && !yexp__streq(c->rows[f + i].key, "control")) np++;
        props = (yexp_prop*)yexp__alloc(c, (size_t)(np ? np : 1) * sizeof *props);
        np = 0;
        if (!props) { yexp__release(c, YEXP__S_CONTROL); return; }
        for (i = 0; i < rows; i++) {
            const yexp_row* r = &c->rows[f + i];
            if (!(r->kinds & mask) || r->type == YEXP__T_SINES || yexp__streq(r->key, "control")) continue;
            if (!s[i].v) continue;
            props[np].key = r->key;
            props[np].value = s[i].v;
            props[np].given = s[i].given;
            np++;
        }
        out->props = props;
        out->n_props = np;
        /* refs: their spelling now, their meaning once every name is known */
        yexp__ref_slot(c, s, mask, "of", true, &out->of, elem);
        yexp__ref_slot(c, s, mask, "from", false, &out->from, elem);
        yexp__ref_slot(c, s, mask, "to", false, &out->to, elem);
        yexp__ref_slot(c, s, mask, "freeze_at", false, &out->freeze_at, elem);
        yexp__ref_slot(c, s, mask, "stimulus", false, &out->stimulus, elem);
        if (kind == YEXP_CURSOR) {
            const yexp_value* mean = yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "mean");
            int given = 0;
            yexp__ref_slot(c, s, mask, "role", false, &out->of, elem);
            yexp__ref_slot(c, s, mask, "x", true, &out->x, elem);
            yexp__ref_slot(c, s, mask, "y", true, &out->y, elem);
            given = (out->of.name != NULL) + (out->x.name || out->y.name) + (mean != NULL);
            if (given != 1) yexp__err(c, m->value->pos, "%s: a cursor has exactly one of 'role', 'x' with 'y', and 'mean' (3.7)", elem);
            else if ((out->x.name != NULL) != (out->y.name != NULL)) yexp__err(c, m->value->pos, "%s: 'x' and 'y' go together (3.7)", elem);
            if (mean) {
                uint32_t q;
                yexp_ref* items = (yexp_ref*)yexp__alloc(c, (mean->n ? mean->n : 1) * sizeof *items);
                if (mean->n < 2 || mean->n > 4) yexp__err(c, mean->pos, "%s: 'mean' has 2 to 4 roles (3.7)", elem);
                if (items) {
                    for (q = 0; q < mean->n; q++) yexp__ref_text(c, mean->items[q].str, mean->items[q].pos, false, &items[q], elem, "mean");
                    out->items = items;
                    out->n_items = (int)mean->n;
                }
            }
        }
        if (kind == YEXP_TRACE) {
            const yexp_value* of = yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "of");
            if (of && of->kind == YEXP_V_ARRAY) {
                uint32_t q;
                yexp_ref* items = (yexp_ref*)yexp__alloc(c, (of->n ? of->n : 1) * sizeof *items);
                if (items) {
                    for (q = 0; q < of->n; q++) yexp__ref_text(c, of->items[q].str, of->items[q].pos, true, &items[q], elem, "of");
                    out->items = items;
                    out->n_items = (int)of->n;
                }
            }
            memset(&out->of, 0, sizeof out->of);
            out->of.index = -1;
            out->of.lane = -1;
        }
        if (kind == YEXP_REGION) {
            const yexp_value* cv = yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "center");
            memset(&out->center, 0, sizeof out->center);
            out->center.index = -1;
            out->center.lane = -1;
            if (cv && cv->kind == YEXP_V_SIGNAL) {
                out->center.name = cv->str;
                out->center.lane = (int)cv->i;
                out->center.pos_ = cv->pos;
            }
        }
        if (kind == YEXP_MOVING_TARGET) {
            static const char* const lanes[2] = { "x", "y" };
            int q;
            for (q = 0; q < 2; q++) {
                yexp__slot* x = yexp__slot_at_m(c, s, YEXP__S_CONTROL, mask, lanes[q]);
                yjs_value* cl = NULL;
                if (x && x->src && yexp__parse_sines(c, x->src, elem, lanes[q], q ? &out->sy : &out->sx, &cl)) yexp__put(c, cs, lanes[q], cl);
            }
            if (out->sx.n && out->sy.n && out->sx.n != out->sy.n)
                yexp__err(c, m->value->pos, "%s: 'x' has %d components and 'y' %d; the same n for both lanes (3.7)", elem, out->sx.n, out->sy.n);
        }
        /* the numbers of 3.16: above 0, or 0 and more (the rows refuse a
         * negative duration) */
        yexp__above0(c, yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "radius"), elem, "radius");
        yexp__above0(c, yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "onset"), elem, "onset");
        yexp__above0(c, yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "speed_below"), elem, "speed_below");
        yexp__above0(c, yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "lag_s"), elem, "lag_s");
        yexp__above0(c, yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "speed_lag_s"), elem, "speed_lag_s");
        yexp__above0(c, yexp__slotv_m(c, s, YEXP__S_CONTROL, mask, "after_s"), elem, "after_s");
        yexp__release(c, YEXP__S_CONTROL);
        yexp__put(c, cset, m->key, cs);
        out->failed_ = c->r->n_errors != before;
    }
}

static void yexp__parse_controls(yexp__ctx* c, const yjs_value* src, bool shared, const char* prefix, const yexp_control** out_c, int* out_n, yjs_value** canon) {
    yexp_control* out;
    yjs_value* cset;
    char sub[128];
    int n = 0, i = 0;
    size_t k;
    *out_n = 0;
    snprintf(sub, sizeof sub, "%scontrols", prefix);
    if (!yexp__set_begin(c, src, sub, YEXP__MAX_SET, &n)) { c->bad_sets |= YEXP__BAD_CONTROLS; return; }
    out = (yexp_control*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        yexp__parse_control(c, m, &out[i++], cset, shared, prefix);
    }
    *out_c = out;
    *out_n = i;
    *canon = cset;
}

/* The display's units as a signal role's (3.4): display_px for px. */
static int yexp__display_role_units(const yexp_def* d) {
    return d->units == 2 ? YEXP_UNITS_DISPLAY_CM : d->units == 1 ? YEXP_UNITS_DISPLAY_DEG : YEXP_UNITS_DISPLAY_PX;
}

static const char* const yexp__units_names[] = { "display_px", "display_cm", "display_deg", "workspace_cm", "device" };

/* A cursor's role against the display's units (3.4, 3.16; finding F7). */
static void yexp__cursor_units(yexp__ctx* c, const yexp_ref* r, const char* elem, const char* key) {
    const yexp_sigrole* sr;
    if (r->kind != YEXP_REF_ROLE) return;
    sr = &c->d->sigroles[r->index];
    if (!sr->failed_ && sr->units != yexp__display_role_units(c->d))
        yexp__err(c, r->pos_, "%s: '%s': the role '%s' is in %s and the display in %s; a cursor gives display positions, so its roles are in %s (3.4)",
                  elem, key, sr->name, yexp__units_names[sr->units], c->d->units == 2 ? "cm" : c->d->units == 1 ? "deg" : "px", yexp__units_names[yexp__display_role_units(c->d)]);
}

/* Every name of one scope's controls, now that the names are known (3.9). */
static void yexp__resolve_controls(yexp__ctx* c, const yexp_control* ctls, int n, const char* prefix) {
    int i, q;
    for (i = 0; i < n; i++) {
        yexp_control* x = (yexp_control*)(void*)&ctls[i];   /* this load's own */
        char elem[160];
        int before = c->r->n_errors;
        if (x->failed_) continue;
        snprintf(elem, sizeof elem, "%scontrol '%s'", prefix, x->name);
        switch (x->kind) {
        case YEXP_CURSOR:
            if (x->of.name && yexp__name_use(c, &x->of, YEXP__N_ROLE, elem, "role")) {
                if (x->of.lanes != 2) yexp__err(c, x->of.pos_, "%s: 'role': '%s' has %d lane%s; a cursor's role has 2 (3.7)", elem, x->of.name, x->of.lanes, x->of.lanes == 1 ? "" : "s");
                yexp__cursor_units(c, &x->of, elem, "role");
            }
            for (q = 0; q < 2; q++) {
                yexp_ref* r = q ? &x->y : &x->x;
                const char* key = q ? "y" : "x";
                if (!r->name) continue;
                if (r->lane < 0) { yexp__err(c, r->pos_, "%s: '%s' is one lane of a role: \"ROLE.x\" or \"ROLE.y\" (3.7)", elem, key); continue; }
                if (yexp__name_use(c, r, YEXP__N_ROLE, elem, key)) yexp__cursor_units(c, r, elem, key);
            }
            for (q = 0; q < x->n_items; q++) {
                yexp_ref* r = (yexp_ref*)(void*)&x->items[q];
                if (!r->name || !yexp__name_use(c, r, YEXP__N_ROLE, elem, "mean")) continue;
                yexp__cursor_units(c, r, elem, "mean");
                if (q > 0 && x->items[0].kind == YEXP_REF_ROLE && c->d->sigroles[r->index].type != c->d->sigroles[x->items[0].index].type)
                    yexp__err(c, r->pos_, "%s: 'mean': '%s' and '%s' are roles of two types; a mean takes roles of one type (3.7)", elem, x->items[0].name, r->name);
                else if (r->lanes != 2)
                    yexp__err(c, r->pos_, "%s: 'mean': '%s' has %d lane%s; a cursor's roles have 2 (3.7)", elem, r->name, r->lanes, r->lanes == 1 ? "" : "s");
            }
            break;
        case YEXP_REGION: case YEXP_DISTANCE:
            if (x->of.name && yexp__name_use(c, &x->of, YEXP__N_VALUE, elem, "of") && (x->of.lanes < 2 || x->of.lanes > 3))
                yexp__err(c, x->of.pos_, "%s: 'of': '%s' gives %d lane%s; a %s tests a value of 2 or 3 lanes (3.7)", elem, x->of.name, x->of.lanes, x->of.lanes == 1 ? "" : "s", yexp__ctl_kinds[(int)x->kind]);
            if (x->center.name && yexp__name_use(c, &x->center, YEXP__N_VALUE, elem, "center")) {
                const yexp_prop* cp = yexp_prop_get(x->props, x->n_props, "center");
                if (cp && cp->value->kind == YEXP_V_SIGNAL) ((yexp_value*)(void*)cp->value)->n = (uint32_t)x->center.lanes;   /* this load's own */
                if (x->of.kind != YEXP_REF_NONE && x->center.lanes != x->of.lanes)
                    yexp__err(c, x->center.pos_, "%s: 'center': '%s' gives %d lane%s and 'of' %d; the center has the lanes of 'of' (D28)", elem, x->center.name, x->center.lanes, x->center.lanes == 1 ? "" : "s", x->of.lanes);
            }
            break;
        case YEXP_MOVEMENT:
            if (x->of.name && yexp__name_use(c, &x->of, YEXP__N_VALUE, elem, "of") && x->of.lanes != 2)
                yexp__err(c, x->of.pos_, "%s: 'of': '%s' gives %d lane%s; a movement reads a value of 2 lanes (3.7)", elem, x->of.name, x->of.lanes, x->of.lanes == 1 ? "" : "s");
            break;
        case YEXP_PEAK:
            if (x->of.name) yexp__name_use(c, &x->of, YEXP__N_VALUE, elem, "of");
            break;
        case YEXP_SIGNAL_LOST:
            if (x->of.name) yexp__name_use(c, &x->of, YEXP__N_ROLE, elem, "of");
            break;
        case YEXP_TRACE: {
            int lanes = 0;
            const yexp_prop* rate = yexp_prop_get(x->props, x->n_props, "rate");
            for (q = 0; q < x->n_items; q++) {
                yexp_ref* r = (yexp_ref*)(void*)&x->items[q];
                if (r->name && yexp__name_use(c, r, YEXP__N_VALUE, elem, "of")) lanes += r->lanes;
            }
            if (rate && rate->value->str && yexp__streq(rate->value->str, "sample") && lanes > 4)
                yexp__err(c, x->pos_, "%s: a sample trace has at most 4 lanes in all (a join); its items have %d. Use \"rate\": \"frame\" (D38)", elem, lanes);
            if (x->to.name) yexp__name_use(c, &x->to, YEXP__N_TIME | YEXP__N_RESPONSE | YEXP__N_END, elem, "to");
            break;
        }
        default: break;
        }
        if (x->kind == YEXP_MOVING_TARGET) {
            if (x->from.name) yexp__name_use(c, &x->from, YEXP__N_STATE, elem, "from");
        } else if (x->from.name) {
            yexp__name_use(c, &x->from, YEXP__N_TIME | YEXP__N_RESPONSE, elem, "from");
        }
        if (x->freeze_at.name) yexp__name_use(c, &x->freeze_at, YEXP__N_HITS, elem, "freeze_at");
        if (x->stimulus.name) yexp__name_use(c, &x->stimulus, YEXP__N_STIM, elem, "stimulus");
        if (c->r->n_errors != before) x->failed_ = true;
    }
}

/* A control reads another through these names; the graph has no cycle. */
static int yexp__ctl_deps(const yexp_control* x, const yexp_control** out) {
    int n = 0, q;
    const yexp_ref* one[5];
    one[0] = &x->of; one[1] = &x->center; one[2] = &x->freeze_at; one[3] = &x->from; one[4] = &x->to;
    for (q = 0; q < 5; q++)
        if (one[q]->kind == YEXP_REF_CONTROL && one[q]->control) out[n++] = one[q]->control;
    for (q = 0; q < x->n_items && n < 64; q++)
        if (x->items[q].kind == YEXP_REF_CONTROL && x->items[q].control) out[n++] = x->items[q].control;
    return n;
}

/* Format finding F9: 3.16 lists no refusal for controls that read each
 * other in a loop, and no graph can open with one. Depth first over the
 * template's controls and the top-level ones; color: 0 new, 1 on the path,
 * 2 done. */
static void yexp__ctl_cycles(yexp__ctx* c, const yexp_control* tc, int nt, const char* prefix) {
    const yexp_def* d = c->d;
    int total = d->n_controls + nt, i;
    uint8_t* color;
    int* stack;
    int* next;
    if (total == 0) return;
    color = (uint8_t*)yexp__alloc(c, (size_t)total);
    stack = (int*)yexp__alloc(c, (size_t)total * sizeof(int));
    next = (int*)yexp__alloc(c, (size_t)total * sizeof(int));
    if (!color || !stack || !next) return;
    memset(color, 0, (size_t)total);
    for (i = 0; i < total; i++) {
        int sp = 0;
        if (color[i]) continue;
        stack[sp++] = i;
        next[i] = 0;
        color[i] = 1;
        while (sp) {
            int u = stack[sp - 1];
            const yexp_control* x = u < d->n_controls ? &d->controls[u] : &tc[u - d->n_controls];
            const yexp_control* dep[72];
            int nd = yexp__ctl_deps(x, dep);
            if (!yexp__work(c)) return;
            if (next[u] < nd) {
                const yexp_control* y = dep[next[u]++];
                int v = (y >= d->controls && y < d->controls + d->n_controls) ? (int)(y - d->controls) : d->n_controls + (int)(y - tc);
                if (v < 0 || v >= total) continue;
                if (color[v] == 1) {
                    yexp__err(c, x->pos_, "%scontrol '%s': it reads '%s', which reads it again through its names; controls cannot read each other in a loop", prefix, x->name, y->name);
                    return;
                }
                if (color[v] == 0) { color[v] = 1; next[v] = 0; stack[sp++] = v; }
            } else {
                color[u] = 2;
                sp--;
            }
        }
    }
}

/* The sets come from sorted JSON objects, so they are sorted by name. */
#define YEXP__FIND(arr, count, want) do {                                  \
        int lo_ = 0, hi_ = (count);                                        \
        while (lo_ < hi_) {                                                \
            int mid_ = lo_ + (hi_ - lo_) / 2;                              \
            int cmp_ = strcmp((arr)[mid_].name, (want));                   \
            if (cmp_ == 0) return &(arr)[mid_];                            \
            if (cmp_ < 0) lo_ = mid_ + 1;                                  \
            else hi_ = mid_;                                               \
        }                                                                  \
        return NULL;                                                       \
    } while (0)

static const yexp_role* yexp__role_get(const yexp_def* d, const char* name) {
    YEXP__FIND(d->roles, d->n_roles, name);
}

/* --- stimuli ------------------------------------------------------------ */

static const char* const yexp__kinds[] = { "shape", "grating", "gabor", "noise", "image", "text", "artwork", "rdk", "sound", "video" };

static void yexp__parse_stimulus(yexp__ctx* c, const yjs_member* m, yexp_stim* out, yjs_value* cset) {
    char elem[96];
    const yjs_value* kv;
    int kind = -1, i, f = c->first[YEXP__S_STIM], rows = c->count[YEXP__S_STIM], np = 0, nd = 0;
    unsigned mask;
    yexp__slot* s;
    yjs_value* cs = NULL;
    yexp_prop* props;
    int* dyn;
    snprintf(elem, sizeof elem, "stimulus '%s'", m->key);
    if (m->value->type != YJS_OBJECT) { yexp__err(c, m->value->pos, "%s: wrong type; expected an object", elem); return; }
    kv = yjs_get(m->value, "kind");
    if (!kv) { yexp__err(c, m->value->pos, "%s: 'kind' is required", elem); return; }
    if (kv->type == YJS_STRING)
        for (i = 0; i < 10; i++)
            if (yexp__streq(kv->u.s, yexp__kinds[i])) kind = i;
    if (kind < 0) {
        yexp__err(c, kv->pos, "%s: 'kind': not one of: shape, grating, gabor, noise, image, text, artwork, rdk, sound, video", elem);
        return;
    }
    mask = 1u << kind;
    s = yexp__object(c, YEXP__S_STIM, mask, m->value, elem, &cs);
    if (!s) return;
    for (i = 0; i < rows; i++)
        if (c->rows[f + i].kinds & mask) np++;
    props = (yexp_prop*)yexp__alloc(c, (size_t)np * sizeof *props);
    /* version 2: `visible` is a step's property, no stimulus key (D41) */
    dyn = (int*)yexp__alloc(c, (size_t)np * sizeof *dyn);
    np = 0;
    if (!props || !dyn) { yexp__release(c, YEXP__S_STIM); return; }
    out->name = m->key;
    out->pos_ = m->key_pos;
    out->kind = (yexp_stim_kind)kind;
    out->layer = 0;
    for (i = 0; i < rows; i++) {
        const yexp_row* r = &c->rows[f + i];
        if (!(r->kinds & mask)) continue;
        if (yexp__streq(r->key, "kind")) continue;
        if (yexp__streq(r->key, "layer")) {
            out->layer = yexp__int_field(c, s[i].v, elem, "layer", INT32_MIN, INT32_MAX, 0);
            continue;
        }
        if (!s[i].v || (r->flags & YEXP__R_STEP)) continue;
        props[np].key = r->key;
        props[np].value = s[i].v;
        props[np].given = s[i].given;
        if (s[i].v->kind != YEXP_V_NUMBER && s[i].v->kind != YEXP_V_BOOL && s[i].v->kind != YEXP_V_STRING && s[i].v->kind != YEXP_V_ARRAY)
            dyn[nd++] = np;
        np++;
    }
    out->props = props;
    out->n_props = np;
    out->dyn_ = dyn;
    out->n_dyn_ = nd;
    if (kind == YEXP_TEXT) {
        const yexp_value* tv = yexp__slotv(c, s, YEXP__S_STIM, "text");
        if (tv && tv->kind == YEXP_V_VAR) {
            bool session = false;
            int j;
            for (j = 0; j < c->d->n_fields; j++) if (yexp__streq(c->d->fields[j].name, tv->str)) session = true;
            if (yexp__is_builtin(tv->str))
                yexp__err(c, tv->pos, "%s: 'text': '$%s' is a built-in variable, whose text is not known at pack build; give each text with a lookup: {\"by\": \"$%s\", \"map\": {...}}", elem, tv->str, tv->str);
            else if (session)
                yexp__err(c, tv->pos, "%s: 'text': '$%s' is a session field, given at session start; its text cannot be laid out at pack build", elem, tv->str);
        }
    }
    yexp__put(c, cset, m->key, cs);
    yexp__release(c, YEXP__S_STIM);
}

static void yexp__parse_stimuli(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_stim* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!src) return;
    if (!yexp__set_begin(c, src, "stimuli", YEXP__MAX_SET, &n)) { c->bad_sets |= YEXP__BAD_STIMULI; return; }
    out = (yexp_stim*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        int before = c->r->n_errors;
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        /* a refused stimulus keeps its name, so a reference to it is no second error */
        out[i].name = m->key;
        out[i].pos_ = m->key_pos;
        yexp__parse_stimulus(c, m, &out[i], cset);
        out[i].failed_ = c->r->n_errors != before;
        i++;
    }
    c->d->stimuli = out;
    c->d->n_stimuli = i;
    *canon = cset;
}

YEXP_API const yexp_stim* yexp_stim_get(const yexp_def* d, const char* name) {
    if (!d || !name) return NULL;
    YEXP__FIND(d->stimuli, d->n_stimuli, name);
}

YEXP_API const yexp_prop* yexp_prop_get(const yexp_prop* props, int n, const char* key) {
    int i;
    if (!props || !key) return NULL;
    for (i = 0; i < n; i++)
        if (yexp__streq(props[i].key, key)) return &props[i];
    return NULL;
}

/* --- methods ------------------------------------------------------------ */

static void yexp__parse_method(yexp__ctx* c, const yjs_member* m, yexp_method* out, yjs_value* cset) {
    char elem[96];
    const yjs_value* tv;
    int sect, i, np = 0;
    yexp_method_type mtype;
    yexp__slot* s;
    yjs_value* cm = NULL;
    yexp_prop* props;
    int f, rows;
    snprintf(elem, sizeof elem, "method '%s'", m->key);
    if (m->value->type != YJS_OBJECT) { yexp__err(c, m->value->pos, "%s: wrong type; expected an object", elem); return; }
    tv = yjs_get(m->value, "type");
    if (!tv) { yexp__err(c, m->value->pos, "%s: 'type' is required", elem); return; }
    if (tv->type == YJS_STRING && yexp__streq(tv->u.s, "stair")) { sect = YEXP__S_STAIR; mtype = YEXP_STAIR; }
    else if (tv->type == YJS_STRING && yexp__streq(tv->u.s, "quest")) { sect = YEXP__S_QUEST; mtype = YEXP_QUEST; }
    else if (tv->type == YJS_STRING && yexp__streq(tv->u.s, "aep")) { sect = YEXP__S_AEP; mtype = YEXP_AEP; }
    else { yexp__err(c, tv->pos, "%s: 'type': not one of: stair, quest, aep", elem); return; }
    s = yexp__object(c, sect, 0xffffu, m->value, elem, &cm);
    if (!s) return;
    f = c->first[sect];
    rows = c->count[sect];
    props = (yexp_prop*)yexp__alloc(c, (size_t)rows * sizeof *props);
    if (!props) { yexp__release(c, sect); return; }
    out->name = m->key;
    out->type = mtype;
    for (i = 0; i < rows; i++) {
        const yexp_row* r = &c->rows[f + i];
        if (yexp__streq(r->key, "type")) continue;
        if (yexp__streq(r->key, "scale")) { out->log10 = s[i].v && yexp__streq(s[i].v->str, "log10"); continue; }
        if (yexp__streq(r->key, "estimate")) { out->estimate = s[i].v ? s[i].v->str : NULL; continue; }
        if (yexp__streq(r->key, "dims")) {
            out->dims = yexp__str_array(c, s[i].v, &out->n_dims);
            if (s[i].v) {
                int k;
                for (k = 0; k < out->n_dims; k++)
                    if (yexp__is_builtin(out->dims[k]))
                        yexp__err(c, s[i].pos, "%s: 'dims': '%s' is a built-in variable's name", elem, out->dims[k]);
            }
            continue;
        }
        if (!s[i].v) continue;
        props[np].key = r->key;
        props[np].value = s[i].v;
        props[np].given = s[i].given;
        np++;
    }
    out->props = props;
    out->n_props = np;
    if (mtype == YEXP_STAIR) {
        const yexp_prop* st = yexp_prop_get(props, np, "steps");
        if (st && st->value->n == 0) yexp__err(c, st->value->pos, "%s: 'steps' has at least one step", elem);
    } else if (mtype == YEXP_AEP) {
        const yexp_prop* lo = yexp_prop_get(props, np, "lo");
        const yexp_prop* hi = yexp_prop_get(props, np, "hi");
        int a, b;
        if (lo && hi && (lo->value->n != hi->value->n || lo->value->n == 0))
            yexp__err(c, m->value->pos, "%s: 'lo' and 'hi' have one number per dimension, and at least one", elem);
        if (lo && out->dims && (uint32_t)out->n_dims != lo->value->n)
            yexp__err(c, m->value->pos, "%s: 'dims' names %d dimensions; 'lo' has %u", elem, out->n_dims, (unsigned)lo->value->n);
        for (a = 0; a < out->n_dims; a++)
            for (b = 0; b < a; b++)
                if (yexp__streq(out->dims[a], out->dims[b])) yexp__err(c, m->value->pos, "%s: 'dims': '%s' named twice", elem, out->dims[a]);
    }
    yexp__put(c, cset, m->key, cm);
    yexp__release(c, sect);
}

static void yexp__parse_methods(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_method* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!src) return;
    if (!yexp__set_begin(c, src, "methods", YEXP__MAX_SET, &n)) { c->bad_sets |= YEXP__BAD_METHODS; return; }
    out = (yexp_method*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        out[i].name = m->key;
        yexp__parse_method(c, m, &out[i], cset);
        i++;
    }
    c->d->methods = out;
    c->d->n_methods = i;
    *canon = cset;
}

static const yexp_method* yexp__method_get(const yexp_def* d, const char* name) {
    YEXP__FIND(d->methods, d->n_methods, name);
}

/* --- steps -------------------------------------------------------------- */

static const char* const yexp__verbs[] = { "on", "off", "wait_s", "at_s", "set", "to", "then", "trigger", "mark", "if", "unless",
                                           "until", "recenter", "flag" };
/* version 1 has the first 11 */
#define YEXP__N_VERBS (c->version >= 2 ? 14 : 11)

/* Whether a name is a control's (version 2), so a step's message can say
 * that a step cannot name one (3.12). */
static bool yexp__is_control_name(const yexp__ctx* c, const char* name) {
    if (c->version < 2) return false;
    return (c->tp && yexp__ctl_find(c->tp->controls, c->tp->n_controls, name)) || yexp__ctl_find(c->d->controls, c->d->n_controls, name);
}

/* Whether this name was already seen in the list numbered c->gen. */
static bool yexp__stim_stamped(yexp__ctx* c, const char* name) {
    const yexp_stim* st = yexp_stim_get(c->d, name);
    uint32_t* u;
    if (!st || !c->stim_stamp) return false;
    u = &c->stim_stamp[st - c->d->stimuli];
    if (*u == c->gen) return true;
    *u = c->gen;
    return false;
}

/* The stimulus keys a step may set or tween: a timeline channel carries a
 * float, so only the kind's numeric properties; a resource, a layout key
 * or an enum is fixed at load. */
static bool yexp__tweenable(const yexp_row* r) {
    /* `ygfx_bind` reaches gfx's float fields and rdk's three update floats;
     * these three T_NUM rows are read once, by an open or a play */
    if (yexp__streq(r->key, "density") || yexp__streq(r->key, "lead") || yexp__streq(r->key, "db")) return false;
    return (r->type == YEXP__T_NUM || r->type == YEXP__T_VEC3) && !(r->flags & YEXP__R_LIT);
}

/* The names in a value's forms (3.10, 3.15), resolved in the template being
 * read. place: the lanes the place takes (1; 3 for color). false after a
 * diagnostic. */
static bool yexp__forms_resolve(yexp__ctx* c, const yexp_value* cv, int place, const char* elem, const char* key) {
    yexp_value* v = (yexp_value*)(void*)cv;   /* the form's own, made in this load */
    yexp_ref r;
    if (!v) return true;
    memset(&r, 0, sizeof r);
    r.name = v->str;
    r.pos_ = v->pos;
    r.lane = -1;
    switch (v->kind) {
    case YEXP_V_SIGNAL:
        r.lane = (int)v->i;
        if (!yexp__name_use(c, &r, YEXP__N_VALUE, elem, key)) return false;
        if (v->by) {
            if (r.lanes != 1) {
                yexp__err(c, v->pos, "%s: '%s': 'above' on '%s', which has %d lanes; name one lane, '%s.x' (3.10)", elem, key, r.name, r.lanes, r.name);
                return false;
            }
        } else if (place == 1) {
            if (!yexp__one_lane(c, &r, elem, key)) return false;
        } else if (r.lanes != place) {
            /* format finding F10: a signal fills every lane of its place */
            yexp__err(c, v->pos, "%s: '%s': '%s' gives %d lane%s; this place takes %d (3.10)", elem, key, r.name, r.lanes, r.lanes == 1 ? "" : "s", place);
            return false;
        }
        v->n = (uint32_t)r.lanes;
        return true;
    case YEXP_V_TEST:
        return yexp__name_use(c, &r, YEXP__N_TEST, elem, key);
    case YEXP_V_WHEN:
        return yexp__forms_resolve(c, v->by, 1, elem, key) && yexp__forms_resolve(c, &v->items[0], place, elem, key) &&
               yexp__forms_resolve(c, &v->items[1], place, elem, key);
    case YEXP_V_HIT: {
        int lanes;
        if (!yexp__name_use(c, &r, YEXP__N_HITS, elem, key)) return false;
        /* the lane is of the control's `of`: the value at the hit (3.15) */
        lanes = r.control->of.lanes;
        if (v->i >= lanes) {
            yexp__err(c, v->pos, "%s: '%s': the 'of' of '%s' has %d lane%s; '.%c' is not one of them (3.15)", elem, key, r.name, lanes, lanes == 1 ? "" : "s", "xyzw"[v->i]);
            return false;
        }
        if (v->i < 0 && lanes > 1) {
            yexp__err(c, v->pos, "%s: '%s': the 'of' of '%s' has %d lanes and a column takes one; name a lane, '%s.x' (3.15)", elem, key, r.name, lanes, r.name);
            return false;
        }
        v->n = 1;
        return true;
    }
    case YEXP_V_TIME: {
        yexp_ref r2;
        memset(&r2, 0, sizeof r2);
        r2.name = v->by->str;
        r2.pos_ = v->by->pos;
        r2.lane = -1;
        return yexp__name_use(c, &r, YEXP__N_TIME | YEXP__N_RESPONSE, elem, key) && yexp__name_use(c, &r2, YEXP__N_TIME | YEXP__N_RESPONSE, elem, "from");
    }
    default:
        return true;
    }
}

/* Whether a value holds a form of 3.10 (a constant needs no selector). */
static bool yexp__is_form(const yexp_value* v) {
    return v && (v->kind == YEXP_V_SIGNAL || v->kind == YEXP_V_WHEN);
}

static void yexp__parse_props(yexp__ctx* c, const yjs_value* props, const yexp_stim* st, const char* elem, yexp_step* out, yjs_value** canon, bool forms) {
    yexp_prop* pr;
    yjs_value* cp;
    size_t k;
    int n = 0;
    unsigned mask = 1u << (int)st->kind;
    if (props->type != YJS_OBJECT || props->n == 0) {
        yexp__err(c, props->pos, "%s: 'props' is an object of properties with at least one key", elem);
        return;
    }
    if (!yexp__note_ok(c, props, elem)) return;
    pr = (yexp_prop*)yexp__alloc(c, props->n * sizeof *pr);
    cp = yexp__cnew(c, YJS_OBJECT);
    if (!pr || !cp) return;
    for (k = 0; k < props->n; k++) {
        const yjs_member* m = &props->u.members[k];
        int ri;
        yexp__slot slot;
        yjs_value* cv = NULL;
        if (yexp__streq(m->key, "note")) {
            yexp__put(c, cp, "note", yexp__cstr(c, m->value->u.s));
            continue;
        }
        ri = yexp__find(c, YEXP__S_STIM, mask, m->key);
        if (ri < 0) {
            if (yexp__streq(m->key, "color_r")) { yexp__unknown(c, YEXP__S_STIM, mask, m, elem); continue; }
            yexp__err(c, m->key_pos, "%s: property '%s' is not a property of the %s stimulus '%s'", elem, m->key, yexp__kinds[(int)st->kind], st->name);
            continue;
        }
        if (!yexp__tweenable(&c->rows[ri])) {
            yexp__err(c, m->key_pos, "%s: property '%s' of '%s' is fixed at load; a step cannot change it", elem, m->key, st->name);
            continue;
        }
        memset(&slot, 0, sizeof slot);
        if (forms) c->extra_allow = YEXP__AV_SIG | YEXP__AV_WHEN;
        if (!yexp__row_value(c, ri, m, elem, &slot, &cv)) { c->extra_allow = 0; continue; }
        c->extra_allow = 0;
        if (forms && yexp__is_form(slot.v) && !yexp__forms_resolve(c, slot.v, c->rows[ri].type == YEXP__T_VEC3 ? 3 : 1, elem, m->key)) continue;
        if (c->version >= 2 && (yexp__streq(m->key, "x") || yexp__streq(m->key, "y"))) {
            /* checked against the controls' stimuli at the template's end */
            yexp__xy* xy = (yexp__xy*)yexp__alloc(c, sizeof *xy);
            if (xy) {
                xy->stim = (int)(st - c->d->stimuli);
                xy->pos = m->key_pos;
                xy->key = c->rows[ri].key;
                snprintf(xy->elem, sizeof xy->elem, "%s", elem);
                xy->next = c->xy;
                c->xy = xy;
            }
        }
        pr[n].key = c->rows[ri].key;
        pr[n].value = slot.v;
        pr[n].given = true;
        n++;
        yexp__put(c, cp, m->key, cv);
    }
    out->props = pr;
    out->n_props = n;
    *canon = cp;
}

static int yexp__parse_steps(yexp__ctx* c, const yjs_value* src, const char* prefix, int depth, const yexp_step** steps, int* n_steps, yjs_value** canon);

static void yexp__parse_step(yexp__ctx* c, const yjs_value* src, const char* elem, int depth, yexp_step* out, yjs_value** canon) {
    int verb = -1, k;
    size_t i;
    yexp__slot* s;
    yjs_value* cs = NULL;
    unsigned mask;
    bool recenter = c->version >= 2 && yjs_get(src, "recenter") != NULL;
    if (src->type != YJS_OBJECT) { yexp__err(c, src->pos, "%s: wrong type; expected a step object", elem); return; }
    for (i = 0; i < src->n; i++)
        for (k = 0; k < YEXP__N_VERBS; k++)
            /* recenter's point is its 'to', not a tween's verb */
            if (yexp__streq(src->u.members[i].key, yexp__verbs[k]) && !(recenter && k == YEXP_TO)) {
                if (verb >= 0) {
                    yexp__err(c, src->u.members[i].key_pos, "%s: a step has one verb; found '%s' and '%s'", elem, yexp__verbs[verb], yexp__verbs[k]);
                    return;
                }
                verb = k;
            }
    if (verb < 0) {
        /* no verb: name what is there, so a v2 step or a key without its
         * suffix gets its own message */
        for (i = 0; i < src->n; i++)
            if (!yexp__streq(src->u.members[i].key, "note")) {
                yexp__unknown(c, YEXP__S_STEP, 0xffffu, &src->u.members[i], elem);
                return;
            }
        yexp__err(c, src->pos, "%s: an empty step", elem);
        return;
    }
    mask = 1u << verb;
    s = yexp__object(c, YEXP__S_STEP, mask, src, elem, &cs);
    if (!s) return;
    out->op = (yexp_step_op)verb;
    switch (verb) {
    case YEXP_ON: case YEXP_OFF: {
        const yexp_value* tg = yexp__slotv(c, s, YEXP__S_STEP, yexp__verbs[verb]);
        out->stims = yexp__str_array(c, tg, &out->n_stims);
        out->stagger = yexp__slotv(c, s, YEXP__S_STEP, "stagger_s");
        {
            const yexp__slot* st = yexp__slot_at(c, s, YEXP__S_STEP, "stagger_s");
            /* a stagger has no effect on one stimulus: it is dropped, so a
             * written one and none give one form and one ID */
            if (st && st->given && tg) {
                const yexp_value* sv = yexp__slotv(c, s, YEXP__S_STEP, "stagger_s");
                int ri = yexp__find(c, YEXP__S_STEP, mask, "stagger_s");
                const yexp_value* d = ri >= 0 ? yexp__default(c, ri) : NULL;
                if (tg->n < 2) out->stagger = d;
                else if (sv && st->src && !(d && yexp__same(sv, d))) yexp__put(c, cs, "stagger_s", (yjs_value*)st->src);
            }
        }
        {
            int a;
            c->gen++;
            for (a = 0; a < out->n_stims; a++) {
                const char* nm = out->stims[a];
                if (!yexp_stim_get(c->d, nm)) {
                    if (yexp__is_control_name(c, nm)) yexp__err(c, tg->pos, "%s: '%s': '%s' is a control; a step cannot name a control (3.12)", elem, yexp__verbs[verb], nm);
                    else if (!(c->bad_sets & YEXP__BAD_STIMULI)) yexp__err(c, tg->pos, "%s: '%s': no stimulus named '%s'", elem, yexp__verbs[verb], nm);
                } else if (yexp__stim_stamped(c, nm)) {
                    yexp__err(c, tg->pos, "%s: '%s': '%s' named twice", elem, yexp__verbs[verb], nm);
                }
            }
        }
        break;
    }
    case YEXP_WAIT: case YEXP_AT:
        out->t = yexp__slotv(c, s, YEXP__S_STEP, yexp__verbs[verb]);
        /* 3.11 rule 6: in events at_s is trial time, and the time an until
         * splits at is not known */
        if (verb == YEXP_AT && c->step_place == 0 && c->until_seen)
            yexp__err(c, src->pos, "%s: 'at_s' after an 'until' in 'events': the time the until splits at is not known (3.11 rule 6); put the steps in states", elem);
        break;
    case YEXP_SET: case YEXP_TO: case YEXP_THEN: {
        const char* name = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, yexp__verbs[verb]));
        const yexp_stim* st = name ? yexp_stim_get(c->d, name) : NULL;
        yexp__slot* ps = yexp__slot_at(c, s, YEXP__S_STEP, "props");
        const char** names = (const char**)yexp__alloc(c, sizeof(char*));
        if (names && name) { names[0] = name; out->stims = names; out->n_stims = 1; }
        if (!st && name && yexp__is_control_name(c, name)) yexp__err(c, src->pos, "%s: '%s': '%s' is a control; a step cannot name a control (3.12)", elem, yexp__verbs[verb], name);
        else if (!st && name && !(c->bad_sets & YEXP__BAD_STIMULI)) yexp__err(c, src->pos, "%s: '%s': no stimulus named '%s'", elem, yexp__verbs[verb], name);
        if (verb != YEXP_SET) {
            out->duration = yexp__slotv(c, s, YEXP__S_STEP, "duration_s");
            out->delay = yexp__slotv(c, s, YEXP__S_STEP, "delay_s");
            out->ease = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, "ease"));
        }
        if (st && !st->failed_ && ps && ps->src) {
            yjs_value* cp = NULL;
            yexp__parse_props(c, ps->src, st, elem, out, &cp, c->version >= 2 && verb == YEXP_SET);
            yexp__put(c, cs, "props", cp);
        }
        break;
    }
    case YEXP_TRIGGER: {
        const char* role = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, "role"));
        out->code = yexp__slotv(c, s, YEXP__S_STEP, "trigger");
        out->role = role;
        if (role) {
            const yexp_role* r = yexp__role_get(c->d, role);
            if (!r) { if (!(c->bad_sets & YEXP__BAD_ROLES)) yexp__err(c, src->pos, "%s: 'role': no role named '%s'", elem, role); }
            else if (!r->failed_ && r->direction == 0) yexp__err(c, src->pos, "%s: 'role': '%s' is an input role; a trigger needs an output", elem, role);
        }
        break;
    }
    case YEXP_MARK:
        out->mark = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, "mark"));
        if (out->mark && !out->mark[0]) yexp__err(c, src->pos, "%s: 'mark' is a name, not empty", elem);
        break;
    case YEXP_IF: case YEXP_UNLESS: {
        yexp__slot* st = yexp__slot_at(c, s, YEXP__S_STEP, "steps");
        out->cond = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, yexp__verbs[verb]));
        out->cond_pos_ = yexp__slot_at(c, s, YEXP__S_STEP, yexp__verbs[verb])->pos;
        if (st && st->src) {
            yjs_value* ca = NULL;
            yexp__parse_steps(c, st->src, elem, depth + 1, &out->steps, &out->n_steps, &ca);
            yexp__put(c, cs, "steps", ca);
        }
        break;
    }
    case YEXP_UNTIL: {
        const yexp__slot* us = yexp__slot_at_m(c, s, YEXP__S_STEP, mask, "until");
        const yexp_value* to = yexp__slotv_m(c, s, YEXP__S_STEP, mask, "timeout_s");
        const char* nm = yexp__v_str(us ? us->v : NULL);
        /* format finding F4: in events or a state's own steps, not in an if
         * body or in after (3.11 rule 9) */
        if (depth > 0 || c->step_place == 1) {
            yexp__err(c, src->pos, "%s: 'until' goes in 'events' or in a state's steps, not %s (3.11 rule 9)", elem, depth > 0 ? "inside an 'if' or 'unless'" : "in 'after'");
            break;
        }
        c->until_seen = 1;
        if (nm && yexp__ref_text(c, nm, us->pos, false, &out->target, elem, "until"))
            yexp__name_use(c, &out->target, YEXP__N_HITS, elem, "until");
        if (to && to->kind == YEXP_V_NUMBER && to->i <= 0) yexp__err(c, to->pos, "%s: 'timeout_s' is above 0", elem);
        out->timeout = to;
        break;
    }
    case YEXP_RECENTER: {
        const yexp__slot* rs = yexp__slot_at_m(c, s, YEXP__S_STEP, mask, "recenter");
        const char* nm = yexp__v_str(rs ? rs->v : NULL);
        out->point = yexp__slotv_m(c, s, YEXP__S_STEP, mask, "to");
        if (nm && yexp__ref_text(c, nm, rs->pos, false, &out->target, elem, "recenter") &&
            yexp__name_use(c, &out->target, YEXP__N_ROLE, elem, "recenter")) {
            const yexp_sigrole* sr = &c->d->sigroles[out->target.index];
            /* D19's note: the loader cannot know the adapter, so it refuses
             * only a role that cannot be relative; the player skips the step
             * on an absolute adapter and counts it */
            if (!sr->failed_ && !(sr->type == YEXP_SIG_POSITION2D_RELATIVE || (sr->type == YEXP_SIG_POSITION2D && sr->allow_relative)))
                yexp__err(c, rs->pos, "%s: 'recenter': the signal role '%s' cannot be relative; recenter needs type position2d_relative, or position2d with allow_relative (D19)", elem, nm);
        }
        break;
    }
    case YEXP_FLAG: {
        const yexp__slot* fs = yexp__slot_at_m(c, s, YEXP__S_STEP, mask, "flag");
        const yexp_value* fv = yexp__slotv_m(c, s, YEXP__S_STEP, mask, "value");
        out->flag = yexp__v_str(fs ? fs->v : NULL);
        out->flag_value = yexp__v_bool(fv, false);
        if (out->flag && yexp__kfind(c->fl_idx, c->n_fl, out->flag) < 0)
            yexp__err(c, fs->pos, "%s: 'flag': the template's 'flags' has no '%s'", elem, out->flag);
        break;
    }
    default: break;
    }
    yexp__release(c, YEXP__S_STEP);
    *canon = cs;
}

static int yexp__parse_steps(yexp__ctx* c, const yjs_value* src, const char* prefix, int depth, const yexp_step** steps, int* n_steps, yjs_value** canon) {
    yexp_step* out;
    yjs_value* carr;
    size_t i;
    *n_steps = 0;
    if (depth > YEXP__MAX_DEPTH) { yexp__err(c, src->pos, "%s: steps nest too deep", prefix); return -1; }
    if (src->type != YJS_ARRAY) { yexp__err(c, src->pos, "%s: wrong type; expected an array of steps", prefix); return -1; }
    if (src->n > YEXP__MAX_LIST) { yexp__err(c, src->pos, "%s: more than %d steps", prefix, YEXP__MAX_LIST); return -1; }
    out = (yexp_step*)yexp__alloc(c, (src->n ? src->n : 1) * sizeof *out);
    carr = yexp__cnew(c, YJS_ARRAY);
    if (!out || !carr) return -1;
    for (i = 0; i < src->n; i++) {
        char elem[128];
        yjs_value* cs = NULL;
        snprintf(elem, sizeof elem, "%s[%u]", prefix, (unsigned)i);
        yexp__parse_step(c, src->u.items[i], elem, depth, &out[i], &cs);
        if (cs) yexp__cpush(c, carr, cs);
    }
    *steps = out;
    *n_steps = (int)src->n;
    *canon = carr;
    return 0;
}

/* --- templates ---------------------------------------------------------- */

static int yexp__keyed_cmp(const void* a, const void* b) {
    const yexp__keyed* x = (const yexp__keyed*)a;
    const yexp__keyed* y = (const yexp__keyed*)b;
    int cmp = strcmp(x->key, y->key);
    return cmp ? cmp : x->index - y->index;
}

/* --- states and exits (version 2, format 3.11) -------------------------- */

static bool yexp__parse_exit(yexp__ctx* c, const yjs_value* src, bool shared, const char* elem, yexp_exit* out, yjs_value** canon) {
    unsigned mask = shared ? 2u : 1u;
    yexp__slot* s;
    yjs_value* ce = NULL;
    const yexp__slot* on;
    const yexp__slot* to;
    const yexp__slot* iff;
    int before = c->r->n_errors;
    memset(out, 0, sizeof *out);
    out->on.index = out->to.index = -1;
    out->on.lane = out->to.lane = -1;
    out->pos_ = src->pos;
    s = yexp__object(c, YEXP__S_EXIT, mask, src, elem, &ce);
    if (!s) return false;
    on = yexp__slot_at_m(c, s, YEXP__S_EXIT, mask, "on");
    to = yexp__slot_at_m(c, s, YEXP__S_EXIT, mask, "to");
    iff = yexp__slot_at_m(c, s, YEXP__S_EXIT, mask, "if");
    out->after = yexp__slotv_m(c, s, YEXP__S_EXIT, mask, "after_s");
    out->outcome = yexp__slotv_m(c, s, YEXP__S_EXIT, mask, "outcome");
    if (on && on->v && yexp__ref_text(c, on->v->str, on->pos, false, &out->on, elem, "on"))
        yexp__name_use(c, &out->on, YEXP__N_HITS | YEXP__N_RESPONSE, elem, "on");
    if (out->after && out->after->kind == YEXP_V_NUMBER && out->after->i <= 0)
        yexp__err(c, out->after->pos, "%s: 'after_s' is above 0 (D20 rule 2)", elem);
    if (on && on->v && out->after)
        yexp__err(c, src->pos, "%s: an exit has 'on' or 'after_s', not both (D20 rule 2)", elem);
    if (iff && iff->v) {
        out->if_var = iff->v->str;
        out->if_pos_ = iff->pos;
        out->if_flag = yexp__kfind(c->fl_idx, c->n_fl, out->if_var) >= 0;
        if ((on && on->v) || out->after) yexp__err(c, iff->pos, "%s: 'if' is for an instant exit, one with no 'on' and no 'after_s' (D20 rule 2)", elem);
    }
    if (to && to->v && yexp__ref_text(c, to->v->str, to->pos, false, &out->to, elem, "to"))
        yexp__name_use(c, &out->to, YEXP__N_STATE | YEXP__N_END, elem, "to");
    if (shared) {
        const yexp__slot* in = yexp__slot_at_m(c, s, YEXP__S_EXIT, mask, "in");
        if (!(on && on->v) && !out->after)
            yexp__err(c, src->pos, "%s: a shared exit is 'on' or 'after_s'; an instant shared exit is refused (D20 rule 11)", elem);
        if (in && in->v) {
            int* idx = (int*)yexp__alloc(c, (in->v->n ? in->v->n : 1) * sizeof(int));
            uint32_t q;
            if (!in->v->n) yexp__err(c, in->pos, "%s: 'in' names at least one state (D20 rule 11)", elem);
            if (idx) {
                for (q = 0; q < in->v->n; q++) {
                    idx[q] = yexp__kfind(c->st_idx, c->n_st, in->v->items[q].str);
                    if (idx[q] < 0) yexp__err(c, in->pos, "%s: 'in': no state named '%s'", elem, in->v->items[q].str);
                }
                out->in = idx;
                out->n_in = (int)in->v->n;
            }
        }
    }
    yexp__release(c, YEXP__S_EXIT);
    *canon = ce;
    return c->r->n_errors == before;
}

static void yexp__parse_states(yexp__ctx* c, yexp_template* t, const yjs_value* src, const char* telem, yjs_value** canon) {
    yexp_state* out;
    yjs_value* carr;
    size_t i;
    if (src->type != YJS_ARRAY || src->n > YEXP__MAX_LIST) {
        yexp__err(c, src->pos, "%s: 'states' is an array of states, at most %d", telem, YEXP__MAX_LIST);
        return;
    }
    out = (yexp_state*)yexp__alloc(c, (src->n ? src->n : 1) * sizeof *out);
    carr = yexp__cnew(c, YJS_ARRAY);
    if (!out || !carr) return;
    t->states = out;
    t->n_states = (int)src->n;
    for (i = 0; i < src->n; i++) {
        char elem[160], sub[200];
        yexp__slot* s;
        yjs_value* cs = NULL;
        yexp__slot* x;
        const yjs_value* nv;
        yexp_state* st = &out[i];
        st->over_to.index = -1;
        st->over_to.lane = -1;
        st->pos_ = src->u.items[i]->pos;
        nv = src->u.items[i]->type == YJS_OBJECT ? yjs_get(src->u.items[i], "name") : NULL;
        snprintf(elem, sizeof elem, "%s: state '%s'", telem, nv && nv->type == YJS_STRING ? nv->u.s : "?");
        s = yexp__object(c, YEXP__S_STATE, 0xffffu, src->u.items[i], elem, &cs);
        if (!s) continue;
        st->name = yexp__v_str(yexp__slotv(c, s, YEXP__S_STATE, "name"));
        x = yexp__slot_at(c, s, YEXP__S_STATE, "steps");
        if (x && x->src) {
            yjs_value* ca = NULL;
            snprintf(sub, sizeof sub, "%s: steps", elem);
            c->step_place = 2;
            c->until_seen = 0;
            yexp__parse_steps(c, x->src, sub, 0, &st->steps, &st->n_steps, &ca);
            if (ca && ca->n) yexp__put(c, cs, "steps", ca);
        }
        x = yexp__slot_at(c, s, YEXP__S_STATE, "exits");
        if (x && x->src) {
            if (x->src->type != YJS_ARRAY || x->src->n > YEXP__MAX_LIST) {
                yexp__err(c, x->src->pos, "%s: 'exits' is an array of exits", elem);
            } else {
                yexp_exit* ex = (yexp_exit*)yexp__alloc(c, (x->src->n ? x->src->n : 1) * sizeof *ex);
                yjs_value* ca = yexp__cnew(c, YJS_ARRAY);
                size_t q;
                if (ex && ca) {
                    for (q = 0; q < x->src->n; q++) {
                        yjs_value* ce = NULL;
                        snprintf(sub, sizeof sub, "%s: exits[%u]", elem, (unsigned)q);
                        yexp__parse_exit(c, x->src->u.items[q], false, sub, &ex[q], &ce);
                        if (ce) yexp__cpush(c, ca, ce);
                    }
                    st->exits = ex;
                    st->n_exits = (int)x->src->n;
                    if (ca->n) yexp__put(c, cs, "exits", ca);
                }
            }
        }
        {
            const yexp_value* mv = yexp__slotv(c, s, YEXP__S_STATE, "max_visits");
            x = yexp__slot_at(c, s, YEXP__S_STATE, "over");
            st->max_visits = yexp__int_field(c, mv, elem, "max_visits", 1, INT32_MAX, 0);
            if ((mv != NULL) != (x && x->src))
                yexp__err(c, mv ? mv->pos : x->src->pos, "%s: 'max_visits' and 'over' go together (D20 rule 12)", elem);
            if (x && x->src) {
                yjs_value* co = NULL;
                yexp__slot* s2;
                snprintf(sub, sizeof sub, "%s: over", elem);
                s2 = yexp__object(c, YEXP__S_OVER, 0xffffu, x->src, sub, &co);
                if (s2) {
                    const yexp__slot* ot = yexp__slot_at(c, s2, YEXP__S_OVER, "to");
                    st->over_outcome = yexp__slotv(c, s2, YEXP__S_OVER, "outcome");
                    if (ot && ot->v && yexp__ref_text(c, ot->v->str, ot->pos, false, &st->over_to, sub, "to"))
                        yexp__name_use(c, &st->over_to, YEXP__N_STATE | YEXP__N_END, sub, "to");
                    yexp__put(c, cs, "over", co);
                    yexp__release(c, YEXP__S_OVER);
                }
            }
        }
        yexp__release(c, YEXP__S_STATE);
        yexp__cpush(c, carr, cs);
    }
    c->step_place = 0;
    *canon = carr;
}

static void yexp__parse_shared_exits(yexp__ctx* c, yexp_template* t, const yjs_value* src, const char* telem, yjs_value** canon) {
    yexp_exit* ex;
    yjs_value* ca;
    size_t q;
    if (src->type != YJS_ARRAY || src->n > YEXP__MAX_LIST) {
        yexp__err(c, src->pos, "%s: 'exits' is an array of shared exits", telem);
        return;
    }
    ex = (yexp_exit*)yexp__alloc(c, (src->n ? src->n : 1) * sizeof *ex);
    ca = yexp__cnew(c, YJS_ARRAY);
    if (!ex || !ca) return;
    for (q = 0; q < src->n; q++) {
        char sub[160];
        yjs_value* ce = NULL;
        snprintf(sub, sizeof sub, "%s: exits[%u]", telem, (unsigned)q);
        yexp__parse_exit(c, src->u.items[q], true, sub, &ex[q], &ce);
        if (ce) yexp__cpush(c, ca, ce);
    }
    t->exits = ex;
    t->n_exits = (int)src->n;
    *canon = ca;
}

/* Whether a list of steps holds an `until` at its own level. */
static bool yexp__steps_until(const yexp_step* steps, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (steps[i].op == YEXP_UNTIL) return true;
    return false;
}

/* Whether state i is in some shared exit's `in`. */
static bool yexp__in_shared(const yexp_template* t, int i) {
    int e, q;
    for (e = 0; e < t->n_exits; e++)
        for (q = 0; q < t->exits[e].n_in; q++)
            if (t->exits[e].in[q] == i) return true;
    return false;
}

/* D20 rule 6 (format finding F5): every cycle of the chart holds an exit
 * that waits (`on` or `after_s`). The edges that do not wait: an instant
 * exit, `over`, and a state with no exits that goes on when its steps end
 * (none when its steps hold an `until`, whose exit waits). */
static void yexp__state_cycles(yexp__ctx* c, const yexp_template* t, const char* telem) {
    int n = t->n_states, i;
    uint8_t* color;
    int* stack;
    int* next;
    if (n == 0) return;
    color = (uint8_t*)yexp__alloc(c, (size_t)n);
    stack = (int*)yexp__alloc(c, (size_t)n * sizeof(int));
    next = (int*)yexp__alloc(c, (size_t)n * sizeof(int));
    if (!color || !stack || !next) return;
    memset(color, 0, (size_t)n);
    for (i = 0; i < n; i++) {
        int sp = 0;
        if (color[i]) continue;
        stack[sp++] = i;
        next[i] = 0;
        color[i] = 1;
        while (sp) {
            int u = stack[sp - 1], v = -1;
            const yexp_state* st = &t->states[u];
            /* edge k: own exits 0..n_exits-1, then over, then the step end */
            while (v < 0 && next[u] < st->n_exits + 2) {
                int k = next[u]++;
                if (!yexp__work(c)) return;
                if (k < st->n_exits) {
                    const yexp_exit* e = &st->exits[k];
                    if (e->on.kind == YEXP_REF_NONE && !e->after && e->to.kind == YEXP_REF_STATE) v = e->to.index;
                } else if (k == st->n_exits) {
                    if (st->over_to.kind == YEXP_REF_STATE) v = st->over_to.index;
                } else if (st->n_exits == 0 && !yexp__in_shared(t, u) && !yexp__steps_until(st->steps, st->n_steps) && u + 1 < n) {
                    v = u + 1;
                }
            }
            if (v >= 0) {
                if (color[v] == 1) {
                    yexp__err(c, st->pos_, "%s: states '%s' and '%s' are on a cycle of exits that do not wait; every cycle needs an 'on' or 'after_s' exit (D20 rule 6)",
                              telem, t->states[v].name ? t->states[v].name : "?", st->name ? st->name : "?");
                    return;
                }
                if (color[v] == 0) { color[v] = 1; next[v] = 0; stack[sp++] = v; }
            } else {
                color[u] = 2;
                sp--;
            }
        }
    }
}

/* An outcome's text, for requeue_outcomes: 1 equal, 0 not, -1 not a
 * literal (not known at load). */
static int yexp__out_eq(const yexp_value* o, const char* text) {
    char buf[32];
    if (!o) return 0;
    switch (o->kind) {
    case YEXP_V_STRING: return yexp__streq(o->str, text);
    case YEXP_V_BOOL: return yexp__streq(o->i ? "true" : "false", text);
    case YEXP_V_NUMBER:
        if (!(o->flags & YEXP_VF_INT)) return -1;
        snprintf(buf, sizeof buf, "%lld", (long long)o->i);
        return yexp__streq(buf, text);
    default: return -1;
    }
}

/* Whether an outcome can be `text` (3.11): an exit's or an over's, the
 * response's ("correct" and "wrong" with a key_answer, "responded" without,
 * "timeout"), an until's "timeout", or "done". 1 yes, 0 no, -1 not known. */
static int yexp__outcome_given(const yexp_template* t, const char* text) {
    int i, k, unknown = 0, e;
    for (i = 0; i < t->n_states; i++) {
        const yexp_state* st = &t->states[i];
        if ((e = yexp__out_eq(st->over_outcome, text)) == 1) return 1;
        if (e < 0) unknown = 1;
        for (k = 0; k < st->n_exits; k++) {
            if ((e = yexp__out_eq(st->exits[k].outcome, text)) == 1) return 1;
            if (e < 0) unknown = 1;
        }
        for (k = 0; k < st->n_steps; k++)
            if (st->steps[k].op == YEXP_UNTIL && st->steps[k].timeout && yexp__streq(text, "timeout")) return 1;
    }
    for (k = 0; k < t->n_exits; k++) {
        if ((e = yexp__out_eq(t->exits[k].outcome, text)) == 1) return 1;
        if (e < 0) unknown = 1;
    }
    for (k = 0; k < t->n_events; k++)
        if (t->events[k].op == YEXP_UNTIL && t->events[k].timeout && yexp__streq(text, "timeout")) return 1;
    if (t->response) {
        const yexp_response* r = t->response;
        if (r->key_answer && (yexp__streq(text, "correct") || yexp__streq(text, "wrong"))) return 1;
        if (!r->key_answer && yexp__streq(text, "responded")) return 1;
        if (yexp__streq(text, "timeout")) return 1;
    }
    if (yexp__streq(text, "done")) return 1;
    return unknown ? -1 : 0;
}

/* --- a template's names (version 2, format 3.9) ------------------------- */

static int yexp__keyed_cmp(const void* a, const void* b);

/* Its states and flags by name, so that every key can name them. A name
 * listed twice is refused here. */
static void yexp__template_names(yexp__ctx* c, yexp_template* t, const yjs_value* states_src, const yexp_value* flags_v, const char* elem) {
    int i, n = 0;
    c->st_idx = c->fl_idx = NULL;
    c->n_st = c->n_fl = 0;
    if (states_src && states_src->type == YJS_ARRAY && states_src->n <= YEXP__MAX_LIST) {
        yexp__keyed* k = (yexp__keyed*)yexp__alloc(c, (states_src->n ? states_src->n : 1) * sizeof *k);
        if (!k) return;
        for (i = 0; i < (int)states_src->n; i++) {
            const yjs_value* it = states_src->u.items[i];
            const yjs_value* nv = it->type == YJS_OBJECT ? yjs_get(it, "name") : NULL;
            if (!nv || nv->type != YJS_STRING || !yexp__name_ok(nv->u.s)) continue;
            k[n].key = nv->u.s;
            k[n].index = i;
            n++;
        }
        qsort(k, (size_t)n, sizeof *k, yexp__keyed_cmp);
        for (i = 1; i < n; i++)
            if (yexp__streq(k[i].key, k[i - 1].key))
                yexp__err(c, states_src->u.items[k[i].index]->pos, "%s: states[%d]: a duplicate state '%s' (states[%d] has the name)", elem, k[i].index, k[i].key, k[i - 1].index);
        c->st_idx = k;
        c->n_st = n;
    }
    if (flags_v && flags_v->kind == YEXP_V_ARRAY) {
        yexp__keyed* k = (yexp__keyed*)yexp__alloc(c, (flags_v->n ? flags_v->n : 1) * sizeof *k);
        if (!k) return;
        t->flags = yexp__str_array(c, flags_v, &t->n_flags);
        for (i = 0; i < t->n_flags; i++) {
            k[i].key = t->flags[i];
            k[i].index = i;
            if (yexp__is_builtin(t->flags[i]))
                yexp__err(c, flags_v->pos, "%s: 'flags': '%s' is a built-in variable's name; a flag is read as $NAME (3.9)", elem, t->flags[i]);
        }
        qsort(k, (size_t)t->n_flags, sizeof *k, yexp__keyed_cmp);
        for (i = 1; i < t->n_flags; i++)
            if (yexp__streq(k[i].key, k[i - 1].key)) yexp__err(c, flags_v->pos, "%s: 'flags': '%s' listed twice", elem, k[i].key);
        c->fl_idx = k;
        c->n_fl = t->n_flags;
    }
}

/* What a top-level name is, for a clash's message; NULL: none. */
static const char* yexp__global_kind(const yexp__ctx* c, const char* name) {
    if (yexp_stim_get(c->d, name)) return "stimulus";
    if (yexp__ctl_find(c->d->controls, c->d->n_controls, name)) return "top-level control";
    if (yexp_sigrole_get(c->d, name)) return "signal role";
    return NULL;
}

/* One namespace per template (3.9, D20 rule 9): the stimuli, the top-level
 * controls, its controls, states and flags, and the signal roles. */
static void yexp__template_namespace(yexp__ctx* c, yexp_template* t, const char* elem) {
    static const char* const reserved[2] = { "end", "response" };
    int i, k;
    for (i = 0; i < t->n_controls; i++) {
        const yexp_control* x = &t->controls[i];
        const char* g = yexp__global_kind(c, x->name);
        if (g && yexp__streq(g, "top-level control"))
            yexp__err(c, x->pos_, "%s: control '%s' has a top-level control's name; a template's control cannot (D25)", elem, x->name);
        else if (g)
            yexp__err(c, x->pos_, "%s: control '%s': the name is also a %s's; one template's names are unique (3.9)", elem, x->name, g);
    }
    for (i = 0; i < c->n_st; i++) {
        const char* nm = c->st_idx[i].key;
        const char* g = yexp__global_kind(c, nm);
        uint32_t pos = t->states_pos_;
        if (g) yexp__err(c, pos, "%s: state '%s': the name is also a %s's; one template's names are unique (3.9)", elem, nm, g);
        else if (yexp__ctl_find(t->controls, t->n_controls, nm)) yexp__err(c, pos, "%s: state '%s': the name is also a control's; one template's names are unique (3.9)", elem, nm);
        else if (yexp__kfind(c->fl_idx, c->n_fl, nm) >= 0) yexp__err(c, pos, "%s: state '%s': the name is also a flag's; one template's names are unique (3.9)", elem, nm);
        if (c->trials_src && yjs_get(c->trials_src, nm)) yexp__err(c, pos, "%s: state '%s' has a template's name (3.9)", elem, nm);
        if (yexp__is_builtin(nm)) yexp__err(c, pos, "%s: state '%s' has a built-in variable's name (3.9)", elem, nm);
    }
    for (i = 0; i < c->n_fl; i++) {
        const char* nm = c->fl_idx[i].key;
        const char* g = yexp__global_kind(c, nm);
        if (g) yexp__err(c, t->pos_, "%s: flag '%s': the name is also a %s's; one template's names are unique (3.9)", elem, nm, g);
        else if (yexp__ctl_find(t->controls, t->n_controls, nm)) yexp__err(c, t->pos_, "%s: flag '%s': the name is also a control's; one template's names are unique (3.9)", elem, nm);
    }
    if (!c->t_states) return;
    for (k = 0; k < 2; k++) {
        const char* g = yexp__global_kind(c, reserved[k]);
        if (g) yexp__err(c, t->pos_, "%s: '%s' is reserved in a template with states, and a %s has the name (3.9)", elem, reserved[k], g);
        if (yexp__ctl_find(t->controls, t->n_controls, reserved[k])) yexp__err(c, t->pos_, "%s: '%s' is reserved in a template with states; a control cannot have it (3.9)", elem, reserved[k]);
        if (yexp__kfind(c->st_idx, c->n_st, reserved[k]) >= 0) yexp__err(c, t->states_pos_, "%s: '%s' is reserved in a template with states; a state cannot have it (3.9)", elem, reserved[k]);
        if (yexp__kfind(c->fl_idx, c->n_fl, reserved[k]) >= 0) yexp__err(c, t->pos_, "%s: '%s' is reserved in a template with states; a flag cannot have it (3.9)", elem, reserved[k]);
    }
}

/* The top-level names (3.3, 3.9, D21): a name is a key of devices or of
 * roles, not both, and stimuli, top-level controls and signal roles share
 * every template's namespace. */
static void yexp__global_names(yexp__ctx* c) {
    const yexp_def* d = c->d;
    int i;
    for (i = 0; i < d->n_sigroles; i++) {
        const yexp_sigrole* r = &d->sigroles[i];
        if (yexp__role_get(d, r->name)) yexp__err(c, r->pos_, "signal role '%s': the name is also a key of 'devices'; a name is an event role or a signal role, not both (D21)", r->name);
        else if (yexp_stim_get(d, r->name)) yexp__err(c, r->pos_, "signal role '%s': the name is also a stimulus's; one template's names are unique (3.9)", r->name);
    }
    for (i = 0; i < d->n_controls; i++) {
        const yexp_control* x = &d->controls[i];
        if (yexp_stim_get(d, x->name)) yexp__err(c, x->pos_, "control '%s': the name is also a stimulus's; one template's names are unique (3.9)", x->name);
        else if (yexp_sigrole_get(d, x->name)) yexp__err(c, x->pos_, "control '%s': the name is also a signal role's; one template's names are unique (3.9)", x->name);
    }
}

/* The top-level controls a template names, with what they name in turn. */
static void yexp__top_closure(yexp__ctx* c, yexp_template* t) {
    const yexp_def* d = c->d;
    bool more = true;
    int i, q;
    if (!t->uses_top) return;
    while (more) {
        more = false;
        for (i = 0; i < d->n_controls; i++) {
            const yexp_control* dep[72];
            int nd;
            if (!t->uses_top[i]) continue;
            if (!yexp__work(c)) return;
            nd = yexp__ctl_deps(&d->controls[i], dep);
            for (q = 0; q < nd; q++) {
                int k = (int)(dep[q] - d->controls);
                if (k >= 0 && k < d->n_controls && !t->uses_top[k]) { t->uses_top[k] = 1; more = true; }
            }
        }
    }
}

/* The controls of the template's graph: its own, then the top-level ones
 * it names. i from 0; NULL past the last. */
static const yexp_control* yexp__graph_control(const yexp__ctx* c, const yexp_template* t, int* i) {
    while (*i < t->n_controls + c->d->n_controls) {
        int k = (*i)++;
        if (k < t->n_controls) return &t->controls[k];
        if (t->uses_top && t->uses_top[k - t->n_controls]) return &c->d->controls[k - t->n_controls];
    }
    return NULL;
}

/* The checks of one template that need all of it read (3.11, 3.7, 3.13). */
static void yexp__template_end(yexp__ctx* c, yexp_template* t, const char* elem) {
    int i, k;
    const yexp_control* x;
    const yexp_control* mt = NULL;
    yexp__xy* xy;
    yexp__top_closure(c, t);
    /* 3.11 rule 10: a state whose exits are all instant always leaves */
    for (i = 0; i < t->n_states; i++) {
        const yexp_state* st = &t->states[i];
        bool all_instant = st->n_exits > 0 && !yexp__in_shared(t, i);
        for (k = 0; k < st->n_exits && all_instant; k++)
            if (st->exits[k].on.kind != YEXP_REF_NONE || st->exits[k].on.name || st->exits[k].after) all_instant = false;
        if (all_instant && st->exits[st->n_exits - 1].if_var)
            yexp__err(c, st->exits[st->n_exits - 1].pos_, "%s: state '%s': its exits are all instant and the last has an 'if', so the state may not leave; end with an exit with no 'if' (3.11 rule 10)",
                      elem, st->name ? st->name : "?");
    }
    yexp__state_cycles(c, t, elem);
    i = 0;
    while ((x = yexp__graph_control(c, t, &i)) != NULL) {
        if (x->failed_) continue;
        /* one base time a frame (D39, G19) */
        if (x->kind == YEXP_MOVING_TARGET) {
            if (!mt) mt = x;
            else if ((mt->from.name == NULL) != (x->from.name == NULL) || (mt->from.name && !yexp__streq(mt->from.name, x->from.name)))
                yexp__err(c, x->pos_, "%s: the moving targets '%s' and '%s' have a different 'from'; one template has one time origin for its generators (D39, G19)", elem, mt->name, x->name);
        }
        if (x->kind == YEXP_MOVEMENT && !x->shared && !x->named_)
            yexp__err(c, x->pos_, "%s: control '%s': a movement is a response's choice, and no response names it (3.7, 3.13)", elem, x->name);
        /* 3.7: a step does not give x or y of a control's stimulus (F12) */
        if ((x->kind == YEXP_CURSOR || x->kind == YEXP_MOVING_TARGET) && x->stimulus.kind == YEXP_REF_STIMULUS)
            for (xy = c->xy; xy; xy = xy->next)
                if (xy->stim == x->stimulus.index)
                    yexp__err(c, xy->pos, "%s: '%s' of '%s': the control '%s' moves the stimulus for the whole trial; to switch what it follows, leave out the control's 'stimulus' and use set steps (3.7)",
                              xy->elem, xy->key, x->stimulus.name, x->name);
    }
    c->xy = NULL;
}

/* A response whose choices are movements (3.13, D34). */
static void yexp__parse_sresponse(yexp__ctx* c, const yjs_value* src, const char* telem, const yexp_response** out_r, yjs_value** canon) {
    char elem[128];
    yjs_value* cr = NULL;
    yexp__slot* s;
    yexp_response* r;
    const yexp__slot* chs;
    const yexp_control* first = NULL;
    snprintf(elem, sizeof elem, "%s: response", telem);
    s = yexp__object(c, YEXP__S_SRESPONSE, 0xffffu, src, elem, &cr);
    if (!s) return;
    r = (yexp_response*)yexp__alloc(c, sizeof *r);
    if (!r) { yexp__release(c, YEXP__S_SRESPONSE); return; }
    r->signal_choices = true;
    r->from = yexp__v_str(yexp__slotv(c, s, YEXP__S_SRESPONSE, "from"));
    r->duration = yexp__slotv(c, s, YEXP__S_SRESPONSE, "duration_s");
    r->min_rt = yexp__slotv(c, s, YEXP__S_SRESPONSE, "minimum_valid_rt_s");
    r->ends_trial = yexp__v_bool(yexp__slotv(c, s, YEXP__S_SRESPONSE, "response_ends_trial"), false);
    r->max_responses = yexp__int_field(c, yexp__slotv(c, s, YEXP__S_SRESPONSE, "max_responses"), elem, "max_responses", 0, INT32_MAX, 0);
    if (r->from && !yexp_stim_get(c->d, r->from) && !(c->bad_sets & YEXP__BAD_STIMULI)) yexp__err(c, src->pos, "%s: 'from': no stimulus named '%s'", elem, r->from);
    chs = yexp__slot_at(c, s, YEXP__S_SRESPONSE, "choices");
    if (chs && chs->src) {
        const yjs_value* arr = chs->src;
        yexp_choice* ch = (yexp_choice*)yexp__alloc(c, (arr->n ? arr->n : 1) * sizeof *ch);
        yjs_value* ca = yexp__cnew(c, YJS_ARRAY);
        size_t i, j;
        if (arr->n > YEXP__MAX_LIST) { yexp__err(c, arr->pos, "%s: 'choices': more than %d", elem, YEXP__MAX_LIST); ch = NULL; }
        if (ch && ca) {
            for (i = 0; i < arr->n; i++) {
                const yjs_value* it = arr->u.items[i];
                const yjs_value* sv = it->type == YJS_OBJECT ? yjs_get(it, "signal") : NULL;
                char ce[200];
                yexp_ref ref;
                size_t q;
                yjs_value* co;
                snprintf(ce, sizeof ce, "%s: 'choices'[%u]", elem, (unsigned)i);
                if (!sv) { yexp__err(c, it->pos, "%s: signal and key choices in one response; a response has one or the other (3.13)", ce); continue; }
                if (!yexp__note_ok(c, it, ce)) continue;
                for (q = 0; q < it->n; q++)
                    if (!yexp__streq(it->u.members[q].key, "signal") && !yexp__streq(it->u.members[q].key, "note")) {
                        yexp__err(c, it->u.members[q].key_pos, "%s: a signal choice is {\"signal\": MOVEMENT}; '%s' is an unknown key", ce, it->u.members[q].key);
                        break;
                    }
                if (q < it->n) continue;
                if (sv->type != YJS_STRING || !yexp__ref_text(c, sv->u.s, sv->pos, false, &ref, ce, "signal")) {
                    if (sv->type != YJS_STRING) yexp__err(c, sv->pos, "%s: 'signal' names a movement", ce);
                    continue;
                }
                if (!yexp__resolve_name(c, &ref) || ref.kind != YEXP_REF_CONTROL || ref.control->kind != YEXP_MOVEMENT) {
                    if (ref.kind == YEXP_REF_CONTROL && ref.control->failed_) continue;
                    if (!(ref.kind == YEXP_REF_NONE && (c->bad_sets & YEXP__BAD_CONTROLS))) yexp__err(c, sv->pos, "%s: a signal choice names a movement control; '%s' is not one (3.13)", ce, ref.name);
                    continue;
                }
                ch[i].key = ch[i].name = ref.name;
                ch[i].signal = ref.control;
                ((yexp_control*)(void*)ref.control)->named_ = true;
                if (ref.control->shared && c->mv_named) c->mv_named[ref.control - c->d->controls] = 1;
                if (ref.control->shared && c->tp && c->tp->uses_top) c->tp->uses_top[ref.control - c->d->controls] = 1;
                for (j = 0; j < i; j++)
                    if (ch[j].signal == ref.control) yexp__err(c, sv->pos, "%s: '%s' is listed twice", ce, ref.name);
                /* one arm for the collector (D34 (c)) */
                if (!first) first = ref.control;
                else if ((first->from.name == NULL) != (ref.control->from.name == NULL) || (first->from.name && !yexp__streq(first->from.name, ref.control->from.name)))
                    yexp__err(c, sv->pos, "%s: the movements '%s' and '%s' have a different 'from'; the collector arms once (3.13)", ce, first->name, ref.name);
                co = yexp__cnew(c, YJS_OBJECT);
                yexp__put(c, co, "signal", yexp__cstr(c, sv->u.s));
                if (yjs_get(it, "note")) yexp__put(c, co, "note", yexp__cstr(c, yjs_get(it, "note")->u.s));
                yexp__cpush(c, ca, co);
            }
            r->choices = ch;
            r->n_choices = (int)arr->n;
            yexp__put(c, cr, "choices", ca);
        }
    }
    *out_r = r;
    yexp__release(c, YEXP__S_SRESPONSE);
    *canon = cr;
}

/* A response's choices that are signals: the response is 3.13's. */
static bool yexp__has_signal_choices(const yjs_value* src) {
    const yjs_value* ch = src->type == YJS_OBJECT ? yjs_get(src, "choices") : NULL;
    size_t i;
    if (!ch || ch->type != YJS_ARRAY) return false;
    for (i = 0; i < ch->n; i++)
        if (ch->u.items[i]->type == YJS_OBJECT && yjs_get(ch->u.items[i], "signal")) return true;
    return false;
}

static void yexp__parse_response(yexp__ctx* c, const yjs_value* src, const char* telem, const yexp_response** out_r, yjs_value** canon) {
    char elem[128];
    yjs_value* cr = NULL;
    yexp__slot* s;
    yexp_response* r;
    const char* name;
    yexp__slot* chs;
    yexp__slot* trg;
    if (c->version >= 2 && yexp__has_signal_choices(src)) {
        yexp__parse_sresponse(c, src, telem, out_r, canon);
        return;
    }
    snprintf(elem, sizeof elem, "%s: response", telem);
    s = yexp__object(c, YEXP__S_RESPONSE, 0xffffu, src, elem, &cr);
    if (!s) return;
    r = (yexp_response*)yexp__alloc(c, sizeof *r);
    if (!r) { yexp__release(c, YEXP__S_RESPONSE); return; }
    r->role = yexp__v_str(yexp__slotv(c, s, YEXP__S_RESPONSE, "role"));
    r->from = yexp__v_str(yexp__slotv(c, s, YEXP__S_RESPONSE, "from"));
    r->duration = yexp__slotv(c, s, YEXP__S_RESPONSE, "duration_s");
    r->min_rt = yexp__slotv(c, s, YEXP__S_RESPONSE, "minimum_valid_rt_s");
    r->key_answer = yexp__slotv(c, s, YEXP__S_RESPONSE, "key_answer");
    r->ends_trial = yexp__v_bool(yexp__slotv(c, s, YEXP__S_RESPONSE, "response_ends_trial"), true);
    r->max_responses = yexp__int_field(c, yexp__slotv(c, s, YEXP__S_RESPONSE, "max_responses"), elem, "max_responses", 0, INT32_MAX, 0);
    r->allow_held_key = yexp__v_bool(yexp__slotv(c, s, YEXP__S_RESPONSE, "allow_held_key"), false);
    r->wait_for_key_release = yexp__v_bool(yexp__slotv(c, s, YEXP__S_RESPONSE, "wait_for_key_release"), false);
    if (r->role) {
        const yexp_role* ro = yexp__role_get(c->d, r->role);
        if (!ro) { if (!(c->bad_sets & YEXP__BAD_ROLES)) yexp__err(c, src->pos, "%s: 'role': no role named '%s'", elem, r->role); }
        else if (!ro->failed_ && ro->direction == 1) yexp__err(c, src->pos, "%s: 'role': '%s' is an output role; a response needs an input", elem, r->role);
    }
    name = r->from;
    if (name && !yexp_stim_get(c->d, name) && !(c->bad_sets & YEXP__BAD_STIMULI)) yexp__err(c, src->pos, "%s: 'from': no stimulus named '%s'", elem, name);
    chs = yexp__slot_at(c, s, YEXP__S_RESPONSE, "choices");
    if (chs && chs->src) {
        const yjs_value* arr = chs->src;
        yexp_choice* ch;
        yjs_value* ca;
        size_t i;
        if (arr->type != YJS_ARRAY || arr->n == 0 || arr->n > YEXP__MAX_LIST) {
            yexp__err(c, arr->pos, "%s: 'choices' is a non-empty array of keys", elem);
        } else {
            ch = (yexp_choice*)yexp__alloc(c, arr->n * sizeof *ch);
            ca = yexp__cnew(c, YJS_ARRAY);
            if (ch && ca) {
                for (i = 0; i < arr->n; i++) {
                    const yjs_value* it = arr->u.items[i];
                    char ce[200];
                    snprintf(ce, sizeof ce, "%s: 'choices'[%u]", elem, (unsigned)i);
                    if (it->type == YJS_STRING) {
                        yjs_value* cs2 = NULL;
                        const yexp_value* kv2 = yexp__literal_string(c, it, ce, "key", &cs2);
                        if (kv2) { ch[i].key = kv2->str; ch[i].name = kv2->str; yexp__cpush(c, ca, cs2); }
                    } else {
                        yjs_value* co = NULL;
                        yexp__slot* s2 = yexp__object(c, YEXP__S_CHOICE, 0xffffu, it, ce, &co);
                        if (s2) {
                            ch[i].key = yexp__v_str(yexp__slotv(c, s2, YEXP__S_CHOICE, "key"));
                            ch[i].name = yexp__v_str(yexp__slotv(c, s2, YEXP__S_CHOICE, "name"));
                            if (!ch[i].name) ch[i].name = ch[i].key;
                            else if (ch[i].key && yexp__streq(ch[i].name, ch[i].key))
                                yexp__err(c, it->pos, "%s: 'name' equals 'key': write the key alone", ce);
                            /* a key alone has one spelling: the bare string */
                            yexp__cpush(c, ca, co && co->n == 1 && yjs_get(co, "key") ? yjs_get(co, "key") : co);
                            yexp__release(c, YEXP__S_CHOICE);
                        }
                    }
                    if (ch[i].key && !ch[i].key[0]) yexp__err(c, it->pos, "%s: an empty key", ce);
                    /* 2.6: a button role's choices are its button names */
                    if (ch[i].key && r->role) {
                        const yexp_role* ro = yexp__role_get(c->d, r->role);
                        if (ro && !ro->failed_ && ro->sorted_buttons_ &&
                            !bsearch(&ch[i].key, (const void*)ro->sorted_buttons_, (size_t)ro->n_buttons, sizeof(char*), yexp__str_cmp))
                            yexp__err(c, it->pos, "%s: '%s' is not a button of the role '%s'", ce, ch[i].key, r->role);
                    }
                }
                {
                    /* sorted once, so a list at the stated limit stays linear */
                    yexp__keyed* ks = (yexp__keyed*)yexp__alloc(c, arr->n * sizeof *ks);
                    size_t nk = 0, a;
                    if (ks) {
                        for (a = 0; a < arr->n; a++)
                            if (ch[a].key) { ks[nk].key = ch[a].key; ks[nk].index = (int)a; nk++; }
                        qsort(ks, nk, sizeof *ks, yexp__keyed_cmp);
                        for (a = 1; a < nk; a++)
                            if (yexp__streq(ks[a].key, ks[a - 1].key))
                                yexp__err(c, arr->u.items[ks[a].index]->pos, "%s: 'choices'[%d]: the key '%s' is listed twice", elem, ks[a].index, ks[a].key);
                    }
                }
                r->choices = ch;
                r->n_choices = (int)arr->n;
                yexp__put(c, cr, "choices", ca);
            }
        }
    }
    trg = yexp__slot_at(c, s, YEXP__S_RESPONSE, "trigger");
    if (trg && trg->src) {
        char te[140];
        yjs_value* ct = NULL;
        yexp__slot* s2;
        snprintf(te, sizeof te, "%s: 'trigger'", elem);
        s2 = yexp__object(c, YEXP__S_RTRIGGER, 0xffffu, trg->src, te, &ct);
        if (s2) {
            r->trigger_role = yexp__v_str(yexp__slotv(c, s2, YEXP__S_RTRIGGER, "role"));
            r->trigger_code = yexp__slotv(c, s2, YEXP__S_RTRIGGER, "code");
            if (r->trigger_role) {
                const yexp_role* ro = yexp__role_get(c->d, r->trigger_role);
                if (!ro) { if (!(c->bad_sets & YEXP__BAD_ROLES)) yexp__err(c, trg->src->pos, "%s: 'role': no role named '%s'", te, r->trigger_role); }
                else if (!ro->failed_ && ro->direction == 0) yexp__err(c, trg->src->pos, "%s: 'role': '%s' is an input role; a trigger needs an output", te, r->trigger_role);
            }
            yexp__put(c, cr, "trigger", ct);
            yexp__release(c, YEXP__S_RTRIGGER);
        }
    }
    *out_r = r;
    yexp__release(c, YEXP__S_RESPONSE);
    *canon = cr;
}

/* D10's default for "record": the response's `from` and any stimulus with a
 * trigger at its onset, that is a trigger step with no step that moves the
 * cursor between it and the stimulus's "on". */
static void yexp__default_record_scan(yexp__ctx* c, const yexp_step* steps, int n, const char** pending, int* n_pending, const char** out, int* n_out, int cap) {
    int i, j;
    for (i = 0; i < n; i++) {
        const yexp_step* st = &steps[i];
        switch (st->op) {
        case YEXP_ON:
            for (j = 0; j < st->n_stims && *n_pending < cap; j++) pending[(*n_pending)++] = st->stims[j];
            break;
        case YEXP_WAIT: case YEXP_AT: case YEXP_THEN: case YEXP_UNTIL:
            *n_pending = 0;
            break;
        case YEXP_TRIGGER:
            for (j = 0; j < *n_pending; j++) {
                if (!yexp__stim_stamped(c, pending[j]) && *n_out < cap) out[(*n_out)++] = pending[j];
            }
            break;
        case YEXP_IF: case YEXP_UNLESS:
            yexp__default_record_scan(c, st->steps, st->n_steps, pending, n_pending, out, n_out, cap);
            break;
        default: break;
        }
    }
}

/* The names that "on" steps can list, nested steps included. */
static size_t yexp__count_on(const yexp_step* steps, int n) {
    size_t total = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (steps[i].op == YEXP_ON) total += (size_t)steps[i].n_stims;
        else if (steps[i].op == YEXP_IF || steps[i].op == YEXP_UNLESS) total += yexp__count_on(steps[i].steps, steps[i].n_steps);
    }
    return total;
}

static void yexp__default_record(yexp__ctx* c, yexp_template* t) {
    const char** pending;
    const char** out;
    int n_pending = 0, n = 0, cap = (int)yexp__count_on(t->events, t->n_events) + 1, i;
    for (i = 0; i < t->n_states; i++) cap += (int)yexp__count_on(t->states[i].steps, t->states[i].n_steps);
    pending = (const char**)yexp__alloc(c, (size_t)cap * sizeof(char*));
    out = (const char**)yexp__alloc(c, (size_t)cap * sizeof(char*));
    if (!pending || !out) return;
    c->gen++;
    if (t->response && t->response->from) { yexp__stim_stamped(c, t->response->from); out[n++] = t->response->from; }
    yexp__default_record_scan(c, t->events, t->n_events, pending, &n_pending, out, &n, cap);
    /* a state's steps start at its entry, a time of their own */
    for (i = 0; i < t->n_states; i++) {
        n_pending = 0;
        yexp__default_record_scan(c, t->states[i].steps, t->states[i].n_steps, pending, &n_pending, out, &n, cap);
    }
    t->record = out;
    t->n_record = n;
}

/* Whether some step of the template shows a stimulus: events, or a state's. */
static bool yexp__steps_show(const yexp_step* steps, int n, const char* name);
static bool yexp__template_shows(const yexp_template* t, const char* name) {
    int i;
    if (yexp__steps_show(t->events, t->n_events, name)) return true;
    for (i = 0; i < t->n_states; i++)
        if (yexp__steps_show(t->states[i].steps, t->states[i].n_steps, name)) return true;
    return false;
}

/* Whether a list of steps has an "on" for a stimulus, in `if` bodies too. */
static bool yexp__steps_show(const yexp_step* steps, int n, const char* name) {
    int i, k;
    for (i = 0; i < n; i++) {
        if (steps[i].op == YEXP_ON)
            for (k = 0; k < steps[i].n_stims; k++)
                if (yexp__streq(steps[i].stims[k], name)) return true;
        if ((steps[i].op == YEXP_IF || steps[i].op == YEXP_UNLESS) && yexp__steps_show(steps[i].steps, steps[i].n_steps, name)) return true;
    }
    return false;
}

static bool yexp__str_arrays_equal(const char* const* a, int na, const char* const* b, int nb) {
    int i;
    if (na != nb) return false;
    for (i = 0; i < na; i++)
        if (!yexp__streq(a[i], b[i])) return false;
    return true;
}

/* A column that holds seconds ends in _s (D4), and only such a column
 * does (3.15; C12 of the fix round). */
static void yexp__check_column_name(yexp__ctx* c, const yjs_member* m, const yexp_value* v, const char* elem) {
    if (v->kind == YEXP_V_VAR && yexp__ends_s(v->str) && !yexp__ends_s(m->key))
        yexp__err(c, m->key_pos, "%s: column '%s' holds seconds ('$%s'), so its name ends in '_s' (D4)", elem, m->key, v->str);
    if (v->kind == YEXP_V_TIME && !yexp__ends_s(m->key))
        yexp__err(c, m->key_pos, "%s: column '%s' holds a time in seconds, so its name ends in '_s' (3.15)", elem, m->key);
    if ((v->kind == YEXP_V_HIT || v->kind == YEXP_V_SIGNAL || v->kind == YEXP_V_WHEN) && yexp__ends_s(m->key))
        yexp__err(c, m->key_pos, "%s: column '%s' holds a %s, not seconds, so its name does not end in '_s' (3.15)", elem, m->key,
                  v->kind == YEXP_V_HIT ? "value at a hit" : v->kind == YEXP_V_SIGNAL ? "signal's value" : "when's value");
}

/* A walk's names, NAME_x to NAME_deg (3.14): each is a variable. */
static const char* const yexp__walk_suffix[5] = { "_x", "_y", "_from_x", "_from_y", "_deg" };

/* A set of columns or vars: name to value. */
static void yexp__parse_values_set(yexp__ctx* c, const yjs_value* src, const char* elem, unsigned allow, bool is_vars, const yexp_kv** out_kv, int* out_n, yjs_value** canon) {
    yexp_kv* kv;
    yjs_value* cobj;
    size_t k;
    int n = 0, i = 0;
    if (!yexp__set_begin(c, src, elem, YEXP__MAX_LIST, &n)) return;
    kv = (yexp_kv*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *kv);
    cobj = yexp__cnew(c, YJS_OBJECT);
    if (!kv || !cobj) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        yjs_value* cv = NULL;
        const yexp_value* v;
        unsigned al = allow;
        if (yexp__is_set_note(m)) { yexp__put(c, cobj, "note", yexp__cstr(c, m->value->u.s)); continue; }
        if (is_vars && yexp__is_builtin(m->key)) {
            yexp__err(c, m->key_pos, "%s: '%s' is a built-in variable's name (2.3)", elem, m->key);
            continue;
        }
        if (yexp__ends_s(m->key)) al |= YEXP__AV_DUR;
        v = yexp__value(c, m->value, al, YEXP__AV_NUM | YEXP__AV_STR, elem, m->key, &cv);
        if (!v) continue;
        yexp__check_column_name(c, m, v, elem);
        if (v->kind == YEXP_V_WALK2D) {
            /* its names are variables: none is a built-in or another vars
             * name of the node (format finding F13) */
            int q;
            for (q = 0; q < 5; q++) {
                char nm[80];
                snprintf(nm, sizeof nm, "%s%s", m->key, yexp__walk_suffix[q]);
                if (!yexp__name_ok(nm)) yexp__err(c, m->key_pos, "%s: '%s': the walk's variable '%s' is longer than a name's 63 bytes", elem, m->key, nm);
                else if (yexp__is_builtin(nm)) yexp__err(c, m->key_pos, "%s: '%s': the walk's variable '%s' is a built-in variable's name (2.3)", elem, m->key, nm);
                else if (yjs_get(src, nm)) yexp__err(c, m->key_pos, "%s: '%s': the walk defines '%s', which this node's vars also has", elem, m->key, nm);
            }
        }
        kv[i].key = m->key;
        kv[i].value = v;
        i++;
        yexp__put(c, cobj, m->key, cv);
    }
    *out_kv = kv;
    *out_n = i;
    *canon = cobj;
}

/* The parts of a template that version 2 adds (3.6), before its steps:
 * the names of its states and flags, its controls, and its namespace. */
static void yexp__template_begin(yexp__ctx* c, yexp_template* out, yexp__slot* s, unsigned mask, yjs_value* ct, const char* elem) {
    yexp__slot* x;
    yexp__slot* st = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "states");
    yexp__slot* ev = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "events");
    yexp__slot* af = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "after");
    yexp__slot* ex = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "exits");
    yexp__slot* rs = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "response");
    c->tp = out;
    c->xy = NULL;
    c->t_states = st && st->src;
    c->t_response = rs && rs->src;
    out->states_pos_ = st && st->src ? st->pos : out->pos_;
    out->uses_top = (uint8_t*)yexp__alloc(c, (size_t)c->d->n_controls + 1);
    if (out->uses_top) memset(out->uses_top, 0, (size_t)c->d->n_controls + 1);
    /* a written [] is the default, the same as none (D2) */
    if (c->t_states && ((ev && ev->src && ev->src->type == YJS_ARRAY && ev->src->n) || (af && af->src && af->src->type == YJS_ARRAY && af->src->n)))
        yexp__err(c, st->pos, "%s: 'states' replaces 'events' and 'after'; a template has one or the other (3.6)", elem);
    if (!c->t_states && ex && ex->src && ex->src->type == YJS_ARRAY && ex->src->n)
        yexp__err(c, ex->pos, "%s: 'exits' (shared exits) needs 'states' (3.6)", elem);
    yexp__template_names(c, out, st ? st->src : NULL, yexp__slotv_m(c, s, YEXP__S_TEMPLATE, mask, "flags"), elem);
    if (out->n_flags) yexp__put(c, ct, "flags", (yjs_value*)(void*)yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "flags")->src);
    x = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "controls");
    if (x && x->src) {
        yjs_value* cc = NULL;
        char prefix[128];
        snprintf(prefix, sizeof prefix, "%s: ", elem);
        yexp__parse_controls(c, x->src, false, prefix, &out->controls, &out->n_controls, &cc);
        if (cc && cc->n) yexp__put(c, ct, "controls", cc);
    }
    yexp__template_namespace(c, out, elem);
    {
        char prefix[128];
        snprintf(prefix, sizeof prefix, "%s: ", elem);
        yexp__resolve_controls(c, out->controls, out->n_controls, prefix);
        yexp__ctl_cycles(c, out->controls, out->n_controls, prefix);
    }
}

static void yexp__parse_template(yexp__ctx* c, const yjs_member* m, yexp_template* out, yjs_value* cset) {
    char elem[96], sub[128];
    yexp__slot* s;
    yjs_value* ct = NULL;
    yexp__slot* x;
    int before_events, events_errors;
    unsigned mask = 0xffffu;
    snprintf(elem, sizeof elem, "template '%s'", m->key);
    if (m->value->type != YJS_OBJECT) { yexp__err(c, m->value->pos, "%s: wrong type; expected an object", elem); return; }
    if (yjs_get(m->value, "script")) {
        const yjs_member* mm = yjs_member_n(m->value, "script", 6);
        yexp__err(c, mm ? mm->key_pos : m->value->pos, "%s: 'script' needs a later format version; version %d refuses it (D6)", elem, c->version);
        return;
    }
    s = yexp__object(c, YEXP__S_TEMPLATE, mask, m->value, elem, &ct);
    if (!s) return;
    out->name = m->key;
    out->pos_ = m->key_pos;
    if (c->version >= 2) yexp__template_begin(c, out, s, mask, ct, elem);
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "events");
    before_events = c->r->n_errors;
    if (x && x->src) {
        yjs_value* ca = NULL;
        snprintf(sub, sizeof sub, "%s: events", elem);
        c->step_place = 0;
        c->until_seen = 0;
        yexp__parse_steps(c, x->src, sub, 0, &out->events, &out->n_events, &ca);
        if (ca && ca->n) yexp__put(c, ct, "events", ca);
    }
    events_errors = c->r->n_errors - before_events;
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "after");
    if (x && x->src) {
        yjs_value* ca = NULL;
        snprintf(sub, sizeof sub, "%s: after", elem);
        c->step_place = 1;
        yexp__parse_steps(c, x->src, sub, 0, &out->after, &out->n_after, &ca);
        c->step_place = 0;
        if (ca && ca->n) yexp__put(c, ct, "after", ca);
    }
    if (c->version >= 2) {
        before_events = c->r->n_errors;
        x = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "states");
        if (x && x->src) {
            yjs_value* ca = NULL;
            yexp__parse_states(c, out, x->src, elem, &ca);
            if (ca) yexp__put(c, ct, "states", ca);
        }
        x = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "exits");
        if (x && x->src && c->t_states) {
            yjs_value* ca = NULL;
            yexp__parse_shared_exits(c, out, x->src, elem, &ca);
            if (ca && ca->n) yexp__put(c, ct, "exits", ca);
        }
        events_errors += c->r->n_errors - before_events;
    }
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "response");
    if (x && x->src) {
        yjs_value* cr = NULL;
        yexp__parse_response(c, x->src, elem, &out->response, &cr);
        yexp__put(c, ct, "response", cr);
        /* the response is armed at the first frame and timed from this onset */
        if (!events_errors && out->response && out->response->from && yexp_stim_get(c->d, out->response->from) &&
            !yexp__template_shows(out, out->response->from))
            yexp__err(c, x->src->pos, "%s: response: 'from': no step in '%s' shows '%s', so its onset never comes", elem, out->states ? "states" : "events", out->response->from);
    }
    {
        const yexp_value* rv = yexp__slotv(c, s, YEXP__S_TEMPLATE, "record");
        yexp__slot* rslot = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "record");
        yexp_template tmp = *out;
        yexp__default_record(c, &tmp);
        if (rslot && rslot->given && rv) {
            int a;
            out->record = yexp__str_array(c, rv, &out->n_record);
            c->gen++;
            for (a = 0; a < out->n_record; a++) {
                if (!yexp_stim_get(c->d, out->record[a])) {
                    if (!(c->bad_sets & YEXP__BAD_STIMULI)) yexp__err(c, rv->pos, "%s: 'record': no stimulus named '%s'", elem, out->record[a]);
                } else if (yexp__stim_stamped(c, out->record[a])) {
                    yexp__err(c, rv->pos, "%s: 'record': '%s' listed twice", elem, out->record[a]);
                }
            }
            /* at its default the key is left out, and the form is the same */
            if (!yexp__str_arrays_equal(out->record, out->n_record, tmp.record, tmp.n_record)) {
                out->record_given = true;
                yexp__put(c, ct, "record", (yjs_value*)rslot->src);
            }
        } else {
            out->record = tmp.record;
            out->n_record = tmp.n_record;
        }
    }
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "columns");
    if (x && x->src) {
        yjs_value* co = NULL;
        snprintf(sub, sizeof sub, "%s: columns", elem);
        yexp__parse_values_set(c, x->src, sub, YEXP__AV_VALUE | (c->version >= 2 ? (YEXP__AV_SIG | YEXP__AV_WHEN | YEXP__AV_COL) : 0u), false, &out->columns, &out->n_columns, &co);
        if (co && co->n) yexp__put(c, ct, "columns", co);
        if (c->version >= 2) {
            int k;
            for (k = 0; k < out->n_columns; k++) yexp__forms_resolve(c, out->columns[k].value, 1, sub, out->columns[k].key);
        }
    }
    {
        /* with no response there is no correctness, so nothing to default to;
         * with states and no key_answer the default is 1 (3.11, D36) */
        yexp__slot* oc = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "outcome");
        const yexp_value* dflt;
        if (c->version >= 2 && out->states && !(out->response && out->response->key_answer)) {
            dflt = yexp__default(c, yexp__find(c, YEXP__S_TEMPLATE, 2u, "outcome"));
        } else if (out->response) {
            dflt = yexp__default(c, yexp__find(c, YEXP__S_TEMPLATE, c->version >= 2 ? 1u : 0xffffu, "outcome"));
        } else {
            yexp_value* e = yexp__vnew(c, YEXP_V_STRING, UINT32_MAX);
            if (e) e->str = "";
            dflt = e;
        }
        if (oc->given && oc->v && !yexp__same(oc->v, dflt)) {
            out->outcome = oc->v;
            yexp__put(c, ct, "outcome", (yjs_value*)oc->src);
        } else {
            out->outcome = dflt;
        }
    }
    {
        /* whether a leaf with tracks runs this is known after the timeline:
         * yexp__finish_method_values() */
        yexp__slot* mv = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "method_value");
        out->method_value = mv->v;
        out->canon_ = ct;
        out->mv_written_ = mv->given;
        out->mv_pos_ = mv->pos;
        if (mv->given && mv->v && !yexp__same(mv->v, yexp__default(c, yexp__find(c, YEXP__S_TEMPLATE, 0xffffu, "method_value"))))
            out->mv_canon_ = (void*)mv->src;
    }
    if (c->version >= 2) {
        x = yexp__slot_at_m(c, s, YEXP__S_TEMPLATE, mask, "requeue_outcomes");
        if (x && x->src) {
            if (x->src->type != YJS_ARRAY || x->src->n > YEXP__MAX_LIST) {
                yexp__err(c, x->src->pos, "%s: 'requeue_outcomes' is an array of outcome values", elem);
            } else if (x->src->n) {
                yjs_value* ca = NULL;
                const yexp_value* rv = yexp__value(c, x->src, YEXP__AV_ARRAY, YEXP__AV_STR | YEXP__AV_NUM | YEXP__AV_BOOL, elem, "requeue_outcomes", &ca);
                if (rv) {
                    uint32_t q;
                    out->requeue_outcomes = rv;
                    yexp__put(c, ct, "requeue_outcomes", ca);
                    for (q = 0; q < rv->n; q++) {
                        char t[64];
                        const yexp_value* it = &rv->items[q];
                        if (it->kind == YEXP_V_STRING) snprintf(t, sizeof t, "%s", it->str);
                        else if (it->kind == YEXP_V_BOOL) snprintf(t, sizeof t, "%s", it->i ? "true" : "false");
                        else if (it->flags & YEXP_VF_INT) snprintf(t, sizeof t, "%lld", (long long)it->i);
                        else continue;
                        if (yexp__outcome_given(out, t) == 0)
                            yexp__warn(c, it->pos, "%s: 'requeue_outcomes': no exit, over or response of this template gives the outcome '%s' (3.16)", elem, t);
                    }
                }
            }
        }
        yexp__template_end(c, out, elem);
        c->tp = NULL;
        c->st_idx = c->fl_idx = NULL;
        c->n_st = c->n_fl = 0;
    }
    yexp__release(c, YEXP__S_TEMPLATE);
    yexp__put(c, cset, m->key, ct);
}

static void yexp__parse_trials(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_template* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!yexp__set_begin(c, src, "trials", YEXP__MAX_SET, &n)) { c->bad_sets |= YEXP__BAD_TRIALS; return; }
    c->trials_src = src;
    out = (yexp_template*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        out[i].name = m->key;
        yexp__parse_template(c, m, &out[i], cset);
        i++;
    }
    c->d->templates = out;
    c->d->n_templates = i;
    *canon = cset;
}

static const yexp_template* yexp__template_get(const yexp_def* d, const char* name) {
    YEXP__FIND(d->templates, d->n_templates, name);
}

static int yexp__str_cmp(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }

/* --- rules: what the sequencer's text names ----------------------------- */

/* The first tokens of one line, as ytr_rules() splits a line: blanks between
 * words, '#' outside quotes starts a comment, "a b" is one token and "" in
 * it a quote. Moves *pp past the line's LF. */
static int yexp__rule_line(const char** pp, char w[3][72]) {
    const char* p = *pp;
    int nw = 0;
    for (;;) {
        size_t k = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '\n' || *p == '#') break;
        if (*p == '"') {
            for (p++; *p && *p != '\n'; p++) {
                if (*p == '"' && p[1] != '"') { p++; break; }
                if (*p == '"') p++;
                if (nw < 3 && k < 71) w[nw][k++] = *p;
            }
        } else {
            for (; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#'; p++)
                if (nw < 3 && k < 71) w[nw][k++] = *p;
        }
        if (nw < 3) w[nw][k] = 0;
        nw++;
    }
    while (*p && *p != '\n') p++;
    *pp = *p ? p + 1 : p;
    return nw;
}

/* The tokens after `var NAME choice` on the line at p. */
static void yexp__rule_values(yexp__ctx* c, const char* p, yexp_rule_var* out) {
    const char* vals[63];
    int nw = 0, k = 0;
    bool over = false;
    for (;;) {
        const char* a;
        const char* e;
        char* tok;
        size_t len = 0;
        bool quoted;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '\n' || *p == '#') break;
        quoted = *p == '"';
        a = quoted ? p + 1 : p;
        if (quoted) {
            for (p++; *p && *p != '\n'; p++) {
                if (*p == '"' && p[1] != '"') break;
                if (*p == '"') p++;
            }
            e = p;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#') p++;
            e = p;
        }
        nw++;
        if (nw <= 3) continue;
        if (k == 62) { over = true; continue; }
        tok = (char*)yexp__alloc(c, (size_t)(e - a) + 1);
        if (!tok) return;
        for (; a < e; a++) {
            if (quoted && *a == '"') a++;
            if (a < e) tok[len++] = *a;
        }
        tok[len] = 0;
        vals[k++] = tok;
    }
    if (over || k == 0) return;
    {
        const char** v = (const char**)yexp__alloc(c, (size_t)k * sizeof(char*));
        if (!v) return;
        memcpy((void*)v, (const void*)vals, (size_t)k * sizeof(char*));
        out->values = v;
        out->n_values = k;
    }
}

/* The jitter and var names that a rules text defines, from every element. */
static void yexp__scan_rules(yexp__ctx* c, const yjs_value* arr, yexp_node* node, const char* elem) {
    const char** jit;
    yexp_rule_var* rv;
    int nj = 0, nv = 0;
    size_t cap = 0;
    uint32_t i;
    for (i = 0; i < arr->n; i++) {
        const char* p = arr->u.items[i]->u.s;
        while (*p) {
            char w[3][72];
            int nw = yexp__rule_line(&p, w);
            if (!yexp__work(c)) return;
            if (nw >= 2 && (yexp__streq(w[0], "jitter") || yexp__streq(w[0], "var"))) cap++;
        }
    }
    if (!cap) return;
    jit = (const char**)yexp__alloc(c, cap * sizeof(char*));
    rv = (yexp_rule_var*)yexp__alloc(c, cap * sizeof *rv);
    if (!jit || !rv) return;
    for (i = 0; i < arr->n; i++) {
        const yjs_value* s = arr->u.items[i];
        const char* p = s->u.s;
        while (*p) {
            char w[3][72];
            const char* line = p;
            int nw = yexp__rule_line(&p, w);
            if (nw >= 2 && (yexp__streq(w[0], "jitter") || yexp__streq(w[0], "var"))) {
                char* name;
                size_t n = strlen(w[1]);
                bool is_jit = w[0][0] == 'j';
                if (!yexp__name_ok(w[1])) {
                    yexp__err(c, s->pos, "%s: rules[%u]: the %s name '%s' is not a name: [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes", elem, (unsigned)i, w[0], w[1]);
                    continue;
                }
                if (yexp__is_builtin(w[1])) {
                    yexp__err(c, s->pos, "%s: rules[%u]: '%s' is a built-in variable's name (2.3)", elem, (unsigned)i, w[1]);
                    continue;
                }
                if (is_jit && !yexp__ends_s(w[1])) {
                    yexp__err(c, s->pos, "%s: rules[%u]: the jitter '%s' holds seconds, so its name ends in '_s' (D4)", elem, (unsigned)i, w[1]);
                    continue;
                }
                name = (char*)yexp__alloc(c, n + 1);
                if (!name) return;
                memcpy(name, w[1], n + 1);
                if (is_jit) {
                    jit[nj++] = name;
                } else {
                    rv[nv].name = name;
                    if (nw >= 3 && yexp__streq(w[2], "choice")) yexp__rule_values(c, line, &rv[nv]);
                    nv++;
                }
            }
        }
    }
    node->jitters = jit;
    node->n_jitters = nj;
    node->rule_vars = rv;
    node->n_rule_vars = nv;
}

/* --- nodes -------------------------------------------------------------- */

static void yexp__parse_node(yexp__ctx* c, const yjs_value* src, const yexp_node* parent, const char* parent_path, int depth, yexp_node* out, yjs_value** canon);

static void yexp__parse_children(yexp__ctx* c, const yjs_value* arr, const yexp_node* parent, const char* path, int depth, const yexp_node** kids, int* n_kids, yjs_value** carr_out) {
    yexp_node* out;
    yjs_value* carr;
    size_t i, a;
    *n_kids = 0;
    if (arr->type != YJS_ARRAY) { yexp__err(c, arr->pos, "'timeline': wrong type; expected an array of nodes"); return; }
    if (arr->n > YEXP__MAX_CHILDREN) { yexp__err(c, arr->pos, "'timeline': more than %d nodes", YEXP__MAX_CHILDREN); return; }
    out = (yexp_node*)yexp__alloc(c, (arr->n ? arr->n : 1) * sizeof *out);
    carr = yexp__cnew(c, YJS_ARRAY);
    if (!out || !carr) return;
    for (i = 0; i < arr->n; i++) {
        yjs_value* ce = NULL;
        yexp__parse_node(c, arr->u.items[i], parent, path, depth, &out[i], &ce);
        if (ce) yexp__cpush(c, carr, ce);
        if (out[i].name)
            for (a = 0; a < i; a++)
                if (out[a].name && yexp__streq(out[a].name, out[i].name))
                    yexp__err(c, arr->u.items[i]->pos, "node '%s': the name is used by a sibling; a node's path names it in data and seeds (2.4)", out[i].path);
    }
    *kids = out;
    *n_kids = (int)arr->n;
    *carr_out = carr;
}

static void yexp__parse_node(yexp__ctx* c, const yjs_value* src, const yexp_node* parent, const char* parent_path, int depth, yexp_node* out, yjs_value** canon) {
    char elem[128];
    yexp__slot* s;
    yjs_value* cn = NULL;
    const char* nm = NULL;
    char* path;
    size_t pn;
    yexp__slot* x;
    bool leaf, container;
    if (depth > YEXP__MAX_DEPTH) { yexp__err(c, src->pos, "the timeline nests more than %d deep", YEXP__MAX_DEPTH); return; }
    if (src->type != YJS_OBJECT) { yexp__err(c, src->pos, "a node is an object"); return; }
    {
        const yjs_value* n = yjs_get(src, "name");
        nm = n && n->type == YJS_STRING ? n->u.s : "";
    }
    pn = strlen(parent_path) + strlen(nm) + 2;
    path = (char*)yexp__alloc(c, pn);
    if (!path) return;
    snprintf(path, pn, "%s%s%s", parent_path, parent_path[0] ? "/" : "", nm);
    snprintf(elem, sizeof elem, "node '%s'", path);
    s = yexp__object(c, YEXP__S_NODE, 0xffffu, src, elem, &cn);
    if (!s) return;
    out->name = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "name"));
    out->path = path;
    out->parent = parent;
    out->pos_ = src->pos;
    out->rules = "";
    out->repetitions = 1;
    out->record_data = true;
    x = yexp__slot_at(c, s, YEXP__S_NODE, "trial");
    leaf = x && x->given;
    {
        yexp__slot* tl = yexp__slot_at(c, s, YEXP__S_NODE, "timeline");
        container = tl && tl->given;
        if (leaf && container) yexp__err(c, src->pos, "%s: a node has 'timeline' or 'trial', not both", elem);
        else if (!leaf && !container) yexp__err(c, src->pos, "%s: a node has 'timeline' or 'trial'", elem);
        if (container && tl->src) {
            yjs_value* ca = NULL;
            yexp__parse_children(c, tl->src, out, path, depth + 1, &out->children, &out->n_children, &ca);
            if (ca) yexp__put(c, cn, "timeline", ca);
        }
    }
    if (leaf) {
        const char* tn = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "trial"));
        out->trial = tn ? yexp__template_get(c->d, tn) : NULL;
        if (tn && !out->trial && !(c->bad_sets & YEXP__BAD_TRIALS)) yexp__err(c, x->pos, "%s: 'trial': no template named '%s'", elem, tn);
    }
    x = yexp__slot_at(c, s, YEXP__S_NODE, "timeline_variables");
    if (x->v) {
        out->table = yexp__v_str(x->v);
    }
    if (container) {
        static const char* const leaf_only[] = { "rules", "tracks", "interleave", "track_rate", "requeue", NULL };
        int k;
        for (k = 0; leaf_only[k]; k++) {
            yexp__slot* y = yexp__slot_at(c, s, YEXP__S_NODE, leaf_only[k]);
            if (y && y->given) yexp__err(c, y->pos, "%s: '%s' is for a leaf (a node with 'trial'); a container opens no sequence", elem, leaf_only[k]);
        }
    }
    if (leaf) {
        yexp__slot* y = yexp__slot_at(c, s, YEXP__S_NODE, "repetitions");
        if (y && y->given) yexp__err(c, y->pos, "%s: 'repetitions' is for a container; a leaf repeats by its rules ('reps')", elem);
    }
    x = yexp__slot_at(c, s, YEXP__S_NODE, "rules");
    if (x->v && x->given) {
        const yexp_value* rv = x->v;
        size_t total = 0, off = 0;
        char* joined;
        uint32_t i;
        for (i = 0; i < rv->n; i++)
            if (strchr(rv->items[i].str, '\n'))
                yexp__err(c, rv->items[i].pos, "%s: rules[%u]: one statement per element (D5); this one holds a line break", elem, (unsigned)i);
        for (i = 0; i < rv->n; i++) total += strlen(rv->items[i].str) + 1;
        joined = (char*)yexp__alloc(c, total + 1);
        if (joined) {
            for (i = 0; i < rv->n; i++) {
                size_t n = strlen(rv->items[i].str);
                memcpy(joined + off, rv->items[i].str, n);
                off += n;
                joined[off++] = '\n';
            }
            if (off) off--;
            joined[off] = 0;
            out->rules = joined;
        }
        out->rule_items = yexp__str_array(c, rv, &out->n_rules);
        yexp__scan_rules(c, x->src, out, elem);
    }
    x = yexp__slot_at(c, s, YEXP__S_NODE, "tracks");
    if (x->v && x->given) {
        const yexp_value* tv = x->v;
        if (out->trial && tv->n) ((yexp_template*)(void*)out->trial)->has_tracks_ = true;
        const yexp_method** ms;
        uint32_t i;
        ms = (const yexp_method**)yexp__alloc(c, (tv->n ? tv->n : 1) * sizeof(void*));
        if (ms) {
            c->gen++;
            for (i = 0; i < tv->n; i++) {
                ms[i] = yexp__method_get(c->d, tv->items[i].str);
                if (!ms[i]) {
                    if (!(c->bad_sets & YEXP__BAD_METHODS)) yexp__err(c, x->pos, "%s: 'tracks': no method named '%s'", elem, tv->items[i].str);
                } else if (c->method_stamp) {
                    uint32_t* u = &c->method_stamp[ms[i] - c->d->methods];
                    if (*u == c->gen) yexp__err(c, x->pos, "%s: 'tracks': '%s' listed twice", elem, tv->items[i].str);
                    *u = c->gen;
                }
            }
            out->tracks = ms;
            out->n_tracks = (int)tv->n;
        }
    }
    {
        const char* il = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "interleave"));
        out->interleave = il && yexp__streq(il, "round_robin");
    }
    {
        const yexp_value* tr = yexp__slotv(c, s, YEXP__S_NODE, "track_rate");
        out->track_rate = tr && tr->kind == YEXP_V_NUMBER ? tr->num : 0;
        if (tr && tr->kind != YEXP_V_NUMBER) yexp__err(c, tr->pos, "%s: 'track_rate' is a literal number", elem);
    }
    x = yexp__slot_at(c, s, YEXP__S_NODE, "vars");
    if (x->src) {
        char sub[140];
        yjs_value* co = NULL;
        snprintf(sub, sizeof sub, "%s: vars", elem);
        yexp__parse_values_set(c, x->src, sub, YEXP__AV_VALUE | YEXP__AV_CHOICE | (c->version >= 2 ? YEXP__AV_VARFORM : 0u), true, &out->vars, &out->n_vars, &co);
        if (co && co->n) yexp__put(c, cn, "vars", co);
    }
    x = yexp__slot_at(c, s, YEXP__S_NODE, "data");
    if (x->src) {
        char sub[140];
        yjs_value* co = NULL;
        snprintf(sub, sizeof sub, "%s: data", elem);
        yexp__parse_values_set(c, x->src, sub, YEXP__AV_VALUE, false, &out->data, &out->n_data, &co);
        if (co && co->n) yexp__put(c, cn, "data", co);
    }
    {
        const yexp_value* r = yexp__slotv(c, s, YEXP__S_NODE, "repetitions");
        if (r && r->kind == YEXP_V_NUMBER && r->i >= 1 && r->i <= 1000000) out->repetitions = (int)r->i;
        else if (r) yexp__err(c, r->pos, "%s: 'repetitions' is a literal integer of at least 1", elem);
    }
    out->if_var = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "if"));
    out->if_pos_ = yexp__slot_at(c, s, YEXP__S_NODE, "if")->pos;
    out->unless_var = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "unless"));
    out->unless_pos_ = yexp__slot_at(c, s, YEXP__S_NODE, "unless")->pos;
    out->requeue_no_response = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "requeue")) != NULL;
    out->record_data = yexp__v_bool(yexp__slotv(c, s, YEXP__S_NODE, "record_data"), true);
    x = yexp__slot_at(c, s, YEXP__S_NODE, "loop");
    if (x->src) {
        char sub[140];
        yjs_value* cl = NULL;
        yexp__slot* s2;
        snprintf(sub, sizeof sub, "%s: loop", elem);
        s2 = yexp__object(c, YEXP__S_LOOP, 0xffffu, x->src, sub, &cl);
        if (s2) {
            const yexp_value* p = yexp__slotv(c, s2, YEXP__S_LOOP, "until_accuracy");
            const yexp_value* mx = yexp__slotv(c, s2, YEXP__S_LOOP, "max");
            out->has_loop = true;
            if (p && p->kind == YEXP_V_NUMBER && p->num >= 0 && p->num <= 1) out->loop_until = p->num;
            else if (p) yexp__err(c, p->pos, "%s: 'until_accuracy' is a literal proportion from 0 to 1", sub);
            if (mx && mx->kind == YEXP_V_NUMBER && mx->i >= 1 && mx->i <= 1000000) out->loop_max = (int)mx->i;
            else if (mx) yexp__err(c, mx->pos, "%s: 'max' is a literal integer of at least 1", sub);
            yexp__put(c, cn, "loop", cl);
            yexp__release(c, YEXP__S_LOOP);
        }
    }
    yexp__release(c, YEXP__S_NODE);
    *canon = cn;
}

/* --- the second pass: references, scopes, lookups, the host ------------- */

enum { YEXP__PH_SESSION, YEXP__PH_NODE, YEXP__PH_TRIAL, YEXP__PH_AFTER, YEXP__PH_ROW };
static const char* const yexp__phase_names[] = { "session start", "node start", "trial start", "'after'", "the data row" };

enum { YEXP__VK_BUILTIN = 1, YEXP__VK_SESSION, YEXP__VK_VARS, YEXP__VK_JITTER, YEXP__VK_RULEVAR, YEXP__VK_TABLE, YEXP__VK_DIM,
       YEXP__VK_FLAG, YEXP__VK_WALK };

typedef struct yexp__var {
    int               kind;
    int               phase;
    const yexp_value* value;     /* VK_VARS                                 */
    const yexp_rule_var* rv;     /* VK_RULEVAR                              */
    const char*       table;     /* VK_TABLE                                */
    int               col, ctype;
    bool              unverified; /* VK_TABLE: no column() to ask           */
} yexp__var;

typedef struct yexp__scope {
    const yexp_node*     chain[YEXP__MAX_DEPTH + 3];
    int                  n;
    const yexp_node*     leaf;       /* NULL while reading a node's if or unless */
    const yexp_template* t;
    int                  rt[3];      /* the leaf's rules can make practice, warmup, after_break true */
    int                  michelson;
    bool                 in_code;    /* inside response.trigger.code        */
    bool                 rsp_only;   /* read only on a trial with a response:
                                      * method_value, outcome, the body of
                                      * "if": "$responded", a lookup's entry
                                      * for a pressed key                   */
    bool                 track_only; /* read only on a track trial:
                                      * method_value, a lookup's entry for a
                                      * method's name                       */
    bool                 flags_ok;   /* version 2: a state's steps, an exit's
                                      * if, a column: $FLAG reads a flag    */
} yexp__scope;

/* Whether a response can end with no key: only a deadline of literal 0 has
 * none. */
static bool yexp__can_time_out(const yexp__scope* s) {
    const yexp_response* r = s->t ? s->t->response : NULL;
    return r && !(r->duration && r->duration->kind == YEXP_V_NUMBER && r->duration->i == 0);
}

/* What a lookup's entry `key` promises about the trial that reads it: 1 a
 * response, 2 a track trial, 0 nothing. A lookup reads only the entry its
 * 'by' selects, so an entry for a pressed key never runs on a timeout. */
static unsigned yexp__entry_scope(const yexp_value* by, const char* key) {
    if (!by || by->kind != YEXP_V_VAR) return 0;
    if (yexp__streq(by->str, "responded") || yexp__streq(by->str, "correct")) return yexp__streq(key, "true") ? 1u : 0u;
    if (yexp__streq(by->str, "outcome")) return yexp__streq(key, "correct") || yexp__streq(key, "wrong") || yexp__streq(key, "responded") ? 1u : 0u;
    if (yexp__streq(by->str, "response")) return key[0] ? 1u : 0u;
    if (yexp__streq(by->str, "method")) return key[0] ? 2u : 0u;
    return 0;
}

/* Column `name` of a table: 1 found, 0 not, -1 no such table or no host. */
static int yexp__table_col(yexp__ctx* c, const char* table, const char* name, int* col, int* type) {
    int i;
    if (!c->h || !c->h->column) return -1;
    for (i = 0;; i++) {
        yexp_column info;
        int rc;
        if (!yexp__work(c)) return 0;   /* a host's list has no promised end */
        memset(&info, 0, sizeof info);
        rc = c->h->column(c->h->ctx, table, i, &info);
        if (rc <= 0) return rc < 0 ? -1 : 0;
        if (info.name && yexp__streq(info.name, name)) {
            *col = i;
            *type = info.type;
            return 1;
        }
    }
}

/* What a name resolves to at this point: true with *out, or false with
 * *why saying what it would need (a built-in whose condition is not met), or
 * *why NULL for a name nothing defines. */
static bool yexp__resolve(yexp__ctx* c, const yexp__scope* s, const char* name, yexp__var* out, const char** why) {
    static const struct { const char* name; int phase; int need; } B[] = {
        { "trial_index", YEXP__PH_TRIAL, 0 }, { "block", YEXP__PH_TRIAL, 0 }, { "rep", YEXP__PH_TRIAL, 0 },
        { "condition", YEXP__PH_TRIAL, 0 }, { "practice", YEXP__PH_TRIAL, 0 }, { "warmup", YEXP__PH_TRIAL, 0 },
        { "first_in_block", YEXP__PH_TRIAL, 0 }, { "after_break", YEXP__PH_TRIAL, 0 }, { "trial_seed", YEXP__PH_TRIAL, 0 },
        { "method", YEXP__PH_TRIAL, 1 }, { "level", YEXP__PH_TRIAL, 1 }, { "estimate", YEXP__PH_ROW, 1 },
        { "response", YEXP__PH_AFTER, 2 }, { "rt_s", YEXP__PH_AFTER, 2 }, { "responded", YEXP__PH_AFTER, 2 },
        { "correct", YEXP__PH_AFTER, 3 }, { "outcome", YEXP__PH_AFTER, 2 }, { "contrast_shown", YEXP__PH_TRIAL, 4 }
    };
    int i;
    memset(out, 0, sizeof *out);
    *why = NULL;
    for (i = 0; i < (int)(sizeof B / sizeof B[0]); i++) {
        if (!yexp__streq(name, B[i].name)) continue;
        out->kind = YEXP__VK_BUILTIN;
        out->phase = B[i].phase;
        if (s->leaf) {
            if (B[i].need == 1 && s->leaf->n_tracks == 0) { *why = "'tracks' on the leaf"; return false; }
            if (B[i].need == 2 && (!s->t || !s->t->response)) { *why = "a 'response' in the template"; return false; }
            if (B[i].need == 3 && (!s->t || !s->t->response || !s->t->response->key_answer)) { *why = "'key_answer' in the response"; return false; }
            if (B[i].need == 4 && s->michelson == 0) { *why = "a stimulus shown by the template with {\"michelson\": ...}"; return false; }
            if (B[i].need == 4 && s->michelson > 1) { *why = "only one stimulus with {\"michelson\": ...} in the template, since $contrast_shown has one value"; return false; }
        }
        return true;
    }
    for (i = 0; i < c->d->n_fields; i++)
        if (yexp__streq(c->d->fields[i].name, name)) {
            out->kind = YEXP__VK_SESSION;
            out->phase = YEXP__PH_SESSION;
            return true;
        }
    if (s->flags_ok && s->t)
        for (i = 0; i < s->t->n_flags; i++)
            if (yexp__streq(s->t->flags[i], name)) {
                out->kind = YEXP__VK_FLAG;
                out->phase = YEXP__PH_TRIAL;
                return true;
            }
    for (i = s->n - 1; i >= 0; i--) {
        const yexp_node* n = s->chain[i];
        int k;
        c->steps += (long)n->n_vars + n->n_jitters + n->n_rule_vars;   /* each name read is work */
        for (k = 0; k < n->n_vars; k++) {
            const yexp_value* vv = n->vars[k].value;
            if (vv->kind == YEXP_V_WALK2D) {
                /* a walk is its five variables, not one (3.14) */
                size_t kl = strlen(n->vars[k].key);
                int q;
                if (strncmp(name, n->vars[k].key, kl) != 0) continue;
                for (q = 0; q < 5; q++)
                    if (yexp__streq(name + kl, yexp__walk_suffix[q])) {
                        out->kind = YEXP__VK_WALK;
                        out->phase = YEXP__PH_TRIAL;
                        out->value = vv;
                        return true;
                    }
                continue;
            }
            if (yexp__streq(n->vars[k].key, name)) {
                out->kind = YEXP__VK_VARS;
                out->phase = YEXP__PH_TRIAL;
                out->value = vv;
                return true;
            }
        }
        for (k = 0; k < n->n_jitters; k++)
            if (yexp__streq(n->jitters[k], name)) {
                out->kind = YEXP__VK_JITTER;
                out->phase = YEXP__PH_TRIAL;
                return true;
            }
        for (k = 0; k < n->n_rule_vars; k++)
            if (yexp__streq(n->rule_vars[k].name, name)) {
                out->kind = YEXP__VK_RULEVAR;
                out->phase = YEXP__PH_TRIAL;
                out->rv = &n->rule_vars[k];
                return true;
            }
        for (k = 0; k < n->n_tracks; k++) {
            int d;
            for (d = 0; d < n->tracks[k]->n_dims; d++)
                if (yexp__streq(n->tracks[k]->dims[d], name)) {
                    out->kind = YEXP__VK_DIM;
                    out->phase = YEXP__PH_TRIAL;
                    return true;
                }
        }
    }
    for (i = s->n - 1; i >= 0; i--) {
        const yexp_node* n = s->chain[i];
        int col = 0, type = 0, rc;
        if (!n->table) continue;
        rc = yexp__table_col(c, n->table, name, &col, &type);
        /* a leaf's own rows are per trial; a container's are fixed for its
         * children, so they are known when a child node starts */
        if (rc == 1 || (rc < 0 && (!c->h || !c->h->column))) {
            out->kind = YEXP__VK_TABLE;
            out->phase = n == s->leaf ? YEXP__PH_TRIAL : YEXP__PH_NODE;
            out->table = n->table;
            out->col = col;
            out->ctype = type;
            out->unverified = rc != 1;
            return true;
        }
    }
    return false;
}

/* What a use expects of a variable's value, where its type is known. */
enum { YEXP__W_ANY, YEXP__W_NUM, YEXP__W_NONNEG, YEXP__W_BOOL, YEXP__W_TEXT, YEXP__W_VEC3, YEXP__W_NUMS };
static const char* const yexp__want_names[] = { "any value", "a number", "a number of at least 0", "a boolean (true or false)", "text", "[r, g, b]",
                                                "an array of numbers" };

static void yexp__check_value(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int need, int want, const char* elem, const char* key);

static void yexp__type_err(yexp__ctx* c, uint32_t pos, const char* elem, const char* key, const char* name, const char* held, int want) {
    yexp__err(c, pos, "%s: '%s': '$%s' holds %s; this key takes %s", elem, key, name, held, yexp__want_names[want]);
}

static bool yexp__text_is_number(const char* s) {
    double x;
    return yjs_parse_double(s, strlen(s), &x) == YJS_OK;
}

static bool yexp__text_is_bool(const char* s) { return yexp__streq(s, "true") || yexp__streq(s, "false"); }

/* A table column: 0 and 1 are the only booleans a table can hold. */
static bool yexp__column_is_flag(yexp__ctx* c, const char* table, int col) {
    int i;
    if (!c->h->level) return true;
    for (i = 0;; i++) {
        char buf[32];
        int n;
        if (!yexp__work(c)) return true;
        n = c->h->level(c->h->ctx, table, col, i, buf, sizeof buf);
        if (n < 0) return true;
        if (n != 1 || (buf[0] != '0' && buf[0] != '1')) return false;
    }
}

static void yexp__type_var(yexp__ctx* c, const yexp__scope* s, const char* name, int want, uint32_t pos, const char* elem, const char* key, int depth);

/* Every value this can give against what the use expects. */
static void yexp__type_value(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int want, const char* name, uint32_t pos, const char* elem, const char* key, int depth) {
    uint32_t k;
    if (!yexp__work(c)) return;
    switch (v->kind) {
    case YEXP_V_BOOL:
        if (want != YEXP__W_BOOL) yexp__type_err(c, pos, elem, key, name, "a boolean", want);
        break;
    case YEXP_V_NUMBER:
        if (want == YEXP__W_NONNEG && v->num < 0) yexp__type_err(c, pos, elem, key, name, "a negative number", want);
        else if (want != YEXP__W_NUM && want != YEXP__W_NONNEG) yexp__type_err(c, pos, elem, key, name, "a number", want);
        break;
    case YEXP_V_STRING:
        if (want != YEXP__W_TEXT) yexp__type_err(c, pos, elem, key, name, "text", want);
        break;
    case YEXP_V_ARRAY:
        if (want == YEXP__W_NUMS) {
            for (k = 0; k < v->n; k++)
                if (v->items[k].kind != YEXP_V_NUMBER) { yexp__type_err(c, pos, elem, key, name, "an array with an item that is no number", want); break; }
        } else if (want != YEXP__W_VEC3 || v->n != 3) {
            yexp__type_err(c, pos, elem, key, name, "an array", want);
        } else {
            for (k = 0; k < 3; k++)
                if (v->items[k].kind != YEXP_V_NUMBER) { yexp__type_err(c, pos, elem, key, name, "an array with an item that is no number", want); break; }
        }
        break;
    case YEXP_V_VAR:
        yexp__type_var(c, s, v->str, want, pos, elem, key, depth + 1);
        break;
    case YEXP_V_LOOKUP: {
        /* the scope is the walker's own, read-only to the checks but for
         * these two flags, set while one entry is checked */
        yexp__scope* ms = (yexp__scope*)s;
        bool r0 = ms->rsp_only, t0 = ms->track_only;
        for (k = 0; k < v->n; k++) {
            unsigned f = yexp__entry_scope(v->by, v->map[k].key);
            ms->rsp_only = r0 || (f & 1u);
            ms->track_only = t0 || (f & 2u);
            yexp__type_value(c, s, v->map[k].value, want, name, pos, elem, key, depth);
        }
        ms->rsp_only = r0;
        ms->track_only = t0;
        break;
    }
    case YEXP_V_CHOICE:
        for (k = 0; k < v->n; k++) yexp__type_value(c, s, &v->items[k], want, name, pos, elem, key, depth);
        break;
    case YEXP_V_UNIFORM:
        /* one value: a number; n values: an array of them (3.14) */
        if (v->i > 1) { if (want != YEXP__W_NUMS) yexp__type_err(c, pos, elem, key, name, "an array of numbers", want); }
        else if (want == YEXP__W_NONNEG && v->items[0].num < 0) yexp__type_err(c, pos, elem, key, name, "a number that can be negative", want);
        else if (want != YEXP__W_NUM && want != YEXP__W_NONNEG) yexp__type_err(c, pos, elem, key, name, "a number", want);
        break;
    default: break;
    }
}

/* Whether the variable can give what the use expects, where that is known. */
static void yexp__type_var(yexp__ctx* c, const yexp__scope* s, const char* name, int want, uint32_t pos, const char* elem, const char* key, int depth) {
    static const char* const bools[] = { "practice", "warmup", "after_break", "first_in_block", "responded", "correct", NULL };
    static const char* const texts[] = { "method", "response", "outcome", NULL };
    yexp__var r;
    const char* why;
    int i;
    if (want == YEXP__W_ANY || depth > YEXP__VAR_CHAIN) return;
    if (!yexp__resolve(c, s, name, &r, &why)) return;
    if (want == YEXP__W_TEXT && (r.kind == YEXP__VK_BUILTIN || r.kind == YEXP__VK_SESSION || r.kind == YEXP__VK_JITTER || r.kind == YEXP__VK_DIM)) {
        /* D14: a text is laid out at pack build, from levels known then */
        if (r.kind == YEXP__VK_JITTER) yexp__err(c, pos, "%s: '%s': '$%s' is a jitter, a duration; a text comes from a string column or a lookup", elem, key, name);
        else yexp__err(c, pos, "%s: '%s': '$%s' is known only at run time; this key needs the levels at pack build: a string column, a vars value or a lookup", elem, key, name);
        return;
    }
    switch (r.kind) {
    case YEXP__VK_BUILTIN: {
        bool is_bool = false, is_text = false;
        for (i = 0; bools[i]; i++) if (yexp__streq(name, bools[i])) is_bool = true;
        for (i = 0; texts[i]; i++) if (yexp__streq(name, texts[i])) is_text = true;
        if (want == YEXP__W_NUM || want == YEXP__W_NONNEG) {
            if (yexp__streq(name, "rt_s") && !s->rsp_only && yexp__can_time_out(s)) {
                yexp__err(c, pos, "%s: '%s': '$rt_s' has no value on a timeout, so a number key cannot read it; use it in 'columns', or take a value by a lookup on $responded", elem, key);
                break;
            }
            if (yexp__streq(name, "level") && !s->track_only && s->leaf && s->leaf->table && s->leaf->n_tracks) {
                yexp__err(c, pos, "%s: '%s': '$level' has no value on a condition trial of a leaf with both 'tracks' and a table, so a number key cannot read it", elem, key);
                break;
            }
        }
        if (is_bool ? want != YEXP__W_BOOL : is_text ? true : (want != YEXP__W_NUM && want != YEXP__W_NONNEG))
            yexp__type_err(c, pos, elem, key, name, is_bool ? "a boolean" : is_text ? "text" : "a number", want);
        break;
    }
    case YEXP__VK_SESSION:
        for (i = 0; i < c->d->n_fields; i++)
            if (yexp__streq(c->d->fields[i].name, name)) {
                bool text = c->d->fields[i].is_string;
                /* an integer field can stand for a boolean; the player checks
                 * at session start that its value is 0 or 1 */
                if ((text && want != YEXP__W_TEXT) || (!text && want != YEXP__W_NUM && want != YEXP__W_NONNEG && want != YEXP__W_BOOL))
                    yexp__type_err(c, pos, elem, key, name, text ? "text" : "an integer", want);
            }
        break;
    case YEXP__VK_VARS:
        yexp__type_value(c, s, r.value, want, name, pos, elem, key, depth);
        break;
    case YEXP__VK_JITTER: case YEXP__VK_DIM: case YEXP__VK_WALK:
        if (want != YEXP__W_NUM && want != YEXP__W_NONNEG) yexp__type_err(c, pos, elem, key, name, "a number", want);
        break;
    case YEXP__VK_FLAG:
        if (want != YEXP__W_BOOL) yexp__type_err(c, pos, elem, key, name, "a boolean (a flag)", want);
        break;
    case YEXP__VK_RULEVAR: {
        /* the rules' choice values are text; they can read as numbers */
        bool all_num = r.rv->n_values > 0, all_bool = r.rv->n_values > 0;
        for (i = 0; i < r.rv->n_values; i++) {
            all_num = all_num && yexp__text_is_number(r.rv->values[i]);
            all_bool = all_bool && yexp__text_is_bool(r.rv->values[i]);
        }
        if (!r.rv->n_values) break;
        if ((want == YEXP__W_NUM || want == YEXP__W_NONNEG) && !all_num) yexp__type_err(c, pos, elem, key, name, "text", want);
        else if (want == YEXP__W_BOOL && !all_bool) yexp__type_err(c, pos, elem, key, name, "text", want);
        else if (want == YEXP__W_VEC3 || want == YEXP__W_NUMS) yexp__type_err(c, pos, elem, key, name, "text", want);
        break;
    }
    case YEXP__VK_TABLE:
        if (r.unverified) break;
        if (want == YEXP__W_TEXT && r.ctype != YEXP_COL_STRING) {
            yexp__err(c, pos, "%s: '%s': '$%s' is a column of '%s' that is not a string; a text comes from a string column or a lookup", elem, key, name, r.table);
        } else if ((want == YEXP__W_NUM || want == YEXP__W_NONNEG) && r.ctype == YEXP_COL_STRING) {
            yexp__type_err(c, pos, elem, key, name, "text", want);
        } else if (want == YEXP__W_VEC3 || want == YEXP__W_NUMS) {
            yexp__type_err(c, pos, elem, key, name, "one value per row", want);
        } else if (want == YEXP__W_BOOL && (r.ctype != YEXP_COL_INTEGER || !yexp__column_is_flag(c, r.table, r.col))) {
            yexp__type_err(c, pos, elem, key, name, r.ctype == YEXP_COL_INTEGER ? "integers other than 0 and 1" : r.ctype == YEXP_COL_STRING ? "text" : "a number", want);
        }
        break;
    default: break;
    }
}

static void yexp__check_var(yexp__ctx* c, const yexp__scope* s, const char* name, uint32_t pos, int need, int want, const char* elem, const char* key) {
    yexp__var r;
    const char* why;
    if (s->in_code && yexp__streq(name, "response") && s->t && s->t->response) {
        /* a response rule's code is per choice (G8), so the variable is known
         * here; it is text, so a code reads it through a lookup */
        if (want != YEXP__W_ANY) yexp__err(c, pos, "%s: '%s': '$response' is text and a code is a number; use a lookup by $response", elem, key);
        return;
    }
    if (!yexp__resolve(c, s, name, &r, &why)) {
        int i;
        /* finding F14: a flag is read in a state's steps, an exit's if and a
         * column, where 3.12 and 3.15 read it */
        for (i = 0; s->t && i < s->t->n_flags; i++)
            if (yexp__streq(s->t->flags[i], name)) {
                yexp__err(c, pos, "%s: '%s': '$%s' is a flag; a flag is read in a state's steps, an exit's 'if' and a column (3.12)", elem, key, name);
                return;
            }
        if (why) yexp__err(c, pos, "%s: '%s': '$%s' needs %s (2.3)", elem, key, name, why);
        else yexp__err(c, pos, "%s: '%s': '$%s' is not known here: no session field, table column, vars name, jitter or built-in has that name", elem, key, name);
        return;
    }
    if (r.phase > need) {
        yexp__err(c, pos, "%s: '%s': '$%s' is known from %s, not here (2.3)", elem, key, name, yexp__phase_names[r.phase]);
        return;
    }
    yexp__type_var(c, s, name, want, pos, elem, key, 0);
}

/* --- lookups: a key for every level (format 2.2) ------------------------ */

/* A lookup this pass could not finish is recorded, never reported as run. */
static void yexp__levels_skipped(yexp__ctx* c) {
    c->lv_over = true;
    c->r->skipped |= YEXP_CHECK_LEVELS;
    c->r->run &= ~(unsigned)YEXP_CHECK_LEVELS;
}

static int yexp__lv_push(yexp__ctx* c, const char* level) {
    if (c->n_lv >= YEXP__MAX_LEVELS) { yexp__levels_skipped(c); return 0; }
    if (c->n_lv == c->c_lv) {
        int nc = c->c_lv ? 2 * c->c_lv : 64;
        const char** q = (const char**)yexp__alloc(c, (size_t)nc * sizeof(char*));
        if (!q) return -1;
        if (c->n_lv) memcpy((void*)q, (const void*)c->lv, (size_t)c->n_lv * sizeof(char*));
        c->lv = q;
        c->c_lv = nc;
    }
    c->lv[c->n_lv++] = level;
    return 0;
}

static const char* yexp__num_text(yexp__ctx* c, const yexp_value* v) {
    char buf[64];
    int n;
    char* p;
    if (v->flags & YEXP_VF_DURATION) {
        yjs_value* j = yjs_new_fixed(c->a, v->i, 9);
        if (!j) { c->nomem = true; return NULL; }
        return j->u.s;
    }
    if (v->flags & YEXP_VF_INT) n = snprintf(buf, sizeof buf, "%lld", (long long)v->i);
    else n = yjs_format_double(v->num, buf, sizeof buf);
    if (n < 0 || n >= (int)sizeof buf) return NULL;
    p = (char*)yexp__alloc(c, (size_t)n + 1);
    if (!p) return NULL;
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

static int yexp__levels(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int depth);

/* The levels "$name" takes in this leaf: 1 known, 0 not known, -1 no memory. */
static int yexp__levels_var(yexp__ctx* c, const yexp__scope* s, const char* name, int depth) {
    static const char* const bools[] = { "practice", "warmup", "after_break", "first_in_block", "responded", "correct", NULL };
    yexp__var r;
    const char* why;
    int i;
    if (depth > YEXP__VAR_CHAIN) { yexp__levels_skipped(c); return 0; }
    for (i = 0; bools[i]; i++)
        if (yexp__streq(name, bools[i])) {
            /* the first three are true only on trials the rules make; read
             * only with a response, $responded is true */
            if (!(s->rsp_only && i == 4) && yexp__lv_push(c, "false")) return -1;
            if ((i >= 3 || s->rt[i]) && yexp__lv_push(c, "true")) return -1;
            return 1;
        }
    if (yexp__streq(name, "outcome")) {
        const yexp_response* rsp = s->t ? s->t->response : NULL;
        if (!rsp) return 0;
        /* with no key_answer a press is neither correct nor wrong */
        if (rsp->key_answer ? (yexp__lv_push(c, "correct") || yexp__lv_push(c, "wrong")) : yexp__lv_push(c, "responded")) return -1;
        /* a timeout needs a deadline */
        if (!s->rsp_only && yexp__can_time_out(s) && yexp__lv_push(c, "timeout")) return -1;
        return 1;
    }
    if (yexp__streq(name, "response")) {
        const yexp_response* rsp = s->t ? s->t->response : NULL;
        if (!rsp || !rsp->n_choices) return 0;
        for (i = 0; i < rsp->n_choices; i++)
            if (yexp__lv_push(c, rsp->choices[i].key)) return -1;
        /* a timeout has no key. A trigger's code goes out only for a press. */
        if (!s->in_code && !s->rsp_only && yexp__can_time_out(s) && yexp__lv_push(c, "")) return -1;
        return 1;
    }
    if (yexp__streq(name, "method")) {
        if (!s->leaf || s->leaf->n_tracks == 0) return 0;
        for (i = 0; i < s->leaf->n_tracks; i++)
            if (yexp__lv_push(c, s->leaf->tracks[i]->name)) return -1;
        /* a condition trial has no method */
        if (s->leaf->table && !s->track_only && yexp__lv_push(c, "")) return -1;
        return 1;
    }
    if (yexp__is_builtin(name)) return 0;
    if (!yexp__resolve(c, s, name, &r, &why)) return 0;
    if (r.kind == YEXP__VK_VARS) return yexp__levels(c, s, r.value, depth);
    if (r.kind == YEXP__VK_RULEVAR) {
        if (!r.rv->n_values) return 0;
        for (i = 0; i < r.rv->n_values; i++)
            if (yexp__lv_push(c, r.rv->values[i])) return -1;
        return 1;
    }
    if (r.kind == YEXP__VK_TABLE && !r.unverified && c->h && c->h->level) {
        for (i = 0;; i++) {
            char buf[256];
            int n = c->h->level(c->h->ctx, r.table, r.col, i, buf, sizeof buf);
            char* p;
            buf[sizeof buf - 1] = 0;
            if (n < 0) return 1;
            if (!yexp__work(c)) return 0;
            if (n >= YEXP__LEVEL_BYTES) { yexp__levels_skipped(c); return 0; }
            p = (char*)yexp__alloc(c, (size_t)n + 1);
            if (!p) return -1;
            if (n < (int)sizeof buf) {
                memcpy(p, buf, (size_t)n);
            } else if (c->h->level(c->h->ctx, r.table, r.col, i, p, (size_t)n + 1) != n) {
                yexp__levels_skipped(c);
                return 0;
            }
            p[n] = 0;
            if (yexp__lv_push(c, p)) return -1;
            if (c->lv_over) return 0;
        }
    }
    return 0;
}

static int yexp__levels(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int depth) {
    uint32_t k;
    int rc, all;
    if (!yexp__work(c)) return 0;
    switch (v->kind) {
    case YEXP_V_STRING: return yexp__lv_push(c, v->str) ? -1 : 1;
    case YEXP_V_BOOL: return yexp__lv_push(c, v->i ? "true" : "false") ? -1 : 1;
    case YEXP_V_NUMBER: {
        const char* t = yexp__num_text(c, v);
        if (!t) return c->nomem ? -1 : 0;
        return yexp__lv_push(c, t) ? -1 : 1;
    }
    case YEXP_V_VAR: return yexp__levels_var(c, s, v->str, depth + 1);
    case YEXP_V_CHOICE:
        all = 1;
        for (k = 0; k < v->n; k++) {
            if ((rc = yexp__levels(c, s, &v->items[k], depth)) < 0) return rc;
            all &= rc;
        }
        return all;
    case YEXP_V_LOOKUP:
        all = 1;
        for (k = 0; k < v->n; k++) {
            if ((rc = yexp__levels(c, s, v->map[k].value, depth)) < 0) return rc;
            all &= rc;
        }
        return all;
    default: return 0;
    }
}


/* Sorts c->lv and drops repeats. */
static void yexp__lv_unique(yexp__ctx* c) {
    int i, k = 0;
    if (c->n_lv < 2) return;
    qsort((void*)c->lv, (size_t)c->n_lv, sizeof(char*), yexp__str_cmp);
    for (i = 1; i < c->n_lv; i++)
        if (strcmp(c->lv[i], c->lv[k]) != 0) c->lv[++k] = c->lv[i];
    c->n_lv = k + 1;
}

static int yexp__kv_find(const yexp_kv* map, uint32_t n, const char* key) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(map[mid].key, key);
        if (cmp == 0) return (int)mid;
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

/* "a", "b" and "c", at most 8, then how many more. */
static void yexp__quote_list(char* out, size_t cap, const char* const* v, int n) {
    size_t at = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < n && i < 8 && at < cap; i++) {
        int w = snprintf(out + at, cap - at, "%s\"%s\"", i == 0 ? "" : i + 1 == n ? " and " : ", ", v[i]);
        if (w < 0) break;
        at += (size_t)w;
    }
    if (n > 8 && at < cap) snprintf(out + at, cap - at, " and %d more", n - 8);
}

static void yexp__by_text(const yexp_value* by, char* out, size_t cap) {
    if (by->kind == YEXP_V_VAR) snprintf(out, cap, "$%s", by->str);
    else if (by->kind == YEXP_V_STRING) snprintf(out, cap, "%s", by->str);
    else snprintf(out, cap, "a value");
}

/* One lookup in the current leaf: refuse a level without a key, mark the
 * keys a level reached for the warning at the end. */
static void yexp__check_lookup(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, const char* elem, const char* key) {
    int i, nmiss = 0, rc;
    c->n_lv = 0;
    c->lv_over = false;
    rc = yexp__levels(c, s, v->by, 0);
    if (rc < 0) return;
    if (rc == 0 || c->lv_over) { v->reached_[v->n] |= 2u; return; }   /* some level is not known here */
    yexp__lv_unique(c);
    for (i = 0; i < c->n_lv; i++) {
        int k = yexp__kv_find(v->map, v->n, c->lv[i]);
        if (k < 0) c->lv[nmiss++] = c->lv[i];
        else v->reached_[k] = 1;
    }
    v->reached_[v->n] |= 1u;
    if (nmiss) {
        char list[400], by[80];
        yexp__quote_list(list, sizeof list, c->lv, nmiss);
        yexp__by_text(v->by, by, sizeof by);
        yexp__err(c, v->pos, "%s: '%s': the lookup's map has no key for %s, which '%s' can take in node '%s'; a lookup needs a key for every level of its 'by'",
                  elem, key, list, by, s->leaf ? s->leaf->path : "");
    }
}

static void yexp__check_value(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int need, int want, const char* elem, const char* key) {
    uint32_t k;
    if (!v || !yexp__work(c)) return;
    switch (v->kind) {
    case YEXP_V_VAR:
        yexp__check_var(c, s, v->str, v->pos, need, want, elem, key);
        break;
    case YEXP_V_LOOKUP:
        yexp__check_value(c, s, v->by, need, YEXP__W_ANY, elem, key);
        if (s->t && s->t->response && !s->t->response->n_choices && v->by && v->by->kind == YEXP_V_VAR && yexp__streq(v->by->str, "response"))
            yexp__err(c, v->pos, "%s: '%s': a lookup by $response needs the response's 'choices': any key can be pressed, so the map cannot name them", elem, key);
        {
            yexp__scope* ms = (yexp__scope*)s;   /* see yexp__type_value */
            bool r0 = ms->rsp_only, t0 = ms->track_only;
            for (k = 0; k < v->n; k++) {
                unsigned f = yexp__entry_scope(v->by, v->map[k].key);
                ms->rsp_only = r0 || (f & 1u);
                ms->track_only = t0 || (f & 2u);
                yexp__check_value(c, s, v->map[k].value, need, want, elem, key);
            }
            ms->rsp_only = r0;
            ms->track_only = t0;
        }
        if (s->leaf) yexp__check_lookup(c, s, v, elem, key);
        break;
    case YEXP_V_ARRAY: case YEXP_V_CHOICE:
        for (k = 0; k < v->n; k++) yexp__check_value(c, s, &v->items[k], need, want == YEXP__W_VEC3 ? YEXP__W_NUM : want, elem, key);
        break;
    case YEXP_V_LINSPACE:
        for (k = 0; k < v->n; k++) yexp__check_value(c, s, &v->items[k], need, YEXP__W_NUM, elem, key);
        break;
    case YEXP_V_MICHELSON:
        yexp__check_value(c, s, v->by, need, YEXP__W_NUM, elem, key);
        break;
    case YEXP_V_SIGNAL: case YEXP_V_TRANSFORM:
        /* an above's level, a rotation's or a mirror's angle */
        yexp__check_value(c, s, v->by, need, YEXP__W_NUM, elem, key);
        break;
    case YEXP_V_WHEN:
        yexp__check_value(c, s, v->by, need, YEXP__W_ANY, elem, key);
        for (k = 0; k < 2; k++) yexp__check_value(c, s, &v->items[k], need, want, elem, key);
        break;
    default: break;
    }
}

/* What a row's key expects of a variable. */
static int yexp__row_want(const yexp_row* r) {
    switch (r->type) {
    case YEXP__T_NUM: case YEXP__T_INT: return YEXP__W_NUM;
    case YEXP__T_DUR: return yexp__streq(r->key, "minimum_valid_rt_s") ? YEXP__W_NUM : YEXP__W_NONNEG;
    case YEXP__T_CODE: return YEXP__W_NONNEG;
    case YEXP__T_VEC3: return YEXP__W_VEC3;
    case YEXP__T_TEXT: case YEXP__T_RES: return YEXP__W_TEXT;
    default: return YEXP__W_ANY;
    }
}

static int yexp__stim_want(const yexp__ctx* c, const yexp_stim* st, const char* key) {
    int ri = yexp__find(c, YEXP__S_STIM, 1u << (int)st->kind, key);
    return ri >= 0 ? yexp__row_want(&c->rows[ri]) : YEXP__W_ANY;
}

/* A michelson value, also as one of a lookup's values. */
static bool yexp__has_michelson(const yexp_value* v) {
    uint32_t k;
    if (v->kind == YEXP_V_MICHELSON) return true;
    if (v->kind == YEXP_V_LOOKUP)
        for (k = 0; k < v->n; k++)
            if (yexp__has_michelson(v->map[k].value)) return true;
    return false;
}

/* 1 = used before 'after', 2 = only in it. */
static void yexp__mark_use(yexp__ctx* c, int index, int value) {
    uint8_t* u = &c->stim_use[index];
    if (!*u) c->used[c->n_used++] = index;
    if (value == 1 || !*u) *u = (uint8_t)value;
}

static int yexp__int_cmp(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

/* Marks every stimulus the steps name. */
static void yexp__mark_stims(yexp__ctx* c, const yexp_step* steps, int n, int phase) {
    int i, j;
    for (i = 0; i < n; i++) {
        const yexp_step* st = &steps[i];
        if (!yexp__work(c)) return;
        c->steps += st->n_stims;
        for (j = 0; j < st->n_stims; j++) {
            const yexp_stim* ps = yexp_stim_get(c->d, st->stims[j]);
            if (ps) {
                yexp__mark_use(c, (int)(ps - c->d->stimuli), phase == YEXP__PH_TRIAL ? 1 : 2);
            }
        }
        if (st->op == YEXP_IF || st->op == YEXP_UNLESS) yexp__mark_stims(c, st->steps, st->n_steps, phase);
    }
}

static void yexp__check_steps(yexp__ctx* c, const yexp__scope* s, const yexp_step* steps, int n, int phase, const char* prefix) {
    int i, k;
    for (i = 0; i < n; i++) {
        const yexp_step* st = &steps[i];
        char elem[128];
        snprintf(elem, sizeof elem, "%s[%d]", prefix, i);
        if (!yexp__work(c)) return;
        yexp__check_value(c, s, st->t, phase, YEXP__W_NONNEG, elem, st->op == YEXP_AT ? "at_s" : "wait_s");
        yexp__check_value(c, s, st->stagger, phase, YEXP__W_NONNEG, elem, "stagger_s");
        yexp__check_value(c, s, st->duration, phase, YEXP__W_NONNEG, elem, "duration_s");
        yexp__check_value(c, s, st->delay, phase, YEXP__W_NONNEG, elem, "delay_s");
        yexp__check_value(c, s, st->code, phase, YEXP__W_NONNEG, elem, "trigger");
        yexp__check_value(c, s, st->timeout, phase, YEXP__W_NONNEG, elem, "timeout_s");
        yexp__check_value(c, s, st->point, phase, YEXP__W_NUM, elem, "to");
        if (st->n_props) {
            const yexp_stim* ps = yexp_stim_get(c->d, st->stims[0]);
            for (k = 0; k < st->n_props; k++)
                yexp__check_value(c, s, st->props[k].value, phase, ps ? yexp__stim_want(c, ps, st->props[k].key) : YEXP__W_ANY, elem, st->props[k].key);
        }
        if (st->op == YEXP_IF || st->op == YEXP_UNLESS) {
            yexp__scope* ms = (yexp__scope*)s;   /* see yexp__type_value */
            bool r0 = ms->rsp_only;
            yexp__check_var(c, s, st->cond, st->cond_pos_, phase, YEXP__W_BOOL, elem, st->op == YEXP_IF ? "if" : "unless");
            /* $correct is true only with a press */
            if (st->op == YEXP_IF && st->cond && (yexp__streq(st->cond, "responded") || yexp__streq(st->cond, "correct"))) ms->rsp_only = true;
            yexp__check_steps(c, s, st->steps, st->n_steps, phase, elem);
            ms->rsp_only = r0;
        }
    }
}

/* Whether the leaf's rules can make practice, warmup and after_break true:
 * a key is required only for a value some trial can take (pack.md 16). Rules
 * this cannot read keep both values. */
static void yexp__rules_truth(const yexp_node* leaf, int rt[3]) {
    const char* p = leaf->rules;
    rt[0] = rt[1] = rt[2] = 0;
    while (p && *p) {
        char w[3][72];
        int nw = yexp__rule_line(&p, w);
        if (nw < 1) continue;
        if (yexp__streq(w[0], "practice") || yexp__streq(w[0], "warmup") || yexp__streq(w[0], "block_size")) {
            int k = w[0][0] == 'p' ? 0 : w[0][0] == 'w' ? 1 : 2;
            const char* t = nw >= 2 ? w[1] : "";
            bool digits = *t != 0, nonzero = false;
            for (; *t; t++) {
                if (*t < '0' || *t > '9') digits = false;
                else if (*t != '0') nonzero = true;
            }
            if (!digits || nonzero) rt[k] = 1;
        } else if (yexp__streq(w[0], "groups")) {
            if (nw < 3 || !yexp__streq(w[2], "alternate")) rt[2] = 1;
        }
    }
}

static int yexp__entry(yexp__ctx* c, int kind, const char* name) {
    if (!c->h || !c->h->entry) return 1;
    return c->h->entry(c->h->ctx, kind, name);
}

/* The levels of a variable that names a pack entry, each checked. */
static void yexp__check_entry_levels(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int kind, const char* elem, const char* key) {
    int i, rc;
    if (!c->h || !c->h->entry) return;
    c->n_lv = 0;
    c->lv_over = false;
    rc = yexp__levels(c, s, v, 0);
    if (rc <= 0 || c->lv_over) return;
    yexp__lv_unique(c);
    for (i = 0; i < c->n_lv; i++)
        if (!c->h->entry(c->h->ctx, kind, c->lv[i]))
            yexp__err(c, v->pos, "%s: '%s': no pack entry named '%s' (a level of '$%s')", elem, key, c->lv[i], v->str);
}

/* Follows a vars value through the names it holds. path[] holds the values
 * being followed, so a name that leads back to one of them is a real loop;
 * a long chain that ends is valid up to YEXP__VAR_CHAIN. */
static void yexp__check_cycle(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, const char* elem, const char* key, const yexp_value** path, int depth) {
    uint32_t k;
    if (!v || !yexp__work(c)) return;
    switch (v->kind) {
    case YEXP_V_VAR: {
        yexp__var r;
        const char* why;
        int q;
        if (!yexp__resolve(c, s, v->str, &r, &why) || r.kind != YEXP__VK_VARS) return;
        for (q = 0; q < depth; q++)
            if (path[q] == r.value) {
                yexp__err(c, v->pos, "%s: '%s': '$%s': the variables refer to each other in a loop", elem, key, v->str);
                return;
            }
        if (depth >= YEXP__VAR_CHAIN) {
            yexp__err(c, v->pos, "%s: '%s': '$%s': variables nest more than %d deep", elem, key, v->str, YEXP__VAR_CHAIN);
            return;
        }
        path[depth] = r.value;
        yexp__check_cycle(c, s, r.value, elem, key, path, depth + 1);
        break;
    }
    case YEXP_V_LOOKUP:
        yexp__check_cycle(c, s, v->by, elem, key, path, depth);
        for (k = 0; k < v->n; k++) yexp__check_cycle(c, s, v->map[k].value, elem, key, path, depth);
        break;
    case YEXP_V_CHOICE: case YEXP_V_ARRAY:
        for (k = 0; k < v->n; k++) yexp__check_cycle(c, s, &v->items[k], elem, key, path, depth);
        break;
    default: break;
    }
}

/* A host's message may come back without its NUL, or empty. */
static const char* yexp__host_msg(char* msg, size_t cap) {
    msg[cap - 1] = 0;
    return msg[0] ? msg : "refused by the host";
}

/* Every use of a variable in one leaf's trial. */
/* A template's written outcome goes to ytr_update(): an integer or a
 * boolean, or the word "correct". */
static void yexp__check_outcome(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, const char* elem, int top, int depth) {
    uint32_t k;
    yexp__var r;
    const char* why;
    if (!v || depth > YEXP__VAR_CHAIN) return;
    switch (v->kind) {
    case YEXP_V_BOOL: return;
    case YEXP_V_NUMBER:
        if (!(v->flags & YEXP_VF_INT)) yexp__err(c, v->pos, "%s: 'outcome' is an integer, a boolean or \"correct\"; ytr_update() takes an integer", elem);
        return;
    case YEXP_V_STRING:
        /* "" is the written form of the default with no response */
        if (!(top && (yexp__streq(v->str, "correct") || !v->str[0]))) yexp__err(c, v->pos, "%s: 'outcome' is an integer, a boolean or \"correct\", not the text '%s'", elem, v->str);
        return;
    case YEXP_V_LOOKUP:
        for (k = 0; k < v->n; k++) yexp__check_outcome(c, s, v->map[k].value, elem, 0, depth);
        return;
    case YEXP_V_CHOICE:
        for (k = 0; k < v->n; k++) yexp__check_outcome(c, s, &v->items[k], elem, 0, depth);
        return;
    case YEXP_V_VAR:
        if (!yexp__resolve(c, s, v->str, &r, &why)) return;   /* reported by the value's check */
        switch (r.kind) {
        case YEXP__VK_BUILTIN: {
            static const char* const ok[] = { "responded", "correct", "practice", "warmup", "first_in_block", "after_break",
                                              "trial_index", "block", "rep", "condition", NULL };
            int i;
            for (i = 0; ok[i]; i++) if (yexp__streq(v->str, ok[i])) return;
            break;
        }
        case YEXP__VK_SESSION: {
            int i;
            for (i = 0; i < c->d->n_fields; i++)
                if (yexp__streq(c->d->fields[i].name, v->str) && !c->d->fields[i].is_string) return;
            break;
        }
        case YEXP__VK_VARS:
            yexp__check_outcome(c, s, r.value, elem, 0, depth + 1);
            return;
        case YEXP__VK_TABLE:
            if (r.unverified || r.ctype == YEXP_COL_INTEGER) return;
            break;
        default:
            break;
        }
        yexp__err(c, v->pos, "%s: 'outcome': '$%s' is not an integer or a boolean; ytr_update() takes an integer", elem, v->str);
        return;
    default:
        yexp__err(c, v->pos, "%s: 'outcome' is an integer, a boolean or \"correct\"", elem);
        return;
    }
}

/* A control's variables in one leaf (3.7: set at the trial's start). */
static void yexp__check_control_vars(yexp__ctx* c, yexp__scope* s, const yexp_control* x, const char* elem) {
    int k, q;
    unsigned mask = 1u << (int)x->kind;
    for (k = 0; k < x->n_props; k++) {
        const yexp_value* v = x->props[k].value;
        int ri = yexp__find(c, YEXP__S_CONTROL, mask, x->props[k].key);
        int want = YEXP__W_ANY;
        if (ri >= 0) {
            int ty = c->rows[ri].type;
            want = ty == YEXP__T_DUR ? YEXP__W_NONNEG : (ty == YEXP__T_NUM || ty == YEXP__T_POINT || ty == YEXP__T_CENTER) ? YEXP__W_NUM : YEXP__W_ANY;
        }
        if (v->kind == YEXP_V_SIGNAL && !v->by) continue;
        yexp__check_value(c, s, v, YEXP__PH_TRIAL, want, elem, x->props[k].key);
    }
    for (q = 0; q < 2 && x->kind == YEXP_MOVING_TARGET; q++) {
        const yexp_sines* sn = q ? &x->sy : &x->sx;
        const char* key = q ? "y" : "x";
        yexp__var r;
        const char* why;
        if (!sn->phases || sn->phases->kind != YEXP_V_VAR) continue;
        yexp__check_value(c, s, sn->phases, YEXP__PH_TRIAL, YEXP__W_NUMS, elem, key);
        /* the phases variable has the lane's n values */
        if (yexp__resolve(c, s, sn->phases->str, &r, &why) && r.kind == YEXP__VK_VARS) {
            const yexp_value* v = r.value;
            uint32_t got = v->kind == YEXP_V_UNIFORM ? (uint32_t)v->i : v->kind == YEXP_V_ARRAY ? v->n : (uint32_t)sn->n;
            if (got != (uint32_t)sn->n)
                yexp__err(c, sn->phases->pos, "%s: '%s': 'phases': '$%s' has %u values and the lane %d components (3.7)", elem, key, sn->phases->str, (unsigned)got, sn->n);
        }
    }
}

static void yexp__check_exit_vars(yexp__ctx* c, yexp__scope* s, const yexp_exit* e, const char* elem) {
    if (e->if_var) yexp__check_var(c, s, e->if_var, e->if_pos_, YEXP__PH_TRIAL, YEXP__W_BOOL, elem, "if");
    if (e->outcome) yexp__check_value(c, s, e->outcome, YEXP__PH_TRIAL, YEXP__W_ANY, elem, "outcome");
}

/* The version 2 parts of one leaf's trial (3.9, 3.11, 3.14). */
static void yexp__check_leaf_v2(yexp__ctx* c, yexp__scope* s) {
    const yexp_template* t = s->t;
    char elem[200];
    const yexp_control* x;
    int i, k;
    s->flags_ok = true;
    for (i = 0; i < t->n_states; i++) {
        const yexp_state* st = &t->states[i];
        snprintf(elem, sizeof elem, "template '%s': state '%s': steps", t->name, st->name ? st->name : "?");
        yexp__check_steps(c, s, st->steps, st->n_steps, YEXP__PH_TRIAL, elem);
        snprintf(elem, sizeof elem, "template '%s': state '%s'", t->name, st->name ? st->name : "?");
        for (k = 0; k < st->n_exits; k++) yexp__check_exit_vars(c, s, &st->exits[k], elem);
        if (st->over_outcome) yexp__check_value(c, s, st->over_outcome, YEXP__PH_TRIAL, YEXP__W_ANY, elem, "over.outcome");
    }
    snprintf(elem, sizeof elem, "template '%s': exits", t->name);
    for (k = 0; k < t->n_exits; k++) yexp__check_exit_vars(c, s, &t->exits[k], elem);
    s->flags_ok = false;
    i = 0;
    while ((x = yexp__graph_control(c, t, &i)) != NULL) {
        if (x->shared) snprintf(elem, sizeof elem, "control '%s'", x->name);
        else snprintf(elem, sizeof elem, "template '%s': control '%s'", t->name, x->name);
        yexp__check_control_vars(c, s, x, elem);
    }
    /* a state or a flag cannot have a variable's name (3.9): a table's
     * column counts only when the host has read it */
    for (k = 0; k < t->n_states + t->n_flags; k++) {
        const char* nm = k < t->n_states ? t->states[k].name : t->flags[k - t->n_states];
        yexp__var r;
        const char* why;
        if (!nm || yexp__is_builtin(nm)) continue;
        if (yexp__resolve(c, s, nm, &r, &why) && !(r.kind == YEXP__VK_TABLE && r.unverified))
            yexp__err(c, k < t->n_states ? t->states[k].pos_ : t->pos_, "template '%s': %s '%s' has the name of a variable that node '%s' knows; $%s would mean both (3.9)",
                      t->name, k < t->n_states ? "state" : "flag", nm, s->leaf->path, nm);
    }
}

static void yexp__check_leaf(yexp__ctx* c, yexp__scope* s) {
    const yexp_template* t = s->t;
    char elem[128];
    int i, k, u;
    yexp__rules_truth(s->leaf, s->rt);
    /* which stimuli this leaf shows, and from when */
    c->tmpl_seen[t - c->d->templates] = 1;
    /* only the stimuli this leaf names: a leaf's cost is its own template */
    for (i = 0; i < c->n_used; i++) c->stim_use[c->used[i]] = 0;
    c->n_used = 0;
    yexp__mark_stims(c, t->events, t->n_events, YEXP__PH_TRIAL);
    yexp__mark_stims(c, t->after, t->n_after, YEXP__PH_AFTER);
    for (i = 0; i < t->n_states; i++) yexp__mark_stims(c, t->states[i].steps, t->states[i].n_steps, YEXP__PH_TRIAL);
    if (t->response && t->response->from) {
        const yexp_stim* ps = yexp_stim_get(c->d, t->response->from);
        if (ps) yexp__mark_use(c, (int)(ps - c->d->stimuli), 1);
    }
    qsort(c->used, (size_t)c->n_used, sizeof(int), yexp__int_cmp);
    s->michelson = 0;
    for (u = 0; u < c->n_used; u++) {
        int stim_index = c->used[u];
        const yexp_stim* st = &c->d->stimuli[stim_index];
        const yexp_prop* p;
        c->stim_seen[stim_index] = 1;
        p = yexp_prop_get(st->props, st->n_props, "contrast");
        if (p && yexp__has_michelson(p->value)) s->michelson++;
    }
    for (i = 0; i < s->n; i++) {
        const yexp_node* n = s->chain[i];
        snprintf(elem, sizeof elem, "node '%s': vars", n->path);
        for (k = 0; k < n->n_vars; k++) {
            const yexp_value* path[YEXP__VAR_CHAIN + 1];
            yexp__check_value(c, s, n->vars[k].value, YEXP__PH_TRIAL, YEXP__W_ANY, elem, n->vars[k].key);
            path[0] = n->vars[k].value;
            yexp__check_cycle(c, s, n->vars[k].value, elem, n->vars[k].key, path, 1);
        }
        snprintf(elem, sizeof elem, "node '%s': data", n->path);
        for (k = 0; k < n->n_data; k++) yexp__check_value(c, s, n->data[k].value, YEXP__PH_TRIAL, YEXP__W_ANY, elem, n->data[k].key);
    }
    snprintf(elem, sizeof elem, "template '%s': events", t->name);
    yexp__check_steps(c, s, t->events, t->n_events, YEXP__PH_TRIAL, elem);
    snprintf(elem, sizeof elem, "template '%s': after", t->name);
    yexp__check_steps(c, s, t->after, t->n_after, YEXP__PH_AFTER, elem);
    snprintf(elem, sizeof elem, "template '%s': response", t->name);
    if (t->response) {
        const yexp_response* r = t->response;
        yexp__check_value(c, s, r->duration, YEXP__PH_TRIAL, YEXP__W_NONNEG, elem, "duration_s");
        yexp__check_value(c, s, r->min_rt, YEXP__PH_TRIAL, YEXP__W_NUM, elem, "minimum_valid_rt_s");
        if (r->key_answer) yexp__check_value(c, s, r->key_answer, YEXP__PH_TRIAL, YEXP__W_ANY, elem, "key_answer");
        if (r->trigger_code) {
            /* a response rule's code is per choice, so it is known as the
             * response arrives (G8) */
            s->in_code = true;
            yexp__check_value(c, s, r->trigger_code, YEXP__PH_TRIAL, YEXP__W_NONNEG, elem, "trigger.code");
            s->in_code = false;
        }
    }
    snprintf(elem, sizeof elem, "template '%s'", t->name);
    /* the player evaluates outcome only on a trial with a response, and
     * method_value only on a track trial with one (2.4, 2.5) */
    s->rsp_only = true;
    if (t->outcome && t->outcome->pos != UINT32_MAX) {
        yexp__check_value(c, s, t->outcome, YEXP__PH_AFTER, YEXP__W_ANY, elem, "outcome");
        yexp__check_outcome(c, s, t->outcome, elem, 1, 0);
    }
    s->track_only = true;
    if (t->method_value && s->leaf->n_tracks && t->method_value->pos != UINT32_MAX) yexp__check_value(c, s, t->method_value, YEXP__PH_AFTER, YEXP__W_NUM, elem, "method_value");
    s->rsp_only = s->track_only = false;
    snprintf(elem, sizeof elem, "template '%s': columns", t->name);
    s->flags_ok = c->version >= 2;   /* a flag is a column, known at the row */
    for (k = 0; k < t->n_columns; k++) yexp__check_value(c, s, t->columns[k].value, YEXP__PH_ROW, YEXP__W_ANY, elem, t->columns[k].key);
    s->flags_ok = false;
    if (c->version >= 2) yexp__check_leaf_v2(c, s);
    for (u = 0; u < c->n_used; u++) {
        const yexp_stim* st = &c->d->stimuli[c->used[u]];
        int need = c->stim_use[c->used[u]] == 1 ? YEXP__PH_TRIAL : YEXP__PH_AFTER;
        snprintf(elem, sizeof elem, "stimulus '%s'", st->name);
        for (k = 0; k < st->n_dyn_; k++) {
            const yexp_prop* p = &st->props[st->dyn_[k]];
            yexp__check_value(c, s, p->value, need, yexp__stim_want(c, st, p->key), elem, p->key);
            if (st->kind == YEXP_IMAGE && yexp__streq(p->key, "texture") && p->value->kind == YEXP_V_VAR)
                yexp__check_entry_levels(c, s, p->value, YEXP_ENTRY_TEXTURE, elem, p->key);
        }
    }
    if (s->leaf->n_tracks && !t->response) yexp__err(c, s->leaf->pos_, "node '%s': 'tracks' needs a template with a 'response': a method hears a trial only through it", s->leaf->path);
    else if (s->leaf->n_tracks && !t->response->key_answer) yexp__err(c, s->leaf->pos_, "node '%s': 'tracks' needs 'key_answer' in the response: a method hears whether the trial was correct", s->leaf->path);
    if (c->h && c->h->rules && s->leaf->n_rules >= 0) {
        char msg[256];
        msg[0] = 0;
        if (c->h->rules(c->h->ctx, s->leaf, msg, sizeof msg))
            yexp__err(c, s->leaf->pos_, "node '%s': rules: %s", s->leaf->path, yexp__host_msg(msg, sizeof msg));
    }
}

/* One node's table: it exists, and no column has a built-in name (2.3). */
static void yexp__check_table(yexp__ctx* c, const yexp_node* n) {
    int i;
    if (!n->table) return;
    if (!yexp__entry(c, YEXP_ENTRY_TABLE, n->table))
        yexp__err(c, n->pos_, "node '%s': 'timeline_variables' is '%s', which is not a table entry of this pack", n->path, n->table);
    if (!c->h || !c->h->column) return;
    for (i = 0;; i++) {
        yexp_column info;
        int rc;
        if (!yexp__work(c)) return;
        memset(&info, 0, sizeof info);
        rc = c->h->column(c->h->ctx, n->table, i, &info);
        if (rc <= 0) {
            if (rc < 0 && yexp__entry(c, YEXP_ENTRY_TABLE, n->table))
                yexp__err(c, n->pos_, "node '%s': 'timeline_variables' is '%s', which the host cannot read", n->path, n->table);
            return;
        }
        if (info.name && yexp__is_builtin(info.name))
            yexp__err(c, n->pos_, "node '%s': table '%s': the column '%s' has a built-in variable's name (2.3)", n->path, n->table, info.name);
    }
}

static void yexp__check_nodes(yexp__ctx* c, yexp__scope* s, const yexp_node* nodes, int n) {
    int i;
    for (i = 0; i < n; i++) {
        const yexp_node* nd = &nodes[i];
        char elem[140];
        if (!yexp__work(c)) return;
        snprintf(elem, sizeof elem, "node '%s'", nd->path);
        /* if/unless: read when the node starts, so the node's own rows and
         * draws are not known yet */
        s->leaf = NULL;
        s->t = NULL;
        if (nd->if_var) yexp__check_var(c, s, nd->if_var, nd->if_pos_, YEXP__PH_NODE, YEXP__W_BOOL, elem, "if");
        if (nd->unless_var) yexp__check_var(c, s, nd->unless_var, nd->unless_pos_, YEXP__PH_NODE, YEXP__W_BOOL, elem, "unless");
        yexp__check_table(c, nd);
        if (s->n > YEXP__MAX_DEPTH + 1) return;
        s->chain[s->n++] = nd;
        if (nd->trial) {
            s->leaf = nd;
            s->t = nd->trial;
            yexp__check_leaf(c, s);
        } else {
            yexp__check_nodes(c, s, nd->children, nd->n_children);
        }
        s->n--;
    }
}

/* A method is opened at session start, before any node: only a session
 * field can be known then. */
static void yexp__check_methods(yexp__ctx* c, yexp__scope* s) {
    int i, k;
    for (i = 0; i < c->d->n_methods; i++) {
        const yexp_method* m = &c->d->methods[i];
        char elem[96];
        snprintf(elem, sizeof elem, "method '%s'", m->name);
        for (k = 0; k < m->n_props; k++)
            if (m->props[k].given) yexp__check_value(c, s, m->props[k].value, YEXP__PH_SESSION, YEXP__W_NUM, elem, m->props[k].key);
    }
}

static void yexp__check_gamuts(yexp__ctx* c) {
    yexp__gamut* g;
    for (g = c->gamuts; g; g = g->next) {
        const char* dot = strchr(g->v->str, '.');
        char name[80];
        const yexp_stim* st;
        const yexp_prop* p;
        if (!dot || (size_t)(dot - g->v->str) >= sizeof name) { yexp__err(c, g->v->pos, "%s: '%s': the gamut's stimulus is too long", g->elem, g->key); continue; }
        memcpy(name, g->v->str, (size_t)(dot - g->v->str));
        name[dot - g->v->str] = 0;
        st = yexp_stim_get(c->d, name);
        if (!st) { yexp__err(c, g->v->pos, "%s: '%s': no stimulus named '%s'", g->elem, g->key, name); continue; }
        p = yexp_prop_get(st->props, st->n_props, dot + 1);
        if (!p) { yexp__err(c, g->v->pos, "%s: '%s': '%s' has no property '%s'", g->elem, g->key, name, dot + 1); continue; }
        {
            int ri = yexp__find(c, YEXP__S_STIM, 1u << (int)st->kind, dot + 1);
            if (ri < 0 || c->rows[ri].type != YEXP__T_NUM)
                yexp__err(c, g->v->pos, "%s: '%s': a gamut is the largest value of a number; '%s' of '%s' is not one", g->elem, g->key, dot + 1, name);
        }
    }
}

static void yexp__check_host_globals(yexp__ctx* c) {
    yexp_def* d = c->d;
    int i, k;
    char msg[256];
    static const struct { const char* key; int kind; } RES[] = {
        { "font", YEXP_ENTRY_FONT }, { "artwork", YEXP_ENTRY_ARTWORK }, { "audio", YEXP_ENTRY_AUDIO },
        { "video", YEXP_ENTRY_VIDEO }, { "texture", YEXP_ENTRY_TEXTURE }
    };
    if (c->h && c->h->role)
        for (i = 0; i < d->n_roles; i++) {
            msg[0] = 0;
            if (c->h->role(c->h->ctx, &d->roles[i], msg, sizeof msg))
                yexp__err(c, d->roles[i].pos_, "role '%s': %s", d->roles[i].name, yexp__host_msg(msg, sizeof msg));
        }
    if (c->h && c->h->sigrole)
        for (i = 0; i < d->n_sigroles; i++) {
            msg[0] = 0;
            if (c->h->sigrole(c->h->ctx, &d->sigroles[i], msg, sizeof msg))
                yexp__err(c, d->sigroles[i].pos_, "signal role '%s': %s", d->sigroles[i].name, yexp__host_msg(msg, sizeof msg));
        }
    for (i = 0; i < d->n_stimuli; i++) {
        const yexp_stim* st = &d->stimuli[i];
        char elem[96];
        snprintf(elem, sizeof elem, "stimulus '%s'", st->name);
        for (k = 0; k < 5; k++) {
            const yexp_prop* p = yexp_prop_get(st->props, st->n_props, RES[k].key);
            if (p && p->value->kind == YEXP_V_STRING && c->h && c->h->entry && !c->h->entry(c->h->ctx, RES[k].kind, p->value->str))
                yexp__err(c, p->value->pos, "%s: '%s': no pack entry named '%s'", elem, RES[k].key, p->value->str);
        }
        if (c->h && c->h->param)
            for (k = 0; k < st->n_props; k++) {
                if (!st->props[k].given) continue;
                msg[0] = 0;
                if (c->h->param(c->h->ctx, YEXP_PARAM_STIMULUS, yexp__kinds[(int)st->kind], &st->props[k], msg, sizeof msg))
                    yexp__err(c, st->props[k].value->pos, "%s: '%s': %s", elem, st->props[k].key, yexp__host_msg(msg, sizeof msg));
            }
    }
    if (c->h && c->h->param)
        for (i = 0; i < d->n_methods; i++) {
            const yexp_method* m = &d->methods[i];
            static const char* const types[] = { "stair", "quest", "aep" };
            for (k = 0; k < m->n_props; k++) {
                if (!m->props[k].given) continue;
                msg[0] = 0;
                if (c->h->param(c->h->ctx, YEXP_PARAM_METHOD, types[(int)m->type], &m->props[k], msg, sizeof msg))
                    yexp__err(c, m->props[k].value->pos, "method '%s': '%s': %s", m->name, m->props[k].key, yexp__host_msg(msg, sizeof msg));
            }
        }
    if (c->h && c->h->check_ops)
        for (i = 0; i < d->n_templates; i++) {
            msg[0] = 0;
            if (c->h->check_ops(c->h->ctx, d, &d->templates[i], msg, sizeof msg))
                yexp__err(c, d->templates[i].pos_, "template '%s': %s", d->templates[i].name, yexp__host_msg(msg, sizeof msg));
        }
}

/* What no leaf reaches has no scope to check against: it is said, not
 * silently passed, and it is no refusal. */
static void yexp__warn_unused(yexp__ctx* c) {
    int i;
    for (i = 0; i < c->d->n_templates; i++)
        if (!c->tmpl_seen[i])
            yexp__warn(c, c->d->templates[i].pos_, "template '%s': no node runs it, so its variables were not checked", c->d->templates[i].name);
    for (i = 0; i < c->d->n_stimuli; i++)
        if (!c->stim_seen[i])
            yexp__warn(c, c->d->stimuli[i].pos_, "stimulus '%s': no step that a node runs shows it, so its variables were not checked", c->d->stimuli[i].name);
}

static bool yexp__has_leaf(const yexp_node* n) {
    int i;
    if (n->trial) return true;
    for (i = 0; i < n->n_children; i++)
        if (yexp__has_leaf(&n->children[i])) return true;
    return false;
}

static void yexp__warn_empty(yexp__ctx* c, const yexp_node* nodes, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (nodes[i].trial) continue;
        if (!yexp__has_leaf(&nodes[i]))
            yexp__warn(c, nodes[i].pos_, "node '%s': no leaf under it, so its variables were not checked", nodes[i].path);
        else
            yexp__warn_empty(c, nodes[i].children, nodes[i].n_children);
    }
}

static void yexp__warn_unreached(yexp__ctx* c) {
    yexp__lookup* l;
    for (l = c->lookups; l; l = l->next) {
        const yexp_value* v = l->v;
        uint32_t k;
        int nx = 0;
        const char** keys;
        if (v->reached_[v->n] != 1u) continue;   /* not reached, or some level not known */
        keys = (const char**)yexp__alloc(c, v->n * sizeof(char*));
        if (!keys) return;
        for (k = 0; k < v->n; k++)
            if (!v->reached_[k]) keys[nx++] = v->map[k].key;
        if (nx) {
            char list[400], by[80];
            yexp__quote_list(list, sizeof list, keys, nx);
            yexp__by_text(v->by, by, sizeof by);
            yexp__warn(c, v->pos, "%s: '%s': no level of '%s' reaches the map's key%s %s; a typo, or a level the trials no longer have?", l->elem, l->key, by, nx > 1 ? "s" : "", list);
        }
    }
}

/* A method_value means nothing without a track to tell. Whether any leaf
 * with tracks runs a template is known only once the tree is read. */
static void yexp__finish_method_values(yexp__ctx* c) {
    int i;
    for (i = 0; i < c->d->n_templates; i++) {
        yexp_template* t = (yexp_template*)(void*)&c->d->templates[i];
        if (t->has_tracks_) {
            if (t->mv_canon_) yexp__put(c, (yjs_value*)t->canon_, "method_value", (yjs_value*)t->mv_canon_);
        } else {
            if (t->mv_written_) yexp__warn(c, t->mv_pos_, "template '%s': 'method_value' is ignored: no leaf with tracks runs this template", t->name);
            t->method_value = NULL;
        }
    }
}

/* --- the root ----------------------------------------------------------- */

/* What two templates share (version 2): a top-level movement some response
 * names (finding F11), and a trace's columns (3.15). */
static bool yexp__trace_same(const yexp_control* a, const yexp_control* b) {
    const yexp_prop* ra = yexp_prop_get(a->props, a->n_props, "rate");
    const yexp_prop* rb = yexp_prop_get(b->props, b->n_props, "rate");
    int q;
    if (!ra || !rb || !ra->value->str || !rb->value->str || !yexp__streq(ra->value->str, rb->value->str) || a->n_items != b->n_items) return false;
    for (q = 0; q < a->n_items; q++) {
        const yexp_ref* x = &a->items[q];
        const yexp_ref* y = &b->items[q];
        if (!x->name || !y->name || !yexp__streq(x->name, y->name) || x->lane != y->lane || x->lanes != y->lanes) return false;
    }
    return true;
}

static void yexp__root_end(yexp__ctx* c) {
    const yexp_def* d = c->d;
    yexp__keyed* k;
    int i, j, n = 0;
    for (i = 0; i < d->n_controls; i++)
        if (d->controls[i].kind == YEXP_MOVEMENT && !d->controls[i].failed_ && c->mv_named && !c->mv_named[i])
            yexp__err(c, d->controls[i].pos_, "control '%s': a movement is a response's choice, and no template's response names it (3.7, 3.13)", d->controls[i].name);
    for (i = 0; i < d->n_templates; i++)
        for (j = 0; j < d->templates[i].n_controls; j++)
            if (d->templates[i].controls[j].kind == YEXP_TRACE) n++;
    if (n < 2) return;
    k = (yexp__keyed*)yexp__alloc(c, (size_t)n * sizeof *k);
    if (!k) return;
    n = 0;
    for (i = 0; i < d->n_templates; i++)
        for (j = 0; j < d->templates[i].n_controls; j++)
            if (d->templates[i].controls[j].kind == YEXP_TRACE) {
                k[n].key = d->templates[i].controls[j].name;
                k[n].index = i * YEXP__MAX_SET + j;
                n++;
            }
    qsort(k, (size_t)n, sizeof *k, yexp__keyed_cmp);
    for (i = 1; i < n; i++) {
        const yexp_control* a;
        const yexp_control* b;
        if (!yexp__streq(k[i].key, k[i - 1].key)) continue;
        a = &d->templates[k[i - 1].index / YEXP__MAX_SET].controls[k[i - 1].index % YEXP__MAX_SET];
        b = &d->templates[k[i].index / YEXP__MAX_SET].controls[k[i].index % YEXP__MAX_SET];
        if (a->failed_ || b->failed_) continue;
        if (!yexp__trace_same(a, b))
            yexp__err(c, b->pos_, "template '%s': control '%s': template '%s' has a trace of this name with other columns; one stream, one set of columns (3.15)",
                      d->templates[k[i].index / YEXP__MAX_SET].name, b->name, d->templates[k[i - 1].index / YEXP__MAX_SET].name);
    }
}

static void yexp__parse_root(yexp__ctx* c, const yjs_value* root) {
    yexp_def* d = c->d;
    const yjs_value* fmt;
    const yjs_value* ver;
    int64_t version = 0;
    yexp__slot* s;
    yexp__slot* x;
    yjs_value* croot = NULL;
    yjs_value* cc;
    if (root->type != YJS_OBJECT) {
        yexp__err(c, root->pos, "a definition is a JSON object");
        return;
    }
    fmt = yjs_get(root, "format");
    ver = yjs_get(root, "version");
    if (!fmt || fmt->type != YJS_STRING || !yexp__streq(fmt->u.s, "ysp-experiment")) {
        yexp__err(c, fmt ? fmt->pos : root->pos, "'format': this file is not a ysp experiment definition (\"format\": \"ysp-experiment\")");
        return;
    }
    if (!ver) {
        yexp__err(c, root->pos, "'version' is required");
        return;
    }
    if (yjs_int64(ver, &version) != YJS_OK) {
        yexp__err(c, ver->pos, "'version' is an integer");
        return;
    }
    if (version > YEXP_FORMAT_VERSION) {
        yexp__err(c, ver->pos, "this file is format version %lld; this player reads up to %d (D12)", (long long)version, YEXP_FORMAT_VERSION);
        return;
    }
    if (version < 1) {
        yexp__err(c, ver->pos, "'version': %lld is not a format version", (long long)version);
        return;
    }
    d->format_version = (int)version;
    yexp__use_rows(c, (int)version);
    s = yexp__object(c, YEXP__S_ROOT, 0xffffu, root, "the file", &croot);
    if (!s) return;
    {
        const char* nm = yexp__v_str(yexp__slotv(c, s, YEXP__S_ROOT, "name"));
        d->name = nm ? nm : "";
    }
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "session");
    yexp__parse_session(c, x->src, &cc);
    if (cc && !yexp__at_default(c, (int)(yexp__slot_at(c, s, YEXP__S_ROOT, "session") - s) + c->first[YEXP__S_ROOT], cc)) yexp__put(c, croot, "session", cc);
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "display");
    yexp__parse_display(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "display", cc);
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "web");
    yexp__parse_web(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "web", cc);
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "devices");
    yexp__parse_devices(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "devices", cc);
    if (c->version >= 2) {
        cc = NULL;
        x = yexp__slot_at(c, s, YEXP__S_ROOT, "roles");
        yexp__parse_sigroles(c, x->src, &cc);
        if (cc && cc->n) yexp__put(c, croot, "roles", cc);
    }
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "stimuli");
    yexp__parse_stimuli(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "stimuli", cc);
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "methods");
    yexp__parse_methods(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "methods", cc);
    if (c->version >= 2) {
        x = yexp__slot_at(c, s, YEXP__S_ROOT, "controls");
        if (x->src) {
            cc = NULL;
            yexp__parse_controls(c, x->src, true, "", &d->controls, &d->n_controls, &cc);
            if (cc && cc->n) yexp__put(c, croot, "controls", cc);
        }
        yexp__global_names(c);
        yexp__resolve_controls(c, d->controls, d->n_controls, "");
        yexp__ctl_cycles(c, NULL, 0, "");
        c->mv_named = (uint8_t*)yexp__alloc(c, (size_t)d->n_controls + 1);
        if (c->mv_named) memset(c->mv_named, 0, (size_t)d->n_controls + 1);
    }
    /* the duplicate checks below are linear: one stamp per name */
    c->stim_stamp = (uint32_t*)yexp__alloc(c, ((size_t)d->n_stimuli + 1) * sizeof(uint32_t));
    c->method_stamp = (uint32_t*)yexp__alloc(c, ((size_t)d->n_methods + 1) * sizeof(uint32_t));
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "trials");
    if (x->src) {
        cc = NULL;
        yexp__parse_trials(c, x->src, &cc);
        yexp__put(c, croot, "trials", cc);
    } else {
        c->bad_sets |= YEXP__BAD_TRIALS;
    }
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "timeline");
    if (x->src) {
        cc = NULL;
        yexp__parse_children(c, x->src, NULL, "", 0, &d->timeline, &d->n_timeline, &cc);
        yexp__put(c, croot, "timeline", cc);
    }
    if (c->version >= 2) yexp__root_end(c);
    yexp__release(c, YEXP__S_ROOT);
    d->canon_ = croot;
}

/* --- SHA-256 (FIPS 180-4), for the experiment ID ------------------------ */

typedef struct yexp__sha {
    uint32_t h[8];
    uint8_t  buf[64];
    uint64_t total;
    size_t   n;
} yexp__sha;

#define YEXP__ROR(x, r) (((x) >> (r)) | ((x) << (32 - (r))))

static void yexp__sha_block(yexp__sha* s, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = YEXP__ROR(w[i - 15], 7) ^ YEXP__ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = YEXP__ROR(w[i - 2], 17) ^ YEXP__ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; cc = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = YEXP__ROR(e, 6) ^ YEXP__ROR(e, 11) ^ YEXP__ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = YEXP__ROR(a, 2) ^ YEXP__ROR(a, 13) ^ YEXP__ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += cc; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void yexp__sha_init(yexp__sha* s) {
    static const uint32_t IV[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memset(s, 0, sizeof *s);
    memcpy(s->h, IV, sizeof IV);
}

static void yexp__sha_update(yexp__sha* s, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    s->total += (uint64_t)n;
    while (n) {
        size_t take = 64 - s->n;
        if (take > n) take = n;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
        if (s->n == 64) { yexp__sha_block(s, s->buf); s->n = 0; }
    }
}

static void yexp__sha_final(yexp__sha* s, uint8_t out[32]) {
    uint64_t bits = s->total * 8u;
    static const uint8_t pad[64] = { 0x80 };
    uint8_t len[8];
    int i;
    yexp__sha_update(s, pad, 1 + ((119 - s->n) % 64));
    for (i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    yexp__sha_update(s, len, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

YEXP_API int yexp_canonical(const yexp_def* d, yjs_arena* a, const char** bytes, size_t* n, uint8_t id[32]) {
    size_t need;
    char* out;
    if (!d || !a || !bytes || !n || !d->canon_) return YEXP_ERR_ARG;
    need = yjs_write_mem(d->canon_, YJS_WRITE_PRETTY, NULL, 0);
    out = (char*)yjs_alloc(a, need + 2);
    if (!out) return YEXP_ERR_NOMEM;
    if (yjs_write_mem(d->canon_, YJS_WRITE_PRETTY, out, need + 1) != need) return YEXP_ERR_ARG;
    out[need] = '\n';
    out[need + 1] = 0;
    *bytes = out;
    *n = need + 1;
    if (id) {
        yexp__sha sh;
        yexp__sha_init(&sh);
        yexp__sha_update(&sh, out, need + 1);
        yexp__sha_final(&sh, id);
    }
    return YEXP_OK;
}

/* Format 3.2's migration step, on the canonical tree: "version" 2 and no
 * stimulus `visible`. The tree is shared, not copied: only the root and the
 * stimuli are new objects. */
YEXP_API int yexp_migrated(const yexp_def* d, yjs_arena* a, const char** bytes, size_t* n, uint8_t id[32]) {
    yexp_def tmp;
    yjs_value* root;
    size_t i, k;
    if (!d || !a || !bytes || !n || !d->canon_) return YEXP_ERR_ARG;
    if (d->format_version >= 2) return yexp_canonical(d, a, bytes, n, id);
    root = yjs_new(a, YJS_OBJECT);
    if (!root) return YEXP_ERR_NOMEM;
    for (i = 0; i < d->canon_->n; i++) {
        const yjs_member* m = &d->canon_->u.members[i];
        yjs_value* v = m->value;
        if (yexp__streq(m->key, "version")) {
            v = yjs_new_int(a, 2);
        } else if (yexp__streq(m->key, "stimuli") && v->type == YJS_OBJECT) {
            yjs_value* set = yjs_new(a, YJS_OBJECT);
            if (!set) return YEXP_ERR_NOMEM;
            for (k = 0; k < v->n; k++) {
                const yjs_member* sm = &v->u.members[k];
                yjs_value* st = sm->value;
                if (st->type == YJS_OBJECT && yjs_get(st, "visible")) {
                    yjs_value* copy = yjs_new(a, YJS_OBJECT);
                    size_t q;
                    if (!copy) return YEXP_ERR_NOMEM;
                    for (q = 0; q < st->n; q++)
                        if (!yexp__streq(st->u.members[q].key, "visible") && yjs_set(a, copy, st->u.members[q].key, st->u.members[q].value) != YJS_OK) return YEXP_ERR_NOMEM;
                    st = copy;
                }
                if (yjs_set(a, set, sm->key, st) != YJS_OK) return YEXP_ERR_NOMEM;
            }
            v = set;
        }
        if (!v || yjs_set(a, root, m->key, v) != YJS_OK) return YEXP_ERR_NOMEM;
    }
    tmp = *d;
    tmp.canon_ = root;
    return yexp_canonical(&tmp, a, bytes, n, id);
}

/* --- the entry point ---------------------------------------------------- */

static void yexp__check_mark(yexp_report* r, unsigned bit, bool present) {
    if (present) r->run |= bit;
    else r->skipped |= bit;
}

YEXP_API int yexp_load(yjs_arena* a, const char* text, size_t n, const yexp_host* host, yexp_def** out, yexp_report* report) {
    yexp__ctx* c;
    yexp_def* d;
    yjs_value* root = NULL;
    yjs_error je;
    if (out) *out = NULL;
    if (report) memset(report, 0, sizeof *report);
    if (!a || !text || !out || !report) return YEXP_ERR_ARG;
    /* one struct, from the arena: it is too large for a small stack */
    c = (yexp__ctx*)yjs_alloc(a, sizeof *c);
    d = c ? (yexp_def*)yjs_alloc(a, sizeof *d) : NULL;
    if (!c || !d) {
        report->arena_need = a->need;
        return YEXP_ERR_NOMEM;
    }
    memset(c, 0, sizeof *c);
    c->a = a;
    c->text = text;
    c->len = n;
    c->r = report;
    c->h = host;
    c->d = d;
    d->name = "";
    d->background[0] = d->background[1] = d->background[2] = 0.5;
    c->slots = (yexp__slot*)yjs_alloc(a, YEXP__SLOT_STACK * sizeof(yexp__slot));
    if (!c->slots) { report->arena_need = a->need; return YEXP_ERR_NOMEM; }
    yexp__use_rows(c, 1);
    if (yjs_parse(a, text, n, NULL, &root, &je) != YJS_OK) {
        yexp_diag* dg;
        if (je.code == YJS_ERR_NOMEM) {
            report->arena_need = a->need;
            snprintf(report->diags[0].msg, sizeof report->diags[0].msg, "out of memory while reading the JSON");
            report->n_diags = 1;
            report->n_errors = 1;
            return YEXP_ERR_NOMEM;
        }
        dg = &report->diags[0];
        dg->severity = YEXP_ERROR;
        dg->line = je.line;
        dg->column = je.column;
        snprintf(dg->msg, sizeof dg->msg, "not JSON: %s", je.msg);
        yexp__flat(dg->msg);
        report->n_diags = 1;
        report->n_errors = 1;
        return YEXP_ERR_REFUSED;
    }
    yexp__parse_root(c, root);
    if (!c->nomem && report->n_errors == 0) yexp__finish_method_values(c);
    if (!c->nomem && report->n_errors == 0) {
        yexp__scope* sc = (yexp__scope*)yexp__alloc(c, sizeof *sc);
        c->stim_use = (uint8_t*)yexp__alloc(c, (size_t)d->n_stimuli + 1);
        c->stim_seen = (uint8_t*)yexp__alloc(c, (size_t)d->n_stimuli + 1);
        c->used = (int*)yexp__alloc(c, ((size_t)d->n_stimuli + 1) * sizeof(int));
        c->tmpl_seen = (uint8_t*)yexp__alloc(c, (size_t)d->n_templates + 1);
        if (sc && c->stim_use && c->stim_seen && c->tmpl_seen && c->used) {
            yexp__check_mark(report, YEXP_CHECK_ENTRIES, host && host->entry);
            yexp__check_mark(report, YEXP_CHECK_COLUMNS, host && host->column);
            yexp__check_mark(report, YEXP_CHECK_LEVELS, host && host->level && host->column);
            yexp__check_mark(report, YEXP_CHECK_ROLES, host && host->role);
            yexp__check_mark(report, YEXP_CHECK_PARAMS, host && host->param);
            yexp__check_mark(report, YEXP_CHECK_RULES, host && host->rules);
            yexp__check_mark(report, YEXP_CHECK_OPS, host && host->check_ops);
            /* only a file with signal roles has this check to run (G18) */
            if (d->n_sigroles) yexp__check_mark(report, YEXP_CHECK_SIGROLES, host && host->sigrole);
            yexp__check_gamuts(c);
            memset(sc, 0, sizeof *sc);
            yexp__check_methods(c, sc);
            yexp__check_nodes(c, sc, d->timeline, d->n_timeline);
            yexp__check_host_globals(c);
            if (report->n_errors == 0) { yexp__warn_unreached(c); yexp__warn_unused(c); yexp__warn_empty(c, d->timeline, d->n_timeline); }
        }
    }
    if (c->nomem) {
        report->arena_need = a->need;
        if (report->n_diags < YEXP_MAX_DIAGS) {
            yexp_diag* dg = &report->diags[report->n_diags++];
            dg->severity = YEXP_ERROR;
            dg->line = dg->column = 0;
            snprintf(dg->msg, sizeof dg->msg, "out of memory: the arena needs about %lu more bytes", (unsigned long)a->need);
        }
        report->n_errors++;
        return YEXP_ERR_NOMEM;
    }
    if (report->n_errors) return YEXP_ERR_REFUSED;
    *out = d;
    return YEXP_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_EXPERIMENT_IMPLEMENTATION_GUARD */
#endif /* YSP_EXPERIMENT_IMPLEMENTATION */

/*
 * ---------------------------------------------------------------------
 * LICENSE
 * ---------------------------------------------------------------------
 * ysp/experiment.h is released under the MIT No Attribution license (MIT-0)
 * and is dedicated to the public domain where that is possible:
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
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
