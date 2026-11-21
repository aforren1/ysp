/* preview_double.c - a test double of designer/core/preview.h for the pane's
 * tests (preview_double.h says why). OWNED BY PART B.
 *
 * Off, every call acts as S's stub in preview.c. On, it keeps one shown
 * session and one being attached, and runs a frame grid with trials of at
 * most 2 s: a key or a choice ends the trial with that response. It is a
 * double of the contract in preview.h, not of the player: no definition is
 * read, and the choices, roles and leaves are the test's script. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "preview_double.h"
#if DSG_PREVIEW_DOUBLE
#include "preview.h"
#include "doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PVD_SETTLE_NS 300000000LL
#define PVD_MAX_STEPS 16
#define PVD_TRIAL_NS 2000000000LL
#define PVD_MAX 8

static struct {
    bool on;
    int n_choices, n_undriven, n_leaves;
    char choices[PVD_MAX][32];
    char undriven[PVD_MAX][64], why[PVD_MAX][64];
    char leaf_path[PVD_MAX][64], leaf_tmpl[PVD_MAX][32];
    int leaf_rows[PVD_MAX];
    char out_role[32];
    uint32_t texture;
    char fail[256];
    bool end;
    int posts, adopted, steps, pointer_calls, choice_last, restarts;
    float px, py;
    bool pin;
} D;

struct dsg_preview {
    const dsg_preview_gl* gl;
    dsg_worker* worker;
    struct dsg_doc* doc;
    dsg_preview_settings settings;
    bool keyboard;
    /* the double's own state */
    bool open;
    char folder[DSG_PATH_MAX];
    uint64_t floor;
    uint64_t seen_rev;          /* the document's revision at the last tick */
    int64_t changed_ns;         /* when it last changed                     */
    uint64_t check_rev;         /* the newest check's                        */
    int check_status, check_errors;
    uint64_t ok_rev;            /* the newest accepted check's               */
    dsg_pack* ok_pack;
    uint64_t posted_rev;        /* the newest job's; 0 none                  */
    bool post_now;              /* a restart or a setting: no settle         */
    int64_t posted_unix;
    int run;
    dsg_session* shown;
    dsg_session* next;          /* adopted, attached at the next tick        */
    int status;
    bool shown_flag, slow, step_once, was_paused;
    int64_t last_tick;
    double vt;                  /* the session's time, ns                    */
    int64_t frame;
    int trials;
    int64_t trial_t0;
    char response[32], outcome[16];
    int64_t rt_ns;
    bool have_issue;
    char issue_msg[256];
    dsg_preview_output outs[DSG_PV_OUTPUTS];
    char out_roles[DSG_PV_OUTPUTS][32];
    int n_outs;
    char out_dir[DSG_PATH_MAX];
    bool key_down[512];
};

static void copy(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src ? src : ""); }

void pvd_enable(bool on) {
    memset(&D, 0, sizeof D);
    D.on = on;
    D.choice_last = -1;
}
void pvd_set_choices(int n, const char* const* labels) {
    D.n_choices = n < PVD_MAX ? n : PVD_MAX;
    for (int i = 0; i < D.n_choices; i++) copy(D.choices[i], sizeof D.choices[i], labels[i]);
}
void pvd_set_undriven(int n, const char* const* roles, const char* const* why) {
    D.n_undriven = n < PVD_MAX ? n : PVD_MAX;
    for (int i = 0; i < D.n_undriven; i++) {
        copy(D.undriven[i], sizeof D.undriven[i], roles[i]);
        copy(D.why[i], sizeof D.why[i], why[i]);
    }
}
void pvd_set_leaves(int n, const char* const* paths, const char* const* tmpls, const int* rows) {
    D.n_leaves = n < PVD_MAX ? n : PVD_MAX;
    for (int i = 0; i < D.n_leaves; i++) {
        copy(D.leaf_path[i], sizeof D.leaf_path[i], paths[i]);
        copy(D.leaf_tmpl[i], sizeof D.leaf_tmpl[i], tmpls[i]);
        D.leaf_rows[i] = rows[i];
    }
}
void pvd_set_output_role(const char* role) { copy(D.out_role, sizeof D.out_role, role); }
void pvd_set_texture(uint32_t name) { D.texture = name; }
void pvd_fail(const char* message) { copy(D.fail, sizeof D.fail, message); }
void pvd_end(void) { D.end = true; }
int pvd_posts(void) { return D.posts; }
int pvd_adopted(void) { return D.adopted; }
int pvd_steps(void) { return D.steps; }
int pvd_pointer_calls(void) { return D.pointer_calls; }
void pvd_pointer(float* x, float* y, bool* inside) {
    *x = D.px;
    *y = D.py;
    *inside = D.pin;
}
int pvd_choice_last(void) { return D.choice_last; }
int pvd_restarts(void) { return D.restarts; }

/* --- preview.h ------------------------------------------------------------- */

void dsg_preview_session_ops(const dsg_preview_gl* gl, dsg_session_ops* out) {
    memset(out, 0, sizeof *out);
    out->ctx = (void*)(uintptr_t)gl;
}

dsg_preview* dsg_preview_new(const dsg_preview_gl* gl, dsg_worker* w, struct dsg_doc* doc) {
    dsg_preview* pv = (dsg_preview*)calloc(1, sizeof *pv);
    if (!pv) return NULL;
    pv->gl = gl;
    pv->worker = w;
    pv->doc = doc;
    dsg_preview_settings_default(&pv->settings);
    return pv;
}

static void hand_back(dsg_preview* pv) {
    if (pv->shown) dsg_worker_close(pv->worker, pv->shown);
    if (pv->next) dsg_worker_close(pv->worker, pv->next);
    pv->shown = pv->next = NULL;
}

void dsg_preview_close(dsg_preview* pv) {
    hand_back(pv);
    dsg_pack_release(pv->ok_pack);
    pv->ok_pack = NULL;
    pv->keyboard = false;
    pv->open = false;
    pv->status = DSG_PV_NONE;
    pv->have_issue = false;
    pv->n_outs = 0;
    pv->posted_rev = pv->ok_rev = pv->check_rev = 0;
    pv->out_dir[0] = 0;
}

void dsg_preview_free(dsg_preview* pv) {
    if (!pv) return;
    dsg_preview_close(pv);
    free(pv);
}

void dsg_preview_open(dsg_preview* pv, const dsg_project* p) {
    pv->open = true;
    copy(pv->folder, sizeof pv->folder, p->folder);
    pv->floor = dsg_doc_revision(pv->doc);
    pv->seen_rev = 0;
    pv->run = 0;
}

void dsg_preview_checked(dsg_preview* pv, const dsg_check_result* r, int64_t now_ns) {
    (void)now_ns;
    if (!D.on) return;
    pv->check_rev = r->revision;
    pv->check_status = r->report.status;
    pv->check_errors = r->report.n_errors;
    if (r->report.status == DSG_CHECK_OK && r->pack) {
        dsg_pack_release(pv->ok_pack);
        pv->ok_pack = dsg_pack_retain(r->pack);
        pv->ok_rev = r->revision;
    }
}

void dsg_preview_adopt(dsg_preview* pv, dsg_session_result* r, int64_t now_ns) {
    (void)now_ns;
    dsg_session* s = r->session;
    r->session = NULL;
    if (!s) {
        pv->have_issue = true;
        copy(pv->issue_msg, sizeof pv->issue_msg, r->error);
        if (!pv->shown) pv->status = DSG_PV_FAILED;
        return;
    }
    if (!D.on || !pv->open || s->revision < pv->floor || s->revision != pv->posted_rev || dsg_worker_session_queued(pv->worker)) {
        dsg_worker_close(pv->worker, s);
        return;
    }
    if (pv->next) dsg_worker_close(pv->worker, pv->next);
    pv->next = s;
    if (!pv->shown) pv->status = DSG_PV_ATTACHING;
}

static void post(dsg_preview* pv, int64_t now_ns) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(pv->doc, DSG_BUF_DEF, &n);
    dsg_session_settings s = pv->settings.session;
    snprintf(s.out_dir, sizeof s.out_dir, "%s/.designer/preview/run-%d", pv->folder, pv->run + 1);
    if (dsg_worker_session(pv->worker, t, n, pv->ok_pack, pv->ok_rev, &s) != 0) return;
    pv->run++;
    pv->posted_rev = pv->ok_rev;
    pv->posted_unix = (int64_t)time(NULL);
    pv->post_now = false;
    pv->changed_ns = now_ns;
    D.posts++;
    if (!pv->shown && !pv->next) pv->status = DSG_PV_WAITING;
}

static void respond(dsg_preview* pv, const char* name, uint32_t code) {
    copy(pv->response, sizeof pv->response, name);
    copy(pv->outcome, sizeof pv->outcome, "response");
    pv->rt_ns = (int64_t)pv->vt - pv->trial_t0;
    pv->trials++;
    pv->trial_t0 = (int64_t)pv->vt;
    if (D.out_role[0]) {
        if (pv->n_outs == DSG_PV_OUTPUTS) {   /* the oldest goes */
            memmove(pv->outs, pv->outs + 1, sizeof pv->outs[0] * (DSG_PV_OUTPUTS - 1));
            memmove(pv->out_roles, pv->out_roles + 1, sizeof pv->out_roles[0] * (DSG_PV_OUTPUTS - 1));
            pv->n_outs--;
        }
        const int i = pv->n_outs++;
        copy(pv->out_roles[i], sizeof pv->out_roles[i], D.out_role);
        pv->outs[i].code = code;
        pv->outs[i].t_ns = (int64_t)pv->vt;
    }
}

static void step(dsg_preview* pv) {
    const int64_t period = 1000000000LL / (pv->shown->settings.hz > 0 ? pv->shown->settings.hz : 60);
    pv->frame++;
    D.steps++;
    if ((int64_t)pv->vt < pv->frame * period) pv->vt = (double)(pv->frame * period);
    if ((int64_t)pv->vt - pv->trial_t0 >= PVD_TRIAL_NS) {
        pv->response[0] = 0;
        copy(pv->outcome, sizeof pv->outcome, "timeout");
        pv->trials++;
        pv->trial_t0 = (int64_t)pv->vt;
    }
}

void dsg_preview_tick(dsg_preview* pv, int64_t now_ns, bool shown) {
    const int64_t dt = pv->last_tick ? now_ns - pv->last_tick : 0;
    pv->last_tick = now_ns;
    pv->shown_flag = shown;
    pv->slow = false;
    if (!D.on || !pv->open) return;
    const uint64_t rev = dsg_doc_revision(pv->doc);
    if (rev != pv->seen_rev) {
        pv->seen_rev = rev;
        pv->changed_ns = now_ns;
    }
    if (!shown) {
        if (pv->status == DSG_PV_RUNNING) pv->status = DSG_PV_PAUSED;
        return;
    }
    if (pv->ok_pack && pv->ok_rev == rev && (pv->post_now || (pv->posted_rev != rev && now_ns - pv->changed_ns >= PVD_SETTLE_NS)))
        post(pv, now_ns);
    if (pv->next) {
        const bool paused = pv->shown && pv->status == DSG_PV_PAUSED;
        if (pv->shown) dsg_worker_close(pv->worker, pv->shown);
        pv->shown = pv->next;
        pv->next = NULL;
        pv->status = paused ? DSG_PV_PAUSED : DSG_PV_RUNNING;
        pv->vt = 0;
        pv->frame = 0;
        pv->trials = 0;
        pv->trial_t0 = 0;
        pv->response[0] = pv->outcome[0] = 0;
        pv->n_outs = 0;
        pv->have_issue = false;
        copy(pv->out_dir, sizeof pv->out_dir, pv->shown->settings.out_dir);
        D.adopted++;
        return;
    }
    if (!pv->shown) return;
    if (D.fail[0]) {
        pv->status = DSG_PV_FAILED;
        pv->have_issue = true;
        copy(pv->issue_msg, sizeof pv->issue_msg, D.fail);
        D.fail[0] = 0;
        return;
    }
    if (D.end) {
        pv->status = DSG_PV_ENDED;
        D.end = false;
        return;
    }
    if (pv->step_once) {
        pv->step_once = false;
        step(pv);
        pv->status = DSG_PV_PAUSED;
        return;
    }
    if (pv->status != DSG_PV_RUNNING) return;
    const int64_t period = 1000000000LL / (pv->shown->settings.hz > 0 ? pv->shown->settings.hz : 60);
    pv->vt += pv->settings.speed * (double)dt;
    const int64_t due = (int64_t)pv->vt / period - pv->frame;
    for (int64_t i = 0; i < due && i < PVD_MAX_STEPS; i++) step(pv);
    if (due > PVD_MAX_STEPS) {
        pv->slow = true;
        pv->frame += due - PVD_MAX_STEPS;
    }
}

bool dsg_preview_animating(const dsg_preview* pv) { return pv->shown && pv->status == DSG_PV_RUNNING && pv->shown_flag; }

bool dsg_preview_work(const dsg_preview* pv) {
    if (!D.on || !pv->open) return false;
    return pv->next || pv->step_once || pv->post_now || (pv->ok_pack && pv->posted_rev != pv->ok_rev);
}

void dsg_preview_run(dsg_preview* pv) {
    if (pv->shown && pv->status == DSG_PV_PAUSED) pv->status = DSG_PV_RUNNING;
}
void dsg_preview_pause(dsg_preview* pv) {
    if (pv->shown && pv->status == DSG_PV_RUNNING) pv->status = DSG_PV_PAUSED;
}
void dsg_preview_step(dsg_preview* pv) {
    if (pv->shown && (pv->status == DSG_PV_RUNNING || pv->status == DSG_PV_PAUSED)) pv->step_once = true;
}
void dsg_preview_restart(dsg_preview* pv) {
    D.restarts++;
    if (D.on) pv->post_now = true;
}

void dsg_preview_set(dsg_preview* pv, const dsg_preview_settings* s) {
    const dsg_session_settings* a = &pv->settings.session;
    const dsg_session_settings* b = &s->session;
    const bool restart = a->width != b->width || a->height != b->height || a->hz != b->hz || a->seed != b->seed ||
                         strcmp(a->participant, b->participant) != 0 || a->rt_s != b->rt_s || strcmp(a->pin_leaf, b->pin_leaf) != 0 ||
                         a->pin_row != b->pin_row || a->n_fields != b->n_fields ||
                         memcmp(a->fields, b->fields, sizeof a->fields[0] * (size_t)(a->n_fields > 0 ? a->n_fields : 0)) != 0;
    pv->settings = *s;
    if (restart && D.on && pv->shown) pv->post_now = true;
}
void dsg_preview_get(const dsg_preview* pv, dsg_preview_settings* out) { *out = pv->settings; }

bool dsg_preview_pin_template(dsg_preview* pv, const char* tmpl) {
    for (int i = 0; i < D.n_leaves; i++)
        if (strcmp(D.leaf_tmpl[i], tmpl) == 0) {
            dsg_preview_settings s = pv->settings;
            copy(s.session.pin_leaf, sizeof s.session.pin_leaf, D.leaf_path[i]);
            s.session.pin_row = -1;
            dsg_preview_set(pv, &s);
            return true;
        }
    return false;
}

void dsg_preview_set_keyboard(dsg_preview* pv, bool on) { pv->keyboard = on; }
bool dsg_preview_keyboard(const dsg_preview* pv) { return pv->keyboard; }

static const char* key_name(int sc) {
    static char letter[2];
    if (sc == 44) return "space";
    if (sc == 79) return "right";
    if (sc == 80) return "left";
    if (sc >= 4 && sc <= 29) {
        letter[0] = (char)('a' + sc - 4);
        letter[1] = 0;
        return letter;
    }
    return "other";
}

bool dsg_preview_key(dsg_preview* pv, int scancode, bool down) {
    const bool in_range = scancode >= 0 && scancode < 512;
    if (!pv->keyboard) {
        /* a key up whose down the session took goes to the session */
        if (!down && in_range && pv->key_down[scancode]) {
            pv->key_down[scancode] = false;
            return true;
        }
        return false;
    }
    if (scancode == DSG_SCANCODE_ESCAPE) {
        if (down) pv->keyboard = false;
        return true;
    }
    if (in_range) pv->key_down[scancode] = down && pv->status == DSG_PV_RUNNING;
    if (down && pv->status == DSG_PV_RUNNING) respond(pv, key_name(scancode), (uint32_t)scancode);
    return true;
}

void dsg_preview_choice(dsg_preview* pv, int i, bool down) {
    if (!down || i < 0 || i >= D.n_choices || pv->status != DSG_PV_RUNNING) return;
    D.choice_last = i;
    respond(pv, D.choices[i], 101u + (uint32_t)i);
}

void dsg_preview_pointer(dsg_preview* pv, float x_px, float y_px, bool inside) {
    (void)pv;
    D.pointer_calls++;
    D.px = x_px;
    D.py = y_px;
    D.pin = inside;
}

void dsg_preview_get_info(const dsg_preview* pv, dsg_preview_info* out) {
    memset(out, 0, sizeof *out);
    out->status = pv->status;
    out->out_dir = pv->shown ? pv->out_dir : "";
    out->note = "";
    out->keyboard = pv->keyboard;
    if (!D.on) return;
    out->loading = pv->posted_rev && dsg_worker_session_queued(pv->worker);
    out->attaching = pv->next != NULL && pv->shown != NULL;
    out->stale = pv->shown && pv->check_rev > pv->shown->revision && pv->check_status != DSG_CHECK_OK;
    out->stale_errors = out->stale ? pv->check_errors : 0;
    out->shown_unix_s = pv->shown ? pv->posted_unix : 0;
    out->slow = pv->slow;
    if (pv->shown) {
        out->width = pv->shown->settings.width;
        out->height = pv->shown->settings.height;
        out->hz = pv->shown->settings.hz;
    }
    out->trials = pv->trials;
}

bool dsg_preview_where_now(const dsg_preview* pv, dsg_preview_where* out) {
    memset(out, 0, sizeof *out);
    out->trial = -1;
    out->condition = -1;
    if (!pv->shown) return false;
    out->trial = pv->trials;
    out->leaf = D.n_leaves ? D.leaf_path[0] : NULL;
    out->tmpl = D.n_leaves ? D.leaf_tmpl[0] : NULL;
    out->phase = DSG_PV_PHASE_RESPONSE;
    out->frame = pv->frame;
    out->t_ns = (int64_t)pv->vt;
    out->outcome = pv->outcome[0] ? pv->outcome : NULL;
    out->response = pv->outcome[0] ? pv->response : NULL;
    out->rt_ns = pv->rt_ns;
    return true;
}

bool dsg_preview_texture(const dsg_preview* pv, uint32_t* gl_name, int32_t* width, int32_t* height) {
    *gl_name = 0;
    *width = *height = 0;
    if (!pv->shown || !D.texture) return false;
    *gl_name = D.texture;
    *width = pv->shown->settings.width;
    *height = pv->shown->settings.height;
    return true;
}

bool dsg_preview_issue(const dsg_preview* pv, dsg_issue* out) {
    memset(out, 0, sizeof *out);
    if (!pv->have_issue) return false;
    out->severity = DSG_SEV_ERROR;
    out->from = DSG_FROM_PREVIEW;
    out->message = pv->issue_msg;
    return true;
}

int dsg_preview_outputs(const dsg_preview* pv) { return pv->n_outs; }

bool dsg_preview_output_at(const dsg_preview* pv, int i, dsg_preview_output* out) {
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= pv->n_outs) return false;
    *out = pv->outs[i];
    out->role = pv->out_roles[i];
    return true;
}

int dsg_preview_undriven(const dsg_preview* pv) { return pv->shown ? D.n_undriven : 0; }

const char* dsg_preview_undriven_role(const dsg_preview* pv, int i, const char** why) {
    if (why) *why = "";
    if (!pv->shown || i < 0 || i >= D.n_undriven) return NULL;
    if (why) *why = D.why[i];
    return D.undriven[i];
}

int dsg_preview_choices(const dsg_preview* pv) { return pv->shown ? D.n_choices : 0; }

const char* dsg_preview_choice_label(const dsg_preview* pv, int i) {
    if (!pv->shown || i < 0 || i >= D.n_choices) return NULL;
    return D.choices[i];
}

int dsg_preview_leaves(const dsg_preview* pv) { return pv->ok_pack ? D.n_leaves : 0; }

bool dsg_preview_leaf_at(const dsg_preview* pv, int i, dsg_preview_leaf* out) {
    memset(out, 0, sizeof *out);
    if (!pv->ok_pack || i < 0 || i >= D.n_leaves) return false;
    out->path = D.leaf_path[i];
    out->tmpl = D.leaf_tmpl[i];
    out->rows = D.leaf_rows[i];
    return true;
}
#else
/* Off: the linker takes preview.c from ysp_designer_core. */
typedef int pvd_off;
#endif
