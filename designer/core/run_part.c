/* run_part.c - the Run view's settings and the view file's part "run"
 * (dsg.h, step 6; docs/designer.md, "Part S as built" of step 6). Shared
 * (part S): ui.cpp reads the part at an open and writes it with the view
 * file, so the Run view (part V) and run.c (part R) never touch the file. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"

#include "ysp/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PART_KEY "run"

/* The player's own budget: --max-seconds above it changes nothing. */
#define RUN_MAX_SECONDS_CAP 172800.0
/* A gain's bound, as ysp/rigfile.h's per_count. */
#define RUN_MAX_GAIN 1e6

void dsg_run_settings_default(dsg_run_settings* s) {
    memset(s, 0, sizeof *s);
    s->max_seconds = DSG_RUN_MAX_SECONDS;
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

/* A profile's name, never a path: the player reads `--rig X` as a file when
 * X has a slash or ends in ".json" (player/lower.c, pl_rig_load()). */
static bool rig_ok(const char* s) {
    const size_t n = strlen(s);
    return !strchr(s, '/') && !strchr(s, '\\') && !(n > 5 && strcmp(s + n - 5, ".json") == 0);
}

/* --seed N: digits only, or "". */
static bool seed_ok(const char* s) {
    size_t i;
    for (i = 0; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return i <= 20;
}

/* A role's name for --allow-below-tier R,R and --sim-gain ROLE=UNITS. */
static bool role_ok(const char* s) { return s[0] && !strchr(s, ',') && !strchr(s, '='); }

void dsg_run_part_read(const dsg_view* v, dsg_run_settings* s, bool* open) {
    size_t n = 0;
    const char* json = v ? dsg_view_part_get(v, PART_KEY, &n) : NULL;
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value* x;
    double d = 0;
    bool b = false;
    uint32_t k;
    memset(&e, 0, sizeof e);
    dsg_run_settings_default(s);
    *open = false;
    if (!json) return;
    yjs_arena_init_heap(&a, (size_t)1 << 20);
    if (yjs_parse(&a, json, n, NULL, &root, &e) != YJS_OK || !root || root->type != YJS_OBJECT) {
        yjs_arena_free(&a);
        return;
    }
    if (yjs_bool(yjs_get(root, "open"), &b) == YJS_OK) *open = b;
    {
        char rig[sizeof s->rig] = "";
        if (get_str(yjs_get(root, "rig"), rig, sizeof rig) && rig_ok(rig)) memcpy(s->rig, rig, sizeof rig);
    }
    if (yjs_bool(yjs_get(root, "sim"), &b) == YJS_OK) s->sim = b;
    {
        char p[sizeof s->participant] = "";
        if (get_str(yjs_get(root, "participant"), p, sizeof p)) memcpy(s->participant, p, sizeof p);
    }
    x = yjs_get(root, "fields");
    if (x && x->type == YJS_OBJECT) {
        for (k = 0; k < x->n && s->n_fields < DSG_SESSION_FIELDS; k++) {
            const yjs_member* m = &x->u.members[k];
            dsg_session_field* f = &s->fields[s->n_fields];
            if (m->key_len == 0 || m->key_len >= sizeof f->name || memchr(m->key, 0, m->key_len) || memchr(m->key, '=', m->key_len))
                continue;
            {
                char val[sizeof f->value];
                if (!get_str(m->value, val, sizeof val)) continue;
                memcpy(f->value, val, sizeof val);
            }
            memcpy(f->name, m->key, m->key_len);
            f->name[m->key_len] = 0;
            s->n_fields++;
        }
    }
    {
        char seed[sizeof s->seed] = "";
        if (get_str(yjs_get(root, "seed"), seed, sizeof seed) && seed_ok(seed)) memcpy(s->seed, seed, sizeof seed);
    }
    x = yjs_get(root, "allow_below_tier");
    if (x && x->type == YJS_ARRAY) {
        for (k = 0; k < x->n && s->n_below < DSG_RUN_ROLES; k++) {
            char r[sizeof s->allow_below_tier[0]] = "";
            if (get_str(yjs_at(x, k), r, sizeof r) && role_ok(r)) memcpy(s->allow_below_tier[s->n_below++], r, sizeof r);
        }
    }
    if (yjs_bool(yjs_get(root, "windowed"), &b) == YJS_OK) s->windowed = b;
    if (yjs_double(yjs_get(root, "max_seconds"), &d) == YJS_OK && d > 0 && d <= RUN_MAX_SECONDS_CAP) s->max_seconds = d;
    x = yjs_get(root, "gains");
    if (x && x->type == YJS_OBJECT) {
        for (k = 0; k < x->n && s->n_gains < DSG_RUN_ROLES; k++) {
            const yjs_member* m = &x->u.members[k];
            dsg_run_gain* g = &s->gains[s->n_gains];
            char role[sizeof g->role];
            if (m->key_len == 0 || m->key_len >= sizeof role || memchr(m->key, 0, m->key_len)) continue;
            if (yjs_double(m->value, &d) != YJS_OK || !isfinite(d) || d == 0 || fabs(d) > RUN_MAX_GAIN) continue;
            memcpy(role, m->key, m->key_len);
            role[m->key_len] = 0;
            if (!role_ok(role)) continue;
            memcpy(g->role, role, sizeof role);
            g->units = d;
            s->n_gains++;
        }
    }
    yjs_arena_free(&a);
}

int dsg_run_part_write(dsg_view* v, const dsg_run_settings* s, bool open) {
    yjs_arena a;
    yjs_value *root, *fields, *below, *gains;
    int i, rc = -1, bad = 0;
    yjs_arena_init_heap(&a, (size_t)1 << 20);
    root = yjs_new(&a, YJS_OBJECT);
    fields = yjs_new(&a, YJS_OBJECT);
    below = yjs_new(&a, YJS_ARRAY);
    gains = yjs_new(&a, YJS_OBJECT);
    if (!root || !fields || !below || !gains) {
        yjs_arena_free(&a);
        return -1;
    }
    bad |= yjs_set(&a, root, "open", yjs_new(&a, open ? YJS_TRUE : YJS_FALSE));
    bad |= yjs_set(&a, root, "rig", yjs_new_stringz(&a, s->rig));
    bad |= yjs_set(&a, root, "sim", yjs_new(&a, s->sim ? YJS_TRUE : YJS_FALSE));
    bad |= yjs_set(&a, root, "participant", yjs_new_stringz(&a, s->participant));
    for (i = 0; i < s->n_fields && i < DSG_SESSION_FIELDS; i++)
        bad |= yjs_set(&a, fields, s->fields[i].name, yjs_new_stringz(&a, s->fields[i].value));
    bad |= yjs_set(&a, root, "fields", fields);
    bad |= yjs_set(&a, root, "seed", yjs_new_stringz(&a, s->seed));
    for (i = 0; i < s->n_below && i < DSG_RUN_ROLES; i++) bad |= yjs_push(&a, below, yjs_new_stringz(&a, s->allow_below_tier[i]));
    bad |= yjs_set(&a, root, "allow_below_tier", below);
    bad |= yjs_set(&a, root, "windowed", yjs_new(&a, s->windowed ? YJS_TRUE : YJS_FALSE));
    bad |= yjs_set(&a, root, "max_seconds", yjs_new_double(&a, s->max_seconds));
    for (i = 0; i < s->n_gains && i < DSG_RUN_ROLES; i++) bad |= yjs_set(&a, gains, s->gains[i].role, yjs_new_double(&a, s->gains[i].units));
    bad |= yjs_set(&a, root, "gains", gains);
    if (!bad) {
        size_t n = yjs_write_mem(root, YJS_WRITE_COMPACT, NULL, 0);
        char* buf = n ? (char*)yjs_alloc(&a, n + 1) : NULL;
        if (buf && yjs_write_mem(root, YJS_WRITE_COMPACT, buf, n + 1) == n) rc = dsg_view_part_set(v, PART_KEY, buf, n);
    }
    yjs_arena_free(&a);
    return rc;
}
