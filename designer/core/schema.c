/* schema.c - the panels' fields from the format's table and the headers'
 * parameter tables, the object at a path, typed writes and renames
 * (schema.h; docs/designer.md, "The panels"). OWNED BY PART C. */
#include "schema.h"
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the fields ---------------------------------------------------------------- */

static dsg_field* g_fields;
static int g_n;

/* A header's row joined to a format key. */
typedef struct join {
    const char* unit;
    const char* doc;
    double      min, max;
    const char* from;
    int         id;        /* equal joins have equal ids; 0: none */
} join;

static int sec(const char* name) { return dsg_schema_section(name); }

static void join_none(join* j) {
    j->unit = "";
    j->doc = "";
    j->min = j->max = NAN;
    j->from = "format";
    j->id = 0;
}

/* Generic over the tables' shared row shape (G6): name, type, min, max, def,
 * unit, doc. scale converts the range to the key's unit (ns to s). */
#define FIND_ROW(T, rows, n, name_, j, from_, id0, scale)                         \
    do {                                                                          \
        int i_;                                                                   \
        for (i_ = 0; rows && i_ < (n); i_++)                                      \
            if (strcmp((rows)[i_].name, (name_)) == 0) {                          \
                (j)->unit = (rows)[i_].unit;                                      \
                (j)->doc = (rows)[i_].doc ? (rows)[i_].doc : "";                  \
                (j)->min = (rows)[i_].min * (scale);                              \
                (j)->max = (rows)[i_].max * (scale);                              \
                (j)->from = (from_);                                              \
                (j)->id = (id0) + i_ + 1;                                         \
                break;                                                            \
            }                                                                     \
    } while (0)

/* Ids keep the joins of different tables apart. */
enum { ID_RDK = 1000, ID_GFX = 2000, ID_GFXE = 3000, ID_RSP = 4000, ID_ST = 5000, ID_QST = 6000, ID_AEP = 7000, ID_SIG = 10000 };

/* The gfx kind that draws a format kind, as a bit of ygfx_param.kinds; 0
 * for a sound. A key of one name can mean another thing in another kind (a
 * sound's offset is a frame, a shape's a distance), so a gfx row joins only
 * the kinds it reads. */
static uint32_t gfx_kind_bit(unsigned kind_bit) {
    static const struct { int yexp; int ygfx; } map[] = {
        { YEXP_SHAPE, YGFX_SHAPE }, { YEXP_GRATING, YGFX_GRATING }, { YEXP_GABOR, YGFX_GABOR }, { YEXP_NOISE, YGFX_NOISE },
        { YEXP_IMAGE, YGFX_IMAGE }, { YEXP_VIDEO, YGFX_IMAGE }, { YEXP_TEXT, YGFX_TEXT }, { YEXP_ARTWORK, YGFX_TEXT },
        { YEXP_RDK, YGFX_DOTS },
    };
    uint32_t out = 0;
    int k;
    for (k = 0; k < (int)(sizeof map / sizeof map[0]); k++)
        if (kind_bit & (1u << map[k].yexp)) out |= 1u << map[k].ygfx;
    return out;
}

static void join_gfx_table(const ygfx_param* rows, int n, const char* key, uint32_t gk, join* j, int id0) {
    int i;
    for (i = 0; rows && i < n; i++)
        if ((rows[i].kinds & gk) && strcmp(rows[i].name, key) == 0) {
            j->unit = rows[i].unit;
            j->doc = rows[i].doc ? rows[i].doc : "";
            j->min = rows[i].min;
            j->max = rows[i].max;
            j->from = "ysp/gfx.h";
            j->id = id0 + i + 1;
            return;
        }
}

static void join_gfx(const char* key, unsigned kind_bit, join* j) {
    int n = 0;
    const uint32_t gk = gfx_kind_bit(kind_bit);
    const ygfx_param* r = ygfx_params(&n);
    join_gfx_table(r, n, key, gk, j, ID_GFX);
    if (j->id) return;
    r = ygfx_enum_params(&n);
    join_gfx_table(r, n, key, gk, j, ID_GFXE);
}

/* A stimulus key for one kind: rdk's own fields first, then gfx's, as the
 * player's param host checks them (player/host.c). "color" is gfx's three
 * fields color_r, color_g and color_b. */
static void join_stimulus(const char* key, unsigned kind_bit, join* j) {
    if (kind_bit & (1u << YEXP_RDK)) {
        int n = 0;
        const yrdk_param* r = yrdk_params(&n);
        FIND_ROW(yrdk_param, r, n, key, j, "ysp/rdk.h", ID_RDK, 1.0);
        if (j->id) return;
    }
    join_gfx(strcmp(key, "color") == 0 ? "color_r" : key, kind_bit, j);
}

/* A response key: yrsp_desc's field. persist is response_ends_trial's
 * negation; the doc says so through `from`. */
static void join_response(const char* key, join* j) {
    static const struct { const char* key; const char* field; double scale; } map[] = {
        { "duration_s", "duration_ns", 1e-9 }, { "minimum_valid_rt_s", "min_rt_ns", 1e-9 },
        { "max_responses", "max_responses", 1.0 }, { "allow_held_key", "allow_held_key", 1.0 },
        { "wait_for_key_release", "wait_for_key_release", 1.0 }, { "response_ends_trial", "persist", 1.0 },
        { "key", "choices.key", 1.0 }, { "name", "choices.name", 1.0 },
    };
    int n = 0, k;
    const yrsp_param* r = yrsp_params(&n);
    for (k = 0; k < (int)(sizeof map / sizeof map[0]); k++)
        if (strcmp(map[k].key, key) == 0) {
            FIND_ROW(yrsp_param, r, n, map[k].field, j,
                     strcmp(key, "response_ends_trial") == 0 ? "ysp/response.h (persist, its negation)" : "ysp/response.h", ID_RSP,
                     map[k].scale);
            if (j->id && map[k].scale != 1.0) j->unit = "s";
            return;
        }
}

/* A control key for one kind: the op row it lowers to (format 3.7). */
static void join_control(const char* key, unsigned kind_bit, join* j) {
    static const struct { const char* key; unsigned kinds; int op; const char* row; } map[] = {
        { "radius", 1u << YEXP_REGION, YSIG_OP_DETECT, "level" },
        { "stay_s", 1u << YEXP_REGION, YSIG_OP_DETECT, "hold" },
        { "rate", 1u << YEXP_REGION, YSIG_OP_DETECT, "at" },
        { "outside", 1u << YEXP_REGION, YSIG_OP_DETECT, "test" },
        { "speed_below", 1u << YEXP_REGION, YSIG_OP_DETECT, "level" },
        { "speed_lag_s", 1u << YEXP_REGION, YSIG_OP_SPEED, "lag" },
        { "scale", 1u << YEXP_DISTANCE, YSIG_OP_SCALE, "k.x" },
        { "lag_s", 1u << YEXP_PEAK, YSIG_OP_SPEED, "lag" },
        { "ramp_s", 1u << YEXP_MOVING_TARGET, YSIG_OP_SINES, "ramp" },
        { "rate", 1u << YEXP_TRACE, YSIG_OP_RECORD, "per" },
        { "after_s", 1u << YEXP_SIGNAL_LOST, YSIG_OP_SILENT, "after" },
        { "transform", 1u << YEXP_CURSOR, YSIG_OP_AFFINE, "angle" },
    };
    int k;
    for (k = 0; k < (int)(sizeof map / sizeof map[0]); k++)
        if ((map[k].kinds & kind_bit) && strcmp(map[k].key, key) == 0) {
            int n = 0;
            const ysig_param* r = ysig_op_params(map[k].op, &n);
            FIND_ROW(ysig_param, r, n, map[k].row, j, "ysp/sig.h", ID_SIG + 100 * map[k].op, 1.0);
            return;
        }
    if ((kind_bit & (1u << YEXP_MOVEMENT)) && strcmp(key, "onset") == 0) {
        int n = 0;
        const yrsp_param* r = yrsp_params(&n);
        FIND_ROW(yrsp_param, r, n, "choices.level", j, "ysp/response.h", ID_RSP, 1.0);
    }
}

static void join_method(int section, const char* key, join* j) {
    int n = 0;
    if (section == YEXP_S_STAIR) {
        const yst_param* r = yst_params(&n);
        FIND_ROW(yst_param, r, n, key, j, "ysp/stair.h", ID_ST, 1.0);
    } else if (section == YEXP_S_QUEST) {
        const yqst_param* r = yqst_params(&n);
        FIND_ROW(yqst_param, r, n, key, j, "ysp/quest.h", ID_QST, 1.0);
    } else if (section == YEXP_S_AEP) {
        const yaep_param* r = yaep_params(&n);
        FIND_ROW(yaep_param, r, n, key, j, "ysp/aep.h", ID_AEP, 1.0);
    }
}

/* The join of one row for one kind (kind_bit 0xffff: a section without kinds). */
static void join_row(const yexp_row* r, unsigned kind_bit, join* j) {
    const int s = r->section;
    join_none(j);
    if (s == YEXP_S_STIM) join_stimulus(r->key, kind_bit, j);
    else if (s == YEXP_S_RESPONSE || s == YEXP_S_SRESPONSE || s == YEXP_S_CHOICE) join_response(r->key, j);
    else if (s == YEXP_S_CONTROL) join_control(r->key, kind_bit, j);
    else join_method(s, r->key, j);
    /* The format's text names the key in the format's terms (a region's
     * radius, not detect's level); its doc is empty only where a header's
     * table is the text (ysp/experiment.h 0.6.0). A key no header has takes
     * the format's unit too. */
    if (r->doc && r->doc[0]) j->doc = r->doc;
    if (!j->id && r->unit) j->unit = r->unit;
    /* a duration's unit is seconds whatever the header's (D4); an enum or a
     * bool has no range worth showing */
    if (r->type == YEXP_T_DUR) j->unit = "s";
    if (r->type != YEXP_T_NUM && r->type != YEXP_T_INT && r->type != YEXP_T_DUR) j->min = j->max = NAN;
    if (!j->unit) j->unit = "";
}

/* What a key names (format 3.9), by section and key. Only the name types
 * take a mask: a control's x is a name for a cursor, sines for a moving
 * target. */
static unsigned refs_of(int section, const char* key, int type) {
    static const struct { const char* section; const char* key; unsigned refs; } map[] = {
        { "node", "trial", DSG_REF_TEMPLATE }, { "node", "tracks", DSG_REF_METHOD },
        { "node", "if", DSG_REF_VAR }, { "node", "unless", DSG_REF_VAR },
        { "response", "role", DSG_REF_EVROLE }, { "response", "from", DSG_REF_STIM },
        { "response trigger", "role", DSG_REF_EVROLE },
        { "response with signal choices", "from", DSG_REF_STIM },
        { "step", "on", DSG_REF_STIM }, { "step", "off", DSG_REF_STIM }, { "step", "set", DSG_REF_STIM },
        { "step", "to", DSG_REF_STIM }, { "step", "then", DSG_REF_STIM }, { "step", "role", DSG_REF_EVROLE },
        { "step", "flag", DSG_REF_FLAG }, { "step", "until", DSG_REF_CTL_HITS }, { "step", "recenter", DSG_REF_SIGROLE },
        { "step", "if", DSG_REF_VAR | DSG_REF_FLAG }, { "step", "unless", DSG_REF_VAR | DSG_REF_FLAG },
        { "control", "of", DSG_REF_SIGROLE | DSG_REF_CTL_VALUE | DSG_REF_LANES },
        { "control", "role", DSG_REF_SIGROLE },
        { "control", "x", DSG_REF_SIGROLE | DSG_REF_LANES }, { "control", "y", DSG_REF_SIGROLE | DSG_REF_LANES },
        { "control", "mean", DSG_REF_SIGROLE }, { "control", "stimulus", DSG_REF_STIM },
        { "control", "freeze_at", DSG_REF_CTL_HITS },
        { "control", "from", DSG_REF_STIM | DSG_REF_CTL_HITS | DSG_REF_STATE | DSG_REF_RESPONSE },
        { "control", "to", DSG_REF_STIM | DSG_REF_CTL_HITS | DSG_REF_STATE | DSG_REF_RESPONSE | DSG_REF_END },
        { "exit", "on", DSG_REF_CTL_HITS | DSG_REF_RESPONSE }, { "exit", "to", DSG_REF_STATE | DSG_REF_END },
        { "exit", "in", DSG_REF_STATE }, { "exit", "if", DSG_REF_VAR | DSG_REF_FLAG },
        { "over", "to", DSG_REF_STATE | DSG_REF_END },
        { "template", "record", DSG_REF_STIM },
    };
    int k;
    if (type != YEXP_T_NAME && type != YEXP_T_NAMEZ && type != YEXP_T_NAMES && type != YEXP_T_REF && type != YEXP_T_REFS &&
        type != YEXP_T_TARGETS && type != YEXP_T_VAR)
        return 0;
    for (k = 0; k < (int)(sizeof map / sizeof map[0]); k++)
        if (section == sec(map[k].section) && strcmp(map[k].key, key) == 0) return map[k].refs;
    return 0;
}

static void fill(dsg_field* f, const yexp_row* r, uint16_t kinds, const join* j) {
    f->key = r->key;
    f->section = r->section;
    f->kinds = kinds;
    f->type = r->type;
    f->flags = r->flags;
    f->required = yexp_row_required(r);
    f->def = r->def;
    f->values = r->values;
    f->unit = j->unit;
    f->doc = j->doc;
    f->min = j->min;
    f->max = j->max;
    f->from = j->from;
    f->refs = refs_of(r->section, r->key, r->type);
}

static int build(void) {
    int n = 0, i, k = 0, cap;
    const yexp_row* rows = yexp_defaults(2, &n);
    if (!rows) return -1;
    /* at most one field per kind bit of a row */
    cap = n * 4 + 64;
    g_fields = (dsg_field*)calloc((size_t)cap, sizeof *g_fields);
    if (!g_fields) return -1;
    for (i = 0; i < n; i++) {
        const yexp_row* r = &rows[i];
        if (r->kinds == 0xffff) {
            join j;
            join_row(r, 0xffffu, &j);
            fill(&g_fields[k++], r, 0xffff, &j);
            continue;
        }
        /* One field per group of kinds with the same join. */
        {
            uint16_t done = 0;
            int b;
            for (b = 0; b < 16; b++) {
                join j, j2;
                uint16_t group = 0;
                int c;
                if (!(r->kinds & (1u << b)) || (done & (1u << b))) continue;
                join_row(r, 1u << b, &j);
                for (c = b; c < 16; c++) {
                    if (!(r->kinds & (1u << c)) || (done & (1u << c))) continue;
                    join_row(r, 1u << c, &j2);
                    if (j2.id == j.id) group = (uint16_t)(group | (1u << c));
                }
                done = (uint16_t)(done | group);
                if (k >= cap) return -1;
                fill(&g_fields[k++], r, group, &j);
            }
        }
    }
    g_n = k;
    return 0;
}

const dsg_field* dsg_schema_fields(int* n) {
    if (!g_fields && build()) {
        free(g_fields);
        g_fields = NULL;
        if (n) *n = 0;
        return NULL;
    }
    if (n) *n = g_n;
    return g_fields;
}

int dsg_schema_section(const char* name) {
    int i;
    for (i = 0; i < 256; i++) {
        const char* s = yexp_section_name(i);
        if (!s[0]) return -1;
        if (strcmp(s, name) == 0) return i;
    }
    return -1;
}

int dsg_schema_select(int section, unsigned kind_bit, const dsg_field** out, int cap) {
    int n, i, k = 0;
    const dsg_field* f = dsg_schema_fields(&n);
    for (i = 0; f && i < n; i++)
        if (f[i].section == section && (f[i].kinds & kind_bit) && !(f[i].flags & YEXP_R_STEP)) {
            if (k < cap) out[k] = &f[i];
            k++;
        }
    return k;
}

const dsg_field* dsg_schema_field(int section, unsigned kind_bit, const char* key) {
    int n, i;
    const dsg_field* f = dsg_schema_fields(&n);
    for (i = 0; f && i < n; i++)
        if (f[i].section == section && (f[i].kinds & kind_bit) && !(f[i].flags & YEXP_R_STEP) && strcmp(f[i].key, key) == 0)
            return &f[i];
    return NULL;
}

/* --- the object at a path ------------------------------------------------------- */

/* "name=value,..." to the value of name; -1 when absent. */
static int enum_value(const char* values, const char* name) {
    const char* p = values;
    size_t ln = strlen(name);
    while (p && *p) {
        const char* eq = strchr(p, '=');
        const char* comma;
        if (!eq) return -1;
        comma = strchr(eq, ',');
        if ((size_t)(eq - p) == ln && memcmp(p, name, ln) == 0) return atoi(eq + 1);
        p = comma ? comma + 1 : NULL;
    }
    return -1;
}

static const char* str_of(const yjs_value* v, const char* key) {
    const yjs_value* s = v && v->type == YJS_OBJECT ? yjs_get(v, key) : NULL;
    return s && s->type == YJS_STRING ? s->u.s : NULL;
}

/* A kind's bit from the section's discriminating enum row ("kind", "control"). */
static unsigned kind_bit_of(int section, const char* disc, const char* kind) {
    const dsg_field* f = dsg_schema_field(section, 0xffffu, disc);
    int v = f && kind ? enum_value(f->values, kind) : -1;
    return v >= 0 && v < 16 ? 1u << v : 0u;
}

/* A step's op from the key that names it (format 2.5, 3.12): the step's
 * bit is 1 << yexp_step_op. recenter has a `to` too, so it goes first. */
static int step_op(const yjs_value* v, const char** name) {
    static const struct { const char* key; int op; } ops[] = {
        { "recenter", YEXP_RECENTER }, { "flag", YEXP_FLAG }, { "until", YEXP_UNTIL }, { "if", YEXP_IF },
        { "unless", YEXP_UNLESS }, { "on", YEXP_ON }, { "off", YEXP_OFF }, { "wait_s", YEXP_WAIT },
        { "at_s", YEXP_AT }, { "set", YEXP_SET }, { "to", YEXP_TO }, { "then", YEXP_THEN },
        { "trigger", YEXP_TRIGGER }, { "mark", YEXP_MARK },
    };
    int k;
    if (!v || v->type != YJS_OBJECT) return -1;
    for (k = 0; k < (int)(sizeof ops / sizeof ops[0]); k++)
        if (yjs_get(v, ops[k].key)) {
            *name = ops[k].key;
            return ops[k].op;
        }
    return -1;
}

static bool has_signal_choices(const yjs_value* rsp) {
    const yjs_value* ch = rsp && rsp->type == YJS_OBJECT ? yjs_get(rsp, "choices") : NULL;
    uint32_t i;
    if (!ch || ch->type != YJS_ARRAY) return false;
    for (i = 0; i < ch->n; i++)
        if (ch->u.items[i]->type == YJS_OBJECT && yjs_get(ch->u.items[i], "signal")) return true;
    return false;
}

static void copy(char* out, size_t cap, const char* s) { snprintf(out, cap, "%s", s ? s : ""); }

/* Section and kind of an object from its value. */
static void finish(dsg_obj* o, const yjs_value* v) {
    const char* k = NULL;
    o->section = -1;
    o->kind_bit = 0xffffu;
    o->kind[0] = 0;
    copy(o->keys, sizeof o->keys, o->path);
    switch (o->obj) {
    case DSG_OBJ_FILE: o->section = YEXP_S_ROOT; break;
    case DSG_OBJ_DISPLAY: o->section = YEXP_S_DISPLAY; break;
    case DSG_OBJ_SESSION: o->section = YEXP_S_SESSION; break;
    case DSG_OBJ_SFIELD: o->section = YEXP_S_SFIELD; break;
    case DSG_OBJ_WEB: o->section = YEXP_S_WEB; break;
    case DSG_OBJ_EVROLE: o->section = YEXP_S_ROLE; break;
    case DSG_OBJ_SIGROLE: o->section = YEXP_S_SIGROLE; break;
    case DSG_OBJ_STIMULUS:
        o->section = YEXP_S_STIM;
        k = str_of(v, "kind");
        o->kind_bit = kind_bit_of(o->section, "kind", k);
        break;
    case DSG_OBJ_METHOD:
        k = str_of(v, "type");
        o->section = k ? (strcmp(k, "stair") == 0 ? YEXP_S_STAIR : strcmp(k, "quest") == 0 ? YEXP_S_QUEST
                                                                          : strcmp(k, "aep") == 0 ? YEXP_S_AEP : -1)
                       : -1;
        if (o->section < 0) {
            /* no known type: only the type key shows */
            o->section = YEXP_S_STAIR;
            o->kind_bit = 0;
        }
        break;
    case DSG_OBJ_CONTROL:
        o->section = YEXP_S_CONTROL;
        k = str_of(v, "control");
        o->kind_bit = kind_bit_of(o->section, "control", k);
        break;
    case DSG_OBJ_TEMPLATE: {
        const yjs_value* rsp = v && v->type == YJS_OBJECT ? yjs_get(v, "response") : NULL;
        const bool states = v && v->type == YJS_OBJECT && yjs_get(v, "states");
        const bool key_answer = rsp && rsp->type == YJS_OBJECT && yjs_get(rsp, "key_answer");
        o->section = YEXP_S_TEMPLATE;
        /* the template rows' kinds: 2 with states and no key_answer (3.11, D36) */
        o->kind_bit = states && !key_answer ? 2u : 1u;
        break;
    }
    case DSG_OBJ_RESPONSE:
        o->section = has_signal_choices(v) ? YEXP_S_SRESPONSE : YEXP_S_RESPONSE;
        break;
    case DSG_OBJ_RTRIGGER: o->section = YEXP_S_RTRIGGER; break;
    case DSG_OBJ_CHOICE:
        /* a signal choice is {"signal": MOVEMENT} and nothing else (3.13) */
        o->section = v && v->type == YJS_OBJECT && yjs_get(v, "signal") ? -1 : YEXP_S_CHOICE;
        break;
    case DSG_OBJ_STATE: o->section = YEXP_S_STATE; break;
    case DSG_OBJ_OVER: o->section = YEXP_S_OVER; break;
    case DSG_OBJ_EXIT: o->section = YEXP_S_EXIT; o->kind_bit = 1u; break;
    case DSG_OBJ_SHARED_EXIT: o->section = YEXP_S_EXIT; o->kind_bit = 2u; break;
    case DSG_OBJ_STEP: {
        const int op = step_op(v, &k);
        o->section = YEXP_S_STEP;
        o->kind_bit = op >= 0 ? 1u << op : 0u;
        break;
    }
    case DSG_OBJ_NODE: o->section = YEXP_S_NODE; break;
    case DSG_OBJ_LOOP: o->section = YEXP_S_LOOP; break;
    case DSG_OBJ_VAR:
        if (v && v->type == YJS_OBJECT && yjs_get(v, "uniform")) {
            o->section = YEXP_S_UNIFORM;
            k = "uniform";
        } else if (v && v->type == YJS_OBJECT && yjs_get(v, "walk2d")) {
            o->section = YEXP_S_WALK2D;
            k = "walk2d";
            dsg_path_key(o->keys, sizeof o->keys, o->path, "walk2d");
        } else if (v && v->type == YJS_OBJECT && yjs_get(v, "choice")) {
            k = "choice";
        }
        break;
    default: break;
    }
    copy(o->kind, sizeof o->kind, k);
    if (o->obj == DSG_OBJ_STATE || o->obj == DSG_OBJ_NODE) copy(o->name, sizeof o->name, str_of(v, "name"));
}

/* How objects hold objects: a key's value is the child (each false) or a set
 * or an array of children (each true). */
static const struct { int parent; const char* key; bool each; int child; } k_edges[] = {
    { DSG_OBJ_FILE, "display", false, DSG_OBJ_DISPLAY }, { DSG_OBJ_FILE, "session", false, DSG_OBJ_SESSION },
    { DSG_OBJ_FILE, "web", false, DSG_OBJ_WEB }, { DSG_OBJ_FILE, "devices", true, DSG_OBJ_EVROLE },
    { DSG_OBJ_FILE, "roles", true, DSG_OBJ_SIGROLE }, { DSG_OBJ_FILE, "stimuli", true, DSG_OBJ_STIMULUS },
    { DSG_OBJ_FILE, "methods", true, DSG_OBJ_METHOD }, { DSG_OBJ_FILE, "controls", true, DSG_OBJ_CONTROL },
    { DSG_OBJ_FILE, "trials", true, DSG_OBJ_TEMPLATE }, { DSG_OBJ_FILE, "timeline", true, DSG_OBJ_NODE },
    { DSG_OBJ_SESSION, "fields", true, DSG_OBJ_SFIELD },
    { DSG_OBJ_TEMPLATE, "response", false, DSG_OBJ_RESPONSE }, { DSG_OBJ_TEMPLATE, "controls", true, DSG_OBJ_CONTROL },
    { DSG_OBJ_TEMPLATE, "states", true, DSG_OBJ_STATE }, { DSG_OBJ_TEMPLATE, "exits", true, DSG_OBJ_SHARED_EXIT },
    { DSG_OBJ_TEMPLATE, "events", true, DSG_OBJ_STEP }, { DSG_OBJ_TEMPLATE, "after", true, DSG_OBJ_STEP },
    { DSG_OBJ_TEMPLATE, "columns", true, DSG_OBJ_COLUMN },
    { DSG_OBJ_RESPONSE, "trigger", false, DSG_OBJ_RTRIGGER }, { DSG_OBJ_RESPONSE, "choices", true, DSG_OBJ_CHOICE },
    { DSG_OBJ_STATE, "over", false, DSG_OBJ_OVER }, { DSG_OBJ_STATE, "exits", true, DSG_OBJ_EXIT },
    { DSG_OBJ_STATE, "steps", true, DSG_OBJ_STEP },
    { DSG_OBJ_STEP, "steps", true, DSG_OBJ_STEP },
    { DSG_OBJ_NODE, "timeline", true, DSG_OBJ_NODE }, { DSG_OBJ_NODE, "loop", false, DSG_OBJ_LOOP },
    { DSG_OBJ_NODE, "vars", true, DSG_OBJ_VAR }, { DSG_OBJ_NODE, "data", true, DSG_OBJ_DATA },
};
#define N_EDGES ((int)(sizeof k_edges / sizeof k_edges[0]))

/* The next token of a JSON Pointer, unescaped; NULL at the end. */
static const char* next_tok(const char* p, char* tok, size_t cap) {
    size_t k = 0;
    if (!p || *p != '/') return NULL;
    p++;
    while (*p && *p != '/') {
        char c = *p++;
        if (c == '~' && (*p == '0' || *p == '1')) c = *p++ == '0' ? '~' : '/';
        if (k + 1 < cap) tok[k++] = c;
    }
    tok[k] = 0;
    return p;
}

static const yjs_value* child_of(const yjs_value* v, const char* tok) {
    if (!v) return NULL;
    if (v->type == YJS_OBJECT) return yjs_get(v, tok);
    if (v->type == YJS_ARRAY) {
        char* end = NULL;
        long i = strtol(tok, &end, 10);
        if (!tok[0] || *end || i < 0 || (uint32_t)i >= v->n) return NULL;
        return v->u.items[i];
    }
    return NULL;
}

int dsg_schema_chain(const yjs_value* root, const char* path, dsg_obj* out, int cap) {
    dsg_obj all[32];
    int depth = 0, n = 0, i;
    const yjs_value* v = root;
    const char* p = path;
    char tok[DSG_NAME_MAX], tok2[DSG_NAME_MAX];
    char tmpl[DSG_NAME_MAX] = "";
    if (!root) return 0;
    memset(&all[0], 0, sizeof all[0]);
    all[0].obj = DSG_OBJ_FILE;
    finish(&all[0], root);
    depth = 1;
    while (p && *p && depth < (int)(sizeof all / sizeof all[0])) {
        dsg_obj* cur = &all[depth - 1];
        dsg_obj* nx = &all[depth];
        const char* q = next_tok(p, tok, sizeof tok);
        const char* q2 = NULL;
        const yjs_value* cv;
        int e, found = -1;
        if (!q) break;
        for (e = 0; e < N_EDGES; e++)
            if (k_edges[e].parent == cur->obj && strcmp(k_edges[e].key, tok) == 0) { found = e; break; }
        cv = child_of(v, tok);
        /* a key of the object, present or not, is its focus; a walk2d
         * variable's keys are one deeper */
        copy(cur->focus, sizeof cur->focus, tok);
        if (found < 0 && cur->obj == DSG_OBJ_VAR && strcmp(tok, "walk2d") == 0 && next_tok(q, tok2, sizeof tok2))
            copy(cur->focus, sizeof cur->focus, tok2);
        if (found < 0 || !cv) break;
        if (k_edges[found].each) {
            q2 = next_tok(q, tok2, sizeof tok2);
            if (!q2 || !child_of(cv, tok2)) break;   /* the set itself: the parent's key */
        }
        cur->focus[0] = 0;
        memset(nx, 0, sizeof *nx);
        nx->obj = k_edges[found].child;
        if (q2) {
            if (snprintf(nx->path, sizeof nx->path, "%.*s", (int)(q2 - path), path) >= (int)sizeof nx->path) break;
            if (cv->type == YJS_OBJECT) {
                copy(nx->name, sizeof nx->name, tok2);
                snprintf(nx->set, sizeof nx->set, "%.*s", (int)(q - path), path);
            }
            if (nx->obj == DSG_OBJ_TEMPLATE) copy(tmpl, sizeof tmpl, tok2);
            v = child_of(cv, tok2);
            p = q2;
        } else {
            if (snprintf(nx->path, sizeof nx->path, "%.*s", (int)(q - path), path) >= (int)sizeof nx->path) break;
            v = cv;
            p = q;
        }
        copy(nx->tmpl, sizeof nx->tmpl, tmpl);
        finish(nx, v);
        depth++;
    }
    /* innermost first, up to its template; the file only when it is the
     * object itself */
    for (i = depth - 1; i >= 0 && n < cap; i--) {
        if (all[i].obj == DSG_OBJ_FILE && i != depth - 1) break;
        out[n++] = all[i];
        if (all[i].obj == DSG_OBJ_TEMPLATE || all[i].obj == DSG_OBJ_NODE || !all[i].tmpl[0]) break;
        /* a step in an if step: the state or the template above them */
        while (all[i].obj == DSG_OBJ_STEP && i > 0 && all[i - 1].obj == DSG_OBJ_STEP) i--;
    }
    return n;
}

int dsg_schema_object(const yjs_value* root, const char* path, dsg_obj* out) {
    dsg_obj c[1];
    if (!root || !dsg_path_get(root, "") || dsg_schema_chain(root, path, c, 1) != 1) return -1;
    *out = c[0];
    return 0;
}

const char* dsg_schema_obj_name(int obj) {
    static const char* const names[DSG_OBJ_COUNT] = {
        "", "file", "display", "session", "session field", "web", "event role", "signal role", "stimulus", "method",
        "control", "template", "response", "response trigger", "choice", "state", "over", "exit", "shared exit", "step",
        "data column", "node", "loop", "variable", "node data column",
    };
    return obj > 0 && obj < DSG_OBJ_COUNT ? names[obj] : "";
}

int dsg_schema_holds(int obj, const char* key) {
    int e;
    for (e = 0; e < N_EDGES; e++)
        if (k_edges[e].parent == obj && strcmp(k_edges[e].key, key) == 0) return k_edges[e].each ? 2 : 1;
    return 0;
}

bool dsg_schema_renamable(const dsg_obj* o) {
    return o->set[0] != 0 || o->obj == DSG_OBJ_STATE || o->obj == DSG_OBJ_NODE;
}

/* --- values ------------------------------------------------------------------------ */

int dsg_schema_key_path(const dsg_obj* o, const char* key, char* out, size_t cap) {
    return dsg_path_key(out, cap, o->keys, key);
}

static bool string_type(int t) {
    return t == YEXP_T_STR || t == YEXP_T_ENUM || t == YEXP_T_TEXT || t == YEXP_T_RES || t == YEXP_T_VAR || t == YEXP_T_NAME ||
           t == YEXP_T_NAMEZ || t == YEXP_T_REF;
}

bool dsg_schema_complex(int t) {
    return t == YEXP_T_ENUMS || t == YEXP_T_VEC3 || t == YEXP_T_NUMS || t == YEXP_T_GRID || t == YEXP_T_NAMES || t == YEXP_T_STRS ||
           t == YEXP_T_TARGETS || t == YEXP_T_SMAP || t == YEXP_T_ANY || t == YEXP_T_OBJ || t == YEXP_T_SET || t == YEXP_T_LIST ||
           t == YEXP_T_POINT || t == YEXP_T_CENTER || t == YEXP_T_TRANSFORM || t == YEXP_T_REFS || t == YEXP_T_SINES || t == YEXP_T_BOX;
}

bool dsg_schema_fits(const dsg_field* f, const yjs_value* v) {
    if (!v) return true;
    switch (f->type) {
    case YEXP_T_NUM: case YEXP_T_INT: case YEXP_T_DUR: return v->type == YJS_NUMBER;
    case YEXP_T_BOOL: return v->type == YJS_TRUE || v->type == YJS_FALSE;
    default: break;
    }
    if (string_type(f->type)) return v->type == YJS_STRING && v->u.s[0] != '$';
    return false;
}

size_t dsg_schema_value_text(const dsg_field* f, const yjs_value* v, char* out, size_t cap) {
    size_t n;
    if (cap == 0) return 0;
    out[0] = 0;
    if (!v) {
        if (!f->def) return 0;
        /* the default's JSON; a string default shows as its text */
        if (f->def[0] == '"' && string_type(f->type)) {
            n = strlen(f->def);
            n = n >= 2 ? n - 2 : 0;
            if (n >= cap) n = cap - 1;
            memcpy(out, f->def + 1, n);
            out[n] = 0;
            return n;
        }
        copy(out, cap, f->def);
        return strlen(out);
    }
    if (v->type == YJS_STRING && string_type(f->type)) {
        n = v->n < cap ? v->n : cap - 1;
        memcpy(out, v->u.s, n);
        out[n] = 0;
        return n;
    }
    n = yjs_write_mem(v, YJS_WRITE_COMPACT, out, cap);
    if (n >= cap) {
        /* too long for the field: the text view shows it whole */
        if (cap > 4) memcpy(out + cap - 4, "...", 4);
        return cap - 1;
    }
    return n;
}

static int err_out(char* err, size_t cap, const char* fmt, const char* a, const char* b) {
    if (err && cap) snprintf(err, cap, fmt, a, b);
    return -1;
}

static bool is_json(const char* t) { return t[0] == '{' || t[0] == '['; }

/* Trailing and leading spaces off: a typed number has none. */
static void trim(const char* text, char* out, size_t cap) {
    size_t a = 0, b = strlen(text);
    while (text[a] == ' ' || text[a] == '\t') a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\n' || text[b - 1] == '\r')) b--;
    if (b - a >= cap) b = a + cap - 1;
    memcpy(out, text + a, b - a);
    out[b - a] = 0;
}

static int set_checked(dsg_doc* d, int rc, char* err, size_t cap) {
    if (rc) return err_out(err, cap, "%s%s", dsg_doc_error(d), "");
    return 0;
}

/* A choice that is a plain string ("left") is the object {"key": "left"}
 * before another key goes in (format 2.6). */
static int as_object(dsg_doc* d, const dsg_obj* o) {
    const yjs_value* v = dsg_def_get(d, o->keys);
    char json[DSG_NAME_MAX * 2 + 16];
    size_t n;
    if (!v || v->type == YJS_OBJECT || o->obj != DSG_OBJ_CHOICE || v->type != YJS_STRING) return 0;
    n = (size_t)snprintf(json, sizeof json, "{\"key\":");
    n += yjs_write_mem(v, YJS_WRITE_COMPACT, json + n, sizeof json - n - 2);
    if (n + 2 > sizeof json) return -1;
    json[n++] = '}';
    json[n] = 0;
    return dsg_def_set(d, o->keys, json, n);
}

static int write_in(dsg_doc* d, const char* path, const dsg_field* f, const char* t, unsigned how, char* err, size_t cap) {
    const size_t n = strlen(t);
    unsigned nf = 0;
    if ((how & DSG_WRITE_JSON) && (is_json(t) || t[0] == '"')) return set_checked(d, dsg_def_set(d, path, t, n), err, cap);
    if (f->type == YEXP_T_TEXT || f->type == YEXP_T_STR || f->type == YEXP_T_RES || f->type == YEXP_T_NAMEZ ||
        f->type == YEXP_T_NAME || f->type == YEXP_T_ENUM || f->type == YEXP_T_REF || f->type == YEXP_T_VAR) {
        return set_checked(d, dsg_def_set_string(d, path, t), err, cap);
    }
    if (t[0] == '$') return set_checked(d, dsg_def_set_string(d, path, t), err, cap);
    if (is_json(t)) return set_checked(d, dsg_def_set(d, path, t, n), err, cap);
    switch (f->type) {
    case YEXP_T_DUR: {
        int64_t ns = 0;
        if (yjs_parse_fixed(t, n, 9, &ns) != YJS_OK) return err_out(err, cap, "'%s' is not a number of seconds%s", t, "");
        return set_checked(d, dsg_def_set_seconds_ns(d, path, ns), err, cap);
    }
    case YEXP_T_NUM:
        if (!yjs_number_ok(t, n, &nf)) return err_out(err, cap, "'%s' is not a number%s", t, "");
        return set_checked(d, dsg_def_set_number_text(d, path, t), err, cap);
    case YEXP_T_INT: {
        int64_t x = 0;
        if (!yjs_number_ok(t, n, &nf) || yjs_parse_fixed(t, n, 0, &x) != YJS_OK)
            return err_out(err, cap, "'%s' is not an integer%s", t, "");
        /* 2.5 as an integer key: written as typed, the loader names it */
        if (!(nf & YJS_NUM_INT)) return set_checked(d, dsg_def_set_number_text(d, path, t), err, cap);
        return set_checked(d, dsg_def_set_int(d, path, x), err, cap);
    }
    case YEXP_T_BOOL:
        if (strcmp(t, "true") == 0) return set_checked(d, dsg_def_set_bool(d, path, true), err, cap);
        if (strcmp(t, "false") == 0) return set_checked(d, dsg_def_set_bool(d, path, false), err, cap);
        return err_out(err, cap, "'%s' is not true or false%s", t, "");
    case YEXP_T_VAL: case YEXP_T_CODE: case YEXP_T_OUTCOME:
        if (yjs_number_ok(t, n, &nf)) return set_checked(d, dsg_def_set_number_text(d, path, t), err, cap);
        if (strcmp(t, "true") == 0 || strcmp(t, "false") == 0) return set_checked(d, dsg_def_set_bool(d, path, t[0] == 't'), err, cap);
        return set_checked(d, dsg_def_set_string(d, path, t), err, cap);
    default:
        /* a list, a map or a form: JSON, or a bare name for a TARGETS key */
        if (f->type == YEXP_T_TARGETS) return set_checked(d, dsg_def_set_string(d, path, t), err, cap);
        return set_checked(d, dsg_def_set(d, path, t, n), err, cap);
    }
}

int dsg_schema_write(dsg_doc* d, const dsg_obj* o, const dsg_field* f, const char* text, unsigned how, char* err, size_t cap) {
    char path[DSG_SEL_PATH], label[DSG_NAME_MAX + 16];
    char* t;
    size_t n = strlen(text) + 1;
    int rc;
    if (dsg_schema_key_path(o, f->key, path, sizeof path)) return err_out(err, cap, "the path of '%s' is too long%s", f->key, "");
    t = (char*)malloc(n);
    if (!t) return err_out(err, cap, "out of memory%s%s", "", "");
    /* user text keeps its spaces; a typed number or name does not */
    if ((f->type == YEXP_T_TEXT || f->type == YEXP_T_STR) && !(how & DSG_WRITE_JSON)) memcpy(t, text, n);
    else trim(text, t, n);
    snprintf(label, sizeof label, "Set %s", f->key);
    dsg_doc_begin(d, label);
    rc = as_object(d, o) ? set_checked(d, -1, err, cap) : write_in(d, path, f, t, how, err, cap);
    free(t);
    if (rc) {
        dsg_doc_cancel(d);
        return -1;
    }
    return set_checked(d, dsg_doc_commit(d), err, cap);
}

int dsg_schema_reset(dsg_doc* d, const dsg_obj* o, const dsg_field* f, char* err, size_t cap) {
    char path[DSG_SEL_PATH], label[DSG_NAME_MAX + 16];
    if (dsg_schema_key_path(o, f->key, path, sizeof path)) return err_out(err, cap, "the path of '%s' is too long%s", f->key, "");
    if (!dsg_def_get(d, path)) return 0;
    snprintf(label, sizeof label, "Reset %s", f->key);
    dsg_doc_begin(d, label);
    if (dsg_def_remove(d, path)) {
        dsg_doc_cancel(d);
        return set_checked(d, -1, err, cap);
    }
    return set_checked(d, dsg_doc_commit(d), err, cap);
}

/* --- names --------------------------------------------------------------------------- */

typedef struct names {
    char*  buf;
    size_t cap, used;
    int    n;
} names;

static void add_name(names* s, const char* a, const char* lane) {
    size_t la = strlen(a), ll = lane ? strlen(lane) + 1 : 0;
    s->n++;
    if (s->used + la + ll + 1 > s->cap) return;
    memcpy(s->buf + s->used, a, la);
    if (lane) {
        s->buf[s->used + la] = '.';
        memcpy(s->buf + s->used + la + 1, lane, ll - 1);
    }
    s->buf[s->used + la + ll] = 0;
    s->used += la + ll + 1;
}

/* Lanes of a role's or a control's value (format 3.5, 3.7). */
static int lanes_of(const yjs_value* v, bool control) {
    const char* t = str_of(v, control ? "control" : "type");
    if (!t) return 1;
    if (control) return strcmp(t, "cursor") == 0 || strcmp(t, "moving_target") == 0 ? 2 : 1;
    return strcmp(t, "position3d") == 0 ? 3 : strcmp(t, "scalar") == 0 ? 1 : 2;
}

/* A control's capability (format 3.7's table). */
static unsigned control_refs(const yjs_value* v) {
    const char* k = str_of(v, "control");
    if (!k) return 0;
    if (strcmp(k, "region") == 0 || strcmp(k, "signal_lost") == 0) return DSG_REF_CTL_HITS | DSG_REF_CTL_VALUE;
    if (strcmp(k, "movement") == 0) return DSG_REF_CTL_MOVE;
    if (strcmp(k, "trace") == 0) return 0;
    return DSG_REF_CTL_VALUE;
}

static void add_set(names* s, const yjs_value* set, bool lanes, bool control) {
    static const char* const lane[3] = { "x", "y", "z" };
    uint32_t i;
    if (!set || set->type != YJS_OBJECT) return;
    for (i = 0; i < set->n; i++) {
        int k, nl = lanes ? lanes_of(set->u.members[i].value, control) : 1;
        add_name(s, set->u.members[i].key, NULL);
        for (k = 0; nl > 1 && k < nl; k++) add_name(s, set->u.members[i].key, lane[k]);
    }
}

static void add_controls(names* s, const yjs_value* set, unsigned refs) {
    uint32_t i;
    if (!set || set->type != YJS_OBJECT) return;
    for (i = 0; i < set->n; i++) {
        const yjs_value* c = set->u.members[i].value;
        int k, nl;
        if (!(control_refs(c) & refs)) continue;
        add_name(s, set->u.members[i].key, NULL);
        nl = (refs & DSG_REF_LANES) ? lanes_of(c, true) : 1;
        for (k = 0; nl > 1 && k < nl; k++) add_name(s, set->u.members[i].key, k == 0 ? "x" : "y");
    }
}

int dsg_schema_candidates(const yjs_value* root, const dsg_obj* o, const dsg_field* f, char* buf, size_t cap) {
    names s;
    const yjs_value* t = NULL;
    char p[DSG_SEL_PATH];
    const unsigned r = f->refs;
    s.buf = buf;
    s.cap = cap;
    s.used = 0;
    s.n = 0;
    if (cap) buf[0] = 0;
    if (!root) return 0;
    if (o->tmpl[0] && dsg_path_key(p, sizeof p, "/trials", o->tmpl) == 0) t = dsg_path_get(root, p);
    if (r & DSG_REF_STIM) add_set(&s, yjs_get(root, "stimuli"), false, false);
    if (r & DSG_REF_CONTROL) {
        if (t) add_controls(&s, yjs_get(t, "controls"), r);
        add_controls(&s, yjs_get(root, "controls"), r);
    }
    if ((r & DSG_REF_STATE) && t) {
        const yjs_value* st = yjs_get(t, "states");
        uint32_t i;
        for (i = 0; st && st->type == YJS_ARRAY && i < st->n; i++) {
            const char* nm = str_of(st->u.items[i], "name");
            if (nm) add_name(&s, nm, NULL);
        }
    }
    if ((r & DSG_REF_FLAG) && t) {
        const yjs_value* fl = yjs_get(t, "flags");
        uint32_t i;
        for (i = 0; fl && fl->type == YJS_ARRAY && i < fl->n; i++)
            if (fl->u.items[i]->type == YJS_STRING) add_name(&s, fl->u.items[i]->u.s, NULL);
    }
    if (r & DSG_REF_SIGROLE) add_set(&s, yjs_get(root, "roles"), (r & DSG_REF_LANES) != 0, false);
    if (r & DSG_REF_EVROLE) add_set(&s, yjs_get(root, "devices"), false, false);
    if (r & DSG_REF_TEMPLATE) add_set(&s, yjs_get(root, "trials"), false, false);
    if (r & DSG_REF_METHOD) add_set(&s, yjs_get(root, "methods"), false, false);
    if (r & DSG_REF_RESPONSE) add_name(&s, "response", NULL);
    if (r & DSG_REF_END) add_name(&s, "end", NULL);
    return s.n;
}

/* --- renames ----------------------------------------------------------------------------- */

/* The references a rename changes: the value's path and its new text. */
typedef struct edit {
    char  path[DSG_SEL_PATH];
    char* text;
} edit;

typedef struct renamer {
    unsigned cls;              /* DSG_REF_* of the renamed thing         */
    const char* from;
    const char* to;
    edit*    edits;
    int      n, cap;
    bool     oom;
} renamer;

/* s names `from` (with a lane, or as "$from" for a variable): the new text. */
static char* renamed(const renamer* r, const char* s, bool lanes) {
    const size_t lf = strlen(r->from);
    const char* rest = NULL;
    char* out;
    size_t lt, lr;
    if (r->cls == DSG_REF_VAR || r->cls == (DSG_REF_VAR | DSG_REF_FLAG)) {
        if (s[0] != '$' || strcmp(s + 1, r->from) != 0) return NULL;
        lt = strlen(r->to);
        out = (char*)malloc(lt + 2);
        if (out) {
            out[0] = '$';
            memcpy(out + 1, r->to, lt + 1);
        }
        return out;
    }
    if (strncmp(s, r->from, lf) != 0) return NULL;
    if (s[lf] == 0) rest = "";
    else if (lanes && s[lf] == '.') rest = s + lf;
    else return NULL;
    lt = strlen(r->to);
    lr = strlen(rest);
    out = (char*)malloc(lt + lr + 1);
    if (out) {
        memcpy(out, r->to, lt);
        memcpy(out + lt, rest, lr + 1);
    }
    return out;
}

static void note(renamer* r, const char* path, char* text) {
    if (!text) return;
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 16;
        edit* e = (edit*)realloc(r->edits, (size_t)nc * sizeof *e);
        if (!e) { r->oom = true; free(text); return; }
        r->edits = e;
        r->cap = nc;
    }
    copy(r->edits[r->n].path, sizeof r->edits[r->n].path, path);
    r->edits[r->n].text = text;
    r->n++;
}

static void check_string(renamer* r, const char* path, const yjs_value* v, bool lanes) {
    if (v && v->type == YJS_STRING) note(r, path, renamed(r, v->u.s, lanes));
}

static void check_value(renamer* r, const char* path, const yjs_value* v, bool lanes) {
    uint32_t i;
    char p[DSG_SEL_PATH];
    if (!v) return;
    if (v->type == YJS_STRING) check_string(r, path, v, lanes);
    else if (v->type == YJS_ARRAY)
        for (i = 0; i < v->n; i++)
            if (dsg_path_index(p, sizeof p, path, (int)i) == 0) check_string(r, p, v->u.items[i], lanes);
}

/* The value forms (3.10, 3.15): {"signal": N}, {"when": T, "then", "else"},
 * {"hit": "C.x"}, {"time": N, "from": M}, a lookup's "by", "$NAME". */
static void check_forms(renamer* r, const char* path, const yjs_value* v, int depth) {
    static const char* const keys[] = { "signal", "when", "hit", "time", "from" };
    char p[DSG_SEL_PATH];
    uint32_t i;
    int k;
    if (!v || depth > 16) return;
    if (v->type == YJS_STRING) {
        if (r->cls & DSG_REF_VAR) check_string(r, path, v, false);
        return;
    }
    if (v->type == YJS_ARRAY) {
        for (i = 0; i < v->n; i++)
            if (dsg_path_index(p, sizeof p, path, (int)i) == 0) check_forms(r, p, v->u.items[i], depth + 1);
        return;
    }
    if (v->type != YJS_OBJECT) return;
    for (k = 0; k < (int)(sizeof keys / sizeof keys[0]); k++) {
        const yjs_value* x = yjs_get(v, keys[k]);
        if (x && x->type == YJS_STRING && dsg_path_key(p, sizeof p, path, keys[k]) == 0 && !(r->cls & DSG_REF_VAR))
            check_string(r, p, x, true);
    }
    for (i = 0; i < v->n; i++)
        if (dsg_path_key(p, sizeof p, path, v->u.members[i].key) == 0) check_forms(r, p, v->u.members[i].value, depth + 1);
}

/* An object's references, then its objects', down the edges that hold them:
 * the walk knows each object's section and kind as it goes, so no path is
 * resolved from the root again. */
static void walk(renamer* r, const dsg_obj* o, const yjs_value* v, int depth) {
    uint32_t i;
    int e;
    char p[DSG_SEL_PATH];
    if (!v || depth > 64) return;
    if (o->section < 0) {
        /* a column, a node's data, a signal choice, a plain variable */
        check_forms(r, o->path, v, 0);
    } else {
        const yjs_value* kv = strcmp(o->keys, o->path) == 0 ? v : (v->type == YJS_OBJECT ? yjs_get(v, "walk2d") : NULL);
        for (i = 0; kv && kv->type == YJS_OBJECT && i < kv->n; i++) {
            const char* key = kv->u.members[i].key;
            const yjs_value* x = kv->u.members[i].value;
            const dsg_field* f = dsg_schema_field(o->section, o->kind_bit ? o->kind_bit : 0xffffu, key);
            if (!f || dsg_path_key(p, sizeof p, o->keys, key)) continue;
            if (f->refs & r->cls) check_value(r, p, x, (f->refs & DSG_REF_LANES) != 0);
            /* the forms of a set step's props and a region's center (3.10);
             * a variable can be in any value but the objects the walk goes
             * into next, which would count it twice */
            else if (((r->cls & DSG_REF_VAR) && !dsg_schema_holds(o->obj, key)) || strcmp(key, "props") == 0 || strcmp(key, "center") == 0)
                check_forms(r, p, x, 0);
        }
    }
    if (v->type != YJS_OBJECT) return;
    for (e = 0; e < N_EDGES; e++) {
        const yjs_value* cv;
        char base[DSG_SEL_PATH];
        if (k_edges[e].parent != o->obj || !(cv = yjs_get(v, k_edges[e].key))) continue;
        if (dsg_path_key(base, sizeof base, o->path, k_edges[e].key)) continue;
        if (!k_edges[e].each) {
            dsg_obj c;
            memset(&c, 0, sizeof c);
            c.obj = k_edges[e].child;
            copy(c.path, sizeof c.path, base);
            copy(c.tmpl, sizeof c.tmpl, o->tmpl);
            finish(&c, cv);
            walk(r, &c, cv, depth + 1);
            continue;
        }
        for (i = 0; (cv->type == YJS_OBJECT || cv->type == YJS_ARRAY) && i < cv->n; i++) {
            dsg_obj c;
            const yjs_value* m = cv->type == YJS_OBJECT ? cv->u.members[i].value : cv->u.items[i];
            memset(&c, 0, sizeof c);
            c.obj = k_edges[e].child;
            if (cv->type == YJS_OBJECT) {
                if (dsg_path_key(c.path, sizeof c.path, base, cv->u.members[i].key)) continue;
                copy(c.name, sizeof c.name, cv->u.members[i].key);
                copy(c.set, sizeof c.set, base);
            } else if (dsg_path_index(c.path, sizeof c.path, base, (int)i)) {
                continue;
            }
            copy(c.tmpl, sizeof c.tmpl, c.obj == DSG_OBJ_TEMPLATE ? c.name : o->tmpl);
            finish(&c, m);
            walk(r, &c, m, depth + 1);
        }
    }
}

static unsigned class_of(const dsg_obj* o) {
    switch (o->obj) {
    case DSG_OBJ_STIMULUS: return DSG_REF_STIM;
    case DSG_OBJ_CONTROL: return DSG_REF_CONTROL;
    case DSG_OBJ_STATE: return DSG_REF_STATE;
    case DSG_OBJ_SIGROLE: return DSG_REF_SIGROLE;
    case DSG_OBJ_EVROLE: return DSG_REF_EVROLE;
    case DSG_OBJ_TEMPLATE: return DSG_REF_TEMPLATE;
    case DSG_OBJ_METHOD: return DSG_REF_METHOD;
    case DSG_OBJ_VAR: case DSG_OBJ_SFIELD: return DSG_REF_VAR;
    default: return 0;
    }
}

/* Nodes other than the variable's own that define a variable of this name. */
static int other_vars(const yjs_value* nodes, const char* name, const char* own, const char* path) {
    uint32_t i;
    int k = 0;
    char p[DSG_SEL_PATH], q[DSG_SEL_PATH];
    if (!nodes || nodes->type != YJS_ARRAY) return 0;
    for (i = 0; i < nodes->n; i++) {
        const yjs_value* nd = nodes->u.items[i];
        const yjs_value* vars = nd->type == YJS_OBJECT ? yjs_get(nd, "vars") : NULL;
        if (dsg_path_index(p, sizeof p, path, (int)i)) continue;
        if (vars && vars->type == YJS_OBJECT && yjs_get(vars, name) && dsg_path_key(q, sizeof q, p, "vars") == 0 &&
            dsg_path_key(q, sizeof q, q, name) == 0 && strcmp(q, own) != 0)
            k++;
        if (nd->type == YJS_OBJECT && dsg_path_key(q, sizeof q, p, "timeline") == 0) k += other_vars(yjs_get(nd, "timeline"), name, own, q);
    }
    return k;
}

int dsg_schema_rename(dsg_doc* d, const dsg_obj* o, const char* to, int* changed, char* err, size_t cap) {
    renamer r;
    const yjs_value* root;
    const yjs_value* v;
    char scope[DSG_SEL_PATH], np[DSG_SEL_PATH], label[DSG_NAME_MAX + 16];
    int i, rc = 0;
    if (changed) *changed = 0;
    if (!dsg_schema_renamable(o)) return err_out(err, cap, "a %s has no name to change%s", dsg_schema_obj_name(o->obj), "");
    if (!to[0]) return err_out(err, cap, "give %s a name%s", o->name[0] ? o->name : "it", "");
    if (strcmp(o->name, to) == 0) return 0;
    root = dsg_def_get(d, "");
    v = root ? dsg_path_get(root, o->path) : NULL;
    if (!v) return err_out(err, cap, "'%s' is not in the definition%s", o->path, "");
    memset(&r, 0, sizeof r);
    r.cls = class_of(o);
    r.from = o->name;
    r.to = to;
    /* a template's own names stay in it (3.9); the rest are global */
    scope[0] = 0;
    if (o->tmpl[0] && (o->obj == DSG_OBJ_STATE || (o->obj == DSG_OBJ_CONTROL && strncmp(o->path, "/trials/", 8) == 0)))
        dsg_path_key(scope, sizeof scope, "/trials", o->tmpl);
    if (r.cls == DSG_REF_VAR && o->obj == DSG_OBJ_VAR && other_vars(yjs_get(root, "timeline"), o->name, o->path, "/timeline") > 0)
        r.cls = 0;   /* another node's NAME: its references are not this one's */
    if (r.cls && o->name[0]) {
        dsg_obj top;
        if (dsg_schema_object(root, scope, &top) == 0 && strcmp(top.path, scope) == 0) walk(&r, &top, dsg_path_get(root, scope), 0);
    }
    if (r.oom) rc = err_out(err, cap, "out of memory%s%s", "", "");
    snprintf(label, sizeof label, "Rename %s", o->name);
    dsg_doc_begin(d, label);
    for (i = 0; rc == 0 && i < r.n; i++)
        if (dsg_def_set_string(d, r.edits[i].path, r.edits[i].text)) rc = set_checked(d, -1, err, cap);
    if (rc == 0) {
        if (o->set[0]) {
            if (dsg_def_rename(d, o->set, o->name, to)) rc = set_checked(d, -1, err, cap);
        } else if (dsg_path_key(np, sizeof np, o->path, "name") || dsg_def_set_string(d, np, to)) {
            rc = set_checked(d, -1, err, cap);
        } else if (o->obj == DSG_OBJ_STATE) {
            /* a state's position in the chart follows its name */
            char states[DSG_SEL_PATH];
            if (dsg_path_split(o->path, states, sizeof states, NULL, 0) == 0) dsg_doc_note_rename(d, states, o->name, to);
        }
    }
    for (i = 0; i < r.n; i++) free(r.edits[i].text);
    free(r.edits);
    if (rc) {
        dsg_doc_cancel(d);
        return -1;
    }
    if (changed) *changed = r.n;
    return set_checked(d, dsg_doc_commit(d), err, cap);
}
