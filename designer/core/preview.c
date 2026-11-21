/* preview.c - the preview (preview.h). OWNED BY PART A (designer step 5).
 *
 * STUB from part S: every call compiles and gives the state of a preview
 * that has no session. It never posts a session job, and its session ops
 * are the worker's stub (load NULL), so the designer runs as before step 5.
 * It keeps the settings and the keyboard claim, because ui.cpp round-trips
 * the settings through the view file and the pane sets them. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "preview.h"

#include <stdlib.h>
#include <string.h>

struct dsg_preview {
    const dsg_preview_gl* gl;
    dsg_worker*           worker;
    struct dsg_doc*       doc;
    dsg_preview_settings  settings;
    bool                  keyboard;
};

void dsg_preview_session_ops(const dsg_preview_gl* gl, dsg_session_ops* out) {
    memset(out, 0, sizeof *out);
    out->ctx = (void*)(uintptr_t)gl;   /* the ops only read it */
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

void dsg_preview_free(dsg_preview* pv) { free(pv); }

void dsg_preview_open(dsg_preview* pv, const dsg_project* p) {
    (void)pv;
    (void)p;
}

void dsg_preview_close(dsg_preview* pv) { pv->keyboard = false; }

void dsg_preview_checked(dsg_preview* pv, const dsg_check_result* r, int64_t now_ns) {
    (void)pv;
    (void)r;
    (void)now_ns;
}

/* The stub posts no job, so nothing arrives; a session that did would go
 * back to the worker, which closes it. */
void dsg_preview_adopt(dsg_preview* pv, dsg_session_result* r, int64_t now_ns) {
    (void)now_ns;
    if (r->session) dsg_worker_close(pv->worker, r->session);
    r->session = NULL;
}

void dsg_preview_tick(dsg_preview* pv, int64_t now_ns, bool shown) {
    (void)pv;
    (void)now_ns;
    (void)shown;
}

bool dsg_preview_animating(const dsg_preview* pv) {
    (void)pv;
    return false;
}

bool dsg_preview_work(const dsg_preview* pv) {
    (void)pv;
    return false;
}

void dsg_preview_run(dsg_preview* pv) { (void)pv; }
void dsg_preview_pause(dsg_preview* pv) { (void)pv; }
void dsg_preview_step(dsg_preview* pv) { (void)pv; }
void dsg_preview_restart(dsg_preview* pv) { (void)pv; }
void dsg_preview_set(dsg_preview* pv, const dsg_preview_settings* s) { pv->settings = *s; }
void dsg_preview_get(const dsg_preview* pv, dsg_preview_settings* out) { *out = pv->settings; }

bool dsg_preview_pin_template(dsg_preview* pv, const char* tmpl) {
    (void)pv;
    (void)tmpl;
    return false;
}

void dsg_preview_set_keyboard(dsg_preview* pv, bool on) { pv->keyboard = on; }
bool dsg_preview_keyboard(const dsg_preview* pv) { return pv->keyboard; }

bool dsg_preview_key(dsg_preview* pv, int scancode, bool down) {
    if (!pv->keyboard) return false;
    if (scancode == DSG_SCANCODE_ESCAPE && down) pv->keyboard = false;
    return true;
}

void dsg_preview_choice(dsg_preview* pv, int i, bool down) {
    (void)pv;
    (void)i;
    (void)down;
}

void dsg_preview_pointer(dsg_preview* pv, float x_px, float y_px, bool inside) {
    (void)pv;
    (void)x_px;
    (void)y_px;
    (void)inside;
}

void dsg_preview_get_info(const dsg_preview* pv, dsg_preview_info* out) {
    memset(out, 0, sizeof *out);
    out->status = DSG_PV_NONE;
    out->out_dir = "";
    out->note = "";
    out->keyboard = pv->keyboard;
}

bool dsg_preview_where_now(const dsg_preview* pv, dsg_preview_where* out) {
    (void)pv;
    memset(out, 0, sizeof *out);
    out->trial = -1;
    out->condition = -1;
    return false;
}

bool dsg_preview_texture(const dsg_preview* pv, uint32_t* gl_name, int32_t* width, int32_t* height) {
    (void)pv;
    *gl_name = 0;
    *width = 0;
    *height = 0;
    return false;
}

bool dsg_preview_issue(const dsg_preview* pv, dsg_issue* out) {
    (void)pv;
    memset(out, 0, sizeof *out);
    return false;
}

int dsg_preview_outputs(const dsg_preview* pv) {
    (void)pv;
    return 0;
}

bool dsg_preview_output_at(const dsg_preview* pv, int i, dsg_preview_output* out) {
    (void)pv;
    (void)i;
    memset(out, 0, sizeof *out);
    return false;
}

int dsg_preview_undriven(const dsg_preview* pv) {
    (void)pv;
    return 0;
}

const char* dsg_preview_undriven_role(const dsg_preview* pv, int i, const char** why) {
    (void)pv;
    (void)i;
    if (why) *why = "";
    return NULL;
}

int dsg_preview_choices(const dsg_preview* pv) {
    (void)pv;
    return 0;
}

const char* dsg_preview_choice_label(const dsg_preview* pv, int i) {
    (void)pv;
    (void)i;
    return NULL;
}

int dsg_preview_leaves(const dsg_preview* pv) {
    (void)pv;
    return 0;
}

bool dsg_preview_leaf_at(const dsg_preview* pv, int i, dsg_preview_leaf* out) {
    (void)pv;
    (void)i;
    memset(out, 0, sizeof *out);
    return false;
}
