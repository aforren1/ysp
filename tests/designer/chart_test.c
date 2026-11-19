/* chart_test.c - the state chart's model and commands with no window
 * (designer/core/chart.h; step 4, part A). OWNED BY PART A.
 *
 * Usage: test_designer_chart
 *   A stub until part A builds the chart: the model refuses, and the
 *   document is untouched by a refused command.
 * Exit 0 on pass, 1 on a failure.
 */
#include "chart.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const char def[] = "{\"trials\": {\"t\": {\"states\": [{\"name\": \"a\"}]}}}";
    dsg_doc* d = dsg_doc_new();
    dsg_chart c;
    if (!d) return 1;
    dsg_doc_reset(d, "experiment.json", def, sizeof def - 1);
    CHECK(dsg_chart_build(dsg_doc_root(d), "nope", &c) == -1, "no such template");
    CHECK(dsg_chart_add_state(d, "t", "b") == -1 || dsg_doc_undo_depth(d) == 1, "a command is one step or none");
    dsg_chart_free(&c);
    dsg_doc_free(d);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
