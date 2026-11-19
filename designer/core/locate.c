/* locate.c - the place in a definition's text that a refusal or a need
 * names. The player names an object (a node's path, a stimulus, ...), not a
 * place, so this file finds the object's key in the text the check read. */
#include "dsg.h"
#include "player.h"

#include <string.h>

uint32_t dsg_offset_of(const char* text, size_t n, uint32_t line, uint32_t column) {
    size_t i = 0;
    uint32_t l = 1;
    if (line == 0) return 0;
    while (l < line && i < n) {
        if (text[i] == '\n') l++;
        i++;
    }
    if (column > 1) {
        size_t c = 1;
        while (c < column && i < n && text[i] != '\n') { i++; c++; }
    }
    return (uint32_t)i;
}

uint32_t dsg_codepoints_before(const char* text, size_t n, uint32_t byte_offset) {
    uint32_t cp = 0;
    size_t i, end = byte_offset < n ? byte_offset : n;
    for (i = 0; i < end; i++)
        if (((unsigned char)text[i] & 0xC0) != 0x80) cp++;
    return cp;
}

uint32_t dsg_byte_of_codepoint(const char* text, size_t n, uint32_t cp) {
    size_t i;
    uint32_t k = 0;
    for (i = 0; i < n; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            if (k == cp) return (uint32_t)i;
            k++;
        }
    }
    return (uint32_t)n;
}

static void place_at(const char* text, size_t n, uint32_t pos, dsg_place* out) {
    out->offset = pos;
    yjs_line_col(text, n, pos, &out->line, &out->column);
}

/* The key of member `name` in object `obj`, else the place of `fallback`. */
static bool member_place(const char* text, size_t n, const yjs_value* obj, const char* name, uint32_t fallback, dsg_place* out) {
    const yjs_member* m = obj && obj->type == YJS_OBJECT && name ? yjs_member_n(obj, name, strlen(name)) : NULL;
    if (m) { place_at(text, n, m->key_pos, out); return true; }
    if (fallback != UINT32_MAX) { place_at(text, n, fallback, out); return true; }
    return false;
}

/* The key's place of a top-level set member: root[set][name]. */
static bool set_member(const char* text, size_t n, const yjs_value* root, const char* set, const char* name, dsg_place* out) {
    const yjs_member* s = yjs_member_n(root, set, strlen(set));
    if (!s) return false;
    return member_place(text, n, s->value, name, s->key_pos, out);
}

/* A node's path "a/b": names from the timeline down. A node with no name
 * has "" in its path, so an empty part matches the first node with none. */
static bool node_place(const char* text, size_t n, const yjs_value* root, const char* path, dsg_place* out) {
    const yjs_member* tm = yjs_member_n(root, "timeline", 8);
    const yjs_value* arr = tm ? tm->value : NULL;
    const yjs_value* found = NULL;
    const char* p = path;
    uint32_t best = tm ? tm->key_pos : UINT32_MAX;
    if (!arr) return false;
    for (;;) {
        const char* slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        uint32_t i;
        found = NULL;
        if (!arr || arr->type != YJS_ARRAY) break;
        for (i = 0; i < arr->n && !found; i++) {
            const yjs_value* nd = arr->u.items[i];
            const yjs_value* nm = nd && nd->type == YJS_OBJECT ? yjs_get(nd, "name") : NULL;
            size_t nl = nm && nm->type == YJS_STRING ? nm->n : 0;
            if (nd && nd->type == YJS_OBJECT && nl == len && (len == 0 || memcmp(nm->u.s, p, len) == 0)) found = nd;
        }
        if (!found) break;
        {
            const yjs_member* nm = yjs_member_n(found, "name", 4);
            best = nm ? nm->key_pos : found->pos;
        }
        if (!slash) break;
        p = slash + 1;
        {
            const yjs_member* kids = yjs_member_n(found, "timeline", 8);
            arr = kids ? kids->value : NULL;
        }
    }
    if (best == UINT32_MAX) return false;
    place_at(text, n, best, out);
    return true;
}

static const yjs_value* parse(yjs_arena* a, const char* text, size_t n) {
    yjs_value* root = NULL;
    yjs_error e;
    yjs_arena_init_heap(a, 0);
    if (yjs_parse(a, text, n, NULL, &root, &e) != YJS_OK || !root || root->type != YJS_OBJECT) return NULL;
    return root;
}

bool dsg_locate_object(const char* text, size_t n, int object, const char* name, dsg_place* out) {
    yjs_arena a;
    const yjs_value* root;
    bool ok = false;
    memset(out, 0, sizeof *out);
    if (!name || object == PL_OBJ_NONE) return false;
    root = parse(&a, text, n);
    if (root) {
        switch (object) {
        case PL_OBJ_NODE: ok = node_place(text, n, root, name, out); break;
        case PL_OBJ_STIMULUS: ok = set_member(text, n, root, "stimuli", name, out); break;
        case PL_OBJ_TEMPLATE: ok = set_member(text, n, root, "trials", name, out); break;
        case PL_OBJ_METHOD: ok = set_member(text, n, root, "methods", name, out); break;
        case PL_OBJ_SIGNAL_ROLE: ok = set_member(text, n, root, "roles", name, out); break;
        case PL_OBJ_FIELD: {
            const yjs_member* s = yjs_member_n(root, "session", 7);
            const yjs_member* f = s ? yjs_member_n(s->value, "fields", 6) : NULL;
            /* a default field (participant) has no key in the text */
            if (f) ok = member_place(text, n, f->value, name, f->key_pos, out);
            else if (s) { place_at(text, n, s->key_pos, out); ok = true; }
            break;
        }
        default: break;
        }
    }
    yjs_arena_free(&a);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

bool dsg_locate_need(const char* text, size_t n, int need, const char* name, dsg_place* out) {
    yjs_arena a;
    const yjs_value* root;
    bool ok = false;
    memset(out, 0, sizeof *out);
    switch (need) {
    case PL_NEED_FIELD: return dsg_locate_object(text, n, PL_OBJ_FIELD, name, out);
    case PL_NEED_SIGNAL:
    case PL_NEED_SIM_GAIN: return dsg_locate_object(text, n, PL_OBJ_SIGNAL_ROLE, name, out);
    default: break;
    }
    root = parse(&a, text, n);
    if (root) {
        const char* key = need == PL_NEED_CALIBRATION ? "calibration" : need == PL_NEED_VIEW ? "units" : "min_tier";
        switch (need) {
        case PL_NEED_DEVICE:
        case PL_NEED_ROLE_TIER: ok = set_member(text, n, root, "devices", name, out); break;
        case PL_NEED_CALIBRATION:
        case PL_NEED_VIEW:
        case PL_NEED_DISPLAY_TIER: ok = set_member(text, n, root, "display", key, out); break;
        default: break;
        }
    }
    yjs_arena_free(&a);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}
