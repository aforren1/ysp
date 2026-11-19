/* chart.h - a template's states and exits as a chart (step 4, part A;
 * docs/designer.md, "Plan of step 4"). OWNED BY PART A: A may change this
 * file and chart.c as it needs. The declarations below are the plan's
 * sketch; until A builds them, each command returns -1.
 *
 * The model is made from the parsed definition (dsg_doc_shown()) each time
 * buffer 0's revision changes. Edits go through the document (doc.h) as one
 * transaction each, so one undo takes back a whole chart edit. Positions
 * are view state (format D13): the view file's "chart" part, keyed by
 * template name and state name, never the definition.
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

/* Kinds of edge (format 3.11). */
enum {
    DSG_EDGE_EXIT = 0,   /* a state's exit                                   */
    DSG_EDGE_SHARED,     /* a template's shared exit, one edge per `in` state */
    DSG_EDGE_NEXT,       /* no exits: the next listed state, or the end (rule 10) */
    DSG_EDGE_OVER        /* `over` after max_visits                          */
};

typedef struct dsg_chart_state {
    const char* name;    /* into the tree: valid as the tree is (doc.h)      */
    int         n_steps, n_exits;
} dsg_chart_state;

typedef struct dsg_chart_edge {
    int  from, to;       /* state indices; n_states is the end               */
    int  kind;           /* DSG_EDGE_*                                       */
    char path[DSG_SEL_PATH]; /* the exit's JSON Pointer; "" for NEXT          */
} dsg_chart_edge;

typedef struct dsg_chart {
    char             tmpl[128];
    bool             has_states;   /* false: the template uses `events`      */
    dsg_chart_state* states;  int n_states;
    dsg_chart_edge*  edges;   int n_edges;
} dsg_chart;

/* The chart of template tmpl in root. -1 when there is no such template. */
int  dsg_chart_build(const yjs_value* root, const char* tmpl, dsg_chart* out);
void dsg_chart_free(dsg_chart* c);

/* Commands: each one transaction, so one undo step. */
int  dsg_chart_add_state(dsg_doc* d, const char* tmpl, const char* name);
/* An exit from state `from` to a state or "end". */
int  dsg_chart_connect(dsg_doc* d, const char* tmpl, int from, const char* to);
/* Removes the state, the exits that lead to it, and its name from shared
 * exits' `in`. */
int  dsg_chart_delete_state(dsg_doc* d, const char* tmpl, int state);
int  dsg_chart_delete_exit(dsg_doc* d, const char* exit_path);
/* Renames the state and every `to`, `in` and `over.to` in the template that
 * names it; notes the rename for the view state. */
int  dsg_chart_rename_state(dsg_doc* d, const char* tmpl, int state, const char* name);
/* Moves the state to the head of the list: the first state is the start. */
int  dsg_chart_set_first(dsg_doc* d, const char* tmpl, int state);

#ifdef __cplusplus
}
#endif
#endif
