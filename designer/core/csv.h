/* csv.h - the project's tables as the designer edits them (step 4, part B;
 * docs/designer.md, "Plan of step 4"). OWNED BY PART B: B may change this
 * file and csv.c as it needs.
 *
 * A table is a buffer of the document (doc.h): its CSV bytes are the truth.
 * dsg_csv is an index of those bytes, made again when the buffer's
 * revision changes: each cell's byte range, so a view reads only the cells
 * it shows and an edit splices one cell. The dialect is ysp/table.h's (RFC
 * 4180; ',' ';' or tab; LF or CRLF), so the pack tool reads what the
 * designer writes.
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
    uint32_t* off;         /* (n_rows + 1) * n_cols: each cell's first byte,
                            * the header's first                         */
    uint32_t* len;         /* its bytes as written, quotes included      */
    uint32_t  cap;
} dsg_csv;

/* Indexes CSV bytes. delim 0 is ','. -1 with "line L: ..." in err when the
 * bytes break the dialect (a quote inside an unquoted field, a record with
 * another field count, an unclosed quote). Reuses out's arrays. */
int  dsg_csv_index(dsg_csv* out, const char* bytes, size_t n, char delim, char* err, size_t cap);
void dsg_csv_free(dsg_csv* c);
/* A cell's text, unquoted, NUL-ended in out (cut at cap). Its length, or -1
 * past the table. row -1 is the header. */
int  dsg_csv_cell(const dsg_csv* c, const char* bytes, int row, int col, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
