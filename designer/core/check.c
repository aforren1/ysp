/* check.c - the definition checked by the player's own load (pl_check()),
 * against a check pack that the linked pack tool builds from the project
 * (docs/designer.md, "The check"). The designer reimplements no rule: every
 * error here comes from yexp_load(), the player or the pack tool. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"
#include "doc.h"
#include "player.h"
#include "ysp/pack_tool.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dsg.h names pl_check()'s statuses for callers without player.h. */
typedef char dsg_status_ok[((int)DSG_CHECK_OK == (int)PL_CHECK_OK && (int)DSG_CHECK_DEFINITION == (int)PL_CHECK_DEFINITION &&
                            (int)DSG_CHECK_REFUSED == (int)PL_CHECK_REFUSED) ? 1 : -1];

/* The session seed of every check: the result must not change between two
 * checks of one text (pl_check() is deterministic for one seed). */
#define DSG_CHECK_SEED 1u

struct dsg_checker {
    pl_player*   pl;            /* 1.6 MB: heap                              */
    ycol_cal     cal;           /* the stand-in calibration, made once       */
    bool         have_cal;
    yexp_report* rep;           /* yexp_load() without a pack                */
    /* the check pack */
    dsg_pack*    pk;            /* the checker's reference                    */
    void*        pack;          /* pk's bytes, malloc's alignment (>= 8)      */
    size_t       pack_size;
    uint8_t      texts[32];     /* the texts it was built for                */
    uint8_t      tables[32];    /* the unsaved tables it was built from      */
    char         folder[DSG_PATH_MAX];
    /* the last failed build, so an unchanged text does not build again */
    uint8_t      failed_text[32];
    bool         have_failed;
    char         failed_msg[1024];
    /* Step 6: the rig profile of the next checks (dsg_checker_set_rig());
     * part G loads it and gives it to pl_check_desc */
    char         rig_name[DSG_RIG_NAME];
    /* the report */
    dsg_issue*   issues;
    size_t*      msg_off;       /* into strs, until the report is final      */
    size_t*      name_off;      /* SIZE_MAX: none                            */
    int          n_issues, c_issues;
    char*        strs;
    size_t       n_strs, c_strs;
};

/* --- the pack's bytes, shared ---------------------------------------------- */

struct dsg_pack {
    volatile int32_t refs;
    size_t size;
    void*  data;
};

/* Atomic: packs are freed on whichever thread drops the last reference. */
static volatile int32_t g_packs_live;

/* Takes over data (malloc'd). One reference, the caller's. */
static dsg_pack* pack_new(void* data, size_t size) {
    dsg_pack* pk = (dsg_pack*)malloc(sizeof *pk);
    if (!pk) return NULL;
    pk->refs = 1;
    pk->size = size;
    pk->data = data;
    dsg_atomic_add(&g_packs_live, 1);
    return pk;
}

dsg_pack* dsg_pack_retain(dsg_pack* pk) {
    if (pk) dsg_atomic_add(&pk->refs, 1);
    return pk;
}

void dsg_pack_release(dsg_pack* pk) {
    if (!pk || dsg_atomic_add(&pk->refs, -1) != 0) return;
    free(pk->data);
    free(pk);
    dsg_atomic_add(&g_packs_live, -1);
}

const void* dsg_pack_bytes(const dsg_pack* pk, size_t* size) {
    if (size) *size = pk ? pk->size : 0;
    return pk ? pk->data : NULL;
}

int32_t dsg_pack_live(void) { return dsg_atomic_load(&g_packs_live); }

dsg_pack* dsg_checker_pack(dsg_checker* c) { return c ? c->pk : NULL; }

dsg_checker* dsg_checker_new(void) {
    dsg_checker* c = (dsg_checker*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->pl = (pl_player*)calloc(1, sizeof *c->pl);
    c->rep = (yexp_report*)calloc(1, sizeof *c->rep);
    if (!c->pl || !c->rep) { dsg_checker_free(c); return NULL; }
    /* most of a check's time otherwise (player.md, Measured) */
    c->have_cal = pl_check_nominal_cal(&c->cal) == 0;
    return c;
}

void dsg_checker_forget(dsg_checker* c) {
    /* a session may still hold it: only the checker's reference goes */
    dsg_pack_release(c->pk);
    c->pk = NULL;
    c->pack = NULL;
    c->pack_size = 0;
    c->have_failed = false;
    c->folder[0] = 0;
}

/* Part S's stub: the name is kept and the check runs with no profile, as
 * before step 6. Part G loads the profile here (dsg_rig_load()). */
void dsg_checker_set_rig(dsg_checker* c, const char* name) { snprintf(c->rig_name, sizeof c->rig_name, "%s", name ? name : ""); }

void dsg_checker_free(dsg_checker* c) {
    if (!c) return;
    dsg_checker_forget(c);
    free(c->pl);
    free(c->rep);
    free(c->issues);
    free(c->msg_off);
    free(c->name_off);
    free(c->strs);
    free(c);
}

/* --- the report ------------------------------------------------------------ */

static size_t put_str(dsg_checker* c, const char* s) {
    size_t n = strlen(s) + 1, at;
    if (c->n_strs + n > c->c_strs) {
        size_t nc = c->c_strs ? c->c_strs * 2 : 4096;
        char* ns;
        while (nc < c->n_strs + n) nc *= 2;
        ns = (char*)realloc(c->strs, nc);
        if (!ns) return SIZE_MAX;
        c->strs = ns;
        c->c_strs = nc;
    }
    at = c->n_strs;
    memcpy(c->strs + at, s, n);
    c->n_strs += n;
    return at;
}

static void add_issue(dsg_checker* c, int sev, int from, const dsg_place* at, int object, const char* name, const char* fmt, ...) {
    char msg[1536];
    va_list ap;
    dsg_issue* is;
    if (c->n_issues == c->c_issues) {
        int nc = c->c_issues ? c->c_issues * 2 : 32;
        dsg_issue* ni = (dsg_issue*)realloc(c->issues, (size_t)nc * sizeof *ni);
        size_t* nm;
        size_t* nn;
        if (!ni) return;
        c->issues = ni;
        nm = (size_t*)realloc(c->msg_off, (size_t)nc * sizeof *nm);
        if (!nm) return;
        c->msg_off = nm;
        nn = (size_t*)realloc(c->name_off, (size_t)nc * sizeof *nn);
        if (!nn) return;
        c->name_off = nn;
        c->c_issues = nc;
    }
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    is = &c->issues[c->n_issues];
    memset(is, 0, sizeof *is);
    is->severity = sev;
    is->from = from;
    if (at) is->place = *at;
    is->object = object;
    c->msg_off[c->n_issues] = put_str(c, msg);
    c->name_off[c->n_issues] = name ? put_str(c, name) : SIZE_MAX;
    if (c->msg_off[c->n_issues] == SIZE_MAX) return;
    c->n_issues++;
}

/* The place a message gives as "line N, column M" (the pack tool's form),
 * so a build error links to its field too. */
static void place_in_message(const char* msg, const char* text, size_t n, dsg_place* out) {
    const char* s = msg;
    unsigned l = 0, col = 0;
    memset(out, 0, sizeof *out);
    while ((s = strstr(s, "line ")) != NULL && sscanf(s, "line %u, column %u", &l, &col) != 2) s += 5;
    if (s && l > 0) {
        out->line = l;
        out->column = col;
        out->offset = dsg_offset_of(text, n, l, col);
    }
}

static void finish(dsg_checker* c, dsg_report* r) {
    int i, j, sev;
    dsg_issue* sorted;
    for (i = 0; i < c->n_issues; i++) {
        c->issues[i].message = c->strs + c->msg_off[i];
        c->issues[i].name = c->name_off[i] == SIZE_MAX ? NULL : c->strs + c->name_off[i];
    }
    /* errors first, each severity in the order the load met them */
    sorted = c->n_issues > 0 ? (dsg_issue*)malloc((size_t)(unsigned)c->n_issues * sizeof *sorted) : NULL;
    if (sorted) {
        for (sev = DSG_SEV_ERROR, j = 0; sev <= DSG_SEV_NOTE; sev++)
            for (i = 0; i < c->n_issues; i++)
                if (c->issues[i].severity == sev) sorted[j++] = c->issues[i];
        memcpy(c->issues, sorted, (size_t)(unsigned)c->n_issues * sizeof *sorted);
        free(sorted);
    }
    r->issues = c->issues;
    r->n_issues = c->n_issues;
    r->n_errors = 0;
    for (i = 0; i < c->n_issues; i++) r->n_errors += c->issues[i].severity == DSG_SEV_ERROR;
    r->have_pack = c->pack != NULL;
}

static void add_diags(dsg_checker* c, const yexp_report* rep, const char* text, size_t n) {
    int i;
    for (i = 0; i < rep->n_diags; i++) {
        const yexp_diag* d = &rep->diags[i];
        dsg_place at;
        memset(&at, 0, sizeof at);
        if (d->line) {
            at.line = d->line;
            at.column = d->column;
            at.offset = dsg_offset_of(text, n, d->line, d->column);
        }
        add_issue(c, d->severity == YEXP_ERROR ? DSG_SEV_ERROR : DSG_SEV_WARNING, DSG_FROM_LOADER, &at, PL_OBJ_NONE, NULL, "%s", d->msg);
    }
    if (rep->n_errors + rep->n_warnings > rep->n_diags)
        add_issue(c, DSG_SEV_NOTE, DSG_FROM_LOADER, NULL, PL_OBJ_NONE, NULL, "%d more diagnostics not kept (the loader keeps %d)",
                  rep->n_errors + rep->n_warnings - rep->n_diags, YEXP_MAX_DIAGS);
}

static void add_need(dsg_checker* c, const pl_need* nd, const char* text, size_t n) {
    dsg_place at;
    const char* nm = nd->name ? nd->name : "";
    dsg_locate_need(text, n, nd->kind, nd->name, &at);
    switch (nd->kind) {
    case PL_NEED_FIELD:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, nd->name, "a real run needs the session field '%s' (%s): --field %s=VALUE",
                  nm, nd->value ? "text" : "integer", nm);
        break;
    case PL_NEED_CALIBRATION:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, NULL,
                  "a real run needs a display calibration (display.calibration is \"required\"): a rig profile with one, or --cal FILE");
        break;
    case PL_NEED_VIEW:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, NULL, "a real run needs a rig profile with a view (display.units is \"%s\")",
                  nd->value == 2 ? "cm" : "deg");
        break;
    case PL_NEED_DISPLAY_TIER:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, NULL, "a real run needs a display of tier %d (display.min_tier)", nd->value);
        break;
    case PL_NEED_DEVICE:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, nd->name,
                  "a real run needs a device for the role '%s' in the rig profile: the keyboard cannot take it", nm);
        break;
    case PL_NEED_ROLE_TIER:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, nd->name,
                  "a real run needs the role '%s' at tier %d: a checked loopback in the rig profile, or --allow-below-tier %s", nm,
                  nd->value, nm);
        break;
    case PL_NEED_SIGNAL:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, nd->name, "a real run needs an adapter for the %ssignal role '%s' in the rig profile",
                  nd->value ? "required " : "", nm);
        break;
    case PL_NEED_SIM_GAIN:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, nd->name,
                  "a --sim run needs --sim-gain %s=UNITS or the rig's signal for the relative role '%s'", nm, nm);
        break;
    default:
        add_issue(c, DSG_SEV_NEED, DSG_FROM_RUN, &at, nd->kind, nd->name, "a real run needs something this designer does not name (%d)",
                  nd->kind);
        break;
    }
}

/* --- the check pack -------------------------------------------------------- */

/* The tables' unsaved bytes are dsg_check()'s document (dsg.h). */
static bool table_dirty(const dsg_doc* d, int b) {
    return d && dsg_doc_buffer_kind(d, b) == DSG_KIND_TABLE && dsg_doc_dirty(d, b);
}

/* The dirty tables of d, pointing into it: at most DSG_MAX_BUFFERS. */
static int doc_tables(const dsg_doc* d, dsg_table_bytes* out) {
    int b, k = 0;
    for (b = 1; d && b < dsg_doc_buffer_count(d) && k < DSG_MAX_BUFFERS; b++) {
        if (!table_dirty(d, b)) continue;
        out[k].buf = b;
        out[k].name = dsg_doc_buffer_name(d, b);
        out[k].rev = dsg_doc_rev(d, b);
        out[k].bytes = dsg_doc_bytes(d, b, &out[k].n);
        k++;
    }
    return k;
}

typedef struct tables_in { const dsg_table_bytes* t; int n; } tables_in;

static const dsg_table_bytes* find_table(const tables_in* ti, const char* name) {
    int i;
    for (i = 0; i < ti->n; i++)
        if (strcmp(ti->t[i].name, name) == 0) return &ti->t[i];
    return NULL;
}

/* Which tables the pack must take from the document, and at which
 * revision: a clean table comes from its file. */
static void tables_key(const tables_in* ti, uint8_t key[32]) {
    ypak_sha256_state h;
    int i;
    ypak_sha256_init(&h);
    for (i = 0; ti && i < ti->n; i++) {
        const char* nm = ti->t[i].name;
        const uint64_t rev = ti->t[i].rev;
        ypak_sha256_update(&h, nm, strlen(nm) + 1);
        ypak_sha256_update(&h, &rev, sizeof rev);
    }
    ypak_sha256_final(&h, key);
}

/* Where a dirty table's bytes go for the build, relative to the folder. */
static void table_copy(char* out, size_t cap, int b) { snprintf(out, cap, DSG_DIR "/check/table%d.csv", b); }

typedef struct tables_ctx { ypak_pack pk; bool open; } tables_ctx;

static const void* table_bytes(void* ctx, const char* name, size_t* n) {
    tables_ctx* t = (tables_ctx*)ctx;
    ypak_entry e;
    const void* d;
    *n = 0;
    if (!t->open || ypak_find(&t->pk, name, &e) != 0 || e.kind != YPAK_KIND_TABLE) return NULL;
    d = ypak_data(&t->pk, &e);
    if (d) *n = (size_t)e.size;
    return d;
}

/* The texts the definition's text stimuli can show, as one hash: the glyph
 * runs are the only entries of the pack that the definition decides. 0, or
 * -1 with the pack tool's refusal in err. */
static int texts_key(const dsg_checker* c, const char* text, size_t n, uint8_t key[32], char* err, size_t cap) {
    tables_ctx t;
    ypt_texts tx;
    ypak_desc d;
    ypak_sha256_state h;
    int i, rc;
    memset(&t, 0, sizeof t);
    memset(&d, 0, sizeof d);
    d.data = c->pack;
    d.size = c->pack_size;
    t.open = c->pack && ypak_open(&t.pk, &d);
    memset(&tx, 0, sizeof tx);
    rc = ypt_derive_texts(text, n, table_bytes, &t, &tx, err, cap);
    if (t.open) ypak_close(&t.pk);
    if (rc != 0) return -1;
    ypak_sha256_init(&h);
    ypak_sha256_update(&h, &tx.definition, sizeof tx.definition);
    for (i = 0; i < tx.n_texts; i++) ypak_sha256_update(&h, tx.texts[i].key, strlen(tx.texts[i].key) + 1);
    for (i = 0; i < tx.n_tables; i++) ypak_sha256_update(&h, tx.tables[i], strlen(tx.tables[i]) + 1);
    ypak_sha256_final(&h, key);
    ypt_texts_free(&tx);
    return 0;
}

/* The project's description with the experiment's source replaced by the
 * text under check, and each dirty table's by its copy. malloc'd,
 * NUL-ended. */
static char* check_description(const dsg_project* p, const tables_in* tables, size_t* len, char* err, size_t cap) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_value* ex;
    yjs_error e;
    char* out = NULL;
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, p->desc, p->desc_len, NULL, &root, &e) != YJS_OK || root->type != YJS_OBJECT ||
        !(ex = yjs_get(root, "experiment")) || ex->type != YJS_OBJECT ||
        yjs_set(&a, ex, "source", yjs_new_stringz(&a, DSG_CHECK_SOURCE)) != YJS_OK) {
        snprintf(err, cap, "%.900s: cannot read its experiment: reopen the project", p->desc_path);
    } else {
        const yjs_value* res = yjs_get(root, "resources");
        size_t i;
        for (i = 0; res && res->type == YJS_ARRAY && i < res->n; i++) {
            yjs_value* r = yjs_at(res, i);
            const yjs_value* src = r && r->type == YJS_OBJECT ? yjs_get(r, "source") : NULL;
            const yjs_value* kind = r && r->type == YJS_OBJECT ? yjs_get(r, "kind") : NULL;
            const dsg_table_bytes* tb;
            if (!src || src->type != YJS_STRING || !kind || kind->type != YJS_STRING || strcmp(kind->u.s, "table") != 0 || !tables)
                continue;
            tb = find_table(tables, src->u.s);
            if (tb) {
                char rel[64];
                table_copy(rel, sizeof rel, tb->buf);
                yjs_set(&a, r, "source", yjs_new_stringz(&a, rel));
            }
        }
        size_t n = yjs_write_mem(root, YJS_WRITE_COMPACT, NULL, 0);
        out = (char*)malloc(n + 1);
        if (out) {
            yjs_write_mem(root, YJS_WRITE_COMPACT, out, n + 1);
            *len = n;
        } else {
            snprintf(err, cap, "out of memory");
        }
    }
    yjs_arena_free(&a);
    return out;
}

static int build(dsg_checker* c, const dsg_project* p, const char* text, size_t n, const tables_in* tables, const uint8_t tkey[32],
                 char* err, size_t cap) {
    char dir[DSG_PATH_MAX], src[DSG_PATH_MAX], pack[DSG_PATH_MAX];
    char* desc;
    size_t dlen = 0;
    char* bytes = NULL;
    size_t blen = 0;
    int rc, i;
    uint8_t key[32];
    dsg_pack* pk;
    if (dsg_join(dir, sizeof dir, p->folder, DSG_DIR) || dsg_join(src, sizeof src, p->folder, DSG_CHECK_SOURCE) ||
        dsg_join(pack, sizeof pack, p->folder, DSG_CHECK_PACK)) {
        snprintf(err, cap, "the project's path is too long");
        return -1;
    }
    if (dsg_make_dir(dir, err, cap)) return -1;
    dsg_join(dir, sizeof dir, p->folder, DSG_DIR "/check");
    if (dsg_make_dir(dir, err, cap)) return -1;
    if (dsg_write_file(src, text, n, err, cap)) return -1;
    for (i = 0; tables && i < tables->n; i++) {
        char rel[64], path[DSG_PATH_MAX];
        table_copy(rel, sizeof rel, tables->t[i].buf);
        if (dsg_join(path, sizeof path, p->folder, rel)) {
            snprintf(err, cap, "the project's path is too long");
            return -1;
        }
        if (dsg_write_file(path, tables->t[i].bytes, tables->t[i].n, err, cap)) return -1;
    }
    desc = check_description(p, tables, &dlen, err, cap);
    if (!desc) return -1;
    /* Build's file (part B): the entry cache, decision 17 */
    rc = dsg_build_check_pack(p, desc, dlen, pack, err, cap);
    free(desc);
    if (rc != 0) return -1;
    if (dsg_read_file(pack, &bytes, &blen, err, cap)) return -1;
    pk = pack_new(bytes, blen);
    if (!pk) {
        free(bytes);
        snprintf(err, cap, "out of memory");
        return -1;
    }
    dsg_pack_release(c->pk);
    c->pk = pk;
    c->pack = bytes;
    c->pack_size = blen;
    if (texts_key(c, text, n, key, err, cap) == 0) memcpy(c->texts, key, sizeof key);
    else memset(c->texts, 0, sizeof c->texts);
    memcpy(c->tables, tkey, sizeof c->tables);
    return 0;
}

/* A build, unless this exact text with these tables failed the last one.
 * Adds the failure. */
static bool try_build(dsg_checker* c, const dsg_project* p, const char* text, size_t n, const tables_in* tables, const uint8_t tkey[32],
                      dsg_report* r) {
    uint8_t h[32];
    dsg_place at;
    ypak_sha256_state hs;
    ypak_sha256_init(&hs);
    ypak_sha256_update(&hs, text, n);
    ypak_sha256_update(&hs, tkey, 32);
    ypak_sha256_final(&hs, h);
    if (!(c->have_failed && memcmp(h, c->failed_text, sizeof h) == 0)) {
        if (build(c, p, text, n, tables, tkey, c->failed_msg, sizeof c->failed_msg) == 0) {
            c->have_failed = false;
            r->built = true;
            return true;
        }
        memcpy(c->failed_text, h, sizeof h);
        c->have_failed = true;
    }
    place_in_message(c->failed_msg, text, n, &at);
    add_issue(c, DSG_SEV_ERROR, DSG_FROM_PACK, &at, PL_OBJ_NONE, NULL, "the check pack: %s", c->failed_msg);
    return false;
}

/* --- the check ------------------------------------------------------------- */

int dsg_check(dsg_checker* c, const dsg_project* p, const char* text, size_t n, const dsg_doc* tables, dsg_report* r) {
    dsg_table_bytes t[DSG_MAX_BUFFERS];
    const int nt = doc_tables(tables, t);
    return dsg_check_tables(c, p, text, n, t, nt, r);
}

int dsg_check_tables(dsg_checker* c, const dsg_project* p, const char* text, size_t n, const dsg_table_bytes* tbl, int n_tables,
                     dsg_report* r) {
    pl_check_desc d;
    tables_in ti;
    const tables_in* tables = &ti;
    pl_check_result res;
    int i;
    bool derive_failed = false;
    char derive_err[1024];
    uint8_t tkey[32] = { 0 };
    ti.t = tbl;
    ti.n = n_tables > 0 ? n_tables : 0;
    memset(r, 0, sizeof *r);
    r->status = -1;
    c->n_issues = 0;
    c->n_strs = 0;
    if (strcmp(c->folder, p->folder) != 0) {
        dsg_checker_forget(c);
        snprintf(c->folder, sizeof c->folder, "%s", p->folder);
    }
    tables_key(tables, tkey);

    if (!c->pack) {
        /* No pack yet: the loader alone, with no host, says whether the
         * text is worth a build. Its host checks wait for the pack. */
        yjs_arena a;
        yexp_def* def = NULL;
        int lr;
        yjs_arena_init_heap(&a, 0);
        lr = yexp_load(&a, text, n, NULL, &def, c->rep);
        yjs_arena_free(&a);
        if (lr != YEXP_OK) {
            add_diags(c, c->rep, text, n);
            add_issue(c, DSG_SEV_NOTE, DSG_FROM_DESIGNER, NULL, PL_OBJ_NONE, NULL,
                      "checked without a check pack: the tables, the pack entries and the player's own checks wait until the loader reads the definition");
            finish(c, r);
            return 0;
        }
        if (!try_build(c, p, text, n, tables, tkey, r)) {
            finish(c, r);
            return 0;
        }
    } else {
        uint8_t key[32];
        if (texts_key(c, text, n, key, derive_err, sizeof derive_err) != 0) {
            derive_failed = true;
        } else if (memcmp(key, c->texts, sizeof key) != 0 || memcmp(tkey, c->tables, sizeof tkey) != 0) {
            /* a failed rebuild leaves the old pack, still good for the rest */
            (void)try_build(c, p, text, n, tables, tkey, r);
        }
    }

    memset(&d, 0, sizeof d);
    d.pack.data = c->pack;
    d.pack.size = c->pack_size;
    d.def_text = text;
    d.def_len = n;
    d.seed = DSG_CHECK_SEED;
    d.cal = c->have_cal ? &c->cal : NULL;
    r->status = pl_check(c->pl, &d, &res);
    if (res.report) add_diags(c, res.report, text, n);
    /* The pack tool's own refusal of the texts; a text the loader refuses
     * has the loader's message, which says the same with more care. */
    if (derive_failed && res.status != PL_CHECK_DEFINITION) {
        dsg_place at;
        place_in_message(derive_err, text, n, &at);
        add_issue(c, DSG_SEV_ERROR, DSG_FROM_PACK, &at, PL_OBJ_NONE, NULL, "the pack tool: %s", derive_err);
    }
    if (res.status == PL_CHECK_REFUSED) {
        dsg_place at;
        dsg_locate_object(text, n, res.object, res.object_name, &at);
        add_issue(c, DSG_SEV_ERROR, DSG_FROM_PLAYER, &at, res.object, res.object_name, "%s", res.message);
    }
    for (i = 0; i < res.n_needs; i++) add_need(c, &res.needs[i], text, n);
    pl_close(c->pl);
    finish(c, r);
    return 0;
}
