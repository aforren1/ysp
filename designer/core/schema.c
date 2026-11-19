/* schema.c - the panels' fields from the format's table and the headers'
 * parameter tables (schema.h). OWNED BY PART C. */
#include "schema.h"
#include "player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static dsg_field* g_fields;
static int g_n;

/* A header's row for a stimulus key, joined by name: rdk's own fields
 * first, then gfx's (as the player's param host checks them, player/host.c). */
static void join_stimulus(dsg_field* f) {
    int n, i;
    if (f->kinds & (1u << YEXP_RDK)) {
        const yrdk_param* r = yrdk_params(&n);
        for (i = 0; i < n; i++)
            if (strcmp(r[i].name, f->key) == 0) {
                f->unit = r[i].unit;
                f->doc = r[i].doc;
                f->min = r[i].min;
                f->max = r[i].max;
                f->from = "ysp/rdk.h";
                return;
            }
    }
    {
        const ygfx_param* r = ygfx_params(&n);
        for (i = 0; i < n; i++)
            if (strcmp(r[i].name, f->key) == 0) {
                f->unit = r[i].unit;
                f->doc = r[i].doc;
                f->min = r[i].min;
                f->max = r[i].max;
                f->from = "ysp/gfx.h";
                return;
            }
    }
}

static int build(void) {
    int n, i, stim;
    const yexp_row* rows = yexp_defaults(2, &n);
    if (!rows) return -1;
    g_fields = (dsg_field*)calloc((size_t)n, sizeof *g_fields);
    if (!g_fields) return -1;
    stim = dsg_schema_section("stimulus");
    for (i = 0; i < n; i++) {
        dsg_field* f = &g_fields[i];
        f->key = rows[i].key;
        f->section = rows[i].section;
        f->kinds = rows[i].kinds;
        f->type = rows[i].type;
        f->required = yexp_row_required(&rows[i]);
        f->def = rows[i].def;
        f->values = rows[i].values;
        f->unit = f->type == DSG_T_DUR ? "s" : "";
        f->doc = "";
        f->min = f->max = NAN;
        f->from = "format";
        if (f->section == stim && f->type != DSG_T_DUR) join_stimulus(f);
    }
    g_n = n;
    return 0;
}

const dsg_field* dsg_schema_fields(int* n) {
    if (!g_fields && build()) {
        if (n) *n = 0;
        return NULL;
    }
    if (n) *n = g_n;
    return g_fields;
}

int dsg_schema_section(const char* name) {
    int i;
    for (i = 0; i < 256; i++) {
        const char* s = yexp_section_name(i);
        if (!s[0]) return -1;
        if (strcmp(s, name) == 0) return i;
    }
    return -1;
}

int dsg_schema_select(int section, unsigned kind_bit, const dsg_field** out, int cap) {
    int n, i, k = 0;
    const dsg_field* f = dsg_schema_fields(&n);
    for (i = 0; f && i < n; i++)
        if (f[i].section == section && (f[i].kinds & kind_bit)) {
            if (k < cap) out[k] = &f[i];
            k++;
        }
    return k;
}
