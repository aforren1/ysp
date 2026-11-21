/* preview.c - the preview (preview.h; docs/designer.md, "Plan of step 5").
 * OWNED BY PART A.
 *
 * Two halves share one private struct, pv_sess (the dsg_session's impl):
 * - The session ops run on the check thread: the load (pl_sim_load() and the
 *   fast forward to the pin) and the close (pl_close() and the removal of
 *   old data sets). bimanual's load alone takes up to 12.9 ms, so neither
 *   fits in a UI frame.
 * - Everything else runs on the UI thread: the settle, the adoption, the
 *   attach in budgeted calls, the steps paced by real time, input, state.
 * The two never touch one pv_sess at once: the worker's lock orders the
 * hand-overs (dsg_worker_take_session(), dsg_worker_close()).
 *
 * Nothing here allocates in a running frame: the input queue, the outputs
 * ring and the strings are fixed arrays, and pl_sim_frame() allocates
 * nothing (test_designer_preview checks it with --wrap). */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "preview.h"
#include "doc.h"
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The plan's three knobs ("Time", "Edits while the preview runs"). */
enum {
    PV_SETTLE_MS        = 300,   /* no change for this long, then a session job (decision 4) */
    PV_ATTACH_BUDGET_MS = 4,     /* the attach's GL work a UI frame                          */
    PV_MAX_STEPS        = 16     /* session frames a UI frame; a 500 Hz display needs 8      */
};
#define PV_SETTLE_NS ((int64_t)PV_SETTLE_MS * 1000000)
#define PV_ATTACH_BUDGET_NS ((int64_t)PV_ATTACH_BUDGET_MS * 1000000)

/* The fast forward's limits (check thread). A trial that waits on a signal
 * role never ends under the script, so the stall is in session time; the
 * frame cap bounds a pin that the walk never reaches. */
#define PV_FF_STALL_NS ((int64_t)30 * 1000000000)
enum { PV_FF_MAX_FRAMES = 200000 };

enum { PV_IN_CAP = 64 };              /* keys and choices between two steps */
enum { PV_MSG = 1024, PV_NAME = 256, PV_NOTE = 256 };

/* What the fast forward stopped on. */
enum { PV_FF_NONE = 0, PV_FF_FOUND, PV_FF_LEFT, PV_FF_STALL, PV_FF_CAP };
/* A shown session's state; the pause is the preview's, not the session's. */
enum { PV_LIVE = 0, PV_ENDED, PV_FAILED };

typedef struct pv_out { const char* role; uint32_t code; int64_t t_ns; } pv_out;
typedef struct pv_drive { int role; int units; } pv_drive;
typedef struct pv_undriven { const char* name; const char* why; } pv_undriven;

/* The session's private part (dsg_session.impl). Made on the check thread
 * by the load; owned by the UI thread from the take; freed by the close on
 * the check thread. */
typedef struct pv_sess {
    pl_player*  p;                     /* 1.6 MB: the heap, not the stack       */
    bool        loaded;                /* pl_sim_load() ran: pl_close() is due  */
    const dsg_session_settings* set;   /* the dsg_session's                     */
    char*       text;                  /* the text it loaded: its places        */
    size_t      n;
    char        runs[DSG_PATH_MAX];    /* PROJECT/.designer/preview             */
    int         run_n;                 /* its data set is runs/run-N; 0: none   */
    /* the load's outcome */
    bool        refused;               /* the load or the fast forward refused  */
    char        msg[PV_MSG];
    int         obj;
    char        obj_name[PV_NAME];
    dsg_place   place;
    int         state;                 /* PV_LIVE, _ENDED, _FAILED              */
    char        note[PV_NOTE];
    /* the pin's fast forward, then the hooks on the UI thread */
    bool        ff;
    bool        ff_seen;
    int         ff_stop;
    /* the outputs ring (on_code), oldest at out_first */
    pv_out      outs[DSG_PV_OUTPUTS];
    int         out_first, n_outs;
    /* lists made at the load */
    dsg_preview_leaf* leaves;
    int         n_leaves;
    pv_drive*   drive;
    int         n_drive;
    pv_undriven* undriven;
    int         n_undriven;
    double      ppu_cm, ppu_deg;       /* participant px per cm and per deg     */
    /* set before the hand-back to the close */
    bool        attach_started;        /* GL objects may exist: detach first    */
    bool        discard;               /* the close removes its data set        */
    int         keep;                  /* the close removes every run below it  */
} pv_sess;

typedef struct pv_in { uint8_t choice; uint8_t down; int16_t code; } pv_in;

struct dsg_preview {
    const dsg_preview_gl* gl;
    dsg_worker*           worker;
    struct dsg_doc*       doc;
    dsg_preview_settings  settings;
    int64_t               now;
    bool                  pane;        /* the last tick's shown                 */
    /* the project */
    bool                  open;
    char                  runs[DSG_PATH_MAX];
    uint64_t              floor;
    /* the checks */
    bool                  have_check;
    uint64_t              check_rev;
    int                   check_status, check_errors;
    bool                  have_ok;
    uint64_t              ok_rev;
    dsg_pack*             ok_pack;     /* the preview's reference               */
    /* the settle and the posts */
    uint64_t              seen_rev;
    int64_t               changed_at;
    bool                  changed;     /* false: no change since the open       */
    bool                  have_posted;
    uint64_t              posted_rev, posted_doc_rev;
    int64_t               posted_unix;
    bool                  waiting;     /* the newest job's result not yet here  */
    bool                  restart_due;
    bool                  first_failed;
    /* the sessions */
    dsg_session*          attaching;
    int64_t               attaching_unix;
    dsg_session*          shown;
    int64_t               shown_unix;
    bool                  cleaned;     /* the first adoption's removal is sent  */
    /* time */
    bool                  paused, step_req, reanchor, slow;
    int64_t               sess_anchor, real_anchor;
    /* input */
    pv_in                 in[PV_IN_CAP];
    int                   n_in;
    uint8_t               keys_down[64];   /* scancodes the session took: 512 bits */
    uint64_t              choices_down;
    float                 ptr_x, ptr_y;
    bool                  ptr_inside, ptr_dirty;
    bool                  keyboard;
    /* the preview issue */
    bool                  have_issue;
    dsg_issue             issue;
    char                  issue_msg[PV_MSG];
    char                  issue_name[PV_NAME];
};

/* --- small helpers ------------------------------------------------------------ */

/* The settings that a load reads; out_dir is the load's. */
static bool same_settings(const dsg_session_settings* a, const dsg_session_settings* b) {
    int i;
    if (a->width != b->width || a->height != b->height || a->hz != b->hz || a->seed != b->seed || a->rt_s < b->rt_s ||
        a->rt_s > b->rt_s || a->pin_row != b->pin_row || a->n_fields != b->n_fields || strcmp(a->participant, b->participant) != 0 ||
        strcmp(a->pin_leaf, b->pin_leaf) != 0)
        return false;
    for (i = 0; i < a->n_fields && i < DSG_SESSION_FIELDS; i++)
        if (strcmp(a->fields[i].name, b->fields[i].name) != 0 || strcmp(a->fields[i].value, b->fields[i].value) != 0) return false;
    return true;
}

static pv_sess* impl_of(const dsg_session* s) { return s ? (pv_sess*)s->impl : NULL; }

static pl_player* shown_player(const dsg_preview* pv) {
    pv_sess* x = impl_of(pv->shown);
    return x ? x->p : NULL;
}

/* The runs' bookkeeping file, "LO HI": the lowest run folder that may be
 * left and the highest made. Only the session ops read and write it, on
 * the check thread, so the next process numbers its runs on and removes
 * what this one left without listing the folder: platform.c has no
 * listing, and dsg.h is frozen in step 5. */
static void runs_path(char* out, size_t cap, const char* runs) {
    if (dsg_join(out, cap, runs, "runs") != 0) out[0] = 0;
}

static void runs_read(const char* runs, int* lo, int* hi) {
    char path[DSG_PATH_MAX], err[256];
    char* d = NULL;
    size_t n = 0;
    int a = 1, b = 0;
    runs_path(path, sizeof path, runs);
    if (path[0] && dsg_is_file(path) && dsg_read_file(path, &d, &n, err, sizeof err) == 0) {
        if (sscanf(d, "%d %d", &a, &b) != 2 || a < 1 || b < 0 || b > 1000000000) {
            a = 1;
            b = 0;
        }
    }
    free(d);
    *lo = a;
    *hi = b;
}

static void runs_write(const char* runs, int lo, int hi) {
    char path[DSG_PATH_MAX], err[256], b[64];
    int k = snprintf(b, sizeof b, "%d %d\n", lo, hi);
    runs_path(path, sizeof path, runs);
    if (path[0] && k > 0) (void)dsg_write_file(path, b, (size_t)k, err, sizeof err);
}

static bool remove_run(const char* runs, int n) {
    char rel[32], err[256];
    snprintf(rel, sizeof rel, "run-%d", n);
    return dsg_remove_tree(runs, rel, err, sizeof err) == 0;
}

/* --- the session ops (check thread) ------------------------------------------------ */

/* live(): at each trial's start. During the fast forward, the trial that
 * meets the pin and every one after it are the person's; before it the
 * scripted participant answers. With no pin every trial is the person's. */
static bool pv_live(void* ctx, const pl_sim_where* w) {
    pv_sess* x = (pv_sess*)ctx;
    bool here;
    if (!x->ff) return true;
    here = w->leaf && strcmp(w->leaf, x->set->pin_leaf) == 0;
    if (here && (x->set->pin_row < 0 || w->condition == x->set->pin_row)) {
        x->ff_stop = PV_FF_FOUND;
        return true;
    }
    if (here) {
        x->ff_seen = true;
        return false;
    }
    if (x->ff_seen) {
        x->ff_stop = PV_FF_LEFT;
        return true;
    }
    return false;
}

/* on_code(): on whichever thread steps the session. The ring keeps the
 * last DSG_PV_OUTPUTS, times from the session's first frame. */
static void pv_on_code(void* ctx, const char* role, uint32_t code, int64_t t_ns) {
    pv_sess* x = (pv_sess*)ctx;
    pv_out* o;
    if (x->n_outs < DSG_PV_OUTPUTS) o = &x->outs[(x->out_first + x->n_outs++) % DSG_PV_OUTPUTS];
    else {
        o = &x->outs[x->out_first];
        x->out_first = (x->out_first + 1) % DSG_PV_OUTPUTS;
    }
    o->role = role;
    o->code = code;
    o->t_ns = x->p ? t_ns - x->p->sim.t0 : t_ns;
}

static void refuse(pv_sess* x, const char* msg, int obj, const char* name) {
    x->refused = true;
    snprintf(x->msg, sizeof x->msg, "%s", msg && msg[0] ? msg : "the session refused the definition");
    x->obj = obj;
    snprintf(x->obj_name, sizeof x->obj_name, "%s", name ? name : "");
    memset(&x->place, 0, sizeof x->place);
    (void)dsg_locate_object(x->text, x->n, obj, name, &x->place);
}

/* The lists that the UI reads: the pin's leaves, the roles the pointer
 * drives, the roles it does not (decision 3, "Input"). */
static int pv_lists(pv_sess* x) {
    pl_player* p = x->p;
    const yexp_def* def = p->def;
    int n = 0, i, k;
    while (p->def_leaves && p->def_leaves[n]) n++;
    x->leaves = (dsg_preview_leaf*)calloc((size_t)n + 1, sizeof *x->leaves);
    x->drive = (pv_drive*)calloc((size_t)def->n_sigroles + 1, sizeof *x->drive);
    x->undriven = (pv_undriven*)calloc((size_t)def->n_sigroles + (size_t)def->n_roles + 1, sizeof *x->undriven);
    if (!x->leaves || !x->drive || !x->undriven) return -1;
    for (i = 0; i < n; i++) {
        const yexp_node* nd = p->def_leaves[i];
        x->leaves[i].path = nd->path;
        x->leaves[i].tmpl = nd->trial ? nd->trial->name : "";
        for (k = 0; k < p->n_leaves; k++)
            if (p->leaves[k].node == nd) {
                x->leaves[i].rows = p->leaves[k].table ? p->leaves[k].table->n_rows : 0;
                break;
            }
    }
    x->n_leaves = n;
    for (i = 0; i < def->n_sigroles; i++) {
        const yexp_sigrole* r = &def->sigroles[i];
        const char* why = NULL;
        if (r->type == YEXP_SIG_POSITION2D_RELATIVE || (r->type == YEXP_SIG_POSITION2D && r->allow_relative))
            why = "relative: the preview does not capture the mouse";
        else if (r->type == YEXP_SIG_POSITION3D) why = "position3d";
        else if (r->type == YEXP_SIG_SCALAR) why = "scalar";
        else if (r->type == YEXP_SIG_GAZE) why = "gaze";
        else if (r->units == YEXP_UNITS_WORKSPACE_CM) why = "workspace_cm units";
        else if (r->units == YEXP_UNITS_DEVICE) why = "device units";
        if (why) {
            x->undriven[x->n_undriven].name = r->name;
            x->undriven[x->n_undriven++].why = why;
        } else {
            x->drive[x->n_drive].role = i;
            x->drive[x->n_drive++].units = r->units;
        }
    }
    for (i = 0; i < def->n_roles; i++) {
        const yexp_role* r = &def->roles[i];
        if (r->direction != 1 && r->kind && strcmp(r->kind, "sync") == 0) {
            x->undriven[x->n_undriven].name = r->name;
            x->undriven[x->n_undriven++].why = "a sync input";
        }
    }
    {
        /* gfx's own scale factors (ysp/gfx.h, ygfx_open()), from the view
         * the load gave the gfx, else the built-in rig's: 570 mm away,
         * 0.27 mm a px (1920 px over 518.4 mm), as pl_gd_units() scales it */
        const double w = (double)p->sim.width;
        const double wmm = p->view.width_mm > 0 ? (double)p->view.width_mm : 0.27 * w;
        const double dmm = p->view.distance_mm > 0 ? (double)p->view.distance_mm : 570.0;
        x->ppu_cm = w * 10.0 / wmm;
        x->ppu_deg = w / wmm * dmm * tan(3.14159265358979323846 / 180.0);
    }
    return 0;
}

static void pv_fast_forward(pv_sess* x) {
    pl_player* p = x->p;
    const dsg_session_settings* st = x->set;
    int f = 0, k, last_trial = -1;
    int64_t trial_start = 0;
    x->ff = true;
    for (k = 0;; k++) {
        f = pl_sim_frame(p, false);
        if (f != 0 || x->ff_stop) break;
        if (k >= PV_FF_MAX_FRAMES) {
            x->ff_stop = PV_FF_CAP;
            break;
        }
        if (p->global_trial != last_trial) {
            last_trial = p->global_trial;
            trial_start = p->sim.frame;
        } else if (p->in_trial && p->def->n_sigroles > 0 && (p->sim.frame - trial_start) * p->sim.period > PV_FF_STALL_NS) {
            x->ff_stop = PV_FF_STALL;
            break;
        }
    }
    x->ff = false;
    if (f < 0) {
        refuse(x, p->error, p->fail_obj, p->fail_name);
        return;
    }
    if (f == 1) x->state = PV_ENDED;
    if (x->ff_stop == PV_FF_FOUND) return;
    if (x->ff_seen && st->pin_row >= 0)
        snprintf(x->note, sizeof x->note, "row %d was not drawn in this run of the leaf: seed %llu", (int)st->pin_row,
                 (unsigned long long)st->seed);
    else if (x->ff_stop == PV_FF_STALL)
        snprintf(x->note, sizeof x->note, "trial %d waits for input that the script cannot give (a signal role)", p->global_trial - 1);
    else if (x->ff_stop == PV_FF_CAP)
        snprintf(x->note, sizeof x->note, "the fast forward stopped after %d frames, before leaf '%.160s'", PV_FF_MAX_FRAMES, st->pin_leaf);
    else snprintf(x->note, sizeof x->note, "the walk did not reach leaf '%.160s': seed %llu", st->pin_leaf, (unsigned long long)st->seed);
}

/* settings.out_dir comes as the runs' folder (PROJECT/.designer/preview).
 * The load numbers the run, so two loads never race for a name, and
 * writes the run's folder back. */
static int pv_load(void* ctx, dsg_session* s, const char* text, size_t n, char* err, size_t cap) {
    const dsg_preview_gl* gl = (const dsg_preview_gl*)ctx;
    pv_sess* x = (pv_sess*)calloc(1, sizeof *x);
    pl_sim_desc d;
    pl_check_result r;
    char parent[DSG_PATH_MAX];
    char* slash;
    int lo = 1, hi = 0, i;
    size_t pack_n = 0;
    if (!x) {
        snprintf(err, cap, "out of memory for the session");
        return -1;
    }
    s->impl = x;
    x->set = &s->settings;
    /* until the UI says otherwise: a session that the worker drops as
     * stale never shows, so its data set goes at its close */
    x->discard = true;
    x->text = (char*)malloc(n + 1);
    x->p = (pl_player*)calloc(1, sizeof *x->p);
    if (!x->text || !x->p) {
        snprintf(err, cap, "out of memory for the session");
        return -1;
    }
    memcpy(x->text, text, n);
    x->text[n] = 0;
    x->n = n;
    snprintf(x->runs, sizeof x->runs, "%s", s->settings.out_dir);
    snprintf(parent, sizeof parent, "%s", x->runs);
    slash = strrchr(parent, '/');
    if (!x->runs[0] || !slash) {
        x->runs[0] = 0;
        snprintf(err, cap, "the preview has no folder for its data sets");
        return -1;
    }
    *slash = 0;
    if (dsg_make_dir(parent, err, cap) != 0 || dsg_make_dir(x->runs, err, cap) != 0) {
        x->runs[0] = 0;
        return -1;
    }
    runs_read(x->runs, &lo, &hi);
    x->run_n = hi + 1;
    for (i = 0; i < 1000; i++) {
        const int k = snprintf(s->settings.out_dir, sizeof s->settings.out_dir, "%s/run-%d", x->runs, x->run_n);
        if (k < 0 || (size_t)k >= sizeof s->settings.out_dir) {
            snprintf(err, cap, "the preview's folder is too long");
            return -1;
        }
        if (!dsg_is_dir(s->settings.out_dir) && !dsg_is_file(s->settings.out_dir)) break;
        x->run_n++;
    }
    runs_write(x->runs, lo, x->run_n);

    memset(&d, 0, sizeof d);
    d.pack.data = s->pack ? dsg_pack_bytes(s->pack, &pack_n) : NULL;
    d.pack.size = pack_n;
    if (!d.pack.data) {
        snprintf(err, cap, "no check pack to load the session from");
        return -1;
    }
    d.def_text = x->text;
    d.def_len = n;
    d.seed = s->settings.seed;
    d.n_fields = s->settings.n_fields < 0 ? 0 : s->settings.n_fields > DSG_SESSION_FIELDS ? DSG_SESSION_FIELDS : s->settings.n_fields;
    if (d.n_fields > PL_MAX_FIELDS) d.n_fields = PL_MAX_FIELDS;
    for (i = 0; i < d.n_fields; i++) {
        d.fields[i].name = s->settings.fields[i].name;
        d.fields[i].value = s->settings.fields[i].value;
    }
    d.out_dir = s->settings.out_dir;
    d.participant = s->settings.participant[0] ? s->settings.participant : NULL;
    d.rt_s = s->settings.rt_s;
    d.width = s->settings.width;
    d.height = s->settings.height;
    d.period_ns = s->settings.hz > 0 ? (1000000000 + s->settings.hz / 2) / s->settings.hz : 0;
    if (gl && gl->caps) {
        d.gl_caps = gl->caps;
        d.cache = gl->cache;
        d.gl_proc = gl->proc;
        d.gl_ctx = gl->proc_ctx;
        d.share = gl->share;
    }
    d.live = pv_live;
    d.on_code = pv_on_code;
    d.ctx = x;
    memset(&r, 0, sizeof r);
    x->loaded = true;
    if (pl_sim_load(x->p, &d, &r) != PL_CHECK_OK) {
        /* posted, not failed: the UI places the refusal in the text and
         * keeps the session it shows */
        refuse(x, r.message && r.message[0] ? r.message : x->p->error, r.object, r.object_name);
        return 0;
    }
    if (pv_lists(x) != 0) {
        snprintf(err, cap, "out of memory for the session");
        return -1;
    }
    if (s->settings.pin_leaf[0]) pv_fast_forward(x);
    return 0;
}

/* The close: pl_close() (the writer's last rows, session.json), then the
 * data sets that are done with. keep is the run that the UI shows now:
 * every run below it is an older one, a stale one, or another process's. */
static void pv_close(void* ctx, dsg_session* s) {
    pv_sess* x = (pv_sess*)s->impl;
    (void)ctx;
    if (!x) return;
    if (x->p) {
        if (x->loaded) pl_close(x->p);
        free(x->p);
    }
    if (x->runs[0]) {
        int lo = 1, hi = 0, m;
        bool all = true;
        if (x->discard && x->run_n > 0 && x->run_n != x->keep) (void)remove_run(x->runs, x->run_n);
        if (x->keep > 0) {
            runs_read(x->runs, &lo, &hi);
            for (m = lo; m < x->keep; m++) all = remove_run(x->runs, m) && all;
            /* an open file stops a removal on Windows: the next close then
             * tries again from the same place */
            if (all && x->keep > lo) runs_write(x->runs, x->keep, hi > x->keep ? hi : x->keep);
        }
    }
    free(x->text);
    free(x->leaves);
    free(x->drive);
    free(x->undriven);
    free(x);
    s->impl = NULL;
}

void dsg_preview_session_ops(const dsg_preview_gl* gl, dsg_session_ops* out) {
    memset(out, 0, sizeof *out);
    out->load = pv_load;
    out->close = pv_close;
    out->ctx = (void*)(uintptr_t)gl;   /* the ops only read it */
}

/* --- hand-backs (UI thread) ------------------------------------------------------- */

/* A session that the UI is done with: its GL objects go here, on the GL
 * thread; the rest on the check thread. */
static void hand_back(dsg_preview* pv, dsg_session* s, bool discard, int keep) {
    pv_sess* x = impl_of(s);
    if (!s) return;
    if (x) {
        if (x->p && x->loaded) {
            pl_sim_stop(x->p);
            if (x->attach_started) pl_sim_detach(x->p);
        }
        x->discard = discard;
        x->keep = keep;
    }
    dsg_worker_close(pv->worker, s);
}

/* The first session of an open has no old one to carry the removal of the
 * runs below it (another process's, or an earlier open's): a session with
 * no player carries it. */
static void send_cleanup(dsg_preview* pv, int keep) {
    dsg_session* s;
    pv_sess* x;
    if (!pv->runs[0] || keep <= 1) return;
    s = (dsg_session*)calloc(1, sizeof *s);
    x = (pv_sess*)calloc(1, sizeof *x);
    if (!s || !x) {
        free(s);
        free(x);
        return;
    }
    snprintf(x->runs, sizeof x->runs, "%s", pv->runs);
    x->keep = keep;
    s->impl = x;
    dsg_worker_close(pv->worker, s);
}

/* --- the issue ----------------------------------------------------------------------- */

static void set_issue(dsg_preview* pv, const char* msg, int obj, const char* name, const dsg_place* at) {
    snprintf(pv->issue_msg, sizeof pv->issue_msg, "%s", msg && msg[0] ? msg : "the session refused the definition");
    snprintf(pv->issue_name, sizeof pv->issue_name, "%s", name ? name : "");
    memset(&pv->issue, 0, sizeof pv->issue);
    pv->issue.severity = DSG_SEV_ERROR;
    pv->issue.from = DSG_FROM_PREVIEW;
    if (at) pv->issue.place = *at;
    pv->issue.object = obj;
    pv->issue.name = pv->issue_name[0] ? pv->issue_name : NULL;
    pv->issue.message = pv->issue_msg;
    pv->have_issue = true;
}

/* --- the preview ------------------------------------------------------------------------ */

dsg_preview* dsg_preview_new(const dsg_preview_gl* gl, dsg_worker* w, struct dsg_doc* doc) {
    dsg_preview* pv = (dsg_preview*)calloc(1, sizeof *pv);
    if (!pv) return NULL;
    pv->gl = gl;
    pv->worker = w;
    pv->doc = doc;
    dsg_preview_settings_default(&pv->settings);
    return pv;
}

void dsg_preview_free(dsg_preview* pv) {
    if (!pv) return;
    dsg_preview_close(pv);
    free(pv);
}

void dsg_preview_open(dsg_preview* pv, const dsg_project* p) {
    dsg_preview_close(pv);
    pv->open = true;
    if (dsg_join(pv->runs, sizeof pv->runs, p->folder, DSG_DIR "/preview") != 0) pv->runs[0] = 0;
    pv->floor = dsg_doc_revision(pv->doc);
    pv->seen_rev = pv->floor;
    pv->changed = false;
    pv->cleaned = false;
    pv->paused = false;
}

void dsg_preview_close(dsg_preview* pv) {
    pv_sess* x = impl_of(pv->shown);
    hand_back(pv, pv->attaching, true, 0);
    pv->attaching = NULL;
    /* the shown run's data set stays for the person to read */
    hand_back(pv, pv->shown, false, x ? x->run_n : 0);
    pv->shown = NULL;
    dsg_pack_release(pv->ok_pack);
    pv->ok_pack = NULL;
    pv->have_ok = false;
    pv->have_check = false;
    pv->have_posted = false;
    pv->waiting = false;
    pv->restart_due = false;
    pv->first_failed = false;
    pv->step_req = false;
    pv->n_in = 0;
    pv->ptr_dirty = false;
    pv->keyboard = false;
    pv->open = false;
    pv->have_issue = false;
}

void dsg_preview_checked(dsg_preview* pv, const dsg_check_result* r, int64_t now_ns) {
    (void)now_ns;
    if (!pv->open || r->revision < pv->floor) return;
    pv->have_check = true;
    pv->check_rev = r->revision;
    pv->check_status = r->report.status;
    pv->check_errors = r->report.n_errors;
    if (r->report.status == DSG_CHECK_OK && r->pack) {
        dsg_pack* pk = dsg_pack_retain(r->pack);
        dsg_pack_release(pv->ok_pack);
        pv->ok_pack = pk;
        pv->ok_rev = r->revision;
        pv->have_ok = true;
    }
}

/* Stale: another revision or other settings than the newest job's, an
 * edit since that job, or a newer job that waits or runs. Such a session
 * never attaches. */
static bool stale(const dsg_preview* pv, const dsg_session* s) {
    return !pv->open || s->revision < pv->floor || !pv->have_posted || s->revision != pv->posted_rev ||
           dsg_doc_revision(pv->doc) != pv->posted_doc_rev || !same_settings(&s->settings, &pv->settings.session) ||
           dsg_worker_session_queued(pv->worker);
}

void dsg_preview_adopt(dsg_preview* pv, dsg_session_result* r, int64_t now_ns) {
    dsg_session* s = r->session;
    pv_sess* x = impl_of(s);
    r->session = NULL;
    (void)now_ns;
    if (!s) {
        /* the load failed: an issue when it was the newest job's */
        if (pv->open && pv->have_posted && r->revision == pv->posted_rev && !dsg_worker_session_queued(pv->worker)) {
            pv->waiting = false;
            set_issue(pv, r->error, 0, NULL, NULL);
            if (!pv->shown && !pv->attaching) pv->first_failed = true;
        }
        return;
    }
    if (!x || stale(pv, s)) {
        hand_back(pv, s, true, 0);
        return;
    }
    pv->waiting = false;
    if (x->refused) {
        set_issue(pv, x->msg, x->obj, x->obj_name, &x->place);
        if (!pv->shown && !pv->attaching) pv->first_failed = true;
        hand_back(pv, s, true, 0);
        return;
    }
    /* newest wins: an attach in progress is an older job's */
    hand_back(pv, pv->attaching, true, 0);
    pv->attaching = s;
    pv->attaching_unix = pv->posted_unix;
    pv->first_failed = false;
    pv->have_issue = false;
}

static void post(dsg_preview* pv, const char* text, size_t n, dsg_pack* pack, uint64_t rev) {
    dsg_session_settings ss = pv->settings.session;
    snprintf(ss.out_dir, sizeof ss.out_dir, "%s", pv->runs);
    pv->restart_due = false;
    if (dsg_worker_session(pv->worker, text, n, pack, rev, &ss) != 0) {
        set_issue(pv, "the preview's session was not started: out of memory", 0, NULL, NULL);
        return;
    }
    pv->have_posted = true;
    pv->posted_rev = rev;
    pv->posted_doc_rev = dsg_doc_revision(pv->doc);
    pv->posted_unix = (int64_t)time(NULL);
    pv->waiting = true;
    /* without a thread the job runs here, as ui.cpp's check does */
    if (!dsg_worker_threaded(pv->worker)) (void)dsg_worker_wait(pv->worker, 0);
}

static bool ok_now(const dsg_preview* pv) { return pv->have_ok && pv->ok_pack && pv->ok_rev == dsg_doc_revision(pv->doc); }

static bool settled(const dsg_preview* pv, int64_t now) { return !pv->changed || now - pv->changed_at >= PV_SETTLE_NS; }

static void maybe_post(dsg_preview* pv, int64_t now) {
    size_t n = 0;
    const char* t;
    if (pv->restart_due) {
        if (ok_now(pv)) {
            t = dsg_doc_bytes(pv->doc, DSG_BUF_DEF, &n);
            post(pv, t, n, pv->ok_pack, pv->ok_rev);
        } else if (pv->shown) {
            /* the text now is refused: the last good one again */
            pv_sess* x = impl_of(pv->shown);
            post(pv, x->text, x->n, pv->shown->pack, pv->shown->revision);
        }
        return;
    }
    if (ok_now(pv) && settled(pv, now) && (!pv->have_posted || pv->posted_rev != pv->ok_rev)) {
        t = dsg_doc_bytes(pv->doc, DSG_BUF_DEF, &n);
        post(pv, t, n, pv->ok_pack, pv->ok_rev);
    }
}

static void anchor(dsg_preview* pv, int64_t now) {
    pl_player* p = shown_player(pv);
    pv->reanchor = false;
    pv->real_anchor = now;
    pv->sess_anchor = p ? p->sim.frame * p->sim.period : 0;
}

static void run_refused(dsg_preview* pv, pv_sess* x) {
    pl_player* p = x->p;
    dsg_place at;
    memset(&at, 0, sizeof at);
    /* once, at the refusal: the parse allocates, and the session stops */
    (void)dsg_locate_object(x->text, x->n, p->fail_obj, p->fail_name, &at);
    x->state = PV_FAILED;
    set_issue(pv, p->error, p->fail_obj, p->fail_name, &at);
}

static void attach_step(dsg_preview* pv, int64_t now) {
    dsg_session* s = pv->attaching;
    pv_sess* x = impl_of(s);
    int rc;
    x->attach_started = true;
    rc = pl_sim_attach(x->p, PV_ATTACH_BUDGET_NS);
    if (rc == 0) return;
    pv->attaching = NULL;
    if (rc < 0) {
        set_issue(pv, x->p->error, 0, NULL, NULL);
        if (!pv->shown) pv->first_failed = true;
        hand_back(pv, s, true, 0);
        return;
    }
    /* attached: the old session stops and goes, the new one shows; its
     * first step is the next frame's */
    if (pv->shown) hand_back(pv, pv->shown, true, x->run_n);
    else if (!pv->cleaned) send_cleanup(pv, x->run_n);
    pv->cleaned = true;
    pv->shown = s;
    pv->shown_unix = pv->attaching_unix;
    pv->n_in = 0;
    pv->ptr_dirty = pv->ptr_inside;
    anchor(pv, now);
    /* paused: one drawn frame, so the pane shows the new definition */
    if (pv->paused) pv->step_req = true;
}

/* Input reaches the session in the next frame that it steps: that frame's
 * onset less 1 ns ("Input": the resolution is the UI frame). */
static void deliver(dsg_preview* pv, pv_sess* x) {
    pl_player* p = x->p;
    const int64_t t = pl_sim_next_onset(p) - 1;
    int i;
    for (i = 0; i < pv->n_in; i++) {
        const pv_in* e = &pv->in[i];
        if (e->choice) (void)pl_sim_choice(p, t, e->code, e->down != 0);
        else (void)pl_sim_key(p, t, e->code, e->down != 0);
    }
    pv->n_in = 0;
    if (pv->ptr_dirty && pv->ptr_inside) {
        /* D22: the origin at the display's center, x right, y down */
        const double cx = (double)pv->ptr_x - 0.5 * (double)p->sim.width, cy = (double)pv->ptr_y - 0.5 * (double)p->sim.height;
        for (i = 0; i < x->n_drive; i++) {
            const int u = x->drive[i].units;
            const double k = u == YEXP_UNITS_DISPLAY_CM ? x->ppu_cm : u == YEXP_UNITS_DISPLAY_DEG ? x->ppu_deg : 1.0;
            (void)pl_sim_sample(p, t, x->drive[i].role, (float)(cx / k), (float)(cy / k));
        }
    }
    pv->ptr_dirty = false;
}

static void step(dsg_preview* pv, int64_t now) {
    pv_sess* x = impl_of(pv->shown);
    pl_player* p;
    int64_t steps = 0, i;
    const bool one = pv->step_req;
    pv->step_req = false;
    if (!x || x->state != PV_LIVE) return;
    p = x->p;
    if (one) steps = 1;
    else if (pv->paused) return;
    else {
        const int64_t due = pv->sess_anchor + (int64_t)((double)(now - pv->real_anchor) * pv->settings.speed);
        const int64_t next = p->sim.frame * p->sim.period;
        if (next > due) return;
        steps = (due - next) / p->sim.period + 1;
        if (steps > PV_MAX_STEPS) {
            steps = PV_MAX_STEPS;
            pv->slow = true;
        }
    }
    deliver(pv, x);
    for (i = 0; i < steps; i++) {
        const int f = pl_sim_frame(p, i == steps - 1);
        if (f == 1) {
            x->state = PV_ENDED;
            break;
        }
        if (f < 0) {
            run_refused(pv, x);
            break;
        }
    }
    if (one) {
        pv->paused = true;
        anchor(pv, now);
    } else if (pv->slow) {
        /* the frames past the cap are dropped: the clock restarts at the
         * last one stepped, so the session never races to catch up */
        pv->real_anchor = now;
        pv->sess_anchor = (p->sim.frame - 1) * p->sim.period;
    }
}

void dsg_preview_tick(dsg_preview* pv, int64_t now_ns, bool shown) {
    uint64_t rev;
    pv->now = now_ns;
    pv->slow = false;
    if (!pv->open) {
        pv->pane = false;
        return;
    }
    rev = dsg_doc_revision(pv->doc);
    if (rev != pv->seen_rev) {
        pv->seen_rev = rev;
        pv->changed_at = now_ns;
        pv->changed = true;
    }
    if (!shown) {
        /* a hidden pane pauses: its time does not count */
        pv->pane = false;
        pv->reanchor = true;
        return;
    }
    pv->pane = true;
    if (pv->reanchor) anchor(pv, now_ns);
    maybe_post(pv, now_ns);
    if (pv->attaching) {
        const dsg_session* was = pv->shown;
        attach_step(pv, now_ns);
        if (pv->shown != was) return;
    }
    step(pv, now_ns);
}

static bool running(const dsg_preview* pv) {
    pv_sess* x = impl_of(pv->shown);
    return x && x->state == PV_LIVE && !pv->paused;
}

bool dsg_preview_animating(const dsg_preview* pv) { return pv->open && pv->pane && running(pv); }

bool dsg_preview_work(const dsg_preview* pv) {
    if (!pv->open || !pv->pane) return false;
    if (pv->attaching || pv->step_req) return true;
    if (pv->restart_due) return ok_now(pv) || pv->shown;
    return ok_now(pv) && (!pv->have_posted || pv->posted_rev != pv->ok_rev);
}

/* --- controls ------------------------------------------------------------------------------ */

void dsg_preview_run(dsg_preview* pv) {
    if (!pv->paused) return;
    pv->paused = false;
    pv->reanchor = true;
}

void dsg_preview_pause(dsg_preview* pv) { pv->paused = true; }

void dsg_preview_step(dsg_preview* pv) {
    pv->paused = true;
    pv->step_req = true;
}

void dsg_preview_restart(dsg_preview* pv) {
    if (pv->open) pv->restart_due = true;
}

void dsg_preview_set(dsg_preview* pv, const dsg_preview_settings* s) {
    const bool restart = !same_settings(&pv->settings.session, &s->session);
    const double speed = pv->settings.speed;
    if (running(pv) && !pv->reanchor && s->speed > 0 && (s->speed < speed || s->speed > speed)) {
        /* the time so far at the old speed, the rest at the new */
        pv->sess_anchor += (int64_t)((double)(pv->now - pv->real_anchor) * speed);
        pv->real_anchor = pv->now;
    }
    pv->settings = *s;
    pv->settings.session.out_dir[0] = 0;
    if (!(pv->settings.speed > 0)) pv->settings.speed = 1.0;
    if (restart && pv->open) pv->restart_due = true;
}

void dsg_preview_get(const dsg_preview* pv, dsg_preview_settings* out) { *out = pv->settings; }

bool dsg_preview_pin_template(dsg_preview* pv, const char* tmpl) {
    pv_sess* x = impl_of(pv->shown);
    int i;
    if (!x || !tmpl) return false;
    for (i = 0; i < x->n_leaves; i++)
        if (strcmp(x->leaves[i].tmpl, tmpl) == 0) {
            dsg_preview_settings s = pv->settings;
            snprintf(s.session.pin_leaf, sizeof s.session.pin_leaf, "%s", x->leaves[i].path);
            s.session.pin_row = -1;
            dsg_preview_set(pv, &s);
            pv->restart_due = pv->open;
            return true;
        }
    return false;
}

/* --- input ------------------------------------------------------------------------------------ */

void dsg_preview_set_keyboard(dsg_preview* pv, bool on) { pv->keyboard = on; }
bool dsg_preview_keyboard(const dsg_preview* pv) { return pv->keyboard; }

static void queue(dsg_preview* pv, bool choice, int code, bool down) {
    if (pv->n_in >= PV_IN_CAP) return;
    pv->in[pv->n_in].choice = (uint8_t)choice;
    pv->in[pv->n_in].down = (uint8_t)down;
    pv->in[pv->n_in].code = (int16_t)code;
    pv->n_in++;
}

bool dsg_preview_key(dsg_preview* pv, int scancode, bool down) {
    const bool tracked = scancode > 0 && scancode < 512;
    const unsigned bit = tracked ? 1u << (scancode & 7) : 0u;
    if (!down && tracked && (pv->keys_down[scancode >> 3] & bit)) {
        /* the session saw it go down, so it sees it go up */
        pv->keys_down[scancode >> 3] = (uint8_t)(pv->keys_down[scancode >> 3] & ~bit);
        queue(pv, false, scancode, false);
        return true;
    }
    if (!pv->keyboard) return false;
    if (scancode == DSG_SCANCODE_ESCAPE) {
        if (down) pv->keyboard = false;
        return true;
    }
    if (down && tracked && running(pv)) {
        pv->keys_down[scancode >> 3] = (uint8_t)(pv->keys_down[scancode >> 3] | bit);
        queue(pv, false, scancode, true);
    }
    return true;
}

void dsg_preview_choice(dsg_preview* pv, int i, bool down) {
    const uint64_t bit = i >= 0 && i < 64 ? (uint64_t)1 << i : 0;
    if (!bit) return;
    if (!down) {
        if (pv->choices_down & bit) {
            pv->choices_down &= ~bit;
            queue(pv, true, i, false);
        }
        return;
    }
    if (!running(pv)) return;
    pv->choices_down |= bit;
    queue(pv, true, i, true);
}

void dsg_preview_pointer(dsg_preview* pv, float x_px, float y_px, bool inside) {
    pv->ptr_inside = inside;
    if (!inside) return;
    pv->ptr_x = x_px;
    pv->ptr_y = y_px;
    pv->ptr_dirty = running(pv);
}

/* --- state -------------------------------------------------------------------------------------- */

void dsg_preview_get_info(const dsg_preview* pv, dsg_preview_info* out) {
    pv_sess* x = impl_of(pv->shown);
    memset(out, 0, sizeof *out);
    out->out_dir = "";
    out->note = "";
    out->keyboard = pv->keyboard;
    out->loading = pv->open && pv->waiting;
    out->attaching = pv->attaching != NULL;
    out->slow = pv->slow;
    if (x) {
        const pl_player* p = x->p;
        out->status = x->state == PV_FAILED ? DSG_PV_FAILED : x->state == PV_ENDED ? DSG_PV_ENDED : pv->paused ? DSG_PV_PAUSED : DSG_PV_RUNNING;
        out->stale = pv->have_check && pv->check_rev > pv->shown->revision && pv->check_status != DSG_CHECK_OK;
        out->stale_errors = out->stale ? pv->check_errors : 0;
        out->shown_unix_s = pv->shown_unix;
        out->width = p->sim.width;
        out->height = p->sim.height;
        out->hz = (int32_t)((1000000000 + p->sim.period / 2) / p->sim.period);
        out->trials = p->trials_done;
        out->out_dir = pv->shown->settings.out_dir;
        out->note = x->note;
    } else if (pv->attaching) out->status = DSG_PV_ATTACHING;
    else if (pv->first_failed) out->status = DSG_PV_FAILED;
    else if (pv->open && pv->waiting) out->status = DSG_PV_WAITING;
    else out->status = DSG_PV_NONE;
}

bool dsg_preview_where_now(const dsg_preview* pv, dsg_preview_where* out) {
    const pl_player* p = shown_player(pv);
    pl_sim_where w;
    memset(&w, 0, sizeof w);
    memset(out, 0, sizeof *out);
    out->trial = -1;
    out->condition = -1;
    if (!p) return false;
    pl_sim_where_now(p, &w);
    out->trial = w.trial;
    out->leaf = w.leaf;
    out->condition = w.condition;
    out->tmpl = w.tmpl;
    out->state = w.state;
    out->phase = w.phase;
    out->frame = w.frame;
    out->t_ns = w.t_ns;
    out->outcome = w.outcome;
    out->response = w.response;
    out->rt_ns = w.rt_ns;
    return true;
}

bool dsg_preview_texture(const dsg_preview* pv, uint32_t* gl_name, int32_t* width, int32_t* height) {
    const pl_player* p = shown_player(pv);
    ygfx_native nat;
    memset(&nat, 0, sizeof nat);
    *gl_name = 0;
    *width = 0;
    *height = 0;
    if (!p || !pl_sim_texture(p, &nat)) return false;
    *gl_name = nat.gl_name;
    *width = p->sim.width;
    *height = p->sim.height;
    return true;
}

bool dsg_preview_issue(const dsg_preview* pv, dsg_issue* out) {
    if (!pv->have_issue) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = pv->issue;
    return true;
}

int dsg_preview_outputs(const dsg_preview* pv) {
    const pv_sess* x = impl_of(pv->shown);
    return x ? x->n_outs : 0;
}

bool dsg_preview_output_at(const dsg_preview* pv, int i, dsg_preview_output* out) {
    const pv_sess* x = impl_of(pv->shown);
    const pv_out* o;
    memset(out, 0, sizeof *out);
    if (!x || i < 0 || i >= x->n_outs) return false;
    o = &x->outs[(x->out_first + i) % DSG_PV_OUTPUTS];
    out->role = o->role;
    out->code = o->code;
    out->t_ns = o->t_ns;
    return true;
}

int dsg_preview_undriven(const dsg_preview* pv) {
    const pv_sess* x = impl_of(pv->shown);
    return x ? x->n_undriven : 0;
}

const char* dsg_preview_undriven_role(const dsg_preview* pv, int i, const char** why) {
    const pv_sess* x = impl_of(pv->shown);
    if (why) *why = "";
    if (!x || i < 0 || i >= x->n_undriven) return NULL;
    if (why) *why = x->undriven[i].why;
    return x->undriven[i].name;
}

int dsg_preview_choices(const dsg_preview* pv) {
    const pl_player* p = shown_player(pv);
    if (!p || !p->in_trial || !p->tmpl || !p->tmpl->collector) return 0;
    return p->tmpl->n_choices;
}

const char* dsg_preview_choice_label(const dsg_preview* pv, int i) {
    const pl_player* p = shown_player(pv);
    if (i < 0 || i >= dsg_preview_choices(pv)) return NULL;
    return p->tmpl->choice_keys ? p->tmpl->choice_keys[i] : NULL;
}

int dsg_preview_leaves(const dsg_preview* pv) {
    const pv_sess* x = impl_of(pv->shown);
    return x ? x->n_leaves : 0;
}

bool dsg_preview_leaf_at(const dsg_preview* pv, int i, dsg_preview_leaf* out) {
    const pv_sess* x = impl_of(pv->shown);
    memset(out, 0, sizeof *out);
    if (!x || i < 0 || i >= x->n_leaves) return false;
    *out = x->leaves[i];
    return true;
}
