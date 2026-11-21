/* preview_part.c - the preview's settings and the view file's part
 * "preview" (preview.h; docs/designer.md, "Part S as built"). Shared (part
 * S): ui.cpp reads the part at an open and writes it with the view file, so
 * the pane (part B) and the preview (part A) never touch the file. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "preview.h"

#include "ysp/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PART_KEY "preview"

/* The panel's choices ("The built-in simulated rig"). A view file written
 * by hand may hold others; the loader of a session refuses what it cannot
 * show, so the read only bounds them. */
#define PV_MAX_SIDE 16384
#define PV_MAX_HZ   1000

void dsg_preview_settings_default(dsg_preview_settings* s) {
    memset(s, 0, sizeof *s);
    s->session.width = 1920;
    s->session.height = 1080;
    s->session.hz = 60;
    s->session.seed = 1;
    snprintf(s->session.participant, sizeof s->session.participant, "correct");
    s->session.rt_s = 0.3;
    s->session.pin_row = -1;
    s->speed = 1.0;
    s->scale = DSG_PV_SCALE_FIT;
}

static bool speed_ok(double v) { return v == 0.25 || v == 0.5 || v == 1.0 || v == 2.0; }

static bool get_int(const yjs_value* v, int64_t lo, int64_t hi, int64_t* out) {
    int64_t x = 0;
    if (yjs_int64(v, &x) != YJS_OK || x < lo || x > hi) return false;
    *out = x;
    return true;
}

/* A string that fits cap with its NUL and holds no NUL of its own. */
static bool get_str(const yjs_value* v, char* out, size_t cap) {
    size_t n = 0;
    const char* s = yjs_string(v, &n);
    if (!s || n >= cap || memchr(s, 0, n)) return false;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}

/* A seed is a uint64: above INT64_MAX the number has no int64 form, so its
 * text is read. */
static bool get_seed(const yjs_value* v, uint64_t* out) {
    int64_t x = 0;
    char* end = NULL;
    unsigned long long u;
    if (!v || v->type != YJS_NUMBER || !(v->flags & YJS_NUM_INT)) return false;
    if (yjs_int64(v, &x) == YJS_OK) {
        if (x < 0) return false;
        *out = (uint64_t)x;
        return true;
    }
    if (v->u.s[0] == '-') return false;
    u = strtoull(v->u.s, &end, 10);
    if (!end || *end || u == ~0ull) return false;   /* ULLONG_MAX is also the overflow's value */
    *out = (uint64_t)u;
    return true;
}

void dsg_preview_part_read(const dsg_view* v, dsg_preview_settings* s, bool* open) {
    size_t n = 0;
    const char* json = v ? dsg_view_part_get(v, PART_KEY, &n) : NULL;
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value* x;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    memset(&e, 0, sizeof e);
    dsg_preview_settings_default(s);
    *open = false;
    if (!json) return;
    yjs_arena_init_heap(&a, (size_t)1 << 20);
    if (yjs_parse(&a, json, n, NULL, &root, &e) != YJS_OK || !root || root->type != YJS_OBJECT) {
        yjs_arena_free(&a);
        return;
    }
    if (yjs_bool(yjs_get(root, "open"), &b) == YJS_OK) *open = b;
    x = yjs_get(root, "display");
    if (x && x->type == YJS_ARRAY && x->n == 2) {
        int64_t w = 0, h = 0;
        if (get_int(yjs_at(x, 0), 1, PV_MAX_SIDE, &w) && get_int(yjs_at(x, 1), 1, PV_MAX_SIDE, &h)) {
            s->session.width = (int32_t)w;
            s->session.height = (int32_t)h;
        }
    }
    if (get_int(yjs_get(root, "hz"), 1, PV_MAX_HZ, &i)) s->session.hz = (int32_t)i;
    if (yjs_double(yjs_get(root, "speed"), &d) == YJS_OK && speed_ok(d)) s->speed = d;
    (void)get_seed(yjs_get(root, "seed"), &s->session.seed);
    {
        char p[sizeof s->session.participant] = "";
        if (get_str(yjs_get(root, "participant"), p, sizeof p) && p[0]) memcpy(s->session.participant, p, sizeof p);
    }
    if (yjs_double(yjs_get(root, "rt_s"), &d) == YJS_OK && d > 0 && d <= 60) s->session.rt_s = d;
    x = yjs_get(root, "pin");
    if (x && x->type == YJS_OBJECT) {
        char leaf[sizeof s->session.pin_leaf] = "";
        if (get_str(yjs_get(x, "leaf"), leaf, sizeof leaf)) {
            memcpy(s->session.pin_leaf, leaf, sizeof leaf);
            s->session.pin_row = -1;
            if (leaf[0] && get_int(yjs_get(x, "row"), -1, INT32_MAX, &i)) s->session.pin_row = (int32_t)i;
        }
    }
    x = yjs_get(root, "fields");
    if (x && x->type == YJS_OBJECT) {
        uint32_t k;
        for (k = 0; k < x->n && s->session.n_fields < DSG_SESSION_FIELDS; k++) {
            const yjs_member* m = &x->u.members[k];
            dsg_session_field* f = &s->session.fields[s->session.n_fields];
            if (m->key_len == 0 || m->key_len >= sizeof f->name || memchr(m->key, 0, m->key_len)) continue;
            if (!get_str(m->value, f->value, sizeof f->value)) continue;
            memcpy(f->name, m->key, m->key_len);
            f->name[m->key_len] = 0;
            s->session.n_fields++;
        }
    }
    {
        size_t sn = 0;
        const char* sc = yjs_string(yjs_get(root, "scale"), &sn);
        if (sc && sn == 3 && memcmp(sc, "1:1", 3) == 0) s->scale = DSG_PV_SCALE_1TO1;
    }
    yjs_arena_free(&a);
}

int dsg_preview_part_write(dsg_view* v, const dsg_preview_settings* s, bool open) {
    yjs_arena a;
    yjs_value *root, *disp, *pin, *fields;
    char seed[24];
    int i, rc = -1, bad = 0;
    yjs_arena_init_heap(&a, (size_t)1 << 20);
    root = yjs_new(&a, YJS_OBJECT);
    disp = yjs_new(&a, YJS_ARRAY);
    pin = yjs_new(&a, YJS_OBJECT);
    fields = yjs_new(&a, YJS_OBJECT);
    if (!root || !disp || !pin || !fields) {
        yjs_arena_free(&a);
        return -1;
    }
    snprintf(seed, sizeof seed, "%llu", (unsigned long long)s->session.seed);
    bad |= yjs_set(&a, root, "open", yjs_new(&a, open ? YJS_TRUE : YJS_FALSE));
    bad |= yjs_push(&a, disp, yjs_new_int(&a, s->session.width));
    bad |= yjs_push(&a, disp, yjs_new_int(&a, s->session.height));
    bad |= yjs_set(&a, root, "display", disp);
    bad |= yjs_set(&a, root, "hz", yjs_new_int(&a, s->session.hz));
    bad |= yjs_set(&a, root, "speed", yjs_new_double(&a, s->speed));
    bad |= yjs_set(&a, root, "seed", yjs_new_number(&a, seed, strlen(seed)));
    bad |= yjs_set(&a, root, "participant", yjs_new_stringz(&a, s->session.participant));
    bad |= yjs_set(&a, root, "rt_s", yjs_new_double(&a, s->session.rt_s));
    bad |= yjs_set(&a, pin, "leaf", yjs_new_stringz(&a, s->session.pin_leaf));
    bad |= yjs_set(&a, pin, "row", yjs_new_int(&a, s->session.pin_leaf[0] ? s->session.pin_row : -1));
    bad |= yjs_set(&a, root, "pin", pin);
    for (i = 0; i < s->session.n_fields && i < DSG_SESSION_FIELDS; i++)
        bad |= yjs_set(&a, fields, s->session.fields[i].name, yjs_new_stringz(&a, s->session.fields[i].value));
    bad |= yjs_set(&a, root, "fields", fields);
    bad |= yjs_set(&a, root, "scale", yjs_new_stringz(&a, s->scale == DSG_PV_SCALE_1TO1 ? "1:1" : "fit"));
    if (!bad) {
        size_t n = yjs_write_mem(root, YJS_WRITE_COMPACT, NULL, 0);
        char* buf = n ? (char*)yjs_alloc(&a, n + 1) : NULL;
        if (buf && yjs_write_mem(root, YJS_WRITE_COMPACT, buf, n + 1) == n) rc = dsg_view_part_set(v, PART_KEY, buf, n);
    }
    yjs_arena_free(&a);
    return rc;
}
