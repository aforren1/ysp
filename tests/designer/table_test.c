/* table_test.c - the tables' index and edits with no window
 * (designer/core/csv.h; step 4, part B). OWNED BY PART B.
 *
 * Usage: test_designer_table
 *   1. The index of a table as ysp/table.h reads it: the header, the rows, a
 *      quoted cell with a delimiter and a quote, CRLF, a skipped empty
 *      record, a BOM.
 *   2. The dialect's refusals, each with its line.
 * Exit 0 on pass, 1 on a failure.
 */
#include "csv.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const char t[] = "\xEF\xBB\xBFword,n\r\n\"a, \"\"b\"\"\",1\r\n,\r\nc,2\r\n";
    dsg_csv c;
    char cell[64], err[256];
    memset(&c, 0, sizeof c);
    CHECK(dsg_csv_index(&c, t, sizeof t - 1, ',', err, sizeof err) == 0, "index: %s", err);
    CHECK(c.n_cols == 2 && c.n_rows == 2 && c.crlf, "2 columns, 2 rows (the empty record skipped), CRLF: %d %d", c.n_cols, c.n_rows);
    CHECK(dsg_csv_cell(&c, t, -1, 0, cell, sizeof cell) == 4 && strcmp(cell, "word") == 0, "the header after the BOM: '%s'", cell);
    CHECK(dsg_csv_cell(&c, t, 0, 0, cell, sizeof cell) >= 0 && strcmp(cell, "a, \"b\"") == 0, "a quoted cell: '%s'", cell);
    CHECK(dsg_csv_cell(&c, t, 1, 1, cell, sizeof cell) == 1 && strcmp(cell, "2") == 0, "the last cell: '%s'", cell);
    CHECK(dsg_csv_cell(&c, t, 2, 0, cell, sizeof cell) == -1, "past the last row");
    CHECK(dsg_csv_index(&c, "a,b\n1\n", 6, ',', err, sizeof err) == -1 && strstr(err, "line 2"), "a short record: '%s'", err);
    CHECK(dsg_csv_index(&c, "a\nx\"y\n", 6, ',', err, sizeof err) == -1 && strstr(err, "quote"), "a quote inside: '%s'", err);
    CHECK(dsg_csv_index(&c, "a\n\"x\n", 5, ',', err, sizeof err) == -1 && strstr(err, "not closed"), "an open quote: '%s'", err);
    dsg_csv_free(&c);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
