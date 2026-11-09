/* ysp/experiment.h - v0.1.0 - public domain single-header loader for experiment definitions
 *
 *   Reads a version 1 ysp experiment definition (docs/experiment_format.md,
 *   the pack's EXPERIMENT entry) into a checked, fully defaulted form in a
 *   caller-given arena: session, display, devices, stimuli, methods, trial
 *   templates and the tree of nodes. Every refusal names the element and
 *   the key, with the line and column of the source. It writes the
 *   definition's canonical bytes with defaults left out and gives the
 *   experiment ID (D2).
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
 *   duration above 60 s (D4), a lookup key that no level reaches (2.2).
 *
 *   ---------------------------------------------------------------------
 *   THE HOST
 *   ---------------------------------------------------------------------
 *   yexp_host is a table of optional functions. entry() says whether a pack
 *   entry exists; column() and level() read a table's columns and levels;
 *   role() checks a role against the rig; param() checks a value against a
 *   header's parameter table; rules() runs ytr_rules() and a dry open;
 *   check_ops() runs ytl_check_ops() on a template. The pack tool, the
 *   player and the designer each supply what they have. report.run and
 *   report.skipped say which checks ran (YEXP_CHECK_*). When column() is
 *   NULL and a table is in scope, a variable that names no other thing
 *   is taken to be a column of that table.
 *
 *   ---------------------------------------------------------------------
 *   CANONICAL BYTES
 *   ---------------------------------------------------------------------
 *   yexp_canonical() writes ysp/json.h's canonical form with one LF after
 *   the value, as the pack tool does (pack.md 16), and with every key at its
 *   default left out; the SHA-256 of those bytes is the experiment ID. A
 *   duration is the exact decimal of its nanoseconds. Another real goes
 *   through its double (yjs_double(), then yjs_new_double()), so
 *   0.1 and 0.1000000000000000055 are one ID; an integer within int64 stays
 *   exact. A number that yjs_double() cannot read (more than 19 significant
 *   digits, or out of range) is refused at load. docs/experiment.md gives
 *   the reasons.
 *
 *   ---------------------------------------------------------------------
 *   BOUNDS AND MEMORY
 *   ---------------------------------------------------------------------
 *   Everything lands in the caller's arena (yjs_arena_init() for a fixed
 *   buffer, yjs_arena_init_heap() for blocks with a limit): the parsed JSON,
 *   the form, the canonical tree and the work space. About 12 times the
 *   source text for the worked examples (docs/experiment.md). A full arena
 *   is YEXP_ERR_NOMEM, with the bytes wanted in the report. Limits: 1024
 *   stimuli, methods, roles and templates, 4096 steps in one list, 256
 *   children of a node and 32 levels of nodes (the JSON depth of 64 stops a
 *   deeper file first), 5,000,000 work steps for the second pass. Not for a
 *   frame loop: a definition is read once per session.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.1.0 - first version: the loader for format version 1, its defaults
 *          table, the host interface and the canonical writer.
 *
 *   STATUS: v0.1.0 (2026-11-09). Tested; not used by a player yet.
 *   docs/experiment.md has the decisions and the test results.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0; the text is at the end of the file.
 */
#ifndef YSP_EXPERIMENT_H_INCLUDED
#define YSP_EXPERIMENT_H_INCLUDED

#define YEXP_VERSION_MAJOR 0
#define YEXP_VERSION_MINOR 1
#define YEXP_VERSION_PATCH 0
#define YEXP_VERSION_STRING "0.1.0"

/* The newest format version this header reads (D12). */
#define YEXP_FORMAT_VERSION 1

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
    size_t    arena_need;             /* after YEXP_ERR_NOMEM                */
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
    YEXP_V_CHOICE       /* a node's vars: items[n], drawn per trial          */
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
    YEXP_TRIGGER, YEXP_MARK, YEXP_IF, YEXP_UNLESS
} yexp_step_op;

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
};

typedef struct yexp_choice {
    const char* key;
    const char* name;             /* the key when not given                  */
} yexp_choice;

typedef struct yexp_response {
    const char*        role;
    const char*        from;
    const yexp_choice* choices;   /* NULL: none listed                       */
    int                n_choices;
    const yexp_value*  duration;       /* seconds; 0: no deadline            */
    const yexp_value*  min_rt;         /* minimum_valid_rt_s                 */
    const yexp_value*  key_answer;     /* NULL when not given                */
    bool               ends_trial;
    bool               persist;
    int                max_responses;
    bool               allow_held_key;
    bool               wait_for_key_release;
    const char*        trigger_role;   /* NULL: no trigger                   */
    const yexp_value*  trigger_code;
} yexp_response;

typedef struct yexp_template {
    const char*          name;
    const yexp_step*     events;   int n_events;
    const yexp_step*     after;    int n_after;
    const yexp_response* response;  /* NULL: none                            */
    const char* const*   record;   int n_record;  /* given, or the default   */
    bool                 record_given;
    const yexp_kv*       columns;  int n_columns;
    const yexp_value*    outcome;
    const yexp_value*    method_value;
    uint32_t             pos_;
} yexp_template;

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
    const char* const*   rule_vars;   int n_rule_vars;
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
    YEXP__S_RTRIGGER, YEXP__S_CHOICE, YEXP__S_NODE, YEXP__S_LOOP, YEXP__S_STEP, YEXP__S_COUNT
};

/* Value types of a row. */
enum {
    YEXP__T_NUM, YEXP__T_INT, YEXP__T_DUR, YEXP__T_BOOL, YEXP__T_STR, YEXP__T_ENUM, YEXP__T_ENUMS,
    YEXP__T_TEXT, YEXP__T_RES, YEXP__T_VEC3, YEXP__T_NUMS, YEXP__T_GRID, YEXP__T_VAL, YEXP__T_CODE,
    YEXP__T_VAR, YEXP__T_NAME, YEXP__T_NAMEZ, YEXP__T_NAMES, YEXP__T_STRS, YEXP__T_TARGETS,
    YEXP__T_SMAP, YEXP__T_ANY, YEXP__T_OBJ, YEXP__T_SET, YEXP__T_LIST
};

/* Flags of a row. */
#define YEXP__R_REQ  0x01u   /* required                                     */
#define YEXP__R_LIT  0x02u   /* a literal, never a variable or lookup        */
#define YEXP__R_NONE 0x04u   /* optional, with no default                    */
#define YEXP__R_SELF 0x08u   /* the section's builder writes the canonical key */

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
    { YEXP__S_SESSION, 0xffff, YEXP__T_SET, YEXP__R_REQ, "fields", NULL, NULL },
    { YEXP__S_SFIELD, 0xffff, YEXP__T_BOOL, YEXP__R_REQ, "required", NULL, NULL },
    { YEXP__S_SFIELD, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "integer=0,string=1" },
    { YEXP__S_WEB, 0xffff, YEXP__T_BOOL, 0, "allowed", "false", NULL },
    { YEXP__S_WEB, 0xffff, YEXP__T_ANY, YEXP__R_NONE, "exclusion", NULL, NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_SMAP, 0, "buttons", "{}", NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "direction", NULL, "in=0,out=1,both=2" },
    { YEXP__S_ROLE, 0xffff, YEXP__T_STRS, YEXP__R_REQ, "family", NULL, NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_STR, 0, "kind", "\"\"", NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_SMAP, 0, "map", "{}", NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_INT, YEXP__R_LIT, "min_tier", "0", NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_DUR, 0, "pulse_s", "0", NULL },
    { YEXP__S_ROLE, 0xffff, YEXP__T_BOOL, 0, "required", "true", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "add", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "ahead", "6", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "algorithm", "\"custom\"", "custom=0,wn=1,mn=2,ll=3,bm=4" },
    { YEXP__S_STIM, 0x0060, YEXP__T_INT, 0, "aliased", "0", NULL },
    { YEXP__S_STIM, 0x0020, YEXP__T_ENUM, YEXP__R_LIT, "align", "\"start\"", "start=0,end=1,center=2" },
    { YEXP__S_STIM, 0x02ff, YEXP__T_ENUM, 0, "anchor", "\"center\"", "center=0,top_left=1,top=2,top_right=3,left=4,right=5,bottom_left=6,bottom=7,bottom_right=8" },
    { YEXP__S_STIM, 0x000a, YEXP__T_ENUM, 0, "aperture", "\"rect\"", "rect=0,circle=1,annulus=2,line=3,polygon=4,cross=5,no_aperture=6,mask_tex=7" },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "aperture", "\"circle\"", "circle=0,rect=1" },
    { YEXP__S_STIM, 0x0040, YEXP__T_RES, YEXP__R_REQ, "artwork", NULL, NULL },
    { YEXP__S_STIM, 0x0004, YEXP__T_NUM, 0, "aspect", "1", NULL },
    { YEXP__S_STIM, 0x0100, YEXP__T_RES, YEXP__R_REQ, "audio", NULL, NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "ax", "0.5", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "ay", "0.5", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "base", "0", NULL },
    { YEXP__S_STIM, 0x0061, YEXP__T_ENUM, 0, "blend", "\"over\"", "over=0,add=1,multiply=2" },
    { YEXP__S_STIM, 0x0001, YEXP__T_ENUM, 0, "cap", "\"butt\"", "butt=0,round=1,square=2" },
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "channels", "0", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "check", "1", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "chroma", "\"sited\"", "sited=0,nearest=1" },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "clock", "\"onset\"", "onset=0,frames=1" },
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "coherence", "0", NULL },
    { YEXP__S_STIM, 0x02f1, YEXP__T_VEC3, 0, "color", "[0,0,0]", NULL },
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "contrast", "1", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_INT, 0, "count", "0", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "coverage", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "dash", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "dash_offset", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_INT, 0, "dash_snap", "0", NULL },
    { YEXP__S_STIM, 0x0100, YEXP__T_NUM, 0, "db", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "decode_threads", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "density", "0", NULL },
    { YEXP__S_STIM, 0x0020, YEXP__T_ENUM, YEXP__R_LIT, "dir", "\"auto\"", "auto=0,ltr=1,rtl=2" },
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "dir_b", "0", NULL },
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "dir_g", "0", NULL },
    { YEXP__S_STIM, 0x021e, YEXP__T_NUM, 0, "dir_r", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "direction", "0", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_ENUM, 0, "dist", "\"uniform\"", "uniform=0,binary=1,gaussian=2,simplex=3" },
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "dot_size", "0", NULL },
    { YEXP__S_STIM, 0x000b, YEXP__T_ENUM, 0, "edge", "\"hard\"", "hard=0,cosine=1,gaussian=2" },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "edge", "\"wrap\"", "wrap=0,replot=1" },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "edge_width", "0", NULL },
    { YEXP__S_STIM, 0x0020, YEXP__T_STR, YEXP__R_REQ | YEXP__R_LIT, "font", NULL, NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_DUR, 0, "frame_s", "0", NULL },
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "frames", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "gap", "0", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "gate", "1", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "gpu_path", "\"auto\"", "auto=0,upload=1,shared=2,gpu=3" },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "h", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "hw_decode", "\"auto\"", "auto=0,off=1,dxva=2,videotoolbox=3,vaapi=4" },
    { YEXP__S_STIM, 0x0001, YEXP__T_ENUM, 0, "join", "\"round\"", "round=0,miter=1,bevel=2" },
    { YEXP__S_STIM, 0x03ff, YEXP__T_ENUM, YEXP__R_REQ, "kind", NULL, "shape=0,grating=1,gabor=2,noise=3,image=4,text=5,artwork=6,rdk=7,sound=8,video=9" },
    { YEXP__S_STIM, 0x0020, YEXP__T_STR, YEXP__R_LIT, "lang", "\"\"", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_INT, 0, "layer", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_NUM, 0, "lead", "0.5", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_INT, 0, "lifetime_frames", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_DUR, 0, "lifetime_s", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_ENUM, 0, "light", "\"auto\"", "auto=0,codes=1,eotf=2" },
    { YEXP__S_STIM, 0x0020, YEXP__T_NUM, YEXP__R_LIT, "line_height", "0", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "linear", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_BOOL, 0, "loop", "false", NULL },
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "loops", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "min_tier", "0", NULL },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "miter_limit", "4", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "modulation", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "movie_index", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "noise", "\"direction\"", "direction=0,position=1,walk=2" },
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_gain", "0.5", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_lacunarity", "2", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_scale", "0", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_NUM, 0, "noise_z", "0", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_INT, 0, "octaves", "1", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "offset", "0", NULL },
    { YEXP__S_STIM, 0x0100, YEXP__T_INT, 0, "offset", "0", NULL },
    { YEXP__S_STIM, 0x02f1, YEXP__T_NUM, 0, "opacity", "1", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "ori", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p0", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p1", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p10", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p11", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p12", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p13", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p14", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p15", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p16", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p17", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p18", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p19", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p2", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p20", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p21", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p22", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p23", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p24", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p25", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p26", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p27", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p28", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p29", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p3", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p30", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p31", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p4", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p5", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p6", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p7", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p8", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "p9", "0", NULL },
    { YEXP__S_STIM, 0x0006, YEXP__T_NUM, 0, "phase", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "pin_cpu", "0", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_ENUM, 0, "place", "\"center\"", "center=0,top_left=1,top=2,top_right=3,left=4,right=5,bottom_left=6,bottom=7,bottom_right=8" },
    { YEXP__S_STIM, 0x0210, YEXP__T_INT, 0, "premultiplied", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_INT, 0, "preroll", "6", NULL },
    { YEXP__S_STIM, 0x0060, YEXP__T_INT, 0, "rays", "0", NULL },
    { YEXP__S_STIM, 0x0008, YEXP__T_INT, 0, "seed", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "select", "\"exact\"", "exact=0,bernoulli=1" },
    { YEXP__S_STIM, 0x0080, YEXP__T_INT, 0, "sets", "1", NULL },
    { YEXP__S_STIM, 0x0006, YEXP__T_NUM, 0, "sf", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_ENUM, 0, "shape", "\"rect\"", "rect=0,circle=1,annulus=2,line=3,polygon=4,cross=5,no_aperture=6,mask_tex=7,rrect=8,arc=9,pie=10,capsule=11,ngon=12,star=13,ellipse=14,qbezier=15,polyline=16,compound=17" },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p0", "0", NULL },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p1", "0", NULL },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p2", "0", NULL },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "shape_p3", "0", NULL },
    { YEXP__S_STIM, 0x0004, YEXP__T_NUM, 0, "sigma", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_ENUM, 0, "signal", "\"same\"", "same=0,different=1,least_recent=2" },
    { YEXP__S_STIM, 0x0020, YEXP__T_NUM, YEXP__R_REQ | YEXP__R_LIT, "size", NULL, NULL },
    { YEXP__S_STIM, 0x0040, YEXP__T_NUM, 0, "size", "0", NULL },
    { YEXP__S_STIM, 0x0080, YEXP__T_NUM, 0, "speed", "0", NULL },
    { YEXP__S_STIM, 0x0002, YEXP__T_INT, 0, "square", "0", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_h", "0", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_w", "0", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_x", "0", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "src_y", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_BOOL, 0, "strict_cadence", "false", NULL },
    { YEXP__S_STIM, 0x008b, YEXP__T_NUM, 0, "stroke", "0", NULL },
    { YEXP__S_STIM, 0x008b, YEXP__T_ENUM, 0, "stroke_align", "\"center\"", "center=0,inside=1,outside=2" },
    { YEXP__S_STIM, 0x0020, YEXP__T_TEXT, YEXP__R_REQ, "text", NULL, NULL },
    { YEXP__S_STIM, 0x0010, YEXP__T_RES, YEXP__R_REQ, "texture", NULL, NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_a", "1", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_b", "1", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_g", "1", NULL },
    { YEXP__S_STIM, 0x0210, YEXP__T_NUM, 0, "tint_r", "1", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "trim_end", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_INT, 0, "trim_set", "0", NULL },
    { YEXP__S_STIM, 0x0001, YEXP__T_NUM, 0, "trim_start", "0", NULL },
    { YEXP__S_STIM, 0x0200, YEXP__T_RES, YEXP__R_REQ, "video", NULL, NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "visible", "1", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "w", "0", NULL },
    { YEXP__S_STIM, 0x0020, YEXP__T_NUM, YEXP__R_LIT, "width", "0", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "x", "0", NULL },
    { YEXP__S_STIM, 0x02ff, YEXP__T_NUM, 0, "y", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "est_reversals", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "est_trials", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "estimate", "\"reversals\"", "reversals=0,trials=1" },
    { YEXP__S_STAIR, 0xffff, YEXP__T_BOOL, 0, "harder_is_up", "false", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_BOOL, 0, "initial_rule", "false", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "max", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "min", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "n_down", "1", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "n_up", "1", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "rule", "\"updown\"", "updown=0,asa=1" },
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "scale", "\"linear\"", "linear=0,log10=1" },
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "start", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "step_down_scale", "1", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, 0, "step_type", "\"lin\"", "lin=0,log=1,db=2" },
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "steps", NULL, NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "stop_at_limit", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "stop_reversals", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_INT, 0, "stop_trials", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_NUM, 0, "target_p", "0", NULL },
    { YEXP__S_STAIR, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "stair=0,quest=1,aep=2" },
    { YEXP__S_STAIR, 0xffff, YEXP__T_BOOL, 0, "use_limits", "false", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "estimate", "\"mean\"", "mean=0,mode=1" },
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUMS, 0, "joint_prior", "[]", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "n_outcomes", "2", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_BOOL, 0, "no_table", "false", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_GRID, YEXP__R_REQ, "param", NULL, NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "pf", "\"gumbel\"", "gumbel=0,weibull=1,logistic=2,normal=3,hypsec=4,custom=5" },
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "scale", "\"linear\"", "linear=0,log10=1" },
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "select", "\"entropy\"", "entropy=0,quantile=1,mean=2,mode=3" },
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "select_param", "0", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "select_quantile", "0.5", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_GRID, YEXP__R_REQ, "stim", NULL, NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "stop_entropy", "0", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "stop_sd", "0", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "stop_sd_param", "0", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "stop_trials", "0", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_INT, 0, "subset_size", "0", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_NUM, 0, "tie_tolerance", "1e-09", NULL },
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, 0, "tiebreak", "\"lowest\"", "lowest=0,nearest=1,alternate=2,random=3" },
    { YEXP__S_QUEST, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "stair=0,quest=1,aep=2" },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "acq", "\"lse\"", "lse=0,eavc=1,localmi=2,balv=3,bald=4,random=5,ucb=6,ei=7,thompson=8" },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "acq_beta", "1.96", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "candidates", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUMS, 0, "dim_kind", "[]", "continuous=0,integer=1,categorical=2" },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "dim_levels", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NAMES, YEXP__R_NONE, "dims", NULL, NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "estimate", "\"mean\"", "mean=0,mode=1" },
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "fit", "false", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "fit_every", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "fit_max_evals", "40", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "fit_pcg", "false", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "fit_tol", "1e-08", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "grid", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "guess", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "hi", NULL, NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper.cutpoint", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper.lengthscale", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper.lengthscale_b", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper.lengthscale_g", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper.mean", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper.mean_g", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper.noise_sd", "0.1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper.outputscale", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper.outputscale_b", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper.outputscale_g", "0.3", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_max.cutpoint", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_max.lengthscale", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_max.lengthscale_b", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_max.lengthscale_g", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_max.mean", "5", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_max.mean_g", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_max.noise_sd", "10", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_max.outputscale", "10", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_max.outputscale_b", "10", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_max.outputscale_g", "10", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_min.cutpoint", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_min.lengthscale", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_min.lengthscale_b", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "hyper_min.lengthscale_g", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_min.mean", "-5", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_min.mean_g", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_min.noise_sd", "0.001", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_min.outputscale", "0.1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_min.outputscale_b", "0.1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "hyper_min.outputscale_g", "0.01", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "intensity_dim", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "jitter", "1e-06", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "kernel", "\"rbf\"", "rbf=0,semip=1" },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "lapse", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "lik", "\"bernoulli\"", "bernoulli=0,ordinal=1,categorical=2,gaussian=3,pairwise=4" },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "link", "\"probit\"", "probit=0,logit=1" },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, YEXP__R_REQ, "lo", NULL, NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "max_trials", "512", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "minimize", "false", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "model", "\"gp\"", "gp=0,psychometric=1" },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "monotone_dims", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "n_candidates", "512", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "n_init", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "n_outcomes", "2", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_BOOL, 0, "no_hyper_prior", "false", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "pcg_threshold", "512", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.lengthscale.center", "0.25", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.lengthscale.sd", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean.center", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean.sd", "0.7", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean_g.ceiling", "0.00390625", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean_g.center", "0.25", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.mean_g.sd", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.noise_sd.center", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.noise_sd.sd", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale.center", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale.sd", "0.7", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_b.center", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_b.sd", "0.7", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_g.center", "0.3", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "priors.outputscale_g.sd", "0.7", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "refine_steps", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "refit_every", "1", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, 0, "scale", "\"linear\"", "linear=0,log10=1" },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUMS, 0, "stop_context", "[]", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "stop_threshold_sd", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "stop_trials", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_INT, 0, "target_outcome", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "target_p", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_NUM, 0, "target_value", "0", NULL },
    { YEXP__S_AEP, 0xffff, YEXP__T_ENUM, YEXP__R_REQ, "type", NULL, "stair=0,quest=1,aep=2" },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "after", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_SET, 0, "columns", "{}", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_LIST, 0, "events", "[]", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_VAL, 0, "method_value", "\"$level\"", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_VAL, 0, "outcome", "\"correct\"", NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_NAMES, YEXP__R_NONE | YEXP__R_SELF, "record", NULL, NULL },
    { YEXP__S_TEMPLATE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "response", NULL, NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "allow_held_key", "false", NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_LIST, YEXP__R_NONE, "choices", NULL, NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_DUR, 0, "duration_s", "0", NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "from", NULL, NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_VAL, YEXP__R_NONE, "key_answer", NULL, NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_INT, YEXP__R_LIT, "max_responses", "0", NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_DUR, 0, "minimum_valid_rt_s", "0", NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "persist", "false", NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "response_ends_trial", "true", NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "role", NULL, NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "trigger", NULL, NULL },
    { YEXP__S_RESPONSE, 0xffff, YEXP__T_BOOL, 0, "wait_for_key_release", "false", NULL },
    { YEXP__S_RTRIGGER, 0xffff, YEXP__T_CODE, YEXP__R_REQ, "code", NULL, NULL },
    { YEXP__S_RTRIGGER, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "role", NULL, NULL },
    { YEXP__S_CHOICE, 0xffff, YEXP__T_STR, YEXP__R_REQ, "key", NULL, NULL },
    { YEXP__S_CHOICE, 0xffff, YEXP__T_STR, YEXP__R_NONE, "name", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_SET, 0, "data", "{}", NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_VAR, YEXP__R_NONE, "if", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_ENUM, 0, "interleave", "\"random\"", "random=0,round_robin=1" },
    { YEXP__S_NODE, 0xffff, YEXP__T_OBJ, YEXP__R_NONE, "loop", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_NAME, YEXP__R_REQ, "name", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_BOOL, 0, "record_data", "true", NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_INT, 0, "repetitions", "1", NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_ENUM, YEXP__R_NONE, "requeue", NULL, "no_response=0" },
    { YEXP__S_NODE, 0xffff, YEXP__T_STRS, 0, "rules", "[]", NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_LIST, YEXP__R_NONE, "timeline", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_STR, YEXP__R_NONE, "timeline_variables", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_NUM, 0, "track_rate", "0", NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_NAMES, 0, "tracks", "[]", NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_NAME, YEXP__R_NONE, "trial", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_VAR, YEXP__R_NONE, "unless", NULL, NULL },
    { YEXP__S_NODE, 0xffff, YEXP__T_SET, 0, "vars", "{}", NULL },
    { YEXP__S_LOOP, 0xffff, YEXP__T_INT, YEXP__R_REQ, "max", NULL, NULL },
    { YEXP__S_LOOP, 0xffff, YEXP__T_NUM, YEXP__R_REQ, "until_accuracy", NULL, NULL },
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
    { YEXP__S_STEP, 0x0003, YEXP__T_DUR, 0, "stagger_s", "0", NULL },
    { YEXP__S_STEP, 0x0600, YEXP__T_LIST, YEXP__R_REQ, "steps", NULL, NULL },
    { YEXP__S_STEP, 0x0040, YEXP__T_NAME, YEXP__R_REQ, "then", NULL, NULL },
    { YEXP__S_STEP, 0x0020, YEXP__T_NAME, YEXP__R_REQ, "to", NULL, NULL },
    { YEXP__S_STEP, 0x0080, YEXP__T_CODE, YEXP__R_REQ, "trigger", NULL, NULL },
    { YEXP__S_STEP, 0x0400, YEXP__T_VAR, YEXP__R_REQ, "unless", NULL, NULL },
    { YEXP__S_STEP, 0x0004, YEXP__T_DUR, YEXP__R_REQ, "wait_s", NULL, NULL },
};
#define YEXP__N_ROWS ((int)(sizeof yexp__rows / sizeof yexp__rows[0]))

static const char* const yexp__section_names[YEXP__S_COUNT] = {
    "file", "display", "session", "session field", "web", "role", "stimulus", "stair method",
    "quest method", "aep method", "template", "response", "response trigger", "choice", "node",
    "loop", "step"
};

YEXP_API const yexp_row* yexp_defaults(int version, int* n) {
    if (version != 1) { if (n) *n = 0; return NULL; }
    if (n) *n = YEXP__N_ROWS;
    return yexp__rows;
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
#define YEXP__WORK_STEPS   5000000L
#define YEXP__MAX_LEVELS   65536
#define YEXP__VAR_DEPTH    8

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
    const yexp_value*  defs[YEXP__N_ROWS];
    const yjs_value*   cdefs[YEXP__N_ROWS];
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

static void* yexp__alloc(yexp__ctx* c, size_t n) {
    void* p;
    if (c->nomem) return NULL;
    p = yjs_alloc(c->a, n ? n : 1);
    if (!p) c->nomem = true;
    return p;
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
        const yexp_row* r = &yexp__rows[f + i];
        if ((r->kinds & mask) && yexp__streq(r->key, key)) return f + i;
    }
    return -1;
}

static const yexp_value* yexp__literal_from_json(yexp__ctx* c, const yexp_row* row, const yjs_value* j);

/* A row's default as a value, parsed once per load. */
static const yexp_value* yexp__default(yexp__ctx* c, int ri) {
    const yexp_row* r = &yexp__rows[ri];
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
    const yexp_row* r = &yexp__rows[ri];
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
#define YEXP__AV_VALUE    (YEXP__AV_NUM | YEXP__AV_STR | YEXP__AV_BOOL | YEXP__AV_ARRAY | YEXP__AV_VAR | YEXP__AV_LOOKUP)

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

static void yexp__number(yexp__ctx* c, const yjs_value* src, unsigned allow, const char* elem, const char* key, yexp_value* v, yjs_value** canon) {
    double x;
    v->kind = YEXP_V_NUMBER;
    if (allow & YEXP__AV_DUR) {
        int64_t ns = 0;
        if (yjs_fixed(src, 9, &ns) != YJS_OK) {
            yexp__err(c, src->pos, "%s: '%s': a duration is a number of seconds that fits 64-bit nanoseconds; '%s' does not", elem, key, src->u.s);
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
    if (yjs_double(src, &x) != YJS_OK) {
        yexp__err(c, src->pos, "%s: '%s': the number '%s' has more than 19 significant digits or is out of range for a double", elem, key, src->u.s);
        return;
    }
    if (x == 0) x = 0;   /* -0 is 0: one spelling, one value (D2) */
    v->num = x;
    if (src->flags & YJS_NUM_I64) {
        v->flags = YEXP_VF_INT;
        v->i = src->i;
    } else if (x == floor(x) && fabs(x) < 9007199254740992.0) {
        v->flags = YEXP_VF_INT;
        v->i = (int64_t)x;
    } else if (allow & YEXP__AV_INT) {
        yexp__err(c, src->pos, "%s: '%s': '%s' is not an integer", elem, key, src->u.s);
        return;
    }
    if (canon) {
        *canon = (v->flags & YEXP_VF_INT) ? yjs_new_int(c->a, v->i) : yjs_new_double(c->a, x);
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
        const yexp_value* mv = yexp__value(c, maps->u.members[i].value, allow, iallow, elem, key, &cv);
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
    if (yjs_get(src, "lua")) {
        yexp__err(c, src->pos, "%s: '%s': {\"lua\": ...} needs a format version with scripts; version 1 refuses it (D6)", elem, key);
        return NULL;
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
            const yexp_value* e = yexp__value(c, ch->u.items[i], allow & ~(unsigned)YEXP__AV_CHOICE, iallow, elem, key, &ce);
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
    if (yexp__is_v2(key)) {
        yexp__err(c, m->key_pos, "%s: '%s' is a key of format version 2 (D16 to D20); this definition is version 1", elem, key);
        return;
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
    if (ri >= 0 && yexp__rows[ri].type == YEXP__T_DUR) {
        yexp__err(c, m->key_pos, "%s: '%s' is a duration; write the key '%s', in seconds (D4)", elem, key, yexp__rows[ri].key);
        return;
    }
    if (sect == YEXP__S_STIM && yexp__streq(key, "color_r")) {
        yexp__err(c, m->key_pos, "%s: '%s': write 'color' as [r, g, b]", elem, key);
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

/* A row's scalar value. Returns false after a diagnostic. */
static bool yexp__row_value(yexp__ctx* c, int ri, const yjs_member* m, const char* elem, yexp__slot* slot, yjs_value** canon) {
    const yexp_row* r = &yexp__rows[ri];
    const yjs_value* src = m->value;
    unsigned lit = (r->flags & YEXP__R_LIT) ? ~(unsigned)(YEXP__AV_VAR | YEXP__AV_LOOKUP | YEXP__AV_GAMUT) : ~0u;
    const yexp_value* v = NULL;
    *canon = NULL;
    switch (r->type) {
    case YEXP__T_NUM: {
        unsigned al = (YEXP__AV_NUM | YEXP__AV_VAR | YEXP__AV_LOOKUP | YEXP__AV_GAMUT | (yexp__streq(r->key, "contrast") ? YEXP__AV_MICH : 0u)) & lit;
        v = yexp__value(c, src, al, 0, elem, r->key, canon);
        break;
    }
    case YEXP__T_INT:
        v = yexp__value(c, src, (YEXP__AV_NUM | YEXP__AV_INT | YEXP__AV_VAR | YEXP__AV_LOOKUP) & lit, 0, elem, r->key, canon);
        break;
    case YEXP__T_DUR:
        v = yexp__value(c, src, (YEXP__AV_NUM | YEXP__AV_DUR | YEXP__AV_VAR | YEXP__AV_LOOKUP) & lit, 0, elem, r->key, canon);
        break;
    case YEXP__T_BOOL:
        if (src->type != YJS_TRUE && src->type != YJS_FALSE) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes true or false", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_BOOL, 0, elem, r->key, canon);
        break;
    case YEXP__T_STR:
        if (src->type != YJS_STRING) {
            yexp__err(c, src->pos, "%s: '%s': wrong type; this key takes a string", elem, r->key);
            return false;
        }
        v = yexp__value(c, src, YEXP__AV_STR | YEXP__AV_VAR, 0, elem, r->key, canon);
        if (v && v->kind == YEXP_V_VAR) {
            yexp__err(c, src->pos, "%s: '%s': a literal string, not a variable", elem, r->key);
            return false;
        }
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
        v = yexp__value(c, src, YEXP__AV_ARRAY | YEXP__AV_VAR | YEXP__AV_LOOKUP, YEXP__AV_NUM, elem, r->key, canon);
        if (v && v->kind == YEXP_V_ARRAY && v->n != 3) {
            yexp__err(c, src->pos, "%s: '%s': three numbers: [r, g, b]", elem, r->key);
            return false;
        }
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
        v = yexp__value(c, src, YEXP__AV_NUM | YEXP__AV_INT | YEXP__AV_VAR | YEXP__AV_LOOKUP, 0, elem, r->key, canon);
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
            if (one) *canon = ce;
            else yexp__cpush(c, carr, ce);
        }
        vv->items = it;
        vv->n = (uint32_t)n;
        v = vv;
        if (!one) *canon = carr;
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
            if (m->value->type != YJS_STRING) yexp__err(c, m->value->pos, "%s: 'note' is a string", elem);
            else yexp__put(c, out, "note", yexp__cstr(c, m->value->u.s));
            continue;
        }
        ri = yexp__find(c, sect, mask, m->key);
        if (ri < 0) { yexp__unknown(c, sect, mask, m, elem); continue; }
        slot = &slots[ri - f];
        slot->given = true;
        slot->src = m->value;
        slot->pos = m->key_pos;
        if (!yexp__row_value(c, ri, m, elem, slot, &cv)) continue;
        if (slot->v && cv) {
            const yexp_row* r = &yexp__rows[ri];
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
        const yexp_row* r = &yexp__rows[f + i];
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

static bool yexp__v_int(const yexp_value* v, int* out, int dflt) {
    if (v && v->kind == YEXP_V_NUMBER && (v->flags & YEXP_VF_INT) && v->i >= INT32_MIN && v->i <= INT32_MAX) {
        *out = (int)v->i;
        return true;
    }
    *out = dflt;
    return !v || v->kind != YEXP_V_NUMBER;
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
    if (!has_participant && n)
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
    {
        const yexp_value* t = yexp__slotv(c, s, YEXP__S_DISPLAY, "min_tier");
        if (t && t->kind == YEXP_V_NUMBER) {
            mt = (int)t->i;
            if (t->i < 0 || t->i > 3) yexp__err(c, t->pos, "display: 'min_tier' is 1 to 3");
        } else if (t && t->kind != YEXP_V_NUMBER) {
            yexp__err(c, t->pos, "display: 'min_tier' is a literal integer");
        }
    }
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

static void yexp__parse_devices(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_role* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!src) return;
    if (!yexp__set_begin(c, src, "devices", YEXP__MAX_SET, &n)) return;
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
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        snprintf(elem, sizeof elem, "role '%s'", m->key);
        s = yexp__object(c, YEXP__S_ROLE, 0xffffu, m->value, elem, &cr);
        if (!s) continue;
        r = &out[i++];
        r->name = m->key;
        r->pos_ = m->key_pos;
        {
            const char* dir = yexp__v_str(yexp__slotv(c, s, YEXP__S_ROLE, "direction"));
            r->direction = !dir ? 0 : yexp__streq(dir, "out") ? 1 : yexp__streq(dir, "both") ? 2 : 0;
        }
        v = yexp__slotv(c, s, YEXP__S_ROLE, "family");
        r->family = yexp__str_array(c, v, &r->n_family);
        if (v && v->n == 0) yexp__err(c, v->pos, "%s: 'family' has at least one name", elem);
        r->required = yexp__v_bool(yexp__slotv(c, s, YEXP__S_ROLE, "required"), true);
        yexp__v_int(yexp__slotv(c, s, YEXP__S_ROLE, "min_tier"), &r->min_tier, 0);
        {
            const yexp_value* p = yexp__slotv(c, s, YEXP__S_ROLE, "pulse_s");
            r->pulse_ns = p && p->kind == YEXP_V_NUMBER ? p->i : 0;
            if (p && p->kind != YEXP_V_NUMBER) yexp__err(c, p->pos, "%s: 'pulse_s' is a number of seconds, not a variable: a role is read before any trial", elem);
        }
        r->kind = yexp__v_str(yexp__slotv(c, s, YEXP__S_ROLE, "kind"));
        v = yexp__slotv(c, s, YEXP__S_ROLE, "buttons");
        if (v) { r->buttons = v->map; r->n_buttons = (int)v->n; }
        v = yexp__slotv(c, s, YEXP__S_ROLE, "map");
        if (v) { r->map = v->map; r->n_map = (int)v->n; }
        yexp__put(c, cset, m->key, cr);
        yexp__release(c, YEXP__S_ROLE);
    }
    c->d->roles = out;
    c->d->n_roles = i;
    *canon = cset;
}

static const yexp_role* yexp__role_get(const yexp_def* d, const char* name) {
    int i;
    for (i = 0; i < d->n_roles; i++)
        if (yexp__streq(d->roles[i].name, name)) return &d->roles[i];
    return NULL;
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
    props = (yexp_prop*)yexp__alloc(c, (size_t)rows * sizeof *props);
    dyn = (int*)yexp__alloc(c, (size_t)rows * sizeof *dyn);
    if (!props || !dyn) { yexp__release(c, YEXP__S_STIM); return; }
    out->name = m->key;
    out->kind = (yexp_stim_kind)kind;
    out->layer = 0;
    for (i = 0; i < rows; i++) {
        const yexp_row* r = &yexp__rows[f + i];
        if (!(r->kinds & mask)) continue;
        if (yexp__streq(r->key, "kind")) continue;
        if (yexp__streq(r->key, "layer")) {
            if (s[i].v && s[i].v->kind != YEXP_V_NUMBER)
                yexp__err(c, s[i].pos, "%s: 'layer' is a literal integer (D3)", elem);
            else
                yexp__v_int(s[i].v, &out->layer, 0);
            continue;
        }
        if (!s[i].v) continue;
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
    if (!yexp__set_begin(c, src, "stimuli", YEXP__MAX_SET, &n)) return;
    out = (yexp_stim*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        int before = c->r->n_errors;
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        yexp__parse_stimulus(c, m, &out[i], cset);
        if (c->r->n_errors == before && !c->nomem) i++;
    }
    c->d->stimuli = out;
    c->d->n_stimuli = i;
    *canon = cset;
}

YEXP_API const yexp_stim* yexp_stim_get(const yexp_def* d, const char* name) {
    int i;
    if (!d || !name) return NULL;
    for (i = 0; i < d->n_stimuli; i++)
        if (yexp__streq(d->stimuli[i].name, name)) return &d->stimuli[i];
    return NULL;
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
        const yexp_row* r = &yexp__rows[f + i];
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
    yexp__put(c, cset, m->key, cm);
    yexp__release(c, sect);
}

static void yexp__parse_methods(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_method* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!src) return;
    if (!yexp__set_begin(c, src, "methods", YEXP__MAX_SET, &n)) return;
    out = (yexp_method*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        int before = c->r->n_errors;
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        yexp__parse_method(c, m, &out[i], cset);
        if (c->r->n_errors == before && !c->nomem) i++;
    }
    c->d->methods = out;
    c->d->n_methods = i;
    *canon = cset;
}

static const yexp_method* yexp__method_get(const yexp_def* d, const char* name) {
    int i;
    for (i = 0; i < d->n_methods; i++)
        if (yexp__streq(d->methods[i].name, name)) return &d->methods[i];
    return NULL;
}

/* --- steps -------------------------------------------------------------- */

static const char* const yexp__verbs[] = { "on", "off", "wait_s", "at_s", "set", "to", "then", "trigger", "mark", "if", "unless" };
#define YEXP__N_VERBS 11

/* The stimulus keys a step may set or tween: a timeline channel carries a
 * float, so only the kind's numeric properties; a resource, a layout key
 * or an enum is fixed at load. */
static bool yexp__tweenable(const yexp_row* r) {
    return (r->type == YEXP__T_NUM || r->type == YEXP__T_INT || r->type == YEXP__T_VEC3) && !(r->flags & YEXP__R_LIT);
}

static void yexp__parse_props(yexp__ctx* c, const yjs_value* props, const yexp_stim* st, const char* elem, yexp_step* out, yjs_value** canon) {
    yexp_prop* pr;
    yjs_value* cp;
    size_t k;
    int n = 0;
    unsigned mask = 1u << (int)st->kind;
    if (props->type != YJS_OBJECT || props->n == 0) {
        yexp__err(c, props->pos, "%s: 'props' is an object of properties with at least one key", elem);
        return;
    }
    pr = (yexp_prop*)yexp__alloc(c, props->n * sizeof *pr);
    cp = yexp__cnew(c, YJS_OBJECT);
    if (!pr || !cp) return;
    for (k = 0; k < props->n; k++) {
        const yjs_member* m = &props->u.members[k];
        int ri;
        yexp__slot slot;
        yjs_value* cv = NULL;
        if (yexp__streq(m->key, "note")) {
            if (m->value->type != YJS_STRING) yexp__err(c, m->value->pos, "%s: 'note' is a string", elem);
            else yexp__put(c, cp, "note", yexp__cstr(c, m->value->u.s));
            continue;
        }
        ri = yexp__find(c, YEXP__S_STIM, mask, m->key);
        if (ri < 0) {
            if (yexp__streq(m->key, "color_r")) { yexp__unknown(c, YEXP__S_STIM, mask, m, elem); continue; }
            yexp__err(c, m->key_pos, "%s: property '%s' is not a property of the %s stimulus '%s'", elem, m->key, yexp__kinds[(int)st->kind], st->name);
            continue;
        }
        if (!yexp__tweenable(&yexp__rows[ri])) {
            yexp__err(c, m->key_pos, "%s: property '%s' of '%s' is fixed at load; a step cannot change it", elem, m->key, st->name);
            continue;
        }
        memset(&slot, 0, sizeof slot);
        if (!yexp__row_value(c, ri, m, elem, &slot, &cv)) continue;
        pr[n].key = yexp__rows[ri].key;
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
    if (src->type != YJS_OBJECT) { yexp__err(c, src->pos, "%s: wrong type; expected a step object", elem); return; }
    for (i = 0; i < src->n; i++)
        for (k = 0; k < YEXP__N_VERBS; k++)
            if (yexp__streq(src->u.members[i].key, yexp__verbs[k])) {
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
            if (st && st->given && tg && tg->n == 1 && src && yjs_get(src, yexp__verbs[verb]) && yjs_get(src, yexp__verbs[verb])->type != YJS_ARRAY)
                yexp__err(c, st->pos, "%s: 'stagger_s' goes with an array of stimuli", elem);
        }
        {
            int a, b;
            for (a = 0; a < out->n_stims; a++) {
                const char* nm = out->stims[a];
                if (!yexp_stim_get(c->d, nm)) yexp__err(c, tg->pos, "%s: '%s': no stimulus named '%s'", elem, yexp__verbs[verb], nm);
                for (b = 0; b < a; b++)
                    if (yexp__streq(out->stims[b], nm)) yexp__err(c, tg->pos, "%s: '%s': '%s' named twice", elem, yexp__verbs[verb], nm);
            }
        }
        break;
    }
    case YEXP_WAIT: case YEXP_AT:
        out->t = yexp__slotv(c, s, YEXP__S_STEP, yexp__verbs[verb]);
        break;
    case YEXP_SET: case YEXP_TO: case YEXP_THEN: {
        const char* name = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, yexp__verbs[verb]));
        const yexp_stim* st = name ? yexp_stim_get(c->d, name) : NULL;
        yexp__slot* ps = yexp__slot_at(c, s, YEXP__S_STEP, "props");
        const char** names = (const char**)yexp__alloc(c, sizeof(char*));
        if (names && name) { names[0] = name; out->stims = names; out->n_stims = 1; }
        if (!st && name) yexp__err(c, src->pos, "%s: '%s': no stimulus named '%s'", elem, yexp__verbs[verb], name);
        if (verb != YEXP_SET) {
            out->duration = yexp__slotv(c, s, YEXP__S_STEP, "duration_s");
            out->delay = yexp__slotv(c, s, YEXP__S_STEP, "delay_s");
            out->ease = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, "ease"));
        }
        if (st && ps && ps->src) {
            yjs_value* cp = NULL;
            yexp__parse_props(c, ps->src, st, elem, out, &cp);
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
            if (!r) yexp__err(c, src->pos, "%s: 'role': no role named '%s'", elem, role);
            else if (r->direction == 0) yexp__err(c, src->pos, "%s: 'role': '%s' is an input role; a trigger needs an output", elem, role);
        }
        break;
    }
    case YEXP_MARK:
        out->mark = yexp__v_str(yexp__slotv(c, s, YEXP__S_STEP, "mark"));
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

static void yexp__parse_response(yexp__ctx* c, const yjs_value* src, const char* telem, const yexp_response** out_r, yjs_value** canon) {
    char elem[128];
    yjs_value* cr = NULL;
    yexp__slot* s;
    yexp_response* r;
    const char* name;
    yexp__slot* chs;
    yexp__slot* trg;
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
    r->persist = yexp__v_bool(yexp__slotv(c, s, YEXP__S_RESPONSE, "persist"), false);
    yexp__v_int(yexp__slotv(c, s, YEXP__S_RESPONSE, "max_responses"), &r->max_responses, 0);
    r->allow_held_key = yexp__v_bool(yexp__slotv(c, s, YEXP__S_RESPONSE, "allow_held_key"), false);
    r->wait_for_key_release = yexp__v_bool(yexp__slotv(c, s, YEXP__S_RESPONSE, "wait_for_key_release"), false);
    if (r->role) {
        const yexp_role* ro = yexp__role_get(c->d, r->role);
        if (!ro) yexp__err(c, src->pos, "%s: 'role': no role named '%s'", elem, r->role);
        else if (ro->direction == 1) yexp__err(c, src->pos, "%s: 'role': '%s' is an output role; a response needs an input", elem, r->role);
    }
    name = r->from;
    if (name && !yexp_stim_get(c->d, name)) yexp__err(c, src->pos, "%s: 'from': no stimulus named '%s'", elem, name);
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
                    size_t a;
                    snprintf(ce, sizeof ce, "%s: 'choices'[%u]", elem, (unsigned)i);
                    if (it->type == YJS_STRING) {
                        ch[i].key = it->u.s;
                        ch[i].name = it->u.s;
                        yexp__cpush(c, ca, yexp__cstr(c, it->u.s));
                    } else {
                        yjs_value* co = NULL;
                        yexp__slot* s2 = yexp__object(c, YEXP__S_CHOICE, 0xffffu, it, ce, &co);
                        if (s2) {
                            ch[i].key = yexp__v_str(yexp__slotv(c, s2, YEXP__S_CHOICE, "key"));
                            ch[i].name = yexp__v_str(yexp__slotv(c, s2, YEXP__S_CHOICE, "name"));
                            if (!ch[i].name) ch[i].name = ch[i].key;
                            else if (ch[i].key && yexp__streq(ch[i].name, ch[i].key))
                                yexp__err(c, it->pos, "%s: 'name' equals 'key': write the key alone", ce);
                            yexp__cpush(c, ca, co);
                            yexp__release(c, YEXP__S_CHOICE);
                        }
                    }
                    if (ch[i].key && !ch[i].key[0]) yexp__err(c, it->pos, "%s: an empty key", ce);
                    for (a = 0; a < i; a++)
                        if (ch[i].key && ch[a].key && yexp__streq(ch[i].key, ch[a].key))
                            yexp__err(c, it->pos, "%s: the key '%s' is listed twice", ce, ch[i].key);
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
                if (!ro) yexp__err(c, trg->src->pos, "%s: 'role': no role named '%s'", te, r->trigger_role);
                else if (ro->direction == 0) yexp__err(c, trg->src->pos, "%s: 'role': '%s' is an input role; a trigger needs an output", te, r->trigger_role);
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
    int i, j, k;
    for (i = 0; i < n; i++) {
        const yexp_step* st = &steps[i];
        switch (st->op) {
        case YEXP_ON:
            for (j = 0; j < st->n_stims && *n_pending < YEXP__MAX_SET; j++) pending[(*n_pending)++] = st->stims[j];
            break;
        case YEXP_WAIT: case YEXP_AT: case YEXP_THEN:
            *n_pending = 0;
            break;
        case YEXP_TRIGGER:
            for (j = 0; j < *n_pending; j++) {
                bool dup = false;
                for (k = 0; k < *n_out; k++)
                    if (yexp__streq(out[k], pending[j])) dup = true;
                if (!dup && *n_out < cap) out[(*n_out)++] = pending[j];
            }
            break;
        case YEXP_IF: case YEXP_UNLESS:
            yexp__default_record_scan(c, st->steps, st->n_steps, pending, n_pending, out, n_out, cap);
            break;
        default: break;
        }
    }
}

static void yexp__default_record(yexp__ctx* c, yexp_template* t) {
    const char** pending;
    const char** out;
    int n_pending = 0, n = 0, cap = YEXP__MAX_LIST + 1;
    pending = (const char**)yexp__alloc(c, (size_t)YEXP__MAX_SET * sizeof(char*));
    out = (const char**)yexp__alloc(c, (size_t)cap * sizeof(char*));
    if (!pending || !out) return;
    if (t->response && t->response->from) out[n++] = t->response->from;
    yexp__default_record_scan(c, t->events, t->n_events, pending, &n_pending, out, &n, cap);
    t->record = out;
    t->n_record = n;
}

static bool yexp__str_arrays_equal(const char* const* a, int na, const char* const* b, int nb) {
    int i;
    if (na != nb) return false;
    for (i = 0; i < na; i++)
        if (!yexp__streq(a[i], b[i])) return false;
    return true;
}

/* A column that holds seconds ends in _s (D4). */
static void yexp__check_column_name(yexp__ctx* c, const yjs_member* m, const yexp_value* v, const char* elem) {
    if (v->kind == YEXP_V_VAR && yexp__ends_s(v->str) && !yexp__ends_s(m->key))
        yexp__err(c, m->key_pos, "%s: column '%s' holds seconds ('$%s'), so its name ends in '_s' (D4)", elem, m->key, v->str);
}

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
        kv[i].key = m->key;
        kv[i].value = v;
        i++;
        yexp__put(c, cobj, m->key, cv);
    }
    *out_kv = kv;
    *out_n = i;
    *canon = cobj;
}

static void yexp__parse_template(yexp__ctx* c, const yjs_member* m, yexp_template* out, yjs_value* cset) {
    char elem[96], sub[128];
    yexp__slot* s;
    yjs_value* ct = NULL;
    yexp__slot* x;
    snprintf(elem, sizeof elem, "template '%s'", m->key);
    if (m->value->type != YJS_OBJECT) { yexp__err(c, m->value->pos, "%s: wrong type; expected an object", elem); return; }
    if (yjs_get(m->value, "script")) {
        const yjs_member* mm = yjs_member_n(m->value, "script", 6);
        yexp__err(c, mm ? mm->key_pos : m->value->pos, "%s: 'script' needs a later format version; version 1 refuses it (D6)", elem);
        return;
    }
    s = yexp__object(c, YEXP__S_TEMPLATE, 0xffffu, m->value, elem, &ct);
    if (!s) return;
    out->name = m->key;
    out->pos_ = m->key_pos;
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "events");
    if (x && x->src) {
        yjs_value* ca = NULL;
        snprintf(sub, sizeof sub, "%s: events", elem);
        yexp__parse_steps(c, x->src, sub, 0, &out->events, &out->n_events, &ca);
        if (ca && ca->n) yexp__put(c, ct, "events", ca);
    }
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "after");
    if (x && x->src) {
        yjs_value* ca = NULL;
        snprintf(sub, sizeof sub, "%s: after", elem);
        yexp__parse_steps(c, x->src, sub, 0, &out->after, &out->n_after, &ca);
        if (ca && ca->n) yexp__put(c, ct, "after", ca);
    }
    x = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "response");
    if (x && x->src) {
        yjs_value* cr = NULL;
        yexp__parse_response(c, x->src, elem, &out->response, &cr);
        yexp__put(c, ct, "response", cr);
    }
    {
        const yexp_value* rv = yexp__slotv(c, s, YEXP__S_TEMPLATE, "record");
        yexp__slot* rslot = yexp__slot_at(c, s, YEXP__S_TEMPLATE, "record");
        yexp_template tmp = *out;
        yexp__default_record(c, &tmp);
        if (rslot && rslot->given && rv) {
            int a, b;
            out->record = yexp__str_array(c, rv, &out->n_record);
            for (a = 0; a < out->n_record; a++) {
                if (!yexp_stim_get(c->d, out->record[a]))
                    yexp__err(c, rv->pos, "%s: 'record': no stimulus named '%s'", elem, out->record[a]);
                for (b = 0; b < a; b++)
                    if (yexp__streq(out->record[a], out->record[b]))
                        yexp__err(c, rv->pos, "%s: 'record': '%s' listed twice", elem, out->record[a]);
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
        yexp__parse_values_set(c, x->src, sub, YEXP__AV_VALUE, false, &out->columns, &out->n_columns, &co);
        if (co && co->n) yexp__put(c, ct, "columns", co);
    }
    out->outcome = yexp__slotv(c, s, YEXP__S_TEMPLATE, "outcome");
    out->method_value = yexp__slotv(c, s, YEXP__S_TEMPLATE, "method_value");
    yexp__release(c, YEXP__S_TEMPLATE);
    yexp__put(c, cset, m->key, ct);
}

static void yexp__parse_trials(yexp__ctx* c, const yjs_value* src, yjs_value** canon) {
    yexp_template* out;
    yjs_value* cset;
    int n = 0, i = 0;
    size_t k;
    if (!yexp__set_begin(c, src, "trials", YEXP__MAX_SET, &n)) return;
    out = (yexp_template*)yexp__alloc(c, (size_t)(n ? n : 1) * sizeof *out);
    cset = yexp__cnew(c, YJS_OBJECT);
    if (!out || !cset) return;
    for (k = 0; k < src->n; k++) {
        const yjs_member* m = &src->u.members[k];
        int before = c->r->n_errors;
        if (yexp__is_set_note(m)) { yexp__put(c, cset, "note", yexp__cstr(c, m->value->u.s)); continue; }
        yexp__parse_template(c, m, &out[i], cset);
        if (c->r->n_errors == before && !c->nomem) i++;
    }
    c->d->templates = out;
    c->d->n_templates = i;
    *canon = cset;
}

static const yexp_template* yexp__template_get(const yexp_def* d, const char* name) {
    int i;
    for (i = 0; i < d->n_templates; i++)
        if (yexp__streq(d->templates[i].name, name)) return &d->templates[i];
    return NULL;
}

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

/* Jitter and var names that a rules text defines, from every element. */
static void yexp__scan_rules(yexp__ctx* c, const yjs_value* arr, yexp_node* node, const char* elem) {
    const char** jit;
    const char** rv;
    int nj = 0, nv = 0, cap = 64;
    uint32_t i;
    jit = (const char**)yexp__alloc(c, (size_t)cap * sizeof(char*));
    rv = (const char**)yexp__alloc(c, (size_t)cap * sizeof(char*));
    if (!jit || !rv) return;
    for (i = 0; i < arr->n; i++) {
        const yjs_value* s = arr->u.items[i];
        const char* p = s->u.s;
        while (*p) {
            char w[3][72];
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
                if ((is_jit ? nj : nv) >= cap) continue;
                name = (char*)yexp__alloc(c, n + 1);
                if (!name) return;
                memcpy(name, w[1], n + 1);
                if (is_jit) jit[nj++] = name;
                else rv[nv++] = name;
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
            if (ca && ca->n) yexp__put(c, cn, "timeline", ca);
            else if (ca) yexp__put(c, cn, "timeline", ca);
        }
    }
    if (leaf) {
        const char* tn = yexp__v_str(yexp__slotv(c, s, YEXP__S_NODE, "trial"));
        out->trial = tn ? yexp__template_get(c->d, tn) : NULL;
        if (tn && !out->trial) yexp__err(c, x->pos, "%s: 'trial': no template named '%s'", elem, tn);
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
        const yexp_method** ms;
        uint32_t i, a;
        ms = (const yexp_method**)yexp__alloc(c, (tv->n ? tv->n : 1) * sizeof(void*));
        if (ms) {
            for (i = 0; i < tv->n; i++) {
                ms[i] = yexp__method_get(c->d, tv->items[i].str);
                if (!ms[i]) yexp__err(c, x->pos, "%s: 'tracks': no method named '%s'", elem, tv->items[i].str);
                for (a = 0; a < i; a++)
                    if (yexp__streq(tv->items[i].str, tv->items[a].str))
                        yexp__err(c, x->pos, "%s: 'tracks': '%s' listed twice", elem, tv->items[i].str);
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
        yexp__parse_values_set(c, x->src, sub, YEXP__AV_VALUE | YEXP__AV_CHOICE, true, &out->vars, &out->n_vars, &co);
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

enum { YEXP__VK_BUILTIN = 1, YEXP__VK_SESSION, YEXP__VK_VARS, YEXP__VK_JITTER, YEXP__VK_RULEVAR, YEXP__VK_TABLE, YEXP__VK_DIM };

typedef struct yexp__var {
    int               kind;
    int               phase;
    const yexp_value* value;     /* VK_VARS                                 */
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
} yexp__scope;

static bool yexp__work(yexp__ctx* c) {
    if (c->over) return false;
    if (++c->steps <= YEXP__WORK_STEPS) return true;
    c->over = true;
    yexp__err(c, UINT32_MAX, "the definition's references take more than %ld steps to check; do many leaves share large templates, or do 'vars' choices refer to each other?", (long)YEXP__WORK_STEPS);
    return false;
}

/* Column `name` of a table: 1 found, 0 not, -1 no such table or no host. */
static int yexp__table_col(yexp__ctx* c, const char* table, const char* name, int* col, int* type) {
    int i;
    if (!c->h || !c->h->column) return -1;
    for (i = 0;; i++) {
        yexp_column info;
        int rc;
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
    for (i = s->n - 1; i >= 0; i--) {
        const yexp_node* n = s->chain[i];
        int k;
        for (k = 0; k < n->n_vars; k++)
            if (yexp__streq(n->vars[k].key, name)) {
                out->kind = YEXP__VK_VARS;
                out->phase = YEXP__PH_TRIAL;
                out->value = n->vars[k].value;
                return true;
            }
        for (k = 0; k < n->n_jitters; k++)
            if (yexp__streq(n->jitters[k], name)) {
                out->kind = YEXP__VK_JITTER;
                out->phase = YEXP__PH_TRIAL;
                return true;
            }
        for (k = 0; k < n->n_rule_vars; k++)
            if (yexp__streq(n->rule_vars[k], name)) {
                out->kind = YEXP__VK_RULEVAR;
                out->phase = YEXP__PH_TRIAL;
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

static void yexp__check_value(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int need, const char* elem, const char* key);

static void yexp__check_var(yexp__ctx* c, const yexp__scope* s, const char* name, uint32_t pos, int need, const char* elem, const char* key) {
    yexp__var r;
    const char* why;
    if (s->in_code && yexp__streq(name, "response") && s->t && s->t->response) return;
    if (!yexp__resolve(c, s, name, &r, &why)) {
        if (why) yexp__err(c, pos, "%s: '%s': '$%s' needs %s (2.3)", elem, key, name, why);
        else yexp__err(c, pos, "%s: '%s': '$%s' is not known here: no session field, table column, vars name, jitter or built-in has that name", elem, key, name);
        return;
    }
    if (r.phase > need)
        yexp__err(c, pos, "%s: '%s': '$%s' is known from %s, not here (2.3)", elem, key, name, yexp__phase_names[r.phase]);
}

/* --- lookups: a key for every level (format 2.2) ------------------------ */

static int yexp__lv_push(yexp__ctx* c, const char* level) {
    if (c->n_lv >= YEXP__MAX_LEVELS) return 0;
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
    if (depth > YEXP__VAR_DEPTH) return 0;
    for (i = 0; bools[i]; i++)
        if (yexp__streq(name, bools[i])) {
            /* the first three are true only on trials the rules make */
            if (yexp__lv_push(c, "false")) return -1;
            if ((i >= 3 || s->rt[i]) && yexp__lv_push(c, "true")) return -1;
            return 1;
        }
    if (yexp__streq(name, "outcome")) {
        const yexp_response* rsp = s->t ? s->t->response : NULL;
        if (!rsp) return 0;
        if (yexp__lv_push(c, "correct") || yexp__lv_push(c, "wrong")) return -1;
        /* a timeout needs a deadline */
        if (!(rsp->duration && rsp->duration->kind == YEXP_V_NUMBER && rsp->duration->i == 0) && yexp__lv_push(c, "timeout")) return -1;
        return 1;
    }
    if (yexp__streq(name, "response")) {
        const yexp_response* rsp = s->t ? s->t->response : NULL;
        if (!rsp || !rsp->n_choices) return 0;
        for (i = 0; i < rsp->n_choices; i++)
            if (yexp__lv_push(c, rsp->choices[i].key)) return -1;
        return 1;
    }
    if (yexp__streq(name, "method")) {
        if (!s->leaf || s->leaf->n_tracks == 0) return 0;
        for (i = 0; i < s->leaf->n_tracks; i++)
            if (yexp__lv_push(c, s->leaf->tracks[i]->name)) return -1;
        return 1;
    }
    if (yexp__is_builtin(name)) return 0;
    if (!yexp__resolve(c, s, name, &r, &why)) return 0;
    if (r.kind == YEXP__VK_VARS) return yexp__levels(c, s, r.value, depth + 1);
    if (r.kind == YEXP__VK_TABLE && !r.unverified && c->h && c->h->level) {
        for (i = 0;; i++) {
            char buf[256];
            int n = c->h->level(c->h->ctx, r.table, r.col, i, buf, sizeof buf);
            char* p;
            if (n < 0) return 1;
            if (n >= (int)sizeof buf) return 0;
            p = (char*)yexp__alloc(c, (size_t)n + 1);
            if (!p) return -1;
            memcpy(p, buf, (size_t)n);
            p[n] = 0;
            if (yexp__lv_push(c, p)) return -1;
            if (!yexp__work(c)) return 0;
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

static int yexp__str_cmp(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }

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
    rc = yexp__levels(c, s, v->by, 0);
    if (rc < 0) return;
    if (rc == 0) { v->reached_[v->n] |= 2u; return; }   /* some level is not known here */
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

static void yexp__check_value(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, int need, const char* elem, const char* key) {
    uint32_t k;
    if (!v || !yexp__work(c)) return;
    switch (v->kind) {
    case YEXP_V_VAR:
        yexp__check_var(c, s, v->str, v->pos, need, elem, key);
        break;
    case YEXP_V_LOOKUP:
        yexp__check_value(c, s, v->by, need, elem, key);
        for (k = 0; k < v->n; k++) yexp__check_value(c, s, v->map[k].value, need, elem, key);
        if (s->leaf) yexp__check_lookup(c, s, v, elem, key);
        break;
    case YEXP_V_ARRAY: case YEXP_V_CHOICE: case YEXP_V_LINSPACE:
        for (k = 0; k < v->n; k++) yexp__check_value(c, s, &v->items[k], need, elem, key);
        break;
    case YEXP_V_MICHELSON:
        yexp__check_value(c, s, v->by, need, elem, key);
        break;
    default: break;
    }
}

/* Marks every stimulus the steps name; byte 1 = used before 'after'. */
static void yexp__mark_stims(yexp__ctx* c, const yexp_step* steps, int n, int phase) {
    int i, j;
    for (i = 0; i < n; i++) {
        const yexp_step* st = &steps[i];
        if (!yexp__work(c)) return;
        for (j = 0; j < st->n_stims; j++) {
            const yexp_stim* ps = yexp_stim_get(c->d, st->stims[j]);
            if (ps) {
                uint8_t* u = &c->stim_use[ps - c->d->stimuli];
                if (phase == YEXP__PH_TRIAL) *u = 1;
                else if (!*u) *u = 2;
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
        yexp__check_value(c, s, st->t, phase, elem, st->op == YEXP_AT ? "at_s" : "wait_s");
        yexp__check_value(c, s, st->stagger, phase, elem, "stagger_s");
        yexp__check_value(c, s, st->duration, phase, elem, "duration_s");
        yexp__check_value(c, s, st->delay, phase, elem, "delay_s");
        yexp__check_value(c, s, st->code, phase, elem, "trigger");
        for (k = 0; k < st->n_props; k++) yexp__check_value(c, s, st->props[k].value, phase, elem, st->props[k].key);
        if (st->op == YEXP_IF || st->op == YEXP_UNLESS) {
            yexp__check_var(c, s, st->cond, st->cond_pos_, phase, elem, st->op == YEXP_IF ? "if" : "unless");
            yexp__check_steps(c, s, st->steps, st->n_steps, phase, elem);
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
    rc = yexp__levels(c, s, v, 0);
    if (rc <= 0) return;
    yexp__lv_unique(c);
    for (i = 0; i < c->n_lv; i++)
        if (!c->h->entry(c->h->ctx, kind, c->lv[i]))
            yexp__err(c, v->pos, "%s: '%s': no pack entry named '%s' (a level of '$%s')", elem, key, c->lv[i], v->str);
}

static void yexp__check_cycle(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, const char* elem, const char* key, int depth);

static void yexp__check_cycle_name(yexp__ctx* c, const yexp__scope* s, const char* name, uint32_t pos, const char* elem, const char* key, int depth) {
    yexp__var r;
    const char* why;
    if (depth > YEXP__VAR_DEPTH) {
        yexp__err(c, pos, "%s: '%s': '$%s': variables nest more than %d deep; is there a loop?", elem, key, name, YEXP__VAR_DEPTH);
        return;
    }
    if (yexp__resolve(c, s, name, &r, &why) && r.kind == YEXP__VK_VARS) yexp__check_cycle(c, s, r.value, elem, key, depth + 1);
}

static void yexp__check_cycle(yexp__ctx* c, const yexp__scope* s, const yexp_value* v, const char* elem, const char* key, int depth) {
    uint32_t k;
    if (!v || !yexp__work(c)) return;
    switch (v->kind) {
    case YEXP_V_VAR: yexp__check_cycle_name(c, s, v->str, v->pos, elem, key, depth); break;
    case YEXP_V_LOOKUP:
        yexp__check_cycle(c, s, v->by, elem, key, depth);
        for (k = 0; k < v->n; k++) yexp__check_cycle(c, s, v->map[k].value, elem, key, depth);
        break;
    case YEXP_V_CHOICE: case YEXP_V_ARRAY:
        for (k = 0; k < v->n; k++) yexp__check_cycle(c, s, &v->items[k], elem, key, depth);
        break;
    default: break;
    }
}

/* Every use of a variable in one leaf's trial. */
static void yexp__check_leaf(yexp__ctx* c, yexp__scope* s) {
    const yexp_template* t = s->t;
    char elem[128];
    int i, k;
    yexp__rules_truth(s->leaf, s->rt);
    /* which stimuli this leaf shows, and from when */
    memset(c->stim_use, 0, (size_t)c->d->n_stimuli + 1);
    yexp__mark_stims(c, t->events, t->n_events, YEXP__PH_TRIAL);
    yexp__mark_stims(c, t->after, t->n_after, YEXP__PH_AFTER);
    if (t->response && t->response->from) {
        const yexp_stim* ps = yexp_stim_get(c->d, t->response->from);
        if (ps) c->stim_use[ps - c->d->stimuli] = 1;
    }
    s->michelson = 0;
    for (i = 0; i < c->d->n_stimuli; i++) {
        const yexp_stim* st = &c->d->stimuli[i];
        const yexp_prop* p;
        if (!c->stim_use[i]) continue;
        p = yexp_prop_get(st->props, st->n_props, "contrast");
        if (p && p->value->kind == YEXP_V_MICHELSON) s->michelson++;
    }
    for (i = 0; i < s->n; i++) {
        const yexp_node* n = s->chain[i];
        snprintf(elem, sizeof elem, "node '%s': vars", n->path);
        for (k = 0; k < n->n_vars; k++) {
            yexp__check_value(c, s, n->vars[k].value, YEXP__PH_TRIAL, elem, n->vars[k].key);
            yexp__check_cycle(c, s, n->vars[k].value, elem, n->vars[k].key, 0);
        }
        snprintf(elem, sizeof elem, "node '%s': data", n->path);
        for (k = 0; k < n->n_data; k++) yexp__check_value(c, s, n->data[k].value, YEXP__PH_TRIAL, elem, n->data[k].key);
    }
    snprintf(elem, sizeof elem, "template '%s': events", t->name);
    yexp__check_steps(c, s, t->events, t->n_events, YEXP__PH_TRIAL, elem);
    snprintf(elem, sizeof elem, "template '%s': after", t->name);
    yexp__check_steps(c, s, t->after, t->n_after, YEXP__PH_AFTER, elem);
    snprintf(elem, sizeof elem, "template '%s': response", t->name);
    if (t->response) {
        const yexp_response* r = t->response;
        yexp__check_value(c, s, r->duration, YEXP__PH_TRIAL, elem, "duration_s");
        yexp__check_value(c, s, r->min_rt, YEXP__PH_TRIAL, elem, "minimum_valid_rt_s");
        if (r->key_answer) yexp__check_value(c, s, r->key_answer, YEXP__PH_TRIAL, elem, "key_answer");
        if (r->trigger_code) {
            /* a response rule's code is per choice, so it is known as the
             * response arrives (G8) */
            s->in_code = true;
            yexp__check_value(c, s, r->trigger_code, YEXP__PH_TRIAL, elem, "trigger.code");
            s->in_code = false;
        }
    }
    snprintf(elem, sizeof elem, "template '%s'", t->name);
    if (t->outcome && t->outcome->pos != UINT32_MAX) yexp__check_value(c, s, t->outcome, YEXP__PH_AFTER, elem, "outcome");
    if (t->method_value && t->method_value->pos != UINT32_MAX) yexp__check_value(c, s, t->method_value, YEXP__PH_AFTER, elem, "method_value");
    snprintf(elem, sizeof elem, "template '%s': columns", t->name);
    for (k = 0; k < t->n_columns; k++) yexp__check_value(c, s, t->columns[k].value, YEXP__PH_ROW, elem, t->columns[k].key);
    for (i = 0; i < c->d->n_stimuli; i++) {
        const yexp_stim* st = &c->d->stimuli[i];
        int need;
        if (!c->stim_use[i]) continue;
        need = c->stim_use[i] == 1 ? YEXP__PH_TRIAL : YEXP__PH_AFTER;
        snprintf(elem, sizeof elem, "stimulus '%s'", st->name);
        for (k = 0; k < st->n_dyn_; k++) {
            const yexp_prop* p = &st->props[st->dyn_[k]];
            yexp__check_value(c, s, p->value, need, elem, p->key);
            if (st->kind == YEXP_IMAGE && yexp__streq(p->key, "texture") && p->value->kind == YEXP_V_VAR)
                yexp__check_entry_levels(c, s, p->value, YEXP_ENTRY_TEXTURE, elem, p->key);
            if (st->kind == YEXP_TEXT && yexp__streq(p->key, "text") && p->value->kind == YEXP_V_VAR) {
                yexp__var r;
                const char* why;
                if (yexp__resolve(c, s, p->value->str, &r, &why)) {
                    if (r.kind == YEXP__VK_JITTER)
                        yexp__err(c, p->value->pos, "%s: 'text': '$%s' is a jitter, a duration; a text comes from a string column or a lookup", elem, p->value->str);
                    else if (r.kind == YEXP__VK_TABLE && !r.unverified && r.ctype != YEXP_COL_STRING)
                        yexp__err(c, p->value->pos, "%s: 'text': '$%s' is a column of '%s' that is not a string; a text comes from a string column or a lookup", elem, p->value->str, r.table);
                }
            }
        }
    }
    if (c->h && c->h->rules && s->leaf->n_rules >= 0) {
        char msg[256];
        msg[0] = 0;
        if (c->h->rules(c->h->ctx, s->leaf, msg, sizeof msg))
            yexp__err(c, s->leaf->pos_, "node '%s': rules: %s", s->leaf->path, msg);
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
        if (nd->if_var) yexp__check_var(c, s, nd->if_var, nd->if_pos_, YEXP__PH_NODE, elem, "if");
        if (nd->unless_var) yexp__check_var(c, s, nd->unless_var, nd->unless_pos_, YEXP__PH_NODE, elem, "unless");
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
                yexp__err(c, d->roles[i].pos_, "role '%s': %s", d->roles[i].name, msg);
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
                    yexp__err(c, st->props[k].value->pos, "%s: '%s': %s", elem, st->props[k].key, msg);
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
                    yexp__err(c, m->props[k].value->pos, "method '%s': '%s': %s", m->name, m->props[k].key, msg);
            }
        }
    if (c->h && c->h->check_ops)
        for (i = 0; i < d->n_templates; i++) {
            msg[0] = 0;
            if (c->h->check_ops(c->h->ctx, d, &d->templates[i], msg, sizeof msg))
                yexp__err(c, d->templates[i].pos_, "template '%s': %s", d->templates[i].name, msg);
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

/* --- the root ----------------------------------------------------------- */

static void yexp__parse_root(yexp__ctx* c, const yjs_value* root) {
    yexp_def* d = c->d;
    const yjs_value* fmt;
    const yjs_value* ver;
    int64_t version = 0;
    yexp__slot* s;
    yexp__slot* x;
    yjs_value* croot = NULL;
    yjs_value* cc;
    static const char* const parts[] = { "display", "web", "devices", "stimuli", "methods", "session" };
    (void)parts;
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
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "stimuli");
    yexp__parse_stimuli(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "stimuli", cc);
    cc = NULL;
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "methods");
    yexp__parse_methods(c, x->src, &cc);
    if (cc && cc->n) yexp__put(c, croot, "methods", cc);
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "trials");
    if (x->src) {
        cc = NULL;
        yexp__parse_trials(c, x->src, &cc);
        yexp__put(c, croot, "trials", cc);
    }
    x = yexp__slot_at(c, s, YEXP__S_ROOT, "timeline");
    if (x->src) {
        cc = NULL;
        yexp__parse_children(c, x->src, NULL, "", 0, &d->timeline, &d->n_timeline, &cc);
        yexp__put(c, croot, "timeline", cc);
    }
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

/* --- the entry point ---------------------------------------------------- */

static void yexp__check_mark(yexp__ctx* c, yexp_report* r, unsigned bit, bool present) {
    (void)c;
    if (present) r->run |= bit;
    else r->skipped |= bit;
}

YEXP_API int yexp_load(yjs_arena* a, const char* text, size_t n, const yexp_host* host, yexp_def** out, yexp_report* report) {
    yexp__ctx* c;
    yexp_def* d;
    yjs_value* root = NULL;
    yjs_error je;
    int i, last;
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
    last = -1;
    for (i = 0; i < YEXP__N_ROWS; i++) {
        int sect = yexp__rows[i].section;
        if (sect != last) { c->first[sect] = i; last = sect; }
        c->count[sect]++;
    }
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
    if (!c->nomem && report->n_errors == 0) {
        yexp__scope* sc = (yexp__scope*)yexp__alloc(c, sizeof *sc);
        c->stim_use = (uint8_t*)yexp__alloc(c, (size_t)d->n_stimuli + 1);
        if (sc && c->stim_use) {
            yexp__check_mark(c, report, YEXP_CHECK_ENTRIES, host && host->entry);
            yexp__check_mark(c, report, YEXP_CHECK_COLUMNS, host && host->column);
            yexp__check_mark(c, report, YEXP_CHECK_LEVELS, host && host->level && host->column);
            yexp__check_mark(c, report, YEXP_CHECK_ROLES, host && host->role);
            yexp__check_mark(c, report, YEXP_CHECK_PARAMS, host && host->param);
            yexp__check_mark(c, report, YEXP_CHECK_RULES, host && host->rules);
            yexp__check_mark(c, report, YEXP_CHECK_OPS, host && host->check_ops);
            yexp__check_gamuts(c);
            memset(sc, 0, sizeof *sc);
            yexp__check_nodes(c, sc, d->timeline, d->n_timeline);
            yexp__check_host_globals(c);
            if (report->n_errors == 0) yexp__warn_unreached(c);
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
