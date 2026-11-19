/* csv.h - the project's tables as the designer edits them (step 4, part B;
 * docs/designer.md, "Plan of step 4" and "The tables"). OWNED BY PART B.
 *
 * A table is a buffer of the document (doc.h): its CSV bytes are the truth.
 * dsg_csv is an index of those bytes, made again when the buffer's
 * revision changes: each cell's byte range, so a view reads only the cells
 * it shows and an edit splices one cell. The dialect is ysp/table.h's (RFC
 * 4180; ',' ';' or tab; LF or CRLF), so the pack tool reads what the
 * designer writes.
 *
 * Each edit (dsg_csv_set_cell() and the others below) is one undo step of
 * the document. It needs an index of the buffer's current bytes
 * (dsg_csv_sync()), and it leaves the bytes it does not change as they are:
 * the file's line ends, its quoting and any record that ysp/table.h skips.
 * A cell's new text is quoted only when it holds the delimiter, a quote, a
 * CR or an LF. Every function that can fail returns 0, or -1 with one line
 * in c->error.
 */
#ifndef YSP_DESIGNER_CSV_H
#define YSP_DESIGNER_CSV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsg_csv {
    int       n_rows;      /* data rows; the header is row -1            */
    int       n_cols;
    char      delim;       /* ',', ';' or '\t'                           */
    bool      crlf;        /* the first record ends with CRLF            */
    bool      eol_at_end;  /* the bytes end with a line end, or are empty */
    uint32_t* off;         /* (n_rows + 1) * n_cols: each cell's first byte,
                            * the header's first                         */
    uint32_t* len;         /* its bytes as written, quotes included      */
    uint32_t  cap;
    /* dsg_csv_sync()'s cache key: the buffer and revision indexed */
    int       buf;         /* -1: none                                    */
    uint64_t  rev;
    bool      ok;          /* the index matches the bytes                 */
    char      error[256];
} dsg_csv;

/* Indexes CSV bytes. delim 0 is ','. -1 with "line L: ..." in err when the
 * bytes break the dialect (a quote inside an unquoted field, a record with
 * another field count, an unclosed quote). Reuses out's arrays. */
int  dsg_csv_index(dsg_csv* out, const char* bytes, size_t n, char delim, char* err, size_t cap);
void dsg_csv_free(dsg_csv* c);
/* A cell's text, unquoted, NUL-ended in out (cut at cap). Its length, or -1
 * past the table. row -1 is the header. */
int  dsg_csv_cell(const dsg_csv* c, const char* bytes, int row, int col, char* out, size_t cap);

/* The index of buffer buf, made again only when the buffer's revision
 * moved since the last call (a view calls this once a frame). 0, or -1 when
 * the bytes break the dialect: c->error says where, and the table has no
 * rows and no columns until the bytes change. */
int  dsg_csv_sync(dsg_csv* c, const dsg_doc* d, int buf, char delim);

/* A cell's text as the file holds it: in quotes, with each quote doubled,
 * when it holds the delimiter, a quote, a CR or an LF; else as it is. The
 * length; writes at most cap bytes and no NUL (cap 0: the length only). */
size_t dsg_csv_quote(const char* s, size_t n, char delim, char* out, size_t cap);

/* A column name that ysp/table.h takes: a letter or '_', then letters,
 * digits or '_', at most 63 bytes. */
bool dsg_csv_name_ok(const char* s, size_t n);

/* --- edits: each one undo step, with the edited cell selected ------------ */

/* Sets a data cell's text (row >= 0). Text equal to the cell's: no step.
 * A row whose cells are all empty is not a row to ysp/table.h, so it leaves
 * the index too. */
int dsg_csv_set_cell(dsg_doc* d, dsg_csv* c, int row, int col, const char* text, size_t n);
/* Inserts a data row before row at (at = n_rows: after the last), a copy of
 * row copy_of. copy_of -1, or a table with no rows: every cell empty but
 * the first, which is "0" (an empty record is not a row). */
int dsg_csv_insert_row(dsg_doc* d, dsg_csv* c, int at, int copy_of);
int dsg_csv_remove_row(dsg_doc* d, dsg_csv* c, int row);
/* Inserts a column before column at (at = n_cols: after the last), its
 * cells empty. name NULL: "column_N", the first N not taken. */
int dsg_csv_insert_column(dsg_doc* d, dsg_csv* c, int at, const char* name);
/* Refuses the only column. */
int dsg_csv_remove_column(dsg_doc* d, dsg_csv* c, int col);
/* Renames a column, and notes the rename in scope "#NAME/columns" (NAME
 * the buffer's name), so view state keyed by the name follows it through
 * undo and redo. Refuses a name that is not a name or that another column
 * has. */
int dsg_csv_rename_column(dsg_doc* d, dsg_csv* c, int col, const char* name);

/* --- places in messages ------------------------------------------------------ */

/* A table's place in a message of the pack tool, ysp/table.h or the
 * player: "'ENTRY': ysp_table: line L, row R, column 'NAME' (C): ...", or
 * "'ENTRY': column 'NAME' ...". entry gets ENTRY (a pack entry's name);
 * *row the data row from 0, -1 for the header, -2 when the message names
 * none; *col the column from 0 (-1: none); col_name the column's name (""
 * when the message gives only its number). false when the message names
 * no table entry. Any out pointer may be NULL. */
bool dsg_csv_place(const char* msg, char* entry, size_t ecap, int* row, int* col, char* col_name, size_t ncap);

#ifdef __cplusplus
}
#endif
#endif
