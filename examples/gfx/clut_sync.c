/* clut_sync.c - CLUT animation synchronized to flips: ysp/gfx.h's output-stage
 * table changed every frame, against the OS gamma ramp that ysp/screen.h
 * manages on Windows.
 *
 * It does what Psychtoolbox-3's ClutAnimDemo.m and PsychTests/
 * SyncedCLUTUpdateTest.m show. PTB is MIT; its files were read for coverage,
 * no code was copied.
 *
 * Three phases, --seconds each, in a 960 x 640 window:
 *   1 ANIM    concentric rings of 16 palette indices; the CLUT (64 entries,
 *             each index a run of 4, so a half-float scene value lands
 *             between two equal entries) turns the palette by one entry per
 *             frame, so the rings move. Nothing is drawn again but the CLUT.
 *   2 CANCEL  frames alternate: A, a ramp 0..1 under a CLUT of slope 1/2; B,
 *             a ramp 0..1/2 under the identity. Both write the same codes,
 *             so the window is a steady ramp 0..1/2 when each table lands
 *             on the flip of its own image. One frame off, it flickers
 *             between 0..1 and 0..1/4 at half the refresh rate.
 *   3 OS RAMP (Windows) SetDeviceGammaRamp() once a frame through
 *             ysp/screen.h's GDI entry points. Default: the ramp read at
 *             start, set again each frame, so nothing visible changes; the
 *             call's cost and its time before the frame's predicted onset
 *             are what is measured. --os-ramp (with --fullscreen): the same
 *             cancel test through the ramp: frame A codes c with the ramp
 *             saved at start, frame B codes c + 16 with that ramp shifted
 *             down 16 entries.
 *
 * Evidence. The gfx CLUT is read by the output pass that the frame's own
 * end() records, after the scene, so the table and the image of a frame
 * reach the same back buffer and the same present; ygfx_set_lut() inside a
 * frame is refused (YGFX_ERR_ORDER, checked). Reading back the codes of
 * ANIM frames 6 and 36 and of CANCEL frames 10 and 11 (ygfx_read_output)
 * shows them; the flip records give each frame's onset and drops. The OS
 * ramp acts after the back buffer, in the display pipe: no read-back sees
 * it, ysp/screen.h does not record when the driver latches it, and a
 * dropped frame shows the next image under the old ramp. Which flip it
 * lands on needs a photodiode or the eyes on the --os-ramp test. Measured
 * here (Iris Xe, 2026-10-09, windowed and fullscreen): SetDeviceGammaRamp()
 * blocked 15.3 to 17.0 ms and returned 0.02 to 0.7 ms after a vblank, so
 * phase 3 ran at 30 Hz. A ramp set before a frame's present is most likely
 * latched at the vblank before that frame shows: one frame early.
 *
 * --sim: phases 1 and 2 on the simulated display (40 frames each): every
 * ygfx_set_lut() between frames returns YGFX_OK, the one inside a frame
 * YGFX_ERR_ORDER, and every frame has a flip record with no drop. A window
 * run also checks the codes read back. Exits 1 on a mismatch.
 *
 * Usage: gfx_clut_sync [--sim] [--fullscreen] [--seconds S] [--os-ramp]
 * Exit code: 0; 1 when something did not open or a check failed; 2 for a bad
 * argument. On Windows set YSCR_ANGLE_DIR. A crash during --os-ramp leaves
 * the shifted ramp until the next mode change or logoff.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WW 960
#define WH 640
#define NPAL 16
#define NLUT 64
#define SHIFT 16           /* --os-ramp: codes */
#define MAXF 4096          /* frames per phase kept for the statistics */
#define PI 3.14159265358979323846

enum { ANIM, CANCEL, OSRAMP, N_PHASES };
static const char* const phase_name[N_PHASES] = { "ANIM", "CANCEL", "OS RAMP" };

/* p0: 0 rings of palette indices; 1 a ramp p1 + p2 u; 2 codes p1 + floor(p2 u) + p3. */
static const char* body =
    "vec4 ysp_main(vec2 p) {\n"
    "    float u = p.x / (2.0 * ysp_size.x) + 0.5, v;\n"
    "    if (ysp_param(0) < 0.5) v = (4.0 * mod(floor(length(p) / 16.0), 16.0) + 1.5) / 63.0;\n"
    "    else if (ysp_param(0) < 1.5) v = ysp_param(1) + ysp_param(2) * u;\n"
    "    else v = (ysp_param(1) + floor(ysp_param(2) * u) + ysp_param(3)) / 255.0;\n"
    "    return vec4(v, v, v, 1.0);\n"
    "}\n";

static yscr_screen scr;
static ygfx_gfx gfx;
static float pal[3][NPAL], lut[3 * NLUT];
static const float half_lut[6] = { 0, 0.5f, 0, 0.5f, 0, 0.5f }, ident[6] = { 0, 1, 0, 1, 0, 1 };
static uint8_t px[2][WW * WH * 4];
static float scene[WW * WH * 4];
static double cost_us[N_PHASES][MAXF], lead_ms[MAXF], grid_ms[MAXF];
static int64_t planned[MAXF];

static void palette_lut(int shift) {
    int c, j;
    for (c = 0; c < 3; c++)
        for (j = 0; j < NLUT; j++) lut[c * NLUT + j] = pal[c][(j / 4 + shift) % NPAL];
}
static void summary(const char* what, const double* v, int n, const char* unit) {
    static double s[MAXF];
    int i, j;
    if (n <= 0) { printf("  %s: none\n", what); return; }
    memcpy(s, v, (size_t)n * sizeof *v);
    for (i = 1; i < n; i++) for (j = i; j > 0 && s[j] < s[j - 1]; j--) { double t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
    printf("  %s: min %.3f, median %.3f, p99 %.3f, max %.3f %s (n %d)\n", what, s[0], s[n / 2], s[(n * 99) / 100], s[n - 1], unit, n);
}
/* ANIM: every pixel's code against the palette entry its ring index has
 * after `shift` turns. The index comes from the scene value read back. */
static long check_anim(int shift) {
    long bad = 0;
    int i, c;
    if (ygfx_read_scene(&gfx, 0, 0, WW, WH, scene) < 0 || ygfx_read_output(&gfx, 0, 0, WW, WH, px[0]) < 0) return -1;
    for (i = 0; i < WW * WH; i++) {
        int k = (int)floor((scene[i * 4] * 63.0f - 1.5f) / 4.0f + 0.5f);
        for (c = 0; c < 3; c++) bad += px[0][i * 4 + c] != (int)floorf(pal[c][(k + shift) % NPAL] * 255.0f + 0.5f);
    }
    return bad;
}

#if defined(YSCR__DXGI)
static WORD ramp_saved[3][256], ramp_b[3][256];
static int ramp_ok;
static WCHAR ramp_dev[32];
static void ramp_restore(void) {
    HDC dc;
    if (!ramp_ok) return;
    dc = yscr__win.create_dc(ramp_dev, NULL, NULL, NULL);
    if (dc) { yscr__win.set_ramp(dc, ramp_saved); yscr__win.delete_dc(dc); }
    ramp_ok = 0;
}
static int ramp_open(void) {
    MONITORINFOEXW mi;
    HDC dc;
    int c, k;
    yscr__win_load();
    if (!yscr__win.get_ramp || !yscr__win.set_ramp || !yscr__win.create_dc || !yscr__win.monitor_info) return 0;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    yscr__win.monitor_info(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), (LPMONITORINFO)&mi);
    memcpy(ramp_dev, mi.szDevice, sizeof ramp_dev);
    dc = yscr__win.create_dc(ramp_dev, NULL, NULL, NULL);
    if (!dc) return 0;
    ramp_ok = yscr__win.get_ramp(dc, ramp_saved) ? 1 : 0;
    yscr__win.delete_dc(dc);
    for (c = 0; c < 3; c++)
        for (k = 0; k < 256; k++) ramp_b[c][k] = ramp_saved[c][k < 255 - SHIFT ? k + SHIFT : 255];
    if (ramp_ok) atexit(ramp_restore);
    return ramp_ok;
}
/* One set, timed; the device context is made outside the timed part. */
static int ramp_set(WORD (*r)[256], double* us) {
    HDC dc = yscr__win.create_dc(ramp_dev, NULL, NULL, NULL);
    int64_t t0;
    BOOL ok;
    if (!dc) return 0;
    t0 = yrt_real_ns();   /* a cost: the real clock */
    ok = yscr__win.set_ramp(dc, r);
    *us = (double)(yrt_real_ns() - t0) / 1e3;
    yscr__win.delete_dc(dc);
    return ok ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_pipeline_desc pd;
    ygfx_user_desc ud;
    ygfx_stim stim[4];
    float p[4][4] = { { 0, 0, 0, 0 }, { 1, 0, 1, 0 }, { 1, 0, 0.5f, 0 }, { 2, 64, 128, 0 } };
    double seconds = 2;
    int i, ph, sim = 0, fullscreen = 0, os_visible = 0, failed = 0, quit = 0, order_refused = 0;
    int n_cost[N_PHASES] = { 0, 0, 0 }, n_lead = 0, ramp_refused = 0, frames[N_PHASES] = { 0, 0, 0 };
    long anim_bad = 0, cancel_diff = 0, records = 0, drops[N_PHASES] = { 0, 0, 0 };
    int64_t first_index[N_PHASES + 1];
    char line[600];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--os-ramp")) os_visible = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else { fprintf(stderr, "usage: gfx_clut_sync [--sim] [--fullscreen] [--seconds S] [--os-ramp]\n"); return 2; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (!(seconds > 0) || (os_visible && (!fullscreen || sim))) {
        fprintf(stderr, "gfx_clut_sync: --seconds takes a number above 0; --os-ramp needs --fullscreen, because the ramp\n"
                        "  changes the whole display\n");
        return 2;
    }
    for (i = 0; i < NPAL; i++)
        for (ph = 0; ph < 3; ph++) pal[ph][i] = (float)(0.5 + 0.35 * sin(2 * PI * (i / (double)NPAL + ph / 3.0)));
    memset(&sd, 0, sizeof sd);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.window_w = WW; sd.window_h = WH;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_clut_sync: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    if (ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK) gd.cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_clut_sync: %s\n", ygfx_error(&gfx)); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    memset(&pd, 0, sizeof pd);
    pd.body = body; pd.mode = YGFX_COLOR; pd.name = "clut sync";
    memset(&ud, 0, sizeof ud);
    ud.pipe = ygfx_pipeline(&gfx, &pd);
    if (!ud.pipe.id) { fprintf(stderr, "gfx_clut_sync: %s\n", ygfx_error(&gfx)); return 1; }
    ud.w = WW; ud.h = WH; ud.n_p = 4;
    for (i = 0; i < 4; i++) { ud.p = p[i]; stim[i] = ygfx_user(&ud); }

    for (ph = 0; ph < N_PHASES && !quit; ph++) {
        yscr_frame f;
        int64_t t0 = 0, n = 0;
        first_index[ph] = -1;
#if defined(YSCR__DXGI)
        if (ph == OSRAMP && (sim || !ramp_open())) { printf("phase 3: %s\n", sim ? "not on the simulated display" : "the ramp could not be read"); break; }
#else
        if (ph == OSRAMP) { printf("phase 3: the OS ramp path is Windows only\n"); break; }
#endif
        printf("phase %d: %s\n", ph + 1, phase_name[ph]);
        while (!quit) {
            int rc = yscr_begin(&scr, &f), b = (int)(n & 1);
            int64_t t;
            if (rc != YSCR_OK) {
                if (rc != YSCR_QUIT) { fprintf(stderr, "gfx_clut_sync: %s\n", yscr_error(&scr)); failed = 1; }
                quit = 1;
                break;
            }
            if (n == 0) { t0 = f.onset; first_index[ph] = f.index; }
            for (i = 0; i < f.n_done; i++) {
                const yscr_record* r = &f.done[i];
                int k;
                records++;
                for (k = N_PHASES - 1; k >= 0; k--)
                    if (first_index[k] >= 0 && r->index >= first_index[k]) { drops[k] += r->dropped != 0; break; }
            }
            if (n < MAXF) planned[n] = f.onset;
            /* the table for this frame, between frames */
            t = yrt_real_ns();
            if (ph == ANIM) { palette_lut((int)(n % NPAL)); rc = ygfx_set_lut(&gfx, lut, NLUT); }
            else if (ph == CANCEL) rc = ygfx_set_lut(&gfx, b ? ident : half_lut, 2);
            else rc = n == 0 ? ygfx_set_lut(&gfx, ident, 2) : YGFX_OK;
            if (n < MAXF && ph != OSRAMP) cost_us[ph][n_cost[ph]++] = (double)(yrt_real_ns() - t) / 1e3;
            if (rc != YGFX_OK) { fprintf(stderr, "gfx_clut_sync: set_lut: %s\n", ygfx_error(&gfx)); failed = 1; }
#if defined(YSCR__DXGI)
            if (ph == OSRAMP && n < MAXF) {
                double us = 0;
                int64_t ret, ph_ns;
                int ok = ramp_set(os_visible && b ? ramp_b : ramp_saved, &us);
                ramp_refused += !ok;
                cost_us[ph][n_cost[ph]++] = us;
                ret = yrt_now_ns();
                ph_ns = (ret - f.onset) % f.period;
                lead_ms[n_lead] = (double)(f.onset - ret) / 1e6;
                grid_ms[n_lead++] = (double)(ph_ns < 0 ? ph_ns + f.period : ph_ns) / 1e6;
            }
#endif
            ygfx_begin(&gfx, &f);
            if (ph == ANIM && n == 0) order_refused = ygfx_set_lut(&gfx, lut, NLUT) == YGFX_ERR_ORDER;
            if (ph == OSRAMP) stim[3].p[3] = os_visible && b ? (float)SHIFT : 0;
            if (ygfx_draw(&gfx, &stim[ph == ANIM ? 0 : ph == CANCEL ? 1 + b : 3]) != YGFX_OK) {
                fprintf(stderr, "gfx_clut_sync: %s\n", ygfx_error(&gfx));
                failed = 1;
            }
            ygfx_end(&gfx);
            if (!sim && ph == ANIM && (n == 6 || n == 36)) {
                long bad = check_anim((int)(n % NPAL));
                anim_bad += bad < 0 ? 1 : bad;
            }
            if (!sim && ph == CANCEL && (n == 10 || n == 11) && ygfx_read_output(&gfx, 0, 0, WW, WH, px[n - 10]) < 0) failed = 1;
            yscr_flip(&scr);
            n++;
            frames[ph]++;
            if (sim ? n >= 40 : f.onset + f.period - t0 >= (int64_t)(seconds * 1e9)) break;
        }
    }
#if defined(YSCR__DXGI)
    ramp_restore();
#endif
    ygfx_close(&gfx);
    yscr_close(&scr);

    if (!sim && frames[CANCEL] > 11)
        for (i = 0; i < WW * WH * 4; i++) cancel_diff += (i & 3) != 3 && px[0][i] != px[1][i];
    printf("\nflip records: %ld; dropped frames: ANIM %ld, CANCEL %ld, OS RAMP %ld\n", records, drops[ANIM], drops[CANCEL], drops[OSRAMP]);
    printf("gfx CLUT: set_lut() inside a frame refused: %s\n", order_refused ? "yes" : "NO");
    summary("set_lut, 64 entries (ANIM)", cost_us[ANIM], n_cost[ANIM], "us");
    summary("set_lut, 2 entries (CANCEL)", cost_us[CANCEL], n_cost[CANCEL], "us");
    if (!sim) {
        printf("  ANIM frames 7 and 37 read back: %ld codes differ from the turned palette\n", anim_bad);
        printf("  CANCEL frames 11 (image A, CLUT A) and 12 (B, B) read back: %ld codes differ\n", cancel_diff);
        printf("  so each table reached the back buffer of its own frame, which that frame's flip presented\n");
    }
    if (frames[OSRAMP] > 0) {
        printf("OS ramp (%s): %d of %d sets refused\n", os_visible ? "the cancel test" : "the same ramp each frame", ramp_refused, frames[OSRAMP]);
        summary("SetDeviceGammaRamp", cost_us[OSRAMP], n_cost[OSRAMP], "us");
        summary("its return before the frame's predicted onset", lead_ms, n_lead, "ms");
        summary("its return after a vblank of the onset grid", grid_ms, n_lead, "ms");
        printf("  no read-back or flip record shows which scanout the ramp reached: that needs a photodiode,\n"
               "  or the eyes on --os-ramp (steady = each ramp on its own frame; flicker at half the rate = off by one)\n");
    }
    failed |= !order_refused || anim_bad != 0 || cancel_diff != 0 || ramp_refused != 0;
    failed |= drops[ANIM] + drops[CANCEL] != 0 && sim;
    failed |= frames[ANIM] == 0 || frames[CANCEL] == 0 || (sim && records < frames[ANIM] + frames[CANCEL] - 2);
    if (failed) { fprintf(stderr, "gfx_clut_sync: a check failed\n"); return 1; }
    printf("checks passed%s\n", sim ? " (--sim reads nothing back)" : "");
    return 0;
}
