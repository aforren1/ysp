/* csv.c - the index of a table's CSV bytes and its edits (csv.h). OWNED BY
 * PART B. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "csv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dsg_csv_free(dsg_csv* c) {
    free(c->off);
    free(c->len);
    memset(c, 0, sizeof *c);
    c->buf = -1;
}

static int grow(dsg_csv* c, uint32_t need) {
    uint32_t nc;
    uint32_t *no, *nl;
    if (need <= c->cap) return 0;
    nc = c->cap ? c->cap : 1024;
    while (nc < need) nc *= 2;
    no = (uint32_t*)realloc(c->off, (size_t)nc * sizeof *no);
    if (!no) return -1;
    c->off = no;
    nl = (uint32_t*)realloc(c->len, (size_t)nc * sizeof *nl);
    if (!nl) return -1;
    c->len = nl;
    c->cap = nc;
    return 0;
}

int dsg_csv_index(dsg_csv* c, const char* b, size_t n, char delim, char* err, size_t cap) {
    size_t i = 0;
    uint32_t cells = 0, line = 1, field = 0, cols = 0;
    bool first = true;
    if (!delim) delim = ',';
    c->n_rows = c->n_cols = 0;
    c->delim = delim;
    c->crlf = false;
    c->eol_at_end = n == 0 || b[n - 1] == '\n';
    if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) i = 3;
    if (n > UINT32_MAX - 1) { snprintf(err, cap, "the table is larger than 4 GiB"); return -1; }
    while (i < n) {
        uint32_t rec_start = cells;
        bool blank = true;
        field = 0;
        for (;;) {
            size_t fs = i;
            bool q = false;
            if (i < n && b[i] == '"') {
                q = true;
                i++;
                for (;;) {
                    if (i >= n) { snprintf(err, cap, "line %u: a quote is not closed", line); return -1; }
                    if (b[i] == '"') {
                        if (i + 1 < n && b[i + 1] == '"') { i += 2; continue; }
                        i++;
                        break;
                    }
                    if (b[i] == '\n') line++;
                    i++;
                }
                if (i < n && b[i] != delim && b[i] != '\n' && b[i] != '\r') {
                    snprintf(err, cap, "line %u: text after a closing quote", line);
                    return -1;
                }
            } else {
                while (i < n && b[i] != delim && b[i] != '\n' && b[i] != '\r') {
                    if (b[i] == '"') { snprintf(err, cap, "line %u: a quote inside an unquoted field", line); return -1; }
                    i++;
                }
            }
            /* ysp/table.h refuses a record longer than the header even when
             * its fields are all empty */
            if (!first && field >= cols) {
                snprintf(err, cap, "line %u: more than the header's %u fields", line, cols);
                return -1;
            }
            if (grow(c, cells + 1)) { snprintf(err, cap, "out of memory"); return -1; }
            c->off[cells] = (uint32_t)fs;
            c->len[cells] = (uint32_t)(i - fs);
            /* "" is an empty field too */
            if (i - fs > (q ? 2u : 0u)) blank = false;
            cells++;
            field++;
            if (i < n && b[i] == delim) { i++; continue; }
            break;
        }
        if (i < n && b[i] == '\r') {
            if (i + 1 >= n || b[i + 1] != '\n') { snprintf(err, cap, "line %u: a CR without an LF", line); return -1; }
            if (first) c->crlf = true;
            i++;
        }
        if (i < n && b[i] == '\n') i++;
        line++;
        if (first) {
            cols = field;
            first = false;
        } else if (blank) {
            cells = rec_start;   /* ysp/table.h skips a record of empty fields */
            continue;
        } else if (field != cols) {
            snprintf(err, cap, "line %u: %u fields, the header has %u", line - 1, field, cols);
            return -1;
        }
    }
    c->n_cols = (int)cols;
    c->n_rows = cols ? (int)(cells / cols) - 1 : 0;
    if (c->n_rows < 0) c->n_rows = 0;
    return 0;
}

int dsg_csv_cell(const dsg_csv* c, const char* bytes, int row, int col, char* out, size_t cap) {
    size_t k = 0, i, at;
    const char* s;
    uint32_t n;
    if (row < -1 || row >= c->n_rows || col < 0 || col >= c->n_cols || cap == 0) return -1;
    at = (size_t)(row + 1) * (size_t)c->n_cols + (size_t)col;
    s = bytes + c->off[at];
    n = c->len[at];
    if (n >= 2 && s[0] == '"') {
        for (i = 1; i + 1 < n; i++) {
            if (s[i] == '"') i++;   /* "" is one quote */
            if (k + 1 < cap) out[k++] = s[i];
        }
    } else {
        for (i = 0; i < n; i++)
            if (k + 1 < cap) out[k++] = s[i];
    }
    out[k] = 0;
    return (int)k;
}

int dsg_csv_sync(dsg_csv* c, const dsg_doc* d, int buf, char delim) {
    size_t n = 0;
    const char* b;
    uint64_t rev;
    if (!delim) delim = ',';
    if (buf <= DSG_BUF_DEF || buf >= dsg_doc_buffer_count(d)) {
        c->buf = -1;
        c->ok = false;
        c->n_rows = c->n_cols = 0;
        snprintf(c->error, sizeof c->error, "no table in buffer %d", buf);
        return -1;
    }
    rev = dsg_doc_rev(d, buf);
    if (c->buf == buf && c->rev == rev && c->delim == delim) return c->ok ? 0 : -1;
    b = dsg_doc_bytes(d, buf, &n);
    c->buf = buf;
    c->rev = rev;
    c->error[0] = 0;
    c->ok = dsg_csv_index(c, b, n, delim, c->error, sizeof c->error) == 0;
    c->delim = delim;
    if (!c->ok) c->n_rows = c->n_cols = 0;
    return c->ok ? 0 : -1;
}

size_t dsg_csv_quote(const char* s, size_t n, char delim, char* out, size_t cap) {
    size_t i, k = 0;
    bool q = false;
    for (i = 0; i < n && !q; i++) q = s[i] == delim || s[i] == '"' || s[i] == '\r' || s[i] == '\n';
    if (!q) {
        if (cap) memcpy(out, s, n < cap ? n : cap);
        return n;
    }
#define PUT(ch) do { if (k < cap) out[k] = (ch); k++; } while (0)
    PUT('"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"') PUT('"');
        PUT(s[i]);
    }
    PUT('"');
#undef PUT
    return k;
}

bool dsg_csv_name_ok(const char* s, size_t n) {
    size_t i;
    if (n == 0 || n > 63) return false;
    for (i = 0; i < n; i++) {
        const char ch = s[i];
        const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
        if (!alpha && !(i > 0 && ch >= '0' && ch <= '9')) return false;
    }
    return true;
}

/* --- edits ------------------------------------------------------------------ */

static int efail(dsg_csv* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof c->error, fmt, ap);
    va_end(ap);
    return -1;
}

/* An edit splices at the index's offsets, so the index must be of the
 * buffer's bytes now. */
static const char* current(dsg_csv* c, const dsg_doc* d, size_t* n) {
    if (c->buf <= DSG_BUF_DEF || c->buf >= dsg_doc_buffer_count(d) || dsg_doc_rev(d, c->buf) != c->rev) {
        efail(c, "the table's index is not of its bytes now");
        return NULL;
    }
    if (!c->ok) {
        efail(c, "the table does not read as CSV; fix it in a text editor first");
        return NULL;
    }
    return dsg_doc_bytes(d, c->buf, n);
}

static size_t at_of(const dsg_csv* c, int row, int col) { return (size_t)(row + 1) * (size_t)c->n_cols + (size_t)col; }
static size_t cell_end(const dsg_csv* c, int row, int col) {
    const size_t a = at_of(c, row, col);
    return (size_t)c->off[a] + c->len[a];
}
/* After a record's line end, or at its end when it has none. */
static size_t record_end(const dsg_csv* c, const char* b, size_t n, int row) {
    size_t e = cell_end(c, row, c->n_cols - 1);
    if (e < n && b[e] == '\r') e++;
    if (e < n && b[e] == '\n') e++;
    return e;
}

static void select_cell(dsg_doc* d, int buf, int row, int col) {
    dsg_sel s;
    dsg_sel_none(&s);
    s.buf = buf;
    s.row = row;
    s.col = col;
    dsg_doc_select(d, &s);
}

/* The selection inside the step, so undo and redo put it back. */
static int splice_step(dsg_doc* d, dsg_csv* c, const char* label, int row, int col, size_t off, size_t del, const char* ins, size_t n) {
    dsg_doc_begin(d, label);
    select_cell(d, c->buf, row, col);
    if (dsg_doc_splice(d, c->buf, off, del, ins, n)) {
        efail(c, "%s", dsg_doc_error(d));
        dsg_doc_cancel(d);
        return -1;
    }
    if (dsg_doc_commit(d)) return efail(c, "%s", dsg_doc_error(d));
    return 0;
}

int dsg_csv_set_cell(dsg_doc* d, dsg_csv* c, int row, int col, const char* text, size_t n) {
    size_t bn = 0, qn, a;
    char* q;
    int rc;
    const char* b = current(c, d, &bn);
    if (!b) return -1;
    if (row < 0 || row >= c->n_rows || col < 0 || col >= c->n_cols) return efail(c, "no cell at row %d, column %d", row + 1, col + 1);
    qn = dsg_csv_quote(text, n, c->delim, NULL, 0);
    q = (char*)malloc(qn + 1);
    if (!q) return efail(c, "out of memory");
    dsg_csv_quote(text, n, c->delim, q, qn);
    a = at_of(c, row, col);
    if (qn == c->len[a] && memcmp(q, b + c->off[a], qn) == 0) {
        free(q);
        return 0;
    }
    rc = splice_step(d, c, "Edit cell", row, col, c->off[a], c->len[a], q, qn);
    free(q);
    return rc;
}

int dsg_csv_insert_row(dsg_doc* d, dsg_csv* c, int at, int copy_of) {
    size_t bn = 0, need = 8, k = 0, off;
    char* rec;
    int i, rc;
    const bool from = copy_of >= 0 && c->n_rows > 0;
    const char* b = current(c, d, &bn);
    if (!b) return -1;
    if (c->n_cols == 0) return efail(c, "the table has no header: add a column first");
    if (at < 0 || at > c->n_rows) return efail(c, "no row %d to insert before", at + 1);
    if (copy_of >= c->n_rows) return efail(c, "no row %d to copy", copy_of + 1);
    if (c->n_rows >= 32767) return efail(c, "a table has at most 32767 rows (ysp/table.h)");
    for (i = 0; i < c->n_cols; i++) need += (from ? c->len[at_of(c, copy_of, i)] : 1u) + 1;
    rec = (char*)malloc(need);
    if (!rec) return efail(c, "out of memory");
    off = at < c->n_rows ? c->off[at_of(c, at, 0)] : bn;
    /* after a last record with no line end: give it one, and leave the new
     * last record without one, as the file was */
    if (at == c->n_rows && !c->eol_at_end) {
        if (c->crlf) rec[k++] = '\r';
        rec[k++] = '\n';
    }
    for (i = 0; i < c->n_cols; i++) {
        if (i) rec[k++] = c->delim;
        if (from) {
            const size_t a = at_of(c, copy_of, i);
            memcpy(rec + k, b + c->off[a], c->len[a]);
            k += c->len[a];
        } else if (i == 0) {
            rec[k++] = '0';
        }
    }
    if (at < c->n_rows || c->eol_at_end) {
        if (c->crlf) rec[k++] = '\r';
        rec[k++] = '\n';
    }
    rc = splice_step(d, c, "Add row", at, -1, off, 0, rec, k);
    free(rec);
    return rc;
}

int dsg_csv_remove_row(dsg_doc* d, dsg_csv* c, int row) {
    size_t bn = 0, s, e;
    const char* b = current(c, d, &bn);
    if (!b) return -1;
    if (row < 0 || row >= c->n_rows) return efail(c, "no row %d", row + 1);
    s = c->off[at_of(c, row, 0)];
    e = record_end(c, b, bn, row);
    /* the last record without a line end: the line end before it goes, so
     * the file still ends as it did */
    if (row == c->n_rows - 1 && !c->eol_at_end) {
        if (s > 0 && b[s - 1] == '\n') s--;
        if (s > 0 && b[s - 1] == '\r') s--;
    }
    return splice_step(d, c, "Remove row", row < c->n_rows - 1 ? row : row - 1, -1, s, e - s, "", 0);
}

/* A whole-buffer edit made of one change a record, as one step: the
 * column edits touch every record, and one replace is one splice where
 * thousands of splices would each move the rest of the buffer. */
typedef struct col_edit { size_t cut, del; const char* ins; size_t n; } col_edit;
typedef void (*col_fn)(const dsg_csv* c, int row, void* ctx, col_edit* e);

static int column_step(dsg_doc* d, dsg_csv* c, const char* b, size_t bn, const char* label, int sel_col, bool drop_blank,
                       col_fn fn, void* ctx) {
    size_t cap = bn + 64, k = 0, pos = 0;
    char* out;
    int r, rc = 0;
    col_edit e = { 0, 0, "", 0 };
    for (r = -1; r < c->n_rows; r++) {
        fn(c, r, ctx, &e);
        cap += e.n;
    }
    out = (char*)malloc(cap);
    if (!out) return efail(c, "out of memory");
    for (r = -1; r < c->n_rows; r++) {
        fn(c, r, ctx, &e);
        /* Records that ysp/table.h skips lie only between the indexed
         * ones. With a column fewer, a skipped record of the old width
         * would be longer than the header, which ysp/table.h refuses. */
        if (drop_blank && r >= 0) {
            const size_t prev_end = record_end(c, b, bn, r - 1);
            memcpy(out + k, b + pos, prev_end - pos);
            k += prev_end - pos;
            pos = c->off[at_of(c, r, 0)];
        }
        memcpy(out + k, b + pos, e.cut - pos);
        k += e.cut - pos;
        memcpy(out + k, e.ins, e.n);
        k += e.n;
        pos = e.cut + e.del;
    }
    {
        const size_t end = drop_blank ? record_end(c, b, bn, c->n_rows - 1) : bn;
        memcpy(out + k, b + pos, end - pos);
        k += end - pos;
    }
    dsg_doc_begin(d, label);
    select_cell(d, c->buf, -1, sel_col);
    if (dsg_doc_replace(d, c->buf, out, k, 0)) {
        rc = efail(c, "%s", dsg_doc_error(d));
        dsg_doc_cancel(d);
    } else if (dsg_doc_commit(d)) {
        rc = efail(c, "%s", dsg_doc_error(d));
    }
    free(out);
    return rc;
}

typedef struct ins_ctx { int at; char cell[80]; size_t cn; char row_ins[2]; } ins_ctx;

static void ins_fn(const dsg_csv* c, int row, void* ctx, col_edit* e) {
    const ins_ctx* x = (const ins_ctx*)ctx;
    e->del = 0;
    e->ins = row == -1 ? x->cell : x->row_ins;
    e->n = row == -1 ? x->cn : 1;
    /* before column at: "name," at its start; after the last: ",name" at
     * its end */
    e->cut = x->at < c->n_cols ? c->off[at_of(c, row, x->at)] : cell_end(c, row, c->n_cols - 1);
}

static bool name_taken(const dsg_csv* c, const char* b, const char* name, int except) {
    char h[128];
    int i;
    for (i = 0; i < c->n_cols; i++)
        if (i != except && dsg_csv_cell(c, b, -1, i, h, sizeof h) >= 0 && strcmp(h, name) == 0) return true;
    return false;
}

int dsg_csv_insert_column(dsg_doc* d, dsg_csv* c, int at, const char* name) {
    size_t bn = 0, nn;
    char gen[32];
    ins_ctx x;
    const char* b = current(c, d, &bn);
    if (!b) return -1;
    if (at < 0 || at > c->n_cols) return efail(c, "no column %d to insert before", at + 1);
    if (c->n_cols >= 256) return efail(c, "a table has at most 256 columns (ysp/table.h)");
    if (!name) {
        int k;
        for (k = c->n_cols + 1;; k++) {
            snprintf(gen, sizeof gen, "column_%d", k);
            if (!name_taken(c, b, gen, -1)) break;
        }
        name = gen;
    }
    nn = strlen(name);
    if (!dsg_csv_name_ok(name, nn))
        return efail(c, "'%.80s' is not a column name: a letter or _, then letters, digits or _, at most 63 bytes", name);
    if (name_taken(c, b, name, -1)) return efail(c, "the table has a column '%s'", name);
    if (c->n_cols == 0) {
        /* an empty file: the new column is the header */
        char rec[80];
        const size_t k = (size_t)snprintf(rec, sizeof rec, "%s\n", name);
        return splice_step(d, c, "Add column", -1, 0, bn, 0, rec, k);
    }
    memset(&x, 0, sizeof x);
    x.at = at;
    x.cn = (size_t)(at < c->n_cols ? snprintf(x.cell, sizeof x.cell, "%s%c", name, c->delim)
                                   : snprintf(x.cell, sizeof x.cell, "%c%s", c->delim, name));
    x.row_ins[0] = c->delim;
    return column_step(d, c, b, bn, "Add column", at, false, ins_fn, &x);
}

static void del_fn(const dsg_csv* c, int row, void* ctx, col_edit* e) {
    const int col = *(const int*)ctx;
    e->ins = "";
    e->n = 0;
    if (col < c->n_cols - 1) {
        /* the cell and the delimiter after it */
        e->cut = c->off[at_of(c, row, col)];
        e->del = c->off[at_of(c, row, col + 1)] - e->cut;
    } else {
        /* the delimiter before the last cell, and the cell */
        e->cut = cell_end(c, row, col - 1);
        e->del = cell_end(c, row, col) - e->cut;
    }
}

int dsg_csv_remove_column(dsg_doc* d, dsg_csv* c, int col) {
    size_t bn = 0;
    const char* b = current(c, d, &bn);
    if (!b) return -1;
    if (col < 0 || col >= c->n_cols) return efail(c, "no column %d", col + 1);
    if (c->n_cols == 1) return efail(c, "a table needs a column: this is its only one");
    return column_step(d, c, b, bn, "Remove column", col < c->n_cols - 1 ? col : col - 1, true, del_fn, &col);
}

int dsg_csv_rename_column(dsg_doc* d, dsg_csv* c, int col, const char* name) {
    size_t bn = 0, nn, a;
    char from[128], scope[DSG_BUF_NAME + 16];
    const char* b = current(c, d, &bn);
    if (!b) return -1;
    if (col < 0 || col >= c->n_cols) return efail(c, "no column %d", col + 1);
    nn = strlen(name);
    if (!dsg_csv_name_ok(name, nn))
        return efail(c, "'%.80s' is not a column name: a letter or _, then letters, digits or _, at most 63 bytes", name);
    if (name_taken(c, b, name, col)) return efail(c, "the table has a column '%s'", name);
    dsg_csv_cell(c, b, -1, col, from, sizeof from);
    if (strcmp(from, name) == 0) return 0;
    snprintf(scope, sizeof scope, "#%s/columns", dsg_doc_buffer_name(d, c->buf));
    a = at_of(c, -1, col);
    dsg_doc_begin(d, "Rename column");
    select_cell(d, c->buf, -1, col);
    if (dsg_doc_splice(d, c->buf, c->off[a], c->len[a], name, nn) || dsg_doc_note_rename(d, scope, from, name)) {
        efail(c, "%s", dsg_doc_error(d));
        dsg_doc_cancel(d);
        return -1;
    }
    return dsg_doc_commit(d) ? efail(c, "%s", dsg_doc_error(d)) : 0;
}

/* --- places in messages ------------------------------------------------------ */

/* 'TEXT' at s into out; the byte after the closing quote, or NULL. */
static const char* quoted(const char* s, char* out, size_t cap) {
    const char* e;
    size_t n;
    if (*s != '\'') return NULL;
    e = strchr(s + 1, '\'');
    if (!e) return NULL;
    n = (size_t)(e - s - 1);
    if (out && cap) {
        if (n >= cap) n = cap - 1;
        memcpy(out, s + 1, n);
        out[n] = 0;
    }
    return e + 1;
}

bool dsg_csv_place(const char* msg, char* entry, size_t ecap, int* row, int* col, char* col_name, size_t ncap) {
    const char* s;
    char ebuf[256], nbuf[128];
    for (s = strchr(msg, '\''); s; s = strchr(s + 1, '\'')) {
        int r = -2, cl = -1;
        const char* t = quoted(s, ebuf, sizeof ebuf);
        nbuf[0] = 0;
        if (!t || strncmp(t, ": ", 2) != 0) continue;
        t += 2;
        if (strncmp(t, "ysp_table: ", 11) == 0) {
            /* ysp/table.h's form: "line L, row R, column 'NAME' (C): " with
             * "header" for row 0 and any part left out */
            int l = 0, v = 0, k = 0;
            t += 11;
            if (sscanf(t, "line %d%n", &l, &k) == 1 && k > 0) {
                t += k;
                if (strncmp(t, ", ", 2) == 0) t += 2;
            }
            k = 0;
            if (strncmp(t, "header", 6) == 0) {
                r = -1;
                t += 6;
            } else if (sscanf(t, "row %d%n", &v, &k) == 1 && k > 0) {
                r = v - 1;
                t += k;
            }
            if (strncmp(t, ", ", 2) == 0) t += 2;
            if (strncmp(t, "column '", 8) == 0) {
                const char* u = quoted(t + 7, nbuf, sizeof nbuf);
                if (u && sscanf(u, " (%d)", &v) == 1) cl = v - 1;
            } else if (sscanf(t, "column %d", &v) == 1) {
                cl = v - 1;
            }
        } else if (strncmp(t, "column '", 8) == 0) {
            quoted(t + 7, nbuf, sizeof nbuf);
        } else {
            continue;
        }
        if (entry && ecap) snprintf(entry, ecap, "%s", ebuf);
        if (row) *row = r;
        if (col) *col = cl;
        if (col_name && ncap) snprintf(col_name, ncap, "%s", nbuf);
        return true;
    }
    return false;
}
