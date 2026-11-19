/* panels_test.c - the panels' fields with no window (designer/core/schema.h;
 * step 4, part C). OWNED BY PART C.
 *
 * Usage: test_designer_panels
 *   1. The schema has format 2's rows; every duration is in seconds.
 *   2. The type mirror of ysp/experiment.h's internal enum holds where it can
 *      be seen: every key that ends in _s is a duration, every row with enum
 *      values is an enum.
 *   3. A stimulus key that ysp/gfx.h has takes its unit and range.
 * Exit 0 on pass, 1 on a failure.
 */
#include "schema.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    int n, i, bad_dur = 0, bad_enum = 0, k;
    const dsg_field* f = dsg_schema_fields(&n);
    const dsg_field* sel[256];
    int stim = dsg_schema_section("stimulus"), exit_sec = dsg_schema_section("exit");
    CHECK(f && n > 100, "format 2's rows (%d)", n);
    CHECK(stim >= 0 && exit_sec >= 0 && dsg_schema_section("no such") == -1, "sections by name");
    for (i = 0; f && i < n; i++) {
        size_t kl = strlen(f[i].key);
        const bool dur_key = kl > 2 && strcmp(f[i].key + kl - 2, "_s") == 0;
        if (dur_key != (f[i].type == DSG_T_DUR)) { bad_dur++; printf("  duration? %s (type %d)\n", f[i].key, f[i].type); }
        if (f[i].type == DSG_T_DUR && strcmp(f[i].unit, "s") != 0) bad_dur++;
        if ((f[i].values != NULL) != (f[i].type == DSG_T_ENUM || f[i].type == DSG_T_ENUMS)) {
            bad_enum++;
            printf("  enum? %s (type %d)\n", f[i].key, f[i].type);
        }
    }
    CHECK(bad_dur == 0, "every _s key is a duration in seconds, and only those (%d not)", bad_dur);
    CHECK(bad_enum == 0, "every row with values is an enum (%d not)", bad_enum);
    k = dsg_schema_select(stim, 1u << 1 /* YEXP_GRATING */, sel, 256);
    for (i = 0; i < k && i < 256; i++)
        if (strcmp(sel[i]->key, "contrast") == 0) break;
    CHECK(i < k && strcmp(sel[i]->from, "ysp/gfx.h") == 0 && !isnan(sel[i]->max), "a grating's contrast has gfx's range");
    k = dsg_schema_select(exit_sec, 0x1, sel, 256);
    CHECK(k >= 4, "a state's exit has its keys (%d)", k);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
