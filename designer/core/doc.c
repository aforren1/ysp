/* doc.c - the document: buffers of bytes, one undo stack of splices, the
 * shared selection, the parsed definition as a cache, and structured edits
 * on a copy of the tree (doc.h; docs/designer.md, "Plan of step 4"). */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "doc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stack drops its oldest steps past these. A step keeps the bytes it
 * replaced and the bytes it put in, so a whole-table rewrite of a large
 * table costs its size twice. */
#define DSG_UNDO_MAX_STEPS 4096
#define DSG_UNDO_MAX_BYTES ((size_t)64 << 20)
#define DSG_MAX_LISTENERS  16

typedef struct buf_t {
    int      kind;
    char     name[DSG_BUF_NAME];
    char*    b;
    size_t   n, cap;
    char*    saved;
    size_t   saved_n;
    uint64_t rev;
} buf_t;

typedef struct splice_t {
    int    buf;
    size_t off;
    char*  old;   size_t old_n;   /* the bytes it replaced */
    char*  ins;   size_t ins_n;   /* the bytes it put in   */
} splice_t;

typedef struct rename_t { char* scope; char* from; char* to; } rename_t;

typedef struct step_t {
    char      label[64];
    splice_t* sp;  int n_sp, c_sp;
    rename_t* rn;  int n_rn, c_rn;
    dsg_sel   before, after;
    bool      merge;       /* typing that later typing may join */
    size_t    bytes;
} step_t;

typedef struct listener_t { dsg_rename_fn fn; void* ctx; } listener_t;

struct dsg_doc {
    buf_t*     bufs[DSG_MAX_BUFFERS];
    int        n_bufs;
    uint64_t   revs;            /* the last revision handed out */
    /* the stack: steps [0, at) are done, [at, n_steps) can be redone */
    step_t*    steps;
    int        n_steps, c_steps, at;
    size_t     undo_bytes;
    bool       merge_open;
    /* the transaction */
    int        depth;
    step_t     cur;
    yjs_arena  work_a;
    yjs_value* work;
    /* the parse cache: good lives in tree_a[tree_cur] */
    yjs_arena  tree_a[2];
    int        tree_cur;
    const yjs_value* good;
    bool       have_parsed, parsed_ok;
    uint64_t   parsed_rev;
    yjs_error  perr;
    /* the selection */
    dsg_sel    sel;
    uint64_t   sel_rev;
    listener_t ls[DSG_MAX_LISTENERS];
    int        n_ls;
    /* the latest change */
    bool       have_last;
    int        last_buf;
    size_t     last_off, last_n;
    char       err[512];
};

static int fail(dsg_doc* d, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->err, sizeof d->err, fmt, ap);
    va_end(ap);
    return -1;
}

static char* dup_n(const char* s, size_t n) {
    char* p = (char*)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* --- buffers ----------------------------------------------------------------- */

static void buf_free(buf_t* b) {
    if (!b) return;
    free(b->b);
    free(b->saved);
    free(b);
}

static int buf_reserve(buf_t* b, size_t n) {
    size_t c;
    char* nb;
    if (n + 1 <= b->cap) return 0;
    c = b->cap ? b->cap : 256;
    while (c < n + 1) c *= 2;
    nb = (char*)realloc(b->b, c);
    if (!nb) return -1;
    b->b = nb;
    b->cap = c;
    return 0;
}

static bool valid_buf(const dsg_doc* d, int buf) { return buf >= 0 && buf < d->n_bufs; }

static void step_free(step_t* s) {
    int i;
    for (i = 0; i < s->n_sp; i++) { free(s->sp[i].old); free(s->sp[i].ins); }
    for (i = 0; i < s->n_rn; i++) { free(s->rn[i].scope); free(s->rn[i].from); free(s->rn[i].to); }
    free(s->sp);
    free(s->rn);
    memset(s, 0, sizeof *s);
}

static void drop_steps(dsg_doc* d, int from) {
    int i;
    for (i = from; i < d->n_steps; i++) {
        d->undo_bytes -= d->steps[i].bytes;
        step_free(&d->steps[i]);
    }
    d->n_steps = from;
    if (d->at > from) d->at = from;
}

dsg_doc* dsg_doc_new(void) {
    dsg_doc* d = (dsg_doc*)calloc(1, sizeof *d);
    if (!d) return NULL;
    yjs_arena_init_heap(&d->work_a, 0);
    yjs_arena_init_heap(&d->tree_a[0], 0);
    yjs_arena_init_heap(&d->tree_a[1], 0);
    dsg_sel_none(&d->sel);
    return d;
}

static void clear(dsg_doc* d) {
    int i;
    if (d->depth) dsg_doc_cancel(d);
    drop_steps(d, 0);
    free(d->steps);
    d->steps = NULL;
    d->c_steps = d->n_steps = d->at = 0;
    d->undo_bytes = 0;
    for (i = 0; i < d->n_bufs; i++) buf_free(d->bufs[i]);
    d->n_bufs = 0;
    yjs_arena_free(&d->work_a);
    yjs_arena_init_heap(&d->work_a, 0);
    d->work = NULL;
    yjs_arena_free(&d->tree_a[0]);
    yjs_arena_free(&d->tree_a[1]);
    yjs_arena_init_heap(&d->tree_a[0], 0);
    yjs_arena_init_heap(&d->tree_a[1], 0);
    d->good = NULL;
    d->have_parsed = d->parsed_ok = false;
    d->merge_open = false;
    d->have_last = false;
    dsg_sel_none(&d->sel);
    d->sel_rev++;
}

void dsg_doc_free(dsg_doc* d) {
    if (!d) return;
    clear(d);
    yjs_arena_free(&d->work_a);
    yjs_arena_free(&d->tree_a[0]);
    yjs_arena_free(&d->tree_a[1]);
    free(d);
}

int dsg_doc_add_buffer(dsg_doc* d, int kind, const char* name, const char* bytes, size_t n) {
    buf_t* b;
    if (d->n_bufs == DSG_MAX_BUFFERS) return fail(d, "more than %d files in one project", DSG_MAX_BUFFERS);
    if (strlen(name) >= DSG_BUF_NAME) return fail(d, "'%.100s...': the name is too long", name);
    if (dsg_doc_find_buffer(d, name) >= 0) return fail(d, "'%s' is open already", name);
    b = (buf_t*)calloc(1, sizeof *b);
    if (!b || buf_reserve(b, n) || !(b->saved = dup_n(bytes, n))) {
        buf_free(b);
        return fail(d, "out of memory");
    }
    b->kind = kind;
    snprintf(b->name, sizeof b->name, "%s", name);
    if (n) memcpy(b->b, bytes, n);
    b->b[n] = 0;
    b->n = n;
    b->saved_n = n;
    b->rev = ++d->revs;
    d->bufs[d->n_bufs] = b;
    return d->n_bufs++;
}

int dsg_doc_reset(dsg_doc* d, const char* name, const char* bytes, size_t n) {
    clear(d);
    return dsg_doc_add_buffer(d, DSG_KIND_DEFINITION, name, bytes, n) == DSG_BUF_DEF ? 0 : -1;
}

int dsg_doc_buffer_count(const dsg_doc* d) { return d->n_bufs; }

int dsg_doc_find_buffer(const dsg_doc* d, const char* name) {
    int i;
    for (i = 0; i < d->n_bufs; i++)
        if (strcmp(d->bufs[i]->name, name) == 0) return i;
    return -1;
}

const char* dsg_doc_buffer_name(const dsg_doc* d, int buf) { return valid_buf(d, buf) ? d->bufs[buf]->name : ""; }
int dsg_doc_buffer_kind(const dsg_doc* d, int buf) { return valid_buf(d, buf) ? d->bufs[buf]->kind : -1; }

const char* dsg_doc_bytes(const dsg_doc* d, int buf, size_t* n) {
    if (!valid_buf(d, buf)) {
        if (n) *n = 0;
        return "";
    }
    if (n) *n = d->bufs[buf]->n;
    return d->bufs[buf]->b;
}

uint64_t dsg_doc_rev(const dsg_doc* d, int buf) { return valid_buf(d, buf) ? d->bufs[buf]->rev : 0; }
uint64_t dsg_doc_revision(const dsg_doc* d) { return d->revs; }

bool dsg_doc_dirty(const dsg_doc* d, int buf) {
    const buf_t* b;
    if (!valid_buf(d, buf)) return false;
    b = d->bufs[buf];
    return b->n != b->saved_n || (b->n && memcmp(b->b, b->saved, b->n) != 0);
}

void dsg_doc_mark_saved(dsg_doc* d, int buf) {
    buf_t* b;
    char* s;
    if (!valid_buf(d, buf)) return;
    b = d->bufs[buf];
    s = dup_n(b->b, b->n);
    if (!s) return;   /* stays dirty: the worst case is a needless question */
    free(b->saved);
    b->saved = s;
    b->saved_n = b->n;
}

int dsg_doc_set_saved(dsg_doc* d, int buf, const char* bytes, size_t n) {
    char* s;
    if (!valid_buf(d, buf)) return fail(d, "no buffer %d", buf);
    s = dup_n(bytes, n);
    if (!s) return fail(d, "out of memory");
    free(d->bufs[buf]->saved);
    d->bufs[buf]->saved = s;
    d->bufs[buf]->saved_n = n;
    return 0;
}

const char* dsg_doc_error(const dsg_doc* d) { return d->err; }

/* --- splices ----------------------------------------------------------------- */

/* Replaces [off, off + del) with ins[0, n) in the buffer. No record. */
static int apply(dsg_doc* d, int buf, size_t off, size_t del, const char* ins, size_t n) {
    buf_t* b = d->bufs[buf];
    size_t nn = b->n - del + n;
    if (buf_reserve(b, nn)) return fail(d, "out of memory");
    memmove(b->b + off + n, b->b + off + del, b->n - off - del);
    if (n) memcpy(b->b + off, ins, n);
    b->n = nn;
    b->b[nn] = 0;
    b->rev = ++d->revs;
    d->have_last = true;
    d->last_buf = buf;
    d->last_off = off;
    d->last_n = n;
    return 0;
}

static int add_splice(dsg_doc* d, step_t* s, int buf, size_t off, size_t del, const char* ins, size_t n) {
    splice_t* sp;
    const buf_t* b = d->bufs[buf];
    if (s->n_sp == s->c_sp) {
        int nc = s->c_sp ? s->c_sp * 2 : 4;
        splice_t* ns = (splice_t*)realloc(s->sp, (size_t)nc * sizeof *ns);
        if (!ns) return fail(d, "out of memory");
        s->sp = ns;
        s->c_sp = nc;
    }
    sp = &s->sp[s->n_sp];
    memset(sp, 0, sizeof *sp);
    sp->buf = buf;
    sp->off = off;
    sp->old = dup_n(b->b + off, del);
    sp->old_n = del;
    sp->ins = dup_n(ins, n);
    sp->ins_n = n;
    if (!sp->old || !sp->ins) {
        free(sp->old);
        free(sp->ins);
        return fail(d, "out of memory");
    }
    if (apply(d, buf, off, del, ins, n)) {
        free(sp->old);
        free(sp->ins);
        return -1;
    }
    s->bytes += del + n;
    s->n_sp++;
    return 0;
}

static void push_step(dsg_doc* d, step_t* s) {
    int i;
    drop_steps(d, d->at);
    if (d->n_steps == d->c_steps) {
        int nc = d->c_steps ? d->c_steps * 2 : 64;
        step_t* ns = (step_t*)realloc(d->steps, (size_t)nc * sizeof *ns);
        if (!ns) { step_free(s); return; }   /* the change stays; its undo is lost */
        d->steps = ns;
        d->c_steps = nc;
    }
    d->steps[d->n_steps++] = *s;
    d->at = d->n_steps;
    d->undo_bytes += s->bytes;
    memset(s, 0, sizeof *s);
    /* the oldest go first */
    for (i = 0; i < d->n_steps - 1 && (d->n_steps - i > DSG_UNDO_MAX_STEPS || d->undo_bytes > DSG_UNDO_MAX_BYTES); i++) {
        d->undo_bytes -= d->steps[i].bytes;
        step_free(&d->steps[i]);
    }
    if (i) {
        memmove(d->steps, d->steps + i, (size_t)(d->n_steps - i) * sizeof *d->steps);
        d->n_steps -= i;
        d->at -= i;
    }
}

static void notify(dsg_doc* d, const step_t* s, bool forward) {
    int i, k;
    for (k = 0; k < s->n_rn; k++) {
        const rename_t* r = &s->rn[forward ? k : s->n_rn - 1 - k];
        for (i = 0; i < d->n_ls; i++)
            d->ls[i].fn(d->ls[i].ctx, r->scope, forward ? r->from : r->to, forward ? r->to : r->from);
    }
}

void dsg_doc_begin(dsg_doc* d, const char* label) {
    if (d->depth++ == 0) {
        memset(&d->cur, 0, sizeof d->cur);
        snprintf(d->cur.label, sizeof d->cur.label, "%s", label ? label : "Edit");
        d->cur.before = d->sel;
        d->work = NULL;
    }
}

bool dsg_doc_in_transaction(const dsg_doc* d) { return d->depth > 0; }

/* The common head and tail of old and new, kept off UTF-8 continuation
 * bytes so a splice never starts inside a character. */
static void trim(const char* a, size_t an, const char* b, size_t bn, size_t* head, size_t* tail) {
    size_t h = 0, t = 0, m = an < bn ? an : bn;
    while (h < m && a[h] == b[h]) h++;
    while (h > 0 && h < an && ((unsigned char)a[h] & 0xC0) == 0x80) h--;
    while (t < m - h && a[an - 1 - t] == b[bn - 1 - t]) t++;
    while (t > 0 && an - t < an && ((unsigned char)a[an - t] & 0xC0) == 0x80) t--;
    *head = h;
    *tail = t;
}

static int replace_in(dsg_doc* d, step_t* s, int buf, const char* bytes, size_t n) {
    const buf_t* b = d->bufs[buf];
    size_t h, t;
    trim(b->b, b->n, bytes, n, &h, &t);
    if (h == b->n && h == n) return 0;
    return add_splice(d, s, buf, h, b->n - h - t, bytes + h, n - h - t);
}

int dsg_doc_commit(dsg_doc* d) {
    int rc = 0;
    if (d->depth == 0) return fail(d, "commit without a transaction");
    if (--d->depth > 0) return 0;
    if (d->work) {
        size_t n = yjs_write_mem(d->work, YJS_WRITE_PRETTY, NULL, 0);
        char* text = (char*)malloc(n + 2);
        if (!text) {
            rc = fail(d, "out of memory");
        } else {
            yjs_write_mem(d->work, YJS_WRITE_PRETTY, text, n + 1);
            text[n++] = '\n';
            rc = replace_in(d, &d->cur, DSG_BUF_DEF, text, n);
            free(text);
        }
        d->work = NULL;
        yjs_arena_reset(&d->work_a);
        if (rc) {
            d->depth = 1;
            dsg_doc_cancel(d);
            return -1;
        }
    }
    if (d->cur.n_sp == 0 && d->cur.n_rn == 0) {
        step_free(&d->cur);
        return 0;
    }
    d->cur.after = d->sel;
    d->merge_open = d->cur.merge;
    {
        step_t s = d->cur;
        memset(&d->cur, 0, sizeof d->cur);
        notify(d, &s, true);
        push_step(d, &s);
    }
    return 0;
}

void dsg_doc_cancel(dsg_doc* d) {
    int i;
    if (d->depth == 0) return;
    d->depth = 0;
    for (i = d->cur.n_sp - 1; i >= 0; i--) {
        const splice_t* sp = &d->cur.sp[i];
        apply(d, sp->buf, sp->off, sp->ins_n, sp->old, sp->old_n);
    }
    if (memcmp(&d->sel, &d->cur.before, sizeof d->sel) != 0) {
        d->sel = d->cur.before;
        d->sel_rev++;
    }
    step_free(&d->cur);
    d->work = NULL;
    yjs_arena_reset(&d->work_a);
}

int dsg_doc_splice(dsg_doc* d, int buf, size_t off, size_t del, const char* ins, size_t n) {
    int rc;
    if (!valid_buf(d, buf)) return fail(d, "no buffer %d", buf);
    if (off > d->bufs[buf]->n || del > d->bufs[buf]->n - off) return fail(d, "a splice past the end of '%s'", d->bufs[buf]->name);
    if (buf == DSG_BUF_DEF && d->work) return fail(d, "a splice of the definition inside a structured edit");
    dsg_doc_begin(d, "Edit");
    rc = add_splice(d, &d->cur, buf, off, del, ins, n);
    if (rc) {
        dsg_doc_cancel(d);
        return -1;
    }
    return dsg_doc_commit(d);
}

/* Typing joins the top step when it continues it: more text where it ended,
 * or a delete of the text it typed. */
static bool try_merge(dsg_doc* d, int buf, const char* bytes, size_t n) {
    step_t* top;
    splice_t* sp;
    const buf_t* b = d->bufs[buf];
    size_t h, t, del, ins, end;
    if (!d->merge_open || d->at != d->n_steps || d->n_steps == 0) return false;
    top = &d->steps[d->n_steps - 1];
    if (!top->merge || top->n_sp != 1 || top->sp[0].buf != buf) return false;
    sp = &top->sp[0];
    trim(b->b, b->n, bytes, n, &h, &t);
    del = b->n - h - t;
    ins = n - h - t;
    end = sp->off + sp->ins_n;
    if (del == 0 && ins > 0 && h == end) {
        char* ni = (char*)realloc(sp->ins, sp->ins_n + ins + 1);
        if (!ni) return false;
        sp->ins = ni;
        memcpy(sp->ins + sp->ins_n, bytes + h, ins);
        sp->ins_n += ins;
        sp->ins[sp->ins_n] = 0;
    } else if (ins == 0 && del > 0 && h + del == end && del <= sp->ins_n) {
        sp->ins_n -= del;
        sp->ins[sp->ins_n] = 0;
    } else {
        return false;
    }
    top->bytes += ins;
    d->undo_bytes += ins;
    apply(d, buf, h, del, bytes + h, ins);
    return true;
}

int dsg_doc_replace(dsg_doc* d, int buf, const char* bytes, size_t n, unsigned flags) {
    int rc;
    const buf_t* b;
    if (!valid_buf(d, buf)) return fail(d, "no buffer %d", buf);
    if (buf == DSG_BUF_DEF && d->work) return fail(d, "a text edit of the definition inside a structured edit");
    b = d->bufs[buf];
    if (b->n == n && memcmp(b->b, bytes, n) == 0) return 0;
    if ((flags & DSG_EDIT_MERGE) && d->depth == 0 && try_merge(d, buf, bytes, n)) return 0;
    dsg_doc_begin(d, (flags & DSG_EDIT_MERGE) ? "Typing" : "Edit");
    rc = replace_in(d, &d->cur, buf, bytes, n);
    if (rc) {
        dsg_doc_cancel(d);
        return -1;
    }
    if (d->depth == 1) d->cur.merge = (flags & DSG_EDIT_MERGE) != 0;
    return dsg_doc_commit(d);
}

/* --- undo ------------------------------------------------------------------- */

bool dsg_doc_can_undo(const dsg_doc* d) { return d->depth == 0 && d->at > 0; }
bool dsg_doc_can_redo(const dsg_doc* d) { return d->depth == 0 && d->at < d->n_steps; }
const char* dsg_doc_undo_label(const dsg_doc* d) { return d->at > 0 ? d->steps[d->at - 1].label : ""; }
const char* dsg_doc_redo_label(const dsg_doc* d) { return d->at < d->n_steps ? d->steps[d->at].label : ""; }
int dsg_doc_undo_depth(const dsg_doc* d) { return d->n_steps; }
size_t dsg_doc_undo_bytes(const dsg_doc* d) { return d->undo_bytes; }
void dsg_doc_break_merge(dsg_doc* d) { d->merge_open = false; }

static void set_sel(dsg_doc* d, const dsg_sel* s) {
    if (memcmp(&d->sel, s, sizeof *s) != 0) {
        d->sel = *s;
        d->sel_rev++;
    }
}

int dsg_doc_undo(dsg_doc* d) {
    int i;
    const step_t* s;
    if (d->depth) return fail(d, "undo inside a transaction");
    if (d->at == 0) return fail(d, "nothing to undo");
    s = &d->steps[d->at - 1];
    for (i = s->n_sp - 1; i >= 0; i--) {
        const splice_t* sp = &s->sp[i];
        if (apply(d, sp->buf, sp->off, sp->ins_n, sp->old, sp->old_n)) return -1;
    }
    d->at--;
    d->merge_open = false;
    set_sel(d, &s->before);
    notify(d, s, false);
    return 0;
}

int dsg_doc_redo(dsg_doc* d) {
    int i;
    const step_t* s;
    if (d->depth) return fail(d, "redo inside a transaction");
    if (d->at == d->n_steps) return fail(d, "nothing to redo");
    s = &d->steps[d->at];
    for (i = 0; i < s->n_sp; i++) {
        const splice_t* sp = &s->sp[i];
        if (apply(d, sp->buf, sp->off, sp->old_n, sp->ins, sp->ins_n)) return -1;
    }
    d->at++;
    d->merge_open = false;
    set_sel(d, &s->after);
    notify(d, s, true);
    return 0;
}

bool dsg_doc_last_change(const dsg_doc* d, int* buf, size_t* off, size_t* n) {
    if (!d->have_last) return false;
    if (buf) *buf = d->last_buf;
    if (off) *off = d->last_off;
    if (n) *n = d->last_n;
    return true;
}

/* --- renames and selection ----------------------------------------------------- */

int dsg_doc_listen(dsg_doc* d, dsg_rename_fn fn, void* ctx) {
    if (d->n_ls == DSG_MAX_LISTENERS) return fail(d, "too many rename listeners");
    d->ls[d->n_ls].fn = fn;
    d->ls[d->n_ls].ctx = ctx;
    d->n_ls++;
    return 0;
}

void dsg_doc_unlisten(dsg_doc* d, dsg_rename_fn fn, void* ctx) {
    int i;
    for (i = 0; i < d->n_ls; i++)
        if (d->ls[i].fn == fn && d->ls[i].ctx == ctx) {
            memmove(&d->ls[i], &d->ls[i + 1], (size_t)(d->n_ls - i - 1) * sizeof d->ls[0]);
            d->n_ls--;
            return;
        }
}

int dsg_doc_note_rename(dsg_doc* d, const char* scope, const char* from, const char* to) {
    rename_t* r;
    step_t* s = &d->cur;
    if (d->depth == 0) return fail(d, "a rename outside a transaction");
    if (s->n_rn == s->c_rn) {
        int nc = s->c_rn ? s->c_rn * 2 : 4;
        rename_t* nr = (rename_t*)realloc(s->rn, (size_t)nc * sizeof *nr);
        if (!nr) return fail(d, "out of memory");
        s->rn = nr;
        s->c_rn = nc;
    }
    r = &s->rn[s->n_rn];
    r->scope = dup_n(scope, strlen(scope));
    r->from = dup_n(from, strlen(from));
    r->to = dup_n(to, strlen(to));
    if (!r->scope || !r->from || !r->to) {
        free(r->scope);
        free(r->from);
        free(r->to);
        return fail(d, "out of memory");
    }
    s->n_rn++;
    return 0;
}

void dsg_sel_none(dsg_sel* s) {
    memset(s, 0, sizeof *s);
    s->buf = -1;
    s->row = s->col = -1;
}

void dsg_sel_def(dsg_sel* s, const char* path) {
    dsg_sel_none(s);
    s->buf = DSG_BUF_DEF;
    snprintf(s->path, sizeof s->path, "%s", path ? path : "");
}

void dsg_doc_select(dsg_doc* d, const dsg_sel* s) {
    dsg_sel c = *s;
    /* the bytes after the path's NUL do not count */
    size_t k = strlen(c.path);
    memset(c.path + k, 0, sizeof c.path - k);
    set_sel(d, &c);
}

const dsg_sel* dsg_doc_selection(const dsg_doc* d) { return &d->sel; }
uint64_t dsg_doc_selection_rev(const dsg_doc* d) { return d->sel_rev; }

/* --- the parse cache ------------------------------------------------------------ */

static void parse_now(dsg_doc* d) {
    const buf_t* b;
    yjs_arena* a;
    yjs_value* root = NULL;
    if (d->n_bufs == 0) return;
    b = d->bufs[DSG_BUF_DEF];
    if (d->have_parsed && d->parsed_rev == b->rev) return;
    a = &d->tree_a[1 - d->tree_cur];
    yjs_arena_reset(a);
    memset(&d->perr, 0, sizeof d->perr);
    if (yjs_parse(a, b->b, b->n, NULL, &root, &d->perr) == YJS_OK) {
        d->tree_cur = 1 - d->tree_cur;
        d->good = root;
        d->parsed_ok = true;
    } else {
        d->parsed_ok = false;
    }
    d->have_parsed = true;
    d->parsed_rev = b->rev;
}

const yjs_value* dsg_doc_root(dsg_doc* d) {
    parse_now(d);
    return d->parsed_ok ? d->good : NULL;
}

const yjs_error* dsg_doc_parse_error(dsg_doc* d) {
    parse_now(d);
    return d->parsed_ok ? NULL : &d->perr;
}

const yjs_value* dsg_doc_shown(dsg_doc* d, bool* stale) {
    parse_now(d);
    if (stale) *stale = !d->parsed_ok;
    return d->good;
}

/* --- paths ------------------------------------------------------------------------ */

/* The next token of a pointer, unescaped into tok (NUL-ended). Returns the
 * rest of the path, or NULL at the end or on a bad escape or overflow. */
static const char* next_token(const char* p, char* tok, size_t cap, bool* bad) {
    size_t k = 0;
    *bad = false;
    if (*p != '/') {
        *bad = *p != 0;
        return NULL;
    }
    p++;
    while (*p && *p != '/') {
        char c = *p++;
        if (c == '~') {
            if (*p == '0') c = '~';
            else if (*p == '1') c = '/';
            else { *bad = true; return NULL; }
            p++;
        }
        if (k + 1 >= cap) { *bad = true; return NULL; }
        tok[k++] = c;
    }
    tok[k] = 0;
    return p;
}

/* An array index: digits, no leading zero but "0". -1 when it is not one. */
static long index_of(const char* tok) {
    long v = 0;
    const char* s = tok;
    if (!*s || (s[0] == '0' && s[1])) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000) return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

static yjs_member* find_member(const yjs_value* obj, const char* key, size_t* at) {
    uint32_t i;
    size_t n = strlen(key);
    for (i = 0; i < obj->n; i++)
        if (obj->u.members[i].key_len == n && memcmp(obj->u.members[i].key, key, n) == 0) {
            if (at) *at = i;
            return &obj->u.members[i];
        }
    return NULL;
}

static yjs_value* step_into(const yjs_value* v, const char* tok) {
    if (!v) return NULL;
    if (v->type == YJS_OBJECT) {
        yjs_member* m = find_member(v, tok, NULL);
        return m ? m->value : NULL;
    }
    if (v->type == YJS_ARRAY) {
        long i = index_of(tok);
        return i >= 0 && (uint32_t)i < v->n ? v->u.items[i] : NULL;
    }
    return NULL;
}

const yjs_value* dsg_path_get(const yjs_value* root, const char* path) {
    char tok[DSG_SEL_PATH];
    const yjs_value* v = root;
    const char* p = path ? path : "";
    bool bad = false;
    while (v && (p = next_token(p, tok, sizeof tok, &bad)) != NULL) v = step_into(v, tok);
    return bad ? NULL : v;
}

static int append_escaped(char* out, size_t cap, size_t at, const char* key) {
    for (; *key; key++) {
        const char* e = *key == '~' ? "~0" : *key == '/' ? "~1" : NULL;
        size_t k = e ? 2 : 1;
        if (at + k + 1 > cap) return -1;
        if (e) { out[at] = e[0]; out[at + 1] = e[1]; }
        else out[at] = *key;
        at += k;
    }
    out[at] = 0;
    return 0;
}

int dsg_path_key(char* out, size_t cap, const char* base, const char* key) {
    size_t b = strlen(base);
    if (b + 2 > cap) return -1;
    if (out != base) memmove(out, base, b);
    out[b] = '/';
    return append_escaped(out, cap, b + 1, key);
}

int dsg_path_index(char* out, size_t cap, const char* base, int index) {
    char num[16];
    snprintf(num, sizeof num, "%d", index);
    return dsg_path_key(out, cap, base, num);
}

int dsg_path_split(const char* path, char* parent, size_t pcap, char* last, size_t lcap) {
    const char* s = strrchr(path, '/');
    size_t pn;
    bool bad = false;
    char tok[DSG_SEL_PATH];
    if (!s) return -1;
    pn = (size_t)(s - path);
    if (parent) {
        if (pn + 1 > pcap) return -1;
        memcpy(parent, path, pn);
        parent[pn] = 0;
    }
    if (last) {
        next_token(s, tok, sizeof tok, &bad);
        if (bad) return -1;
        if (strlen(tok) + 1 > lcap) return -1;
        memcpy(last, tok, strlen(tok) + 1);
    }
    return 0;
}

bool dsg_path_within(const char* path, const char* prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == 0 || path[n] == '/');
}

int dsg_path_at(const yjs_value* root, uint32_t offset, char* out, size_t cap) {
    const yjs_value* v = root;
    if (cap == 0) return -1;
    out[0] = 0;
    while (v && (v->type == YJS_OBJECT || v->type == YJS_ARRAY)) {
        uint32_t i, best = UINT32_MAX, best_pos = 0;
        if (v->type == YJS_OBJECT) {
            for (i = 0; i < v->n; i++) {
                uint32_t kp = v->u.members[i].key_pos;
                if (kp <= offset && (best == UINT32_MAX || kp >= best_pos)) { best = i; best_pos = kp; }
            }
            if (best == UINT32_MAX) break;
            if (dsg_path_key(out, cap, out, v->u.members[best].key)) return -1;
            v = v->u.members[best].value;
        } else {
            for (i = 0; i < v->n; i++) {
                uint32_t ip = v->u.items[i]->pos;
                if (ip <= offset && (best == UINT32_MAX || ip >= best_pos)) { best = i; best_pos = ip; }
            }
            if (best == UINT32_MAX) break;
            if (dsg_path_index(out, cap, out, (int)best)) return -1;
            v = v->u.items[best];
        }
        if (v->pos > offset) break;   /* on the key, before its value */
    }
    return 0;
}

bool dsg_path_offset(const yjs_value* root, const char* path, uint32_t* offset) {
    char tok[DSG_SEL_PATH];
    const yjs_value* v = root;
    const char* p = path ? path : "";
    bool bad = false;
    uint32_t at = root ? root->pos : 0;
    if (!root) return false;
    while ((p = next_token(p, tok, sizeof tok, &bad)) != NULL) {
        if (v->type == YJS_OBJECT) {
            yjs_member* m = find_member(v, tok, NULL);
            if (!m) return false;
            at = m->key_pos;
            v = m->value;
        } else if (v->type == YJS_ARRAY) {
            long i = index_of(tok);
            if (i < 0 || (uint32_t)i >= v->n) return false;
            v = v->u.items[i];
            at = v->pos;
        } else {
            return false;
        }
    }
    if (bad) return false;
    *offset = at;
    return true;
}

/* --- structured edits ------------------------------------------------------------ */

/* The transaction's tree, parsed from buffer 0's bytes at its first edit. */
static yjs_value* work_tree(dsg_doc* d) {
    yjs_error e;
    const buf_t* b;
    if (d->work) return d->work;
    if (d->n_bufs == 0) { fail(d, "no definition"); return NULL; }
    b = d->bufs[DSG_BUF_DEF];
    yjs_arena_reset(&d->work_a);
    if (yjs_parse(&d->work_a, b->b, b->n, NULL, &d->work, &e) != YJS_OK) {
        d->work = NULL;
        fail(d, "the definition's text does not parse (line %u, column %u: %s): fix it in the text view", e.line, e.column, e.msg);
        return NULL;
    }
    return d->work;
}

const yjs_value* dsg_def_get(dsg_doc* d, const char* path) {
    return dsg_path_get(d->work ? d->work : dsg_doc_root(d), path);
}

/* Opens a transaction when none is open: every edit below is one step. */
static bool own_begin(dsg_doc* d, const char* label) {
    bool own = d->depth == 0;
    if (own) dsg_doc_begin(d, label);
    return own;
}

static int own_end(dsg_doc* d, bool own, int rc) {
    if (!own) return rc;
    if (rc) {
        dsg_doc_cancel(d);
        return -1;
    }
    return dsg_doc_commit(d);
}

/* The value at path in the work tree, making missing object members on the
 * way when make is true. */
static yjs_value* resolve(dsg_doc* d, yjs_value* root, const char* path, bool make) {
    char tok[DSG_SEL_PATH];
    yjs_value* v = root;
    const char* p = path;
    bool bad = false;
    while ((p = next_token(p, tok, sizeof tok, &bad)) != NULL) {
        yjs_value* nx = step_into(v, tok);
        if (!nx && make && v->type == YJS_OBJECT) {
            nx = yjs_new(&d->work_a, YJS_OBJECT);
            if (!nx || yjs_set(&d->work_a, v, tok, nx) != YJS_OK) { fail(d, "out of memory"); return NULL; }
            nx = step_into(v, tok);
        }
        if (!nx) { fail(d, "'%s': no such value in the definition", path); return NULL; }
        v = nx;
    }
    if (bad) { fail(d, "'%s' is not a path", path); return NULL; }
    return v;
}

static int set_value(dsg_doc* d, const char* path, yjs_value* val) {
    char parent[DSG_SEL_PATH], last[DSG_SEL_PATH];
    yjs_value* root = d->work;
    yjs_value* pv;
    if (!val) return fail(d, "out of memory");
    if (dsg_path_split(path, parent, sizeof parent, last, sizeof last)) return fail(d, "'%s': not a member's path", path);
    pv = resolve(d, root, parent, true);
    if (!pv) return -1;
    if (pv->type == YJS_OBJECT) {
        if (yjs_set(&d->work_a, pv, last, val) != YJS_OK) return fail(d, "out of memory");
        return 0;
    }
    if (pv->type == YJS_ARRAY) {
        long i;
        if (strcmp(last, "-") == 0) return yjs_push(&d->work_a, pv, val) == YJS_OK ? 0 : fail(d, "out of memory");
        i = index_of(last);
        if (i < 0 || (uint32_t)i >= pv->n) return fail(d, "'%s': no such item", path);
        pv->u.items[i] = val;
        return 0;
    }
    return fail(d, "'%s': its parent is not an object or an array", path);
}

static yjs_value* parse_value(dsg_doc* d, const char* json, size_t n) {
    yjs_value* v = NULL;
    yjs_error e;
    if (yjs_parse(&d->work_a, json, n, NULL, &v, &e) != YJS_OK) {
        fail(d, "the value '%.60s' is not JSON: %s", json, e.msg);
        return NULL;
    }
    return v;
}

int dsg_def_set(dsg_doc* d, const char* path, const char* json, size_t n) {
    bool own = own_begin(d, "Set");
    yjs_value* v;
    int rc = -1;
    if (work_tree(d) && (v = parse_value(d, json, n)) != NULL) rc = set_value(d, path, v);
    return own_end(d, own, rc);
}

int dsg_def_set_string(dsg_doc* d, const char* path, const char* utf8) {
    bool own = own_begin(d, "Set");
    yjs_value* v;
    int rc = -1;
    if (work_tree(d)) {
        v = yjs_new_stringz(&d->work_a, utf8);
        rc = v ? set_value(d, path, v) : fail(d, "'%.60s' is not valid UTF-8 text", utf8);
    }
    return own_end(d, own, rc);
}

int dsg_def_set_int(dsg_doc* d, const char* path, int64_t x) {
    bool own = own_begin(d, "Set");
    int rc = work_tree(d) ? set_value(d, path, yjs_new_int(&d->work_a, x)) : -1;
    return own_end(d, own, rc);
}

int dsg_def_set_bool(dsg_doc* d, const char* path, bool x) {
    bool own = own_begin(d, "Set");
    int rc = work_tree(d) ? set_value(d, path, yjs_new(&d->work_a, x ? YJS_TRUE : YJS_FALSE)) : -1;
    return own_end(d, own, rc);
}

int dsg_def_set_seconds_ns(dsg_doc* d, const char* path, int64_t ns) {
    bool own = own_begin(d, "Set");
    int rc = work_tree(d) ? set_value(d, path, yjs_new_fixed(&d->work_a, ns, 9)) : -1;
    return own_end(d, own, rc);
}

int dsg_def_set_number_text(dsg_doc* d, const char* path, const char* text) {
    bool own = own_begin(d, "Set");
    int rc = -1;
    if (work_tree(d)) {
        yjs_value* v = yjs_new_number(&d->work_a, text, strlen(text));
        rc = v ? set_value(d, path, v) : fail(d, "'%.60s' is not a number", text);
    }
    return own_end(d, own, rc);
}

int dsg_def_remove(dsg_doc* d, const char* path) {
    char parent[DSG_SEL_PATH], last[DSG_SEL_PATH];
    bool own = own_begin(d, "Remove");
    yjs_value* pv;
    int rc = -1;
    if (!work_tree(d)) return own_end(d, own, -1);
    if (dsg_path_split(path, parent, sizeof parent, last, sizeof last)) return own_end(d, own, fail(d, "'%s': the root stays", path));
    pv = resolve(d, d->work, parent, false);
    if (pv && pv->type == YJS_OBJECT) {
        size_t at = 0;
        if (!find_member(pv, last, &at)) rc = fail(d, "'%s': no such member", path);
        else {
            memmove(&pv->u.members[at], &pv->u.members[at + 1], (pv->n - at - 1) * sizeof pv->u.members[0]);
            pv->n--;
            rc = 0;
        }
    } else if (pv && pv->type == YJS_ARRAY) {
        long i = index_of(last);
        if (i < 0 || (uint32_t)i >= pv->n) rc = fail(d, "'%s': no such item", path);
        else {
            memmove(&pv->u.items[i], &pv->u.items[i + 1], (pv->n - (uint32_t)i - 1) * sizeof pv->u.items[0]);
            pv->n--;
            rc = 0;
        }
    } else if (pv) {
        rc = fail(d, "'%s': its parent is not an object or an array", path);
    }
    return own_end(d, own, rc);
}

int dsg_def_insert(dsg_doc* d, const char* array_path, int index, const char* json, size_t n) {
    bool own = own_begin(d, "Insert");
    yjs_value *arr, *v;
    int rc = -1;
    if (work_tree(d) && (arr = resolve(d, d->work, array_path, false)) != NULL && (v = parse_value(d, json, n)) != NULL) {
        uint32_t at = index < 0 ? arr->n : (uint32_t)index;
        if (arr->type != YJS_ARRAY) rc = fail(d, "'%s' is not an array", array_path);
        else if (at > arr->n) rc = fail(d, "'%s': no item %d", array_path, index);
        else if (yjs_push(&d->work_a, arr, v) != YJS_OK) rc = fail(d, "out of memory");
        else {
            memmove(&arr->u.items[at + 1], &arr->u.items[at], (arr->n - 1 - at) * sizeof arr->u.items[0]);
            arr->u.items[at] = v;
            rc = 0;
        }
    }
    return own_end(d, own, rc);
}

int dsg_def_move(dsg_doc* d, const char* array_path, int from, int to) {
    bool own = own_begin(d, "Move");
    yjs_value* arr;
    int rc = -1;
    if (work_tree(d) && (arr = resolve(d, d->work, array_path, false)) != NULL) {
        if (arr->type != YJS_ARRAY) rc = fail(d, "'%s' is not an array", array_path);
        else if (from < 0 || to < 0 || (uint32_t)from >= arr->n || (uint32_t)to >= arr->n) rc = fail(d, "'%s': no such item", array_path);
        else {
            yjs_value* v = arr->u.items[from];
            if (from < to) memmove(&arr->u.items[from], &arr->u.items[from + 1], (size_t)(to - from) * sizeof v);
            else memmove(&arr->u.items[to + 1], &arr->u.items[to], (size_t)(from - to) * sizeof v);
            arr->u.items[to] = v;
            rc = 0;
        }
    }
    return own_end(d, own, rc);
}

int dsg_def_rename(dsg_doc* d, const char* object_path, const char* from, const char* to) {
    bool own = own_begin(d, "Rename");
    yjs_value* obj;
    int rc = -1;
    if (work_tree(d) && (obj = resolve(d, d->work, object_path, false)) != NULL) {
        size_t at = 0;
        yjs_member* m;
        if (obj->type != YJS_OBJECT) rc = fail(d, "'%s' is not an object", object_path);
        else if (!(m = find_member(obj, from, &at))) rc = fail(d, "'%s' has no '%s'", object_path, from);
        else if (strcmp(from, to) == 0) rc = 0;
        else if (find_member(obj, to, NULL)) rc = fail(d, "'%s' has a '%s' already", object_path, to);
        else if (!yjs_utf8_ok(to, strlen(to)) || !to[0]) rc = fail(d, "'%.60s' is not a name", to);
        else {
            yjs_value* v = m->value;
            memmove(&obj->u.members[at], &obj->u.members[at + 1], (obj->n - at - 1) * sizeof obj->u.members[0]);
            obj->n--;
            if (yjs_set(&d->work_a, obj, to, v) != YJS_OK) rc = fail(d, "out of memory");
            else rc = dsg_doc_note_rename(d, object_path, from, to);
        }
    }
    return own_end(d, own, rc);
}
