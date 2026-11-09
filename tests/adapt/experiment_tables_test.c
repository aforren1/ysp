/* experiment_tables_test.c - the format's table of keys and defaults
 * (ysp/experiment.h, D2) against the parameter tables of the headers it
 * was taken from. It maps every table name to its definition key (rig_spec
 * section 11, step 3) and compares the defaults row by row. The format's
 * rows stay as they are when a header changes (D2), so a failure here means
 * a header's default moved: the lowering must then write the format's value
 * into that desc field. List the row in `moved[]` once that is done.
 *
 * Needs the headers' parameter tables, not their devices: built without
 * SDL, miniaudio and pl_mpeg, as param_tables_test.c is.
 *
 *     gcc -std=c11 -Wall -Wextra -Iinclude -o t tests/adapt/experiment_tables_test.c -lm -pthread -ldl
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG
#endif
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
#ifdef hyper
#undef hyper
#endif
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define FAIL(...) do { g_failures++; fprintf(stderr, "experiment_tables_test: FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

/* Format stimulus kinds and the gfx kinds they use. */
static unsigned gfx_mask(unsigned kinds) {
    /* YGFX_SHAPE=1, GRATING=2, GABOR=3, DOTS=4, IMAGE=5, NOISE=6, TEXT=7 */
    unsigned out = 0;
    if (kinds & (1u << YGFX_SHAPE)) out |= 1u << YEXP_SHAPE;
    if (kinds & (1u << YGFX_GRATING)) out |= 1u << YEXP_GRATING;
    if (kinds & (1u << YGFX_GABOR)) out |= 1u << YEXP_GABOR;
    if (kinds & (1u << YGFX_NOISE)) out |= 1u << YEXP_NOISE;
    if (kinds & (1u << YGFX_IMAGE)) out |= (1u << YEXP_IMAGE) | (1u << YEXP_VIDEO);
    if (kinds & (1u << YGFX_TEXT)) out |= (1u << YEXP_TEXT) | (1u << YEXP_ARTWORK);
    if (kinds & (1u << YGFX_DOTS)) out |= 1u << YEXP_RDK;
    return out;
}

static const char* const g_section_names[] = { "file", "display", "session", "session field", "web", "role", "stimulus", "stair method", "quest method", "aep method", "template", "response", "response trigger", "choice", "node", "loop", "step" };

static int sect_id(const char* name) {
    int i;
    for (i = 0; i < 17; i++)
        if (!strcmp(g_section_names[i], name)) return i;
    return -1;
}

/* A row of the format's table, or NULL. */
static const yexp_row* row_get(int section, unsigned mask, const char* key) {
    int n, i;
    const yexp_row* rows = yexp_defaults(1, &n);
    for (i = 0; i < n; i++)
        if (rows[i].section == section && (rows[i].kinds & mask) && !strcmp(rows[i].key, key)) return &rows[i];
    return NULL;
}

/* A header's default against a format row's text. */
static void cmp_default(const char* table, const char* name, double hd, const char* values, const yexp_row* r) {
    double fd;
    if (!r->def) return;   /* required in the format; the header's value is its zero */
    g_checks++;
    if (r->def[0] == '[' || r->def[0] == '{') return;   /* an array's own emptiness */
    if (values) {
        /* the enum's default: the format holds its name */
        char want[64];
        const char* p = values;
        int found = 0;
        while (p && *p) {
            const char* eq = strchr(p, '=');
            const char* e = eq ? strchr(eq, ',') : NULL;
            if (eq && atof(eq + 1) == hd) {
                snprintf(want, sizeof want, "\"%.*s\"", (int)(eq - p), p);
                found = 1;
                break;
            }
            p = e ? e + 1 : NULL;
        }
        if (!found) { FAIL("%s.%s: the default %g names no value", table, name, hd); return; }
        if (strcmp(want, r->def) != 0) FAIL("%s.%s: the default is %s; the format has %s", table, name, want, r->def);
        return;
    }
    if (r->def[0] == 't' || r->def[0] == 'f') {
        if ((hd != 0) != (r->def[0] == 't')) FAIL("%s.%s: the default is %g; the format has %s", table, name, hd, r->def);
        return;
    }
    fd = atof(r->def);
    if (fd != hd) FAIL("%s.%s: the default is %.17g; the format has %s", table, name, hd, r->def);
}

/* --- gfx --------------------------------------------------------------- */

static void check_gfx(void) {
    int n, i, bit;
    int st = sect_id("stimulus");
    const ygfx_param* p = ygfx_params(&n);
    for (i = 0; i < n; i++) {
        const char* name = p[i].name;
        unsigned mask = gfx_mask(p[i].kinds);
        if (!mask) continue;
        if (!strncmp(name, "color_", 6)) {
            const yexp_row* r = row_get(st, mask & (0u - mask), "color");
            g_checks++;
            if (!r) FAIL("color: no format row");
            if (r && p[i].def != 0) FAIL("color: a header default of %g", (double)p[i].def);
            continue;
        }
        for (bit = 0; bit < 10; bit++) {
            const yexp_row* r;
            if (!(mask & (1u << bit))) continue;
            /* text's 'size' is the format's layout key: required, literal */
            if (!strcmp(name, "size") && bit == YEXP_TEXT) continue;
            r = row_get(st, 1u << bit, name);
            g_checks++;
            if (!r) { FAIL("gfx row %s for kind %d: no format row", name, bit); continue; }
            cmp_default("ygfx_params", name, p[i].def, NULL, r);
        }
    }
    p = ygfx_enum_params(&n);
    for (i = 0; i < n; i++) {
        const char* name = p[i].name;
        unsigned mask = gfx_mask(p[i].kinds);
        for (bit = 0; bit < 10; bit++) {
            const yexp_row* r;
            if (!(mask & (1u << bit))) continue;
            /* rdk's aperture and edge are yrdk's: below */
            if (bit == YEXP_RDK && (!strcmp(name, "aperture") || !strcmp(name, "edge"))) continue;
            r = row_get(st, 1u << bit, name);
            g_checks++;
            if (!r) { FAIL("gfx enum %s for kind %d: no format row", name, bit); continue; }
            cmp_default("ygfx_enum_params", name, p[i].def, p[i].values, r);
        }
    }
}

/* --- rdk --------------------------------------------------------------- */

static void check_rdk(void) {
    int n, i;
    int st = sect_id("stimulus");
    const yrdk_param* p = yrdk_params(&n);
    for (i = 0; i < n; i++) {
        const yexp_row* r = row_get(st, 1u << YEXP_RDK, p[i].name);
        g_checks++;
        if (!r) { FAIL("rdk row %s: no format row", p[i].name); continue; }
        cmp_default("yrdk_params", p[i].name, p[i].def, NULL, r);
    }
    {
        /* desc fields that no table lists: the zero of the struct */
        static const char* const zero[] = { "aperture", "edge", "signal", "select", "noise", "clock", "algorithm", "count", "density", "lifetime_frames", "lifetime_s", "frame_s", NULL };
        yrdk_desc z;
        int k;
        memset(&z, 0, sizeof z);
        for (k = 0; zero[k]; k++) {
            const yexp_row* r = row_get(st, 1u << YEXP_RDK, zero[k]);
            g_checks++;
            if (!r) { FAIL("rdk field %s: no format row", zero[k]); continue; }
            if (r->values) {
                char want[64];
                const char* eq = strchr(r->values, '=');
                snprintf(want, sizeof want, "\"%.*s\"", (int)(eq - r->values), r->values);
                if (strcmp(want, r->def)) FAIL("rdk field %s: the format's default %s is not value 0", zero[k], r->def);
            } else if (atof(r->def) != 0) {
                FAIL("rdk field %s: the format's default is %s, not 0", zero[k], r->def);
            }
        }
        (void)z;
    }
}

/* --- methods ----------------------------------------------------------- */

typedef struct pmap { const char* table_name; const char* key; } pmap;

static int skip_name(const char* name, const char* const* skip) {
    int i;
    for (i = 0; skip[i]; i++)
        if (!strcmp(name, skip[i]) || !strncmp(name, skip[i], strlen(skip[i]))) return 1;
    return 0;
}

#define CHECK_METHOD(label, TYPE, get, skip) do {                                                       \
        int n_, i_;                                                                                     \
        int sect_ = sect_id(label);                                                                     \
        const TYPE* p_ = get(&n_);                                                                      \
        for (i_ = 0; i_ < n_; i_++) {                                                                   \
            const yexp_row* r_;                                                                         \
            if (skip_name(p_[i_].name, skip)) continue;                                                  \
            r_ = row_get(sect_, 0xffffu, p_[i_].name);                                                  \
            g_checks++;                                                                                 \
            if (!r_) { FAIL("%s row %s: no format row", label, p_[i_].name); continue; }                 \
            cmp_default(label, p_[i_].name, p_[i_].def, p_[i_].values, r_);                             \
        }                                                                                               \
    } while (0)

static void check_methods(void) {
    static const char* const stair_skip[] = { "n_steps", NULL };
    static const char* const quest_skip[] = { "n_stim", "stim", "n_param", "param", NULL };
    static const char* const aep_skip[] = { "n_dims", NULL };
    CHECK_METHOD("stair method", yst_param, yst_params, stair_skip);
    CHECK_METHOD("quest method", yqst_param, yqst_params, quest_skip);
    CHECK_METHOD("aep method", yaep_param, yaep_params, aep_skip);
}

/* --- response, timeline, device ---------------------------------------- */

static void check_response(void) {
    static const struct { const char* table; const char* key; } m[] = {
        { "duration_ns", "duration_s" }, { "min_rt_ns", "minimum_valid_rt_s" },
        { "max_responses", "max_responses" }, { "allow_held_key", "allow_held_key" },
        { "wait_for_key_release", "wait_for_key_release" }
    };
    int n, i;
    size_t k;
    int sect = sect_id("response");
    const yrsp_param* p = yrsp_params(&n);
    for (k = 0; k < sizeof m / sizeof m[0]; k++) {
        const yexp_row* r = row_get(sect, 0xffffu, m[k].key);
        int found = 0;
        g_checks++;
        for (i = 0; i < n; i++)
            if (!strcmp(p[i].name, m[k].table)) {
                found = 1;
                if (r) {
                    double d = p[i].def;
                    if (!strcmp(p[i].unit, "ns")) d /= 1e9;   /* key _s, table ns */
                    cmp_default("yrsp_params", p[i].name, d, NULL, r);
                }
            }
        if (!found) FAIL("response table has no row %s", m[k].table);
        if (!r) FAIL("response: no format row %s", m[k].key);
    }
}

static void check_timeline(void) {
    int n, i;
    int step = sect_id("step");
    const ytl_param* p = ytl_op_params(YTL_OP_TO, &n);
    for (i = 0; i < n; i++) {
        const yexp_row* r = NULL;
        double d = p[i].def;
        if (!strcmp(p[i].name, "tween.duration_ns")) { r = row_get(step, 1u << YEXP_TO, "duration_s"); d /= 1e9; }
        else if (!strcmp(p[i].name, "tween.delay_ns")) { r = row_get(step, 1u << YEXP_TO, "delay_s"); d /= 1e9; }
        else if (!strcmp(p[i].name, "tween.ease")) {
            r = row_get(step, 1u << YEXP_TO, "ease");
            g_checks++;
            if (!r) { FAIL("no format row for ease"); continue; }
            if (strcmp(p[i].values, r->values)) FAIL("the ease names moved: %s against %s", p[i].values, r->values);
            cmp_default("ytl_op_params", p[i].name, p[i].def, p[i].values, r);
            continue;
        } else continue;
        g_checks++;
        if (!r) { FAIL("no format row for %s", p[i].name); continue; }
        cmp_default("ytl_op_params", p[i].name, d, NULL, r);
    }
}

/* Every format row of the stimulus and method sections has its source. */
static void check_reverse(void) {
    static const char* const stim_format_only[] = {
        "kind", "layer", "color", "font", "text", "width", "line_height", "align", "dir", "lang", "texture", "artwork", "audio", "db", "channels",
        "offset", "frames", "loops", "video", "coherence", "direction", "speed", "count", "density", "signal", "select", "noise", "algorithm",
        "sets", "clock", "lifetime_s", "lifetime_frames", "frame_s", NULL
    };
    int n, i, k;
    const yexp_row* rows = yexp_defaults(1, &n);
    int st = sect_id("stimulus");
    int total = 0;
    for (i = 0; i < n; i++) {
        const yexp_row* r = &rows[i];
        int found = 0;
        if (r->section != st) continue;
        total++;
        for (k = 0; stim_format_only[k]; k++)
            if (!strcmp(r->key, stim_format_only[k])) found = 1;
        if (!found) {
            int gn, g;
            const ygfx_param* p = ygfx_params(&gn);
            for (g = 0; g < gn; g++) if (!strcmp(p[g].name, r->key)) found = 1;
            p = ygfx_enum_params(&gn);
            for (g = 0; g < gn; g++) if (!strcmp(p[g].name, r->key)) found = 1;
            if (r->kinds & (1u << YEXP_VIDEO)) {
                const yscr_param* v = yvid_params(&gn);
                for (g = 0; g < gn; g++) if (!strcmp(v[g].name, r->key)) found = 1;
            }
        }
        g_checks++;
        if (!found) FAIL("stimulus format row '%s' has no source table", r->key);
    }
    g_checks++;
    if (total < 90) FAIL("only %d stimulus rows", total);
}

int main(void) {
    check_gfx();
    check_rdk();
    check_methods();
    check_response();
    check_timeline();
    check_reverse();
    if (g_failures) {
        fprintf(stderr, "experiment_tables_test: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("experiment_tables_test: %d checks passed\n", g_checks);
    return 0;
}
