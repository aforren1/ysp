/* table_test.c - the tables' index, edits and check with no window
 * (designer/core/csv.h; step 4, part B; docs/designer.md, "The tables").
 * OWNED BY PART B.
 *
 * Usage: test_designer_table WORK
 *   WORK is a folder the test may write: it copies a project into it and
 *   builds its check pack there.
 *   1. The index of a table as ysp/table.h reads it: the header, the rows, a
 *      quoted cell with a delimiter and a quote, CRLF, a skipped empty
 *      record, a BOM; the dialect's refusals, each with its line.
 *   2. The index against ytb_open_csv() on every CSV in tests/ and
 *      examples/ and on made-up ones: the same rows, columns and cell texts.
 *   3. Edits: a cell, a row and a column added and removed, a column
 *      renamed. Each is one undo step; ytb_open_csv() reads the new bytes as
 *      intended; undo gives the old bytes back and redo the new; quoting
 *      where a cell holds a delimiter, a quote or a line end; the file's
 *      line ends kept (LF, CRLF, no line end at the end).
 *   4. A rename notes "#NAME/columns" for the listeners, and undo and redo
 *      call them back; bad and taken names are refused with no step.
 *   5. Places in messages: ysp/table.h's and the pack tool's to a row and a
 *      column.
 *   6. The check with an unsaved table (tests/player/oddball): a level the
 *      definition names, removed in the document only, is reported; the
 *      file on disk does not change; undo checks clean again. A cell that
 *      the pack tool refuses names its row and column.
 * Exit 0 on pass, 1 on a failure.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "csv.h"
#include "dsg.h"
#include "ysp/table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work;

/* --- ysp/table.h as the reference ---------------------------------------- */

typedef struct ref { ytb_table t; void* arena; } ref;

static bool ref_open(ref* r, const char* b, size_t n, char delim) {
    ytb_csv_desc* d = (ytb_csv_desc*)calloc(1, sizeof *d);
    bool ok;
    memset(r, 0, sizeof *r);
    if (!d) return false;
    d->text = b;
    d->len = n;
    d->delimiter = delim;
    (void)ytb_open_csv(&r->t, d);
    if (!r->t.need) { free(d); return false; }
    r->arena = malloc(r->t.need + 16);
    d->arena = r->arena;
    d->arena_size = r->t.need;
    ok = r->arena && ytb_open_csv(&r->t, d);
    free(d);
    return ok;
}

static void ref_close(ref* r) {
    ytb_close(&r->t);
    free(r->arena);
    r->arena = NULL;
}

/* The index and ytb_open_csv() read the same table: rows, columns, names
 * and every cell's text. The first difference in why. */
static bool same_as_ytb(const char* b, size_t n, char delim, char* why, size_t cap) {
    dsg_csv c;
    ref r = { 0 };
    char err[256], cell[4200];
    int i, j;
    bool ok = true;
    memset(&c, 0, sizeof c);
    why[0] = 0;
    if (!ref_open(&r, b, n, delim)) {
        snprintf(why, cap, "ytb refuses: %s", ytb_error(&r.t));
        ref_close(&r);
        return false;
    }
    if (dsg_csv_index(&c, b, n, delim, err, sizeof err)) {
        snprintf(why, cap, "the index refuses: %s", err);
        ok = false;
    } else if (c.n_rows != r.t.n_rows || c.n_cols != r.t.n_cols) {
        snprintf(why, cap, "%d x %d, ytb %d x %d", c.n_rows, c.n_cols, r.t.n_rows, r.t.n_cols);
        ok = false;
    }
    for (j = 0; ok && j < c.n_cols; j++) {
        dsg_csv_cell(&c, b, -1, j, cell, sizeof cell);
        if (strcmp(cell, ytb_col_name(&r.t, j)) != 0) {
            snprintf(why, cap, "header %d: '%s', ytb '%s'", j, cell, ytb_col_name(&r.t, j));
            ok = false;
        }
    }
    for (i = 0; ok && i < c.n_rows; i++)
        for (j = 0; ok && j < c.n_cols; j++) {
            const char* t = ytb_text(&r.t, i, j);
            dsg_csv_cell(&c, b, i, j, cell, sizeof cell);
            if (!t || strcmp(cell, t) != 0) {
                /* a number column keeps one text for each level: "0.50" and
                 * "0.5" are one level */
                double x = 0, y = 0;
                if (!t || ytb_col_type(&r.t, j) == YTB_STRING || ytb_parse_number(cell, strlen(cell), &x) != 0 ||
                    ytb_parse_number(t, strlen(t), &y) != 0 || x != y) {
                    snprintf(why, cap, "row %d col %d: '%s', ytb '%s'", i, j, cell, t ? t : "(null)");
                    ok = false;
                }
            }
        }
    ref_close(&r);
    dsg_csv_free(&c);
    return ok;
}

static char* read_all(const char* path, size_t* n) {
    char* d = NULL;
    char err[512];
    if (dsg_read_file(path, &d, n, err, sizeof err)) return NULL;
    return d;
}

/* --- 1. the index --------------------------------------------------------- */

static void test_index(void) {
    static const char t[] = "\xEF\xBB\xBFword,n\r\n\"a, \"\"b\"\"\",1\r\n,\r\nc,2\r\n";
    dsg_csv c;
    char cell[64], err[256];
    memset(&c, 0, sizeof c);
    CHECK(dsg_csv_index(&c, t, sizeof t - 1, ',', err, sizeof err) == 0, "index: %s", err);
    CHECK(c.n_cols == 2 && c.n_rows == 2 && c.crlf && c.eol_at_end, "2 columns, 2 rows (the empty record skipped), CRLF: %d %d", c.n_cols, c.n_rows);
    CHECK(dsg_csv_cell(&c, t, -1, 0, cell, sizeof cell) == 4 && strcmp(cell, "word") == 0, "the header after the BOM: '%s'", cell);
    CHECK(dsg_csv_cell(&c, t, 0, 0, cell, sizeof cell) >= 0 && strcmp(cell, "a, \"b\"") == 0, "a quoted cell: '%s'", cell);
    CHECK(dsg_csv_cell(&c, t, 1, 1, cell, sizeof cell) == 1 && strcmp(cell, "2") == 0, "the last cell: '%s'", cell);
    CHECK(dsg_csv_cell(&c, t, 2, 0, cell, sizeof cell) == -1, "past the last row");
    CHECK(dsg_csv_index(&c, "a,b\n\"\",\"\"\n1,2", 13, ',', err, sizeof err) == 0 && c.n_rows == 1 && !c.eol_at_end,
          "a record of quoted empty fields is skipped too, as ysp/table.h does: %d rows", c.n_rows);
    CHECK(dsg_csv_index(&c, "a,b\n1\n", 6, ',', err, sizeof err) == -1 && strstr(err, "line 2"), "a short record: '%s'", err);
    CHECK(dsg_csv_index(&c, "a,b\n,,\n", 7, ',', err, sizeof err) == -1 && strstr(err, "line 2"),
          "an empty record longer than the header, which ysp/table.h refuses: '%s'", err);
    CHECK(dsg_csv_index(&c, "a\nx\"y\n", 6, ',', err, sizeof err) == -1 && strstr(err, "quote"), "a quote inside: '%s'", err);
    CHECK(dsg_csv_index(&c, "a\n\"x\n", 5, ',', err, sizeof err) == -1 && strstr(err, "not closed"), "an open quote: '%s'", err);
    dsg_csv_free(&c);
}

/* --- 2. the index against ysp/table.h -------------------------------------- */

static void test_against_ytb(void) {
    static const char* const files[] = {
        "tests/pack/corpus/conditions.csv", "tests/pack/fixtures/small/conditions.csv", "tests/player/oddball/oddball.csv",
        "tests/player/stroop/stroop.csv", "tests/player/rdk/conditions.csv", "tests/player/visuomotor/targets.csv",
        "examples/pack/trial_images.csv", "examples/pack/stroop/stroop.csv", "examples/pack/emoji/emoji.csv",
        "tests/compare/serial_bench_py/results/2026-10-12-teensy40-linux/run1/summary.csv",
        "tests/compare/serial_bench_py/results/2026-10-12-teensy40/run1/summary.csv", NULL };
    static const struct { const char* t; char delim; } made[] = {
        { "a,b\r\n\"x,\"\"y\"\"\nz\",2\r\n,\r\n3,4", ',' },
        { "\xEF\xBB\xBFname;n\n\"p;q\";1\n;\n\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7;2\n", ';' },
        { "a\tb\n1\t \n\t\n", '\t' },
        { "x\n\"\"\"\"\n\n7\n", ',' },
        { "only\n", ',' },
    };
    char path[DSG_PATH_MAX], why[512];
    int i, n_files = 0;
    for (i = 0; files[i]; i++) {
        size_t n = 0;
        char* b;
        snprintf(path, sizeof path, "%s/%s", YSP_SOURCE_DIR, files[i]);
        b = read_all(path, &n);
        CHECK(b != NULL, "read %s", files[i]);
        if (!b) continue;
        n_files++;
        CHECK(same_as_ytb(b, n, ',', why, sizeof why), "%s: %s", files[i], why);
        free(b);
    }
    for (i = 0; i < (int)(sizeof made / sizeof made[0]); i++)
        CHECK(same_as_ytb(made[i].t, strlen(made[i].t), made[i].delim, why, sizeof why), "made-up table %d: %s", i, why);
    CHECK(n_files == 11, "every CSV read: %d", n_files);
}

/* --- 3. edits ----------------------------------------------------------- */

typedef struct fixture { dsg_doc* d; int buf; dsg_csv c; } fixture;

static void fx_open(fixture* f, const char* bytes, char delim) {
    memset(f, 0, sizeof *f);
    f->c.buf = -1;
    f->d = dsg_doc_new();
    dsg_doc_reset(f->d, "experiment.json", "{}\n", 3);
    f->buf = dsg_doc_add_buffer(f->d, DSG_KIND_TABLE, "data/t.csv", bytes, strlen(bytes));
    dsg_csv_sync(&f->c, f->d, f->buf, delim);
}

static void fx_close(fixture* f) {
    dsg_csv_free(&f->c);
    dsg_doc_free(f->d);
}

static const char* fx_bytes(fixture* f) {
    size_t n = 0;
    return dsg_doc_bytes(f->d, f->buf, &n);
}

/* After an edit: one more step, the bytes as want, ytb reads them as the
 * index does, undo gives before back, redo gives want. */
static void after_edit(fixture* f, int rc, int depth0, const char* before, const char* want, const char* what) {
    char why[512];
    const char* now = fx_bytes(f);
    CHECK(rc == 0, "%s: %s", what, f->c.error);
    CHECK(dsg_doc_undo_depth(f->d) == depth0 + 1, "%s: one step (%d to %d)", what, depth0, dsg_doc_undo_depth(f->d));
    CHECK(strcmp(now, want) == 0, "%s: the bytes\n  are  '%s'\n  want '%s'", what, now, want);
    CHECK(same_as_ytb(now, strlen(now), f->c.delim, why, sizeof why), "%s: ytb reads it as the index does: %s", what, why);
    CHECK(dsg_doc_undo(f->d) == 0 && strcmp(fx_bytes(f), before) == 0, "%s: undo gives the old bytes", what);
    CHECK(dsg_doc_redo(f->d) == 0 && strcmp(fx_bytes(f), want) == 0, "%s: redo gives the new bytes", what);
    dsg_csv_sync(&f->c, f->d, f->buf, f->c.delim);
}

static void edit_case(const char* before, char delim, const char* want, const char* what, int op, int a, int b, const char* text) {
    fixture f;
    int rc = -1, depth0;
    fx_open(&f, before, delim);
    depth0 = dsg_doc_undo_depth(f.d);
    switch (op) {
    case 0: rc = dsg_csv_set_cell(f.d, &f.c, a, b, text, strlen(text)); break;
    case 1: rc = dsg_csv_insert_row(f.d, &f.c, a, b); break;
    case 2: rc = dsg_csv_remove_row(f.d, &f.c, a); break;
    case 3: rc = dsg_csv_insert_column(f.d, &f.c, a, text); break;
    case 4: rc = dsg_csv_remove_column(f.d, &f.c, a); break;
    case 5: rc = dsg_csv_rename_column(f.d, &f.c, a, text); break;
    default: break;
    }
    after_edit(&f, rc, depth0, before, want, what);
    fx_close(&f);
}

static void test_edits(void) {
    static const char lf[] = "a,b\n1,x\n2,y\n";
    static const char crlf[] = "a,b\r\n1,x\r\n2,y\r\n";
    static const char open_end[] = "a,b\n1,x\n2,y";
    static const char blank[] = "a,b,c\n1,2,3\n,,\n4,5,6\n,,\n";
    fixture f;
    char cell[64];
    int depth0;

    /* cells */
    edit_case(lf, ',', "a,b\n1,x\n2,zz\n", "a cell", 0, 1, 1, "zz");
    edit_case(lf, ',', "a,b\n1,\"p,q\"\n2,y\n", "a cell with the delimiter is quoted", 0, 0, 1, "p,q");
    edit_case(lf, ',', "a,b\n1,\"say \"\"hi\"\"\"\n2,y\n", "a cell with quotes: quoted, each quote doubled", 0, 0, 1, "say \"hi\"");
    edit_case(lf, ',', "a,b\n1,\"two\nlines\"\n2,y\n", "a cell with a line end is quoted", 0, 0, 1, "two\nlines");
    edit_case("a;b\n1;x\n", ';', "a;b\n1;p,q\n", "a comma in a ';' table is not quoted", 0, 0, 1, "p,q");
    edit_case("a;b\n1;x\n", ';', "a;b\n1;\"p;q\"\n", "a ';' in a ';' table is quoted", 0, 0, 1, "p;q");
    edit_case(crlf, ',', "a,b\r\n1,\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\r\n2,y\r\n", "Arabic in a CRLF table", 0, 0, 1,
              "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
    edit_case("a,b\n\"1\",x\n", ',', "a,b\n\"1\",x2\n", "a quoted cell beside stays as written", 0, 0, 1, "x2");
    /* rows */
    edit_case(lf, ',', "a,b\n1,x\n1,x\n2,y\n", "a row copied below", 1, 1, 0, NULL);
    edit_case(lf, ',', "a,b\n1,x\n2,y\n2,y\n", "a row added at the end", 1, 2, 1, NULL);
    edit_case(crlf, ',', "a,b\r\n2,y\r\n1,x\r\n2,y\r\n", "a row in a CRLF table", 1, 0, 1, NULL);
    edit_case(open_end, ',', "a,b\n1,x\n2,y\n2,y", "a row at the end of a file with no final line end", 1, 2, 1, NULL);
    edit_case("a,b\n", ',', "a,b\n0,\n", "a row in a table with none", 1, 0, -1, NULL);
    edit_case(lf, ',', "a,b\n2,y\n", "a row removed", 2, 0, 0, NULL);
    edit_case(crlf, ',', "a,b\r\n1,x\r\n", "the last row removed (CRLF)", 2, 1, 0, NULL);
    edit_case(open_end, ',', "a,b\n1,x", "the last row of a file with no final line end", 2, 1, 0, NULL);
    edit_case(blank, ',', "a,b,c\n1,2,3\n,,\n,,\n", "a row between skipped records", 2, 1, 0, NULL);
    /* columns */
    edit_case(lf, ',', "n,a,b\n,1,x\n,2,y\n", "a column first", 3, 0, 0, "n");
    edit_case(lf, ',', "a,n,b\n1,,x\n2,,y\n", "a column between", 3, 1, 0, "n");
    edit_case(crlf, ',', "a,b,column_3\r\n1,x,\r\n2,y,\r\n", "a column last, named for you (CRLF)", 3, 2, 0, NULL);
    edit_case("", ',', "column_1\n", "a column in an empty file", 3, 0, 0, NULL);
    edit_case(lf, ',', "b\nx\ny\n", "the first column removed", 4, 0, 0, NULL);
    edit_case(crlf, ',', "a\r\n1\r\n2\r\n", "the last column removed (CRLF)", 4, 1, 0, NULL);
    edit_case("a,b,c\n1,\"p,q\",3\n", ',', "a,c\n1,3\n", "a quoted column removed", 4, 1, 0, NULL);
    edit_case(blank, ',', "a,c\n1,3\n4,6\n", "a column removed: the skipped records go, as they would be too long", 4, 1, 0, NULL);
    edit_case(lf, ',', "a,c\n1,x\n2,y\n", "a column renamed", 5, 1, 0, "c");

    /* refusals: no step, no change */
    fx_open(&f, lf, ',');
    depth0 = dsg_doc_undo_depth(f.d);
    CHECK(dsg_csv_remove_column(f.d, &f.c, 5) == -1, "no column 6");
    CHECK(dsg_csv_rename_column(f.d, &f.c, 0, "b") == -1 && strstr(f.c.error, "has a column"), "a taken name: %s", f.c.error);
    CHECK(dsg_csv_rename_column(f.d, &f.c, 0, "1x") == -1 && strstr(f.c.error, "not a column name"), "a bad name: %s", f.c.error);
    CHECK(dsg_csv_insert_column(f.d, &f.c, 0, "a b") == -1, "a name with a space");
    CHECK(dsg_csv_set_cell(f.d, &f.c, 0, 1, "x", 1) == 0 && dsg_doc_undo_depth(f.d) == depth0, "the same text: no step");
    CHECK(dsg_doc_undo_depth(f.d) == depth0 && strcmp(fx_bytes(&f), lf) == 0, "the refusals changed nothing");
    /* an edit with a stale index is refused, not spliced at old offsets */
    dsg_doc_splice(f.d, f.buf, 0, 0, "q,r\n", 4);
    CHECK(dsg_csv_set_cell(f.d, &f.c, 0, 0, "z", 1) == -1 && strstr(f.c.error, "not of its bytes"), "a stale index: %s", f.c.error);
    dsg_csv_sync(&f.c, f.d, f.buf, ',');
    CHECK(dsg_csv_cell(&f.c, fx_bytes(&f), -1, 0, cell, sizeof cell) == 1 && cell[0] == 'q', "sync after a change: '%s'", cell);
    fx_close(&f);
    fx_open(&f, "a\n1\n", ',');
    CHECK(dsg_csv_remove_column(f.d, &f.c, 0) == -1 && strstr(f.c.error, "only"), "the only column: %s", f.c.error);
    fx_close(&f);

    /* the selection is part of the step */
    fx_open(&f, lf, ',');
    CHECK(dsg_csv_set_cell(f.d, &f.c, 1, 0, "9", 1) == 0, "edit for the selection");
    CHECK(dsg_doc_selection(f.d)->buf == f.buf && dsg_doc_selection(f.d)->row == 1 && dsg_doc_selection(f.d)->col == 0,
          "the edited cell is selected");
    fx_close(&f);
}

/* --- 4. renames ----------------------------------------------------------- */

static char heard[256];
static int n_heard;
static void on_rename_heard(void* ctx, const char* scope, const char* from, const char* to) {
    (void)ctx;
    n_heard++;
    snprintf(heard, sizeof heard, "%s %s>%s", scope, from, to);
}

static void test_rename(void) {
    fixture f;
    fx_open(&f, "a,b\n1,2\n", ',');
    dsg_doc_listen(f.d, on_rename_heard, NULL);
    CHECK(dsg_csv_rename_column(f.d, &f.c, 1, "speed") == 0, "rename: %s", f.c.error);
    CHECK(n_heard == 1 && strcmp(heard, "#data/t.csv/columns b>speed") == 0, "the rename's scope: %d '%s'", n_heard, heard);
    dsg_doc_undo(f.d);
    CHECK(n_heard == 2 && strcmp(heard, "#data/t.csv/columns speed>b") == 0, "undo renames back: '%s'", heard);
    dsg_doc_redo(f.d);
    CHECK(n_heard == 3 && strcmp(heard, "#data/t.csv/columns b>speed") == 0, "redo: '%s'", heard);
    fx_close(&f);
}

/* --- 5. places ------------------------------------------------------------ */

static void test_places(void) {
    static const char bad[] = "a,b\n1,2\n3,\xFF\n";
    ref r = { 0 };
    char entry[64], name[64], msg[512];
    int row = 9, col = 9;
    CHECK(!ref_open(&r, bad, sizeof bad - 1, ','), "ytb refuses a byte that is not UTF-8");
    snprintf(msg, sizeof msg, "the check pack: ypak: 't.pstb': %s", ytb_error(&r.t));
    ref_close(&r);
    CHECK(dsg_csv_place(msg, entry, sizeof entry, &row, &col, name, sizeof name) && strcmp(entry, "t.pstb") == 0 && row == 1 &&
              col == 1 && (!name[0] || strcmp(name, "b") == 0),
          "'%s': %s row %d col %d '%s'", msg, entry, row, col, name);
    CHECK(dsg_csv_place("ypak: 'x.pstb': ysp_table: line 1, header, column 2: column name '1a' is not a name", entry, sizeof entry,
                        &row, &col, name, sizeof name) && row == -1 && col == 1 && name[0] == 0,
          "a header cell: row %d col %d", row, col);
    CHECK(dsg_csv_place("ypak: 'x.pstb': ysp_table: line 5, row 4: row has 3 fields, the header has 2", entry, sizeof entry, &row,
                        &col, name, sizeof name) && row == 3 && col == -1,
          "a row: row %d col %d", row, col);
    CHECK(dsg_csv_place("'odd.pstb': column 'type' has no level 3", entry, sizeof entry, &row, &col, name, sizeof name) &&
              strcmp(entry, "odd.pstb") == 0 && row == -2 && col == -1 && strcmp(name, "type") == 0,
          "a column by name: %d %d '%s'", row, col, name);
    CHECK(!dsg_csv_place("stimulus 'dots': 'coherence' has no value", entry, sizeof entry, &row, &col, name, sizeof name),
          "a message about no table");
}

/* --- 6. the check with an unsaved table ----------------------------------- */

static int copy_file(const char* from, const char* to) {
    char* d;
    size_t n = 0;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

static int count_errors_with(const dsg_report* r, const char* text) {
    int i, k = 0;
    for (i = 0; i < r->n_issues; i++)
        if (r->issues[i].severity == DSG_SEV_ERROR && strstr(r->issues[i].message, text)) k++;
    return k;
}

static void print_issues(const dsg_report* r) {
    int i;
    for (i = 0; i < r->n_issues; i++) printf("    issue %d (sev %d): %s\n", i, r->issues[i].severity, r->issues[i].message);
}

static void test_check(void) {
    static const char* const files[] = { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", NULL };
    char dir[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[1024], entry[64], name[64];
    dsg_project p;
    dsg_checker* ck;
    dsg_report rep;
    fixture f;
    char* disk0;
    char* disk1;
    size_t n0 = 0, n1 = 0;
    int i, row = 0, col = 0, k;
    snprintf(dir, sizeof dir, "%s/oddball", work);
    CHECK(dsg_make_dir(dir, err, sizeof err) == 0, "%s", err);
    for (i = 0; files[i]; i++) {
        snprintf(a, sizeof a, "%s/tests/player/oddball/%s", YSP_SOURCE_DIR, files[i]);
        dsg_join(b, sizeof b, dir, files[i]);
        CHECK(copy_file(a, b) == 0, "copy %s", files[i]);
    }
    dsg_join(b, sizeof b, dir, DSG_CHECK_PACK);
    remove(b);
    CHECK(dsg_project_open(&p, dir, err, sizeof err) == 0, "open: %s", err);
    ck = dsg_checker_new();
    memset(&f, 0, sizeof f);
    f.c.buf = -1;
    f.d = dsg_doc_new();
    dsg_doc_reset(f.d, p.def_source, p.text, p.text_len);
    dsg_join(a, sizeof a, dir, "oddball.csv");
    disk0 = read_all(a, &n0);
    f.buf = dsg_doc_add_buffer(f.d, DSG_KIND_TABLE, "oddball.csv", disk0, n0);
    dsg_check_use_tables(f.d);

    dsg_check(ck, &p, p.text, p.text_len, &rep);
    CHECK(rep.built && rep.n_errors == 0, "oddball checks clean: built %d, %d errors", rep.built, rep.n_errors);
    if (rep.n_errors) print_issues(&rep);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    CHECK(!rep.built, "no table changed: no build");

    /* An edit the definition takes: the pack is built from the document. */
    dsg_csv_sync(&f.c, f.d, f.buf, ',');
    CHECK(dsg_csv_set_cell(f.d, &f.c, 1, 2, "3", 1) == 0, "edit: %s", f.c.error);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    CHECK(rep.built && rep.n_errors == 0, "an unsaved table: built %d, %d errors", rep.built, rep.n_errors);
    dsg_doc_undo(f.d);

    /* The rule "max_run type=target 1" names the level 'target' of column
     * 'type'. Its only row becomes "standard", in the document only. */
    dsg_csv_sync(&f.c, f.d, f.buf, ',');
    CHECK(dsg_csv_set_cell(f.d, &f.c, 1, 1, "standard", 8) == 0, "edit: %s", f.c.error);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    k = count_errors_with(&rep, "no level 'target'");
    CHECK(k == 1, "the unsaved table is checked: %d errors name the level 'target'", k);
    if (k != 1) print_issues(&rep);
    disk1 = read_all(a, &n1);
    CHECK(disk1 && n1 == n0 && memcmp(disk0, disk1, n0) == 0, "the file on disk is as it was");
    free(disk1);

    /* the pack of the clean table is still the one built last */
    dsg_doc_undo(f.d);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    CHECK(rep.n_errors == 0, "undo: checked clean again (%d errors)", rep.n_errors);
    if (rep.n_errors) print_issues(&rep);

    /* A byte that is not UTF-8: the pack tool refuses the cell, and the
     * message names its row and column. */
    dsg_csv_sync(&f.c, f.d, f.buf, ',');
    CHECK(dsg_csv_set_cell(f.d, &f.c, 2, 3, "l\xFF", 2) == 0, "edit: %s", f.c.error);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    k = -1;
    for (i = 0; i < rep.n_issues; i++)
        if (rep.issues[i].from == DSG_FROM_PACK && dsg_csv_place(rep.issues[i].message, entry, sizeof entry, &row, &col, name, sizeof name)) k = i;
    CHECK(k >= 0 && strcmp(entry, "oddball.pstb") == 0 && row == 2 && col == 3 && (!name[0] || strcmp(name, "answer") == 0),
          "the refused cell: issue %d, '%s' row %d col %d '%s'", k, entry, row, col, name);
    if (k < 0) print_issues(&rep);
    CHECK(k < 0 || rep.issues[k].place.line == 0, "a table's place is not a place in the definition");

    /* Saved: the pack comes from the file again, with no new build. */
    dsg_doc_undo(f.d);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    CHECK(rep.n_errors == 0, "undo again: clean");
    dsg_check_use_tables(NULL);
    dsg_check(ck, &p, p.text, p.text_len, &rep);
    CHECK(!rep.built && rep.n_errors == 0, "without the document: the files on disk, which the pack has (built %d)", rep.built);

    free(disk0);
    dsg_checker_free(ck);
    dsg_project_close(&p);
    fx_close(&f);
}

int main(int argc, char** argv) {
    char err[512];
    if (argc < 2) {
        printf("usage: test_designer_table WORK\n");
        return 2;
    }
    work = argv[1];
    if (dsg_make_dir(work, err, sizeof err)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    test_index();
    test_against_ytb();
    test_edits();
    test_rename();
    test_places();
    test_check();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
