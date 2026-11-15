/* player/v2.c - format version 2 (docs/experiment_format.md, part 3): a
 * template's controls lowered to one ysp/sig.h graph, its states and exits
 * to op tables per state visit, the value forms to selector channels and
 * mux nodes, the state rows, the traces and the columns from controls, and
 * the --sim participant of signal roles (a samples file).
 *
 * Decisions this file takes where part 3 is not exact are marked
 * "reading F<n>" and listed in docs/player.md and docs/experiment.md. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V2_END (-1)
#define V2_MAX_FORMS 8           /* distinct forms of one property         */
#define V2_HITQ 64
#define V2_TRACE_ROWS 65536      /* a sample trace's rows in one trial      */
/* a frame trace's rows that wait for records: the rows' bound, the test
 * delay of 8 frames and margin, and never fewer than 0.3.0's 64 */
#define V2_FRAME_WAIT(p) (((p)->out_wait_frames + 12) * 2 > 64 ? ((p)->out_wait_frames + 12) * 2 : 64)
#define V2_STATE_ROWS 64         /* state rows waiting for their record     */
#define V2_HISTORY 4096          /* D38's ring for a value read at a time   */
#define V2_WINDOW_CKS 8          /* sig.h's checkpoints (catchup_frames)    */
#define V2_CATCHUP_NS 50000000LL /* sig.h's window in input time (catchup_ns) */
#define V2_WINDOW_HZ 8000        /* the fastest raw mouse a role may have   */
#define V2_WINDOW_FRAME_CALLS 16 /* a frame's own calls into the log        */
#define V2_SAMPLE_MAX_S 1e7      /* a samples file's latest time: 115 days  */
#define V2_MAX_FRAME_TRACES 256  /* frame traces of a definition: their rows wait */
#define V2_SAMPLE_MAX_V 1e9      /* a sample's largest coordinate           */

enum { X_HIT = 1, X_RESPONSE, X_AFTER, X_INSTANT };
enum { P_NUM = 0, P_NEG, P_SEC, P_ENTRY };   /* P_ENTRY: a hold in seconds to a detect's entry */

/* --- structures ------------------------------------------------------- */

typedef struct v2_param {      /* a node parameter a variable gives (3.6, 1) */
    int node, param;           /* param >= 0: p[]; -1 - k: data[k]        */
    const yexp_value* v;
    int elem;                  /* -1 the value; k element k of an array     */
    int how;                   /* P_*                                      */
} v2_param;

typedef struct v2_ctl {
    const yexp_control* def;
    int val;                   /* value node, -1 none                       */
    int test;                  /* test node (1 lane), -1 none               */
    int hit;                   /* hit detect, -1 none                       */
    int hit_u;                 /* an until's own detect, when exits name the
                                * control too (F3: theirs stays armed), -1  */
    int of;                    /* the node of `of`                          */
    int emit;                  /* a movement's emit                         */
    int choice;                /* a movement's choice in the collector      */
    bool armed_by_exit;        /* some exit or until names its hits         */
    bool building;
    int slot;                  /* a cursor's or moving target's stimulus binds */
    int64_t hit_t; bool has_hit;
} v2_ctl;

typedef struct v2_form {       /* one distinct form of a property            */
    const yexp_value* v;
    char key[256];             /* its canonical text                        */
    int ch_sel;
} v2_form;

typedef struct v2_prop {       /* a stimulus float that forms drive (3.10)   */
    const char* stim;
    const char* prop;          /* "x", "color_r", ... "visible"             */
    int comp;                  /* a color's component, else -1             */
    int ch_c;                  /* its own timeline channel (constants)      */
    int n_forms;
    v2_form forms[V2_MAX_FORMS];
    int slot;                  /* its graph channel                         */
} v2_prop;

typedef struct v2_vch {        /* a literal branch of a when: a channel     */
    const yexp_value* v;       /* the branch                                */
    int comp;
    int ch;
    int prop, form;            /* set when that form lands                  */
} v2_vch;

typedef struct v2_exit {
    int kind;                  /* X_*                                      */
    int ctl;                   /* X_HIT                                    */
    int node;                  /* X_HIT: the detect it reads and arms       */
    bool until;                /* an until's own hit: counts from its wait  */
    const yexp_value* after;   /* X_AFTER                                  */
    const char* if_var;
    bool if_flag;
    int to;                    /* a runtime state, V2_END                   */
    const yexp_value* outcome;
    const char* outcome_text;  /* an until's "timeout"                      */
    int code;                  /* the state row's code                      */
    bool part;                 /* an until's timeout: from the wait's start,
                                * not the state's (F3)                     */
} v2_exit;

typedef struct v2_rs {         /* a runtime state: a segment or a wait (rule 9) */
    int def;                   /* the state; -1 in an events template       */
    const yexp_step* steps; int n_steps;
    bool wait;                 /* an until's wait                           */
    bool first;                /* the first of its state: the visit starts  */
    v2_exit* exits; int n_exits;
    int next;                  /* after the steps: the next runtime state,
                                * V2_END, or -2: stay (exits only)          */
} v2_rs;

typedef struct v2_trace {
    const yexp_control* def;
    bool frame;
    int n_items;
    int item_node[8], item_lanes[8];
    int join, rec;             /* a sample trace's nodes                    */
    int stream;
    int n_cols;
    ytr_col cols[40];
    char names[40][YTB_MAX_NAME + 1];
    /* a sample trace's rows in this trial: t, lanes, tier, device */
    int64_t* rt; double* rv; uint8_t* rtier; uint32_t* rdev; int nrows; long lost;
} v2_trace;

/* A frame trace's row waiting for its frame's flip record (3.7). One the
 * next trial's reset overtakes keeps its values read at planned_s. */
typedef struct v2_frow { int64_t frame, planned; int trial; int tmpl; bool snap; int flags; double vals[32]; } v2_frow;
enum { V2_FR_NO_RECORD = 1, V2_FR_AT_PLANNED = 2 };   /* a frame trace's flags column */

typedef struct v2_srow {       /* a state row waiting for its entry frame's record */
    int64_t frame, end_ns, onset, planned, residual;
    int trial, code;
    const char* name;
    bool ended, rec_in;
} v2_srow;

struct pl_v2 {
    const yexp_template* def;
    int index;
    /* graph */
    ysig_graph g;
    ysig_desc gd;
    ysig_node* nodes; int nn, ncap;
    ysig_role* roles;
    ysig_adapter* adapters; ysig_node* ad_nodes;
    double* data; int nd, dcap;
    void* arena;
    int* role_node;            /* per signal role: its role node, -1        */
    int* sel_cache;            /* node x 4: a lane's select node            */
    bool has_graph;
    v2_ctl* ctl; int n_ctl;
    v2_param* params; int n_params, cap_params;
    struct { int node; const yexp_value* v; const yexp_value* about; } xform[8]; int n_xform;
    /* forms */
    v2_prop* props; int n_props, cap_props;
    v2_vch* vch; int n_vch, cap_vch;
    int n_slots;
    ygfx_bind* binds; int n_binds;
    int* gate_ch;              /* per state: a peak's gate channel, -1      */
    /* states */
    v2_rs* rs; int n_rs;
    int* first_rs;             /* per state: its first runtime state         */
    int n_states;
    const yexp_step** recenter; int n_recenter;
    /* traces */
    v2_trace* traces; int n_traces;
    /* response */
    int resp_from_kind;        /* the movements' from: YEXP_REF_*            */
    int resp_from_index;
    const yexp_control* resp_from_ctl;
    int base_state;            /* a moving target's from, -1               */
    /* the trial */
    int cur;                   /* runtime state, V2_END                     */
    int def_cur;               /* its state                                */
    int64_t entry_t, steps_end, entry_onset;
    int64_t visit_onset;       /* the state visit's entry frame onset: its
                                * after_s exits count from it in every part (F3) */
    int64_t def_entry_frame, def_entry_t;
    bool steps_pending, entry_onset_known;
    int ev0, ev1;              /* the runtime state's events               */
    int* visits;
    int64_t* state_onset;      /* per state: its latest entry frame's onset, -1 */
    int64_t* stim_onset;       /* per stimulus: first onset in the trial, -1 */
    int64_t* stim_frame;
    bool* flags;
    const yexp_value* outcome; const char* outcome_text; bool outcome_set;
    bool end_pending, ended;
    int64_t end_onset;
    int64_t base_onset;        /* the from state's entry frame onset, -1     */
    bool hit_entry;            /* the runtime state's ops land by D16: no late flag */
    int64_t resp_t;            /* the response's time, known at the end      */
    char outcome_buf[256];
    int64_t take_t; int n_take;
    bool frozen;               /* ysig_open() done: no node is added after */
    /* an events template (2.5 kept by part 3): `after` follows the events
     * and the response, as in version 1 */
    bool events_done, resp_done, after_built;
    bool trial_entry;          /* the first state's visit at the trial's start:
                                * its head recenters ran before the reset (3.6) */
    int arm_rs;                /* a visit whose first step is a recenter: its
                                * exits arm at that call (3.12), -1 none      */
    const yexp_step* arm_after;/* the visit's last head recenter             */
    int visit_ev0;             /* the visit's first event: rule 8 takes all its parts' */
    int trial_ev0;             /* the trial's first event                   */
    uint32_t* arm_gen;         /* per node: arms so far, so a drained hit of
                                * an arm since made is the old arm's        */
    int64_t arm_bad;           /* sig.h's retracted, stale and fallback
                                * counts seen                               */
    int64_t* arm_t;            /* per node: its latest arm's time           */
    int16_t* st_armed;         /* the nodes the current state's exits armed */
    int64_t* dis_t;            /* per node: the time of its latest
                                * ysig_disarm_at() this trial, INT64_MIN: an
                                * arm before it would be ended by it (W2)    */
    int64_t late_frames0;      /* sig.h's late_frames at the trial's start  */
    yin_event* der; int n_der; /* a poll's derived events, held while its
                                * hits are taken (v2_der_upto())            */
    int64_t late_bound;        /* the latest time of an arm made at or
                                * before the graph's input time (in sig.h's
                                * reference order at its call)              */
    int64_t n_late;            /* input calls with a time before late_bound:
                                * late input after an arm (any footprint)   */
    int n_st_armed;
    bool rows_open;            /* the trial's rows go to its traces: from its
                                * reset to the flush at its end             */
    bool* det_now;             /* per node: entry now this trial (no hold)  */
    /* the visit's tweens, every part's: rule 8 stops one still running at
     * the value it last wrote (a track is no event ytl_remove() takes) */
    struct v2_tw { int ch; int64_t end; }* tw; int n_tw, cap_tw;   /* sized at load */
    struct v2_frz { int ch; float v; }* frz; int n_frz;   /* the last exit's stopped
                                * channels: the next table's first tween on one
                                * starts from its frozen value */
    int arm_wait;              /* an until whose arm waits for a recenter's mark, -1 */
    int arm_wait_id;           /* that mark's event id                       */
    int from_ch;               /* the response's from: its visible channel, -1 */
    bool from_pending;         /* a table of this trial shows `from`         */
    bool resp_ended;           /* the key response's window ended            */
    bool end_wait;             /* F34: the states reached `end`; the trial
                                * waits for the response's end              */
    int64_t end_wait_t, resp_dur, resp_minrt;
    /* a key response's end being delivered (pl_v2_response()): the hits of
     * the samples up to it compete with its exit by time and list order */
    int64_t resp_tie_t; bool resp_tie_responded, resp_taken;
    int take_cap;              /* state changes at one time: the template's
                                * runtime states and overs, and a margin    */
    int64_t last_eval_rel;     /* the last evaluated frame's base time: a
                                * stopped tween holds what it wrote there    */
    /* the flip records' onsets of the trial's frame times (a time column
     * takes the record's, as the rows do): per stimulus, per state, the end */
    int64_t* state_frame;      /* the latest entry frame, -1                  */
    int64_t* stim_recf; int64_t* stim_rec;
    int64_t* state_recf; int64_t* state_rec;
    int64_t end_frame, end_recf, end_rec;
    int64_t events_end_rel, resp_bt;   /* from the trial's base            */
    /* 0.12.0, the wait for complete input (docs/player.md, "Explanation:
     * late input"): per signal role, whether it reaches a detect or an
     * emit; any such role with a lateness bound above 0 (late) */
    bool* feeds; bool late;
    int64_t g_used;            /* the horizon of the drain before the latest
                                * take (N1)                                */
    int64_t dr_n, dr_g;        /* hit drains so far; the latest's horizon   */
    int64_t take_dr;           /* dr_n at the latest take                   */
    /* R5 (i): every take at one time, not only the latest: a hit at that
     * time of an exit earlier in a left state's list would have won by
     * rule 4, so it came late. A chain of takes at one time leaves several
     * states, and the latest alone misses the earlier ones' exits. Per
     * take: the state left, its exit's place (n_exits: a steps' end, which
     * a hit at its time beats; -1: none), dr_n at it */
    int64_t ch_t;              /* the chain's time; INT64_MIN: none         */
    int ch_n, ch_cap;
    int* ch_rs; int* ch_k; int64_t* ch_dr;
    int64_t* arm_tk;           /* per node: dr_n at its arm, and at the    */
    int64_t* arm_tk_prev;      /* arm before: a hit of an arm that a drain
                                * before a take followed, earlier than that
                                * drain's horizon, came late               */
    int64_t g_der;             /* the horizon of the last derived drain     */
    int64_t arm_min;           /* the earliest state call placed since then
                                * (an arm, a disarm_at, a recenter): an
                                * event before it is no such call's (N1,
                                * R5 (ii))                                */
    int64_t lag_max;           /* the trial's largest; YTR_MISSING_NS none */
    bool resp_pend, resp_pend_responded;   /* a key response's end later */
    int64_t resp_pend_t;       /* than the horizon waits for it            */
    int der_cap;               /* v->der's room: sig.h's derived queue      */
};

typedef struct pl_v2_session {
    pl_v2** t;                 /* per template; NULL: version 1 path        */
    int n_t;
    int cur_t;                 /* the template being lowered or run         */
    /* the samples (3.19 item 1) */
    struct v2_sample { int64_t t, arr; float x, y; int role; }* smp;   /* arr: when it reaches the player */
    int n_smp, next_smp;
    double* gain;              /* per signal role                           */
    /* the graphs' channels after the timeline's */
    float* vals; int n_vals;
    float dummy;
    /* uniform draws of the open leaf (D29) */
    struct v2_uni { const char* name; int n; double v[64]; yexp_value items[64]; }* uni; int n_uni, cap_uni;
    /* frames whose trace rows and state rows wait for a record */
    v2_frow* fpend; int n_fpend, cap_fpend;   /* sized at load, V2_FRAME_WAIT(p) a frame trace */
    v2_srow srow[V2_STATE_ROWS]; int n_srow;
    int64_t t0;                /* the samples' time 0: the first frame's onset */
    bool t0_set;
    long recenter_skipped;
    int64_t bad_seen;          /* sig.h's counts the guard stops on, summed */
    yin_event* batch; int batch_cap;   /* a poll's samples, one ysig_feed_n() */
    int n_sample_rows;
    bool* reads;               /* pl_v2_reads()'s scratch, one per stimulus */
    /* 0.12.0: per signal role, its lateness bound B (ns), the newest stamp
     * fed this trial S (INT64_MIN none), reports fed at or before the
     * role's complete-to time (late_beyond_bound) */
    int64_t* bnd; int64_t* newest; int64_t* beyond;
    int64_t bnd_max;
    int64_t beyond_take;       /* hits and events after a take (N1)         */
    int64_t poll, plim;        /* the last whole feed's poll time and limit */
    bool polled;
    int64_t lag_n, lag_max; double lag_sum;   /* session.json exit_lag    */
    pl_v2* flush_pend;         /* a trial's end whose flush waits (C2)      */
} pl_v2_session;

typedef struct v2_tcell { int col; int64_t a, b; int sa, sb; bool has_a, has_b; int64_t fa, fb; } v2_tcell;
struct pl_v2_row {
    int n, cap;                /* cap: the most `time` columns of a template */
    v2_tcell* t;
};

/* --- helpers ----------------------------------------------------------- */

static pl_v2_session* v2s(pl_player* p) {
    if (!p->v2s) p->v2s = (pl_v2_session*)pl_alloc(p, sizeof(pl_v2_session));
    return p->v2s;
}

static pl_v2* v2_cur(pl_player* p) {
    if (!p->v2s || p->v2s->cur_t < 0 || p->v2s->cur_t >= p->v2s->n_t) return NULL;
    return p->v2s->t[p->v2s->cur_t];
}

static int v2_num(pl_player* p, const yexp_value* v, double* out) {
    if (p->env.extreme) {
        if (v->kind == YEXP_V_NUMBER) { *out = v->flags & YEXP_VF_DURATION ? (double)v->i / 1e9 : v->num; return 0; }
        *out = 0.5;
        return 0;
    }
    return pl_eval_num(p, &p->env, v, out);
}

static bool v2_is_form(const yexp_value* v) { return v && (v->kind == YEXP_V_SIGNAL || v->kind == YEXP_V_WHEN); }

/* A form's canonical text: two steps with one text share a selector (3.10). */
static void v2_form_text(const yexp_value* v, char* buf, size_t cap, size_t* used) {
    char t[64];
    size_t n;
#define PUT(s) do { n = strlen(s); if (*used + n < cap) { memcpy(buf + *used, s, n); *used += n; buf[*used] = 0; } } while (0)
    if (!v) { PUT("null"); return; }
    switch (v->kind) {
    case YEXP_V_SIGNAL:
        PUT("{signal:"); PUT(v->str);
        snprintf(t, sizeof t, ".%d", (int)v->i); PUT(t);
        if (v->by) { PUT(",above:"); v2_form_text(v->by, buf, cap, used); }
        PUT("}");
        break;
    case YEXP_V_WHEN:
        PUT("{when:"); v2_form_text(v->by, buf, cap, used);
        PUT(",then:"); v2_form_text(&v->items[0], buf, cap, used);
        PUT(",else:"); v2_form_text(&v->items[1], buf, cap, used);
        PUT("}");
        break;
    case YEXP_V_TEST: PUT("{test:"); PUT(v->str); PUT("}"); break;
    case YEXP_V_VAR: PUT("$"); PUT(v->str); break;
    case YEXP_V_STRING: PUT("\""); PUT(v->str); PUT("\""); break;
    case YEXP_V_BOOL: PUT(v->i ? "true" : "false"); break;
    case YEXP_V_NUMBER: snprintf(t, sizeof t, "%.17g", v->num); PUT(t); break;
    case YEXP_V_ARRAY: {
        uint32_t k;
        PUT("[");
        for (k = 0; k < v->n; k++) { if (k) PUT(","); v2_form_text(&v->items[k], buf, cap, used); }
        PUT("]");
        break;
    }
    case YEXP_V_LOOKUP: {
        uint32_t k;
        PUT("{by:"); v2_form_text(v->by, buf, cap, used);
        for (k = 0; k < v->n; k++) { PUT(","); PUT(v->map[k].key); PUT(":"); v2_form_text(v->map[k].value, buf, cap, used); }
        PUT("}");
        break;
    }
    default: snprintf(t, sizeof t, "<%d>", v->kind); PUT(t); break;
    }
#undef PUT
}

/* --- states (3.11) ------------------------------------------------------ */

static int v2_ctl_index_of(pl_v2* v, const yexp_control* c);
static int v2_hit_clone(pl_player* p, pl_v2* v, int k);
static bool v2_entry_frame(const pl_v2* v, const v2_exit* x, const yscr_frame* f);
static int v2_recenter_scan(pl_player* p, pl_v2* v);

/* The runtime states of a template: each state's steps split at `until`
 * (rule 9, reading F3: the state's exits stay armed in every part), and an
 * events template with `until` as states with no names. */
static int v2_states_build(pl_player* p, pl_v2* v) {
    const yexp_template* t = v->def;
    int s, cap = 4, k;
    const yexp_step* steps;
    int n_states = t->states ? t->n_states : 1;
    for (s = 0; s < n_states; s++) {
        int n = t->states ? t->states[s].n_steps : t->n_events;
        steps = t->states ? t->states[s].steps : t->events;
        cap += 2;
        for (k = 0; k < n; k++) if (steps[k].op == YEXP_UNTIL) cap += 2;
    }
    v->rs = (v2_rs*)pl_alloc(p, sizeof(v2_rs) * (size_t)cap);
    v->first_rs = (int*)pl_alloc(p, sizeof(int) * (size_t)(n_states + 1));
    v->n_states = t->states ? t->n_states : 0;
    if (!v->rs || !v->first_rs) return -1;
    for (s = 0; s < n_states; s++) {
        int n = t->states ? t->states[s].n_steps : t->n_events, a = 0;
        steps = t->states ? t->states[s].steps : t->events;
        v->first_rs[s] = v->n_rs;
        for (;;) {
            int b = a;
            v2_rs* r;
            while (b < n && steps[b].op != YEXP_UNTIL) b++;
            r = &v->rs[v->n_rs++];
            memset(r, 0, sizeof *r);
            r->def = t->states ? s : -1;
            r->steps = steps + a;
            r->n_steps = b - a;
            r->first = a == 0;
            if (b >= n) break;
            /* the until's wait */
            r = &v->rs[v->n_rs++];
            memset(r, 0, sizeof *r);
            r->def = t->states ? s : -1;
            r->wait = true;
            r->steps = &steps[b];   /* the until step itself: its control and timeout */
            r->n_steps = 0;
            a = b + 1;
        }
    }
    v->first_rs[n_states] = v->n_rs;
    return 0;
}

/* Exits of each runtime state, after the controls exist (a hit names one). */
static int v2_exits_build(pl_player* p, pl_v2* v) {
    const yexp_template* t = v->def;
    int i;
    for (i = 0; i < v->n_rs; i++) {
        v2_rs* r = &v->rs[i];
        int s = r->def, n_own = 0, n_sh = 0, k, cap, ne = 0;
        const yexp_state* st = s >= 0 ? &t->states[s] : NULL;
        bool resp_end = t->states && t->response && t->response->ends_trial;
        int last_of_def = (s >= 0 ? v->first_rs[s + 1] : v->n_rs) - 1;
        if (st) {
            n_own = st->n_exits;
            for (k = 0; k < t->n_exits; k++) {
                int m;
                for (m = 0; m < t->exits[k].n_in; m++) if (t->exits[k].in[m] == s) { n_sh++; break; }
            }
        }
        cap = n_own + n_sh + 3;
        r->exits = (v2_exit*)pl_alloc(p, sizeof(v2_exit) * (size_t)cap);
        if (!r->exits) return -1;
#define ADD_EXIT(ex, code_) do { \
            v2_exit* x = &r->exits[ne++]; \
            const yexp_exit* e_ = (ex); \
            memset(x, 0, sizeof *x); \
            x->code = (code_); \
            x->if_var = e_->if_var; x->if_flag = e_->if_flag; \
            x->outcome = e_->outcome; \
            x->to = e_->to.kind == YEXP_REF_END ? V2_END : v->first_rs[e_->to.index]; \
            if (e_->on.kind == YEXP_REF_RESPONSE) x->kind = X_RESPONSE; \
            else if (e_->on.kind == YEXP_REF_CONTROL) { x->kind = X_HIT; x->ctl = v2_ctl_index_of(v, e_->on.control); \
                if (x->ctl < 0) { pl_fail(p, "template '%s': an exit names a control with no hits", t->name); return -1; } \
                x->node = v->ctl[x->ctl].hit; \
                v->ctl[x->ctl].armed_by_exit = true; } \
            else if (e_->after) { x->kind = X_AFTER; x->after = e_->after; } \
            else x->kind = X_INSTANT; \
        } while (0)
        if (st) {
            int code = 0;
            for (k = 0; k < st->n_exits; k++) ADD_EXIT(&st->exits[k], code++);
            for (k = 0; k < t->n_exits; k++) {
                int m, in = 0;
                for (m = 0; m < t->exits[k].n_in; m++) if (t->exits[k].in[m] == s) in = 1;
                if (in) ADD_EXIT(&t->exits[k], code++);
            }
            if (resp_end) {
                /* 3.13: response_ends_trial with states, a shared exit to end after the state's own */
                v2_exit* x = &r->exits[ne++];
                memset(x, 0, sizeof *x);
                x->kind = X_RESPONSE;
                x->to = V2_END;
                x->code = code++;
            }
        }
#undef ADD_EXIT
        if (r->wait) {
            /* rule 9: {"on": C, "to": NEXT} and, with timeout_s, {"after_s": t,
             * "to": "end", "outcome": "timeout"}; codes after the state's */
            const yexp_step* u = r->steps;
            v2_exit* x = &r->exits[ne++];
            memset(x, 0, sizeof *x);
            x->kind = X_HIT;
            x->until = true;
            x->ctl = u->target.kind == YEXP_REF_CONTROL ? v2_ctl_index_of(v, u->target.control) : -1;
            if (x->ctl < 0) { pl_fail(p, "template '%s': an until names no control with hits", t->name); return -1; }
            v->ctl[x->ctl].armed_by_exit = true;
            x->node = v->ctl[x->ctl].hit;
            {
                /* F3: the state's exits arm at its entry and stay armed; an
                 * until on a control they name arms a detect of its own, so
                 * its arm does not restart their hold */
                int m;
                for (m = 0; m < ne - 1; m++)
                    if (r->exits[m].kind == X_HIT && r->exits[m].ctl == x->ctl) {
                        if (v->ctl[x->ctl].hit_u < 0 && (v->ctl[x->ctl].hit_u = v2_hit_clone(p, v, v->ctl[x->ctl].hit)) < 0) return -1;
                        x->node = v->ctl[x->ctl].hit_u;
                        break;
                    }
            }
            x->to = i + 1;
            x->code = n_own + n_sh + (resp_end ? 1 : 0);
            if (u->timeout) {
                x = &r->exits[ne++];
                memset(x, 0, sizeof *x);
                x->kind = X_AFTER;
                x->after = u->timeout;
                x->to = V2_END;
                x->outcome_text = "timeout";
                x->part = true;
                /* reading F28: the code after the state's own, shared and
                 * added exits; the until's own hit writes no row (it stays
                 * in the state), so it takes no code of its own */
                x->code = n_own + n_sh + (resp_end ? 1 : 0);
            }
        }
        r->n_exits = ne;
        /* after the steps: the until's wait or the next part; at the last
         * part, stay when the state has exits (rule 10), else the next
         * listed state, or the end */
        if (!r->wait && i < last_of_def) r->next = i + 1;
        else if (r->wait) r->next = -2;
        /* reading F34: the response exit that response_ends_trial adds
         * does not make a state one with exits; only the author's own and
         * shared exits do (3.11 rule 10, as the loader's rules count) */
        else if (st && (st->n_exits > 0 || n_sh > 0)) r->next = -2;
        else if (s >= 0 && s + 1 < t->n_states) r->next = v->first_rs[s + 1];
        else r->next = V2_END;
    }
    return 0;
}

/* --- channels (pl_channels_assign) ------------------------------------ */

static const char* v2_strdup(pl_player* p, const char* s) {
    size_t n = strlen(s);
    char* d = (char*)pl_alloc(p, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static int v2_prop_of(pl_player* p, pl_v2* v, const char* stim, const char* prop, int comp) {
    int i;
    for (i = 0; i < v->n_props; i++)
        if (strcmp(v->props[i].stim, stim) == 0 && strcmp(v->props[i].prop, prop) == 0) return i;
    if (v->n_props == v->cap_props) {
        int nc = v->cap_props ? v->cap_props * 2 : 8;
        v2_prop* q = (v2_prop*)pl_alloc(p, sizeof(v2_prop) * (size_t)nc);
        if (!q) return -1;
        if (v->n_props) memcpy(q, v->props, sizeof(v2_prop) * (size_t)v->n_props);
        v->props = q;
        v->cap_props = nc;
    }
    memset(&v->props[v->n_props], 0, sizeof v->props[0]);
    v->props[v->n_props].stim = stim;
    v->props[v->n_props].prop = prop;
    v->props[v->n_props].comp = comp;
    v->props[v->n_props].ch_c = pl_chan_add(p, stim, strcmp(prop, "visible") == 0 ? NULL : prop);
    v->props[v->n_props].slot = -1;
    if (v->props[v->n_props].ch_c < 0) return -1;
    return v->n_props++;
}

/* A literal branch's channel, under a when of one form. */
static int v2_vch_scan(pl_player* p, pl_v2* v, const yexp_value* f, int comp, int prop, int form) {
    char name[96];
    if (!f) return 0;
    if (f->kind == YEXP_V_WHEN) {
        if (v2_vch_scan(p, v, &f->items[0], comp, prop, form) < 0) return -1;
        return v2_vch_scan(p, v, &f->items[1], comp, prop, form);
    }
    if (f->kind == YEXP_V_SIGNAL) return 0;
    if (v->n_vch == v->cap_vch) {
        int nc = v->cap_vch ? v->cap_vch * 2 : 8;
        v2_vch* q = (v2_vch*)pl_alloc(p, sizeof(v2_vch) * (size_t)nc);
        if (!q) return -1;
        if (v->n_vch) memcpy(q, v->vch, sizeof(v2_vch) * (size_t)v->n_vch);
        v->vch = q;
        v->cap_vch = nc;
    }
    snprintf(name, sizeof name, "\x01v:%s:%d", v->def->name, v->n_vch);
    v->vch[v->n_vch].v = f;
    v->vch[v->n_vch].comp = comp;
    v->vch[v->n_vch].prop = prop;
    v->vch[v->n_vch].form = form;
    v->vch[v->n_vch].ch = pl_chan_add(p, v->props[prop].stim, v2_strdup(p, name));
    if (v->vch[v->n_vch].ch < 0) return -1;
    v->n_vch++;
    return 0;
}

static int v2_forms_scan(pl_player* p, pl_v2* v, const yexp_step* st, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        if (s->op == YEXP_IF || s->op == YEXP_UNLESS) { if (v2_forms_scan(p, v, s->steps, s->n_steps) < 0) return -1; continue; }
        if (s->op != YEXP_SET) continue;
        for (k = 0; k < s->n_props; k++) {
            const yexp_value* f = s->props[k].value;
            int comp, n_comp = strcmp(s->props[k].key, "color") == 0 ? 3 : 1;
            char key[256];
            size_t used = 0;
            if (!v2_is_form(f)) continue;
            key[0] = 0;
            v2_form_text(f, key, sizeof key, &used);
            for (comp = 0; comp < n_comp; comp++) {
                static const char* const cn[3] = { "color_r", "color_g", "color_b" };
                const char* pn = n_comp == 3 ? cn[comp] : s->props[k].key;
                int pi = v2_prop_of(p, v, s->stims[0], pn, n_comp == 3 ? comp : -1), j;
                v2_prop* pr;
                if (pi < 0) return -1;
                pr = &v->props[pi];
                for (j = 0; j < pr->n_forms; j++) if (strcmp(pr->forms[j].key, key) == 0) break;
                if (j < pr->n_forms) continue;
                if (pr->n_forms == V2_MAX_FORMS) { pl_fail(p, "template '%s': '%s.%s' has more than %d forms", v->def->name, pr->stim, pr->prop, V2_MAX_FORMS); return -1; }
                {
                    char name[96];
                    snprintf(name, sizeof name, "\x01s:%s:%d:%d", v->def->name, pi, j);
                    pr->forms[j].v = f;
                    snprintf(pr->forms[j].key, sizeof pr->forms[j].key, "%s", key);
                    pr->forms[j].ch_sel = pl_chan_add(p, pr->stim, v2_strdup(p, name));
                    if (pr->forms[j].ch_sel < 0) return -1;
                    pr->n_forms++;
                }
                if (v2_vch_scan(p, v, f, n_comp == 3 ? comp : -1, pi, j) < 0) return -1;
            }
        }
    }
    return 0;
}

int pl_v2_ops_bound(const yexp_template* t) {
    int b = 16, s;
    if (t->states)
        for (s = 0; s < t->n_states; s++) b += 4 * pl_ops_bound(t->states[s].steps, t->states[s].n_steps) + 8;
    b += 4 * (pl_ops_bound(t->events, t->n_events) + pl_ops_bound(t->after, t->n_after));
    return b;
}

static bool v2_needs(const yexp_template* t) {
    int k;
    if (t->states || t->n_controls || t->n_flags) return true;
    for (k = 0; k < t->n_events; k++) if (t->events[k].op == YEXP_UNTIL) return true;
    if (t->uses_top) return true;
    if (t->response && t->response->signal_choices) return true;
    return false;
}

int pl_v2_channels(pl_player* p) {
    pl_v2_session* S;
    int i;
    if (p->def->format_version < 2) return 0;
    S = v2s(p);
    if (!S) return -1;
    S->n_t = p->def->n_templates;
    S->cur_t = -1;
    S->t = (pl_v2**)pl_alloc(p, sizeof(pl_v2*) * (size_t)(S->n_t + 1));
    if (!S->t) return -1;
    for (i = 0; i < S->n_t; i++) {
        const yexp_template* t = &p->def->templates[i];
        pl_v2* v;
        int s;
        if (!v2_needs(t)) continue;
        v = (pl_v2*)pl_alloc(p, sizeof(pl_v2));
        if (!v) return -1;
        S->t[i] = v;
        v->def = t;
        v->index = i;
        v->base_state = -1;
        if (v2_states_build(p, v) < 0) return -1;
        /* every state's steps take channels and marks, as events do */
        for (s = 0; s < v->n_rs; s++) {
            extern void pl_chan_steps(pl_player* p, const yexp_step* st, int n);
            if (!v->rs[s].wait) pl_chan_steps(p, v->rs[s].steps, v->rs[s].n_steps);
            if (v2_forms_scan(p, v, v->rs[s].steps, v->rs[s].wait ? 0 : v->rs[s].n_steps) < 0) return -1;
        }
        if (p->failed) return -1;
    }
    return 0;
}

/* --- the graph (3.6, 3.7) --------------------------------------------- */

static int v2_node(pl_player* p, pl_v2* v, int op, int in0, int in1) {
    if (v->frozen) {
        /* a node made after ysig_open() would alias another's index */
        pl_fail(p, "template '%s': internal: a graph node made after the graph opened", v->def->name);
        return -1;
    }
    if (v->nn >= v->ncap) {
        int nc = v->ncap ? v->ncap * 2 : 64;
        ysig_node* q = (ysig_node*)pl_alloc(p, sizeof(ysig_node) * (size_t)nc);
        int* sc = (int*)pl_alloc(p, sizeof(int) * 4 * (size_t)nc);
        if (!q || !sc) return -1;
        if (v->nn) memcpy(q, v->nodes, sizeof(ysig_node) * (size_t)v->nn);
        memset(sc, 0xff, sizeof(int) * 4 * (size_t)nc);
        if (v->nn) memcpy(sc, v->sel_cache, sizeof(int) * 4 * (size_t)v->nn);
        v->nodes = q;
        v->sel_cache = sc;
        v->ncap = nc;
    }
    v->nodes[v->nn] = ysig_node_of(op, in0, in1);
    return v->nn++;
}

static int v2_param_add(pl_player* p, pl_v2* v, int node, int param, const yexp_value* val, int elem, int how) {
    if (!val) return 0;
    if (v->n_params == v->cap_params) {
        int nc = v->cap_params ? v->cap_params * 2 : 32;
        v2_param* q = (v2_param*)pl_alloc(p, sizeof(v2_param) * (size_t)nc);
        if (!q) return -1;
        if (v->n_params) memcpy(q, v->params, sizeof(v2_param) * (size_t)v->n_params);
        v->params = q;
        v->cap_params = nc;
    }
    v->params[v->n_params].node = node;
    v->params[v->n_params].param = param;
    v->params[v->n_params].v = val;
    v->params[v->n_params].elem = elem;
    v->params[v->n_params].how = how;
    v->n_params++;
    return 0;
}

/* A value that is fixed at load sets the node now; one that a variable
 * gives is set at each trial's start (3.7: "set at the trial's start with
 * ysig_set()"). Both go through the list, so the load's value is the
 * first trial's too. */
static int v2_set(pl_player* p, pl_v2* v, int node, int param, const yexp_value* val, int elem, int how) {
    const yexp_value* lit = val;
    if (!val) return 0;
    if (elem >= 0) lit = val->kind == YEXP_V_ARRAY && (uint32_t)elem < val->n ? &val->items[elem] : NULL;
    if (lit && (lit->kind == YEXP_V_NUMBER || lit->kind == YEXP_V_BOOL)) {
        /* a literal: the node has it from the open */
        double x = how == P_SEC && (lit->flags & YEXP_VF_DURATION) ? (double)lit->i / 1e9 : lit->num;
        if (how == P_NEG) x = -x;
        if (param >= 0) v->nodes[node].p[param] = x;
        else ((double*)v->nodes[node].data)[-1 - param] = x;
        return 0;
    }
    return v2_param_add(p, v, node, param, val, elem, how);
}

static int v2_role_node(pl_player* p, pl_v2* v, int role) {
    int k;
    if (v->role_node[role] >= 0) return v->role_node[role];
    k = v2_node(p, v, YSIG_OP_ROLE, -1, -1);
    if (k < 0) return -1;
    v->nodes[k].p[YSIG_ROLE_ROLE] = role;
    v->nodes[k].history = V2_HISTORY;
    v->role_node[role] = k;
    return k;
}

static int v2_lane(pl_player* p, pl_v2* v, int node, int lane) {
    int k;
    if (lane < 0) return node;
    if (v->sel_cache[node * 4 + lane] >= 0) return v->sel_cache[node * 4 + lane];
    k = v2_node(p, v, YSIG_OP_SELECT, node, -1);
    if (k < 0) return -1;
    v->nodes[k].lanes = 1;
    v->nodes[k].p[0] = lane;
    v->nodes[k].history = V2_HISTORY;
    v->sel_cache[node * 4 + lane] = k;
    return k;
}

static int v2_ctl_build(pl_player* p, pl_v2* v, int ci);

static int v2_ctl_index_of(pl_v2* v, const yexp_control* c) {
    int i;
    for (i = 0; i < v->n_ctl; i++) if (v->ctl[i].def == c) return i;
    for (i = 0; i < v->n_ctl; i++) if (strcmp(v->ctl[i].def->name, c->name) == 0) return i;
    return -1;
}

/* The node of a name's value (3.9): a role, or a control's value or test. */
static int v2_ref_node(pl_player* p, pl_v2* v, const yexp_ref* r, bool all_lanes) {
    int n = -1;
    if (r->kind == YEXP_REF_ROLE) n = v2_role_node(p, v, r->index);
    else if (r->kind == YEXP_REF_CONTROL) {
        int ci = v2_ctl_index_of(v, r->control);
        if (ci < 0) { pl_fail(p, "template '%s': the control '%s' is not in its graph", v->def->name, r->name); return -1; }
        if (v2_ctl_build(p, v, ci) < 0) return -1;
        n = v->ctl[ci].val >= 0 ? v->ctl[ci].val : v->ctl[ci].test;
    }
    if (n < 0) { pl_fail(p, "template '%s': '%s' has no value", v->def->name, r->name ? r->name : "?"); return -1; }
    if (!all_lanes && r->lane >= 0) return v2_lane(p, v, n, r->lane);
    return n;
}

static const yexp_value* v2_key(const yexp_control* c, const char* key) {
    const yexp_prop* pr = yexp_prop_get(c->props, c->n_props, key);
    return pr ? pr->value : NULL;
}

static bool v2_given(const yexp_control* c, const char* key) {
    const yexp_prop* pr = yexp_prop_get(c->props, c->n_props, key);
    return pr && pr->value && pr->value->kind != YEXP_V_NONE;
}

static int v2_rate(const yexp_control* c) {
    const yexp_value* r = v2_key(c, "rate");
    return r && r->kind == YEXP_V_STRING && strcmp(r->str, "frame") == 0 ? 1 : 0;
}

/* A hit detect armed by ysig_arm(): one hit per arm; entry now with no
 * hold (D35 (f), G16), arm with one. */
static int v2_hit_detect(pl_player* p, pl_v2* v, int in, const yexp_value* stay, int rate) {
    int k = v2_node(p, v, YSIG_OP_DETECT, in, -1);
    if (k < 0) return -1;
    v->nodes[k].p[YSIG_DETECT_START] = 1;
    v->nodes[k].p[YSIG_DETECT_REPEAT] = 0;
    v->nodes[k].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    v->nodes[k].p[YSIG_DETECT_AT] = rate;
    v->nodes[k].history = V2_HISTORY;
    if (stay && v2_set(p, v, k, YSIG_DETECT_HOLD, stay, -1, P_SEC) < 0) return -1;
    {
        /* entry: now for a hit with no hold, arm with one (3.7); a hold of 0
         * ns after rounding is none (the loader's F26 rule too); a variable
         * hold chooses at each trial's start */
        bool lit = !stay || stay->kind == YEXP_V_NUMBER;
        double h = !stay ? 0 : (stay->flags & YEXP_VF_DURATION) ? (double)stay->i / 1e9 : stay->num;
        v->nodes[k].p[YSIG_DETECT_ENTRY] = lit && h < 0.5e-9 && h > -0.5e-9 ? YSIG_ENTRY_NOW : YSIG_ENTRY_ARM;
        if (!lit && v2_param_add(p, v, k, YSIG_DETECT_ENTRY, stay, -1, P_ENTRY) < 0) return -1;
    }
    return k;
}

/* A second detect with k's input, parameters and per-trial values. */
static int v2_hit_clone(pl_player* p, pl_v2* v, int k) {
    int k2, i, n = v->n_params;
    if (k < 0) return -1;
    k2 = v2_node(p, v, YSIG_OP_DETECT, -1, -1);
    if (k2 < 0) return -1;
    v->nodes[k2] = v->nodes[k];
    for (i = 0; i < n; i++)
        if (v->params[i].node == k && v2_param_add(p, v, k2, v->params[i].param, v->params[i].v, v->params[i].elem, v->params[i].how) < 0) return -1;
    return k2;
}

static int v2_test_detect(pl_player* p, pl_v2* v, int in, int test, int lane, int rate) {
    int k = v2_node(p, v, YSIG_OP_DETECT, in, -1);
    if (k < 0) return -1;
    v->nodes[k].lanes = 1;
    v->nodes[k].p[YSIG_DETECT_TEST] = test;
    v->nodes[k].p[YSIG_DETECT_LANE] = lane;
    v->nodes[k].p[YSIG_DETECT_START] = 0;
    v->nodes[k].p[YSIG_DETECT_REPEAT] = 1;
    v->nodes[k].p[YSIG_DETECT_OUT] = YSIG_OUT_TEST;
    v->nodes[k].p[YSIG_DETECT_AT] = rate;
    v->nodes[k].history = V2_HISTORY;
    return k;
}

/* A point key (center, about, origin): two parameters, each element a
 * literal or a variable (D27). */
static int v2_point(pl_player* p, pl_v2* v, int node, int px, int py, const yexp_value* pt) {
    if (!pt) return 0;
    if (pt->kind == YEXP_V_ARRAY && pt->n == 2) {
        if (v2_set(p, v, node, px, &pt->items[0], -1, P_NUM) < 0) return -1;
        return v2_set(p, v, node, py, &pt->items[1], -1, P_NUM);
    }
    /* a variable or a lookup that holds [x, y] */
    if (v2_set(p, v, node, px, pt, 0, P_NUM) < 0) return -1;
    return v2_set(p, v, node, py, pt, 1, P_NUM);
}

static int v2_gate_node(pl_player* p, pl_v2* v, const yexp_ref* from) {
    int k;
    if (from->kind == YEXP_REF_CONTROL) {
        int ci = v2_ctl_index_of(v, from->control);
        if (ci < 0 || v2_ctl_build(p, v, ci) < 0) return -1;
        if (v->ctl[ci].hit < 0) { pl_fail(p, "template '%s': '%s' has no hits for a from", v->def->name, from->name); return -1; }
        return v->ctl[ci].hit;
    }
    k = v2_node(p, v, YSIG_OP_CHANNEL, -1, -1);
    if (k < 0) return -1;
    v->nodes[k].history = V2_HISTORY;
    if (from->kind == YEXP_REF_STIMULUS) {
        int ch = pl_channel(p, p->def->stimuli[from->index].name, NULL);
        if (ch < 0) { pl_fail(p, "template '%s': the stimulus '%s' has no visible channel", v->def->name, from->name); return -1; }
        v->nodes[k].p[0] = ch;
        return k;
    }
    if (from->kind == YEXP_REF_STATE && v->gate_ch && v->gate_ch[from->index] >= 0) {
        v->nodes[k].p[0] = v->gate_ch[from->index];
        return k;
    }
    pl_fail(p, "template '%s': a peak's from '%s' is no control, stimulus or state", v->def->name, from->name ? from->name : "?");
    return -1;
}

static int v2_ctl_build(pl_player* p, pl_v2* v, int ci) {
    v2_ctl* c = &v->ctl[ci];
    const yexp_control* d = c->def;
    if (c->val >= 0 || c->test >= 0 || c->hit >= 0 || c->emit >= 0 || c->of >= 0) return 0;
    if (c->building) { pl_fail(p, "template '%s': the control '%s' reads itself", v->def->name, d->name); return -1; }
    c->building = true;
    switch (d->kind) {
    case YEXP_CURSOR: {
        int in = -1, k;
        if (d->of.kind == YEXP_REF_ROLE) in = v2_ref_node(p, v, &d->of, true);
        else if (d->n_items > 0) {
            int m, ins[4];
            for (m = 0; m < d->n_items && m < 4; m++) if ((ins[m] = v2_ref_node(p, v, &d->items[m], true)) < 0) return -1;
            in = v2_node(p, v, YSIG_OP_MEAN, ins[0], d->n_items > 1 ? ins[1] : -1);
            if (in < 0) return -1;
            for (m = 2; m < d->n_items && m < 4; m++) v->nodes[in].in[m] = (int16_t)ins[m];
            for (m = 0; m < d->n_items && m < 4; m++) v->nodes[in].p[m] = 1;
        } else {
            int x = v2_ref_node(p, v, &d->x, false), y;
            if (x < 0) return -1;
            y = v2_ref_node(p, v, &d->y, false);
            if (y < 0) return -1;
            in = v2_node(p, v, YSIG_OP_JOIN, x, y);
        }
        if (in < 0) return -1;
        k = v2_node(p, v, YSIG_OP_AFFINE, in, -1);
        if (k < 0) return -1;
        v->nodes[k].history = V2_HISTORY;
        if (v->n_xform < 8) {
            v->xform[v->n_xform].node = k;
            v->xform[v->n_xform].v = v2_key(d, "transform");
            v->xform[v->n_xform].about = v2_key(d, "about");
            v->n_xform++;
        }
        c->val = k;
        if (d->freeze_at.kind == YEXP_REF_CONTROL) {
            int fi = v2_ctl_index_of(v, d->freeze_at.control), l;
            if (fi < 0 || v2_ctl_build(p, v, fi) < 0) return -1;
            l = v2_node(p, v, YSIG_OP_LATCH, k, v->ctl[fi].hit);
            if (l < 0) return -1;
            v->nodes[l].history = V2_HISTORY;
            c->val = l;
        }
        break;
    }
    case YEXP_REGION: {
        int of = v2_ref_node(p, v, &d->of, true), test_in, lane = -1, dt, k, out;
        const yexp_value* sb = v2_given(d, "speed_below") ? v2_key(d, "speed_below") : NULL;
        const yexp_value* outside = v2_key(d, "outside");
        int rate = v2_rate(d);
        int tst = outside && outside->kind == YEXP_V_BOOL && outside->i ? YSIG_TEST_GE : YSIG_TEST_LE;
        if (of < 0) return -1;
        v->nodes[of].history = V2_HISTORY;
        c->of = of;
        test_in = of;
        if (d->center.kind != YEXP_REF_NONE) {
            /* D28: a center that moves: the distance, then a test on its lane */
            int cn = v2_ref_node(p, v, &d->center, true);
            if (cn < 0) return -1;
            test_in = v2_node(p, v, YSIG_OP_DISTANCE, of, cn);
            if (test_in < 0) return -1;
            v->nodes[test_in].lanes = 1;
            v->nodes[test_in].history = V2_HISTORY;
            lane = 0;
        }
        dt = v2_test_detect(p, v, test_in, tst, lane, rate);
        if (dt < 0) return -1;
        if (v2_set(p, v, dt, YSIG_DETECT_LEVEL, v2_key(d, "radius"), -1, P_NUM) < 0) return -1;
        if (lane < 0 && v2_point(p, v, dt, YSIG_DETECT_A_X, YSIG_DETECT_A_Y, v2_key(d, "center")) < 0) return -1;
        out = dt;
        if (sb) {
            int sp = v2_node(p, v, YSIG_OP_SPEED, of, -1), st;
            if (sp < 0) return -1;
            v->nodes[sp].history = V2_HISTORY;
            if (v2_set(p, v, sp, YSIG_SPEED_LAG, v2_key(d, "speed_lag_s"), -1, P_SEC) < 0) return -1;
            st = v2_test_detect(p, v, sp, YSIG_TEST_LE, 0, rate);
            if (st < 0) return -1;
            if (v2_set(p, v, st, YSIG_DETECT_LEVEL, sb, -1, P_NUM) < 0) return -1;
            out = v2_node(p, v, YSIG_OP_ALL, dt, st);
            if (out < 0) return -1;
            v->nodes[out].history = V2_HISTORY;
            k = v2_hit_detect(p, v, out, v2_key(d, "stay_s"), rate);
            if (k < 0) return -1;
            v->nodes[k].p[YSIG_DETECT_TEST] = YSIG_TEST_GE;
            v->nodes[k].p[YSIG_DETECT_LANE] = 0;
            v->nodes[k].p[YSIG_DETECT_LEVEL] = 0.5;
        } else {
            k = v2_hit_detect(p, v, test_in, v2_key(d, "stay_s"), rate);
            if (k < 0) return -1;
            v->nodes[k].p[YSIG_DETECT_TEST] = tst;
            v->nodes[k].p[YSIG_DETECT_LANE] = lane;
            if (v2_set(p, v, k, YSIG_DETECT_LEVEL, v2_key(d, "radius"), -1, P_NUM) < 0) return -1;
            if (lane < 0 && v2_point(p, v, k, YSIG_DETECT_A_X, YSIG_DETECT_A_Y, v2_key(d, "center")) < 0) return -1;
        }
        c->test = out;
        c->hit = k;
        break;
    }
    case YEXP_DISTANCE: {
        int of = v2_ref_node(p, v, &d->of, true), k, s;
        if (of < 0) return -1;
        c->of = of;
        k = v2_node(p, v, YSIG_OP_DISTANCE, of, -1);
        if (k < 0) return -1;
        v->nodes[k].lanes = 1;
        if (v2_point(p, v, k, YSIG_DISTANCE_POINT_X, YSIG_DISTANCE_POINT_Y, v2_key(d, "center")) < 0) return -1;
        s = v2_node(p, v, YSIG_OP_SCALE, k, -1);
        if (s < 0) return -1;
        v->nodes[s].history = V2_HISTORY;
        if (v2_set(p, v, s, YSIG_SCALE_K_X, v2_key(d, "scale"), -1, P_NUM) < 0) return -1;
        c->val = s;
        break;
    }
    case YEXP_MOVEMENT: {
        int of = v2_ref_node(p, v, &d->of, true), k;
        if (of < 0) return -1;
        c->of = of;
        k = v2_node(p, v, YSIG_OP_EMIT, of, -1);
        if (k < 0) return -1;
        v->nodes[k].p[YSIG_EMIT_KIND] = 7;
        v->nodes[k].p[YSIG_EMIT_CONTROL] = YIN_AXIS_POSITION;
        c->emit = k;
        break;
    }
    case YEXP_PEAK: {
        int of = v2_ref_node(p, v, &d->of, true), sp, k, gate = -1;
        if (of < 0) return -1;
        c->of = of;
        sp = v2_node(p, v, YSIG_OP_SPEED, of, -1);
        if (sp < 0) return -1;
        if (v2_set(p, v, sp, YSIG_SPEED_LAG, v2_key(d, "lag_s"), -1, P_SEC) < 0) return -1;
        if (d->from.kind != YEXP_REF_NONE && (gate = v2_gate_node(p, v, &d->from)) < 0) return -1;
        k = v2_node(p, v, YSIG_OP_PEAK, sp, gate);
        if (k < 0) return -1;
        v->nodes[k].p[YSIG_PEAK_LANE] = 0;
        v->nodes[k].history = V2_HISTORY;
        c->val = k;
        if (d->freeze_at.kind == YEXP_REF_CONTROL) {
            int fi = v2_ctl_index_of(v, d->freeze_at.control), l;
            if (fi < 0 || v2_ctl_build(p, v, fi) < 0) return -1;
            l = v2_node(p, v, YSIG_OP_LATCH, k, v->ctl[fi].hit);
            if (l < 0) return -1;
            v->nodes[l].history = V2_HISTORY;
            c->val = l;
        }
        break;
    }
    case YEXP_MOVING_TARGET: {
        int k = v2_node(p, v, YSIG_OP_SINES, -1, -1), lane, j, n = d->sx.n;
        if (k < 0) return -1;
        v->nodes[k].lanes = 2;
        v->nodes[k].history = V2_HISTORY;
        v->nodes[k].p[YSIG_SINES_N] = n;
        if (v2_set(p, v, k, YSIG_SINES_RAMP, v2_key(d, "ramp_s"), -1, P_SEC) < 0) return -1;
        if (v->nd + 2 * n * 3 > v->dcap) { pl_fail(p, "template '%s': too many sines", v->def->name); return -1; }
        v->nodes[k].n_data = 2 * n * 3;
        v->nodes[k].data = v->data + v->nd;
        for (lane = 0; lane < 2; lane++) {
            const yexp_sines* sn = lane ? &d->sy : &d->sx;
            for (j = 0; j < n; j++) {
                int base = (lane * n + j) * 3;
                if (v2_set(p, v, k, -1 - base, sn->amplitudes, j, P_NUM) < 0) return -1;
                if (v2_set(p, v, k, -1 - (base + 1), sn->freqs_hz, j, P_NUM) < 0) return -1;
                if (v2_set(p, v, k, -1 - (base + 2), sn->phases, j, P_NUM) < 0) return -1;
            }
        }
        v->nd += 2 * n * 3;
        c->val = k;
        if (d->from.kind == YEXP_REF_STATE) v->base_state = d->from.index;
        break;
    }
    case YEXP_TRACE:
        c->of = -2;   /* built with the traces */
        break;
    case YEXP_SIGNAL_LOST: {
        /* 3.7: silent(of) after after_s; hits when it becomes true */
        int of = v2_ref_node(p, v, &d->of, true), sl, k;
        if (of < 0) return -1;
        c->of = of;
        sl = v2_node(p, v, YSIG_OP_SILENT, of, -1);
        if (sl < 0) return -1;
        v->nodes[sl].history = V2_HISTORY;
        if (v2_set(p, v, sl, YSIG_SILENT_AFTER, v2_key(d, "after_s"), -1, P_SEC) < 0) return -1;
        k = v2_hit_detect(p, v, sl, NULL, 0);
        if (k < 0) return -1;
        v->nodes[k].p[YSIG_DETECT_TEST] = YSIG_TEST_GE;
        v->nodes[k].p[YSIG_DETECT_LANE] = 0;
        v->nodes[k].p[YSIG_DETECT_LEVEL] = 0.5;
        c->val = sl;
        c->test = sl;
        c->hit = k;
        break;
    }
    default:
        pl_fail(p, "template '%s': the control '%s' has a kind this player does not lower", v->def->name, d->name);
        return -1;
    }
    /* a control that drives a stimulus (3.7: cursor, moving_target) */
    if ((d->kind == YEXP_CURSOR || d->kind == YEXP_MOVING_TARGET) && d->stimulus.kind == YEXP_REF_STIMULUS) {
        int b = v2_node(p, v, YSIG_OP_BIND, c->val, -1);
        if (b < 0) return -1;
        v->nodes[b].p[YSIG_BIND_CHANNEL] = v->n_slots;
        c->slot = v->n_slots;
        v->n_slots += 2;
    }
    c->building = false;
    return 0;
}

/* --- forms in the graph (3.10) ------------------------------------------ */

static int v2_name_node(pl_player* p, pl_v2* v, const char* name, int* lanes) {
    const yexp_sigrole* r = yexp_sigrole_get(p->def, name);
    const yexp_control* c;
    int n;
    if (r) {
        n = v2_role_node(p, v, (int)(r - p->def->sigroles));
        if (lanes) *lanes = r->lanes;
        return n;
    }
    c = yexp_control_get(p->def, v->def, name);
    if (c) {
        int ci = v2_ctl_index_of(v, c);
        if (ci < 0 || v2_ctl_build(p, v, ci) < 0) return -1;
        n = v->ctl[ci].val >= 0 ? v->ctl[ci].val : v->ctl[ci].test;
        if (lanes) *lanes = v->ctl[ci].val >= 0 ? c->lanes : 1;
        if (n >= 0) return n;
    }
    pl_fail(p, "template '%s': '%s' has no value for a form", v->def->name, name);
    return -1;
}

static int v2_test_node(pl_player* p, pl_v2* v, const yexp_value* t) {
    if (t->kind == YEXP_V_TEST) {
        const yexp_control* c = yexp_control_get(p->def, v->def, t->str);
        int ci = c ? v2_ctl_index_of(v, c) : -1;
        if (ci < 0 || v2_ctl_build(p, v, ci) < 0 || v->ctl[ci].test < 0) {
            if (!p->failed) pl_fail(p, "template '%s': '%s' has no test", v->def->name, t->str);
            return -1;
        }
        return v->ctl[ci].test;
    }
    if (t->kind == YEXP_V_SIGNAL && t->by) {
        /* {"signal": N, "above": L}: a detect ge L, a value equal to L is above */
        int lanes = 1, n = v2_name_node(p, v, t->str, &lanes), k;
        if (n < 0) return -1;
        if (t->i >= 0) n = v2_lane(p, v, n, (int)t->i);
        k = v2_test_detect(p, v, n, YSIG_TEST_GE, 0, 0);
        if (k < 0) return -1;
        if (v2_set(p, v, k, YSIG_DETECT_LEVEL, t->by, -1, P_NUM) < 0) return -1;
        return k;
    }
    pl_fail(p, "template '%s': a when's test is neither a control nor a signal with above", v->def->name);
    return -1;
}

static int v2_form_node(pl_player* p, pl_v2* v, const yexp_value* f, int comp, int prop, int form) {
    if (f->kind == YEXP_V_SIGNAL && !f->by) {
        int lanes = 1, n = v2_name_node(p, v, f->str, &lanes);
        if (n < 0) return -1;
        if (f->i >= 0) return v2_lane(p, v, n, (int)f->i);
        /* reading F10: a value of the place's lanes; a color takes lane comp */
        if (lanes > 1) return v2_lane(p, v, n, comp >= 0 ? comp : 0);
        return n;
    }
    if (f->kind == YEXP_V_WHEN) {
        int s = v2_test_node(p, v, f->by), a, b, m;
        if (s < 0) return -1;
        a = v2_form_node(p, v, &f->items[0], comp, prop, form);
        if (a < 0) return -1;
        b = v2_form_node(p, v, &f->items[1], comp, prop, form);
        if (b < 0) return -1;
        m = v2_node(p, v, YSIG_OP_MUX, a, b);
        if (m < 0) return -1;
        v->nodes[m].in[2] = (int16_t)s;
        v->nodes[m].history = V2_HISTORY;
        return m;
    }
    {
        /* a literal, a variable or a lookup: its channel, set when the step lands */
        int i, k;
        for (i = 0; i < v->n_vch; i++)
            if (v->vch[i].v == f && v->vch[i].comp == comp && v->vch[i].prop == prop && v->vch[i].form == form) break;
        if (i == v->n_vch) { pl_fail(p, "template '%s': a form's value has no channel", v->def->name); return -1; }
        k = v2_node(p, v, YSIG_OP_CHANNEL, -1, -1);
        if (k < 0) return -1;
        v->nodes[k].p[0] = v->vch[i].ch;
        v->nodes[k].history = V2_HISTORY;
        return k;
    }
}

static int v2_forms_graph(pl_player* p, pl_v2* v) {
    int i, j;
    for (i = 0; i < v->n_props; i++) {
        v2_prop* pr = &v->props[i];
        int chain = v2_node(p, v, YSIG_OP_CHANNEL, -1, -1), b;
        if (chain < 0) return -1;
        v->nodes[chain].p[0] = pr->ch_c;
        v->nodes[chain].history = V2_HISTORY;
        for (j = pr->n_forms - 1; j >= 0; j--) {
            int fv = v2_form_node(p, v, pr->forms[j].v, pr->comp, i, j), sel, m;
            if (fv < 0) return -1;
            sel = v2_node(p, v, YSIG_OP_CHANNEL, -1, -1);
            if (sel < 0) return -1;
            v->nodes[sel].p[0] = pr->forms[j].ch_sel;
            v->nodes[sel].history = V2_HISTORY;
            m = v2_node(p, v, YSIG_OP_MUX, fv, chain);
            if (m < 0) return -1;
            v->nodes[m].in[2] = (int16_t)sel;
            v->nodes[m].history = V2_HISTORY;
            chain = m;
        }
        b = v2_node(p, v, YSIG_OP_BIND, chain, -1);
        if (b < 0) return -1;
        v->nodes[b].p[YSIG_BIND_CHANNEL] = v->n_slots;
        pr->slot = v->n_slots++;
    }
    return 0;
}

/* The tween ops of a step list (TO and THEN, a color's three), into the
 * if's and unless's steps: a visit's bound for rule 8's stops. */
static int v2_tween_ops(const yexp_step* st, int n) {
    int i, k, c = 0;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        if (s->op == YEXP_IF || s->op == YEXP_UNLESS) c += v2_tween_ops(s->steps, s->n_steps);
        if (s->op != YEXP_TO && s->op != YEXP_THEN) continue;
        for (k = 0; k < s->n_props; k++) c += strcmp(s->props[k].key, "color") == 0 ? 3 : 1;
    }
    return c;
}

/* A property both tweened and set by a constant, in any of the template's
 * step lists: a timeline channel is driven by a track or by events, never
 * both (ysp/timeline.h), so version 1 refuses it in one table; across
 * states it is refused too, at load (reading F36). 1 when found. */
static int v2_mix_find(const yexp_step* st, int n, const yexp_step* const* all, const int* n_all, int lists, const char** stim, const char** prop) {
    int i, k, l, j;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        if (s->op == YEXP_IF || s->op == YEXP_UNLESS) {
            if (v2_mix_find(s->steps, s->n_steps, all, n_all, lists, stim, prop)) return 1;
            continue;
        }
        if (s->op != YEXP_TO && s->op != YEXP_THEN) continue;
        for (k = 0; k < s->n_props; k++)
            for (l = 0; l < lists; l++)
                for (j = 0; j < n_all[l]; j++) {
                    const yexp_step* q = &all[l][j];
                    int m;
                    if (q->op == YEXP_IF || q->op == YEXP_UNLESS) {
                        const yexp_step* sub[1] = { q->steps };
                        int nsub[1] = { q->n_steps };
                        yexp_step one = *s;
                        one.n_props = 1;
                        one.props = &s->props[k];
                        if (v2_mix_find(&one, 1, sub, nsub, 1, stim, prop)) return 1;
                        continue;
                    }
                    if (q->op != YEXP_SET || strcmp(q->stims[0], s->stims[0]) != 0) continue;
                    for (m = 0; m < q->n_props; m++)
                        if (strcmp(q->props[m].key, s->props[k].key) == 0 && !v2_is_form(q->props[m].value)) {
                            *stim = s->stims[0];
                            *prop = s->props[k].key;
                            return 1;
                        }
                }
    }
    return 0;
}

static int v2_template_checks(pl_player* p, pl_v2* v) {
    const yexp_template* t = v->def;
    /* a list per state, the events, the after: sized from the template, so
     * every state count the loader takes is taken here too */
    int cap = (t->states ? t->n_states : 1) + 1;
    const yexp_step** all = (const yexp_step**)pl_alloc(p, sizeof(yexp_step*) * (size_t)cap);
    int* n_all = (int*)pl_alloc(p, sizeof(int) * (size_t)cap);
    int lists = 0, i, most = 0;
    const char* stim = NULL, *prop = NULL;
    if (!all || !n_all) return -1;
    if (t->states)
        for (i = 0; i < t->n_states; i++) {
            int c = v2_tween_ops(t->states[i].steps, t->states[i].n_steps);
            if (c > most) most = c;
            all[lists] = t->states[i].steps;
            n_all[lists++] = t->states[i].n_steps;
        }
    else {
        most = v2_tween_ops(t->events, t->n_events);
        all[lists] = t->events;
        n_all[lists++] = t->n_events;
    }
    all[lists] = t->after;
    n_all[lists++] = t->n_after;
    for (i = 0; i < lists; i++)
        if (v2_mix_find(all[i], n_all[i], all, n_all, lists, &stim, &prop)) {
            pl_fail(p, "template '%s': '%s.%s' is tweened (to, then) in one step list and set in another; a property is driven by a tween or by sets, not both (as in one version 1 table)",
                    t->name, stim, prop);
            return -1;
        }
    v->cap_tw = most + 1;
    v->tw = (struct v2_tw*)pl_alloc(p, sizeof(struct v2_tw) * (size_t)v->cap_tw);
    v->frz = (struct v2_frz*)pl_alloc(p, sizeof(struct v2_frz) * (size_t)v->cap_tw);
    return v->tw && v->frz ? 0 : -1;
}

/* The nodes a column's form reads at the row's fill (3.15): made at load,
 * so the fill only reads them. */
static int v2_column_nodes(pl_player* p, pl_v2* v, const yexp_value* f) {
    if (!f) return 0;
    if (f->kind == YEXP_V_SIGNAL) return v2_name_node(p, v, f->str, NULL) < 0 ? -1 : 0;
    if (f->kind == YEXP_V_WHEN) {
        if (f->by && f->by->kind == YEXP_V_SIGNAL && v2_name_node(p, v, f->by->str, NULL) < 0) return -1;
        if (v2_column_nodes(p, v, &f->items[0]) < 0) return -1;
        return v2_column_nodes(p, v, &f->items[1]);
    }
    return 0;
}

/* --- traces (3.7, 3.15) --------------------------------------------------- */

static int v2_traces_build(pl_player* p, pl_v2* v) {
    int i, n = 0;
    for (i = 0; i < v->n_ctl; i++) if (v->ctl[i].def->kind == YEXP_TRACE) n++;
    if (!n) return 0;
    v->traces = (v2_trace*)pl_alloc(p, sizeof(v2_trace) * (size_t)n);
    if (!v->traces) return -1;
    for (i = 0; i < v->n_ctl; i++) {
        const yexp_control* d = v->ctl[i].def;
        v2_trace* tr;
        const yexp_value* rate;
        int k, lanes = 0;
        if (d->kind != YEXP_TRACE) continue;
        /* a waiting frame row names its trace as template x 64 + trace */
        if (v->n_traces >= 64) { pl_fail(p, "template '%s': more than 63 traces", v->def->name); return -1; }
        tr = &v->traces[v->n_traces++];
        memset(tr, 0, sizeof *tr);
        tr->def = d;
        tr->stream = -1;
        rate = v2_key(d, "rate");
        tr->frame = rate && rate->kind == YEXP_V_STRING && strcmp(rate->str, "frame") == 0;
        if (d->n_items > 8) { pl_fail(p, "template '%s': trace '%s' has more than 8 items", v->def->name, d->name); return -1; }
        for (k = 0; k < d->n_items; k++) {
            int nd = v2_ref_node(p, v, &d->items[k], false);
            if (nd < 0) return -1;
            v->nodes[nd].history = V2_HISTORY;
            tr->item_node[k] = nd;
            tr->item_lanes[k] = d->items[k].lanes > 0 ? d->items[k].lanes : 1;
            lanes += tr->item_lanes[k];
        }
        tr->n_items = d->n_items;
        if (!tr->frame) {
            int j = tr->item_node[0], r;
            if (lanes > 4) { pl_fail(p, "template '%s': sample trace '%s' has %d lanes; a join takes 4", v->def->name, d->name, lanes); return -1; }
            if (tr->n_items > 1) {
                j = v2_node(p, v, YSIG_OP_JOIN, tr->item_node[0], tr->item_node[1]);
                if (j < 0) return -1;
                for (k = 2; k < tr->n_items; k++) v->nodes[j].in[k] = (int16_t)tr->item_node[k];
            }
            r = v2_node(p, v, YSIG_OP_RECORD, j, -1);
            if (r < 0) return -1;
            v->nodes[r].name = d->name;
            tr->join = j;
            tr->rec = r;
            tr->rt = (int64_t*)pl_alloc(p, sizeof(int64_t) * V2_TRACE_ROWS);
            tr->rv = (double*)pl_alloc(p, sizeof(double) * 4 * V2_TRACE_ROWS);
            tr->rtier = (uint8_t*)pl_alloc(p, V2_TRACE_ROWS);
            tr->rdev = (uint32_t*)pl_alloc(p, sizeof(uint32_t) * V2_TRACE_ROWS);
            if (!tr->rt || !tr->rv || !tr->rtier || !tr->rdev) return -1;
        }
        v->ctl[i].of = -2;
    }
    return 0;
}

/* A sample trace's rows, from the record node: kept for the trial, cut to
 * its window at the end (from and to are known then, 3.7). */
static void v2_on_row(void* user, const ysig_row* row) {
    pl_v2* v = (pl_v2*)user;
    int i;
    for (i = 0; i < v->n_traces; i++) {
        v2_trace* tr = &v->traces[i];
        int k;
        if (tr->frame || tr->rec != row->node) continue;
        /* the trial's rows: from its reset to the flush at its end (a row
         * comes when final, up to four frames late; between trials none) */
        if (!v->rows_open) return;
        if (tr->nrows >= V2_TRACE_ROWS) { tr->lost++; return; }
        tr->rt[tr->nrows] = row->value.t;
        for (k = 0; k < 4; k++) tr->rv[tr->nrows * 4 + k] = row->value.v[k];
        tr->rtier[tr->nrows] = row->value.tier;
        tr->rdev[tr->nrows] = row->value.device;
        tr->nrows++;
        return;
    }
}

/* --- load -------------------------------------------------------------- */

/* --test-sample-late: each role's reports reach the player that much after
 * their stamp. The player takes them in the order they arrive, as a real
 * run's queue gives them; a role's own order is its stamps'. */
static int v2_smp_cmp(const void* a, const void* b) {
    const struct v2_sample* x = (const struct v2_sample*)a;
    const struct v2_sample* y = (const struct v2_sample*)b;
    if (x->arr != y->arr) return x->arr < y->arr ? -1 : 1;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;   /* the file's order: stamps never go back */
    return x->role - y->role;
}
static int v2_samples_late(pl_player* p) {
    pl_v2_session* S = p->v2s;
    int i, k;
    if (p->args.n_lates == 0) return 0;
    for (k = 0; k < p->args.n_lates; k++) {
        const yexp_sigrole* sr = yexp_sigrole_get(p->def, p->args.lates[k].name);
        int64_t late = (int64_t)llround(atof(p->args.lates[k].value) * 1e9);
        if (!sr) {
            pl_fail(p, "--test-sample-late %s: the definition has no signal role '%s'", p->args.lates[k].name, p->args.lates[k].name);
            p->bad_arg = true;
            return -1;
        }
        for (i = 0; i < S->n_smp; i++)
            if (S->smp[i].role == (int)(sr - p->def->sigroles)) S->smp[i].arr = S->smp[i].t + late;
    }
    if (S->n_smp > 1) qsort(S->smp, (size_t)S->n_smp, sizeof *S->smp, v2_smp_cmp);
    return 0;
}

static int v2_samples_load(pl_player* p) {
    pl_v2_session* S = p->v2s;
    FILE* f;
    char line[512];
    int cap = 0, ln = 0;
    if (!p->args.samples_path) return 0;
    f = fopen(p->args.samples_path, "r");
    if (!f) { pl_fail(p, "--samples '%s': cannot read it", p->args.samples_path); p->bad_arg = true; return -1; }
    while (fgets(line, sizeof line, f)) {
        char role[128];
        double t, x, y;
        int r;
        const yexp_sigrole* sr;
        ln++;
        if (ln == 1 && strncmp(line, "t_s", 3) == 0) continue;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%lf,%127[^,],%lf,%lf", &t, role, &x, &y) != 4 || !(t >= 0)) {
            fclose(f);
            pl_fail(p, "--samples '%s', line %d: expected t_s,role,x,y", p->args.samples_path, ln);
            p->bad_arg = true;
            return -1;
        }
        /* a time past int64 ns, or a value past a float, would wrap or
         * turn infinite: refused, with the bounds */
        if (!(t <= V2_SAMPLE_MAX_S) || !isfinite(x) || !isfinite(y) || fabs(x) > V2_SAMPLE_MAX_V || fabs(y) > V2_SAMPLE_MAX_V) {
            fclose(f);
            pl_fail(p, "--samples line %d: t_s must be 0 to %g s, and x and y finite and within %g", ln, V2_SAMPLE_MAX_S, V2_SAMPLE_MAX_V);
            p->bad_arg = true;
            return -1;
        }
        sr = yexp_sigrole_get(p->def, role);
        if (!sr) { fclose(f); pl_fail(p, "--samples line %d: '%s' is no signal role of the definition", ln, role); p->bad_arg = true; return -1; }
        r = (int)(sr - p->def->sigroles);
        if (S->n_smp == cap) {
            int nc = cap ? cap * 2 : 65536;
            struct v2_sample* q = (struct v2_sample*)realloc(S->smp, sizeof(struct v2_sample) * (size_t)nc);
            if (!q) { fclose(f); pl_fail(p, "--samples: out of memory"); return -1; }
            S->smp = q;
            cap = nc;
        }
        S->smp[S->n_smp].t = (int64_t)llround(t * 1e9);
        S->smp[S->n_smp].x = (float)x;
        S->smp[S->n_smp].y = (float)y;
        S->smp[S->n_smp].role = r;
        if (S->n_smp && S->smp[S->n_smp].t < S->smp[S->n_smp - 1].t) {
            fclose(f);
            pl_fail(p, "--samples line %d: t_s goes back (the file is in time order)", ln);
            p->bad_arg = true;
            return -1;
        }
        S->smp[S->n_smp].arr = S->smp[S->n_smp].t;
        S->n_smp++;
    }
    fclose(f);
    return v2_samples_late(p);
}

/* 0.12.0: the roles that reach a detect or an emit (a control's or a
 * movement's), and whether one of them has a lateness bound above 0: the
 * template's horizon G is the least complete-to time over them */
static int v2_feeds(pl_player* p, pl_v2* v) {
    pl_v2_session* S = p->v2s;
    int r, i, k, more, n = p->def->n_sigroles;
    unsigned char* in_d = (unsigned char*)calloc((size_t)v->nn + 1, 1);
    v->feeds = (bool*)pl_alloc(p, sizeof(bool) * (size_t)(n + 1));
    if (!in_d || !v->feeds) { free(in_d); if (v->feeds) pl_fail(p, "out of memory"); return -1; }
    v->late = false;
    for (r = 0; r < n; r++) {
        memset(in_d, 0, (size_t)v->nn);
        for (i = 0; i < v->nn; i++) in_d[i] = v->nodes[i].op == YSIG_OP_ROLE && (int)v->nodes[i].p[0] == r;
        for (more = 1; more;) {
            more = 0;
            for (i = 0; i < v->nn; i++) {
                if (in_d[i]) continue;
                for (k = 0; k < 4; k++)
                    if (v->nodes[i].in[k] >= 0 && v->nodes[i].in[k] < v->nn && in_d[v->nodes[i].in[k]]) { in_d[i] = 1; more = 1; break; }
            }
        }
        v->feeds[r] = false;
        for (i = 0; i < v->nn; i++)
            if (in_d[i] && (v->nodes[i].op == YSIG_OP_DETECT || v->nodes[i].op == YSIG_OP_EMIT)) v->feeds[r] = true;
        if (v->feeds[r] && S->bnd[r] > 0) v->late = true;
    }
    free(in_d);
    return 0;
}

/* The horizon G: input is complete to it for every role that feeds the
 * template's controls and movements (docs/player.md, "Terms"). Per role
 * (one device in --sim), C = max(S - 1, min(L, P - B)): every report
 * stamped at or before P - B reached a poll by P, and a device's stamps
 * do not go back. P and L are the last whole feed's: a poll fed in
 * parts claims only its newest stamps until its end. */
static int64_t v2_horizon(const pl_player* p, const pl_v2* v) {
    const pl_v2_session* S = p->v2s;
    int64_t g, c;
    int r;
    if (!S->polled) return INT64_MIN;
    g = S->poll < S->plim ? S->poll : S->plim;
    for (r = 0; r < p->def->n_sigroles; r++) {
        if (!v->feeds || !v->feeds[r]) continue;
        c = S->poll - S->bnd[r];
        if (c > S->plim) c = S->plim;
        if (S->newest[r] != INT64_MIN && S->newest[r] - 1 > c) c = S->newest[r] - 1;
        if (c < g) g = c;
    }
    return g;
}
/* The horizon over every signal role, fed or not: a barrier (a flush, a
 * reset) drops a report of any device stamped before it, so the trial's
 * end waits on all of them, not only the template's (docs/player.md, "The
 * trial's end"). P when no role has a bound above 0. */
static int64_t v2_horizon_all(const pl_player* p) {
    const pl_v2_session* S = p->v2s;
    int64_t g, c;
    int r;
    if (!S->polled) return INT64_MIN;
    g = S->poll < S->plim ? S->poll : S->plim;
    for (r = 0; r < p->def->n_sigroles; r++) {
        c = S->poll - S->bnd[r];
        if (c > S->plim) c = S->plim;
        if (S->newest[r] != INT64_MIN && S->newest[r] - 1 > c) c = S->newest[r] - 1;
        if (c < g) g = c;
    }
    return g;
}

/* What a drain hands out: up to G when the template waits; else all, as
 * 0.11.0 (every bound 0: nothing waits) */
static int64_t v2_drain_to(const pl_player* p, const pl_v2* v) {
    return v->late ? v2_horizon(p, v) : INT64_MAX;
}

/* N1 (docs/player.md): a hit of an arm made before a take, or a derived
 * event no arm since the last drain explains, at or before a horizon the
 * player already used: a report came later than its role's bound, and
 * the take may be wrong. Counted in late_beyond_bound; the guard stops. */
static int64_t v2_anchor(pl_player* p);
static int v2_beyond(pl_player* p, const pl_v2* v, const char* what, int64_t t, int64_t g) {
    p->v2s->beyond_take++;
    pl_fail(p, "template '%s': internal guard: %s at %.9f s came out after the player used the horizon %.9f s: a report came later than its role's "
            "lateness bound (late_beyond_bound)", v->def->name, what, (double)(t - v2_anchor(p)) / 1e9, (double)(g - v2_anchor(p)) / 1e9);
    return -1;
}

/* The --sim adapter of each signal role: a raw mouse's counts integrated
 * at --sim-gain units per count (3.19 item 3; G18 has no rig block). */
/* sig.h's window in input time (catchup_ns): the lateness it absorbs. The
 * rig profile has no measured input lateness for a signal role (it has no
 * adapter block, G18), so 50 ms, the user's default (2026-11-15): a 16 ms
 * FTDI timer, a 20 ms LSL offset and a 60 Hz frame (docs/sig.md, "The
 * window in time"). With a measured lateness, larger when it needs (below). */
static int64_t v2_catchup_ns(const pl_player* p) {
    /* 0.12.0: an exit taken at the horizon arms the next state at the
     * exit's time, up to max B plus two frames before the newest input, and
     * the newest input can be up to catchup_ns / 8 plus a frame past the
     * newest checkpoint (sig.h takes one each catchup_ns / 8): the window
     * keeps about 7/8 of catchup_ns, so max B plus three frames, times 8/7
     * (docs/player.md; the coordinator's R4, 2026-11-15) */
    int64_t per = p->caps.period_ns > 0 ? p->caps.period_ns : 16666667;
    int64_t b = p->v2s ? ((p->v2s->bnd_max + 3 * per) * 8 + 6) / 7 : 0;
    return b > V2_CATCHUP_NS ? b : V2_CATCHUP_NS;
}

/* The window's logs (sig.h catchup_calls, catchup_writes): every role at
 * V2_WINDOW_HZ over the span the log holds, and a frame's own calls (the
 * frame, settles, arms, sets) for each frame in it. The log holds the calls
 * since the oldest checkpoint: sig.h takes one at a frame catchup_ns / 8
 * after the newest (s, whole frames) and drops the oldest when the next is
 * catchup_ns behind the newest, so the oldest is at most catchup_ns + 2 s
 * old, and at most 8 s with 8 images; one frame more for the calls up to
 * the next. A report writes at most one value to each ring in its role's
 * downstream set (sig.md, Footprint); a frame's calls at most one to each
 * ring. */
static void v2_window_size(pl_player* p, const pl_v2* v, int* calls, int* writes) {
    int64_t per = p->caps.period_ns > 0 ? p->caps.period_ns : 16666667;
    int64_t ns = v2_catchup_ns(p), sp = (ns / 8 + per - 1) / per * per;
    int64_t span = (ns + 2 * sp < V2_WINDOW_CKS * sp ? ns + 2 * sp : V2_WINDOW_CKS * sp) + per;
    int64_t frames = (span + per - 1) / per;
    int64_t reps = (V2_WINDOW_HZ * span + 999999999) / 1000000000, c, w = 0, rings = 0;
    int r, i, roles = p->def->n_sigroles > 2 ? p->def->n_sigroles : 2;
    unsigned char* in_d = (unsigned char*)calloc((size_t)v->nn + 1, 1);
    for (i = 0; i < v->nn; i++) rings += v->nodes[i].history > 0;
    for (r = 0; r < p->def->n_sigroles; r++) {
        int64_t wr = 0, more = 1;
        int k;
        for (k = 0; k < v->adapters[r].n_nodes; k++) wr += v->adapters[r].nodes[k].history > 0;
        if (in_d) {
            memset(in_d, 0, (size_t)v->nn);
            for (i = 0; i < v->nn; i++) in_d[i] = v->nodes[i].op == YSIG_OP_ROLE && (int)v->nodes[i].p[0] == r;
            while (more) {
                more = 0;
                for (i = 0; i < v->nn; i++) {
                    if (in_d[i]) continue;
                    for (k = 0; k < 4; k++)
                        if (v->nodes[i].in[k] >= 0 && v->nodes[i].in[k] < v->nn && in_d[v->nodes[i].in[k]]) { in_d[i] = 1; more = 1; break; }
                }
            }
            for (i = 0; i < v->nn; i++) wr += in_d[i] && v->nodes[i].history > 0;
        } else wr += rings;
        if (wr > w) w = wr;
    }
    free(in_d);
    c = reps * roles + frames * V2_WINDOW_FRAME_CALLS;
    if (c < 1024) c = 1024;   /* 0.10.0's, the floor */
    if (p->v2s && p->v2s->bnd_max > 0) {
        /* sig.h's queues hold YSIG_QUEUE_CAP + catchup_calls: the hits and
         * derived events that max B plus a frame can make, each detect
         * and emit at V2_WINDOW_HZ, wait in them (M2) */
        int64_t q = 0, each = (V2_WINDOW_HZ * (p->v2s->bnd_max + per) + 999999999) / 1000000000;
        for (i = 0; i < v->nn; i++) q += (v->nodes[i].op == YSIG_OP_DETECT || v->nodes[i].op == YSIG_OP_EMIT) ? each : 0;
        if (q > c) c = q;
    }
    *calls = (int)c;
    c = reps * roles * (w > 0 ? w : 1) + frames * V2_WINDOW_FRAME_CALLS * (rings > 0 ? rings : 1);
    if (c < 16384) c = 16384;
    *writes = (int)c;
}

static int v2_adapters(pl_player* p, pl_v2* v) {
    int i, n = p->def->n_sigroles;
    pl_v2_session* S = p->v2s;
    v->roles = (ysig_role*)pl_alloc(p, sizeof(ysig_role) * (size_t)(n + 1));
    v->adapters = (ysig_adapter*)pl_alloc(p, sizeof(ysig_adapter) * (size_t)(n + 1));
    v->ad_nodes = (ysig_node*)pl_alloc(p, sizeof(ysig_node) * 2 * (size_t)(n + 1));
    if (!v->roles || !v->adapters || !v->ad_nodes) return -1;
    for (i = 0; i < n; i++) {
        const yexp_sigrole* r = &p->def->sigroles[i];
        static const int ty[] = { YSIG_TYPE_POSITION2D, YSIG_TYPE_POSITION2D_RELATIVE, YSIG_TYPE_POSITION3D, YSIG_TYPE_SCALAR, YSIG_TYPE_GAZE };
        static const int un[] = { YSIG_UNITS_DISPLAY_PX, YSIG_UNITS_DISPLAY_CM, YSIG_UNITS_DISPLAY_DEG, YSIG_UNITS_WORKSPACE_CM, YSIG_UNITS_DEVICE };
        ysig_node* an = &v->ad_nodes[2 * i];
        v->roles[i].name = r->name;
        v->roles[i].type = (uint8_t)ty[r->type];
        v->roles[i].units = (uint8_t)un[r->units];
        v->roles[i].allow_relative = r->allow_relative;
        if (!(r->type == YEXP_SIG_POSITION2D_RELATIVE || (r->type == YEXP_SIG_POSITION2D && r->allow_relative))) {
            /* an absolute role: its samples are positions in the role's
             * units, times --sim-gain (1 by default), a pen's events */
            an[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
            an[0].lanes = (uint8_t)r->lanes;
            an[0].p[YSIG_SOURCE_KIND] = YIN_KIND_PEN;
            an[0].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_POSITION;
            an[0].p[YSIG_SOURCE_DEVICE] = i + 1;
            an[0].history = V2_HISTORY;
            an[1] = ysig_node_of(YSIG_OP_SCALE, 0, -1);
            an[1].p[YSIG_SCALE_K_X] = an[1].p[YSIG_SCALE_K_Y] = an[1].p[YSIG_SCALE_K_Z] = S->gain[i] > 0 ? S->gain[i] : 1;
            an[1].history = V2_HISTORY;
            memset(&v->adapters[i], 0, sizeof v->adapters[i]);
            v->adapters[i].role = r->name;
            v->adapters[i].name = "sim positions";
            v->adapters[i].type = (uint8_t)ty[r->type];
            v->adapters[i].units = (uint8_t)un[r->units];
            v->adapters[i].nodes = an;
            v->adapters[i].n_nodes = 2;
            continue;
        }
        an[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
        an[0].lanes = 2;
        an[0].p[YSIG_SOURCE_KIND] = YIN_KIND_MOUSE;
        an[0].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_DELTA;
        an[0].p[YSIG_SOURCE_DEVICE] = i + 1;
        an[0].p[YSIG_SOURCE_TIER] = yin_raw_mouse_source().tier;
        an[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        an[1].p[YSIG_INTEGRATE_GAIN_X] = an[1].p[YSIG_INTEGRATE_GAIN_Y] = S->gain[i];
        an[1].history = V2_HISTORY;
        memset(&v->adapters[i], 0, sizeof v->adapters[i]);
        v->adapters[i].role = r->name;
        v->adapters[i].name = "sim raw mouse";
        v->adapters[i].type = YSIG_TYPE_POSITION2D_RELATIVE;
        v->adapters[i].units = (uint8_t)un[r->units];
        v->adapters[i].nodes = an;
        v->adapters[i].n_nodes = 2;
    }
    return 0;
}

static int v2_collector(pl_player* p, pl_v2* v, pl_tmpl* t) {
    const yexp_response* r = v->def->response;
    yrsp_desc rd;
    yrsp_choice* ch;
    int i;
    if (!r || !r->signal_choices) return 0;
    if (!p->collectors) {
        p->collectors = (yrsp_collector*)pl_alloc(p, sizeof(yrsp_collector) * (size_t)(p->def->n_templates + 1));
        if (!p->collectors) return -1;
    }
    t->collector = &p->collectors[p->n_collectors++];
    t->n_choices = r->n_choices;
    t->choice_keys = (const char**)pl_alloc(p, sizeof(char*) * (size_t)(r->n_choices + 1));
    t->choice_codes = (uint32_t*)pl_alloc(p, sizeof(uint32_t) * (size_t)(r->n_choices + 1));
    ch = (yrsp_choice*)pl_alloc(p, sizeof(yrsp_choice) * (size_t)(r->n_choices + 1));
    if (!t->choice_keys || !t->choice_codes || !ch) return -1;
    t->role = -1;
    v->resp_from_kind = YEXP_REF_NONE;
    for (i = 0; i < r->n_choices; i++) {
        const yexp_control* m = r->choices[i].signal;
        int ci = m ? v2_ctl_index_of(v, m) : -1;
        double onset;
        if (ci < 0 || v2_ctl_build(p, v, ci) < 0 || v->ctl[ci].emit < 0) { if (!p->failed) pl_fail(p, "template '%s': a signal choice names no movement", v->def->name); return -1; }
        memset(&p->env, 0, sizeof p->env);
        p->env.extreme = 0;
        {
            const yexp_value* on = v2_key(m, "onset");
            if (!on || on->kind != YEXP_V_NUMBER) {
                /* reading F19: the collector's level is fixed at open */
                pl_fail(p, "template '%s': movement '%s': onset must be a number here (the collector's level is fixed when it opens)", v->def->name, m->name);
                return -1;
            }
            onset = on->num;
        }
        v->ctl[ci].choice = i;
        t->choice_keys[i] = r->choices[i].key;
        memset(&ch[i], 0, sizeof ch[i]);
        ch[i].name = r->choices[i].name;
        ch[i].kind = YRSP_KIND_USER + 7;
        ch[i].device = (uint32_t)v->ctl[ci].emit;
        ch[i].control = YRSP_AXIS_POSITION;
        ch[i].cross = YRSP_CROSS_DISTANCE;
        ch[i].level = (float)onset;
        /* the window opens at the movements' from (D34 (c)); one from for all */
        if (i == 0) {
            v->resp_from_kind = m->from.kind;
            v->resp_from_index = m->from.index;
            v->resp_from_ctl = m->from.control;
        }
    }
    memset(&rd, 0, sizeof rd);
    rd.choices = ch;
    rd.n_choices = r->n_choices;
    rd.persist = !r->ends_trial;
    rd.max_responses = r->max_responses;
    {
        static yrsp_source src;
        src = yrsp_raw_mouse_source();
        rd.sources = &src;
        rd.n_sources = 1;
    }
    if (!yrsp_open(t->collector, &rd)) { pl_fail(p, "template '%s': %s", v->def->name, yrsp_error(t->collector)); return -1; }
    return 0;
}

static int v2_binds(pl_player* p, pl_v2* v) {
    int i, extra = 0;
    for (i = 0; i < v->n_ctl; i++)
        if ((v->ctl[i].def->kind == YEXP_CURSOR || v->ctl[i].def->kind == YEXP_MOVING_TARGET) && v->ctl[i].def->stimulus.kind == YEXP_REF_STIMULUS) extra += 2;
    v->binds = (ygfx_bind*)pl_alloc(p, sizeof(ygfx_bind) * (size_t)(p->n_binds + extra + 1));
    if (!v->binds) return -1;
    memcpy(v->binds, p->binds, sizeof(ygfx_bind) * (size_t)p->n_binds);
    v->n_binds = p->n_binds;
    for (i = 0; i < p->n_channels; i++)
        if (p->channels[i].prop && p->channels[i].prop[0] == '\x01') {
            v->binds[i].stim = NULL;
            v->binds[i].group = NULL;
            v->binds[i].field = &p->v2s->dummy;
        }
    /* a property that forms drive: the graph's channel, not the timeline's */
    for (i = 0; i < v->n_props; i++) v->binds[v->props[i].ch_c].channel = (uint16_t)(p->n_channels + v->props[i].slot);
    for (i = 0; i < v->n_ctl; i++) {
        const yexp_control* d = v->ctl[i].def;
        pl_stim* s;
        int a;
        if (!((d->kind == YEXP_CURSOR || d->kind == YEXP_MOVING_TARGET) && d->stimulus.kind == YEXP_REF_STIMULUS)) continue;
        s = &p->stims[d->stimulus.index];
        for (a = 0; a < 2; a++) {
            int ch = pl_channel(p, s->def->name, a ? "y" : "x");
            ygfx_bind* b = ch >= 0 ? &v->binds[ch] : &v->binds[v->n_binds++];
            memset(b, 0, sizeof *b);
            b->field = a ? &s->gs.y : &s->gs.x;
            b->channel = (uint16_t)(p->n_channels + v->ctl[i].slot + a);
        }
    }
    return 0;
}

int pl_v2_load(pl_player* p) {
    pl_v2_session* S = p->v2s;
    int ti, i, max_slots = 0;
    /* a --sim-gain for no signal role, or samples nothing reads, would be
     * a silent no-op: refused by name */
    for (i = 0; i < p->args.n_gains; i++)
        if (!yexp_sigrole_get(p->def, p->args.gains[i].name)) {
            pl_fail(p, "--sim-gain %s: the definition has no signal role '%s'", p->args.gains[i].name, p->args.gains[i].name);
            p->bad_arg = true;
            return -1;
        }
    if (p->args.samples_path && p->def->n_sigroles == 0) {
        pl_fail(p, "--samples: the definition has no signal role to feed");
        p->bad_arg = true;
        return -1;
    }
    if (!S) return 0;
    S->gain = (double*)pl_alloc(p, sizeof(double) * (size_t)(p->def->n_sigroles + 1));
    S->reads = (bool*)pl_alloc(p, (size_t)p->def->n_stimuli + 1);
    S->uni = (struct v2_uni*)pl_alloc(p, sizeof(struct v2_uni) * 64);   /* at load: a leaf opens on the frame path */
    S->cap_uni = 64;
    if (!S->gain || !S->reads || !S->uni) return -1;
    for (i = 0; i < p->def->n_sigroles; i++) {
        int k;
        S->gain[i] = 0;
        for (k = 0; k < p->args.n_gains; k++)
            if (strcmp(p->args.gains[k].name, p->def->sigroles[i].name) == 0) S->gain[i] = atof(p->args.gains[k].value);
    }
    /* the lateness bound B per role (docs/player.md, "Where each bound
     * comes from"): in --sim, --test-sample-late's value, or
     * --test-sample-bound's; 0 for every other role (a scripted source
     * stamps on arrival). A real run binds no signal role (G18), so no
     * role there needs a measured bound yet */
    S->bnd = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->def->n_sigroles + 1));
    S->newest = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->def->n_sigroles + 1));
    S->beyond = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->def->n_sigroles + 1));
    if (!S->bnd || !S->newest || !S->beyond) return -1;
    S->bnd_max = 0;
    for (i = 0; i < p->def->n_sigroles; i++) {
        int k;
        S->bnd[i] = 0;
        S->newest[i] = INT64_MIN;
        S->beyond[i] = 0;
        for (k = 0; k < p->args.n_lates; k++)
            if (strcmp(p->args.lates[k].name, p->def->sigroles[i].name) == 0) S->bnd[i] = (int64_t)llround(atof(p->args.lates[k].value) * 1e9);
        for (k = 0; k < p->args.n_bounds; k++)
            if (strcmp(p->args.bounds[k].name, p->def->sigroles[i].name) == 0) S->bnd[i] = (int64_t)llround(atof(p->args.bounds[k].value) * 1e9);
        if (S->bnd[i] > S->bnd_max) S->bnd_max = S->bnd[i];
    }
    for (i = 0; i < p->args.n_bounds; i++)
        if (!yexp_sigrole_get(p->def, p->args.bounds[i].name)) {
            pl_fail(p, "--test-sample-bound %s: the definition has no signal role '%s'", p->args.bounds[i].name, p->args.bounds[i].name);
            p->bad_arg = true;
            return -1;
        }
    if (p->def->n_sigroles > 0) {
        if (!p->args.sim) {
            /* G18: the rig profile has no adapter block, so a real run cannot bind a signal role */
            pl_fail(p, "signal role '%s': a real run needs the rig profile's adapter block (gap G18, not built); --sim gives each role a scripted source",
                    p->def->sigroles[0].name);
            return -1;
        }
        for (i = 0; i < p->def->n_sigroles; i++)
            if (!(S->gain[i] > 0) && (p->def->sigroles[i].type == YEXP_SIG_POSITION2D_RELATIVE ||
                                      (p->def->sigroles[i].type == YEXP_SIG_POSITION2D && p->def->sigroles[i].allow_relative))) {
                pl_fail(p, "signal role '%s': --sim needs --sim-gain %s=UNITS (its --sim adapter's %s per count)", p->def->sigroles[i].name,
                        p->def->sigroles[i].name, p->def->units == 2 ? "cm" : p->def->units == 1 ? "deg" : "px");
                p->bad_arg = true;
                return -1;
            }
    }
    for (ti = 0; ti < S->n_t; ti++) {
        pl_v2* v = S->t[ti];
        const yexp_template* t;
        int n_ctl = 0, k;
        if (!v) continue;
        t = v->def;
        p->tmpls[ti].v2 = v;
        S->cur_t = ti;
        n_ctl = t->n_controls;
        for (k = 0; k < p->def->n_controls; k++) if (t->uses_top && t->uses_top[k]) n_ctl++;
        v->ctl = (v2_ctl*)pl_alloc(p, sizeof(v2_ctl) * (size_t)(n_ctl + 1));
        v->role_node = (int*)pl_alloc(p, sizeof(int) * (size_t)(p->def->n_sigroles + 1));
        v->visits = (int*)pl_alloc(p, sizeof(int) * (size_t)(v->n_states + 1));
        v->state_onset = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(v->n_states + 1));
        v->stim_onset = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->n_stims + 1));
        v->stim_frame = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->n_stims + 1));
        v->flags = (bool*)pl_alloc(p, sizeof(bool) * (size_t)(t->n_flags + 1));
        v->state_frame = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(v->n_states + 1));
        v->state_recf = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(v->n_states + 1));
        v->state_rec = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(v->n_states + 1));
        v->stim_recf = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->n_stims + 1));
        v->stim_rec = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)(p->n_stims + 1));
        if (!v->state_frame || !v->state_recf || !v->state_rec || !v->stim_recf || !v->stim_rec) return -1;
        v->dcap = 2 * 64 * 3 * 8;
        v->data = (double*)pl_alloc(p, sizeof(double) * (size_t)v->dcap);
        if (!v->ctl || !v->role_node || !v->visits || !v->state_onset || !v->stim_onset || !v->stim_frame || !v->flags || !v->data) return -1;
        for (k = 0; k < p->def->n_sigroles; k++) v->role_node[k] = -1;
        for (k = 0; k < t->n_controls; k++) v->ctl[v->n_ctl++].def = &t->controls[k];
        for (k = 0; k < p->def->n_controls; k++) if (t->uses_top && t->uses_top[k]) v->ctl[v->n_ctl++].def = &p->def->controls[k];
        for (k = 0; k < v->n_ctl; k++) {
            v->ctl[k].val = v->ctl[k].test = v->ctl[k].hit = v->ctl[k].hit_u = v->ctl[k].of = v->ctl[k].emit = v->ctl[k].choice = v->ctl[k].slot = -1;
        }
        /* nodes: a template with no control and no form has no graph */
        for (k = 0; k < v->n_ctl; k++) if (v2_ctl_build(p, v, k) < 0) return -1;
        if (v2_forms_graph(p, v) < 0 || v2_traces_build(p, v) < 0 || v2_exits_build(p, v) < 0) return -1;
        /* the nodes the columns read, before the graph opens */
        for (k = 0; k < t->n_columns; k++) if (v2_column_nodes(p, v, t->columns[k].value) < 0) return -1;
        if (v2_template_checks(p, v) < 0) return -1;
        {
            /* the guard: every runtime state once, every over row, a margin */
            int s2;
            v->take_cap = v->n_rs + 8;
            for (s2 = 0; s2 < v->n_states; s2++) if (t->states[s2].max_visits > 0) v->take_cap++;
        }
        v->from_ch = t->response && t->response->from ? pl_channel(p, t->response->from, NULL) : -1;
        for (k = 0; k < v->n_traces; k++) if (v->traces[k].frame) S->cap_fpend++;
        if (v2_collector(p, v, &p->tmpls[ti]) < 0) return -1;
        if (v->nn > 0) {
            ysig_desc* d = &v->gd;
            size_t need;
            if (v2_adapters(p, v) < 0) return -1;
            memset(d, 0, sizeof *d);
            d->nodes = v->nodes;
            d->n_nodes = v->nn;
            d->roles = v->roles;
            d->n_roles = p->def->n_sigroles;
            d->adapters = v->adapters;
            d->n_adapters = p->def->n_sigroles;
            d->n_channels = v->n_slots > 0 ? v->n_slots : 1;
            d->channels = &S->dummy;   /* the real ones after every template's count */
            d->on_row = v2_on_row;
            d->user = v;
            /* ysig/sig.h 0.6.0: an arm at a state's entry time, after the
             * input the frame fed, runs the graph again from a checkpoint
             * (3.11 rules 2 and 4); 0.7.0 places a late report at its time
             * the same way. The window is catchup_ns of input time in 8
             * images (sig.md, the coordinator's ruling 3), whatever the
             * display's rate. The logs hold the window's input at the
             * fastest raw mouse (docs/signal_spec.md: 125 to 8000 Hz) on
             * every role, and at least two: a full log clears the window,
             * and the next past arm or late report falls back
             * (arm_past_full, late_full; t_v2_window_8k) */
            d->catchup_frames = V2_WINDOW_CKS;
            d->catchup_ns = v2_catchup_ns(p);
            v2_window_size(p, v, &d->catchup_calls, &d->catchup_writes);
            /* the sample traces are written at the trial's end: their rows
             * come once no arm can change them, so they are always the
             * reference order's (sig.h, final-only). Derived events feed
             * the response collector in their frame: at once */
            d->rows_final = true;
            if (v2_feeds(p, v) < 0) return -1;
            v->has_graph = true;
            if (v->n_slots > max_slots) max_slots = v->n_slots;
            need = ysig_arena_size(d);
            if (!need) {
                char err[256];
                ysig_check(d, err, sizeof err);
                pl_fail(p, "template '%s': the graph: %s", t->name, err);
                return -1;
            }
            d->arena_size = need;
            d->arena = pl_alloc(p, need + 16);
            if (!d->arena) return -1;
        }
        if (v2_binds(p, v) < 0 || v2_recenter_scan(p, v) < 0) return -1;
    }
    /* a stream a trace name, and one an rdk: the data set holds
     * YDS_MAX_STREAMS; refused here by name, not at the session's start */
    {
        int names = 0, k2, j2;
        for (ti = 0; ti < S->n_t; ti++) {
            pl_v2* v = S->t[ti];
            if (!v) continue;
            for (k2 = 0; k2 < v->n_traces; k2++) {
                int seen = 0, tj;
                for (tj = 0; tj <= ti && !seen; tj++) {
                    pl_v2* w = S->t[tj];
                    if (!w) continue;
                    for (j2 = 0; j2 < (tj == ti ? k2 : w->n_traces); j2++)
                        if (strcmp(w->traces[j2].def->name, v->traces[k2].def->name) == 0) { seen = 1; break; }
                }
                names += !seen;
            }
        }
        for (k2 = 0; k2 < p->def->n_stimuli; k2++) if (p->def->stimuli[k2].kind == YEXP_RDK) names++;
        if (names > YDS_MAX_STREAMS) {
            pl_fail(p, "the definition has %d streams (trace names and rdk stimuli); the data set holds %d", names, YDS_MAX_STREAMS);
            return -1;
        }
    }
    /* every frame trace's rows can wait V2_FRAME_WAIT(p) frames for records */
    if (S->cap_fpend > V2_MAX_FRAME_TRACES) {
        pl_fail(p, "the definition has %d frame traces; the player takes at most %d", S->cap_fpend, V2_MAX_FRAME_TRACES);
        return -1;
    }
    S->cap_fpend = (S->cap_fpend > 0 ? S->cap_fpend : 1) * V2_FRAME_WAIT(p);
    S->fpend = (v2_frow*)pl_alloc(p, sizeof(v2_frow) * (size_t)S->cap_fpend);
    if (!S->fpend) return -1;
    S->n_vals = p->n_channels + (max_slots > 0 ? max_slots : 1);
    S->vals = (float*)pl_alloc(p, sizeof(float) * (size_t)S->n_vals);
    if (!S->vals) return -1;
    for (ti = 0; ti < S->n_t; ti++) {
        pl_v2* v = S->t[ti];
        if (!v || !v->has_graph) continue;
        v->gd.channels = S->vals + p->n_channels;
        if (!ysig_open(&v->g, &v->gd)) { pl_fail(p, "template '%s': the graph: %s", v->def->name, ysig_error(&v->g)); return -1; }
        v->frozen = true;
        v->arm_gen = (uint32_t*)pl_alloc(p, sizeof(uint32_t) * (size_t)v->nn);
        if (!v->arm_gen) return -1;
        memset(v->arm_gen, 0, sizeof(uint32_t) * (size_t)v->nn);
        v->arm_t = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)v->nn);
        v->st_armed = (int16_t*)pl_alloc(p, sizeof(int16_t) * (size_t)v->nn);
        v->dis_t = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)v->nn);
        v->der_cap = YSIG_QUEUE_CAP + v->gd.catchup_calls;   /* sig.h's derived queue */
        v->der = (yin_event*)pl_alloc(p, sizeof(yin_event) * (size_t)v->der_cap);
        v->arm_tk = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)v->nn);
        v->arm_tk_prev = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)v->nn);
        if (!v->arm_tk || !v->arm_tk_prev) return -1;
        memset(v->arm_tk, 0, sizeof(int64_t) * (size_t)v->nn);
        memset(v->arm_tk_prev, 0, sizeof(int64_t) * (size_t)v->nn);
        v->g_used = v->g_der = INT64_MIN;
        /* every v2_take() and steps' end at one time: take_cap bounds the
         * takes, and each take can follow a steps' end */
        v->ch_cap = 2 * v->take_cap + 2;
        v->ch_rs = (int*)pl_alloc(p, sizeof(int) * (size_t)v->ch_cap);
        v->ch_k = (int*)pl_alloc(p, sizeof(int) * (size_t)v->ch_cap);
        v->ch_dr = (int64_t*)pl_alloc(p, sizeof(int64_t) * (size_t)v->ch_cap);
        if (!v->ch_rs || !v->ch_k || !v->ch_dr) return -1;
        v->ch_t = INT64_MIN;
        v->ch_n = 0;
        v->arm_min = INT64_MAX;
        v->lag_max = YTR_MISSING_NS;
        if (!v->st_armed || !v->dis_t || !v->der) return -1;
        v->det_now = (bool*)pl_alloc(p, sizeof(bool) * (size_t)v->nn);
        if (!v->arm_t || !v->det_now) return -1;
        {
            int k;
            for (k = 0; k < v->nn; k++) {
                v->arm_t[k] = INT64_MIN;
                v->dis_t[k] = INT64_MIN;
                v->det_now[k] = v->nodes[k].op == YSIG_OP_DETECT && v->nodes[k].p[YSIG_DETECT_ENTRY] == YSIG_ENTRY_NOW;
            }
        }
        ysig_reset(&v->g, 0);
        v->cur = V2_END;
        v->ended = true;
    }
    {
        /* a poll's samples go in one ysig_feed_n() a graph, in parts that
         * each emit at most sig.h's derived queue (64): every sample can
         * fire each emit node once, and the hits and events are taken
         * after the part */
        int e = 1, tj;
        for (tj = 0; tj < S->n_t; tj++) {
            int k, n = 0;
            if (!S->t[tj] || !S->t[tj]->has_graph) continue;
            for (k = 0; k < S->t[tj]->nn; k++) n += S->t[tj]->nodes[k].op == YSIG_OP_EMIT;
            if (n > e) e = n;
        }
        S->batch_cap = YSIG_QUEUE_CAP / e > 0 ? YSIG_QUEUE_CAP / e : 1;
        S->batch = (yin_event*)pl_alloc(p, sizeof(yin_event) * (size_t)S->batch_cap);
        if (!S->batch) return -1;
    }
    S->cur_t = -1;
    return v2_samples_load(p);
}

void pl_v2_close(pl_player* p) {
    if (p->v2s) {
        free(p->v2s->smp);
        p->v2s->smp = NULL;
    }
}

/* --- variables (3.14) and flags ------------------------------------------ */

static double v2_mix_u53(uint64_t* s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}
static uint64_t v2_mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static int v2_name_cmp(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }

/* D29: the leaf's j-th uniform (its name's rank in byte order among them,
 * over the leaf and its containers, nearest first), from the leaf's seed. */
int pl_v2_leaf_open(pl_player* p, uint64_t leaf_seed) {
    pl_v2_session* S = p->v2s;
    const yexp_node* n;
    const char* names[64];
    const yexp_value* vals[64];
    int nn = 0, j, k;
    if (!S) return 0;
    S->n_uni = 0;
    for (n = p->leaf ? p->leaf->node : NULL; n; n = n->parent)
        for (k = 0; k < n->n_vars; k++) {
            int dup = 0, m;
            if (n->vars[k].value->kind != YEXP_V_UNIFORM) continue;
            /* 3.14: a leaf whose template does not read it does not draw it (reading
             * F24: j ranks the uniforms the leaf draws) */
            if (!p->leaf->tmpl || !pl_v2_reads(p, p->leaf->tmpl->def, n->vars[k].key)) continue;
            for (m = 0; m < nn; m++) if (strcmp(names[m], n->vars[k].key) == 0) dup = 1;
            if (dup || nn >= 64) continue;
            names[nn] = n->vars[k].key;
            vals[nn] = n->vars[k].value;
            nn++;
        }
    if (!nn) return 0;
    if (!S->uni) { pl_fail(p, "internal: the uniform draws have no storage"); return -1; }
    {
        /* rank by byte order: sort the names, carry the values */
        const char* sorted[64];
        memcpy(sorted, names, sizeof(char*) * (size_t)nn);
        qsort(sorted, (size_t)nn, sizeof(char*), v2_name_cmp);
        for (j = 0; j < nn; j++) {
            const yexp_value* v = NULL;
            struct v2_uni* u = &S->uni[j];
            double lo, hi;
            uint64_t st;
            for (k = 0; k < nn; k++) if (names[k] == sorted[j]) v = vals[k];
            u->name = sorted[j];
            u->n = (int)v->i;
            if (u->n < 1 || u->n > 64) u->n = 1;
            if (pl_eval_num(p, &p->env, &v->items[0], &lo) < 0 || pl_eval_num(p, &p->env, &v->items[1], &hi) < 0) return -1;
            st = v2_mix64((leaf_seed ^ 0xBB67AE8584CAA73BULL) + (uint64_t)(j + 1) * 0x9E3779B97F4A7C15ULL);
            for (k = 0; k < u->n; k++) {
                u->v[k] = lo + (hi - lo) * v2_mix_u53(&st);
                memset(&u->items[k], 0, sizeof u->items[k]);
                u->items[k].kind = YEXP_V_NUMBER;
                u->items[k].num = u->v[k];
            }
        }
        S->n_uni = nn;
    }
    return 0;
}

int pl_v2_var(pl_player* p, const pl_env* e, const char* name, pl_val* out) {
    pl_v2_session* S = p->v2s;
    const yexp_node* n;
    int k;
    if (!S) return 0;
    for (k = 0; k < S->n_uni; k++)
        if (strcmp(S->uni[k].name, name) == 0) {
            memset(out, 0, sizeof *out);
            if (S->uni[k].n == 1) { out->kind = YEXP_V_NUMBER; out->num = S->uni[k].v[0]; out->i = (int64_t)out->num; return 1; }
            out->kind = YEXP_V_ARRAY;
            out->n = (uint32_t)S->uni[k].n;
            out->items = S->uni[k].items;
            return 1;
        }
    /* a walk named W gives W_x, W_y, W_from_x, W_from_y, W_deg */
    for (n = e->node; n; n = n->parent)
        for (k = 0; k < n->n_vars; k++) {
            static const char* const sfx[5] = { "_x", "_y", "_from_x", "_from_y", "_deg" };
            size_t kl = strlen(n->vars[k].key);
            int c;
            if (n->vars[k].value->kind != YEXP_V_WALK2D || strncmp(name, n->vars[k].key, kl) != 0) continue;
            for (c = 0; c < 5; c++)
                if (strcmp(name + kl, sfx[c]) == 0) {
                    double w[5];
                    int j;
                    if (!e->trials || !e->ti) { pl_fail(p, "'$%s' is drawn per trial; it has no value here", name); return -1; }
                    j = ytr_var_index(e->trials, n->vars[k].key);
                    if (j < 0) return 0;   /* this leaf does not draw it (3.14) */
                    if (ytr_var_walk(e->trials, e->ti->index, j, w) < 0) { pl_fail(p, "'$%s': the trial has no walk", name); return -1; }
                    memset(out, 0, sizeof *out);
                    out->kind = YEXP_V_NUMBER;
                    out->num = w[c];
                    out->i = (int64_t)w[c];
                    return 1;
                }
        }
    {
        bool b;
        if (e->t && pl_v2_flag(p, name, &b) == 1) {
            memset(out, 0, sizeof *out);
            out->kind = YEXP_V_BOOL;
            out->i = b;
            out->num = b;
            return 1;
        }
    }
    return 0;
}

int pl_v2_flag(pl_player* p, const char* name, bool* out) {
    pl_v2* v;
    int k;
    if (!p->tmpl || !(v = p->tmpl->v2)) return 0;
    for (k = 0; k < v->def->n_flags; k++)
        if (strcmp(v->def->flags[k], name) == 0) { *out = v->flags[k]; return 1; }
    return 0;
}

/* --- the trial: parameters ----------------------------------------------- */

static int v2_item(pl_player* p, const yexp_value* v, int elem, double* out, int how) {
    pl_val x;
    if (pl_eval(p, &p->env, v, &x) < 0) return -1;
    if (elem >= 0) {
        pl_val y;
        if (x.kind != YEXP_V_ARRAY || (uint32_t)elem >= x.n) { pl_fail(p, "a point or array value has no element %d", elem); return -1; }
        if (pl_eval(p, &p->env, &x.items[elem], &y) < 0) return -1;
        x = y;
    }
    if (x.kind != YEXP_V_NUMBER && x.kind != YEXP_V_BOOL) { pl_fail(p, "a control's number is not a number"); return -1; }
    *out = (how == P_SEC || how == P_ENTRY) && (x.flags & YEXP_VF_DURATION) ? (double)x.i / 1e9 : x.num;
    if (how == P_NEG) *out = -*out;
    /* 3.7: a hold of 0 (in ns, as the loader rounds) is entry now, else arm */
    if (how == P_ENTRY) *out = (*out < 0.5e-9 && *out > -0.5e-9) ? YSIG_ENTRY_NOW : YSIG_ENTRY_ARM;
    return 0;
}

/* cos and sin of a multiple of 90 degrees exact (3.4), so a mirror keeps
 * the sign of zero */
static void v2_cs90(double deg, double* c, double* s) {
    double q = deg / 90.0;
    if (q == floor(q) && fabs(q) < 1e9) {
        long k = ((long)q % 4 + 4) % 4;
        static const double C[4] = { 1, 0, -1, 0 }, S[4] = { 0, 1, 0, -1 };
        *c = C[k];
        *s = S[k];
        return;
    }
    *c = cos(deg * 3.14159265358979323846 / 180.0);
    *s = sin(deg * 3.14159265358979323846 / 180.0);
}

static int v2_params_apply(pl_player* p, pl_v2* v) {
    int i;
    for (i = 0; i < v->n_params; i++) {
        const v2_param* q = &v->params[i];
        double x;
        bool ok;
        if (v2_item(p, q->v, q->elem, &x, q->how) < 0) return -1;
        ok = q->param >= 0 ? ysig_set(&v->g, q->node, q->param, x) : ysig_set_data(&v->g, q->node, -1 - q->param, x);
        if (q->param == YSIG_DETECT_ENTRY && v->det_now) v->det_now[q->node] = x == YSIG_ENTRY_NOW;
        if (!ok) {
            pl_fail(p, "template '%s': the graph refused %g for node %d's parameter %d", v->def->name, x, q->node, q->param);
            return -1;
        }
    }
    for (i = 0; i < v->n_xform; i++) {
        int node = v->xform[i].node, k;
        double m[4] = { 0, 0, 0, 0 }, angle = 0, ax = 0, ay = 0;
        pl_val x;
        if (v->xform[i].v) {
            if (pl_eval(p, &p->env, v->xform[i].v, &x) < 0) return -1;
            if (x.kind == YEXP_V_TRANSFORM && x.str && strcmp(x.str, "rotation_deg") == 0) angle = -x.num;   /* y down: ccw on screen */
            else if (x.kind == YEXP_V_TRANSFORM && x.str && strcmp(x.str, "mirror_deg") == 0) {
                double c, s;
                v2_cs90(2 * x.num, &c, &s);
                m[0] = c; m[1] = -s; m[2] = -s; m[3] = -c;
            } else if (!(x.kind == YEXP_V_STRING && x.str && strcmp(x.str, "identity") == 0)) {
                pl_fail(p, "template '%s': a cursor's transform is not identity, rotation_deg or mirror_deg", v->def->name);
                return -1;
            }
        }
        if (v->xform[i].about) {
            if (v->xform[i].about->kind == YEXP_V_ARRAY) {
                if (v2_item(p, &v->xform[i].about->items[0], -1, &ax, P_NUM) < 0 || v2_item(p, &v->xform[i].about->items[1], -1, &ay, P_NUM) < 0) return -1;
            } else if (v2_item(p, v->xform[i].about, 0, &ax, P_NUM) < 0 || v2_item(p, v->xform[i].about, 1, &ay, P_NUM) < 0) return -1;
        }
        for (k = 0; k < 4; k++) if (!ysig_set(&v->g, node, YSIG_AFFINE_M_XX + k, m[k])) return -1;
        if (!ysig_set(&v->g, node, YSIG_AFFINE_ANGLE, angle) || !ysig_set(&v->g, node, YSIG_AFFINE_PRE_X, ax) ||
            !ysig_set(&v->g, node, YSIG_AFFINE_PRE_Y, ay) || !ysig_set(&v->g, node, YSIG_AFFINE_POST_X, ax) ||
            !ysig_set(&v->g, node, YSIG_AFFINE_POST_Y, ay)) {
            pl_fail(p, "template '%s': the graph refused a transform", v->def->name);
            return -1;
        }
    }
    return 0;
}

/* --- the trial: states and exits (3.11) ------------------------------------ */

static int64_t v2_anchor(pl_player* p) { return p->trial_start_onset; }

static void v2_outcome(pl_v2* v, const yexp_value* o, const char* text) {
    if (!o && !text) return;
    v->outcome = o;
    v->outcome_text = text;
    v->outcome_set = true;
}

static void v2_srow_close(pl_player* p, pl_v2* v, int code, int64_t t) {
    pl_v2_session* S = p->v2s;
    int i;
    if (v->def_cur < 0) return;
    for (i = 0; i < S->n_srow; i++) {
        v2_srow* r = &S->srow[i];
        if (!r->ended && r->trial == p->global_trial - 1) {
            r->ended = true;
            r->code = code;
            r->end_ns = t;
        }
    }
    v->def_cur = -1;
}

static int v2_srow_flush(pl_player* p, bool all) {
    pl_v2_session* S = p->v2s;
    int i, m = 0;
    for (i = 0; i < S->n_srow; i++) {
        v2_srow* r = &S->srow[i];
        if (r->ended && (r->rec_in || all)) {
            yds_event ev;
            yds_event_init(&ev);
            ev.kind = "state";
            ev.trial_index = r->trial;
            ev.frame = r->frame;
            ev.t_ns = r->rec_in ? r->onset : r->planned;
            ev.planned_ns = r->planned;
            ev.residual_ns = r->rec_in ? r->residual : YTR_MISSING_NS;
            ev.end_ns = r->end_ns;
            ev.code = r->code;
            ev.text = r->name;
            if (pl_event_queue(p, &ev) < 0) return -1;
            continue;
        }
        S->srow[m++] = *r;
    }
    S->n_srow = m;
    return 0;
}

static int v2_srow_open(pl_player* p, pl_v2* v, int def, const yscr_frame* f) {
    pl_v2_session* S = p->v2s;
    v2_srow* r;
    if (S->n_srow >= V2_STATE_ROWS && v2_srow_flush(p, true) < 0) return -1;
    r = &S->srow[S->n_srow++];
    memset(r, 0, sizeof *r);
    r->frame = f->index;
    r->planned = f->onset;
    r->trial = p->global_trial - 1;
    r->name = v->def->states[def].name;
    return 0;
}

static bool v2_if_true(pl_player* p, pl_v2* v, const v2_exit* x) {
    pl_val val;
    (void)v;
    if (!x->if_var) return true;
    if (pl_var(p, &p->env, x->if_var, &val) < 0) return false;
    if (val.kind == YEXP_V_BOOL) return val.i != 0;
    return val.kind == YEXP_V_NUMBER && val.num == 1;
}

static int v2_take(pl_player* p, pl_v2* v, const v2_exit* x, int64_t t, const yscr_frame* f, bool hit);

/* sig.h 0.6.0: an arm before input the graph had runs it again from a
 * checkpoint; 0.7.0 places a late report or state call at its time the
 * same way. The player takes changes in time order and arms at a take's
 * time t, so a run again changes only what is after t: the re-armed
 * detect's earlier arm's hits (superseded, and dropped here by arm_gen)
 * and the hit at t that ended that arm. A hit of another detect, a row
 * or a derived event that it changes is one the player may have acted on
 * or written; a fallback or a shape outside the verified ones is not the
 * reference order's result: an internal guard (docs/player.md, "An arm in
 * the past"). Summed over every graph: each takes every sample, and an
 * adapter keeps its state across templates. Not late_frames (a drawn
 * frame, the input's own lateness) or late_barrier (a report of a trial
 * that ended, dropped in R too). */
static int v2_guard(pl_player* p, const pl_v2* v, const char* what, const char* name, int64_t t) {
    pl_v2_session* S = p->v2s;
    int64_t c[15], bad = 0;
    int ti, k;
    memset(c, 0, sizeof c);
    for (ti = 0; S && ti < S->n_t; ti++) {
        ysig_stats st;
        if (!S->t[ti] || !S->t[ti]->has_graph) continue;
        ysig_get_stats(&S->t[ti]->g, &st);
        c[0] += st.arm_hits_retracted;
        c[1] += st.arm_rows_stale;
        c[2] += st.arm_derived_stale;
        c[3] += st.arm_past_pinned;
        c[4] += st.arm_past_far;
        c[5] += st.arm_past_full;
        c[6] += st.arm_past_off;
        c[7] += st.arm_unverified;
        c[8] += st.late_far;
        c[9] += st.late_full;
        c[10] += st.late_off;
        c[11] += st.late_unverified;
        c[12] += st.late_hits_voided;
        /* a hit or event that found sig.h's queue full while the player
         * held it past the horizon: lost (M2) */
        if (S->bnd_max > 0) { c[13] += st.hits_lost; c[14] += st.derived_lost; }
    }
    for (k = 0; k < 15; k++) bad += c[k];
    if (!S || bad == S->bad_seen) return 0;
    S->bad_seen = bad;
    {
        char at[192];
        if (name) snprintf(at, sizeof at, "%s '%s' at %.9f s", what, name, (double)(t - v2_anchor(p)) / 1e9);
        else snprintf(at, sizeof at, "%s", what);
        /* the counts last: a harness keeps the message's tail */
        pl_fail(p, "template '%s': internal guard: %s (a hit taken, or a row or event written, changed; a fallback; or a shape "
                "outside the verified ones; sig.h: %lld hits retracted, %lld row runs, %lld derived runs changed; "
                "%lld pinned, %lld before the window, %lld logs full, %lld no window; %lld unverified; late: %lld before the window, "
                "%lld logs full, %lld no window, %lld unverified, %lld hits voided; held: %lld hits lost, %lld derived lost)",
                v->def->name, at, (long long)c[0], (long long)c[1], (long long)c[2], (long long)c[3], (long long)c[4],
                (long long)c[5], (long long)c[6], (long long)c[7], (long long)c[8], (long long)c[9], (long long)c[10],
                (long long)c[11], (long long)c[12], (long long)c[13], (long long)c[14]);
    }
    return -1;
}
static int v2_arm_check(pl_player* p, pl_v2* v, const char* name, int64_t t) {
    return v2_guard(p, v, "a past arm of", name, t);
}

/* W2 (docs/sig.md, "The cancel, by key"): a disarm at a state's exit time
 * ends every arm of the control with a smaller key, also one made after
 * it, so an arm earlier than that time would be made and ended at once.
 * The next state's arms are at the exit time or later: an internal guard. */
static int v2_arm_after_disarm(pl_player* p, pl_v2* v, int node, const char* name, int64_t t) {
    if (t >= v->dis_t[node]) return 0;
    pl_fail(p, "template '%s': internal guard: '%s' armed at %.9f s, before its disarm at %.9f s (docs/sig.md, W2)", v->def->name, name,
            (double)(t - v2_anchor(p)) / 1e9, (double)(v->dis_t[node] - v2_anchor(p)) / 1e9);
    return -1;
}

/* An arm at t: in the reference order at once when the graph has input up
 * to t; input with an earlier time after it is late (sig_arms.late_input) */
static void v2_late_arm(pl_v2* v, int64_t t) {
    if (t <= v->g.t_cover && t > v->late_bound) v->late_bound = t;
}
static void v2_late_in(pl_v2* v, int64_t t) {
    if (t < v->late_bound) v->n_late++;
}

static int v2_arm_ctl(pl_player* p, pl_v2* v, int ci, int64_t t) {
    v2_ctl* c = &v->ctl[ci];
    if (c->hit < 0) return 0;
    if (v2_arm_after_disarm(p, v, c->hit, c->def->name, t) < 0) return -1;
    v2_late_arm(v, t);
    v->arm_gen[c->hit]++;
    v->arm_t[c->hit] = t;
    v->arm_tk_prev[c->hit] = v->arm_tk[c->hit];
    v->arm_tk[c->hit] = v->dr_n;
    if (t < v->arm_min) v->arm_min = t;
    if (!ysig_arm(&v->g, c->hit, t)) { pl_fail(p, "template '%s': cannot arm '%s'", v->def->name, c->def->name); return -1; }
    return v2_arm_check(p, v, c->def->name, t);
}

static int v2_arm_exit(pl_player* p, pl_v2* v, const v2_exit* x, int64_t t) {
    int k;
    if (x->node < 0) return 0;
    /* the state's own: disarmed when it exits (v2_disarm_state) */
    for (k = 0; k < v->n_st_armed && v->st_armed[k] != x->node; k++) { }
    if (k == v->n_st_armed && v->n_st_armed < v->nn) v->st_armed[v->n_st_armed++] = (int16_t)x->node;
    if (v2_arm_after_disarm(p, v, x->node, v->ctl[x->ctl].def->name, t) < 0) return -1;
    v->arm_gen[x->node]++;
    v->arm_t[x->node] = t;
    v->arm_tk_prev[x->node] = v->arm_tk[x->node];
    v->arm_tk[x->node] = v->dr_n;
    if (t < v->arm_min) v->arm_min = t;
    v2_late_arm(v, t);
    if (!ysig_arm(&v->g, x->node, t)) { pl_fail(p, "template '%s': cannot arm '%s'", v->def->name, v->ctl[x->ctl].def->name); return -1; }
    return v2_arm_check(p, v, v->ctl[x->ctl].def->name, t);
}

/* Whether the loop arms exit i of r (`to` < 0: every hit exit, else those
 * to `to`): each detect once. One arm per detect gives the same R: two
 * exits on one control would arm one detect twice at one time with no call
 * between, and the second arm restarts what the first started. But a
 * second arm at one time is a later group that meets the first, and
 * sig.h's condition 2 (b) counts the first arm_unverified when a
 * disarm_at lets both in (X1): a guard stop (seed 100829 of the model's
 * harness at 500 Hz; player_test t_v2_two_exits_one_ctl). */
static bool v2_arms(const v2_rs* r, int i, int to) {
    int j;
    if (r->exits[i].kind != X_HIT || (to >= 0 && r->exits[i].to != to)) return false;
    for (j = 0; j < i; j++)
        if (r->exits[j].kind == X_HIT && (to < 0 || r->exits[j].to == to) && r->exits[j].node == r->exits[i].node) return false;
    return true;
}

/* A poll's derived events earlier than t, to the collector: fed one sample
 * at a time, these came before a hit at t was taken (the hit's sample's
 * own after it). Held events stay in their order. */
static void v2_der_upto(pl_player* p, pl_v2* v, int64_t t) {
    int k, w = 0;
    for (k = 0; k < v->n_der; k++) {
        if (v->der[k].t < t) { if (p->tmpl->collector) yrsp_feed(p->tmpl->collector, &v->der[k]); }
        else v->der[w++] = v->der[k];
    }
    v->n_der = w;
}

static void v2_resp_arm(pl_player* p, pl_v2* v, int64_t t) {
    const yexp_response* r = v->def->response;
    int i;
    yrsp_collector* c = p->tmpl->collector;
    if (!c || !r || !r->signal_choices) return;
    /* the window's reference is the last position fed before the arm */
    v2_der_upto(p, v, t);
    /* yrsp_arm() sets the desc's window again: the trial's deadline and
     * minimum rt go back on at every arm */
    yrsp_arm(c, t);
    yrsp_set_window(c, v->resp_minrt, v->resp_dur);
    yrsp_set_wait_final(c, PL_WAIT_FINAL_NS);
    /* each movement's origin, else the last position fed before the arm */
    for (i = 0; i < v->n_ctl; i++) {
        const yexp_control* m = v->ctl[i].def;
        double ox, oy;
        if (m->kind != YEXP_MOVEMENT || v->ctl[i].choice < 0 || !v2_given(m, "origin")) continue;
        {
            const yexp_value* o = v2_key(m, "origin");
            if (o->kind == YEXP_V_ARRAY) {
                if (v2_item(p, &o->items[0], -1, &ox, P_NUM) < 0 || v2_item(p, &o->items[1], -1, &oy, P_NUM) < 0) return;
            } else if (v2_item(p, o, 0, &ox, P_NUM) < 0 || v2_item(p, o, 1, &oy, P_NUM) < 0) return;
        }
        yrsp_set_origin(c, v->ctl[i].choice, (float)ox, (float)oy);
    }
}

/* The trial's end at t, as version 1's end MARK (rule 10). */
static int v2_end_mark(pl_player* p, pl_v2* v, int64_t t) {
    ytl_op o[2];
    int rc;
    memset(o, 0, sizeof o);
    o[0].op = YTL_OP_AT;
    o[0].t_ns = t - v2_anchor(p);
    o[1].op = YTL_OP_MARK;
    o[1].code = PL_MARK_END;
    p->q = ytl_seq_on(&p->tl, PL_BASE_TRIAL);
    rc = ytl_run(&p->q, o, 2);
    if (rc != 0 || p->q.err) { pl_fail(p, "template '%s': the end: %s", v->def->name, ytl_strerror(rc ? rc : p->q.err)); return -1; }
    return 0;
}

/* Whether s is one of a state's head recenter steps (3.6, 3.12). */
static bool v2_head_recenter(const yexp_state* st, const yexp_step* s) {
    int k;
    for (k = 0; k < st->n_steps && st->steps[k].op == YEXP_RECENTER; k++)
        if (&st->steps[k] == s) return true;
    return false;
}

/* An events template's `after`, once its events are done and its response
 * has ended (version 1's rule, 2.5): from the later of the two. A response
 * whose `from` never landed (an until's timeout before it) is not waited
 * for (reading F35). */
static int v2_after(pl_player* p, pl_v2* v) {
    int64_t start = v->events_end_rel;
    if (v->after_built || !v->events_done) return 0;
    /* `from` landed, or a table of this trial shows it (its onset can land
     * in this frame's evaluate, after the events' end) */
    if (v->def->response && p->responding && !v->resp_done && (p->from_landed || v->from_pending)) return 0;
    if (v->resp_done && v->resp_bt > start) start = v->resp_bt;
    v->after_built = true;
    return pl_after_build(p, start);
}

int pl_v2_events_response(pl_player* p, int64_t bt) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    if (!v || v->def->states) return 0;
    v->resp_done = true;
    v->resp_bt = bt;
    return v2_after(p, v);
}

/* The state left at t: each control its exits armed, made or waiting, is
 * disarmed at t (sig.h 0.7.0, ysig_disarm_at()). An arm at or before t is
 * made and ended at t, so what the reference order gives by the exit time
 * stands; a waiting arm later than t is cancelled, not made later for a
 * state that ended. The next state's arms of the same control are at t or
 * later, after the disarm by call order, so it does not end them (the
 * shape 0.10.0's ca9bc48 fixed by disarming only the arms ahead of the
 * input). A one-hit control's output stands (a freeze_at latch keeps its
 * value). */
static int v2_disarm_state(pl_player* p, pl_v2* v, int64_t t) {
    int k;
    for (k = 0; k < v->n_st_armed; k++) {
        int node = v->st_armed[k];
        if (!ysig_disarm_at(&v->g, node, t)) { pl_fail(p, "template '%s': cannot disarm node %d", v->def->name, node); return -1; }
        v->dis_t[node] = t;
        if (t < v->arm_min) v->arm_min = t;   /* R5 (ii): a state call placed */
    }
    v->n_st_armed = 0;
    /* a disarm placed before calls already made runs the window again */
    return v2_guard(p, v, "a state's end", NULL, t);
}

/* Enter runtime state rs at time t; f is the frame being made (the entry
 * frame, D35 and rule 2). */
static int v2_enter(pl_player* p, pl_v2* v, int rs, int64_t t, const yscr_frame* f, bool hit) {
    const v2_rs* r;
    int i, guard = 0;
    for (;;) {
        if (++guard > 4096) { pl_fail(p, "template '%s': more than 4096 instant exits at one time", v->def->name); return -1; }
        /* a state left (not a part of it: an until's next part keeps the
         * state's exits armed) */
        if (v->has_graph && (rs == V2_END || v->rs[rs].first) && v2_disarm_state(p, v, t) < 0) return -1;
        if (rs == V2_END && !v->def->states) {
            /* an events template: its events are done; `after` follows
             * them and the response (2.5), and its end MARK ends the trial */
            v->cur = V2_END;
            v->hit_entry = hit;
            v->events_done = true;
            v->events_end_rel = t - v2_anchor(p);
            return v2_after(p, v);
        }
        if (rs == V2_END) {
            v->cur = V2_END;
            v->hit_entry = hit;
            /* F34 (3): an open response window (its from landed or a table
             * shows it, and it has not ended) holds the end; a from never
             * shown opened no window, so nothing waits */
            /* a window that can end on its own: a press ends it, or it has a
             * deadline; a persisting one with no deadline is closed by the
             * trial's end, as before (else nothing would end the trial) */
            if (v->def->response && p->responding && !v->resp_ended && (p->from_landed || v->from_pending) &&
                (v->def->response->ends_trial || v->resp_dur > 0 || v->def->response->max_responses > 0)) {
                v->end_wait = true;
                v->end_wait_t = t;
                return 0;
            }
            return v2_end_mark(p, v, t);
        }
        r = &v->rs[rs];
        if (r->first && r->def >= 0) {
            const yexp_state* st = &v->def->states[r->def];
            v->visits[r->def]++;
            if (st->max_visits > 0 && v->visits[r->def] > st->max_visits) {
                /* D20 rule 12: past max_visits the state takes `over`, a
                 * visit of no time with code -2 (reading F20) */
                if (v2_srow_open(p, v, r->def, f) < 0) return -1;
                v->def_cur = r->def;
                v2_srow_close(p, v, -2, t);
                v2_outcome(v, st->over_outcome, NULL);
                rs = st->over_to.kind == YEXP_REF_END ? V2_END : v->first_rs[st->over_to.index];
                continue;
            }
            if (v2_srow_open(p, v, r->def, f) < 0) return -1;
            v->def_cur = r->def;
            v->def_entry_frame = f->index;
            v->def_entry_t = t;
            v->visit_onset = f->onset;
            v->state_onset[r->def] = f->onset;
            v->state_frame[r->def] = f->index;
            if (r->def == v->base_state) v->base_onset = f->onset;
            /* rule 3: the state's exits' controls arm at its entry time; a
             * state whose first step is a recenter arms them at that call's
             * time (3.12), when its mark lands (pl_v2_on_mark) */
            v->arm_rs = -1;
            v->visit_ev0 = -1;
            if (st->n_steps > 0 && st->steps[0].op == YEXP_RECENTER && !(v->trial_entry && r->def == 0)) {
                int h = 0;
                while (h < st->n_steps && st->steps[h].op == YEXP_RECENTER) h++;
                v->arm_rs = rs;
                v->arm_after = &st->steps[h - 1];
            } else
                for (i = 0; i < r->n_exits; i++)
                    if (v2_arms(r, i, -1) && v2_arm_exit(p, v, &r->exits[i], t) < 0) return -1;
            if (v->resp_from_kind == YEXP_REF_STATE && v->resp_from_index == r->def) v2_resp_arm(p, v, t);
            /* an instant exit is taken when the state starts, before its
             * steps run (reading F21) */
            for (i = 0; i < r->n_exits; i++)
                if (r->exits[i].kind == X_INSTANT && v2_if_true(p, v, &r->exits[i])) break;
            if (p->failed) return -1;
            if (i < r->n_exits) {
                const v2_exit* x = &r->exits[i];
                v2_outcome(v, x->outcome, x->outcome_text);
                v2_srow_close(p, v, x->code, t);
                rs = x->to;
                continue;
            }
        }
        if (r->wait) {
            /* the until's control arms when the steps before it end (D35
             * (h)); after a recenter among them, at that call (3.12), when
             * its mark lands later in this frame */
            int id, rid = -1;
            for (id = v->ev0; id < v->ev1; id++) {
                const ytl_event* e = ytl_find(&p->tl, id);
                if (e && e->kind == YTL_MARK && !(e->flags & (YTL_EV_FIRED | YTL_EV_SKIPPED)) &&
                    e->code >= PL_MARK_RECENTER && e->code < PL_MARK_RECENTER + 4096 && v2_anchor(p) + e->time_ns <= t) rid = id;
            }
            v->arm_wait = -1;
            if (rid >= 0) { v->arm_wait = rs; v->arm_wait_id = rid; }
            else
                for (i = 0; i < r->n_exits; i++)
                    if (v2_arms(r, i, rs + 1) && v2_arm_exit(p, v, &r->exits[i], t) < 0) return -1;
        }
        break;
    }
    v->cur = rs;
    v->entry_t = t;
    v->entry_onset = f->onset;
    if (r->def < 0) v->visit_onset = f->onset;   /* an events template's part */
    v->entry_onset_known = true;
    v->hit_entry = hit;
    /* the state's steps: one op table from its entry time */
    {
        ytl_op* ops = p->tmpl->ops;
        int64_t cursor = t - v2_anchor(p);
        int n = 0, m, rc;
        ytl_event dummy;
        (void)ytl_prune(&p->tl, PL_BASE_TRIAL);
        memset(&dummy, 0, sizeof dummy);
        memset(&ops[0], 0, sizeof ops[0]);
        ops[0].op = YTL_OP_AT;
        ops[0].t_ns = cursor;
        n = 1;
        p->ops_latest = cursor;
        /* an events template's at_s counts from the trial's start (2.5) */
        p->ops_origin = r->def >= 0 ? cursor : 0;
        m = r->wait ? 0 : pl_build_ops(p, r->steps, r->n_steps, ops + n, p->tmpl->ops_cap - n, &cursor);
        p->ops_origin = 0;
        if (m < 0) return -1;
        n += m;
        {
            /* a tween on a channel the exit froze starts from the frozen
             * value, not from the stopped track's at the start (which the
             * jump would only reach once it started) */
            int k, j;
            for (j = 0; j < v->n_frz; j++)
                for (k = 0; k < n; k++)
                    if ((ops[k].op == YTL_OP_TO || ops[k].op == YTL_OP_THEN) && ops[k].ch == v->frz[j].ch) {
                        if (!ops[k].tween.from_set) { ops[k].tween.from = v->frz[j].v; ops[k].tween.from_set = true; }
                        break;
                    }
            v->n_frz = 0;
        }
        v->ev0 = ytl_add_n(&p->tl, &dummy, 0);
        if (r->first || v->visit_ev0 < 0) { v->visit_ev0 = v->ev0; v->n_tw = 0; }
        {
            /* the table's tweens and their ends, on the trial's base */
            int64_t cur = 0;
            int k;
            for (k = 0; k < n; k++) {
                const ytl_op* o = &ops[k];
                if (o->op == YTL_OP_ON && o->ch == v->from_ch) v->from_pending = true;
                if (o->op == YTL_OP_AT) cur = o->t_ns;
                else if (o->op == YTL_OP_WAIT) cur += o->t_ns;
                else if (o->op == YTL_OP_TO || o->op == YTL_OP_THEN) {
                    int64_t legs = (int64_t)(o->tween.cycles > 1 ? o->tween.cycles : 1) * (o->tween.yoyo ? 2 : 1);
                    int64_t end = o->tween.cycles == YTL_FOREVER ? INT64_MAX : cur + o->tween.delay_ns + o->tween.duration_ns * legs;
                    if (v->n_tw >= v->cap_tw) { pl_fail(p, "template '%s': internal: a visit has more than its %d tweens", v->def->name, v->cap_tw); return -1; }
                    v->tw[v->n_tw].ch = o->ch;
                    v->tw[v->n_tw].end = end;
                    v->n_tw++;
                    if (o->op == YTL_OP_THEN) cur += o->tween.delay_ns + o->tween.duration_ns;
                }
            }
        }
        p->q = ytl_seq_on(&p->tl, PL_BASE_TRIAL);
        rc = ytl_run(&p->q, ops, n);
        if (rc != 0 || p->q.err) { pl_fail(p, "template '%s': a state's table: %s (op %d)", v->def->name, ytl_strerror(rc ? rc : p->q.err), p->q.err_call); return -1; }
        v->ev1 = ytl_add_n(&p->tl, &dummy, 0);
        /* rule 7: the steps end at the cursor; `to` does not move it */
        v->steps_end = v2_anchor(p) + cursor;
        v->steps_pending = r->next != -2;
    }
    return 0;
}

/* Rule 8: a tween of the visit still running at t keeps the value it last
 * wrote: a jump to that value takes its channel over at t. */
static int v2_tweens_stop(pl_player* p, pl_v2* v, int64_t t) {
    int64_t rel = t - v2_anchor(p);
    /* the jump starts where the value was written, the last evaluated
     * frame: a tween the next state starts at t then starts from it */
    int64_t at = v->last_eval_rel < rel ? v->last_eval_rel : rel;
    const float* vals = ytl_values(&p->tl);
    int k;
    if (at < 0) at = 0;
    v->n_frz = 0;
    for (k = 0; k < v->n_tw; k++) {
        ytl_op o[2];
        int rc;
        if (v->tw[k].end <= rel) continue;
        memset(o, 0, sizeof o);
        o[0].op = YTL_OP_AT;
        o[0].t_ns = at;
        o[1].op = YTL_OP_TO;
        o[1].ch = v->tw[k].ch;
        o[1].tween.to = vals[v->tw[k].ch];
        o[1].tween.duration_ns = 0;
        p->q = ytl_seq_on(&p->tl, PL_BASE_TRIAL);
        rc = ytl_run(&p->q, o, 2);
        if (rc != 0 || p->q.err) { pl_fail(p, "template '%s': a tween's stop: %s", v->def->name, ytl_strerror(rc ? rc : p->q.err)); return -1; }
        v->frz[v->n_frz].ch = v->tw[k].ch;
        v->frz[v->n_frz].v = vals[v->tw[k].ch];
        v->n_frz++;
    }
    v->n_tw = 0;
    return 0;
}

/* Take exit x at time t (rule 8: the state's events not yet fired go). */
/* An onset of `from` this trial that has landed or is still due. */
static bool v2_from_due(pl_player* p, const pl_v2* v) {
    ytl_event dummy;
    int id, end;
    memset(&dummy, 0, sizeof dummy);
    end = ytl_add_n(&p->tl, &dummy, 0);
    for (id = v->trial_ev0; id < end; id++) {
        const ytl_event* e = ytl_find(&p->tl, id);
        if (e && e->kind == YTL_ONSET && e->target == v->from_ch && !(e->flags & YTL_EV_SKIPPED)) return true;
    }
    return false;
}

/* An exit (or a steps' end) taken at t on frame f: its lag (the poll's
 * time, the frame's onset in --sim, minus t: trials.csv
 * exit_lag_max_s, session.json exit_lag), and the horizon it used (N1) */
static void v2_took(pl_player* p, pl_v2* v, int64_t t, const yscr_frame* f, int k) {
    pl_v2_session* S = p->v2s;
    int64_t lag = f->onset - t;
    if (t != v->ch_t) { v->ch_t = t; v->ch_n = 0; }
    if (v->ch_n < v->ch_cap) {
        v->ch_rs[v->ch_n] = v->cur;
        v->ch_k[v->ch_n] = k;
        v->ch_dr[v->ch_n] = v->dr_n;
        v->ch_n++;
    }
    if (v->lag_max == YTR_MISSING_NS || lag > v->lag_max) v->lag_max = lag;
    if (S->lag_n == 0 || lag > S->lag_max) S->lag_max = lag;
    S->lag_n++;
    S->lag_sum += (double)lag;
    /* the take decided on what the drains so far gave: a hit of an arm
     * they followed, earlier than their horizon, should have been in */
    if (v->dr_n > 0 && v->dr_g > v->g_used) v->g_used = v->dr_g;
    v->take_dr = v->dr_n;
}

static int v2_take(pl_player* p, pl_v2* v, const v2_exit* x, int64_t t, const yscr_frame* f, bool hit) {
    const v2_rs* r = v->cur >= 0 ? &v->rs[v->cur] : NULL;
    int id;
    if (t == v->take_t) {
        /* the loader refuses every cycle with no SURE wait (D20 rule 6,
         * F26) and a hit is taken once, so one time's changes are bounded
         * by the template; past that is a player bug, stopped by name
         * rather than filling the timeline */
        if (++v->n_take > v->take_cap) {
            pl_fail(p, "template '%s', trial %d: internal guard: more than %d state changes at one time, the template's runtime states and overs",
                    v->def->name, p->global_trial - 1, v->take_cap);
            return -1;
        }
    } else {
        v->take_t = t;
        v->n_take = 0;
    }
    {
        /* rule 8: the state's events not yet fired go, every part's (a
         * tween of an earlier part too), unless the exit stays in the state */
        bool stays = r && r->def >= 0 && x->to >= 0 && v->rs[x->to].def == r->def && !v->rs[x->to].first, recount = false;
        int from = !stays && v->visit_ev0 >= 0 ? v->visit_ev0 : v->ev0;
        /* an events template has no exits: a part's change, or an until's
         * timeout to the events' end, removes nothing and stops no tween,
         * as version 1's `to` runs on into `after` */
        if (v->def->states)
            for (id = from; id < v->ev1; id++) {
                const ytl_event* e = ytl_find(&p->tl, id);
                if (!e || (e->flags & (YTL_EV_FIRED | YTL_EV_SKIPPED))) continue;
                if (v2_anchor(p) + e->time_ns < t) {
                    /* reading F38: due before the exit's time, it happened
                     * and lands in this frame's evaluate (a flag holds for
                     * the next state now); at the exit's time the exit wins */
                    if (e->kind == YTL_MARK && e->code >= PL_MARK_FLAG && e->code < PL_MARK_FLAG + 2 * 4096 && (e->code - PL_MARK_FLAG) / 2 < v->def->n_flags)
                        v->flags[(e->code - PL_MARK_FLAG) / 2] = ((e->code - PL_MARK_FLAG) & 1) != 0;
                    continue;
                }
                if (e->kind == YTL_ONSET && e->target == v->from_ch) recount = true;
                (void)ytl_remove(&p->tl, id);
            }
        /* F34 rule 3: the window is open while `from` has shown or is
         * still due in this trial, whichever visit's step shows it */
        if (recount && !p->from_landed) v->from_pending = v2_from_due(p, v);
        if (!stays && v->def->states) {
            v->arm_rs = -1;
            v->arm_wait = -1;
            if (v2_tweens_stop(p, v, t) < 0) return -1;
        }
        v2_outcome(v, x->outcome, x->outcome_text);
        if (r && r->def >= 0 && !stays) v2_srow_close(p, v, x->code, t);
    }
    v2_took(p, v, t, f, r && x >= r->exits && x < r->exits + r->n_exits ? (int)(x - r->exits) : -1);
    return v2_enter(p, v, x->to, t, f, hit);
}

/* The steps ended with no exit (rule 10): the next part, state or the end. */
static int v2_steps_end(pl_player* p, pl_v2* v, const yscr_frame* f) {
    const v2_rs* r = &v->rs[v->cur];
    int64_t t = v->steps_end;
    int to = r->next;
    int id;
    v->steps_pending = false;
    /* 3.12: a flag the steps set by their end holds when the next state's
     * exits and table read it, though its mark lands later in the frame */
    for (id = v->ev0; id < v->ev1; id++) {
        const ytl_event* e = ytl_find(&p->tl, id);
        if (!e || e->kind != YTL_MARK || (e->flags & (YTL_EV_FIRED | YTL_EV_SKIPPED))) continue;
        if (e->code < PL_MARK_FLAG || e->code >= PL_MARK_FLAG + 2 * 4096 || v2_anchor(p) + e->time_ns > t) continue;
        if ((e->code - PL_MARK_FLAG) / 2 < v->def->n_flags) v->flags[(e->code - PL_MARK_FLAG) / 2] = ((e->code - PL_MARK_FLAG) & 1) != 0;
    }
    if (r->def >= 0) {
        bool recount = false;
        /* rule 7: the steps end at the cursor; a step a backward at_s put
         * after it belongs to a part that has ended (rule 9 makes an
         * until's wait a state of its own), so it never lands */
        for (id = v->ev0; id < v->ev1; id++) {
            const ytl_event* e = ytl_find(&p->tl, id);
            if (!e || (e->flags & (YTL_EV_FIRED | YTL_EV_SKIPPED)) || v2_anchor(p) + e->time_ns <= t) continue;
            if (e->kind == YTL_ONSET && e->target == v->from_ch) recount = true;
            (void)ytl_remove(&p->tl, id);
        }
        if (recount && !p->from_landed) v->from_pending = v2_from_due(p, v);
        if (to < 0 || v->rs[to].def != r->def || v->rs[to].first) v2_srow_close(p, v, -1, t);
    }
    v2_took(p, v, t, f, r->n_exits);
    return v2_enter(p, v, to, t, f, false);
}

/* F34 (2): a response exit; the deadline's outcome is "timeout" unless
 * the exit gives one. */
static int v2_take_resp(pl_player* p, pl_v2* v, const v2_exit* e, int64_t t, const yscr_frame* f, bool responded) {
    v2_exit x = *e;
    if (!responded && !x.outcome && !x.outcome_text) x.outcome_text = "timeout";
    return v2_take(p, v, &x, t, f, true);
}

static int v2_ctl_of_hit(pl_v2* v, int node) {
    int i;
    for (i = 0; i < v->n_ctl; i++) if (v->ctl[i].hit == node || v->ctl[i].hit_u == node) return i;
    return -1;
}

/* The hits drained: their times, the collector's arm, and an exit (rule 4:
 * the earliest pending time, then the list order; an after_s time is
 * pending once a hit is at or after it). */
/* A timed exit's time (D20 rule 2; F41, decided 2026-11-13, "clamp to
 * entry"): the later of the entry frame's onset plus the duration and the
 * entry time. The lead rule can put a between-frames entry's frame before
 * the entry, so the onset plus a short duration can fall before it. The
 * visit's entry for its own exits, the part's (an until's wait) for a
 * part's, and an events template's part is the visit. */
static int64_t v2_after_t(const pl_v2* v, const v2_exit* x, int64_t a) {
    int64_t t = (x->part ? v->entry_onset : v->visit_onset) + a;
    int64_t e = x->part || v->cur < 0 || v->rs[v->cur].def < 0 ? v->entry_t : v->def_entry_t;
    return t > e ? t : e;
}

/* A key response's end that waited for the horizon (resp_pend) is a
 * candidate as any other (docs/player.md, "The order of exits"): before a
 * candidate at tn is taken, a pending end at or before the horizon and
 * before tn (at or before, `tie`, where the caller leaves the tie to
 * v2_resp_states()' list order) goes first. pl_v2_frame_timed() asked for
 * it only after v2_timed() and the feed's v2_hits(), so a timed exit 1 ns
 * after a press won (the model's harness, 500 Hz, 10 ms late, seed
 * 100640). Returns 1 when it took the end. */
static int v2_resp_end(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t t, bool responded);
static int v2_resp_pend_first(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t tn, bool tie) {
    if (!v->resp_pend || v->cur < 0 || v->resp_pend_t > tn || (!tie && v->resp_pend_t == tn) || v->resp_pend_t > v2_horizon(p, v)) return 0;
    v->resp_pend = false;
    return v2_resp_end(p, v, f, v->resp_pend_t, v->resp_pend_responded) < 0 ? -1 : 1;
}

static int v2_hits(pl_player* p, pl_v2* v, const yscr_frame* f) {
    ysig_hit h[V2_HITQ];
    uint32_t hg[V2_HITQ];
    int n, i, k, guard = 0, from;
    int64_t g_to = v2_drain_to(p, v);
    for (;;) {
        int64_t tmax = INT64_MIN;
        n = ysig_detected_to(&v->g, h, V2_HITQ, g_to);
        v->dr_n++;
        v->dr_g = v2_horizon(p, v);
        if (n <= 0) break;
        if (++guard > 256) { pl_fail(p, "template '%s': hits that never stop", v->def->name); return -1; }
        from = 0;
    more:
        for (i = from; i < n; i++) {
            int ci = v2_ctl_of_hit(v, h[i].node);
            hg[i] = v->arm_gen[h[i].node];
            /* an arm's hits are at or after its time, after it for a hold
             * (entry arm): one before is the earlier arm's, also when the
             * re-arm itself found it (sig.h completes the hold met by the
             * arm's time), so it is that arm's generation */
            if (h[i].t < v->arm_t[h[i].node] || (h[i].t == v->arm_t[h[i].node] && !v->det_now[h[i].node])) hg[i]--;
            if (ci < 0) continue;
            if (h[i].t < v->g_used) {
                /* strictly before: a hit at a take's own time that a chain
                 * of takes at that time had not drained lost by rule 4's
                 * list order, as in 0.11.0, not by lateness */
                /* N1: its arm was made before the take that used g_used. A
                 * hit at the latest arm's time can be that arm's (an entry
                 * arm met at once): taken as its, so no false stop */
                int64_t tk = h[i].t >= v->arm_t[h[i].node] ? v->arm_tk[h[i].node] : v->arm_tk_prev[h[i].node];
                if (tk < v->take_dr) return v2_beyond(p, v, "a hit", h[i].t, v->g_used);
            } else if (h[i].t == v->ch_t) {
                /* R5 (i): at a take's own time, a hit of an arm that take
                 * followed whose exit comes earlier in the left state's
                 * list than the one taken would have won by rule 4: it came
                 * late. Each take of the chain at that time, not only the
                 * latest */
                int64_t tk = h[i].t >= v->arm_t[h[i].node] ? v->arm_tk[h[i].node] : v->arm_tk_prev[h[i].node];
                int c, j;
                for (c = 0; c < v->ch_n; c++) {
                    const v2_rs* tr;
                    if (v->ch_rs[c] < 0 || tk >= v->ch_dr[c]) continue;
                    tr = &v->rs[v->ch_rs[c]];
                    for (j = 0; j < v->ch_k[c] && j < tr->n_exits; j++)
                        if (tr->exits[j].kind == X_HIT && tr->exits[j].node == h[i].node) return v2_beyond(p, v, "a hit", h[i].t, v->ch_t);
                }
            }
            v->ctl[ci].hit_t = h[i].t;
            v->ctl[ci].has_hit = true;
            if (h[i].t > tmax) tmax = h[i].t;
            if (v->resp_from_kind == YEXP_REF_CONTROL && v->resp_from_ctl == v->ctl[ci].def) v2_resp_arm(p, v, h[i].t);
        }
        for (;;) {
            const v2_rs* r;
            const v2_exit* best = NULL;
            int64_t bt = INT64_MAX;
            if (v->cur < 0) break;
            r = &v->rs[v->cur];
            for (k = 0; k < r->n_exits; k++) {
                const v2_exit* x = &r->exits[k];
                int64_t tx = INT64_MAX;
                if (x->kind == X_HIT) {
                    /* the state's exits are armed from its entry on, in
                     * every part (F3); an until's from its wait */
                    int64_t since = x->until || r->def < 0 ? v->entry_t : v->def_entry_t;
                    for (i = 0; i < n; i++)
                        if (h[i].node == x->node && hg[i] == v->arm_gen[x->node] && h[i].t >= since && h[i].t < tx) tx = h[i].t;
                } else if (x->kind == X_AFTER && v->entry_onset_known) {
                    int64_t a = 0;
                    if (pl_eval_ns(p, &p->env, x->after, &a) < 0) return -1;
                    int64_t ta = v2_after_t(v, x, a);
                    if (ta <= tmax) tx = ta;
                } else if (x->kind == X_RESPONSE && v->resp_tie_t != INT64_MIN && !v->resp_taken && v->resp_tie_t <= tmax) {
                    /* rule 4: a response that ends by this batch's hits */
                    tx = v->resp_tie_t > v->entry_t ? v->resp_tie_t : v->entry_t;
                }
                if (tx < bt) { bt = tx; best = x; }
            }
            {
                int64_t tn = v->steps_pending && v->steps_end <= tmax && v->steps_end < bt ? v->steps_end : bt;
                int rc = tn == INT64_MAX ? 0 : v2_resp_pend_first(p, v, f, tn, false);
                if (rc < 0) return -1;
                if (rc) {
                    for (i = 0; i < n; i++) if (h[i].t <= v->entry_t) h[i].node = -1;
                    continue;
                }
            }
            if (v->steps_pending && v->steps_end <= tmax && v->steps_end < bt) {
                if (v2_steps_end(p, v, f) < 0) return -1;
                continue;
            }
            if (!best) break;
            if (best->kind == X_RESPONSE && n < V2_HITQ) {
                /* rule 4 at the response's time: an exit taken in this
                 * batch armed the new state's controls at its entry, and
                 * one met at its arm (a region the hand is in) has a hit
                 * at the entry, queued after this batch. It competes with
                 * the response: at one time the list order decides
                 * (format 3.11 rule 4; seed 89320 of the model's harness) */
                int m = ysig_detected_to(&v->g, h + n, V2_HITQ - n, g_to);
                if (m > 0) {
                    from = n;
                    n += m;
                    goto more;
                }
            }
            if (best->kind == X_RESPONSE) {
                v->resp_taken = true;
                if (v2_take_resp(p, v, best, bt, f, v->resp_tie_responded) < 0) return -1;
            } else if (v2_take(p, v, best, bt, f, true) < 0) return -1;
            /* the new state's hits are from its entry on: consumed ones go */
            for (i = 0; i < n; i++) if (h[i].t <= bt) h[i].node = -1;
        }
    }
    return 0;
}

/* Timed exits whose time the frame's window reaches (rule 2: the lead rule). */
static int v2_feed(pl_player* p, const yscr_frame* f, int64_t limit);
/* The timed exits and step ends due before `limit` (strictly), within the
 * frame's window; v2_timed() takes every one the window reaches. */
/* F40 (decided 2026-11-12): a timed exit is never taken on its state's
 * entry frame, so every cycle advances the clock; an until's timeout
 * counts the wait's own entry frame (rule 9 makes it a state). The exit
 * keeps its time and lands on the next frame. */
static bool v2_entry_frame(const pl_v2* v, const v2_exit* x, const yscr_frame* f) {
    return (x->part ? v->entry_onset : v->visit_onset) == f->onset;
}

static int v2_timed_to(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t limit) {
    int64_t wend = 0, guard = 0;
    ytl_frame tf;
    tf.onset = f->onset;
    tf.period = f->period;
    tf.index = f->index;
    if (!ytl_window(&p->tl, PL_BASE_TRIAL, &tf, &wend)) return 0;
    wend += v2_anchor(p);
    if (limit - 1 < wend) wend = limit - 1;
    while (v->cur >= 0) {
        const v2_rs* r = &v->rs[v->cur];
        const v2_exit* best = NULL;
        int64_t bt = INT64_MAX;
        int k;
        if (++guard > 4096) { pl_fail(p, "template '%s': timed exits that never stop", v->def->name); return -1; }
        for (k = 0; k < r->n_exits; k++) {
            const v2_exit* x = &r->exits[k];
            int64_t a = 0;
            if (x->kind != X_AFTER || !v->entry_onset_known) continue;
            if (pl_eval_ns(p, &p->env, x->after, &a) < 0) return -1;
            {
                int64_t ta = v2_after_t(v, x, a);
                if (ta < bt) { bt = ta; best = x; }
            }
        }
        if (v->has_graph) {
            /* rule 4: input is fed up to the frame's onset (or a response's
             * end), so a hold the hand meets by then is a hit before the
             * timed changes and the evaluate; v2_hits() takes a timed exit
             * or a steps' end before it first, and rule 8 drops the steps
             * at or after the hit. ysig_frame() would find it only after
             * the evaluate had landed those steps */
            int64_t fed = limit < f->onset ? limit : f->onset;
            {
                int cur = v->cur;
                int64_t ent = v->entry_t;
                v2_late_in(v, fed);
                if (ysig_settle(&v->g, fed) < 0 || v2_hits(p, v, f) < 0) return -1;
                if (v->cur != cur || v->entry_t != ent) continue;
            }
        }
        {
            /* 0.12.0: with a late role, a timed candidate waits for the
             * horizon, as a hit does: no hit at or before it can come */
            int64_t g = v->has_graph && v->late ? v2_horizon(p, v) : INT64_MAX;
            int64_t tn = v->steps_pending && v->steps_end <= wend && v->steps_end <= bt && v->steps_end <= g ? v->steps_end
                       : best && bt <= wend && bt <= g ? bt : INT64_MAX;
            int rc = tn == INT64_MAX ? 0 : v2_resp_pend_first(p, v, f, tn, true);
            if (rc < 0) return -1;
            if (rc) continue;
            if (v->steps_pending && v->steps_end <= wend && v->steps_end <= bt && v->steps_end <= g) {
                if (v2_steps_end(p, v, f) < 0) return -1;
                continue;
            }
            if (!best || bt > wend || bt > g) break;
        }
        /* F40: due on the entry frame, it waits for the next; what is
         * after it waits too */
        if (v2_entry_frame(v, best, f)) break;
        if (v2_take(p, v, best, bt, f, false) < 0) return -1;
    }
    return 0;
}

static int v2_timed(pl_player* p, pl_v2* v, const yscr_frame* f) { return v2_timed_to(p, v, f, INT64_MAX); }

/* After the response's collector this frame (run.c): the timed exits, so a
 * response's end and an after_s exit at one time go by rule 4's list order
 * (pl_v2_response()) rather than by which the frame met first. */
static int v2_resp_end(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t t, bool responded);
int pl_v2_frame_timed(pl_player* p, const yscr_frame* f) {
    pl_v2* v = p->in_trial && p->tmpl ? p->tmpl->v2 : NULL;
    if (!v) return 0;
    if (v2_timed(p, v, f) < 0) return -1;
    if (v->resp_pend && v->cur >= 0 && v->resp_pend_t <= v2_horizon(p, v)) {
        /* a key response's end that waited for the input up to it */
        v->resp_pend = false;
        if (v2_resp_end(p, v, f, v->resp_pend_t, v->resp_pend_responded) < 0) return -1;
    }
    /* F38, one rule for every way a state starts (a timed exit, a steps'
     * end, the response's end, a hit): a hit its arm makes at once is
     * taken before this frame's evaluate, so the exit wins at its time
     * and the state's steps at that time do not land */
    return v->has_graph ? v2_hits(p, v, f) : 0;
}

/* --- the trial: start, frames ------------------------------------------- */

static int v2_frame_rows_flush(pl_player* p, bool all);

int pl_v2_trial_start(pl_player* p, const yscr_frame* f) {
    pl_v2* v = p->tmpl->v2;
    pl_v2_session* S = p->v2s;
    int i;
    S->cur_t = v->index;
    v->lag_max = YTR_MISSING_NS;   /* the first visit: no exit started it */
    v->resp_pend = false;
    /* the last trial's frame rows read its graph's rings: before the reset */
    if (v2_frame_rows_flush(p, true) < 0) return -1;
    v->ended = false;
    v->end_pending = false;
    v->outcome = NULL;
    v->outcome_text = NULL;
    v->outcome_set = false;
    v->def_cur = -1;
    v->base_onset = -1;
    v->cur = V2_END;
    v->steps_pending = false;
    v->entry_onset_known = false;
    v->resp_t = YRSP_NO_TIME;
    v->events_done = v->resp_done = v->after_built = false;
    v->arm_rs = -1;
    v->arm_wait = -1;
    v->visit_ev0 = -1;
    {
        ytl_event dummy;
        memset(&dummy, 0, sizeof dummy);
        v->trial_ev0 = ytl_add_n(&p->tl, &dummy, 0);
    }
    v->n_tw = 0;
    v->n_frz = 0;
    v->from_pending = false;
    v->resp_ended = v->end_wait = false;
    v->resp_tie_t = INT64_MIN;
    v->resp_taken = false;
    v->last_eval_rel = 0;
    v->events_end_rel = v->resp_bt = 0;
    for (i = 0; i < v->n_states; i++) { v->visits[i] = 0; v->state_onset[i] = -1; v->state_frame[i] = v->state_recf[i] = -1; }
    for (i = 0; i < p->n_stims; i++) { v->stim_onset[i] = -1; v->stim_frame[i] = v->stim_recf[i] = -1; }
    v->end_frame = v->end_recf = -1;
    for (i = 0; i < v->def->n_flags; i++) v->flags[i] = false;
    for (i = 0; i < v->n_ctl; i++) v->ctl[i].has_hit = false;
    for (i = 0; i < v->n_traces; i++) { v->traces[i].nrows = 0; }
    for (i = 0; v->dis_t && i < v->nn; i++) v->dis_t[i] = INT64_MIN;   /* the reset is a barrier */
    v->n_st_armed = 0;
    v->rows_open = false;
    if (v->has_graph) {
        /* 3.6: parameters, the recenter steps at the head, the reset */
        int rc;
        YRT_ZONE(zp, "pl.v2_params");
        rc = v2_params_apply(p, v);
        YRT_ZONE_END(zp);
        if (rc < 0) return -1;

        if (v->def->states && v->def->n_states > 0) {
            const yexp_state* st = &v->def->states[0];
            int k;
            for (k = 0; k < st->n_steps && st->steps[k].op == YEXP_RECENTER; k++) {
                const yexp_step* s = &st->steps[k];
                double x = 0, y = 0;
                if (s->point->kind == YEXP_V_ARRAY) {
                    if (v2_item(p, &s->point->items[0], -1, &x, P_NUM) < 0 || v2_item(p, &s->point->items[1], -1, &y, P_NUM) < 0) return -1;
                } else if (v2_item(p, s->point, 0, &x, P_NUM) < 0 || v2_item(p, s->point, 1, &y, P_NUM) < 0) return -1;
                {
                    int ti;
                    for (ti = 0; ti < S->n_t; ti++)
                        if (S->t[ti] && S->t[ti]->has_graph && !ysig_recenter(&S->t[ti]->g, s->target.index, x, y, f->onset)) S->recenter_skipped++;
                }
            }
        }
        {
            /* the reset is a barrier: a report stamped before its time that
             * arrives after it is dropped (late_barrier), and the held value
             * it carried would be stale in this trial. At its onset the input
             * is complete only to G (below P while a bound is above 0), so
             * the barrier goes at G + 1, and this trial's arms at the onset
             * wait in sig.h for input past them (an arm ahead of the input).
             * Every bound 0: G is P, the onset */
            int64_t ga = v2_horizon_all(p), tb = f->onset;
            if (S->bnd_max > 0 && ga != INT64_MIN && ga < tb - 1) tb = ga + 1;
            ysig_reset_trial(&v->g, tb);
        }
        {
            /* the newest stamps are this trial's (the reset dropped the
             * rest); hits and events of an earlier trial do not come */
            int r;
            for (r = 0; r < p->def->n_sigroles; r++) S->newest[r] = INT64_MIN;
            v->g_used = v->g_der = INT64_MIN;
            v->ch_t = INT64_MIN;
            v->ch_n = 0;
            v->arm_min = INT64_MAX;
        }
        {
            ysig_stats st;
            ysig_get_stats(&v->g, &st);
            v->late_frames0 = st.late_frames;   /* the trial's late_frames column */
        }
        v->rows_open = true;   /* the rows the reset made final were the last trial's */
        /* every control whose hits no exit names arms at the trial's start */
        for (i = 0; i < v->n_ctl; i++)
            if (!v->ctl[i].armed_by_exit && v2_arm_ctl(p, v, i, f->onset) < 0) return -1;
    }
    if (p->tmpl->collector) {
        const yexp_response* r = v->def->response;
        int64_t dur = 0, minrt = 0;
        yrsp_collector* c = p->tmpl->collector;
        if (pl_eval_ns(p, &p->env, r->duration, &dur) < 0 || pl_eval_ns(p, &p->env, r->min_rt, &minrt) < 0) return -1;
        if (pl_window_check(p, v->def, dur, minrt) < 0) return -1;
        v->resp_dur = dur;
        v->resp_minrt = minrt;
        /* signal choices with a from: armed again there (D34 (c)); this
         * arm only clears the last trial's result */
        yrsp_arm(c, f->onset);
        yrsp_set_window(c, minrt, dur);
        yrsp_set_wait_final(c, PL_WAIT_FINAL_NS);
        if (r->signal_choices && v->resp_from_kind == YEXP_REF_NONE) v2_resp_arm(p, v, f->onset);
        p->have_live = false;
        p->responding = true;
        p->env.key_answer_known = false;
        if (r->key_answer) {
            if (pl_eval_text(p, &p->env, r->key_answer, p->key_answer, p->max_text + 1) < 0) return -1;
            p->env.key_answer_known = true;
        }
    }
    p->from_landed = false;
    p->from_frame = -1;
    p->press_pending = false;
    p->in_trial = true;
    if (v->def->states || v->n_rs > 0) {
        /* the first state's lowering: its steps to timeline ops */
        int rc;
        YRT_ZONE(ze, "pl.v2_enter");
        v->trial_entry = true;
        rc = v2_enter(p, v, 0, f->onset, f, false);
        v->trial_entry = false;
        YRT_ZONE_END(ze);
        if (rc < 0) return -1;
    }
    if (v->has_graph && v2_hits(p, v, f) < 0) return -1;
    return 0;
}

/* The scripted samples up to the frame's onset, each through every graph
 * (an adapter keeps its state across templates); the active one's hits
 * and derived events in rule 4's order. */
int pl_v2_frame_input(pl_player* p, const yscr_frame* f) {
    pl_v2_session* S = p->v2s;
    if (!S) return 0;
    if (!S->t0_set) { S->t0 = f->onset; S->t0_set = true; }
    return v2_feed(p, f, INT64_MAX);
}

/* A part of a poll's samples through every graph: one ysig_feed_n() each
 * (sig.h 0.7.0 places a late one at its time, with one run again at the
 * part's end), then the active graph's hits, taken by time (v2_hits()),
 * and its derived events, each one earlier than a hit's arm of the
 * collector before that arm (v2_resp_arm()). Format 3.11 rule 4 feeds each
 * event alone; --test-feed-each does, to check the results match. */
static int v2_feed_part(pl_player* p, pl_v2* v, const yscr_frame* f, int n) {
    pl_v2_session* S = p->v2s;
    int ti;
    for (ti = 0; ti < S->n_t; ti++) {
        pl_v2* w = S->t[ti];
        if (!w || !w->has_graph) continue;
        if (n == 1) (void)ysig_feed(&w->g, S->batch);
        else (void)ysig_feed_n(&w->g, S->batch, n);
        if (w == v) {
            int k;
            int64_t gd = v2_horizon(p, v);
            v->n_der = ysig_derived_to(&w->g, v->der, v->der_cap, v2_drain_to(p, v));
            if (v->n_der < 0) v->n_der = 0;
            if (p->tmpl->collector)
                for (k = 0; k < v->n_der; k++)
                    if (v->der[k].t < v->g_der && v->der[k].t < v->arm_min) return v2_beyond(p, v, "a derived event", v->der[k].t, v->g_der);
            if (gd > v->g_der) v->g_der = gd;
            v->arm_min = INT64_MAX;
            if (v2_hits(p, v, f) < 0) return -1;
            for (k = 0; k < v->n_der; k++) if (p->tmpl->collector) yrsp_feed(p->tmpl->collector, &v->der[k]);
            v->n_der = 0;
        } else {
            ysig_hit h[V2_HITQ];
            yin_event der[64];
            while (ysig_detected(&w->g, h, V2_HITQ) > 0) { }
            while (ysig_derived(&w->g, der, 64) > 0) { }
        }
    }
    return 0;
}

/* The samples up to the frame's onset and `limit` (a key response's end,
 * pl_v2_response()), through every graph, the active one's hits. */
static int v2_feed(pl_player* p, const yscr_frame* f, int64_t limit) {
    pl_v2_session* S = p->v2s;
    pl_v2* v = p->in_trial && p->tmpl ? p->tmpl->v2 : NULL;
    int ti, cap;
    if (!S || !S->t0_set) return 0;
    if (v && !v->has_graph) v = NULL;
    cap = p->args.feed_each ? 1 : S->batch_cap;
    {
    int fed = 0, rc = 0, nb = 0;
    YRT_ZONE(zs, "sig.samples");
    while (rc == 0 && S->next_smp < S->n_smp && S->t0 + S->smp[S->next_smp].arr <= f->onset && S->t0 + S->smp[S->next_smp].t <= limit) {
        const struct v2_sample* sm = &S->smp[S->next_smp++];
        yin_event* e = &S->batch[nb++];
        memset(e, 0, sizeof *e);
        e->t = S->t0 + sm->t;
        {
            const yexp_sigrole* sr = &p->def->sigroles[sm->role];
            bool rel = sr->type == YEXP_SIG_POSITION2D_RELATIVE || (sr->type == YEXP_SIG_POSITION2D && sr->allow_relative);
            e->kind = rel ? YIN_KIND_MOUSE : YIN_KIND_PEN;
            e->control = rel ? YIN_AXIS_DELTA : YIN_AXIS_POSITION;
        }
        e->type = YIN_SAMPLE;
        e->device = (uint32_t)sm->role + 1;
        /* reported at or before the role's complete-to time of the last
         * poll: later than its bound (late_beyond_bound) */
        if (S->polled && e->t <= (S->poll - S->bnd[sm->role] < S->plim ? S->poll - S->bnd[sm->role] : S->plim)) S->beyond[sm->role]++;
        if (e->t > S->newest[sm->role]) S->newest[sm->role] = e->t;
        e->x = sm->x;
        e->y = sm->y;
        e->stamp = YIN_TIER_SIM;
        S->n_sample_rows++;
        for (ti = 0; ti < S->n_t; ti++)
            if (S->t[ti] && S->t[ti]->has_graph) v2_late_in(S->t[ti], e->t);
        fed++;
        if (nb == cap) { rc = v2_feed_part(p, v, f, nb); nb = 0; }
    }
    if (rc == 0 && nb) rc = v2_feed_part(p, v, f, nb);
    YRT_ZONE_VALUE(zs, fed);
    YRT_ZONE_END(zs);
    if (rc < 0) return -1;
    }
    /* the poll is whole: input arrived by P is in, up to the limit */
    S->poll = f->onset;
    S->plim = limit;
    S->polled = true;
    return 0;
}

const float* pl_v2_values(pl_player* p, const float* tl_values) {
    pl_v2_session* S = p->v2s;
    memcpy(S->vals, tl_values, sizeof(float) * (size_t)p->n_channels);
    return S->vals;
}

const ygfx_bind* pl_v2_binds(pl_player* p, int* n) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    if (!v) { *n = p->n_binds; return p->binds; }
    *n = v->n_binds;
    return v->binds;
}

/* The trace's window (3.7): from the latest time of `from`, up to `to`. */
static int64_t v2_name_time(pl_player* p, pl_v2* v, const yexp_ref* r, bool* known) {
    (void)p;
    *known = true;
    switch (r->kind) {
    case YEXP_REF_STIMULUS: if (v->stim_onset[r->index] >= 0) return v->stim_onset[r->index]; break;
    case YEXP_REF_CONTROL: {
        int ci = v2_ctl_index_of(v, r->control);
        if (ci >= 0 && v->ctl[ci].has_hit) return v->ctl[ci].hit_t;
        break;
    }
    case YEXP_REF_STATE: if (v->state_onset[r->index] >= 0) return v->state_onset[r->index]; break;
    case YEXP_REF_RESPONSE: if (v->resp_t != YRSP_NO_TIME) return v->resp_t; break;
    case YEXP_REF_END: if (v->ended || v->end_pending) return v->end_onset; break;
    default: break;
    }
    *known = false;
    return 0;
}

static int v2_frame_rows_queue(pl_player* p, pl_v2* v, const yscr_frame* f) {
    pl_v2_session* S = p->v2s;
    int i;
    for (i = 0; i < v->n_traces; i++) {
        v2_trace* tr = &v->traces[i];
        bool known = true;
        int64_t from = tr->def->from.kind == YEXP_REF_NONE ? p->trial_start_onset : v2_name_time(p, v, &tr->def->from, &known);
        if (!tr->frame || !known || f->onset < from) continue;
        if (tr->def->to.kind != YEXP_REF_NONE && tr->def->to.kind != YEXP_REF_END) {
            bool k2;
            int64_t to = v2_name_time(p, v, &tr->def->to, &k2);
            if (k2 && (tr->def->to.kind == YEXP_REF_STATE ? f->onset >= to : f->onset > to)) continue;
        }
        if (v->end_pending) continue;   /* the end frame is out when `end` is `to` */
        if (S->n_fpend >= S->cap_fpend) { pl_fail(p, "internal: frame trace rows: more than %d wait for their records", S->cap_fpend); return -1; }
        S->fpend[S->n_fpend].frame = f->index;
        S->fpend[S->n_fpend].planned = f->onset;
        S->fpend[S->n_fpend].trial = p->global_trial - 1;
        S->fpend[S->n_fpend].tmpl = v->index * 64 + i;
        S->fpend[S->n_fpend].snap = false;
        S->fpend[S->n_fpend].flags = 0;
        S->n_fpend++;
    }
    return 0;
}

static int v2_trace_stream(pl_player* p, pl_v2* v, v2_trace* tr);

typedef struct v2_srow_data { int32_t trial; int32_t tier; int32_t device; int32_t flags; int64_t frame, planned, t; double v[32]; } v2_srow_data;
/* the writer's side buffer holds one in each slot (pl_wq_push_row) */
typedef char v2_srow_fits[sizeof(v2_srow_data) <= PL_WQ_ROW_BYTES ? 1 : -1];

static int v2_frame_row_write(pl_player* p, const v2_frow* fr, int64_t onset, bool have) {
    pl_v2_session* S = p->v2s;
    pl_v2* v = S->t[fr->tmpl / 64];
    v2_trace* tr = &v->traces[fr->tmpl % 64];
    v2_srow_data row;
    int k, c = 0;
    if (v2_trace_stream(p, v, tr) < 0) return -1;
    memset(&row, 0, sizeof row);
    row.trial = fr->trial;
    row.frame = fr->frame;
    row.planned = fr->planned;
    row.t = have ? onset : YTR_MISSING_NS;
    row.flags = fr->flags | (have ? 0 : V2_FR_NO_RECORD);
    if (fr->snap) {
        memcpy(row.v, fr->vals, sizeof row.v);
        return pl_wq_push_row(p, tr->stream, &row, sizeof row);
    }
    for (k = 0; k < tr->n_items; k++) {
        ysig_value val;
        int l;
        if (ysig_value_at(&v->g, tr->item_node[k], have ? onset : fr->planned, &val) < 0) {
            const ysig_value* y = ysig_value_of(&v->g, tr->item_node[k]);
            if (!y) { pl_fail(p, "the trace '%s': an item has no value in the graph", tr->def->name); return -1; }
            val = *y;
        }
        for (l = 0; l < tr->item_lanes[k] && c < 32; l++) row.v[c++] = val.v[l];
    }
    /* the writer's: a real run's frame thread never calls write() */
    return pl_wq_push_row(p, tr->stream, &row, sizeof row);
}

/* Frame rows whose record came; with all, every one left (planned_s only). */
static int v2_frame_rows_flush(pl_player* p, bool all) {
    pl_v2_session* S = p->v2s;
    int i;
    if (!S || !all) return 0;
    /* the next trial's reset empties the rings: the waiting rows keep their
     * values at planned_s and wait on for their record's t_s (reading F29) */
    for (i = 0; i < S->n_fpend; i++) {
        v2_frow* fr = &S->fpend[i];
        pl_v2* v = S->t[fr->tmpl / 64];
        v2_trace* tr = &v->traces[fr->tmpl % 64];
        int k, c = 0;
        if (fr->snap) continue;
        memset(fr->vals, 0, sizeof fr->vals);
        for (k = 0; k < tr->n_items; k++) {
            ysig_value val;
            int l;
            if (ysig_value_at(&v->g, tr->item_node[k], fr->planned, &val) < 0) {
                const ysig_value* y = ysig_value_of(&v->g, tr->item_node[k]);
                if (!y) { pl_fail(p, "the trace '%s': an item has no value in the graph", tr->def->name); return -1; }
                val = *y;
            }
            for (l = 0; l < tr->item_lanes[k] && c < 32; l++) fr->vals[c++] = val.v[l];
        }
        fr->snap = true;
        fr->flags |= V2_FR_AT_PLANNED;
    }
    return 0;
}

/* The session's end: frame trace rows whose record never came, flagged. */
int pl_v2_finish(pl_player* p) {
    pl_v2_session* S = p->v2s;
    int i;
    if (!S) return 0;
    for (i = 0; i < S->n_fpend; i++) if (v2_frame_row_write(p, &S->fpend[i], 0, false) < 0) return -1;
    S->n_fpend = 0;
    return 0;
}

int pl_v2_on_record(pl_player* p, const yscr_record* rec) {
    pl_v2_session* S = p->v2s;
    int i, m = 0;
    if (!S) return 0;
    for (i = 0; i < S->n_fpend; i++) {
        if (S->fpend[i].frame == rec->index) {
            if (v2_frame_row_write(p, &S->fpend[i], rec->onset ? rec->onset : rec->planned, rec->onset != 0) < 0) return -1;
            continue;
        }
        if (rec->index >= S->fpend[i].frame + p->out_wait_frames) {
            /* its record did not come in the rows' bound: t_s empty, flagged */
            if (v2_frame_row_write(p, &S->fpend[i], 0, false) < 0) return -1;
            continue;
        }
        S->fpend[m++] = S->fpend[i];
    }
    S->n_fpend = m;
    for (i = 0; i < S->n_srow; i++)
        if (S->srow[i].frame == rec->index && !S->srow[i].rec_in) {
            S->srow[i].rec_in = true;
            S->srow[i].onset = rec->onset ? rec->onset : rec->planned;
            S->srow[i].residual = rec->onset ? rec->residual : YTR_MISSING_NS;
        }
    return v2_srow_flush(p, false);
}

/* After ytl_evaluate(): the graph's frame (3.6), the hits it found (rule 5),
 * the frame rows, and the trial's end after its ysig_frame (3.10). */
int pl_v2_frame_eval(pl_player* p, const yscr_frame* f) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    int guard = 0;
    if (!v || !p->in_trial) return 0;
    v->last_eval_rel = f->onset - v2_anchor(p);   /* the frame's values are written */
    if (v->has_graph) {
        ysig_frame_in fi;
        memset(&fi, 0, sizeof fi);
        fi.index = f->index;
        fi.onset_ns = f->onset;
        fi.base_ns = v->base_state >= 0 ? (v->base_onset >= 0 ? f->onset - v->base_onset : 0) : f->onset - p->trial_start_onset;
        fi.values = ytl_values(&p->tl);
        fi.n_values = p->n_channels;
        fi.poll_ns = f->onset;
        fi.has_poll = true;
        pl_v2_values(p, ytl_values(&p->tl));
        {
            /* the graph's frame, its node count the zone's value (sig.h has
             * no zones of its own: it does not include ysp/rt.h) */
            int rf;
            YRT_ZONE(zs, "sig.frame");
            YRT_ZONE_VALUE(zs, v->g.n);
            v2_late_in(v, fi.onset_ns);
            rf = ysig_frame(&v->g, &fi);
            YRT_ZONE_END(zs);
            if (rf < 0) { pl_fail(p, "template '%s': ysig_frame", v->def->name); return -1; }
        }
        for (;;) {
            int ev0 = v->ev0, ev1 = v->ev1, cur = v->cur;
            bool endp = v->cur == V2_END;
            if (++guard > 64) { pl_fail(p, "template '%s': a frame's hits never settle", v->def->name); return -1; }
            if (v2_hits(p, v, f) < 0) return -1;
            if (v2_timed(p, v, f) < 0) return -1;
            if (v->cur == cur && v->ev0 == ev0 && v->ev1 == ev1 && (v->cur == V2_END) == endp) break;
            {
                /* rule 5: a state a frame's hit started lands on this frame:
                 * the timeline again (an equal onset fires only what was
                 * added since), its late flags cleared */
                int n = ytl_evaluate(&p->tl, &(ytl_frame){ f->onset, f->period, f->index }, p->fired, p->fired_cap), k;
                if (n < 0) { pl_fail(p, "ytl_evaluate: %s", ytl_strerror(n)); return -1; }
                for (k = 0; k < n; k++) { p->fired[k].flags &= (uint16_t)~YTL_EV_LATE; p->fired[k].residual = 0; }
                if (pl_on_fired(p, f, n) < 0) return -1;
                pl_v2_values(p, ytl_values(&p->tl));
            }
        }
    }
    {
        int rc;
        YRT_ZONE(zq, "pl.v2_frame_rows");
        rc = v2_frame_rows_queue(p, v, f);
        YRT_ZONE_END(zq);
        if (rc < 0) return -1;
    }
    /* sig.h counts at an input too (a late report placed with a run again,
     * a group an input lets in): every graph, each frame */
    if (v2_guard(p, v, "the frame's input", NULL, 0) < 0) return -1;
    if (v->end_pending) {
        v->end_pending = false;
        v->ended = true;
        if (pl_end_trial(p, f) < 0) return -1;
    }
    return 0;
}

/* A version 2 mark (pl_on_fired): the end, a flag, a recenter. */
int pl_v2_on_mark(pl_player* p, const ytl_event* e) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    if (!v) return 0;
    if (e->code == PL_MARK_END) {
        v->end_pending = true;
        v->end_onset = e->onset;
        v->end_frame = e->frame;
        return 1;
    }
    if (e->code >= PL_MARK_FLAG && e->code < PL_MARK_FLAG + 2 * 4096) {
        int k = (e->code - PL_MARK_FLAG) / 2;
        if (k < v->def->n_flags) v->flags[k] = ((e->code - PL_MARK_FLAG) & 1) != 0;
        return 1;
    }
    if (e->code >= PL_MARK_RECENTER && e->code < PL_MARK_RECENTER + 4096) {
        const yexp_step* s = v->recenter && e->code - PL_MARK_RECENTER < v->n_recenter ? v->recenter[e->code - PL_MARK_RECENTER] : NULL;
        pl_v2_session* S = p->v2s;
        double x = 0, y = 0;
        int ti;
        if (!s) return 1;
        if (s->point->kind == YEXP_V_ARRAY) {
            if (v2_item(p, &s->point->items[0], -1, &x, P_NUM) < 0 || v2_item(p, &s->point->items[1], -1, &y, P_NUM) < 0) return -1;
        } else if (v2_item(p, s->point, 0, &x, P_NUM) < 0 || v2_item(p, s->point, 1, &y, P_NUM) < 0) return -1;
        for (ti = 0; ti < S->n_t; ti++) {
            if (S->t[ti] && S->t[ti]->has_graph && !ysig_recenter(&S->t[ti]->g, s->target.index, x, y, e->onset)) S->recenter_skipped++;
            if (S->t[ti] && e->onset < S->t[ti]->arm_min) S->t[ti]->arm_min = e->onset;   /* R5 (ii) */
        }
        if (v->arm_wait >= 0 && e->id == v->arm_wait_id && v->cur == v->arm_wait) {
            /* the until after a recenter arms at the call (3.12) */
            const v2_rs* r = &v->rs[v->arm_wait];
            int k;
            for (k = 0; k < r->n_exits; k++)
                if (v2_arms(r, k, v->arm_wait + 1) && v2_arm_exit(p, v, &r->exits[k], e->onset) < 0) return -1;
            v->arm_wait = -1;
        }
        if (v->arm_rs >= 0 && s == v->arm_after && v->rs[v->arm_rs].def == v->def_cur) {
            /* 3.12: the visit's exits arm at the recenter's call */
            const v2_rs* r = &v->rs[v->arm_rs];
            int k;
            v->arm_rs = -1;
            for (k = 0; k < r->n_exits; k++)
                if (v2_arms(r, k, -1) && v2_arm_exit(p, v, &r->exits[k], e->onset) < 0) return -1;
        }
        return 1;
    }
    return 0;
}

/* A stimulus's first onset in the trial (3.9: the plan until the record). */
void pl_v2_on_onset(pl_player* p, const ytl_event* e, int stim);
void pl_v2_on_onset(pl_player* p, const ytl_event* e, int stim) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    if (!v || stim < 0) return;
    if (v->stim_onset[stim] < 0) { v->stim_onset[stim] = e->onset; v->stim_frame[stim] = e->frame; }
    if (v->resp_from_kind == YEXP_REF_STIMULUS && v->resp_from_index == stim && v->stim_frame[stim] == e->frame) v2_resp_arm(p, v, e->onset);
}

/* The time the response ended (2.6, F34): the entry that ended the
 * window (the press, the max_responses-th one), at its release with
 * wait_for_key_release; else the window's end. */
static int64_t v2_resp_end_time(pl_player* p, pl_v2* v, const yrsp_result* r, const yscr_frame* f) {
    const yexp_response* rd = v->def->response;
    int n = 0, i;
    const yrsp_entry* e = p->tmpl->collector ? yrsp_entries(p->tmpl->collector, &n) : NULL;
    for (i = 0; e && i < n; i++)
        if (e[i].flags & YRSP_E_ENDED) {
            if (rd->wait_for_key_release && (e[i].flags & YRSP_E_RELEASED) && !(e[i].flags & YRSP_E_CROSSING)) return e[i].t_release;
            return e[i].t;
        }
    return r->t_onset && v->resp_dur > 0 ? r->t_onset + v->resp_dur : f->onset;
}

/* The response's end at t takes the current state's response exit, after
 * what is due by t (pl_v2_response()). With v->resp_tie_t set, every
 * v2_hits() on the way offers the response too (rule 4). */
static int v2_resp_states(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t t, bool responded) {
    int k, guard = 0;
    if (v->resp_tie_t != INT64_MIN) {
        int rc = v2_feed(p, f, t);
        /* input up to t is fed: a hold the hand met by then is a hit
         * before the response's end at t (rule 4, the model's
         * future-arm-hold) */
        if (rc == 0 && v->has_graph && !v->resp_taken && v->cur >= 0) {
            v2_late_in(v, t);
            if (ysig_settle(&v->g, t) == 0) rc = v2_hits(p, v, f);
        }
        if (rc < 0) return -1;
    }
    for (;;) {
        bool moved = false;
        if (v->resp_taken || v->cur < 0) return 0;
        if (++guard > v->take_cap) { pl_fail(p, "template '%s': internal guard: a response's end passed %d state changes at one time", v->def->name, v->take_cap); return -1; }
        if (v2_timed_to(p, v, f, t) < 0) return -1;
        if (v->resp_taken || v->cur < 0) return 0;
        if (v->steps_pending && v->steps_end <= t) {
            /* steps that end at the response's time end first (as in
             * v2_timed()): the next state hears the end */
            if (v2_steps_end(p, v, f) < 0) return -1;
            continue;
        }
        if (v->resp_tie_t != INT64_MIN && v->has_graph) {
            /* hits still queued (an arm at the entry of a state that a
             * timed exit or a step end led to): at t the list order
             * decides */
            int cur = v->cur;
            int64_t et = v->entry_t;
            if (v2_hits(p, v, f) < 0) return -1;
            if (v->resp_taken || v->cur < 0) return 0;
            if (v->cur != cur || v->entry_t != et) continue;   /* a hit's exit moved it */
        }
        for (k = 0; k < v->rs[v->cur].n_exits && !moved; k++) {
            const v2_exit* e = &v->rs[v->cur].exits[k];
            if (e->kind == X_AFTER && v->entry_onset_known) {
                int64_t a = 0;
                if (pl_eval_ns(p, &p->env, e->after, &a) < 0) return -1;
                /* an after_s exit listed first at the same time: it goes,
                 * and the next state's response exit still hears the end */
                if (v2_after_t(v, e, a) == t) {
                    if (v2_take(p, v, e, t, f, false) < 0) return -1;
                    moved = true;
                }
            } else if (e->kind == X_RESPONSE) {
                v->resp_taken = true;
                return v2_take_resp(p, v, e, t > v->entry_t ? t : v->entry_t, f, responded);
            }
        }
        if (!moved) return 0;
    }
}

/* A key response that ended (3.11 rule 11): an exit on it. */
int pl_v2_response(pl_player* p, const yrsp_result* r, const yscr_frame* f);
static int v2_resp_end(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t t, bool responded);
int pl_v2_response(pl_player* p, const yrsp_result* r, const yscr_frame* f) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    bool responded = (r->flags & YRSP_R_RESPONDED) != 0;
    int64_t t;
    if (!v) return 0;
    if (responded) v->resp_t = r->t_response;
    if (v->resp_ended) return 0;   /* the trial's end asks again */
    v->resp_ended = true;
    t = v2_resp_end_time(p, v, r, f);
    if (v->end_wait) {
        /* F34 (3): the states reached `end` while the window was open; the
         * trial ends with the response, as version 1's `after` starts at the
         * later of the events' end and the response's (2.5) */
        v->end_wait = false;
        return v2_end_mark(p, v, f->onset > v->end_wait_t ? f->onset : v->end_wait_t);
    }
    if (v->cur < 0) return 0;
    if (v->has_graph && v->late) {
        /* 0.12.0: the response's end is a candidate at its time, taken once
         * the input is complete to it (docs/player.md, "The response
         * window"); until then it waits, and pl_v2_frame_timed() asks */
        if (v2_feed(p, f, t) < 0) return -1;
        if (t > v2_horizon(p, v)) {
            v->resp_pend = true;
            v->resp_pend_t = t;
            v->resp_pend_responded = responded;
            return 0;
        }
    }
    return v2_resp_end(p, v, f, t, responded);
}

/* The response's end at t, final and at or before the horizon */
static int v2_resp_end(pl_player* p, pl_v2* v, const yscr_frame* f, int64_t t, bool responded) {
    /* F34 (2) and rule 4: a press or the deadline takes the `on:
     * "response"` exits of whichever state is current at the response's
     * end, after what is due before it: a key response's hits (the samples
     * up to its end), timed exits, step ends, chains through empty or
     * instant states; at its time the list order decides */
    if (!v->def->response->signal_choices) {
        /* the response competes with every hit drained until it is taken,
         * the hits of a state entered on the way included (an arm met at
         * once has a hit at the entry; seed 89320 of the model's harness) */
        int rc;
        v->resp_tie_t = t;
        v->resp_tie_responded = responded;
        v->resp_taken = false;
        rc = v2_resp_states(p, v, f, t, responded);
        v->resp_tie_t = INT64_MIN;
        return rc;
    }
    v->resp_taken = false;
    return v2_resp_states(p, v, f, t, responded);
}

/* --- steps (3.12) and forms' ops (3.10) ---------------------------------- */

static int v2_op(ytl_op* ops, int* c, int cap, int op, int ch, float value, int32_t code) {
    if (*c + 1 > cap) return -1;
    memset(&ops[*c], 0, sizeof ops[*c]);
    ops[*c].op = op;
    ops[*c].ch = ch;
    ops[*c].value = value;
    ops[*c].code = code;
    (*c)++;
    return 0;
}

/* A constant step on a property that forms drive: every selector to 0. */
int pl_v2_clear_ops(pl_player* p, const char* stim, const char* prop, ytl_op* ops, int c, int cap);
int pl_v2_clear_ops(pl_player* p, const char* stim, const char* prop, ytl_op* ops, int c, int cap) {
    pl_v2* v = v2_cur(p);
    int i, j;
    if (!v) return c;
    if (!prop) prop = "visible";
    for (i = 0; i < v->n_props; i++) {
        if (strcmp(v->props[i].stim, stim) != 0) continue;
        if (strcmp(v->props[i].prop, prop) != 0 && !(strcmp(prop, "color") == 0 && v->props[i].comp >= 0)) continue;
        for (j = 0; j < v->props[i].n_forms; j++)
            if (v2_op(ops, &c, cap, YTL_OP_SET_VALUE, v->props[i].forms[j].ch_sel, 0.0f, 0) < 0) { pl_fail(p, "the op table is full"); return -1; }
    }
    return c;
}

static int v2_vch_value(pl_player* p, const v2_vch* q, double* out) {
    if (q->comp >= 0) {
        pl_val arr;
        if (p->env.extreme && q->v->kind != YEXP_V_ARRAY) { *out = 0.5; return 0; }
        if (pl_eval(p, &p->env, q->v, &arr) < 0) return -1;
        if (arr.kind != YEXP_V_ARRAY || arr.n != 3) { pl_fail(p, "a color's form value is not [r, g, b]"); return -1; }
        return v2_num(p, &arr.items[q->comp], out);
    }
    return v2_num(p, q->v, out);
}

/* Version 2's steps and a `set` with a form. Returns the new count, -1 on
 * an error, -2 when the step is version 1's (pl_build_ops() does it). */
int pl_v2_step_ops(pl_player* p, const yexp_step* s, ytl_op* ops, int c, int cap);
int pl_v2_step_ops(pl_player* p, const yexp_step* s, ytl_op* ops, int c, int cap) {
    pl_v2* v = v2_cur(p);
    int k;
    switch (s->op) {
    case YEXP_FLAG: {
        int f = -1;
        if (v) for (k = 0; k < v->def->n_flags; k++) if (strcmp(v->def->flags[k], s->flag) == 0) f = k;
        if (f < 0) { pl_fail(p, "the flag '%s' is not the template's", s->flag); return -1; }
        if (v2_op(ops, &c, cap, YTL_OP_MARK, 0, 0, PL_MARK_FLAG + 2 * f + (s->flag_value ? 1 : 0)) < 0) { pl_fail(p, "the op table is full"); return -1; }
        return c;
    }
    case YEXP_RECENTER: {
        int idx = -1;
        if (v) for (k = 0; k < v->n_recenter; k++) if (v->recenter[k] == s) idx = k;
        if (idx < 0) return c;
        if (v->trial_entry && v->def->states && v2_head_recenter(&v->def->states[0], s)) return c;   /* ran at the trial's start (3.6) */
        if (v2_op(ops, &c, cap, YTL_OP_MARK, 0, 0, PL_MARK_RECENTER + idx) < 0) { pl_fail(p, "the op table is full"); return -1; }
        return c;
    }
    case YEXP_UNTIL:
        pl_fail(p, "internal: an until in a state's table");
        return -1;
    case YEXP_SET: {
        bool any = false;
        if (!v) return -2;
        for (k = 0; k < s->n_props; k++) if (v2_is_form(s->props[k].value)) any = true;
        if (!any) {
            /* constant: version 1's ops, then the selectors to 0 */
            return -2;
        }
        for (k = 0; k < s->n_props; k++) {
            const yexp_value* f = s->props[k].value;
            int comp, n_comp = strcmp(s->props[k].key, "color") == 0 ? 3 : 1;
            for (comp = 0; comp < n_comp; comp++) {
                static const char* const cn[3] = { "color_r", "color_g", "color_b" };
                const char* pn = n_comp == 3 ? cn[comp] : s->props[k].key;
                int pi, j, sel = -1, q;
                for (pi = 0; pi < v->n_props; pi++)
                    if (strcmp(v->props[pi].stim, s->stims[0]) == 0 && strcmp(v->props[pi].prop, pn) == 0) break;
                if (!v2_is_form(f)) {
                    /* a constant beside forms in one step */
                    int ch = pl_channel(p, s->stims[0], strcmp(pn, "visible") == 0 ? NULL : pn);
                    double val;
                    if (ch < 0) { pl_fail(p, "'%s.%s' has no channel", s->stims[0], pn); return -1; }
                    if (n_comp == 3) {
                        pl_val arr;
                        if (p->env.extreme && f->kind != YEXP_V_ARRAY) val = 0.5;
                        else {
                            if (pl_eval(p, &p->env, f, &arr) < 0) return -1;
                            if (arr.kind != YEXP_V_ARRAY || arr.n != 3) { pl_fail(p, "'%s.color' is not [r, g, b]", s->stims[0]); return -1; }
                            if (v2_num(p, &arr.items[comp], &val) < 0) return -1;
                        }
                    } else if (v2_num(p, f, &val) < 0) return -1;
                    if (v2_op(ops, &c, cap, YTL_OP_SET_VALUE, ch, (float)val, 0) < 0) { pl_fail(p, "the op table is full"); return -1; }
                    if (pi < v->n_props && (c = pl_v2_clear_ops(p, s->stims[0], pn, ops, c, cap)) < 0) return -1;
                    continue;
                }
                if (pi == v->n_props) { pl_fail(p, "internal: '%s.%s' has no forms", s->stims[0], pn); return -1; }
                {
                    char key[256];
                    size_t used = 0;
                    key[0] = 0;
                    v2_form_text(f, key, sizeof key, &used);
                    for (j = 0; j < v->props[pi].n_forms; j++) if (strcmp(v->props[pi].forms[j].key, key) == 0) sel = j;
                }
                if (sel < 0) { pl_fail(p, "internal: a form of '%s.%s' has no selector", s->stims[0], pn); return -1; }
                /* the form's literal channels first, then its selector 1, the others 0 */
                for (q = 0; q < v->n_vch; q++) {
                    double val;
                    if (v->vch[q].prop != pi || v->vch[q].form != sel) continue;
                    if (v2_vch_value(p, &v->vch[q], &val) < 0) return -1;
                    if (v2_op(ops, &c, cap, YTL_OP_SET_VALUE, v->vch[q].ch, (float)val, 0) < 0) { pl_fail(p, "the op table is full"); return -1; }
                }
                for (j = 0; j < v->props[pi].n_forms; j++)
                    if (v2_op(ops, &c, cap, YTL_OP_SET_VALUE, v->props[pi].forms[j].ch_sel, j == sel ? 1.0f : 0.0f, 0) < 0) { pl_fail(p, "the op table is full"); return -1; }
            }
        }
        return c;
    }
    default:
        return -2;
    }
}

/* Recenter steps that are not at the head of the first state: marks. */
static int v2_recenter_scan(pl_player* p, pl_v2* v) {
    int r, k, n = 0;
    for (r = 0; r < v->n_rs; r++) {
        const v2_rs* x = &v->rs[r];
        if (x->wait) continue;
        for (k = 0; k < x->n_steps; k++)
            if (x->steps[k].op == YEXP_RECENTER) n++;
    }
    if (!n) return 0;
    v->recenter = (const yexp_step**)pl_alloc(p, sizeof(yexp_step*) * (size_t)n);
    if (!v->recenter) return -1;
    /* every recenter, the first state's head ones too: they run again on
     * a later visit (a state's steps run on every visit); the trial's
     * first visit skips them (pl_v2_step_ops) */
    for (r = 0; r < v->n_rs; r++) {
        const v2_rs* x = &v->rs[r];
        if (x->wait) continue;
        for (k = 0; k < x->n_steps; k++)
            if (x->steps[k].op == YEXP_RECENTER) v->recenter[v->n_recenter++] = &x->steps[k];
    }
    return 0;
}

/* --- the trial's end (3.11, D36) and its row (3.15) ----------------------- */

static bool v2_text_in(pl_player* p, const yexp_value* arr, const char* text) {
    uint32_t k;
    if (!arr || arr->kind != YEXP_V_ARRAY) return false;
    for (k = 0; k < arr->n; k++) {
        char buf[256];
        if (pl_eval_text(p, &p->env, &arr->items[k], buf, sizeof buf) < 0) return false;
        if (strcmp(buf, text) == 0) return true;
    }
    return false;
}

/* D36: the latest outcome set in the trial wins, else version 1's from the
 * response, else "done". requeue_outcomes makes the trial invalid. */
int pl_v2_trial_end(pl_player* p, const yscr_frame* f, int choice, int* outcome_val, bool* requeue) {
    pl_v2* v = p->tmpl->v2;
    const yexp_template* d = v->def;
    (void)f;
    v2_srow_close(p, v, -1, v->end_onset);
    if (v->outcome_set) {
        if (v->outcome_text) snprintf(v->outcome_buf, sizeof v->outcome_buf, "%s", v->outcome_text);
        else if (pl_eval_text(p, &p->env, v->outcome, v->outcome_buf, sizeof v->outcome_buf) < 0) return -1;
        p->env.outcome = v->outcome_buf;
    } else if (!d->response) p->env.outcome = "done";
    *requeue = false;
    *outcome_val = YTR_INVALID;
    if (p->env.outcome && v2_text_in(p, d->requeue_outcomes, p->env.outcome)) {
        /* reading F23: `requeue` is the node's requeue (no_response) */
        if (p->leaf->node->requeue_no_response && !p->ti.practice && !p->ti.warmup && p->ti.track < 0) *requeue = true;
        return 0;
    }
    if (d->outcome && d->outcome->kind == YEXP_V_STRING) {
        /* "correct" as version 1; "" (an events template with no response,
         * 2.5's written default) gives no value, as version 1 */
        /* as version 1 (2.5): with no key_answer, the choice's index */
        if (strcmp(d->outcome->str, "correct") == 0 && p->env.responded)
            *outcome_val = p->env.has_correct ? (p->env.correct ? 1 : 0) : choice >= 0 ? choice : 0;
    } else if (d->outcome) {
        int64_t x;
        if (pl_eval_int(p, &p->env, d->outcome, &x) < 0) return -1;
        *outcome_val = x < 0 ? YTR_INVALID : (int)x;
    } else *outcome_val = 1;
    return 0;
}

/* A column's value form (3.15), read at the trial's end frame after its
 * ysig_frame(). A `time` column waits for the row's flush (*deferred): a
 * stimulus's time is its flip record's onset. */
static int v2_ref_by_name(pl_player* p, pl_v2* v, const char* name, yexp_ref* r) {
    const yexp_control* c;
    int i;
    memset(r, 0, sizeof *r);
    r->name = name;
    r->lane = -1;
    if (strcmp(name, "response") == 0) { r->kind = YEXP_REF_RESPONSE; return 0; }
    if (strcmp(name, "end") == 0) { r->kind = YEXP_REF_END; return 0; }
    if ((c = yexp_control_get(p->def, v->def, name)) != NULL) { r->kind = YEXP_REF_CONTROL; r->control = c; return 0; }
    for (i = 0; i < v->n_states; i++) if (strcmp(v->def->states[i].name, name) == 0) { r->kind = YEXP_REF_STATE; r->index = i; return 0; }
    for (i = 0; i < p->def->n_stimuli; i++) if (strcmp(p->def->stimuli[i].name, name) == 0) { r->kind = YEXP_REF_STIMULUS; r->index = i; return 0; }
    pl_fail(p, "template '%s': '%s' has no time", v->def->name, name);
    return -1;
}

static int v2_value_form(pl_player* p, pl_v2* v, const yexp_value* f, pl_val* out) {
    memset(out, 0, sizeof *out);
    if (f->kind == YEXP_V_SIGNAL && !f->by) {
        int lanes = 1, n = v2_name_node(p, v, f->str, &lanes);
        const ysig_value* x;
        if (n < 0) return -1;
        x = v->has_graph ? ysig_value_of(&v->g, n) : NULL;
        if (!x) { pl_fail(p, "template '%s': '%s' has no value in the graph", v->def->name, f->str); return -1; }
        out->kind = YEXP_V_NUMBER;
        out->num = x->v[f->i >= 0 ? f->i : 0];
        return 0;
    }
    if (f->kind == YEXP_V_WHEN) {
        bool test = false;
        const yexp_value* t = f->by;
        if (t->kind == YEXP_V_TEST) {
            const yexp_control* c = yexp_control_get(p->def, v->def, t->str);
            int ci = c ? v2_ctl_index_of(v, c) : -1;
            const ysig_value* x;
            if (ci < 0 || v->ctl[ci].test < 0 || !(x = ysig_value_of(&v->g, v->ctl[ci].test))) { pl_fail(p, "a when's test '%s' has no node", t->str); return -1; }
            test = x->v[0] != 0;
        } else {
            /* above: the signal's lane at or above the level */
            int lanes = 1, n = v2_name_node(p, v, t->str, &lanes);
            double lv;
            const ysig_value* x;
            if (n < 0 || pl_eval_num(p, &p->env, t->by, &lv) < 0) return -1;
            if (!(x = ysig_value_of(&v->g, n))) { pl_fail(p, "template '%s': '%s' has no value in the graph", v->def->name, t->str); return -1; }
            test = x->v[t->i >= 0 ? t->i : 0] >= lv;
        }
        f = &f->items[test ? 0 : 1];
        if (v2_is_form(f)) return v2_value_form(p, v, f, out);
        return pl_eval(p, &p->env, f, out);
    }
    if (f->kind == YEXP_V_HIT) {
        const yexp_control* c = yexp_control_get(p->def, v->def, f->str);
        int ci = c ? v2_ctl_index_of(v, c) : -1;
        ysig_value x;
        if (ci < 0) { pl_fail(p, "a hit column names '%s', no control here", f->str); return -1; }
        if (!v->ctl[ci].has_hit) return 0;   /* empty with no hit */
        if (ysig_value_at(&v->g, v->ctl[ci].of, v->ctl[ci].hit_t, &x) < 0) {
            const ysig_value* y = ysig_value_of(&v->g, v->ctl[ci].of);
            if (!y) { pl_fail(p, "a hit column's '%s' has no value in the graph", f->str); return -1; }
            x = *y;
        }
        out->kind = YEXP_V_NUMBER;
        out->num = x.v[f->i >= 0 ? f->i : 0];
        return 0;
    }
    return pl_eval(p, &p->env, f, out);
}

/* A frame time's flip record onset: into *t when its record is in, and -1
 * back; else the frame whose record the row still waits for (-1: no frame
 * time, a hit's or the response's). */
static int64_t v2_rec_time(const pl_v2* v, const yexp_ref* r, int64_t* t) {
    int64_t fr = -1, rf = -1, ro = 0;
    switch (r->kind) {
    case YEXP_REF_STIMULUS: fr = v->stim_frame[r->index]; rf = v->stim_recf[r->index]; ro = v->stim_rec[r->index]; break;
    case YEXP_REF_STATE: fr = v->state_frame[r->index]; rf = v->state_recf[r->index]; ro = v->state_rec[r->index]; break;
    case YEXP_REF_END: fr = v->end_frame; rf = v->end_recf; ro = v->end_rec; break;
    default: return -1;
    }
    if (fr < 0) return -1;
    if (rf == fr) { *t = ro; return -1; }
    return fr;
}

/* A flip record: the running trial's frame times, or a row's cells that
 * still wait for it (pl_on_record(), before the row's flush). */
void pl_v2_record_times(pl_player* p, pl_row* r, const yscr_record* rec) {
    int64_t on = rec->onset ? rec->onset : rec->planned;
    int i;
    if (r) {
        if (!r->v2) return;
        for (i = 0; i < r->v2->n; i++) {
            if (r->v2->t[i].fa == rec->index) { r->v2->t[i].a = on; r->v2->t[i].fa = -1; }
            if (r->v2->t[i].fb == rec->index) { r->v2->t[i].b = on; r->v2->t[i].fb = -1; }
        }
        return;
    }
    {
        pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
        if (!v) return;
        for (i = 0; i < p->n_stims; i++) if (v->stim_frame[i] == rec->index) { v->stim_recf[i] = rec->index; v->stim_rec[i] = on; }
        for (i = 0; i < v->n_states; i++) if (v->state_frame[i] == rec->index) { v->state_recf[i] = rec->index; v->state_rec[i] = on; }
        if (v->end_frame == rec->index) { v->end_recf = rec->index; v->end_rec = on; }
    }
}

int pl_v2_column(pl_player* p, pl_row* r, int col, const yexp_value* f, pl_val* out, bool* deferred);
int pl_v2_column(pl_player* p, pl_row* r, int col, const yexp_value* f, pl_val* out, bool* deferred) {
    pl_v2* v = p->tmpl->v2;
    *deferred = false;
    if (!v) return pl_eval(p, &p->env, f, out);
    if (f->kind == YEXP_V_TIME) {
        yexp_ref a, b;
        bool ka, kb;
        int64_t ta, tb;
        struct pl_v2_row* x = r->v2;
        if (v2_ref_by_name(p, v, f->str, &a) < 0 || v2_ref_by_name(p, v, f->by->str, &b) < 0) return -1;
        ta = v2_name_time(p, v, &a, &ka);
        tb = v2_name_time(p, v, &b, &kb);
        *deferred = true;
        if (!x || x->n >= x->cap) { pl_fail(p, "template '%s': internal: more time columns than the row holds", v->def->name); return -1; }
        x->t[x->n].col = col;
        x->t[x->n].a = ta;
        x->t[x->n].b = tb;
        x->t[x->n].has_a = ka;
        x->t[x->n].has_b = kb;
        x->t[x->n].sa = a.kind == YEXP_REF_STIMULUS ? a.index : -1;
        x->t[x->n].sb = b.kind == YEXP_REF_STIMULUS ? b.index : -1;
        x->t[x->n].fa = v2_rec_time(v, &a, &x->t[x->n].a);
        x->t[x->n].fb = v2_rec_time(v, &b, &x->t[x->n].b);
        x->n++;
        return 0;
    }
    if (v2_is_form(f) || f->kind == YEXP_V_HIT) return v2_value_form(p, v, f, out);
    return pl_eval(p, &p->env, f, out);
}

static struct pl_v2_row* v2_row_new(pl_player* p);

int pl_v2_row_fill(pl_player* p, pl_row* r) {
    pl_v2_session* S = p->v2s;
    if (!S) return 0;
    if (!r->v2) {
        /* pl_v2_rows() gave every pending row its cells at the run's start;
         * a row made elsewhere (--test-draws) gets them here */
        r->v2 = v2_row_new(p);
        if (!r->v2) return -1;
    }
    r->v2->n = 0;
    return 0;
}

/* A row's cells for the most `time` columns any template has (the
 * loader bounds the columns), sized at load. */
static struct pl_v2_row* v2_row_new(pl_player* p) {
    int ti, k, most = 1;
    struct pl_v2_row* x;
    for (ti = 0; ti < p->def->n_templates; ti++) {
        const yexp_template* t = &p->def->templates[ti];
        int n = 0;
        for (k = 0; k < t->n_columns; k++) if (t->columns[k].value && t->columns[k].value->kind == YEXP_V_TIME) n++;
        if (n > most) most = n;
    }
    x = (struct pl_v2_row*)pl_alloc(p, sizeof(struct pl_v2_row));
    if (!x) return NULL;
    x->t = (v2_tcell*)pl_alloc(p, sizeof(v2_tcell) * (size_t)most);
    if (!x->t) return NULL;
    x->cap = most;
    return x;
}

int pl_v2_rows(pl_player* p);
int pl_v2_rows(pl_player* p) {
    int i;
    if (!p->v2s) return 0;
    for (i = 0; i < PL_PENDING_ROWS; i++) {
        p->rows[i].v2 = v2_row_new(p);
        if (!p->rows[i].v2) return -1;
    }
    return 0;
}

/* The deferred time columns: a stimulus's time is its record's onset. */
int pl_v2_row_flush(pl_player* p, pl_row* r, ytr_data* w) {
    int i;
    if (!r->v2) return 0;
    for (i = 0; i < r->v2->n; i++) {
        int64_t a = r->v2->t[i].a, b = r->v2->t[i].b;
        int k;
        if (!r->v2->t[i].has_a || !r->v2->t[i].has_b) continue;
        for (k = 0; k < r->n_stats; k++) {
            if (r->stats[k].stim == r->v2->t[i].sa && r->stats[k].onset_ns != YTR_MISSING_NS) a = r->stats[k].onset_ns;
            if (r->stats[k].stim == r->v2->t[i].sb && r->stats[k].onset_ns != YTR_MISSING_NS) b = r->stats[k].onset_ns;
        }
        if (ytr_data_set_time_ns(w, r->v2->t[i].col, a - b) < 0) { snprintf(p->werror, sizeof p->werror, "a time column: %s", ytr_data_error(w)); return -1; }
    }
    (void)p;
    return 0;
}

/* rsp_role: the signal role whose sample crossed (3.15): the derived
 * event's code, low 16 bits, is a node of that role's adapter. */
const char* pl_v2_rsp_role(pl_player* p, const yrsp_result* res);
const char* pl_v2_rsp_role(pl_player* p, const yrsp_result* res) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    int node, a, i;
    if (!v || !v->has_graph || !(res->flags & YRSP_R_RESPONDED)) return NULL;
    node = (int)(res->code & 0xffff);
    for (a = 0; a < p->def->n_sigroles; a++)
        for (i = 0; i < 2; i++)
            if (ysig_adapter_node(&v->g, a, i) == node) return p->def->sigroles[a].name;
    return NULL;
}

/* A sample trace's rows of the trial, cut to its window, to its stream. */
static int v2_trace_stream(pl_player* p, pl_v2* v, v2_trace* tr) {
    int k, l, c = 0;
    static const char lane[4] = { 'x', 'y', 'z', 'w' };
    if (tr->stream >= 0) return 0;
    /* two templates' traces of one name share a stream (3.15) */
    {
        pl_v2_session* S = p->v2s;
        int ti, i;
        for (ti = 0; ti < S->n_t; ti++) {
            pl_v2* w = S->t[ti];
            if (!w) continue;
            for (i = 0; i < w->n_traces; i++)
                if (&w->traces[i] != tr && w->traces[i].stream >= 0 && strcmp(w->traces[i].def->name, tr->def->name) == 0) {
                    tr->stream = w->traces[i].stream;
                    return 0;
                }
        }
    }
    {
#define COL(nm, ty, off) do { snprintf(tr->names[tr->n_cols], sizeof tr->names[0], "%s", nm); \
        memset(&tr->cols[tr->n_cols], 0, sizeof tr->cols[0]); tr->cols[tr->n_cols].name = tr->names[tr->n_cols]; \
        tr->cols[tr->n_cols].type = ty; tr->cols[tr->n_cols].offset = off; tr->n_cols++; } while (0)
        COL("trial_index", YTR_COL_INT, offsetof(v2_srow_data, trial));
        if (tr->frame) {
            COL("frame", YTR_COL_INT64, offsetof(v2_srow_data, frame));
            COL("planned_s", YTR_COL_TIME_NS, offsetof(v2_srow_data, planned));
        }
        COL("t_s", YTR_COL_TIME_NS, offsetof(v2_srow_data, t));
        for (k = 0; k < tr->n_items; k++) {
            char nm[YTB_MAX_NAME + 1];
            for (l = 0; l < tr->item_lanes[k]; l++) {
                const yexp_ref* r = &tr->def->items[k];
                if (tr->item_lanes[k] == 1) snprintf(nm, sizeof nm, "%s", r->name);
                else snprintf(nm, sizeof nm, "%s_%c", r->name, lane[l]);
                if (c >= 32 || tr->n_cols >= 38) { pl_fail(p, "trace '%s': too many lanes", tr->def->name); return -1; }
                COL(nm, YTR_COL_DOUBLE, offsetof(v2_srow_data, v) + sizeof(double) * (size_t)c);
                c++;
            }
        }
        if (!tr->frame) {
            COL("tier", YTR_COL_INT, offsetof(v2_srow_data, tier));
            COL("device", YTR_COL_INT, offsetof(v2_srow_data, device));
        } else COL("flags", YTR_COL_INT, offsetof(v2_srow_data, flags));
#undef COL
    }
    tr->stream = yds_stream(&p->ds, tr->def->name, tr->cols, tr->n_cols);
    if (tr->stream < 0) { pl_fail(p, "the stream '%s': %s", tr->def->name, yds_error(&p->ds)); return -1; }
    (void)v;
    return 0;
}

static int v2_traces_end(pl_player* p, pl_v2* v) {
    int i, k;
    for (i = 0; i < v->n_traces; i++) {
        v2_trace* tr = &v->traces[i];
        int64_t from = p->trial_start_onset, to = INT64_MAX;
        bool kf = true, kt = true, to_in = true;
        if (tr->frame) continue;
        if (tr->def->from.kind != YEXP_REF_NONE) from = v2_name_time(p, v, &tr->def->from, &kf);
        if (tr->def->to.kind != YEXP_REF_NONE) {
            to = v2_name_time(p, v, &tr->def->to, &kt);
            to_in = tr->def->to.kind == YEXP_REF_CONTROL || tr->def->to.kind == YEXP_REF_STIMULUS || tr->def->to.kind == YEXP_REF_RESPONSE;
        }
        if (!kf) { tr->nrows = 0; continue; }
        if (!kt) to = INT64_MAX;
        if (v2_trace_stream(p, v, tr) < 0) return -1;
        for (k = 0; k < tr->nrows; k++) {
            v2_srow_data row;
            int l, c = 0, m;
            if (tr->rt[k] < from || tr->rt[k] > to || (!to_in && tr->rt[k] == to)) continue;
            memset(&row, 0, sizeof row);
            row.trial = p->global_trial - 1;
            row.t = tr->rt[k];
            for (m = 0; m < tr->n_items; m++)
                for (l = 0; l < tr->item_lanes[m]; l++, c++) row.v[c] = tr->rv[k * 4 + c];
            row.tier = tr->rtier[k];
            row.device = (int32_t)tr->rdev[k];
            if (pl_wq_push_row(p, tr->stream, &row, sizeof row) < 0) return -1;
        }
        tr->nrows = 0;
    }
    return 0;
}

static int v2_trial_rows_now(pl_player* p, pl_v2* v) {
    /* the trial's held rows come now: a flush is a barrier, so every row
     * so far is final; then the traces */
    if (v->has_graph && v->rows_open && !ysig_flush(&v->g)) {
        pl_fail(p, "template '%s': the graph: %s", v->def->name, ysig_error(&v->g));
        return -1;
    }
    v->rows_open = false;
    /* the inputs since the trial's last frame, before the flush's barrier */
    if (v2_guard(p, v, "the trial's end", NULL, 0) < 0) return -1;
    if (v2_traces_end(p, v) < 0) return -1;
    return v2_srow_flush(p, false);
}

/* The flush's barrier is the graph's newest input or frame (t_cover). A
 * report of any role stamped before it that arrives after it is dropped
 * (late_barrier), and the next trial reads the held value before it (the
 * model's harness, 60 Hz, 10 ms late, seed 102928). So the flush and the
 * trial's rows wait until the input is complete to the barrier
 * (pl_v2_barrier_wait()); the next trial starts once they are done. Every
 * bound 0: G is the poll's time, never before the barrier. */
int pl_v2_trial_rows(pl_player* p);
int pl_v2_trial_rows(pl_player* p) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    if (!v) return 0;
    if (p->v2s->bnd_max > 0 && v->has_graph && v->rows_open && v2_horizon_all(p) < v->g.t_cover - 1) {
        p->v2s->flush_pend = v;
        return 0;
    }
    return v2_trial_rows_now(p, v);
}

/* Between trials, after the poll is fed: 1 while a trial's end waits for
 * the input to its barrier (no trial starts on this frame), 0 when none
 * waits or it is done now, -1 on an error. */
int pl_v2_barrier_wait(pl_player* p) {
    pl_v2_session* S = p->v2s;
    pl_v2* v = S ? S->flush_pend : NULL;
    if (!v) return 0;
    if (v2_horizon_all(p) < v->g.t_cover - 1) return 1;
    S->flush_pend = NULL;
    return v2_trial_rows_now(p, v) < 0 ? -1 : 0;
}

/* --- what a template reads (3.14: a leaf draws only what it reads) -------- */

static bool v2_name_match(const char* var, const char* name) {
    size_t n = strlen(name);
    if (strcmp(var, name) == 0) return true;
    /* a walk's five variables */
    return strncmp(var, name, n) == 0 && (strcmp(var + n, "_x") == 0 || strcmp(var + n, "_y") == 0 ||
                                         strcmp(var + n, "_from_x") == 0 || strcmp(var + n, "_from_y") == 0 ||
                                         strcmp(var + n, "_deg") == 0);
}

static bool v2_val_reads(const yexp_value* v, const char* name, int depth) {
    uint32_t k;
    if (!v || depth > 64) return false;
    if (v->kind == YEXP_V_VAR && v->str && v2_name_match(v->str, name)) return true;
    if (v->by && v2_val_reads(v->by, name, depth + 1)) return true;
    if ((v->kind == YEXP_V_ARRAY || v->kind == YEXP_V_WHEN || v->kind == YEXP_V_LINSPACE || v->kind == YEXP_V_CHOICE ||
         v->kind == YEXP_V_UNIFORM || v->kind == YEXP_V_WALK2D) && v->items)
        for (k = 0; k < v->n; k++) if (v2_val_reads(&v->items[k], name, depth + 1)) return true;
    if (v->kind == YEXP_V_LOOKUP)
        for (k = 0; k < v->n; k++) if (v2_val_reads(v->map[k].value, name, depth + 1)) return true;
    return false;
}

static bool v2_steps_read(pl_player* p, const yexp_step* st, int n, const char* name, bool* stims) {
    int i, k;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        if (v2_val_reads(s->t, name, 0) || v2_val_reads(s->stagger, name, 0) || v2_val_reads(s->duration, name, 0) ||
            v2_val_reads(s->delay, name, 0) || v2_val_reads(s->code, name, 0) || v2_val_reads(s->timeout, name, 0) ||
            v2_val_reads(s->point, name, 0))
            return true;
        if (s->cond && v2_name_match(s->cond, name)) return true;
        for (k = 0; k < s->n_props; k++) if (v2_val_reads(s->props[k].value, name, 0)) return true;
        for (k = 0; k < s->n_stims; k++) {
            int m;
            for (m = 0; m < p->def->n_stimuli; m++) if (strcmp(p->def->stimuli[m].name, s->stims[k]) == 0) stims[m] = true;
        }
        if (v2_steps_read(p, s->steps, s->n_steps, name, stims)) return true;
    }
    return false;
}

static bool v2_ctl_reads(const yexp_control* c, const char* name) {
    int k;
    for (k = 0; k < c->n_props; k++) if (v2_val_reads(c->props[k].value, name, 0)) return true;
    return v2_val_reads(c->sx.amplitudes, name, 0) || v2_val_reads(c->sx.phases, name, 0) || v2_val_reads(c->sx.freqs_hz, name, 0) ||
           v2_val_reads(c->sy.amplitudes, name, 0) || v2_val_reads(c->sy.phases, name, 0) || v2_val_reads(c->sy.freqs_hz, name, 0);
}

bool pl_v2_reads(pl_player* p, const yexp_template* t, const char* name) {
    /* the scratch is the session's, sized at load: a leaf opens on the
     * frame path, which allocates nothing */
    bool* stims = p->v2s && p->v2s->reads ? p->v2s->reads : NULL;
    bool r = false;
    int i, k;
    if (!stims) return true;
    memset(stims, 0, (size_t)p->def->n_stimuli + 1);
    if (v2_steps_read(p, t->events, t->n_events, name, stims) || v2_steps_read(p, t->after, t->n_after, name, stims)) r = true;
    for (i = 0; !r && t->states && i < t->n_states; i++) {
        const yexp_state* st = &t->states[i];
        if (v2_steps_read(p, st->steps, st->n_steps, name, stims)) r = true;
        for (k = 0; !r && k < st->n_exits; k++)
            if (v2_val_reads(st->exits[k].after, name, 0) || v2_val_reads(st->exits[k].outcome, name, 0) ||
                (st->exits[k].if_var && v2_name_match(st->exits[k].if_var, name)))
                r = true;
        if (v2_val_reads(st->over_outcome, name, 0)) r = true;
    }
    for (k = 0; !r && k < t->n_exits; k++)
        if (v2_val_reads(t->exits[k].after, name, 0) || v2_val_reads(t->exits[k].outcome, name, 0)) r = true;
    for (k = 0; !r && k < t->n_controls; k++) if (v2_ctl_reads(&t->controls[k], name)) r = true;
    for (k = 0; !r && k < p->def->n_controls; k++) if (t->uses_top && t->uses_top[k] && v2_ctl_reads(&p->def->controls[k], name)) r = true;
    for (k = 0; !r && k < t->n_columns; k++) if (v2_val_reads(t->columns[k].value, name, 0)) r = true;
    if (!r && (v2_val_reads(t->outcome, name, 0) || v2_val_reads(t->method_value, name, 0))) r = true;
    if (!r && t->response) {
        const yexp_response* rs = t->response;
        int m;
        if (v2_val_reads(rs->duration, name, 0) || v2_val_reads(rs->min_rt, name, 0) || v2_val_reads(rs->key_answer, name, 0) ||
            v2_val_reads(rs->trigger_code, name, 0))
            r = true;
        for (m = 0; m < p->def->n_stimuli; m++) if (strcmp(p->def->stimuli[m].name, rs->from) == 0) stims[m] = true;
    }
    /* the stimuli the template shows: their keys */
    for (i = 0; !r && i < p->def->n_stimuli; i++) {
        if (!stims[i]) continue;
        for (k = 0; k < p->def->stimuli[i].n_props; k++) if (v2_val_reads(p->def->stimuli[i].props[k].value, name, 0)) { r = true; break; }
    }
    return r;
}

/* --- the data set: streams and session.json (3.15, D40) ------------------ */

int pl_v2_streams(pl_player* p) {
    pl_v2_session* S = p->v2s;
    int ti, i, n = 0;
    if (!S) return 0;
    for (ti = 0; ti < S->n_t; ti++) {
        pl_v2* v = S->t[ti];
        if (!v) continue;
        for (i = 0; i < v->n_traces; i++, n++) if (v2_trace_stream(p, v, &v->traces[i]) < 0) return -1;
    }
    /* a trace's rows are wider than the writer's job (pl_wq_push_row) */
    if (n > 0 && p->wq && !p->wq_rows) {
        p->wq_rows = (unsigned char*)pl_alloc(p, (size_t)PL_WRITE_QUEUE * PL_WQ_ROW_BYTES);
        if (!p->wq_rows) return -1;
    }
    return 0;
}

/* Per signal role: the adapter's name, the device, the type, the units,
 * the scale, and the recenter steps skipped (3.15, D40); the samples. */
int pl_v2_session_json(pl_player* p, yjs_arena* a, yjs_value* s) {
    pl_v2_session* S = p->v2s;
    yjs_value* arr;
    int i;
    static const char* const tn[] = { "position2d", "position2d_relative", "position3d", "scalar", "gaze" };
    static const char* const un[] = { "display_px", "display_cm", "display_deg", "workspace_cm", "device" };
    if (!S || p->def->n_sigroles == 0) return 0;
    arr = yjs_new(a, YJS_ARRAY);
    for (i = 0; i < p->def->n_sigroles; i++) {
        const yexp_sigrole* r = &p->def->sigroles[i];
        yjs_value* o = yjs_new(a, YJS_OBJECT);
        yjs_set(a, o, "name", yjs_new_stringz(a, r->name));
        yjs_set(a, o, "adapter", yjs_new_stringz(a, r->type == YEXP_SIG_POSITION2D_RELATIVE || (r->type == YEXP_SIG_POSITION2D && r->allow_relative) ? "sim raw mouse" : "sim positions"));
        yjs_set(a, o, "device", yjs_new_stringz(a, p->args.samples_path ? "--samples" : "none"));
        yjs_set(a, o, "type", yjs_new_stringz(a, tn[r->type]));
        yjs_set(a, o, "units", yjs_new_stringz(a, un[r->units]));
        yjs_set(a, o, "per_count", yjs_new_double(a, S->gain ? S->gain[i] : 0));
        yjs_set(a, o, "calibration_sha256", yjs_new_stringz(a, ""));
        yjs_push(a, arr, o);
    }
    yjs_set(a, s, "signal_roles", arr);
    yjs_set(a, s, "recenter_skipped", yjs_new_int(a, S->recenter_skipped));
    return 0;
}

/* sig.h 0.6.0's window, summed over the templates' graphs (stats outlive
 * a reset): the arms run again, the arms that fell back to the newest
 * value (inexact), and what a run again changed after the player had it;
 * 0.7.0's late calls (LATE INPUT). Version 2 only, so version 1's
 * session.json keeps its bytes. */
int pl_v2_arm_counts(pl_player* p, int64_t c[PL_ARM_COUNTS]) {
    pl_v2_session* S = p->v2s;
    ysig_stats st;
    int i, n = 0;
    memset(c, 0, sizeof(int64_t) * PL_ARM_COUNTS);
    if (!S) return 0;
    for (i = 0; i < S->n_t; i++) {
        if (!S->t[i] || !S->t[i]->has_graph) continue;
        ysig_get_stats(&S->t[i]->g, &st);
        c[0] += st.arm_caught_up;
        c[1] += st.arm_past_off;
        c[2] += st.arm_past_far;
        c[3] += st.arm_past_full;
        c[4] += st.arm_hits_superseded;
        c[5] += st.arm_hits_retracted;
        c[6] += st.arm_rows_stale;
        c[7] += st.arm_derived_stale;
        c[8] += st.arm_past_pinned;
        c[9] += st.arm_ahead_cancelled;
        c[10] += S->t[i]->n_late;
        c[11] += st.arm_unverified;
        c[12] += st.late_far;
        c[13] += st.late_full;
        c[14] += st.late_barrier;
        c[15] += st.late_off;
        c[16] += st.late_frames;
        c[17] += st.late_unverified;
        c[18] += st.late_hits_voided;
        n++;
    }
    return n;
}

/* The trial's late_frames column: sig.h's frames of this trial already
 * drawn when a late report changed what they show; empty with no graph. */
int pl_v2_late_frames(pl_player* p, int64_t* n) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    ysig_stats st;
    if (!v || !v->has_graph) return 0;
    ysig_get_stats(&v->g, &st);
    *n = st.late_frames - v->late_frames0;
    return 1;
}

/* The trial's exit_lag_max_s column: its largest exit lag; empty with no
 * graph, or no exit */
int pl_v2_exit_lag_max(pl_player* p, int64_t* ns) {
    pl_v2* v = p->tmpl ? p->tmpl->v2 : NULL;
    if (!v || !v->has_graph || v->lag_max == YTR_MISSING_NS) return 0;
    *ns = v->lag_max;
    return 1;
}

/* A signal choice collector's time: the horizon when the template waits,
 * so a deadline is not judged on input that is not in */
int64_t pl_v2_collector_now(pl_player* p, int64_t onset) {
    pl_v2* v = p->in_trial && p->tmpl ? p->tmpl->v2 : NULL;
    int64_t g;
    if (!v || !v->has_graph || !v->late || !v->def->response || !v->def->response->signal_choices) return onset;
    g = v2_horizon(p, v);
    return g < onset ? g : onset;
}

/* Whether any template has a graph: the late_frames column. */
bool pl_v2_any_graph(pl_player* p) {
    int i;
    for (i = 0; p->v2s && i < p->v2s->n_t; i++)
        if (p->v2s->t[i] && p->v2s->t[i]->has_graph) return true;
    return false;
}

int pl_v2_session_end(pl_player* p, yjs_arena* a) {
    static const char* const name[PL_ARM_COUNTS] = { "caught_up", "past_off", "past_far", "past_full", "hits_superseded", "hits_retracted",
                                                     "rows_stale", "derived_stale", "past_pinned", "ahead_cancelled", "late_input",
                                                     "unverified", "late_far", "late_full", "late_barrier", "late_off", "late_frames",
                                                     "late_unverified", "late_hits_voided" };
    int64_t c[PL_ARM_COUNTS];
    yjs_value* o;
    int i;
    if (!pl_v2_arm_counts(p, c)) return 0;
    o = yjs_new(a, YJS_OBJECT);
    for (i = 0; i < PL_ARM_COUNTS; i++) yjs_set(a, o, name[i], yjs_new_int(a, c[i]));
    if (c[1] + c[2] + c[3] + c[8] > 0)
        fprintf(stderr, "ysp_player: %lld arms in the past fell back (inexact; session.json sig_arms)\n", (long long)(c[1] + c[2] + c[3] + c[8]));
    if (c[12] + c[13] + c[15] > 0)
        fprintf(stderr, "ysp_player: %lld late reports fell back (inexact; session.json sig_arms)\n", (long long)(c[12] + c[13] + c[15]));
    if (yds_session_set(&p->ds, "sig_arms", o) < 0) return -1;
    {
        /* 0.12.0: the exits' lag (docs/player.md, "What the data shows"),
         * and the reports later than their role's bound */
        pl_v2_session* S = p->v2s;
        yjs_value* l = yjs_new(a, YJS_OBJECT);
        yjs_value* b = yjs_new(a, YJS_OBJECT);
        yjs_set(a, l, "n", yjs_new_int(a, S->lag_n));
        yjs_set(a, l, "mean_s", S->lag_n ? yjs_new_double(a, S->lag_sum / (double)S->lag_n / 1e9) : yjs_new(a, YJS_NULL));
        yjs_set(a, l, "max_s", S->lag_n ? yjs_new_double(a, (double)S->lag_max / 1e9) : yjs_new(a, YJS_NULL));
        if (yds_session_set(&p->ds, "exit_lag", l) < 0) return -1;
        for (i = 0; i < p->def->n_sigroles; i++) yjs_set(a, b, p->def->sigroles[i].name, yjs_new_int(a, S->beyond[i]));
        yjs_set(a, b, "after_take", yjs_new_int(a, S->beyond_take));
        return yds_session_set(&p->ds, "late_beyond_bound", b);
    }
}

/* The host's op check (host.c): template ti's runtime states' steps, each
 * a table of its own. The number of runtime states, or 0 for none; the
 * k-th's steps, NULL for an until's wait. */
int pl_v2_rs_steps(pl_player* p, int ti, int k, const yexp_step** st, int* n);
int pl_v2_rs_steps(pl_player* p, int ti, int k, const yexp_step** st, int* n) {
    pl_v2* v;
    if (!p->v2s || ti < 0 || ti >= p->v2s->n_t || !(v = p->v2s->t[ti])) return 0;
    p->v2s->cur_t = ti;
    if (k >= 0 && k < v->n_rs) {
        *st = v->rs[k].wait ? NULL : v->rs[k].steps;
        *n = v->rs[k].wait ? 0 : v->rs[k].n_steps;
    }
    return v->n_rs;
}

void pl_v2_select(pl_player* p) {
    if (!p->v2s) return;
    p->v2s->cur_t = p->tmpl && p->tmpl->v2 ? p->tmpl->v2->index : -1;
}
