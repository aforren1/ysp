/* chart.h - a template's states and exits as a chart (step 4, part A;
 * docs/designer.md, "The chart"). OWNED BY PART A.
 *
 * Three parts, each with no window:
 * - The model (dsg_chart): the states, the end and the edges of one
 *   template, made from a parsed definition. The view makes it again each
 *   time buffer 0's revision changes.
 * - The commands: each is one transaction, so one undo takes back a whole
 *   chart edit. Each sets the selection to what it made or changed, so undo
 *   and redo put the selection back too.
 * - The view state (dsg_chart_view): pan, zoom and the position of each
 *   state, keyed by template name and state name. It is the view file's
 *   "chart" part, never the definition (format D13), and it follows renames
 *   through dsg_doc_listen() with dsg_chart_view_on_rename().
 *
 * States are named by their index in the template's `states` array. A
 * command reads the definition as it is when it runs; the caller's model may
 * be older, so a command checks the index and refuses one out of range.
 */
#ifndef YSP_DESIGNER_CHART_H
#define YSP_DESIGNER_CHART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The end node's name in the view state; a state cannot have it (format
 * 3.9), so it never collides with a state's position. */
#define DSG_CHART_END "end"

/* Kinds of edge (format 3.11). */
enum {
    DSG_EDGE_EXIT = 0,   /* a state's exit                                     */
    DSG_EDGE_SHARED,     /* a template's shared exit, one edge per `in` state  */
    DSG_EDGE_NEXT,       /* no exits: the next listed state, or the end (rule 10) */
    DSG_EDGE_OVER        /* `over`, after `max_visits`                         */
};

typedef struct dsg_chart_state {
    const char* name;    /* in the chart's own memory                          */
    int  n_steps, n_exits;
    int  max_visits;     /* 0: none                                            */
    int  step;           /* events templates: the part's first step; else -1  */
} dsg_chart_state;

typedef struct dsg_chart_edge {
    int from, to;        /* state indices; n_states is the end; to -1: a `to`
                          * that names no state                               */
    int kind;            /* DSG_EDGE_*                                         */
    int index;           /* EXIT: the exit's index in its state; SHARED: the
                          * shared exit's index; else -1                       */
    int in_index;        /* SHARED: the state's index in `in`; else -1         */
    int slot;            /* 0 for the first edge between two nodes, 1, 2 ...
                          * for the next ones (either way), so they bow apart  */
    const char* label;   /* "on ready", "after 0.5 s, if $x", ...; the chart's */
    const char* to_name; /* `to` as written; "" for NEXT                       */
} dsg_chart_edge;

typedef struct dsg_chart {
    const char*      tmpl;         /* the template's name; the chart's own    */
    bool             has_states;   /* false: the template uses `events`, and
                                    * the states are its parts split at each
                                    * `until` (format 3.11 rule 9), read-only */
    dsg_chart_state* states;  int n_states;
    dsg_chart_edge*  edges;   int n_edges;
    /* memory, kept between builds so a build allocates only when it grows */
    char* pool_;  size_t pool_n_, pool_cap_;
    int   states_cap_, edges_cap_;
} dsg_chart;

/* The chart of template tmpl in root (the definition's tree). out is zeroed
 * or a chart built before; its memory is reused. -1 when there is no such
 * template or it has neither `states` nor `events` (out is then empty), or
 * out of memory. */
int  dsg_chart_build(const yjs_value* root, const char* tmpl, dsg_chart* out);
void dsg_chart_free(dsg_chart* c);
/* The state named name, or -1. */
int  dsg_chart_find(const dsg_chart* c, const char* name);
/* The JSON Pointer of state i ("/trials/T/states/i"; an events part: its
 * first step), of edge e ("" for NEXT), and of a template. -1 when it does
 * not fit. */
int  dsg_chart_state_path(const dsg_chart* c, int i, char* out, size_t cap);
int  dsg_chart_edge_path(const dsg_chart* c, const dsg_chart_edge* e, char* out, size_t cap);
int  dsg_chart_template_path(const char* tmpl, char* out, size_t cap);

/* Where a path in the definition falls in the chart (a selection, an
 * issue's place). */
typedef struct dsg_chart_place {
    int state;           /* the state the path is in, or -1                   */
    int kind;            /* the edge's kind when the path is in an exit, a
                          * shared exit or `over`; else -1                    */
    int index;           /* EXIT: the exit's index; SHARED: the shared exit's;
                          * else -1                                           */
} dsg_chart_place;
/* false when the path is not under the chart's template's states or shared
 * exits. A shared exit's path has state -1. */
bool dsg_chart_locate(const dsg_chart* c, const char* path, dsg_chart_place* out);
/* The place names edge e: its exit, its shared exit (every edge of it) or
 * its `over`. */
bool dsg_chart_place_has_edge(const dsg_chart_place* p, const dsg_chart_edge* e);

/* --- the preview's place (step 5, part C) ------------------------------------ */

struct dsg_preview_where;   /* preview.h */

/* The nodes that the chart rings for the preview's place w, as the range
 * [*first, *last] (states, then the end at n_states). A states template:
 * the state that w names, or the end when it names none. An events
 * template: w gives no step (pl_sim_where has none), so every part while
 * the events run or the response waits, and the end once `after` runs.
 * false, with -1 in both: w is NULL, between trials, in another template,
 * or names a state that this chart does not have (the text changed since
 * the session loaded). */
bool dsg_chart_running(const dsg_chart* c, const struct dsg_preview_where* w, int* first, int* last);
/* Follow: the template that w runs, as root's `trials` names it, so the
 * pointer lives as long as root. NULL when w is NULL, between trials, or
 * runs a template that root does not have. */
const char* dsg_chart_follow(const yjs_value* root, const struct dsg_preview_where* w);

/* --- commands: each one transaction, so one undo step ------------------------ */

/* Each returns 0, or -1 with the transaction cancelled and one line in
 * dsg_chart_error(): the last refusal (one document per thread). */
const char* dsg_chart_error(void);

/* Appends a state {"name": name}; makes `states` when the template has
 * neither `states` nor `events`. Selects the new state. Refuses an empty
 * name, "end", "response", and a name another state has. */
int  dsg_chart_add_state(dsg_doc* d, const char* tmpl, const char* name);
/* Appends the exit {"to": to} to state `from` (`to` a state or "end").
 * The exit is instant until a panel gives it `on` or `after_s`. Selects it. */
int  dsg_chart_connect(dsg_doc* d, const char* tmpl, int from, const char* to);
/* Removes the state, every exit and shared exit that leads to it, its name
 * from shared exits' `in` (a shared exit whose `in` empties goes too), and
 * `over` and `max_visits` where `over` leads to it. Selects the template. */
int  dsg_chart_delete_state(dsg_doc* d, const char* tmpl, int state);
/* Removes the exit, the shared exit, or the `over` (with its `max_visits`)
 * at exit_path. An emptied `exits` goes too. Selects the template. */
int  dsg_chart_delete_exit(dsg_doc* d, const char* exit_path);
/* Renames the state and every name in the template that refers to it: an
 * exit's, a shared exit's and an `over`'s `to`, a shared exit's `in`, and a
 * template control's and a column's `from`, `to` and `time` (format 3.9).
 * Notes the rename for the view state. Refuses as dsg_chart_add_state(). */
int  dsg_chart_rename_state(dsg_doc* d, const char* tmpl, int state, const char* name);
/* Moves the state to the head of the list: the first state is the start.
 * Selects it. */
int  dsg_chart_set_first(dsg_doc* d, const char* tmpl, int state);

/* --- the view state: the view file's "chart" part ----------------------------- */

typedef struct dsg_chart_pos {
    char* name;          /* a state's name, or DSG_CHART_END                  */
    float x, y;          /* the box's top left, in chart units                */
} dsg_chart_pos;

typedef struct dsg_chart_tview {
    char* name;          /* the template's name                               */
    float pan_x, pan_y;  /* screen px from the canvas's top left to chart 0,0 */
    float zoom;          /* screen px per chart unit                          */
    dsg_chart_pos* pos;  int n_pos, cap_pos;
} dsg_chart_tview;

typedef struct dsg_chart_view {
    char* current;       /* the template shown; NULL: none chosen             */
    dsg_chart_tview* t;  int n_t, cap_t;
    uint64_t gen;        /* changes when a position is added, removed or
                          * renamed, so a view can cache lookups              */
    bool follow;         /* step 5: show the template that the preview runs;
                          * the part's "follow", written only when true       */
} dsg_chart_view;

void dsg_chart_view_init(dsg_chart_view* v);
void dsg_chart_view_free(dsg_chart_view* v);   /* leaves it empty and usable */
/* Reads the part's JSON into an empty view. Entries of the wrong shape are
 * skipped: view state is not worth refusing a project for. -1 when it does
 * not parse (the view stays empty). */
int  dsg_chart_view_read(dsg_chart_view* v, const char* json, size_t n);
/* The part's JSON (compact, keys in byte order), malloc'd, with *n its
 * length; NULL when out of memory. Positions and pan are written as whole
 * units, the zoom to 3 decimals. */
char* dsg_chart_view_write(const dsg_chart_view* v, size_t* n);
int  dsg_chart_view_set_current(dsg_chart_view* v, const char* tmpl);
/* The template's entry; made (pan 40, 40, zoom 1) when make is true. NULL
 * when there is none or out of memory. */
dsg_chart_tview* dsg_chart_view_template(dsg_chart_view* v, const char* tmpl, bool make);
bool dsg_chart_view_pos(const dsg_chart_view* v, const char* tmpl, const char* state, float* x, float* y);
int  dsg_chart_view_set_pos(dsg_chart_view* v, const char* tmpl, const char* state, float x, float y);
/* A dsg_rename_fn: ctx is the dsg_chart_view. A state's rename (scope
 * "/trials/T/states") moves its position; a template's ("/trials") moves the
 * template's entry. A position the new name had is dropped. */
void dsg_chart_view_on_rename(void* ctx, const char* scope, const char* from, const char* to);

#ifdef __cplusplus
}
#endif
#endif
