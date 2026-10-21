/* param_tables_test.c - every parameter table of every ysp header against
 * the one row shape (name, type, min, max, def, unit, doc, values). No
 * framework: it returns 0 when every check passed and 1 after printing
 * each failure.
 *
 * The player's loader checks a definition's keys and values against these
 * tables and the designer builds its panels from them (docs/rig_spec.md,
 * principle 8; docs/experiment_format.md, G6), so one reader must read
 * them all. The rule, for every row of every table:
 *   - type is a known type, alone or as an array ("enum[]");
 *   - an "enum" or "enum[]" row has values: "name=value" pairs joined by
 *     ',', each name [a-z][a-z0-9_]* and unique, the values increasing
 *     (ptc_values_parse() in param_table_check.h);
 *   - its def is one of the listed values, and every value is inside min
 *     and max (ysp/device.h's and ysp/response.h's kind rows keep the
 *     input kind's full range, 15, past their last value);
 *   - every other row has values NULL;
 *   - name, unit and doc are set.
 * Each header's own test checks its rows against its desc and its C enums;
 * this one checks that the tables agree with each other.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o param_tables_test tests/adapt/param_tables_test.c -lm -pthread -ldl
 * On Windows, link what the rig group links (CMakeLists.txt).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* The tables need no device: build without SDL, miniaudio and pl_mpeg. */
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG
#endif
/* video_test.c's order: Media Foundation's headers come before any other
 * Windows header the rt implementation brings in. */
#define YSP_AUDIO_IMPLEMENTATION          /* and rt */
#include "ysp/audio.h"
#define YSP_VIDEO_IMPLEMENTATION          /* and gfx, screen, color, timeline */
#include "ysp/video.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_DEVICE_IMPLEMENTATION         /* and serial, box */
#include "ysp/device.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
/* rpcndr.h (through Media Foundation's and D3D's headers) defines hyper as
 * __int64; ysp/aep.h has a field of that name and no Windows header. */
#ifdef hyper
#undef hyper
#endif
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_tables, g_rows, g_enums;
static const char* g_table = "";

static void ptc_fail(int line, const char* what) {
    fprintf(stderr, "param_tables_test [%s]: FAIL line %d: %s\n", g_table, line, what);
    g_failures++;
}
#include "param_table_check.h"

static int known_type(const char* t) {
    static const char* const base[] = { "bool", "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64",
                                        "f32", "f64", "str", "enum" };
    size_t i, n;
    if (!t) return 0;
    n = strlen(t);
    if (n > 2 && !strcmp(t + n - 2, "[]")) n -= 2;
    for (i = 0; i < sizeof base / sizeof base[0]; i++)
        if (strlen(base[i]) == n && !strncmp(base[i], t, n)) return 1;
    return 0;
}

static void check_rows(const char* table, const ptc_row* r, int n) {
    char msg[320];
    int i, j;
    g_table = table;
    g_tables++;
    if (n <= 0) { ptc_fail(__LINE__, "no rows"); return; }
    for (i = 0; i < n; i++, r++) {
        const char* name = r->name ? r->name : "(null)";
        int is_enum = r->type && (!strcmp(r->type, "enum") || !strcmp(r->type, "enum[]"));
        g_rows++;
        if (!r->name || !r->name[0] || !r->unit || !r->doc || !r->doc[0]) {
            snprintf(msg, sizeof msg, "row %d (%s): name, unit or doc missing", i, name);
            ptc_fail(__LINE__, msg);
        }
        if (!known_type(r->type)) {
            snprintf(msg, sizeof msg, "row %s: type %s", name, r->type ? r->type : "(null)");
            ptc_fail(__LINE__, msg);
        }
        if (!is_enum) {
            if (r->values) {
                snprintf(msg, sizeof msg, "row %s: a %s with values \"%s\"", name, r->type ? r->type : "(null)", r->values);
                ptc_fail(__LINE__, msg);
            }
            continue;
        }
        g_enums++;
        {
            long v[64];
            const char* why;
            int k = ptc_values_parse(r->values, v, 64, &why), def_listed = 0;
            if (k < 1) {
                snprintf(msg, sizeof msg, "row %s: values \"%s\": %s", name, r->values ? r->values : "(null)",
                         r->values ? why : "missing");
                ptc_fail(__LINE__, msg);
                continue;
            }
            if (k > 64) { snprintf(msg, sizeof msg, "row %s: more than 64 values", name); ptc_fail(__LINE__, msg); continue; }
            for (j = 0; j < k; j++) def_listed |= r->def == (double)v[j];
            if (!def_listed) {
                snprintf(msg, sizeof msg, "row %s: def %g is not one of \"%s\"", name, r->def, r->values);
                ptc_fail(__LINE__, msg);
            }
            if (r->min > (double)v[0] || r->max < (double)v[k - 1]) {
                snprintf(msg, sizeof msg, "row %s: min %g and max %g, the values run %ld to %ld", name, r->min, r->max,
                         v[0], v[k - 1]);
                ptc_fail(__LINE__, msg);
            }
        }
    }
}

/* Any header's table: copy its rows into the shared shape by field name,
 * so an extra field (offset, kinds) in between does not matter. */
static ptc_row g_buf[256];
#define CHECK_TABLE(label, p, n) do {                                   \
        int i_, n_ = (n);                                               \
        if (!(p) || n_ > 256) { g_table = (label); ptc_fail(__LINE__, "no table, or too long"); break; } \
        for (i_ = 0; i_ < n_; i_++) PTC_ROW(g_buf[i_], (p)[i_]);        \
        check_rows((label), g_buf, n_);                                 \
    } while (0)

int main(void) {
    int n, k, found;
    char label[64];
    /* the methods */
    { const yst_param* p = yst_params(&n); CHECK_TABLE("yst_params", p, n); }
    { const yqst_param* p = yqst_params(&n); CHECK_TABLE("yqst_params", p, n); }
    { const yaep_param* p = yaep_params(&n); CHECK_TABLE("yaep_params", p, n); }
    /* the timeline: every op kind that has rows, and the desc */
    for (k = 0, found = 0; k < 64; k++) {
        const ytl_param* p = ytl_op_params(k, &n);
        if (!p || n == 0) continue;
        snprintf(label, sizeof label, "ytl_op_params(%d)", k);
        CHECK_TABLE(label, p, n);
        found++;
    }
    g_table = "ytl_op_params";
    if (found != YTL_OP_THEN + 1) ptc_fail(__LINE__, "an op kind without rows");
    { const ytl_param* p = ytl_desc_params(&n); CHECK_TABLE("ytl_desc_params", p, n); }
    /* the devices: every family that has rows */
    for (k = 0, found = 0; k < 64; k++) {
        const ydev_param* p = ydev_params(k, &n);
        if (!p || n == 0) continue;
        snprintf(label, sizeof label, "ydev_params(%d)", k);
        CHECK_TABLE(label, p, n);
        found++;
    }
    g_table = "ydev_params";
    if (found < 8) ptc_fail(__LINE__, "fewer than 8 families");
    { const yrsp_param* p = yrsp_params(&n); CHECK_TABLE("yrsp_params", p, n); }
    /* the rig */
    { const yscr_param* p = yscr_params(&n); CHECK_TABLE("yscr_params", p, n); }
    { const yau_param* p = yau_params(&n); CHECK_TABLE("yau_params", p, n); }
    { const ycol_param* p = ycol_desc_params(&n); CHECK_TABLE("ycol_desc_params", p, n); }
    { const yol_param* p = yol_stroke_params(&n); CHECK_TABLE("yol_stroke_params", p, n); }
    { const yol_param* p = yol_raster_params(&n); CHECK_TABLE("yol_raster_params", p, n); }
    { const yol_param* p = yol_cset_params(&n); CHECK_TABLE("yol_cset_params", p, n); }
    { const yol_param* p = yol_glyph_params(&n); CHECK_TABLE("yol_glyph_params", p, n); }
    { const ygfx_param* p = ygfx_params(&n); CHECK_TABLE("ygfx_params", p, n); }
    { const ygfx_param* p = ygfx_group_params(&n); CHECK_TABLE("ygfx_group_params", p, n); }
    { const ygfx_param* p = ygfx_prim_params(&n); CHECK_TABLE("ygfx_prim_params", p, n); }
    { const ygfx_param* p = ygfx_fx_params(&n); CHECK_TABLE("ygfx_fx_params", p, n); }
    { const ygfx_param* p = ygfx_paint_params(&n); CHECK_TABLE("ygfx_paint_params", p, n); }
    { const ygfx_param* p = ygfx_enum_params(&n); CHECK_TABLE("ygfx_enum_params", p, n); }
    { const yscr_param* p = ygfx_desc_params(&n); CHECK_TABLE("ygfx_desc_params", p, n); }
    { const yscr_param* p = yvid_params(&n); CHECK_TABLE("yvid_params", p, n); }
    { const yrdk_param* p = yrdk_params(&n); CHECK_TABLE("yrdk_params", p, n); }
    /* the rule's own refusals */
    {
        static const char* const bad[] = { "", "a", "a=", "=0", "A=0", "0a=1", "a=0,", "a=0,,b=1", "a=1,b=0",
                                           "a=0,b=0", "a=0,a=1", "a=0;b=1", "a=x", "a b=0" };
        static const char* const good[] = { "a=0", "a=0,b=1", "x_1=3,y=7", "neg=-1,zero=0" };
        long v[8];
        const char* why;
        size_t i;
        g_table = "rule";
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
            if (ptc_values_parse(bad[i], v, 8, &why) >= 0) {
                fprintf(stderr, "param_tables_test [rule]: FAIL: \"%s\" passed\n", bad[i]);
                g_failures++;
            }
        for (i = 0; i < sizeof good / sizeof good[0]; i++)
            if (ptc_values_parse(good[i], v, 8, &why) < 1) {
                fprintf(stderr, "param_tables_test [rule]: FAIL: \"%s\" refused: %s\n", good[i], why);
                g_failures++;
            }
        if (ptc_values_parse(NULL, v, 8, &why) >= 0) { fprintf(stderr, "param_tables_test [rule]: FAIL: NULL passed\n"); g_failures++; }
    }
    if (g_failures) {
        fprintf(stderr, "param_tables_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("param_tables_test: %d tables, %d rows, %d enum rows: all checks passed\n", g_tables, g_rows, g_enums);
    return 0;
}
