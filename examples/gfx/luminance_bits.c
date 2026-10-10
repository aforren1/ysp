/* luminance_bits.c - the luminance resolution of ysp/gfx.h's output stage:
 * 8-bit rounding, the ordered dither, the noise dither, and PseudoGray
 * (bit-stealing) in a USER shader, each read back and checked code by code.
 *
 * It does what Psychtoolbox-3's PsychTutorials/
 * AdditiveBlendingForLinearSuperpositionTutorial.m (its PseudoGray output),
 * PsychTests/HighPrecisionLuminanceOutputDriversImagingPipelineTest.m and
 * the PseudoGray mode of PsychImaging show. PTB is MIT; its files were read
 * for coverage, no code was copied. PseudoGray is Tyler's bit-stealing
 * (Tyler et al. 1992; Tyler 1997): a gray step k to k + 1 split into
 * sub-steps by raising one or two guns by one code.
 *
 * Four paths, one after another (desc.dither is one per gfx, so the gfx is
 * opened again for each). Each shows two strips of 768 x 192 px in a 960 x
 * 640 window, about a pedestal of linear value 0.2:
 *   top     a shallow ramp, linear 0.195 to 0.205 (about 3 codes);
 *   bottom  a grating of 0.4 % contrast, 1/64 cycle per px (0.2 codes).
 *   1 NONE      the scene through the calibration's CLUT, rounded
 *   2 ORDERED   the same, Bayer 8 x 8 threshold
 *   3 NOISE     the same, a hashed threshold, new each frame
 *   4 PSEUDO    an identity CLUT; the USER shader looks the linear value up
 *               in a table of (r, g, b) codes sorted by luminance (all of
 *               k + {0, 1} per gun), and writes those codes
 * The luminance of a code triplet comes from ysp/color.h (DEVICE to XYZ
 * through the calibration). Per path: distinct per-pixel levels in the ramp,
 * distinct 8 x 8 block means, the RMS error of each against the ramp, and
 * the grating's shown contrast. Then the table: levels, the step at the
 * pedestal, the largest chromaticity shift of a PseudoGray level.
 *
 * Without a window (--sim) the numbers come from a CPU model of the output
 * stage (the scene as half floats, the CLUT in float, the dither formulas).
 * With a window, frame 3 of each path is read back (ygfx_read_scene,
 * ygfx_read_output): each code must equal the one the formulas give for the
 * scene value read (ties within 2e-3 of a code are skipped and counted), and
 * every PseudoGray pixel must be a table triplet, non-decreasing along the
 * ramp. Any mismatch exits 1.
 *
 * NEEDS A PHOTOMETER: every luminance and chromaticity number here comes
 * from the calibration (nominal: sRGB primaries, gamma 2.2, unless --cal).
 * Whether the panel shows 256 levels per gun (many laptop panels are 6-bit
 * with their own temporal dither), whether the guns add (PseudoGray assumes
 * it), and what the eye averages of a dither: none of it is measured here.
 * The codes are exact; the light is not checked.
 *
 * Usage: gfx_luminance_bits [--sim] [--fullscreen] [--seconds S] [--cal FILE]
 *   --seconds S  each path S seconds (default 3; --sim: 4 frames each)
 * Exit code: 0; 1 when something did not open, a draw was refused or a check
 * failed; 2 for a bad argument. On Windows set YSCR_ANGLE_DIR.
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
#define RX 96          /* both strips: x 96..863 */
#define RW 768
#define RH 192
#define RY0 64         /* ramp rows; multiples of 8, so 8 x 8 blocks align */
#define RY1 352        /* grating rows */
#define VP 0.2         /* the pedestal, linear */
#define V0 0.195
#define V1 0.205
#define GC 0.004       /* grating contrast */
#define GSF (1.0 / 64)
#define NG 16384       /* the PseudoGray texture: 4096 x 4 entries */
#define PI 3.14159265358979323846

enum { P_NONE, P_ORDERED, P_NOISE, P_PSEUDO, N_PATHS };
static const char* const path_name[N_PATHS] = { "NONE", "ORDERED", "NOISE", "PSEUDO" };

/* p0 0 ramp / 1 grating, p1 p2 the ramp's ends or the pedestal and contrast,
 * p3 sf c/px, p4 1 = look up PseudoGray codes in ysp_tex0. */
static const char* body =
    "vec4 ysp_main(vec2 p) {\n"
    "    float u = p.x / (2.0 * ysp_size.x) + 0.5;\n"
    "    float v = ysp_param(0) < 0.5 ? mix(ysp_param(1), ysp_param(2), u)\n"
    "            : ysp_param(1) * (1.0 + ysp_param(2) * cos(2.0 * ysp_PI * ysp_param(3) * p.x));\n"
    "    if (ysp_param(4) < 0.5) return vec4(v, v, v, 1.0);\n"
    "    int i = int(clamp(v, 0.0, 1.0) * 16383.0 + 0.5);\n"
    "    return vec4(texelFetch(ysp_tex0, ivec2(i & 4095, i >> 12), 0).rgb, 1.0);\n"
    "}\n";

static yscr_screen scr;
static ygfx_gfx gfx;
static ycol_cal cal;
static ycol_ctx cx;
static double ygun[3][256];              /* luminance above black per gun code, / white */
static uint8_t cand[256 * 8][3];         /* PseudoGray triplets, sorted by luminance */
static double cand_y[256 * 8];
static int n_cand, rank_of[256 * 8];     /* (k, mask) to the sorted rank, -1 none */
static uint8_t grid[NG * 4];             /* the texture: the nearest triplet per linear value */
static uint8_t seen[1 << 21];            /* a bit per 24-bit triplet */
static float scene[RW * RH * 4];
static uint8_t rgba8[RW * RH * 4];
static uint8_t code[RW * RH * 3];        /* the strip's codes: model or read back */

static double ylum(const uint8_t* c) { return ygun[0][c[0]] + ygun[1][c[1]] + ygun[2][c[2]]; }

/* Round to the nearest half float (ties to even): the RGBA16F scene. */
static float to_half(float v) {
    int e;
    double m = frexp(fabs((double)v), &e), q;
    if (m == 0) return 0;
    q = e - 1 < -14 ? ldexp(1.0, -24) : ldexp(1.0, e - 11);
    return (float)(v < 0 ? -1 : 1) * (float)(nearbyint(fabs((double)v) / q) * q);
}
/* The output stage's CLUT, as its shader computes it, in float. */
static float clut(int path, int c, float v) {
    int n = cal.lut_n ? cal.lut_n : YCOL_CAL_MAX_LUT, i0;
    float x, a, b;
    if (path == P_PSEUDO) return v;   /* identity, n = 2 */
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    x = v * (float)(n - 1);
    i0 = (int)x < n - 2 ? (int)x : n - 2;
    a = cal.lut[c][i0]; b = cal.lut[c][i0 + 1];
    return a + (b - a) * (x - (float)i0);
}
/* The threshold at window pixel (x, row): the scene's grid runs from the
 * bottom-left corner. */
static float thresh(int path, int x, int row, int64_t index) {
    int y = WH - 1 - row;
    if (path == P_ORDERED) {
        int u = x & 7, w = y & 7, z = u ^ w;
        int b = ((z & 1) << 5) | ((u & 1) << 4) | ((z & 2) << 2) | ((u & 2) << 1) | ((z & 4) >> 1) | ((u & 4) >> 2);
        return ((float)b + 0.5f) / 64.0f;
    }
    if (path == P_NOISE) {
        uint32_t h = ygfx_hash2(x, y, (uint32_t)index ^ ygfx_hash(7u));
        return ((float)(h >> 8) + 0.5f) * (1.0f / 16777216.0f);
    }
    return 0.5f;
}
static int to_code(float d, float t, int* tie) {
    float r = d * 255.0f + t, f = r - floorf(r);
    if (f < 2e-3f || f > 1 - 2e-3f) (*tie)++;
    r = floorf(r);
    return r < 0 ? 0 : r > 255 ? 255 : (int)r;
}
static double v_at(int strip, int i) {
    double px = i + 0.5 - RW / 2.0;
    return strip == 0 ? V0 + (V1 - V0) * (i + 0.5) / RW : VP * (1 + GC * cos(2 * PI * GSF * px));
}

static int pseudo_table(void) {
    int k, m, i, j, c;
    ycol_color o;
    double ytop = 0;
    for (c = 0; c < 3; c++) {
        for (k = 0; k < 256; k++) {
            ycol_color in = YCOL_DEVICE(0, 0, 0);
            in.u.v[c] = k / 255.0;
            if (ycol_convert(&cx, in, YCOL_SPACE_XYZ, &o, NULL) < 0) return 0;
            ygun[c][k] = o.u.xyz.Y;
        }
        for (k = 255; k >= 0; k--) ygun[c][k] -= ygun[c][0];
        ytop += ygun[c][255];
    }
    for (c = 0; c < 3; c++) for (k = 0; k < 256; k++) ygun[c][k] /= ytop;
    n_cand = 0;
    for (k = 0; k < 256; k++)
        for (m = 0; m < 7 && (k < 255 || m == 0); m++) {   /* m 7 is the next gray */
            cand[n_cand][0] = (uint8_t)(k + (m >> 2 & 1));
            cand[n_cand][1] = (uint8_t)(k + (m >> 1 & 1));
            cand[n_cand][2] = (uint8_t)(k + (m & 1));
            cand_y[n_cand] = ylum(cand[n_cand]);
            n_cand++;
        }
    for (i = 1; i < n_cand; i++)   /* insertion sort: nearly sorted already */
        for (j = i; j > 0 && cand_y[j] < cand_y[j - 1]; j--) {
            double t = cand_y[j]; uint8_t s[3];
            cand_y[j] = cand_y[j - 1]; cand_y[j - 1] = t;
            memcpy(s, cand[j], 3); memcpy(cand[j], cand[j - 1], 3); memcpy(cand[j - 1], s, 3);
        }
    for (i = 0; i < 256 * 8; i++) rank_of[i] = -1;
    for (i = 0; i < n_cand; i++) {
        int kk = cand[i][0] < cand[i][1] ? cand[i][0] : cand[i][1];
        kk = kk < cand[i][2] ? kk : cand[i][2];
        rank_of[kk * 8 + ((cand[i][0] - kk) << 2 | (cand[i][1] - kk) << 1 | (cand[i][2] - kk))] = i;
    }
    for (i = 0, j = 0; i < NG; i++) {   /* nearest by luminance; v is the luminance above black / white */
        double v = (double)i / (NG - 1);
        while (j + 1 < n_cand && fabs(cand_y[j + 1] - v) <= fabs(cand_y[j] - v)) j++;
        memcpy(&grid[i * 4], cand[j], 3);
        grid[i * 4 + 3] = 255;
    }
    return 1;
}
static int triplet_rank(const uint8_t* c) {
    int k = c[0] < c[1] ? c[0] : c[1];
    k = k < c[2] ? k : c[2];
    if (c[0] - k > 1 || c[1] - k > 1 || c[2] - k > 1) return -1;
    return rank_of[k * 8 + ((c[0] - k) << 2 | (c[1] - k) << 1 | (c[2] - k))];
}

/* The codes the model gives for one strip on frame `index`. */
static void model(int path, int strip, int64_t index) {
    int i, j, c, ties = 0;
    for (j = 0; j < RH; j++)
        for (i = 0; i < RW; i++) {
            uint8_t* q = &code[(j * RW + i) * 3];
            float v = (float)v_at(strip, i);
            if (path == P_PSEUDO) {
                int g = (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 16383.0f + 0.5f);
                memcpy(q, &grid[g * 4], 3);
                continue;
            }
            for (c = 0; c < 3; c++)
                q[c] = (uint8_t)to_code(clut(path, c, to_half(v)), thresh(path, RX + i, (strip ? RY1 : RY0) + j, index), &ties);
        }
}

typedef struct stats { int levels, blocks; double rms_px, rms_blk, shown; } stats;
static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
/* Ramp: distinct levels, 8 x 8 block means, errors in % of the pedestal.
 * Grating: the shown contrast at its frequency from the column means. */
static stats measure(int strip) {
    static double blk[(RW / 8) * (RH / 8)];
    stats s;
    int i, j, n = 0;
    double e2 = 0, b2 = 0, mean = 0, ac = 0, as = 0;
    memset(&s, 0, sizeof s);
    memset(seen, 0, sizeof seen);
    for (j = 0; j < RH; j++)
        for (i = 0; i < RW; i++) {
            const uint8_t* q = &code[(j * RW + i) * 3];
            uint32_t key = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
            double d = ylum(q) - v_at(strip, i);
            if (!(seen[key >> 3] & (1u << (key & 7)))) { seen[key >> 3] |= (uint8_t)(1u << (key & 7)); s.levels++; }
            e2 += d * d;
        }
    s.rms_px = sqrt(e2 / (RW * RH)) / VP * 100;
    for (j = 0; j < RH; j += 8)
        for (i = 0; i < RW; i += 8) {
            double y = 0, v = 0;
            int a, b;
            for (b = 0; b < 8; b++)
                for (a = 0; a < 8; a++) { y += ylum(&code[((j + b) * RW + i + a) * 3]); v += v_at(strip, i + a); }
            b2 += (y - v) * (y - v) / 4096;
            blk[n++] = floor(y / 64 * 1e12 + 0.5);
        }
    s.rms_blk = sqrt(b2 / n) / VP * 100;
    qsort(blk, (size_t)n, sizeof blk[0], cmp_d);
    for (i = 0; i < n; i++) s.blocks += i == 0 || blk[i] != blk[i - 1];
    for (i = 0; i < RW; i++) {
        double y = 0, px = i + 0.5 - RW / 2.0;
        for (j = 0; j < RH; j++) y += ylum(&code[(j * RW + i) * 3]);
        y /= RH;
        mean += y; ac += y * cos(2 * PI * GSF * px); as += y * sin(2 * PI * GSF * px);
    }
    mean /= RW;
    s.shown = 2 * hypot(ac, as) / RW / mean * 100;
    return s;
}

/* Frame 3's read-back against the formulas, for one strip; returns mismatches. */
static long check(int path, int strip, int64_t index, long* ties_out, long* checked) {
    int i, j, c, row0 = strip ? RY1 : RY0, last = -1, ties = 0;
    long bad = 0;
    if (ygfx_read_scene(&gfx, RX, row0, RW, RH, scene) < 0 || ygfx_read_output(&gfx, RX, row0, RW, RH, rgba8) < 0) {
        fprintf(stderr, "gfx_luminance_bits: read back: %s\n", ygfx_error(&gfx));
        return 1;
    }
    for (j = 0; j < RH; j++)
        for (i = 0; i < RW; i++) {
            const uint8_t* o = &rgba8[(j * RW + i) * 4];
            memcpy(&code[(j * RW + i) * 3], o, 3);
            if (path == P_PSEUDO) {
                int r = triplet_rank(o);
                bad += r < 0;
                if (strip == 0 && j == 0) { bad += r < last; last = r; }   /* monotone along the ramp */
            }
            for (c = 0; c < 3; c++) {
                int t0 = ties, want = to_code(clut(path, c, scene[(j * RW + i) * 4 + c]), thresh(path, RX + i, row0 + j, index), &ties);
                if (ties == t0) { bad += o[c] != want; (*checked)++; }
            }
        }
    *ties_out += ties;
    return bad;
}

static int load_cal(const char* path) {
    static unsigned char bytes[sizeof(ycol_cal)];
    char err[200];
    size_t n;
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "gfx_luminance_bits: cannot read %s\n", path); return 0; }
    n = fread(bytes, 1, sizeof bytes, fp);
    fclose(fp);
    if (ycol_cal_load(&cal, bytes, n, err, sizeof err) < 0) { fprintf(stderr, "gfx_luminance_bits: %s\n", err); return 0; }
    return 1;
}

static ygfx_stim strip_stim(ygfx_pipe pipe, int strip, int pseudo, ygfx_tex tex) {
    ygfx_user_desc d;
    float p[5];
    memset(&d, 0, sizeof d);
    p[0] = (float)strip;
    p[1] = (float)(strip ? VP : V0); p[2] = (float)(strip ? GC : V1); p[3] = (float)GSF; p[4] = (float)pseudo;
    d.pipe = pipe; d.place = YGFX_TOP_LEFT;
    d.x = RX + RW / 2.0f; d.y = (strip ? RY1 : RY0) + RH / 2.0f; d.w = RW; d.h = RH;
    d.p = p; d.n_p = 5; d.tex = tex;
    return ygfx_user(&d);
}

int main(int argc, char** argv) {
    static const float srgb_xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    yscr_desc sd;
    ycol_ctx_desc xd;
    stats st[N_PATHS][2];
    const char* cal_path = NULL;
    double seconds = 3, xy_shift = 0;
    int i, k, path, sim = 0, fullscreen = 0, failed = 0, quit = 0, levels_grid = 0, kp, read_done[N_PATHS];
    long mismatch = 0, ties = 0, checked = 0;
    char line[600];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cal") && i + 1 < argc) cal_path = argv[++i];
        else { fprintf(stderr, "usage: gfx_luminance_bits [--sim] [--fullscreen] [--seconds S] [--cal FILE]\n"); return 2; }
    }
    if (!(seconds > 0)) { fprintf(stderr, "gfx_luminance_bits: --seconds takes a number above 0\n"); return 2; }
    if (cal_path ? !load_cal(cal_path) : ycol_cal_nominal(&cal, srgb_xy, 100.0f, 2.2) < 0) return 1;
    memset(&xd, 0, sizeof xd);
    xd.cal = &cal;
    xd.background[0] = xd.background[1] = xd.background[2] = VP;
    if (!ycol_ctx_open(&cx, &xd) || !(cx.can & YCOL_CAN_XYZ)) {
        fprintf(stderr, "gfx_luminance_bits: the calibration needs chromaticities: %s\n", cx.why_no_xyz ? cx.why_no_xyz : ycol_ctx_error(&cx));
        return 1;
    }
    if (!pseudo_table()) { fprintf(stderr, "gfx_luminance_bits: a DEVICE to XYZ conversion was refused\n"); return 1; }
    ycol_cal_describe(&cal, line, sizeof line);
    printf("calibration: %s%s\n", line, cal.flags & YCOL_CAL_NOMINAL ? " (NOMINAL: no luminance below is a measurement)" : "");

    memset(&sd, 0, sizeof sd);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.window_w = WW; sd.window_h = WH;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_luminance_bits: %s\n", yscr_error(&scr)); return 1; }
    memset(st, 0, sizeof st);
    memset(read_done, 0, sizeof read_done);
    for (path = 0; path < N_PATHS && !quit; path++) {
        ygfx_desc gd;
        ygfx_pipeline_desc pd;
        ygfx_texture_desc td;
        ygfx_pipe pipe;
        ygfx_tex tex;
        ygfx_stim s[2];
        yscr_frame f;
        int64_t t0 = 0, n = 0;
        memset(&gd, 0, sizeof gd);
        gd.screen = &scr;
        gd.cal = path == P_PSEUDO ? NULL : &cal;   /* PseudoGray writes codes: identity CLUT */
        gd.dither = path == P_ORDERED ? YGFX_DITHER_ORDERED : path == P_NOISE ? YGFX_DITHER_NOISE : YGFX_DITHER_NONE;
        gd.seed = 7;
        gd.background[0] = gd.background[1] = gd.background[2] =
            path == P_PSEUDO ? grid[(int)(VP * 16383 + 0.5) * 4] / 255.0f : (float)VP;
        if (ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK) gd.cache = ygfx_file_cache_init(&pcache, cache_dir);
        if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_luminance_bits: %s\n", ygfx_error(&gfx)); return 1; }
        memset(&pd, 0, sizeof pd);
        pd.body = body; pd.mode = YGFX_COLOR; pd.name = "luminance strips";
        pipe = ygfx_pipeline(&gfx, &pd);
        memset(&td, 0, sizeof td);
        td.w = 4096; td.h = NG / 4096; td.format = YGFX_RGBA8; td.data = grid;
        memset(&tex, 0, sizeof tex);
        if (path == P_PSEUDO) tex = ygfx_texture(&gfx, &td);
        if (!pipe.id || (path == P_PSEUDO && !tex.id)) { fprintf(stderr, "gfx_luminance_bits: %s\n", ygfx_error(&gfx)); return 1; }
        for (k = 0; k < 2; k++) s[k] = strip_stim(pipe, k, path == P_PSEUDO, tex);
        printf("path %d of 4: %s\n", path + 1, path_name[path]);
        while (!quit) {
            int rc = yscr_begin(&scr, &f);
            if (rc != YSCR_OK) {
                if (rc != YSCR_QUIT) { fprintf(stderr, "gfx_luminance_bits: %s\n", yscr_error(&scr)); failed = 1; }
                quit = 1;
                break;
            }
            if (n == 0) t0 = f.onset;
            ygfx_begin(&gfx, &f);
            for (k = 0; k < 2; k++)
                if (ygfx_draw(&gfx, &s[k]) != YGFX_OK) { fprintf(stderr, "gfx_luminance_bits: %s\n", ygfx_error(&gfx)); failed = 1; }
            ygfx_end(&gfx);
            if (n == 2) {   /* frame 3: the model, then the read-back where there is one */
                for (k = 0; k < 2; k++) {
                    long before = mismatch;
                    model(path, k, f.index);
                    st[path][k] = measure(k);
                    if (!sim) {
                        mismatch += check(path, k, f.index, &ties, &checked);
                        st[path][k] = measure(k);   /* the codes read back */
                        if (mismatch > before)
                            fprintf(stderr, "gfx_luminance_bits: %s %s: %ld codes differ from the formulas\n",
                                    path_name[path], k ? "grating" : "ramp", mismatch - before);
                    }
                }
                read_done[path] = 1;
            }
            yscr_flip(&scr);
            n++;
            if (sim ? n >= 4 : f.onset + f.period - t0 >= (int64_t)(seconds * 1e9)) break;   /* after a flip: begin() needs one */
        }
        ygfx_close(&gfx);
    }
    yscr_close(&scr);
    if (quit) { printf("stopped before every path ran\n"); return failed; }

    /* the table, from the calibration */
    for (i = 0, k = -1; i < NG; i++) {
        int r = triplet_rank(&grid[i * 4]);
        levels_grid += r != k;
        k = r;
    }
    for (i = 0; i < n_cand; i++) {
        int kk = cand[i][0] < cand[i][1] ? cand[i][0] : cand[i][1];
        ycol_color a, b;
        kk = kk < cand[i][2] ? kk : cand[i][2];
        if (kk < 16 || kk == 255) continue;   /* below code 16 the chromaticity is mostly the black's */
        if (ycol_convert(&cx, YCOL_DEVICE(cand[i][0] / 255.0, cand[i][1] / 255.0, cand[i][2] / 255.0), YCOL_SPACE_XYY, &a, NULL) < 0 ||
            ycol_convert(&cx, YCOL_DEVICE(kk / 255.0, kk / 255.0, kk / 255.0), YCOL_SPACE_XYY, &b, NULL) < 0) continue;
        if (hypot(a.u.xyy.x - b.u.xyy.x, a.u.xyy.y - b.u.xyy.y) > xy_shift) xy_shift = hypot(a.u.xyy.x - b.u.xyy.x, a.u.xyy.y - b.u.xyy.y);
    }
    {
        uint32_t q[3] = { 0, 0, 0 };
        uint8_t g0[3], g1[3];
        double smin = 1, smax = 0, prev = -1;
        ycol_rgb v;
        v.r = v.g = v.b = VP;
        ycol_output_code(&cal, v, 8, q);
        kp = (int)q[1];
        g0[0] = g0[1] = g0[2] = (uint8_t)kp;
        g1[0] = g1[1] = g1[2] = (uint8_t)(kp + 1);
        for (i = 0; i < n_cand; i++)
            if (cand_y[i] >= ylum(g0) && cand_y[i] <= ylum(g1)) {
                if (prev >= 0) { double d = (cand_y[i] - prev) / VP * 100; smin = d < smin ? d : smin; smax = d > smax ? d : smax; }
                prev = cand_y[i];
            }
        printf("\nlevels from the calibration (luminance above black, %% of the pedestal %.3g):\n", VP);
        printf("  8-bit gray     256 levels (8 bits); step at code %d: %.3f%%\n", kp, (ylum(g1) - ylum(g0)) / VP * 100);
        printf("  PseudoGray     %d triplets, %d reachable through the %d-entry table (%.2f bits); steps between codes %d and %d: "
               "%.3f to %.3f%%; largest chromaticity shift (codes 16 to 254) %.4f in x, y\n",
               n_cand, levels_grid, NG, log2((double)levels_grid), kp, kp + 1, smin, smax, xy_shift);
    }
    printf("\n%s (%% of the pedestal; ramp: per-pixel levels, 8 x 8 block means; grating: %.2f%% asked)\n",
           sim ? "CPU model of the output stage" : "codes read back", GC * 100);
    printf("  path      levels  blocks   rms px   rms 8x8   grating shown\n");
    for (path = 0; path < N_PATHS; path++)
        printf("  %-8s %7d %7d %7.3f%% %8.4f%% %9.3f%%\n", path_name[path], st[path][0].levels, st[path][0].blocks,
               st[path][0].rms_px, st[path][0].rms_blk, st[path][1].shown);
    if (!sim) printf("read back: %ld codes checked, %ld differ, %ld ties skipped\n", checked, mismatch, ties);

    /* the self-check: the orderings the arithmetic predicts */
    for (path = 0; path < N_PATHS; path++) failed |= !read_done[path];
    failed |= mismatch != 0 || levels_grid <= 6 * 256;
    failed |= !(st[P_ORDERED][0].rms_blk < st[P_NONE][0].rms_blk / 4);
    failed |= !(st[P_NOISE][0].rms_blk < st[P_NONE][0].rms_blk / 2);
    failed |= !(st[P_PSEUDO][0].rms_px < st[P_NONE][0].rms_px / 3);
    failed |= !(fabs(st[P_ORDERED][1].shown - GC * 100) < 0.1 * GC * 100);
    failed |= !(fabs(st[P_PSEUDO][1].shown - GC * 100) < fabs(st[P_NONE][1].shown - GC * 100));   /* its steps are uneven */
    if (failed) { fprintf(stderr, "gfx_luminance_bits: a check failed\n"); return 1; }
    printf("checks passed%s\n", sim ? " (model only: --sim reads nothing back)" : "");
    return 0;
}
