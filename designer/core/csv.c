/* csv.c - the index of a table's CSV bytes (csv.h). OWNED BY PART B. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "csv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dsg_csv_free(dsg_csv* c) {
    free(c->off);
    free(c->len);
    memset(c, 0, sizeof *c);
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
    if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) i = 3;
    if (n > UINT32_MAX - 1) { snprintf(err, cap, "the table is larger than 4 GiB"); return -1; }
    while (i < n) {
        size_t start = i;
        uint32_t rec_start = cells;
        bool blank = true;
        field = 0;
        for (;;) {
            size_t fs = i;
            if (i < n && b[i] == '"') {
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
            if (grow(c, cells + 1)) { snprintf(err, cap, "out of memory"); return -1; }
            c->off[cells] = (uint32_t)fs;
            c->len[cells] = (uint32_t)(i - fs);
            if (i > fs) blank = false;
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
        (void)start;
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
