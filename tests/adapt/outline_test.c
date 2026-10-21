/* outline_test.c - self-checking test for ysp/outline.h (exit 0 on pass).
 *
 * Needs no font file: it writes small TrueType and CFF fonts in memory.
 * When the Windows fonts named below exist (and neither --quick nor the
 * environment variable YOL_TEST_QUICK is given), it runs the same checks on
 * them and says so; when they do not, it says "skipped". ysp/gfx.h is
 * included only for ygfx_cset_check() and ygfx_cset_winding(), the
 * format's own validator (CPU only, no GL, no SDL).
 *
 * The references are independent of the header: exact winding by the roots
 * of each quadratic, coverage by adaptive Gauss-Kronrod over y of the exact
 * nonzero measure of a pixel column, area by the same integral. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSCR_NO_SDL
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
/* the sweep's work bound, lowered so the bound's test is quick */
#define YOL_SWEEP_WORK (1 << 24)
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures, g_checks;
static int g_long;   /* --long: the full counts; the default fits a sanitizer run */
static const char* g_where = "";
#define CHECK(cond) do { g_checks++; if (!(cond)) { fprintf(stderr, "outline_test [%s]: FAIL line %d: %s\n", g_where, __LINE__, #cond); g_failures++; } } while (0)
#define CHECKF(cond, ...) do { g_checks++; if (!(cond)) { fprintf(stderr, "outline_test [%s]: FAIL line %d: ", g_where, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; } } while (0)

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static double rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (double)(g_rng >> 11) / 9007199254740992.0; }
static uint32_t rndu(uint32_t n) { return (uint32_t)(rnd() * n) % (n ? n : 1); }

/* --- independent geometry ---------------------------------------------------- */

typedef struct tq { double x0, y0, x1, y1, x2, y2; } tq;
typedef struct tqs { tq* q; int n, cap; } tqs;

static void tq_push(tqs* b, double x0, double y0, double x1, double y1, double x2, double y2) {
    if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 64; b->q = (tq*)realloc(b->q, (size_t)b->cap * sizeof(tq)); }
    b->q[b->n].x0 = x0; b->q[b->n].y0 = y0; b->q[b->n].x1 = x1; b->q[b->n].y1 = y1; b->q[b->n].x2 = x2; b->q[b->n].y2 = y2;
    b->n++;
}

/* every segment of a path, open contours closed by a line, scaled */
static void path_quads(const yol_path* p, tqs* out, double s, double ox, double oy) {
    int k, i;
    out->n = 0;
    for (k = 0; k < p->n_contours; k++) {
        int f = (int)(p->contours[k] & ~YOL_OPEN), e = k + 1 < p->n_contours ? (int)(p->contours[k + 1] & ~YOL_OPEN) : p->n_pts;
        const double* q = p->pts;
        if (e - f < 3) continue;
        for (i = f; i + 2 < e; i += 2)
            tq_push(out, q[2 * i] * s + ox, q[2 * i + 1] * s + oy, q[2 * i + 2] * s + ox, q[2 * i + 3] * s + oy, q[2 * i + 4] * s + ox, q[2 * i + 5] * s + oy);
        if (q[2 * (e - 1)] != q[2 * f] || q[2 * (e - 1) + 1] != q[2 * f + 1]) {
            double ax = q[2 * (e - 1)] * s + ox, ay = q[2 * (e - 1) + 1] * s + oy;
            tq_push(out, ax, ay, ax, ay, q[2 * f] * s + ox, q[2 * f + 1] * s + oy);
        }
    }
}

/* roots in [0, 1) of a t^2 + b t + c */
static int qroots(double a, double b, double c, double* r) {
    int n = 0;
    if (fabs(a) < 1e-15 * (fabs(b) + fabs(c))) {
        if (b != 0) { double t = -c / b; if (t >= 0 && t < 1) r[n++] = t; }
        return n;
    }
    {
        double d = b * b - 4 * a * c, s, q, t1, t2;
        if (d < 0) return 0;
        s = sqrt(d); q = -0.5 * (b + (b >= 0 ? s : -s));
        t1 = q / a; t2 = q != 0 ? c / q : t1;
        if (t1 >= 0 && t1 < 1) r[n++] = t1;
        if (t2 >= 0 && t2 < 1 && t2 != t1) r[n++] = t2;
    }
    return n;
}

static double qx(const tq* q, double t) { double u = 1 - t; return u * u * q->x0 + 2 * t * u * q->x1 + t * t * q->x2; }
static double qy(const tq* q, double t) { double u = 1 - t; return u * u * q->y0 + 2 * t * u * q->y1 + t * t * q->y2; }

/* +1 inside a contour that turns clockwise on the screen (y down) */
static int winding(const tqs* b, double px, double py) {
    int i, w = 0;
    for (i = 0; i < b->n; i++) {
        const tq* q = &b->q[i];
        double r[2], a = q->y0 - 2 * q->y1 + q->y2, bb = 2 * (q->y1 - q->y0), c = q->y0 - py;
        int n = qroots(a, bb, c, r), k;
        if (fmin(q->y0, fmin(q->y1, q->y2)) > py || fmax(q->y0, fmax(q->y1, q->y2)) < py) continue;
        for (k = 0; k < n; k++) {
            double dy = 2 * a * r[k] + bb;
            if (dy == 0) continue;
            if (qx(q, r[k]) > px) w += dy > 0 ? 1 : -1;
        }
    }
    return w;
}

/* The filled measure of [c0, c1] on the line y, under a rule. */
typedef struct xc { double x; int d; } xc;
static int cmp_xc(const void* a, const void* b) { double p = ((const xc*)a)->x, q = ((const xc*)b)->x; return p < q ? -1 : p > q; }
static xc* g_xc; static int g_xccap;
static double measure(const tqs* b, double y, double c0, double c1, int evenodd) {
    int i, n = 0, w = 0, k;
    double m = 0, xs = 0;
    for (i = 0; i < b->n; i++) {
        const tq* q = &b->q[i];
        double r[2], a = q->y0 - 2 * q->y1 + q->y2, bb = 2 * (q->y1 - q->y0);
        int nr;
        if (fmin(q->y0, fmin(q->y1, q->y2)) > y || fmax(q->y0, fmax(q->y1, q->y2)) < y) continue;
        nr = qroots(a, bb, q->y0 - y, r);
        for (k = 0; k < nr; k++) {
            double dy = 2 * a * r[k] + bb;
            if (dy == 0) continue;
            if (n == g_xccap) { g_xccap = g_xccap ? 2 * g_xccap : 256; g_xc = (xc*)realloc(g_xc, (size_t)g_xccap * sizeof(xc)); }
            g_xc[n].x = qx(q, r[k]); g_xc[n].d = dy > 0 ? 1 : -1; n++;
        }
    }
    if (n > 1) qsort(g_xc, (size_t)n, sizeof(xc), cmp_xc);
    for (k = 0; k < n; k++) {
        int w2 = w + g_xc[k].d, in0 = evenodd ? (w & 1) : w != 0, in1 = evenodd ? (w2 & 1) : w2 != 0;
        if (!in0 && in1) xs = g_xc[k].x;
        else if (in0 && !in1) { double l = fmax(xs, c0), r = fmin(g_xc[k].x, c1); if (r > l) m += r - l; }
        w = w2;
    }
    return m;
}

static const tqs* g_gb; static double g_c0, g_c1; static int g_eo;
static double gk15(double a, double b, double* err) {
    static const double xgk[8] = { 0.991455371120812639206854697526329, 0.949107912342758524526189684047851, 0.864864423359769072789712788640926, 0.741531185599394439863864773280788, 0.586087235467691130294144845693013, 0.405845151377397166906606412076961, 0.207784955007898467600689403773245, 0.0 };
    static const double wgk[8] = { 0.022935322010529224963732008058970, 0.063092092629978553290700663189204, 0.104790010322250183839876322541518, 0.140653259715525918745189590510238, 0.169004726639267902826583426598550, 0.190350578064785409913256402421014, 0.204432940075298892414161999234649, 0.209482141084727828012999174891714 };
    static const double wg[4] = { 0.129484966168869693270611432679082, 0.279705391489276667901467771423780, 0.381830050505118944950369775488975, 0.417959183673469387755102040816327 };
    double c = 0.5 * (a + b), h = 0.5 * (b - a), fc = measure(g_gb, c, g_c0, g_c1, g_eo), rk = fc * wgk[7], rg = fc * wg[3];
    int j;
    for (j = 0; j < 7; j++) {
        double f1 = measure(g_gb, c - h * xgk[j], g_c0, g_c1, g_eo), f2 = measure(g_gb, c + h * xgk[j], g_c0, g_c1, g_eo);
        rk += wgk[j] * (f1 + f2);
        if (j & 1) rg += wg[j / 2] * (f1 + f2);
    }
    *err = fabs((rk - rg) * h);
    return rk * h;
}
static double adapt(double a, double b, double tol, int depth) {
    double e, v = gk15(a, b, &e);
    /* the estimate cannot go below its own rounding (a near-horizontal
     * curve turns 1e-16 in y into 1e-12 in x): a floor of 1e-10 per
     * unit height keeps the recursion from splitting rounding noise */
    if (e < tol || e < 1e-10 * (b - a) || depth > 40) return v;
    return adapt(a, 0.5 * (a + b), 0.5 * tol, depth + 1) + adapt(0.5 * (a + b), b, 0.5 * tol, depth + 1);
}
static int cmp_d(const void* a, const void* b) { double p = *(const double*)a, q = *(const double*)b; return p < q ? -1 : p > q; }

/* Crossings of two quadratics by recursive subdivision of both, down to
 * boxes below 1e-13: independent of the header's algebraic finder. Each
 * crossing's y goes to ys. A node budget stops coincident pieces. */
static void sub_quad(const double* q, double* a, double* b) {
    double mx0 = 0.5 * (q[0] + q[2]), my0 = 0.5 * (q[1] + q[3]), mx1 = 0.5 * (q[2] + q[4]), my1 = 0.5 * (q[3] + q[5]);
    double px = 0.5 * (mx0 + mx1), py = 0.5 * (my0 + my1);
    a[0] = q[0]; a[1] = q[1]; a[2] = mx0; a[3] = my0; a[4] = px; a[5] = py;
    b[0] = px; b[1] = py; b[2] = mx1; b[3] = my1; b[4] = q[4]; b[5] = q[5];
}
static int g_nodes;
static double* g_cr; static int g_ncr, g_crcap, g_cr_n; static const void* g_cr_for; static double g_cr_x0;
static void cross_rec(const double* P, const double* Q, double* ys, int* n, int cap, int depth) {
    double pl = fmin(P[0], fmin(P[2], P[4])), ph = fmax(P[0], fmax(P[2], P[4])), pb = fmin(P[1], fmin(P[3], P[5])), pt = fmax(P[1], fmax(P[3], P[5]));
    double ql = fmin(Q[0], fmin(Q[2], Q[4])), qh = fmax(Q[0], fmax(Q[2], Q[4])), qb = fmin(Q[1], fmin(Q[3], Q[5])), qt = fmax(Q[1], fmax(Q[3], Q[5]));
    if (ph < ql || qh < pl || pt < qb || qt < pb || ++g_nodes > 20000) return;
    if (depth > 46 || (ph - pl < 1e-13 && pt - pb < 1e-13 && qh - ql < 1e-13 && qt - qb < 1e-13)) {
        if (*n < cap) ys[(*n)++] = 0.25 * (pb + pt + qb + qt);
        return;
    }
    {
        double a[6], b[6], c[6], d[6];
        sub_quad(P, a, b); sub_quad(Q, c, d);
        cross_rec(a, c, ys, n, cap, depth + 1); cross_rec(a, d, ys, n, cap, depth + 1);
        cross_rec(b, c, ys, n, cap, depth + 1); cross_rec(b, d, ys, n, cap, depth + 1);
    }
}

/* Integral over [y0, y1] of the filled measure of [c0, c1]: breakpoints at
 * segment ends, y extrema, column-boundary crossings and order changes
 * (found by sampling and bisection), then adaptive Gauss-Kronrod. */
static double ref_integral(const tqs* b, double c0, double c1, double y0, double y1, int evenodd, double tol) {
    static double* br; static int brcap;
    int nb = 0, i, k;
    double s = 0;
#define ADDB(v) do { if ((v) > y0 && (v) < y1) { if (nb == brcap) { brcap = brcap ? 2 * brcap : 256; br = (double*)realloc(br, (size_t)brcap * sizeof(double)); } br[nb++] = (v); } } while (0)
    if (brcap < 2) { brcap = 256; br = (double*)realloc(br, (size_t)brcap * sizeof(double)); }
    br[nb++] = y0; br[nb++] = y1;
    for (i = 0; i < b->n; i++) {
        const tq* q = &b->q[i];
        double r[2], ty, e;
        int nr;
        ADDB(q->y0); ADDB(q->y2);
        if (q->y0 - 2 * q->y1 + q->y2 != 0) { ty = (q->y0 - q->y1) / (q->y0 - 2 * q->y1 + q->y2); if (ty > 0 && ty < 1) ADDB(qy(q, ty)); }
        /* a vertical tangent: x(y) has a square-root kink there */
        if (q->x0 - 2 * q->x1 + q->x2 != 0) { ty = (q->x0 - q->x1) / (q->x0 - 2 * q->x1 + q->x2); if (ty > 0 && ty < 1) ADDB(qy(q, ty)); }
        for (e = c0; e <= c1; e += c1 - c0) {
            nr = qroots(q->x0 - 2 * q->x1 + q->x2, 2 * (q->x1 - q->x0), q->x0 - e, r);
            for (k = 0; k < nr; k++) ADDB(qy(q, r[k]));
            if (c1 == c0) break;
        }
    }
    qsort(br, (size_t)nb, sizeof(double), cmp_d);
    /* the crossings, once per set of quadratics */
    if (b != g_cr_for || b->n != g_cr_n || (b->n && b->q[0].x0 != g_cr_x0)) {
        int j;
        g_ncr = 0;
        for (i = 0; i < b->n; i++) for (j = i + 1; j < b->n; j++) {
            double ys[16];
            int n = 0, m;
            g_nodes = 0;
            cross_rec(&b->q[i].x0, &b->q[j].x0, ys, &n, 16, 0);
            for (m = 0; m < n; m++) {
                if (g_ncr == g_crcap) { g_crcap = g_crcap ? 2 * g_crcap : 256; g_cr = (double*)realloc(g_cr, (size_t)g_crcap * sizeof(double)); }
                g_cr[g_ncr++] = ys[m];
            }
        }
        g_cr_for = b; g_cr_n = b->n; g_cr_x0 = b->n ? b->q[0].x0 : 0;
    }
    for (i = 0; i < g_ncr; i++) ADDB(g_cr[i]);
    qsort(br, (size_t)nb, sizeof(double), cmp_d);
#undef ADDB
    g_gb = b; g_c0 = c0; g_c1 = c1; g_eo = evenodd;
    for (i = 0; i + 1 < nb; i++) if (br[i + 1] > br[i]) s += adapt(br[i], br[i + 1], tol * (br[i + 1] - br[i]) / (y1 - y0 + 1e-300), 0);
    return s;
}

/* --- byte writer, synthetic fonts --------------------------------------------- */

typedef struct bb { uint8_t* d; size_t n, cap; } bb;
static void b_need(bb* b, size_t k) { if (b->n + k > b->cap) { while (b->n + k > b->cap) b->cap = b->cap ? 2 * b->cap : 256; b->d = (uint8_t*)realloc(b->d, b->cap); } }
static void b8(bb* b, uint32_t v) { b_need(b, 1); b->d[b->n++] = (uint8_t)v; }
static void b16(bb* b, uint32_t v) { b8(b, v >> 8 & 255); b8(b, v & 255); }
static void b32(bb* b, uint32_t v) { b16(b, v >> 16); b16(b, v & 0xffff); }
static void bcat(bb* b, const bb* s) { b_need(b, s->n); memcpy(b->d + b->n, s->d, s->n); b->n += s->n; }
static void bpad4(bb* b) { while (b->n & 3) b8(b, 0); }
static void bset32(bb* b, size_t at, uint32_t v) { b->d[at] = (uint8_t)(v >> 24); b->d[at + 1] = (uint8_t)(v >> 16); b->d[at + 2] = (uint8_t)(v >> 8); b->d[at + 3] = (uint8_t)v; }

typedef struct tbl { char tag[5]; bb data; } tbl;

static void font_assemble(bb* out, tbl* t, int n, uint32_t version) {
    int i;
    size_t dir;
    out->n = 0;
    b32(out, version); b16(out, (uint32_t)n); b16(out, 0); b16(out, 0); b16(out, 0);
    dir = out->n;
    for (i = 0; i < n; i++) { b32(out, 0); b32(out, 0); b32(out, 0); b32(out, 0); }
    for (i = 0; i < n; i++) {
        size_t off;
        bpad4(out);
        off = out->n;
        bcat(out, &t[i].data);
        memcpy(out->d + dir + 16 * i, t[i].tag, 4);
        bset32(out, dir + 16 * i + 8, (uint32_t)off);
        bset32(out, dir + 16 * i + 12, (uint32_t)t[i].data.n);
    }
    bpad4(out);
}

static void t_head(bb* b, int upem, int locfmt) {
    int i;
    for (i = 0; i < 54; i++) b8(b, 0);
    b->d[18] = (uint8_t)(upem >> 8); b->d[19] = (uint8_t)upem;
    b->d[50] = 0; b->d[51] = (uint8_t)locfmt;
}
static void t_hhea(bb* b, int nhm) {
    int i;
    for (i = 0; i < 36; i++) b8(b, 0);
    b->d[4] = 0x03; b->d[5] = 0x20;            /* ascender 800 */
    b->d[6] = 0xff; b->d[7] = 0x38;            /* descender -200 */
    b->d[8] = 0; b->d[9] = 90;                 /* lineGap 90 */
    b->d[34] = (uint8_t)(nhm >> 8); b->d[35] = (uint8_t)nhm;
}
static void t_maxp(bb* b, int ng) { b32(b, 0x00005000); b16(b, (uint32_t)ng); }
static void t_hmtx(bb* b, int ng, int nhm, const int* lsb) {
    int g;
    for (g = 0; g < nhm; g++) { b16(b, (uint32_t)(600 + 10 * g)); b16(b, (uint32_t)((lsb ? lsb[g] : g * 3) & 0xffff)); }
    for (; g < ng; g++) b16(b, (uint32_t)((lsb ? lsb[g] : g * 3) & 0xffff));
}

/* A simple glyph from points (font units), on-curve flags and contour ends,
 * with short vectors, repeats and instructions as the encoder sees fit. */
static void glyf_simple(bb* b, const int* xs, const int* ys, const int* on, int np, const int* ends, int nc, int ninstr) {
    int i, x = 0, y = 0, xmin = 1 << 30, ymin = 1 << 30, xmax = -(1 << 30), ymax = -(1 << 30);
    uint8_t* fl = (uint8_t*)malloc((size_t)np);
    for (i = 0; i < np; i++) { xmin = xs[i] < xmin ? xs[i] : xmin; xmax = xs[i] > xmax ? xs[i] : xmax; ymin = ys[i] < ymin ? ys[i] : ymin; ymax = ys[i] > ymax ? ys[i] : ymax; }
    b16(b, (uint32_t)nc); b16(b, (uint32_t)(xmin & 0xffff)); b16(b, (uint32_t)(ymin & 0xffff)); b16(b, (uint32_t)(xmax & 0xffff)); b16(b, (uint32_t)(ymax & 0xffff));
    for (i = 0; i < nc; i++) b16(b, (uint32_t)ends[i]);
    b16(b, (uint32_t)ninstr);
    for (i = 0; i < ninstr; i++) b8(b, 0xb0);
    for (i = 0; i < np; i++) {
        int dx = xs[i] - x, dy = ys[i] - y;
        uint8_t f = (uint8_t)(on[i] ? 1 : 0);
        if (dx == 0) f |= 16; else if (dx > -256 && dx < 256) { f |= 2; if (dx > 0) f |= 16; }
        if (dy == 0) f |= 32; else if (dy > -256 && dy < 256) { f |= 4; if (dy > 0) f |= 32; }
        fl[i] = f; x = xs[i]; y = ys[i];
    }
    for (i = 0; i < np;) {
        int r = 0;
        while (i + 1 + r < np && fl[i + 1 + r] == fl[i] && r < 255) r++;
        if (r >= 1) { b8(b, fl[i] | 8u); b8(b, (uint32_t)r); i += r + 1; }
        else { b8(b, fl[i]); i++; }
    }
    for (x = 0, i = 0; i < np; i++) {
        int dx = xs[i] - x;
        if (fl[i] & 2) b8(b, (uint32_t)abs(dx)); else if (!(fl[i] & 16)) b16(b, (uint32_t)(dx & 0xffff));
        x = xs[i];
    }
    for (y = 0, i = 0; i < np; i++) {
        int dy = ys[i] - y;
        if (fl[i] & 4) b8(b, (uint32_t)abs(dy)); else if (!(fl[i] & 32)) b16(b, (uint32_t)(dy & 0xffff));
        y = ys[i];
    }
    free(fl);
}

/* The TrueType test font. Glyphs, font units, y up, unitsPerEm 1000:
 *   1 a square with a square hole        2 a contour of 8 off-curve points
 *   3 two overlapping rectangles         4 composite: 1 by the 2 x 2 (0.5,
 *                                          0.25; 0, 0.5) at (100, 200), 3
 *                                          flipped by an x-y scale at (10, -20)
 *   5 a composite that names itself      6 a point-matched component
 *   7 repeats, short vectors, instructions
 *   8 64 components of 9, which is 64 components of 1: past the visit bound
 *   10 40000 on-curve points             11 two components of 10: past the
 *                                          point bound
 *   HarfBuzz's placement (FONTS): hmtx's lsb equals xMin for every glyph
 *   above; these differ:
 *   12 a square from x = 100, lsb 70: moves by -30
 *   13 composite: 1 at (50, 0), header xMin 40, lsb 45: moves by +5
 *   14 composite: 12 at (200, 0) with USE_MY_METRICS, then 1; its own
 *      xMin and lsb 0: moves by 12's -30 */
#define TT_NG 15
static const int tt_lsb[TT_NG] = { 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, -400, 0, 70, 45, 0 };
static const int tt_g12x[] = { 100, 300, 300, 100 }, tt_g12y[] = { 0, 0, 200, 200 };
static const int tt_g1x[] = { 0, 1000, 1000, 0, 250, 250, 750, 750 }, tt_g1y[] = { 0, 0, 1000, 1000, 250, 750, 750, 250 };
static const int tt_g3x[] = { 0, 600, 600, 0, 300, 900, 900, 300 }, tt_g3y[] = { 0, 0, 400, 400, 200, 200, 700, 700 };
static const int tt_g7x[] = { 0, 10, 20, 30, 40, 40, 40, 40, 30, 20, 10, 0 }, tt_g7y[] = { 0, 0, 0, 0, 0, 10, 20, 30, 30, 30, 30, 30 };
static const int tt_g7on[] = { 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1 };
static int tt_g2x[8], tt_g2y[8];

static void make_tt(bb* font, int with_fvar, int collection) {
    tbl t[9];
    bb glyf = { 0, 0, 0 };
    uint32_t loca[TT_NG + 1];
    int g, i, nt = 0;
    static const int on1[8] = { 1, 1, 1, 1, 1, 1, 1, 1 }, ends1[2] = { 3, 7 }, off8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }, ends2[1] = { 7 }, ends7[1] = { 11 };
    memset(t, 0, sizeof t);
    for (i = 0; i < 8; i++) { tt_g2x[i] = 500 + (int)floor(400 * cos(i * 0.785398163397448) + 0.5); tt_g2y[i] = 500 + (int)floor(400 * sin(i * 0.785398163397448) + 0.5); }
    for (g = 0; g < TT_NG; g++) {
        bpad4(&glyf);
        loca[g] = (uint32_t)glyf.n;
        if (g == 1) glyf_simple(&glyf, tt_g1x, tt_g1y, on1, 8, ends1, 2, 0);
        else if (g == 2) glyf_simple(&glyf, tt_g2x, tt_g2y, off8, 8, ends2, 1, 0);
        else if (g == 3) glyf_simple(&glyf, tt_g3x, tt_g3y, on1, 8, ends1, 2, 0);
        else if (g == 4) {
            b16(&glyf, 0xffff); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 1000); b16(&glyf, 1000);
            b16(&glyf, 0x0001 | 0x0002 | 0x0080 | 0x0020); b16(&glyf, 1); b16(&glyf, 100); b16(&glyf, 200);
            b16(&glyf, 0x2000); b16(&glyf, 0x1000); b16(&glyf, 0); b16(&glyf, 0x2000);
            b16(&glyf, 0x0002 | 0x0040); b16(&glyf, 3); b8(&glyf, 10); b8(&glyf, (uint32_t)(-20 & 255));
            b16(&glyf, 0x4000); b16(&glyf, 0xc000);
        } else if (g == 5) {
            b16(&glyf, 0xffff); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0);
            b16(&glyf, 0x0002); b16(&glyf, 5); b8(&glyf, 0); b8(&glyf, 0);
        } else if (g == 6) {
            b16(&glyf, 0xffff); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0);
            b16(&glyf, 0x0000); b16(&glyf, 1); b8(&glyf, 0); b8(&glyf, 0);
        } else if (g == 7) glyf_simple(&glyf, tt_g7x, tt_g7y, tt_g7on, 12, ends7, 1, 3);
        else if (g == 8 || g == 9) {
            int k;
            b16(&glyf, 0xffff); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0);
            for (k = 0; k < 64; k++) { b16(&glyf, 0x0002 | (k < 63 ? 0x0020u : 0u)); b16(&glyf, g == 8 ? 9u : 1u); b8(&glyf, (uint32_t)k); b8(&glyf, 0); }
        } else if (g == 10) {
            int* px = (int*)malloc(40000 * sizeof(int)), * py = (int*)malloc(40000 * sizeof(int)), * pon = (int*)malloc(40000 * sizeof(int)), e1 = 39999, k;
            for (k = 0; k < 40000; k++) { px[k] = (int)floor(400 * cos(k * 6.283185307179586 / 40000) + 0.5); py[k] = (int)floor(400 * sin(k * 6.283185307179586 / 40000) + 0.5) + (k & 1); pon[k] = 1; }
            glyf_simple(&glyf, px, py, pon, 40000, &e1, 1, 0);
            free(px); free(py); free(pon);
        } else if (g == 11) {
            b16(&glyf, 0xffff); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 0);
            b16(&glyf, 0x0002 | 0x0020); b16(&glyf, 10); b8(&glyf, 0); b8(&glyf, 0);
            b16(&glyf, 0x0002); b16(&glyf, 10); b8(&glyf, 5); b8(&glyf, 0);
        } else if (g == 12) {
            static const int on4[4] = { 1, 1, 1, 1 }, e3 = 3;
            glyf_simple(&glyf, tt_g12x, tt_g12y, on4, 4, &e3, 1, 0);
        } else if (g == 13) {
            b16(&glyf, 0xffff); b16(&glyf, 40); b16(&glyf, 0); b16(&glyf, 1050); b16(&glyf, 1000);
            b16(&glyf, 0x0002); b16(&glyf, 1); b8(&glyf, 50); b8(&glyf, 0);
        } else if (g == 14) {
            b16(&glyf, 0xffff); b16(&glyf, 0); b16(&glyf, 0); b16(&glyf, 500); b16(&glyf, 1000);
            b16(&glyf, 0x0001 | 0x0002 | 0x0020 | 0x0200); b16(&glyf, 12); b16(&glyf, 200); b16(&glyf, 0);
            b16(&glyf, 0x0002); b16(&glyf, 1); b8(&glyf, 0); b8(&glyf, 0);
        }
    }
    bpad4(&glyf);
    loca[TT_NG] = (uint32_t)glyf.n;
    memcpy(t[nt].tag, "head", 4); t_head(&t[nt].data, 1000, 1); nt++;
    memcpy(t[nt].tag, "hhea", 4); t_hhea(&t[nt].data, 5); nt++;
    memcpy(t[nt].tag, "maxp", 4); t_maxp(&t[nt].data, TT_NG); nt++;
    memcpy(t[nt].tag, "hmtx", 4); t_hmtx(&t[nt].data, TT_NG, 5, tt_lsb); nt++;
    memcpy(t[nt].tag, "loca", 4); for (g = 0; g <= TT_NG; g++) b32(&t[nt].data, loca[g]); nt++;
    memcpy(t[nt].tag, "glyf", 4); t[nt].data = glyf; nt++;
    memcpy(t[nt].tag, "cmap", 4);
    {
        bb* c = &t[nt].data;
        /* format 4: 'A'-'B' by delta, 'C'-'D' through glyphIdArray, then 0xFFFF */
        b16(c, 0); b16(c, 1); b16(c, 3); b16(c, 1); b32(c, 12);
        b16(c, 4); b16(c, 16 + 4 * 6 + 4); b16(c, 0);
        b16(c, 6); b16(c, 4); b16(c, 1); b16(c, 2);
        b16(c, 66); b16(c, 68); b16(c, 0xffff); b16(c, 0);
        b16(c, 65); b16(c, 67); b16(c, 0xffff);
        b16(c, (uint32_t)((1 - 65) & 0xffff)); b16(c, 0); b16(c, 1);
        b16(c, 0); b16(c, 4); b16(c, 0);
        b16(c, 3); b16(c, 4);
        nt++;
    }
    if (with_fvar) { memcpy(t[nt].tag, "fvar", 4); b32(&t[nt].data, 0x00010000); nt++; }
    if (!collection) font_assemble(font, t, nt, 0x00010000);
    else {
        /* two faces: the same tables twice, the second face behind the first */
        bb one = { 0, 0, 0 };
        font_assemble(&one, t, nt, 0x00010000);
        font->n = 0;
        b32(font, 0x74746366); b32(font, 0x00010000); b32(font, 2); b32(font, 20); b32(font, (uint32_t)(20 + one.n));
        {
            /* table offsets inside each face are relative to the file start */
            size_t base1 = font->n, base2, k;
            bcat(font, &one);
            base2 = font->n;
            bcat(font, &one);
            for (k = 0; k < (size_t)nt; k++) {
                size_t r1 = base1 + 12 + 16 * k + 8, r2 = base2 + 12 + 16 * k + 8;
                uint32_t o = (uint32_t)font->d[r1] << 24 | (uint32_t)font->d[r1 + 1] << 16 | (uint32_t)font->d[r1 + 2] << 8 | font->d[r1 + 3];
                bset32(font, r1, o + (uint32_t)base1);
                bset32(font, r2, o + (uint32_t)base2);
            }
        }
        free(one.d);
    }
    for (i = 0; i < nt; i++) free(t[i].data.d);
}

/* --- CFF ------------------------------------------------------------------------- */

static void cs_num(bb* b, double v) {
    if (v != floor(v)) { b8(b, 255); b32(b, (uint32_t)(int32_t)floor(v * 65536.0 + 0.5)); }
    else if (v >= -107 && v <= 107) b8(b, (uint32_t)(v + 139));
    else { b8(b, 28); b16(b, (uint32_t)((int)v & 0xffff)); }
}
static void cs_nums(bb* b, const double* v, int n) { int i; for (i = 0; i < n; i++) cs_num(b, v[i]); }
static void cs_op(bb* b, int op) { if (op >= 1200) { b8(b, 12); b8(b, (uint32_t)(op - 1200)); } else b8(b, (uint32_t)op); }

static void cff_index(bb* out, bb* items, int n) {
    int i;
    uint32_t off = 1;
    b16(out, (uint32_t)n);
    if (n == 0) return;
    b8(out, 4);
    for (i = 0; i <= n; i++) { b32(out, off); if (i < n) off += (uint32_t)items[i].n; }
    for (i = 0; i < n; i++) bcat(out, &items[i]);
}
static void dict_int5(bb* b, uint32_t v) { b8(b, 29); b32(b, v); }

#define CFF_NG 15
/* Charstrings, font units, y up:
 *   1 width, a square by rmoveto, hlineto, vlineto
 *   2 stems, hintmask, rrcurveto with a 16.16 fixed coordinate, hhcurveto,
 *     vvcurveto, hvcurveto, vhcurveto, rcurveline, rlinecurve, flex
 *   3 callsubr, callgsubr, hflex, flex1, hflex1
 *   4 seac (refused)  5 an arithmetic operator (refused)
 *   6 a subroutine that calls itself (depth bound)  7 no endchar
 *   8 a large cubic, to count quadratics
 *   9 subroutines that fan out 4^9 calls (the token bound)
 *   10 49 operands (the stack bound)     11 120 stems (the stem bound)
 *   12 4000 large cubics (the point bound)
 *   13 subroutines nested 10 deep (allowed)  14 nested 11 deep (refused) */
static void cff_charstrings(bb* cs) {
    bb* b;
    { double v[] = { 0 }; (void)v; b = &cs[0]; cs_op(b, 14); }
    b = &cs[1];
    { double v[] = { 600, 100, 100 }; cs_nums(b, v, 3); cs_op(b, 21); }
    { double v[] = { 800 }; cs_nums(b, v, 1); cs_op(b, 6); }
    { double v[] = { 800 }; cs_nums(b, v, 1); cs_op(b, 7); }
    { double v[] = { -800 }; cs_nums(b, v, 1); cs_op(b, 6); }
    cs_op(b, 14);
    b = &cs[2];
    { double v[] = { 0, 50 }; cs_nums(b, v, 2); cs_op(b, 1); }
    { double v[] = { 100, 50, 200, 50 }; cs_nums(b, v, 4); cs_op(b, 19); b8(b, 0xe0); }
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 100.5, 0, 100, 100, 0, 100 }; cs_nums(b, v, 6); cs_op(b, 8); }
    { double v[] = { 10, 50, 50, 50, 50 }; cs_nums(b, v, 5); cs_op(b, 27); }
    { double v[] = { 5, -50, -50, -50, -50 }; cs_nums(b, v, 5); cs_op(b, 26); }
    { double v[] = { -50, -50, -50, -10 }; cs_nums(b, v, 4); cs_op(b, 31); }
    { double v[] = { -10, -50, -10, -50, 3 }; cs_nums(b, v, 5); cs_op(b, 30); }
    { double v[] = { 10, 10, 10, 10, 10, 10, -20, -20 }; cs_nums(b, v, 8); cs_op(b, 24); }
    { double v[] = { -10, 0, 0, -10, 0, -10, 0, -10 }; cs_nums(b, v, 8); cs_op(b, 25); }
    { double v[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 50 }; cs_nums(b, v, 13); cs_op(b, 1235); }
    cs_op(b, 14);
    b = &cs[3];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { -107 }; cs_nums(b, v, 1); cs_op(b, 10); }
    { double v[] = { -107 }; cs_nums(b, v, 1); cs_op(b, 29); }
    { double v[] = { 10, 5, 5, 10, 5, 5, 10 }; cs_nums(b, v, 7); cs_op(b, 1234); }
    { double v[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 5 }; cs_nums(b, v, 11); cs_op(b, 1237); }
    { double v[] = { 5, 1, 5, 1, 5, 5, 5, -1, 5 }; cs_nums(b, v, 9); cs_op(b, 1236); }
    cs_op(b, 14);
    b = &cs[4];
    { double v[] = { 0, 0, 65, 66 }; cs_nums(b, v, 4); cs_op(b, 14); }
    b = &cs[5];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 1, 2 }; cs_nums(b, v, 2); cs_op(b, 1210); }
    cs_op(b, 14);
    b = &cs[6];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { -106 }; cs_nums(b, v, 1); cs_op(b, 10); }
    cs_op(b, 14);
    b = &cs[7];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 100, 0 }; cs_nums(b, v, 2); cs_op(b, 5); }
    b = &cs[8];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 0, 900, 900, 0, 0, -900 }; cs_nums(b, v, 6); cs_op(b, 8); }
    cs_op(b, 14);
    b = &cs[9];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 2 - 107 }; cs_nums(b, v, 1); cs_op(b, 10); }
    cs_op(b, 14);
    b = &cs[10];
    { int k; for (k = 0; k < 49; k++) cs_num(b, 1); cs_op(b, 5); cs_op(b, 14); }
    b = &cs[11];
    { int k, j; for (j = 0; j < 5; j++) { for (k = 0; k < 48; k++) cs_num(b, 10); cs_op(b, 18); } }
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    cs_op(b, 14);
    b = &cs[12];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { int k; for (k = 0; k < 4000; k++) { double v[] = { 500, 3000, 500, -6000, 500, 3000 }; cs_nums(b, v, 6); cs_op(b, 8); } }
    cs_op(b, 14);
    b = &cs[13];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 13 - 107 }; cs_nums(b, v, 1); cs_op(b, 10); }
    { double v[] = { 50, 50 }; cs_nums(b, v, 2); cs_op(b, 5); }
    cs_op(b, 14);
    b = &cs[14];
    { double v[] = { 0, 0 }; cs_nums(b, v, 2); cs_op(b, 21); }
    { double v[] = { 12 - 107 }; cs_nums(b, v, 1); cs_op(b, 10); }
    cs_op(b, 14);
}

/* A CFF font; cid: CID-keyed (ROS, FDArray of one Font DICT, FDSelect
 * format 3). cmap format 12: U+0041 -> 1, U+1F600 -> 3. */
static void make_cff(bb* font, int cid, int cff2_only) {
    tbl t[7];
    bb cff = { 0, 0, 0 }, name = { 0, 0, 0 }, top = { 0, 0, 0 }, cs[CFF_NG], gsub[1], lsub[23], tmp = { 0, 0, 0 };
    bb priv = { 0, 0, 0 }, csidx = { 0, 0, 0 }, gsidx = { 0, 0, 0 }, lsidx = { 0, 0, 0 }, fdidx = { 0, 0, 0 }, fdsel = { 0, 0, 0 }, fd = { 0, 0, 0 };
    size_t topsize, base_after_gs, o_cs, o_priv, o_lsub, o_fdarray, o_fdsel;
    int i, nt = 0;
    memset(t, 0, sizeof t); memset(cs, 0, sizeof cs); memset(gsub, 0, sizeof gsub); memset(lsub, 0, sizeof lsub);
    cff_charstrings(cs);
    { double v[] = { 0, 100 }; cs_nums(&gsub[0], v, 2); cs_op(&gsub[0], 5); cs_op(&gsub[0], 11); }
    { double v[] = { 100, 0 }; cs_nums(&lsub[0], v, 2); cs_op(&lsub[0], 5); cs_op(&lsub[0], 11); }
    { double v[] = { -106 }; cs_nums(&lsub[1], v, 1); cs_op(&lsub[1], 10); cs_op(&lsub[1], 11); }
    {   /* subr k calls subr k + 1 four times; subr 11 returns */
        int k, j;
        for (k = 2; k < 11; k++) { for (j = 0; j < 4; j++) { cs_num(&lsub[k], k + 1 - 107); cs_op(&lsub[k], 10); } cs_op(&lsub[k], 11); }
        cs_op(&lsub[11], 11);
        /* a chain: subr k calls subr k + 1 once, 12 to 22 */
        for (k = 12; k < 22; k++) { cs_num(&lsub[k], k + 1 - 107); cs_op(&lsub[k], 10); cs_op(&lsub[k], 11); }
        { double v[] = { 10, 0 }; cs_nums(&lsub[22], v, 2); cs_op(&lsub[22], 5); cs_op(&lsub[22], 11); }
    }
    b8(&name, 'T');
    cff_index(&csidx, cs, CFF_NG);
    cff_index(&gsidx, gsub, 1);
    cff_index(&lsidx, lsub, 23);
    /* the Top DICT has a fixed size: every offset is a 5-byte integer */
    topsize = cid ? 5 + 6 + 7 + 7 : 6 + 11;
    {
        size_t hdr = 4, nameidx, topidx, stridx = 2;
        nameidx = 2 + 1 + 4 * 2 + name.n;
        topidx = 2 + 1 + 4 * 2 + topsize;
        base_after_gs = hdr + nameidx + topidx + stridx + gsidx.n;
    }
    o_cs = base_after_gs;
    o_priv = o_cs + csidx.n;
    o_lsub = o_priv + 6;                       /* Private: Subrs at +6 */
    o_fdarray = o_lsub + lsidx.n;
    /* Private: the local Subrs right after it */
    dict_int5(&priv, 6); cs_op(&priv, 19);
    if (!cid) {
        dict_int5(&top, (uint32_t)o_cs); cs_op(&top, 17);
        dict_int5(&top, 6); dict_int5(&top, (uint32_t)o_priv); cs_op(&top, 18);
    } else {
        /* ROS: three small integers */
        b8(&top, 139); b8(&top, 139); b8(&top, 139); cs_op(&top, 1230);
        dict_int5(&top, (uint32_t)o_cs); cs_op(&top, 17);
        /* FDArray: one Font DICT naming the same Private */
        dict_int5(&fd, 6); dict_int5(&fd, (uint32_t)o_priv); cs_op(&fd, 18);
        cff_index(&fdidx, &fd, 1);
        o_fdsel = o_fdarray + fdidx.n;
        dict_int5(&top, (uint32_t)o_fdarray); cs_op(&top, 1236);
        dict_int5(&top, (uint32_t)o_fdsel); cs_op(&top, 1237);
        /* FDSelect format 3: one range, every glyph in FD 0 */
        b8(&fdsel, 3); b16(&fdsel, 1); b16(&fdsel, 0); b8(&fdsel, 0); b16(&fdsel, CFF_NG);
    }
    if (top.n != topsize) { fprintf(stderr, "test bug: Top DICT %u, planned %u\n", (unsigned)top.n, (unsigned)topsize); exit(2); }
    b8(&cff, 1); b8(&cff, 0); b8(&cff, 4); b8(&cff, 4);
    cff_index(&cff, &name, 1);
    cff_index(&cff, &top, 1);
    b16(&cff, 0);                               /* String INDEX, empty */
    bcat(&cff, &gsidx);
    bcat(&cff, &csidx);
    bcat(&cff, &priv);
    bcat(&cff, &lsidx);
    if (cid) { bcat(&cff, &fdidx); bcat(&cff, &fdsel); }
    memcpy(t[nt].tag, "head", 4); t_head(&t[nt].data, 1000, 0); nt++;
    memcpy(t[nt].tag, "hhea", 4); t_hhea(&t[nt].data, CFF_NG); nt++;
    memcpy(t[nt].tag, "maxp", 4); t_maxp(&t[nt].data, CFF_NG); nt++;
    memcpy(t[nt].tag, "hmtx", 4); t_hmtx(&t[nt].data, CFF_NG, CFF_NG, NULL); nt++;
    memcpy(t[nt].tag, "cmap", 4);
    {
        bb* c = &t[nt].data;
        b16(c, 0); b16(c, 1); b16(c, 3); b16(c, 10); b32(c, 12);
        b16(c, 12); b16(c, 0); b32(c, 16 + 2 * 12); b32(c, 0); b32(c, 2);
        b32(c, 0x41); b32(c, 0x41); b32(c, 1);
        b32(c, 0x1f600); b32(c, 0x1f600); b32(c, 3);
        nt++;
    }
    memcpy(t[nt].tag, cff2_only ? "CFF2" : "CFF ", 4); t[nt].data = cff; nt++;
    font_assemble(font, t, nt, 0x4F54544F);
    for (i = 0; i < nt; i++) free(t[i].data.d);
    for (i = 0; i < CFF_NG; i++) free(cs[i].d);
    for (i = 0; i < 23; i++) free(lsub[i].d);
    free(gsub[0].d); free(name.d); free(top.d); free(priv.d); free(csidx.d); free(gsidx.d);
    free(lsidx.d); free(fdidx.d); free(fdsel.d); free(fd.d); free(tmp.d);
}

/* --- helpers on the header's types --------------------------------------------- */

static yol_ctx g_cx;

static int path_closed(const yol_path* p) {
    int k;
    for (k = 0; k < p->n_contours; k++) {
        int f = (int)(p->contours[k] & ~YOL_OPEN), e = k + 1 < p->n_contours ? (int)(p->contours[k + 1] & ~YOL_OPEN) : p->n_pts;
        if (p->contours[k] & YOL_OPEN) return 0;
        if ((e - f) < 3 || ((e - f) & 1) == 0) return 0;
        if (p->pts[2 * (e - 1)] != p->pts[2 * f] || p->pts[2 * (e - 1) + 1] != p->pts[2 * f + 1]) return 0;
    }
    return 1;
}

static int path_segments(const yol_path* p) { return (p->n_pts - p->n_contours) / 2; }

static ygfx_cset_desc cset_desc(const yol_cset* s) {
    ygfx_cset_desc d;
    memset(&d, 0, sizeof d);
    d.texels = s->texels; d.n_texels = s->n_texels; d.words = s->words; d.n_words = s->n_words;
    return d;
}
static int cset_ok(const yol_cset* s) {
    ygfx_cset_desc d = cset_desc(s);
    char msg[300];
    int rc = ygfx_cset_check(&d, msg, sizeof msg);
    if (rc != 0) fprintf(stderr, "outline_test [%s]: ygfx_cset_check: %s\n", g_where, msg);
    return rc == 0;
}

/* winding at a point and at four points 1e-6 * scale around it; -99 when they
 * differ (the point is too near a curve to compare) */
static int stable_winding(const tqs* b, double x, double y, double e) {
    int w = winding(b, x, y);
    if (winding(b, x + e, y) != w || winding(b, x - e, y) != w || winding(b, x, y + e) != w || winding(b, x, y - e) != w) return -99;
    return w;
}

/* --- T1: the format's test vector ------------------------------------------------ */

static void t1_vector(void) {
    static const float tex[40] = { 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0,
                                   0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f,
                                   0.75f, 0.25f, 0.75f, 0.25f, 0.25f, 0.25f, 0, 0 };
    static const uint32_t words[60] = {
        0x43505359u, 1, 1, 0, 0, 0, 0, 0, 12, 0, 0, 0,
        0, 0, 0x3f800000u, 0x3f800000u, 0x00020002u, 0, 0, 10,
        36, 6, 0, 0, 42, 6, 0, 0, 48, 6, 0, 0, 54, 6, 0, 0,
        0, 1, 7, 8, 5, 3, 1, 2, 6, 7, 5, 3, 2, 3, 5, 6, 8, 0, 1, 2, 6, 7, 8, 0 };
    yol_path p;
    yol_cset s;
    yol_cset_desc d;
    int i, same = 1;
    g_where = "T1 vector";
    yol_path_init(&p, &g_cx);
    yol_move(&p, 0, 0); yol_line(&p, 1, 0); yol_line(&p, 1, 1); yol_line(&p, 0, 1); yol_close_path(&p);
    yol_move(&p, 0.25, 0.25); yol_line(&p, 0.25, 0.75); yol_line(&p, 0.75, 0.75); yol_line(&p, 0.75, 0.25); yol_close_path(&p);
    CHECK(yol_path_end(&p) == YOL_OK);
    memset(&d, 0, sizeof d);
    d.n_glyphs = 1; d.nh = 2; d.nv = 2; d.keep_overlaps = true;
    CHECK(yol_cset_open(&s, &g_cx, &d));
    CHECK(yol_cset_add(&s, 0, &p) == YOL_OK);
    CHECK(s.n_texels == 10 && s.n_words == 60);
    if (s.n_texels == 10) for (i = 0; i < 40; i++) if (memcmp(&s.texels[i], &tex[i], 4) != 0) same = 0;
    if (s.n_words == 60) for (i = 0; i < 60; i++) if (s.words[i] != words[i]) { same = 0; fprintf(stderr, "  word %d: %u, want %u\n", i, s.words[i], words[i]); }
    CHECK(same);
    CHECK(cset_ok(&s));
    /* the same glyph resolved (the default) carries the RESOLVED flag; with
     * keep_overlaps it does not */
    {
        yol_cset r;
        yol_cset_desc dr;
        memset(&dr, 0, sizeof dr);
        dr.n_glyphs = 2;
        CHECK(yol_cset_open(&r, &g_cx, &dr));
        CHECK(yol_cset_add(&r, 1, &p) == YOL_OK);
        CHECK(r.words[8] == 0 && (r.words[r.words[9] + 5] & 0x4u) == 0x4u);
        CHECK((s.words[s.words[8] + 5] & 0x4u) == 0);
        CHECK(yol_cset_add(&r, 1, &p) == YOL_ERR_ARG);     /* already in the set */
        CHECK(yol_cset_add(&r, 2, &p) == YOL_ERR_RANGE);
        CHECK(cset_ok(&r));
        yol_cset_close(&r);
    }
    yol_cset_close(&s);
    yol_path_free(&p);
}

/* --- T6: cubics ---------------------------------------------------------------- */

static void t6_cubics(void) {
    int i, j, k;
    double worst = 0;
    static const double tols[3] = { 1e-2, 1e-4, 1e-6 };
    yol_path p;
    g_where = "T6 cubics";
    yol_path_init(&p, &g_cx);
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 300; i++) {
            double P[8], tol = tols[k], err = 0;
            int n, q;
            for (j = 0; j < 8; j++) P[j] = 2 * rnd() - 1;
            yol_path_clear(&p);
            yol_set_tol(&p, tol);
            yol_move(&p, P[0], P[1]);
            yol_cubic(&p, P[2], P[3], P[4], P[5], P[6], P[7]);
            CHECK(yol_path_end(&p) == YOL_OK);
            n = path_segments(&p);
            /* piece q covers t in [q / n, (q + 1) / n]: compare parametrically */
            for (q = 0; q < n; q++) {
                const double* s = p.pts + 4 * q;
                int m;
                for (m = 0; m <= 40; m++) {
                    double u = m / 40.0, t = (q + u) / n, a = 1 - u, c = 1 - t;
                    double xq = a * a * s[0] + 2 * a * u * s[2] + u * u * s[4], yq = a * a * s[1] + 2 * a * u * s[3] + u * u * s[5];
                    double cxv = c * c * c * P[0] + 3 * c * c * t * P[2] + 3 * c * t * t * P[4] + t * t * t * P[6];
                    double yc = c * c * c * P[1] + 3 * c * c * t * P[3] + 3 * c * t * t * P[5] + t * t * t * P[7];
                    err = fmax(err, sqrt((xq - cxv) * (xq - cxv) + (yq - yc) * (yq - yc)) / tol);
                }
            }
            worst = fmax(worst, err);
            CHECK(p.pts[2 * p.n_pts - 2] == P[6] && p.pts[2 * p.n_pts - 1] == P[7]);
        }
    }
    CHECKF(worst <= 1.0 + 1e-9, "cubic error %.3g of tol", worst);
    printf("T6 cubics: 900 random cubics, worst error %.3f of the tolerance\n", worst);
    /* arcs and ellipses within tol of the exact ellipse, transformed */
    {
        double e = 0;
        yol_path_clear(&p);
        yol_set_tol(&p, 1e-5);
        yol_set_xform(&p, yol_xf_mul(yol_xf_rotate(30), yol_xf_scale(2, 0.5)));
        yol_ellipse(&p, 0.1, 0.2, 0.7, 0.3);
        CHECK(yol_path_end(&p) == YOL_OK && path_closed(&p));
        {
            tqs b = { 0, 0, 0 };
            yol_xform t = p.xf, inv;
            double det = t.a * t.d - t.b * t.c;
            path_quads(&p, &b, 1, 0, 0);
            inv.a = t.d / det; inv.b = -t.b / det; inv.c = -t.c / det; inv.d = t.a / det; inv.e = 0; inv.f = 0;
            for (i = 0; i < b.n; i++) for (j = 0; j <= 20; j++) {
                /* the exact ellipse's nearest point, by Newton on its angle in
                 * the transformed frame */
                double x = qx(&b.q[i], j / 20.0), y = qy(&b.q[i], j / 20.0), ux = inv.a * x + inv.c * y - 0.1, uy = inv.b * x + inv.d * y - 0.2;
                double th = atan2(uy / 0.3, ux / 0.7), dmin;
                int it;
                for (it = 0; it < 30; it++) {
                    double ex = 0.1 + 0.7 * cos(th), ey = 0.2 + 0.3 * sin(th), dx = -0.7 * sin(th), dy = 0.3 * cos(th), ddx = -0.7 * cos(th), ddy = -0.3 * sin(th);
                    double X = t.a * ex + t.c * ey, Y = t.b * ex + t.d * ey, DX = t.a * dx + t.c * dy, DY = t.b * dx + t.d * dy, DDX = t.a * ddx + t.c * ddy, DDY = t.b * ddx + t.d * ddy;
                    double f1 = (X - x) * DX + (Y - y) * DY, f2 = DX * DX + DY * DY + (X - x) * DDX + (Y - y) * DDY;
                    if (f2 <= 0) break;
                    th -= f1 / f2;
                }
                {
                    double ex = 0.1 + 0.7 * cos(th), ey = 0.2 + 0.3 * sin(th), X = t.a * ex + t.c * ey, Y = t.b * ex + t.d * ey;
                    dmin = sqrt((X - x) * (X - x) + (Y - y) * (Y - y));
                }
                e = fmax(e, dmin);
            }
            free(b.q);
        }
        CHECKF(e <= 1.05e-5, "ellipse error %.3g", e);
        yol_set_xform(&p, yol_xf_identity());
    }
    yol_path_free(&p);
}

/* --- T4, T5: the font reader ------------------------------------------------------- */

static int path_has_ends(const yol_path* p, const double* xy, int n, double tol) {
    /* every point in order; the expected on-curve points appear in order */
    int i, k = 0;
    for (i = 0; i < p->n_pts && k < n; i++)
        if (fabs(p->pts[2 * i] - xy[2 * k]) <= tol && fabs(p->pts[2 * i + 1] - xy[2 * k + 1]) <= tol) k++;
    return k == n;
}

static void t4_fonts(void) {
    bb tt = { 0, 0, 0 }, cf = { 0, 0, 0 }, cid = { 0, 0, 0 }, c2 = { 0, 0, 0 }, ttc = { 0, 0, 0 }, var = { 0, 0, 0 };
    yol_font f;
    yol_path p;
    char err[200];
    double adv, lsb;
    int rc, k;
    g_where = "T4 fonts";
    make_tt(&tt, 0, 0); make_cff(&cf, 0, 0); make_cff(&cid, 1, 0); make_cff(&c2, 0, 1); make_tt(&ttc, 0, 1); make_tt(&var, 1, 0);
    yol_path_init(&p, &g_cx);
    /* TrueType */
    CHECK(yol_font_view(&f, tt.d, tt.n, 0, err, sizeof err) == YOL_OK);
    CHECK(f.units_per_em == 1000 && f.n_glyphs == TT_NG && (f.flags & YOL_FONT_GLYF) && f.ascender == 800 && f.descender == -200 && f.line_gap == 90);
    CHECK(yol_font_glyph_index(&f, 'A') == 1 && yol_font_glyph_index(&f, 'B') == 2 && yol_font_glyph_index(&f, 'C') == 3 &&
          yol_font_glyph_index(&f, 'D') == 4 && yol_font_glyph_index(&f, 'E') == 0 && yol_font_glyph_index(&f, 0x1f600) == 0);
    CHECK(yol_font_hmetrics(&f, 2, &adv, &lsb) == YOL_OK && adv == 620 / 1000.0 && lsb == 100 / 1000.0);
    CHECK(yol_font_hmetrics(&f, 10, &adv, &lsb) == YOL_OK && adv == 640 / 1000.0 && lsb == -400 / 1000.0);
    /* HarfBuzz's placement: x moves by hmtx lsb - glyf xMin (the left
     * phantom point); a USE_MY_METRICS component's phantom wins */
    {
        int k2, found = 0;
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 12, &p, NULL) == YOL_OK && p.pts[0] == 0.07 && p.pts[1] == 0 && p.pts[4] == 0.27);
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 13, &p, NULL) == YOL_OK && p.pts[0] == 0.055 && p.pts[4] == 1.055);
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 14, &p, NULL) == YOL_OK && p.pts[0] == 0.27);
        for (k2 = 0; k2 < p.n_pts; k2++) if (p.pts[2 * k2] == -0.03 && p.pts[2 * k2 + 1] == 0) found = 1;
        CHECK(found);
        /* the curve set and exact alpha see the same outline */
        {
            yol_cset s;
            yol_cset_desc cd;
            float bx;
            static const uint32_t g12 = 12;
            memset(&cd, 0, sizeof cd);
            cd.n_glyphs = TT_NG;
            CHECK(yol_cset_open(&s, &g_cx, &cd) && yol_cset_add_font(&s, &f, &g12, 1, 0) == YOL_OK);
            memcpy(&bx, &s.words[s.words[8 + 12]], 4);
            CHECK(bx == 0.07f);
            yol_cset_close(&s);
        }
        {
            yol_raster_desc rd;
            yol_box bxx;
            yol_path_clear(&p);
            yol_font_glyph(&g_cx, &f, 12, &p, NULL);
            memset(&rd, 0, sizeof rd);
            rd.scale = 1000;
            CHECK(yol_raster(&g_cx, &p, &rd, &bxx) == YOL_OK && bxx.x0 == 70 && bxx.x1 == 270);
        }
    }
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, 0, &p, NULL) == YOL_OK && p.n_pts == 0);
    CHECK(yol_font_glyph(&g_cx, &f, 1, &p, NULL) == YOL_OK);
    {
        static const double want[18] = { 0, 0, 0, 0, 1, 0, 1, 0, 1, -1, 1, -1, 0, -1, 0, -1, 0, 0 };
        int same = p.n_contours == 2 && p.n_pts >= 9;
        for (k = 0; same && k < 18; k++) if (p.pts[k] != want[k] || (p.pts[k] == 0 && 1.0 / p.pts[k] < 0.0)) same = 0;   /* -0: MinGW's signbit() narrows to float */
        CHECK(same);
        CHECK(path_closed(&p) && yol_path_area(&p) == -0.75);
    }
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, 2, &p, NULL) == YOL_OK);
    CHECK(p.n_contours == 1 && path_segments(&p) == 8 && path_closed(&p));
    CHECK(fabs(p.pts[0] - 0.5 * (tt_g2x[0] + tt_g2x[1]) / 1000.0) < 1e-15 && fabs(p.pts[1] + 0.5 * (tt_g2y[0] + tt_g2y[1]) / 1000.0) < 1e-15);
    CHECK(fabs(p.pts[2] - tt_g2x[1] / 1000.0) < 1e-15 && fabs(p.pts[3] + tt_g2y[1] / 1000.0) < 1e-15);   /* the first control */
    CHECK(fabs(p.pts[4] - 0.5 * (tt_g2x[1] + tt_g2x[2]) / 1000.0) < 1e-15 && fabs(p.pts[5] + 0.5 * (tt_g2y[1] + tt_g2y[2]) / 1000.0) < 1e-15);   /* an implied point */
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, 4, &p, NULL) == YOL_OK);
    CHECKF(fabs(yol_path_area(&p) - 0.3525) < 1e-12, "composite area %.17g", yol_path_area(&p));
    /* (1000, 0) of glyph 1 under the 2 x 2: x = 0.5 1000 + 100, y = 0.25 1000 + 200 */
    CHECK(fabs(p.pts[4] - 0.6) < 1e-15 && fabs(p.pts[5] + 0.45) < 1e-15);
    {
        yol_path r;
        yol_path_init(&r, &g_cx);
        CHECK(yol_resolve(&g_cx, &p, &r) == YOL_OK);
        CHECKF(fabs(yol_path_area(&r) - 0.6675) < 1e-12, "composite resolved %.17g", yol_path_area(&r));
        yol_path_free(&r);
    }
    yol_path_clear(&p);
    rc = yol_font_glyph(&g_cx, &f, 5, &p, NULL);
    CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "deeper"));
    yol_path_clear(&p);
    rc = yol_font_glyph(&g_cx, &f, 6, &p, NULL);
    CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "point-matched"));
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, 7, &p, NULL) == YOL_OK);
    {
        /* on-curve points of glyph 7 (lines and quads, implied none: no two
         * off-curve points are adjacent) */
        double xy[24];
        int n = 0;
        for (k = 1; k < 12; k++) if (tt_g7on[k]) { xy[2 * n] = tt_g7x[k] / 1000.0; xy[2 * n + 1] = -tt_g7y[k] / 1000.0; n++; }
        CHECK(path_has_ends(&p, xy, n, 0));
        CHECK(path_closed(&p));
    }
    yol_path_clear(&p);
    rc = yol_font_glyph(&g_cx, &f, 8, &p, NULL);
    CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "composite components"));
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, 10, &p, NULL) == YOL_OK);
    yol_path_clear(&p);
    rc = yol_font_glyph(&g_cx, &f, 11, &p, NULL);
    CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "outline points"));
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, TT_NG, &p, NULL) == YOL_ERR_RANGE);
    /* size, pen, y up, and the path's transform */
    {
        yol_glyph_desc gd;
        memset(&gd, 0, sizeof gd);
        gd.size = 50; gd.x = 10; gd.y = 20; gd.y_up = true;
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 1, &p, &gd) == YOL_OK);
        CHECK(p.pts[0] == 10 && p.pts[1] == 20 && p.pts[4] == 60 && p.pts[9] == 70);
        CHECK(yol_path_area(&p) == 0.75 * 2500);
        yol_path_clear(&p);
        gd.axes = &adv; gd.n_axes = 1;
        rc = yol_font_glyph(&g_cx, &f, 1, &p, &gd);
        CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "gvar"));
    }
    /* collections, variable flag, refusals */
    CHECK(yol_font_faces(ttc.d, ttc.n) == 2 && yol_font_faces(tt.d, tt.n) == 1);
    CHECK(yol_font_view(&f, ttc.d, ttc.n, 1, err, sizeof err) == YOL_OK);
    yol_path_clear(&p);
    CHECK(yol_font_glyph(&g_cx, &f, 1, &p, NULL) == YOL_OK && yol_path_area(&p) == -0.75);
    CHECK(yol_font_view(&f, ttc.d, ttc.n, 2, err, sizeof err) == YOL_ERR_RANGE);
    CHECK(yol_font_view(&f, var.d, var.n, 0, err, sizeof err) == YOL_OK && (f.flags & YOL_FONT_VARIABLE));
    CHECK(yol_font_view(&f, c2.d, c2.n, 0, err, sizeof err) == YOL_ERR_REFUSED && strstr(err, "CFF2"));
    CHECK(yol_font_view(&f, tt.d, 11, 0, err, sizeof err) == YOL_ERR_FORMAT);
    /* CFF and CID-keyed CFF: the same charstrings, the same outlines */
    for (k = 0; k < 2; k++) {
        bb* fb = k ? &cid : &cf;
        int g;
        CHECK(yol_font_view(&f, fb->d, fb->n, 0, err, sizeof err) == YOL_OK);
        CHECK((f.flags & YOL_FONT_CFF) && ((f.flags & YOL_FONT_CID) != 0) == k && f.n_glyphs == CFF_NG);
        CHECK(yol_font_glyph_index(&f, 0x41) == 1 && yol_font_glyph_index(&f, 0x1f600) == 3 && yol_font_glyph_index(&f, 0x42) == 0);
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 0, &p, NULL) == YOL_OK && p.n_pts == 0);
        CHECK(yol_font_glyph(&g_cx, &f, 1, &p, NULL) == YOL_OK && path_closed(&p) && fabs(yol_path_area(&p) + 0.64) < 1e-15);
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 2, &p, NULL) == YOL_OK && path_closed(&p));
        {
            static const double e2[] = { 0, 0, 200.5, 200, 350.5, 260, 305.5, 110, 205.5, 50, 105.5, 33, 135.5, 63, 115.5, 43, 105.5, 43, 105.5, 13, 108.5, 16, 111.5, 19 };
            double xy[24];
            for (g = 0; g < 12; g++) { xy[2 * g] = e2[2 * g] / 1000.0; xy[2 * g + 1] = -e2[2 * g + 1] / 1000.0; }
            CHECK(path_has_ends(&p, xy, 12, 1e-15));
        }
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 3, &p, NULL) == YOL_OK && path_closed(&p));
        {
            static const double e3[] = { 100, 0, 100, 100, 125, 105, 145, 100, 148, 103, 145, 110, 160, 112, 175, 110 };
            double xy[16];
            for (g = 0; g < 8; g++) { xy[2 * g] = e3[2 * g] / 1000.0; xy[2 * g + 1] = -e3[2 * g + 1] / 1000.0; }
            CHECK(path_has_ends(&p, xy, 8, 1e-15));
        }
        yol_path_clear(&p);
        rc = yol_font_glyph(&g_cx, &f, 4, &p, NULL);
        CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "seac"));
        rc = yol_font_glyph(&g_cx, &f, 5, &p, NULL);
        CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "arithmetic"));
        rc = yol_font_glyph(&g_cx, &f, 6, &p, NULL);
        CHECK(rc == YOL_ERR_FORMAT && strstr(yol_error(&g_cx), "deeper"));
        rc = yol_font_glyph(&g_cx, &f, 7, &p, NULL);
        CHECK(rc == YOL_ERR_FORMAT && strstr(yol_error(&g_cx), "endchar"));
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 8, &p, NULL) == YOL_OK && path_segments(&p) == 10 + 1);
        rc = yol_font_glyph(&g_cx, &f, 9, &p, NULL);
        CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "tokens"));
        rc = yol_font_glyph(&g_cx, &f, 10, &p, NULL);
        CHECK(rc == YOL_ERR_FORMAT && strstr(yol_error(&g_cx), "stack"));
        rc = yol_font_glyph(&g_cx, &f, 11, &p, NULL);
        CHECK(rc == YOL_ERR_FORMAT && strstr(yol_error(&g_cx), "stems"));
        rc = yol_font_glyph(&g_cx, &f, 12, &p, NULL);
        CHECK(rc == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "outline points"));
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, 13, &p, NULL) == YOL_OK && path_segments(&p) == 3);   /* 10 deep: allowed */
        rc = yol_font_glyph(&g_cx, &f, 14, &p, NULL);
        CHECK(rc == YOL_ERR_FORMAT && strstr(yol_error(&g_cx), "deeper"));
    }
    printf("T4 fonts: TrueType, CFF, CID CFF, a collection and the refusals\n");
    yol_path_free(&p);
    free(tt.d); free(cf.d); free(cid.d); free(c2.d); free(ttc.d); free(var.d);
}

static void t5_fuzz(void) {
    bb fonts[3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    int k, i, opened = 0, glyphs_ok = 0, refused = 0;
    long outcomes[16];
    uint8_t* buf;
    yol_path p, r;
    yol_cset s;
    yol_cset_desc d;
    g_where = "T5 fuzz";
    memset(outcomes, 0, sizeof outcomes);
    make_tt(&fonts[0], 0, 0); make_cff(&fonts[1], 0, 0); make_cff(&fonts[2], 1, 0);
    yol_path_init(&p, &g_cx); yol_path_init(&r, &g_cx);
    for (k = 0; k < 3; k++) {
        size_t n0 = fonts[k].n;
        buf = (uint8_t*)malloc(n0);
        for (i = 0; i < 10000; i++) {
            yol_font f;
            size_t n = n0;
            int m, kind = (int)rndu(4), rc, g;
            memcpy(buf, fonts[k].d, n0);
            if (kind == 0) { for (m = 0; m < 1 + (int)rndu(8); m++) { size_t at = rndu((uint32_t)n0); buf[at] ^= (uint8_t)(1u << rndu(8)); } }
            else if (kind == 1) { for (m = 0; m < 1 + (int)rndu(3); m++) { size_t at = rndu((uint32_t)n0 - 4); static const uint32_t v[5] = { 0xffffffffu, 0x7fffffffu, 0x80000000u, 0, 0x0000ffffu }; uint32_t x = v[rndu(5)]; buf[at] = (uint8_t)(x >> 24); buf[at + 1] = (uint8_t)(x >> 16); buf[at + 2] = (uint8_t)(x >> 8); buf[at + 3] = (uint8_t)x; } }
            else if (kind == 2) n = rndu((uint32_t)n0);
            else { for (m = 0; m < 1 + (int)rndu(16); m++) buf[rndu((uint32_t)n0)] = (uint8_t)rndu(256); }
            rc = yol_font_view(&f, buf, n, 0, NULL, 0);
            outcomes[-rc & 15]++;
            if (rc < 0) continue;
            opened++;
            memset(&d, 0, sizeof d);
            d.n_glyphs = (uint32_t)(f.n_glyphs < 16 ? f.n_glyphs : 16);
            d.keep_overlaps = (i & 1) != 0;
            CHECK(yol_cset_open(&s, &g_cx, &d));
            for (g = 0; g < (int)d.n_glyphs; g++) {
                if (k == 0 && (g == 8 || g == 10 || g == 11)) continue;   /* slow on purpose */
                if (k > 0 && (g == 9 || g == 12)) continue;
                yol_path_clear(&p);
                rc = yol_font_glyph(&g_cx, &f, (uint32_t)g, &p, NULL);
                CHECK(rc == YOL_OK || rc == YOL_ERR_FORMAT || rc == YOL_ERR_REFUSED || rc == YOL_ERR_RANGE || rc == YOL_ERR_FULL);
                if (rc == YOL_ERR_REFUSED) refused++;
                if (rc < 0 || p.n_pts > 20000) continue;
                glyphs_ok++;
                yol_font_hmetrics(&f, (uint32_t)g, NULL, NULL);
                yol_font_glyph_index(&f, (uint32_t)rndu(0x20000));
                rc = yol_cset_add(&s, (uint32_t)g, &p);
                CHECK(rc == YOL_OK || rc == YOL_ERR_NUMERIC || rc == YOL_ERR_REFUSED);
            }
            if (d.n_glyphs) CHECK(cset_ok(&s));
            yol_cset_close(&s);
        }
        free(buf);
    }
    printf("T5 fuzz: 30000 mutated fonts, %d opened, %d glyphs built, %d refused; no crash\n", opened, glyphs_ok, refused);
    yol_path_free(&p); yol_path_free(&r);
    free(fonts[0].d); free(fonts[1].d); free(fonts[2].d);
}

/* --- random shapes -------------------------------------------------------------- */

static void random_contour(yol_path* p) {
    int kind = (int)rndu(4), n, i;
    if (kind == 0) {
        n = 3 + (int)rndu(6);
        yol_move(p, rnd(), rnd());
        for (i = 1; i < n; i++) yol_line(p, rnd(), rnd());
        yol_close_path(p);
    } else if (kind == 1) {
        double cx = rnd(), cy = rnd();
        yol_xform keep = p->xf;
        if (rnd() < 0.5) yol_set_xform(p, yol_xf_mul(yol_xf_translate(cx, cy), yol_xf_mul(yol_xf_scale(1, -1), yol_xf_translate(-cx, -cy))));
        yol_ellipse(p, cx, cy, 0.05 + 0.4 * rnd(), 0.05 + 0.4 * rnd());
        yol_set_xform(p, keep);
    } else if (kind == 2) {
        n = 2 + (int)rndu(5);
        yol_move(p, rnd(), rnd());
        for (i = 1; i < n; i++) yol_quad(p, rnd(), rnd(), rnd(), rnd());
        yol_close_path(p);
    } else {
        double x = rnd() * 0.6, y = rnd() * 0.6;
        yol_rect(p, x, y, 0.1 + 0.4 * rnd(), 0.1 + 0.4 * rnd(), rnd() < 0.5 ? 0 : 0.05, 0.05);
    }
}

static void random_shape(yol_path* p) {
    int n = 1 + (int)rndu(4), i;
    yol_path_clear(p);
    p->rule = rnd() < 0.3 ? YOL_EVENODD : YOL_NONZERO;
    for (i = 0; i < n; i++) random_contour(p);
    yol_path_end(p);
}

/* --- T7: resolve ------------------------------------------------------------------ */

static void t7_resolve(void) {
    yol_path p, r, r2;
    tqs bi = { 0, 0, 0 }, bo = { 0, 0, 0 };
    int t, k, wrong = 0, inside = 0;
    double worst = 0;
    g_where = "T7 resolve";
    yol_path_init(&p, &g_cx); yol_path_init(&r, &g_cx); yol_path_init(&r2, &g_cx);
    int nshapes = g_long ? 300 : 60;
    for (t = 0; t < nshapes; t++) {
        double ref, area, miny = 1e9, maxy = -1e9;
        int rc;
        random_shape(&p);
        rc = yol_resolve(&g_cx, &p, &r);
        CHECKF(rc == YOL_OK, "shape %d: %s", t, yol_error(&g_cx));
        if (rc < 0) continue;
        CHECK(path_closed(&r) && (r.flags & YOL_PATH_RESOLVED) && r.rule == YOL_NONZERO);
        path_quads(&p, &bi, 1, 0, 0);
        path_quads(&r, &bo, 1, 0, 0);
        for (k = 0; k < p.n_pts; k++) { miny = fmin(miny, p.pts[2 * k + 1]); maxy = fmax(maxy, p.pts[2 * k + 1]); }
        ref = ref_integral(&bi, -1e9, 1e9, miny - 0.01, maxy + 0.01, p.rule == YOL_EVENODD, 1e-13);
        area = yol_path_area(&r);
        worst = fmax(worst, fabs(area - ref));
        CHECKF(fabs(area - ref) <= 1e-10, "shape %d: area %.17g, reference %.17g", t, area, ref);
        for (k = 0; k < 200; k++) {
            double x = rnd() * 1.4 - 0.2, y = rnd() * 1.4 - 0.2;
            int wi = stable_winding(&bi, x, y, 1e-7), wo = stable_winding(&bo, x, y, 1e-7), in;
            if (wi == -99 || wo == -99) continue;
            in = p.rule == YOL_EVENODD ? (wi & 1) != 0 : wi != 0;
            if (wo != (in ? 1 : 0)) wrong++;
            inside += in;
        }
        rc = yol_resolve(&g_cx, &r, &r2);
        CHECK(rc == YOL_OK && fabs(yol_path_area(&r2) - area) <= 1e-14 && path_segments(&r2) <= path_segments(&r));
    }
    CHECKF(wrong == 0, "%d points with the wrong winding", wrong);
    printf("T7 resolve: %d random shapes, area within %.2g of the reference, %d inside points, %d wrong\n", nshapes, worst, inside, wrong);
    /* abutting rectangles: one rectangle; the same contour twice, opposite: empty */
    yol_path_clear(&p);
    yol_rect(&p, 0, 0, 1, 1, 0, 0); yol_rect(&p, 1, 0, 1, 1, 0, 0);
    CHECK(yol_resolve(&g_cx, &p, &r) == YOL_OK && r.n_contours == 1 && path_segments(&r) == 4 && yol_path_area(&r) == 2);
    yol_path_clear(&p);
    yol_move(&p, 0, 0); yol_line(&p, 1, 0); yol_line(&p, 1, 1); yol_close_path(&p);
    yol_move(&p, 0, 0); yol_line(&p, 1, 1); yol_line(&p, 1, 0); yol_close_path(&p);
    CHECK(yol_resolve(&g_cx, &p, &r) == YOL_OK && r.n_contours == 0);
    /* even-odd: nested squares turning the same way make a ring */
    yol_path_clear(&p);
    p.rule = YOL_EVENODD;
    yol_rect(&p, 0, 0, 4, 4, 0, 0); yol_rect(&p, 1, 1, 2, 2, 0, 0);
    CHECK(yol_resolve(&g_cx, &p, &r) == YOL_OK && yol_path_area(&r) == 12 && r.n_contours == 2);
    p.rule = YOL_NONZERO;
    CHECK(yol_resolve(&g_cx, &p, &r) == YOL_OK && yol_path_area(&r) == 16 && r.n_contours == 1);
    /* in place */
    CHECK(yol_resolve(&g_cx, &p, &p) == YOL_OK && yol_path_area(&p) == 16 && (p.flags & YOL_PATH_RESOLVED));
    /* v0.3.1: crossing strokes whose edges are a few ulps of float from
     * horizontal or vertical (each start rounded to float, the other corners
     * not: the path gcc 11.4 -O3 built from gfx_test's grid glyph). A nearly
     * horizontal edge's x at a crossing is off by its |dx/dy| times y's
     * rounding, past eps_x; v0.3.0 failed "the boundary does not close". */
    {
        int g, ng = g_long ? 400 : 100, bad = 0, nbad_area = 0;
        double worst_g = 0;
        for (g = 0; g < ng; g++) {
            int i;
            double ref, area, miny = 1e9, maxy = -1e9;
            yol_path_clear(&p);
            for (i = 0; i < 4; i++) {
                /* g 0: the glyph itself; then random bars across random stems */
                double x0, y0, x1, y1;
                if (g == 0) {
                    x0 = i < 2 ? 0.05 : 0.2 + (i - 2) * 0.4; x1 = i < 2 ? 0.85 : 0.27 + (i - 2) * 0.4;
                    y0 = i < 2 ? -0.65 + i * 0.4 : -0.8; y1 = i < 2 ? -0.58 + i * 0.4 : 0.0;
                } else if (i < 2) {
                    x0 = 0.1 * rnd(); x1 = 0.8 + 0.2 * rnd(); y0 = rnd() * 0.8; y1 = y0 + 0.02 + 0.1 * rnd();
                } else {
                    y0 = 0.1 * rnd(); y1 = 0.8 + 0.2 * rnd(); x0 = rnd() * 0.8; x1 = x0 + 0.02 + 0.1 * rnd();
                }
                yol_move(&p, (float)x0, (float)y0);
                yol_line(&p, x1, y0); yol_line(&p, x1, y1); yol_line(&p, x0, y1); yol_line(&p, x0, y0);
                yol_close_path(&p);
            }
            yol_path_end(&p);
            k = yol_resolve(&g_cx, &p, &r);
            if (k != YOL_OK) {
                if (bad++ < 3) fprintf(stderr, "T7 grid %d: %s\n", g, yol_error(&g_cx));
                continue;
            }
            path_quads(&p, &bi, 1, 0, 0);
            for (i = 0; i < p.n_pts; i++) { miny = fmin(miny, p.pts[2 * i + 1]); maxy = fmax(maxy, p.pts[2 * i + 1]); }
            ref = ref_integral(&bi, -1e9, 1e9, miny - 0.01, maxy + 0.01, YOL_NONZERO, 1e-13);
            area = yol_path_area(&r);
            worst_g = fmax(worst_g, fabs(area - ref));
            if (!(fabs(area - ref) <= 1e-12) || !path_closed(&r))
                if (nbad_area++ < 3) fprintf(stderr, "T7 grid %d: area %.17g, reference %.17g\n", g, area, ref);
        }
        CHECKF(bad == 0, "%d of %d grids not resolved", bad, ng);
        CHECKF(nbad_area == 0, "%d grids with the wrong area", nbad_area);
        printf("T7 resolve: %d grids with float-rounded starts, %d refused, area within %.2g of the reference\n", ng, bad, worst_g);
    }
    /* the work bound: 600 random chords cross about 90000 times */
    yol_path_clear(&p);
    yol_move(&p, 0, 0);
    for (k = 0; k < 600; k++) yol_line(&p, (k & 1) ? 1 + rnd() : -rnd(), rnd());
    yol_close_path(&p);
    k = yol_resolve(&g_cx, &p, &r);
    CHECK(k == YOL_ERR_REFUSED && strstr(yol_error(&g_cx), "complex"));
    free(bi.q); free(bo.q);
    yol_path_free(&p); yol_path_free(&r); yol_path_free(&r2);
}

/* --- T8: exact alpha ------------------------------------------------------------- */

static double g_t8_worst;
static long g_t8_px;

static void raster_check(const yol_path* p, double scale, double ox, double oy, const char* what) {
    yol_raster_desc d;
    yol_box box;
    tqs b = { 0, 0, 0 };
    double *img, sum = 0, area, worst = 0;
    uint8_t* u8;
    uint16_t* u16;
    float* f32;
    int W, H, x, y, ok = 1;
    memset(&d, 0, sizeof d);
    d.scale = scale; d.x = ox; d.y = oy;
    CHECK(yol_raster(&g_cx, p, &d, &box) == YOL_OK);
    W = box.x1 - box.x0 + 2; H = box.y1 - box.y0 + 2;     /* a 1 px border that must stay 0 */
    img = (double*)calloc((size_t)W * H, sizeof(double));
    u8 = (uint8_t*)calloc((size_t)W * H, 1); u16 = (uint16_t*)calloc((size_t)W * H, 2); f32 = (float*)calloc((size_t)W * H, 4);
    d.x = ox - box.x0 + 1; d.y = oy - box.y0 + 1; d.w = W; d.h = H;
    d.out = img; d.format = YOL_ALPHA_F64; d.stride = W * 8;
    CHECK(yol_raster(&g_cx, p, &d, NULL) == YOL_OK);
    d.out = u8; d.format = YOL_ALPHA_U8; d.stride = W;
    CHECK(yol_raster(&g_cx, p, &d, NULL) == YOL_OK);
    d.out = u16; d.format = YOL_ALPHA_U16; d.stride = W * 2;
    CHECK(yol_raster(&g_cx, p, &d, NULL) == YOL_OK);
    d.out = f32; d.format = YOL_ALPHA_F32; d.stride = W * 4;
    CHECK(yol_raster(&g_cx, p, &d, NULL) == YOL_OK);
    path_quads(p, &b, scale, d.x, d.y);
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
        double v = img[y * W + x], ref = ref_integral(&b, x, x + 1, y, y + 1, p->rule == YOL_EVENODD, 1e-13);
        sum += v;
        worst = fmax(worst, fabs(v - ref));
        if ((x == 0 || y == 0 || x == W - 1 || y == H - 1) && v != 0) { if (ok) fprintf(stderr, "  border pixel %d %d of %dx%d: %.17g\n", x, y, W, H, v); ok = 0; }
        if (u8[y * W + x] != (uint8_t)floor(v * 255 + 0.5) || u16[y * W + x] != (uint16_t)floor(v * 65535 + 0.5) || f32[y * W + x] != (float)v) { if (ok) fprintf(stderr, "  codes at %d %d: %.17g -> %u %u %.9g\n", x, y, v, u8[y * W + x], u16[y * W + x], f32[y * W + x]); ok = 0; }
        if (v < 0 || v > 1) { if (ok) fprintf(stderr, "  range at %d %d: %.17g\n", x, y, v); ok = 0; }
    }
    {
        yol_path r;
        yol_path_init(&r, &g_cx);
        CHECK(yol_resolve(&g_cx, p, &r) == YOL_OK);
        area = yol_path_area(&r) * scale * scale;
        yol_path_free(&r);
    }
    {
        double* tight = (double*)calloc((size_t)(W - 2) * (H - 2), sizeof(double));
        yol_raster_desc t2 = d;
        int same = 1;
        t2.x = d.x - 1; t2.y = d.y - 1; t2.w = W - 2; t2.h = H - 2; t2.out = tight; t2.format = YOL_ALPHA_F64; t2.stride = (W - 2) * 8;
        CHECK(yol_raster(&g_cx, p, &t2, NULL) == YOL_OK);
        for (y = 0; y < H - 2; y++) for (x = 0; x < W - 2; x++) if (fabs(tight[y * (W - 2) + x] - img[(y + 1) * W + x + 1]) > 1e-14) same = 0;
        CHECKF(same, "%s: the image without a border differs", what);
        free(tight);
    }
    CHECKF(ok, "%s: border, range or stored codes", what);
    CHECKF(worst <= 1e-9, "%s at %g px: %.3g from the reference", what, scale, worst);
    CHECKF(fabs(sum - area) <= 1e-10 * (1 + area), "%s: coverage sum %.17g, area %.17g", what, sum, area);
    g_t8_worst = fmax(g_t8_worst, worst);
    g_t8_px += (long)W * H;
    free(img); free(u8); free(u16); free(f32); free(b.q);
}

static void t8_raster(void) {
    bb tt = { 0, 0, 0 }, cf = { 0, 0, 0 };
    yol_font f;
    yol_path p;
    static const int ttg[5] = { 1, 2, 3, 4, 7 };
    static const double sizes[3] = { 7.3, 12, 24 };
    int i, s;
    g_where = "T8 raster";
    make_tt(&tt, 0, 0); make_cff(&cf, 0, 0);
    yol_path_init(&p, &g_cx);
    CHECK(yol_font_view(&f, tt.d, tt.n, 0, NULL, 0) == YOL_OK);
    for (i = 0; i < 5; i++) for (s = g_long ? 0 : 1; s < 3; s += g_long ? 1 : 2) {
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, (uint32_t)ttg[i], &p, NULL) == YOL_OK);
        raster_check(&p, sizes[s], rnd(), rnd(), "TrueType glyph");
    }
    CHECK(yol_font_view(&f, cf.d, cf.n, 0, NULL, 0) == YOL_OK);
    for (i = 1; i <= 3; i++) for (s = g_long ? 0 : 1; s < (g_long ? 3 : 2); s++) {
        yol_path_clear(&p);
        CHECK(yol_font_glyph(&g_cx, &f, (uint32_t)i, &p, NULL) == YOL_OK);
        raster_check(&p, sizes[s], rnd(), rnd(), "CFF glyph");
    }
    for (i = 0; i < (g_long ? 20 : 6); i++) {
        random_shape(&p);
        raster_check(&p, 10 + 20 * rnd(), rnd() * 3, rnd() * 3, "random shape");
    }
    /* arguments */
    {
        yol_raster_desc d;
        memset(&d, 0, sizeof d);
        CHECK(yol_raster(&g_cx, &p, &d, NULL) == YOL_ERR_ARG);
        d.scale = 1; d.format = 9;
        CHECK(yol_raster(&g_cx, &p, &d, NULL) == YOL_ERR_ARG);
        d.format = 0; d.out = &d; d.w = 4; d.h = 4; d.stride = 3;
        CHECK(yol_raster(&g_cx, &p, &d, NULL) == YOL_ERR_ARG);
    }
    printf("T8 raster: %ld pixels against the reference, worst %.2g\n", g_t8_px, g_t8_worst);
    yol_path_free(&p);
    free(tt.d); free(cf.d);
}

/* --- T2, T3: curve sets and winding agreement --------------------------------------- */

static void cset_winding_check(const yol_cset* s, uint32_t g, const yol_path* p, int expect_resolved) {
    ygfx_cset_desc d = cset_desc(s);
    tqs b = { 0, 0, 0 };
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    int k, bad = 0, n = 0;
    path_quads(p, &b, 1, 0, 0);
    for (k = 0; k < p->n_pts; k++) { x0 = fmin(x0, p->pts[2 * k]); x1 = fmax(x1, p->pts[2 * k]); y0 = fmin(y0, p->pts[2 * k + 1]); y1 = fmax(y1, p->pts[2 * k + 1]); }
    for (k = 0; k < (g_long ? 2000 : 400); k++) {
        double x = x0 + (x1 - x0) * (1.2 * rnd() - 0.1), y = y0 + (y1 - y0) * (1.2 * rnd() - 0.1);
        int w = stable_winding(&b, x, y, 1e-5 * (x1 - x0 + y1 - y0)), wg;
        if (w == -99) continue;
        wg = ygfx_cset_winding(&d, g, x, y);
        n++;
        if (expect_resolved ? (wg != w || (w != 0 && w != 1)) : wg != w) bad++;
    }
    CHECKF(bad == 0, "glyph %u: %d of %d points disagree with ygfx_cset_winding", g, bad, n);
    free(b.q);
}

/* Every band lists exactly the curves whose y (x) range meets the band
 * widened by h / 256, sorted as the format says: recomputed from the
 * texels, independent of the writer. The curves are those any band lists. */
static void check_bands(const yol_cset* s, uint32_t g) {
    uint32_t o = s->words[8 + g], nh = s->words[o + 4] & 0xffffu, nv = s->words[o + 4] >> 16, b, j, k;
    uint32_t* refs = (uint32_t*)malloc(((size_t)s->words[o + 7] + 1) * sizeof(uint32_t)), nref = 0;
    float bbx[4];
    int bad = 0;
    memcpy(bbx, &s->words[o], 16);
    for (b = 0; b < nh + nv; b++) {
        uint32_t off = s->words[o + 8 + 4 * b], cnt = s->words[o + 9 + 4 * b];
        for (j = 0; j < cnt; j++) {
            uint32_t r = s->words[off + j];
            for (k = 0; k < nref && refs[k] != r; k++) {}
            if (k == nref) refs[nref++] = r;
        }
    }
    for (b = 0; b < nh + nv; b++) {
        int vert = b >= nh;
        uint32_t kb = vert ? b - nh : b, nb = vert ? nv : nh, off = s->words[o + 8 + 4 * b], cnt = s->words[o + 9 + 4 * b], want = 0;
        double lo0 = vert ? bbx[0] : bbx[1], hi0 = vert ? bbx[2] : bbx[3], h = (hi0 - lo0) / nb, lo = lo0 + kb * h - h / 256, hi = lo0 + kb * h + h + h / 256;
        double prev = HUGE_VAL;
        for (k = 0; k < nref; k++) {
            const float* t = s->texels + 4 * refs[k];
            double mn = vert ? fmin(t[0], fmin(t[2], t[4])) : fmin(t[1], fmin(t[3], t[5]));
            double mx = vert ? fmax(t[0], fmax(t[2], t[4])) : fmax(t[1], fmax(t[3], t[5]));
            int listed = 0;
            for (j = 0; j < cnt; j++) if (s->words[off + j] == refs[k]) listed = 1;
            if ((mx >= lo && mn <= hi) != listed) bad++;
            want += mx >= lo && mn <= hi;
        }
        for (j = 0; j < cnt; j++) {
            const float* t = s->texels + 4 * s->words[off + j];
            double key = vert ? fmax(t[1], fmax(t[3], t[5])) : fmax(t[0], fmax(t[2], t[4]));
            if (key > prev) bad++;
            prev = key;
        }
        if (want != cnt) bad++;
    }
    CHECKF(bad == 0, "glyph %u: %d band membership or order errors", g, bad);
    free(refs);
}

static void t2_csets(void) {
    bb tt = { 0, 0, 0 }, cf = { 0, 0, 0 };
    yol_font f;
    yol_path p, r;
    static const uint32_t gl[6] = { 1, 2, 3, 4, 7, 10 };
    int i, keep;
    g_where = "T2 curve sets";
    make_tt(&tt, 0, 0); make_cff(&cf, 1, 0);
    yol_path_init(&p, &g_cx); yol_path_init(&r, &g_cx);
    CHECK(yol_font_view(&f, tt.d, tt.n, 0, NULL, 0) == YOL_OK);
    for (keep = 0; keep < 2; keep++) {
        yol_cset s;
        yol_cset_desc d;
        memset(&d, 0, sizeof d);
        d.n_glyphs = TT_NG; d.keep_overlaps = keep != 0; d.backward = keep != 0;
        CHECK(yol_cset_open(&s, &g_cx, &d));
        for (i = 0; i < 6; i++) {
            yol_path_clear(&p);
            CHECK(yol_font_glyph(&g_cx, &f, gl[i], &p, NULL) == YOL_OK);
            CHECK(yol_cset_add(&s, gl[i], &p) == YOL_OK);
        }
        CHECK(cset_ok(&s));
        for (i = 0; i < 6; i++) {
            uint32_t o = s.words[8 + gl[i]];
            yol_path_clear(&p);
            yol_font_glyph(&g_cx, &f, gl[i], &p, NULL);
            if (!keep) { CHECK(yol_resolve(&g_cx, &p, &r) == YOL_OK); cset_winding_check(&s, gl[i], &r, 1); }
            else cset_winding_check(&s, gl[i], &p, 0);
            CHECK(((s.words[o + 5] & 0x4u) != 0) == !keep && ((s.words[o + 5] & 0x2u) != 0) == (keep != 0));
            check_bands(&s, gl[i]);
        }
        yol_cset_close(&s);
    }
    /* band counts: explicit and the default formula on a glyph of 9 curves */
    {
        yol_cset s;
        yol_cset_desc d;
        memset(&d, 0, sizeof d);
        d.n_glyphs = 3; d.keep_overlaps = true;
        CHECK(yol_cset_open(&s, &g_cx, &d));
        yol_path_clear(&p);
        yol_move(&p, 0, 0);
        for (i = 1; i < 7; i++) yol_line(&p, cos(i * 0.8975979), sin(i * 0.8975979));
        yol_close_path(&p);
        yol_path_end(&p);
        CHECK(yol_cset_add(&s, 0, &p) == YOL_OK);
        CHECK(s.words[s.words[8] + 4] == (3u | 3u << 16));       /* round(sqrt(7)), not floor */
        yol_path_clear(&p);                                        /* empty glyph */
        CHECK(yol_cset_add(&s, 1, &p) == YOL_OK && s.words[s.words[9] + 4] == 0);
        CHECK(cset_ok(&s));
        yol_cset_close(&s);
        d.nh = 5; d.nv = 7;
        CHECK(yol_cset_open(&s, &g_cx, &d));
        yol_path_clear(&p);
        yol_rect(&p, 0, 0, 1, 1, 0.2, 0.2);
        CHECK(yol_cset_add(&s, 2, &p) == YOL_OK && s.words[s.words[10] + 4] == (5u | 7u << 16) && cset_ok(&s));
        cset_winding_check(&s, 2, &p, 0);
        yol_cset_close(&s);
    }
    /* whole fonts through yol_cset_add_font, CFF CID included */
    {
        yol_cset s;
        yol_cset_desc d;
        static const uint32_t cg[4] = { 0, 1, 2, 3 };
        memset(&d, 0, sizeof d);
        CHECK(yol_font_view(&f, cf.d, cf.n, 0, NULL, 0) == YOL_OK);
        d.n_glyphs = (uint32_t)f.n_glyphs;
        CHECK(yol_cset_open(&s, &g_cx, &d));
        CHECK(yol_cset_add_font(&s, &f, cg, 4, 0) == YOL_OK && cset_ok(&s));
        CHECK(yol_cset_add_font(&s, &f, cg + 3, 1, 0) == YOL_ERR_ARG);     /* twice */
        yol_cset_close(&s);
    }
    printf("T2 curve sets: checked by ygfx_cset_check, winding equal to ygfx_cset_winding\n");
    yol_path_free(&p); yol_path_free(&r);
    free(tt.d); free(cf.d);
}

/* --- T9: strokes -------------------------------------------------------------------- */

/* Inside the band of a quadratic with butt ends: some foot point B(t), t in
 * [0, 1], within h. margin gets the distance to the nearest band edge, so a
 * point within the tolerance of it can be skipped. */
static int band_ref(const tq* q, double px, double py, double h, double* margin) {
    /* g(t) = (B(t) - p) . B'(t) is a cubic: its roots are every foot point.
     * The roots of g' cut [0, 1] into monotone pieces; each sign change is
     * bisected. Exact up to rounding, however close two roots are. */
    double Dx = 2 * (q->x1 - q->x0), Dy = 2 * (q->y1 - q->y0), Ex = q->x2 - 2 * q->x1 + q->x0, Ey = q->y2 - 2 * q->y1 + q->y0;
    double Px = q->x0 - px, Py = q->y0 - py;
    double c0 = Px * Dx + Py * Dy, c1 = Dx * Dx + Dy * Dy + 2 * (Px * Ex + Py * Ey), c2 = 3 * (Dx * Ex + Dy * Ey), c3 = 2 * (Ex * Ex + Ey * Ey);
    double cut[4], best = 1e300;
    int nc = 0, i, in = 0;
    cut[nc++] = 0;
    {   /* g' = c1 + 2 c2 t + 3 c3 t^2 */
        double a = 3 * c3, b = 2 * c2, c = c1, r[2];
        int n = 0;
        if (a != 0) {
            double d = b * b - 4 * a * c;
            if (d > 0) { double s = sqrt(d); r[n++] = (-b - s) / (2 * a); r[n++] = (-b + s) / (2 * a); }
        } else if (b != 0) r[n++] = -c / b;
        for (i = 0; i < n; i++) if (r[i] > 0 && r[i] < 1) cut[nc++] = r[i];
        if (nc == 3 && cut[1] > cut[2]) { double t = cut[1]; cut[1] = cut[2]; cut[2] = t; }
    }
    cut[nc++] = 1;
    for (i = 0; i <= nc - 1; i++) {
        /* the ends of [0, 1]: the butt ends' normal lines */
        if (i == 0 || i == nc - 1) {
            double t = cut[i], bx = q->x0 + t * Dx + t * t * Ex, by = q->y0 + t * Dy + t * t * Ey, tx = Dx + 2 * t * Ex, ty = Dy + 2 * t * Ey;
            double L = sqrt(tx * tx + ty * ty), g = (bx - px) * tx + (by - py) * ty, dd = sqrt((bx - px) * (bx - px) + (by - py) * (by - py));
            if (L > 0 && dd <= h + 1e-3) best = fmin(best, fabs(g) / L);
        }
    }
    for (i = 0; i + 1 < nc; i++) {
        double a = cut[i], b = cut[i + 1], ga = c0 + a * (c1 + a * (c2 + a * c3)), gb = c0 + b * (c1 + b * (c2 + b * c3));
        int it;
        if ((ga > 0) == (gb > 0) || ga == 0 || gb == 0) continue;
        for (it = 0; it < 100 && b - a > 1e-17; it++) {
            double m = 0.5 * (a + b), gm = c0 + m * (c1 + m * (c2 + m * c3));
            if ((gm > 0) == (ga > 0)) { a = m; ga = gm; } else b = m;
        }
        {
            double t = 0.5 * (a + b), bx = q->x0 + t * Dx + t * t * Ex, by = q->y0 + t * Dy + t * t * Ey;
            double d = sqrt((bx - px) * (bx - px) + (by - py) * (by - py));
            best = fmin(best, fabs(d - h));
            if (d <= h) in = 1;
        }
    }
    *margin = best;
    return in;
}

/* the distance from a point to a quadratic: samples, then Newton */
static double dist_tq(const tq* q, double px, double py) {
    double best = 1e300, bt = 0;
    int i, it;
    for (i = 0; i <= 32; i++) { double t = i / 32.0, d = (qx(q, t) - px) * (qx(q, t) - px) + (qy(q, t) - py) * (qy(q, t) - py); if (d < best) { best = d; bt = t; } }
    for (it = 0; it < 20; it++) {
        double t = bt, x = qx(q, t), y = qy(q, t);
        double dx = 2 * ((1 - t) * (q->x1 - q->x0) + t * (q->x2 - q->x1)), dy = 2 * ((1 - t) * (q->y1 - q->y0) + t * (q->y2 - q->y1));
        double ddx = 2 * (q->x2 - 2 * q->x1 + q->x0), ddy = 2 * (q->y2 - 2 * q->y1 + q->y0);
        double f = (x - px) * dx + (y - py) * dy, fp = dx * dx + dy * dy + (x - px) * ddx + (y - py) * ddy;
        if (fp <= 0) break;
        bt = t - f / fp; if (bt < 0) bt = 0; if (bt > 1) bt = 1;
    }
    best = fmin(best, (qx(q, bt) - px) * (qx(q, bt) - px) + (qy(q, bt) - py) * (qy(q, bt) - py));
    return sqrt(best);
}

static yol_stroke_desc sd(double width, int join, int cap, int mode, double tol) {
    yol_stroke_desc d;
    memset(&d, 0, sizeof d);
    d.width = width; d.join = join; d.cap = cap; d.mode = mode; d.tol = tol;
    return d;
}

static void t9_strokes(void) {
    yol_path p, o;
    tqs b = { 0, 0, 0 };
    int t, wrong = 0, total = 0, failed_curves = 0;
    double px = 20;
    g_where = "T9 strokes";
    yol_path_init(&p, &g_cx); yol_path_init(&o, &g_cx);
    /* 400 random quadratics, butt ends: every pixel center against the band */
    for (t = 0; t < 400; t++) {
        tq q;
        double h;
        int x, y, rc, bad = 0;
        yol_stroke_desc d;
        q.x0 = rnd() * 2 - 1; q.y0 = rnd() * 2 - 1; q.x1 = rnd() * 2 - 1; q.y1 = rnd() * 2 - 1; q.x2 = rnd() * 2 - 1; q.y2 = rnd() * 2 - 1;
        h = 0.02 + 0.3 * rnd();
        yol_path_clear(&p);
        yol_move(&p, q.x0, q.y0); yol_quad(&p, q.x1, q.y1, q.x2, q.y2);
        yol_path_end(&p);
        d = sd(2 * h, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 1e-5);
        rc = yol_stroke(&g_cx, &p, &o, &d);
        CHECKF(rc == YOL_OK, "curve %d: %s", t, yol_error(&g_cx));
        if (rc < 0) continue;
        CHECK(path_closed(&o) && (o.flags & YOL_PATH_RESOLVED));
        path_quads(&o, &b, 1, 0, 0);
        for (y = (int)floor(-1.4 * px); y < 1.4 * px; y++) for (x = (int)floor(-1.4 * px); x < 1.4 * px; x++) {
            double cx = (x + 0.5) / px, cy = (y + 0.5) / px, mg;
            int ref = band_ref(&q, cx, cy, h, &mg), w;
            if (mg < 3e-4) continue;
            w = winding(&b, cx, cy);
            total++;
            if ((w != 0) != ref || (w != 0 && w != 1)) { wrong++; bad++; }
        }
        if (bad) { failed_curves++; fprintf(stderr, "  curve %d (%.17g %.17g %.17g %.17g %.17g %.17g) h %.17g: %d wrong\n", t, q.x0, q.y0, q.x1, q.y1, q.x2, q.y2, h, bad); }
    }
    CHECKF(wrong == 0, "%d wrong pixel centers in %d curves", wrong, failed_curves);
    printf("T9 strokes: 400 random quadratics, %d wrong of %d pixel centers\n", wrong, total);
    /* Regressions (docs/outline.md): three curves the phase-1 prototype's
     * half bands folded on (5, 2 and 1 wrong centers at 50 px), two whose
     * resolved stroke did not close, and a near cusp. 50 px per unit. */
    {
        static const double cases[6][7] = {
            { -0.78106021301919615, 0.61320841090121148, -0.77391888180181279, -0.90289010284737692, -0.9353007599108859, -0.71001312295907471, 0.26297921689504683 },
            { 0.44456312753685112, -0.96569719534897913, -0.44737083040864289, 0.6964018677327799, -0.51536606952116459, 0.51542710654011659, 0.18125675222022156 },
            { -0.76970732749412518, 0.90923795281838427, -0.65294351023895991, 0.71044038209173865, 0.28769798883022557, 0.4592120120853298, 0.24787255470442821 },
            { 0.94506668294320506, -0.66380809961241494, 0.84160893581957463, 0.4327829828791161, -0.093478194524979408, 0.035126804406872658, 0.038558305612353891 },
            { 0.62565996276741842, 0.8334299752800074, 0.24253059480574968, 0.022614215521713943, -0.65355388042847995, 0.32297738578447821, 0.27922116763817256 },
            /* a near cusp (curvature radius 1.7e-9): v0.1 before the 30-degree
             * split got 2 centers wrong */
            { 0.49716974912315237, -0.13147667190097878, 0.64310141567152002, 0.29364400703822802, 0.23550519873225606, -0.8933008253210426, 0.31477612913031677 } };
        int c, x, y, bad = 0;
        for (c = 0; c < 6; c++) {
            tq q;
            yol_stroke_desc d = sd(2 * cases[c][6], YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 1e-5);
            q.x0 = cases[c][0]; q.y0 = cases[c][1]; q.x1 = cases[c][2]; q.y1 = cases[c][3]; q.x2 = cases[c][4]; q.y2 = cases[c][5];
            yol_path_clear(&p);
            yol_move(&p, q.x0, q.y0); yol_quad(&p, q.x1, q.y1, q.x2, q.y2);
            yol_path_end(&p);
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK && path_closed(&o));
            path_quads(&o, &b, 1, 0, 0);
            for (y = -70; y < 70; y++) for (x = -70; x < 70; x++) {
                double cx = (x + 0.5) / 50, cy = (y + 0.5) / 50, mg;
                int ref = band_ref(&q, cx, cy, cases[c][6], &mg), w;
                if (mg < 3e-4) continue;
                w = winding(&b, cx, cy);
                if ((w != 0) != ref || (w != 0 && w != 1)) bad++;
            }
        }
        CHECKF(bad == 0, "prototype regressions: %d wrong centers", bad);
    }
    /* The glyph the prototype's approximation hung on (Segoe UI glyph 2522,
     * em, y down), outlined at 0.08 em: it finishes, closes, and is the set
     * within 0.04 of the contours. */
    {
        static const double hang_pts[] = {
            0, 0.0068359375, -0.01708984375, 0.0068359375, -0.025634765625, -0.005859375,
            -0.0341796875, -0.0185546875, -0.0341796875, -0.0341796875, -0.0341796875, -0.0498046875,
            -0.025634765625, -0.062255859375, -0.01708984375, -0.07470703125, 0, -0.07470703125,
            0, -0.07470703125, 0.14453125, -0.07470703125, 0.19189453125, -0.07470703125,
            0.22314453125, -0.080078125, 0.25439453125, -0.08544921875, 0.27294921875, -0.095703125,
            0.29150390625, -0.10595703125, 0.298828125, -0.121337890625, 0.30615234375, -0.13671875,
            0.30615234375, -0.15673828125, 0.30615234375, -0.19384765625, 0.28466796875, -0.22802734375,
            0.26318359375, -0.26220703125, 0.22314453125, -0.294921875, 0.18310546875, -0.32763671875,
            0.126708984375, -0.360107421875, 0.0703125, -0.392578125, 0, -0.4267578125,
            0, -0.4267578125, 0, -0.484375, 0, -0.484375,
            0.41162109375, -0.7509765625, 0.41162109375, -0.7509765625, 0.41162109375, -0.6650390625,
            0.41162109375, -0.6650390625, 0.08642578125, -0.46142578125, 0.154296875, -0.42919921875,
            0.209228515625, -0.3955078125, 0.26416015625, -0.36181640625, 0.302734375, -0.324951171875,
            0.34130859375, -0.2880859375, 0.3623046875, -0.24755859375, 0.38330078125, -0.20703125,
            0.38330078125, -0.16162109375, 0.38330078125, -0.14697265625, 0.380615234375, -0.12890625,
            0.3779296875, -0.11083984375, 0.369384765625, -0.092041015625, 0.36083984375, -0.0732421875,
            0.3447265625, -0.055419921875, 0.32861328125, -0.03759765625, 0.302001953125, -0.023681640625,
            0.275390625, -0.009765625, 0.236328125, -0.00146484375, 0.197265625, 0.0068359375,
            0.14306640625, 0.0068359375, 0.14306640625, 0.0068359375, 0, 0.0068359375,
            0.28955078125, -0.6171875, 0.3212890625, -0.6171875, 0.34423828125, -0.594482421875,
            0.3671875, -0.57177734375, 0.3671875, -0.5380859375, 0.3671875, -0.50634765625,
            0.34423828125, -0.4833984375, 0.3212890625, -0.46044921875, 0.28955078125, -0.46044921875,
            0.255859375, -0.46044921875, 0.233154296875, -0.4833984375, 0.21044921875, -0.50634765625,
            0.21044921875, -0.5380859375, 0.21044921875, -0.57177734375, 0.233154296875, -0.594482421875,
            0.255859375, -0.6171875, 0.28955078125, -0.6171875, 0.28955078125, -0.4970703125,
            0.306640625, -0.4970703125, 0.318603515625, -0.509033203125, 0.33056640625, -0.52099609375,
            0.33056640625, -0.5380859375, 0.33056640625, -0.55615234375, 0.318603515625, -0.568359375,
            0.306640625, -0.58056640625, 0.28955078125, -0.58056640625, 0.271484375, -0.58056640625,
            0.25927734375, -0.568359375, 0.2470703125, -0.55615234375, 0.2470703125, -0.5380859375,
            0.2470703125, -0.52099609375, 0.25927734375, -0.509033203125, 0.271484375, -0.4970703125,
            0.28955078125, -0.4970703125,
        };
        static const uint32_t hang_contours[] = {
            0, 57, 74,
        };
        yol_stroke_desc d = sd(0.08, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 1e-5);
        yol_path h;
        tqs g = { 0, 0, 0 };
        int x, y, bad = 0, n = 0, k;
        yol_path_init(&h, NULL);
        h.pts = (double*)hang_pts; h.n_pts = h.cap_pts = (int)(sizeof hang_pts / sizeof hang_pts[0] / 2);
        h.contours = (uint32_t*)hang_contours; h.n_contours = h.cap_contours = (int)(sizeof hang_contours / sizeof hang_contours[0]);
        CHECK(yol_stroke(&g_cx, &h, &o, &d) == YOL_OK && path_closed(&o));
        path_quads(&h, &g, 1, 0, 0);
        path_quads(&o, &b, 1, 0, 0);
        for (y = -800; y < 100; y += 3) for (x = -100; x < 700; x += 3) {
            double cx = (x + 0.5) / 1000, cy = (y + 0.5) / 1000, dm = 1e9;
            int w;
            for (k = 0; k < g.n; k++) dm = fmin(dm, dist_tq(&g.q[k], cx, cy));
            if (fabs(dm - 0.04) < 3e-5) continue;
            w = winding(&b, cx, cy);
            n++;
            if ((w != 0) != (dm <= 0.04) || (w != 0 && w != 1)) bad++;
        }
        CHECKF(bad == 0, "the prototype's hang glyph: %d wrong of %d", bad, n);
        free(g.q);
    }
    /* joins and caps by area: an open L of two unit lines, width w */
    {
        double w = 0.1, h = 0.05, pi = 3.14159265358979323846;
        struct { int join, cap; double area; } c[] = {
            { YOL_JOIN_MITER, YOL_CAP_BUTT, 0 }, { YOL_JOIN_BEVEL, YOL_CAP_BUTT, 0 }, { YOL_JOIN_ROUND, YOL_CAP_BUTT, 0 },
            { YOL_JOIN_MITER, YOL_CAP_SQUARE, 0 }, { YOL_JOIN_MITER, YOL_CAP_ROUND, 0 } };
        int k;
        c[0].area = 2 * w; c[1].area = 2 * w - h * h / 2; c[2].area = 2 * w - h * h + pi * h * h / 4;
        c[3].area = 2 * w + 2 * w * h; c[4].area = 2 * w + pi * h * h;
        for (k = 0; k < 5; k++) {
            yol_stroke_desc d = sd(w, c[k].join, c[k].cap, YOL_STROKE, 1e-9);
            double a;
            yol_path_clear(&p);
            yol_move(&p, 0, 0); yol_line(&p, 1, 0); yol_line(&p, 1, 1);
            yol_path_end(&p);
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            a = yol_path_area(&o);
            CHECKF(fabs(a - c[k].area) <= 1e-8, "join %d cap %d: area %.12g, want %.12g", c[k].join, c[k].cap, a, c[k].area);
        }
        {
            yol_stroke_desc d = sd(w, YOL_JOIN_MITER, YOL_CAP_BUTT, YOL_STROKE, 1e-9);
            yol_path_clear(&p);
            yol_move(&p, 0, 0); yol_line(&p, 1, 0); yol_line(&p, 1, 1);
            yol_path_end(&p);
            d.miter_limit = 1.5;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK && fabs(yol_path_area(&o) - 2 * w) <= 1e-8);
        }
        /* a sharp turn past the miter limit is a bevel */
        {
            yol_stroke_desc d = sd(w, YOL_JOIN_MITER, YOL_CAP_BUTT, YOL_STROKE, 1e-9);
            double a1, a2;
            yol_path_clear(&p);
            yol_move(&p, 0, 0); yol_line(&p, 1, 0); yol_line(&p, 0, 0.2);
            yol_path_end(&p);
            d.miter_limit = 1.5;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            a1 = yol_path_area(&o);
            d.join = YOL_JOIN_BEVEL;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            a2 = yol_path_area(&o);
            CHECK(fabs(a1 - a2) <= 1e-12);
            d.join = YOL_JOIN_MITER; d.miter_limit = 100;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK && yol_path_area(&o) > a2 + 1e-4);
        }
        /* faux bold and inset of a unit square, round joins */
        {
            yol_stroke_desc d = sd(w, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_BOLD, 1e-9);
            yol_path_clear(&p);
            yol_rect(&p, 0, 0, 1, 1, 0, 0);
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            CHECKF(fabs(yol_path_area(&o) - ((1 + w) * (1 + w) - (4 - pi) * h * h)) <= 1e-8, "bold %.12g", yol_path_area(&o));
            d.mode = YOL_INSET;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            CHECKF(fabs(yol_path_area(&o) - (1 - w) * (1 - w)) <= 1e-12, "inset %.12g", yol_path_area(&o));
            d.mode = YOL_STROKE;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            CHECKF(fabs(yol_path_area(&o) - ((1 + w) * (1 + w) - (4 - pi) * h * h - (1 - w) * (1 - w))) <= 1e-8, "outline %.12g", yol_path_area(&o));
        }
        /* a dot: a zero-length open subpath with round caps */
        {
            yol_stroke_desc d = sd(w, YOL_JOIN_ROUND, YOL_CAP_ROUND, YOL_STROKE, 1e-9);
            yol_path_clear(&p);
            yol_move(&p, 3, 3); yol_line(&p, 3, 3);
            yol_path_end(&p);
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK && fabs(yol_path_area(&o) - pi * h * h) <= 1e-8);
            d.tol = 1e-6;
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
            {
                int i, j;
                double e = 0;
                path_quads(&o, &b, 1, 0, 0);
                for (i = 0; i < b.n; i++) for (j = 0; j <= 16; j++) {
                    double dx = qx(&b.q[i], j / 16.0) - 3, dy = qy(&b.q[i], j / 16.0) - 3;
                    e = fmax(e, fabs(sqrt(dx * dx + dy * dy) - h));
                }
                CHECKF(e <= 1e-6 + 1e-12, "a dot's circle off by %.3g", e);
            }
        }
    }
    /* an annulus: the boundary within the tolerances of radii r +- h */
    {
        yol_stroke_desc d = sd(0.1, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 1e-7);
        double e = 0;
        int i, j;
        yol_path_clear(&p);
        yol_set_tol(&p, 1e-7);
        yol_ellipse(&p, 0, 0, 0.5, 0.5);
        yol_set_tol(&p, 0);
        CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK && o.n_contours == 2);
        path_quads(&o, &b, 1, 0, 0);
        for (i = 0; i < b.n; i++) for (j = 0; j <= 16; j++) {
            double r = sqrt(qx(&b.q[i], j / 16.0) * qx(&b.q[i], j / 16.0) + qy(&b.q[i], j / 16.0) * qy(&b.q[i], j / 16.0));
            e = fmax(e, fmin(fabs(r - 0.45), fabs(r - 0.55)));
        }
        CHECKF(e <= 2.5e-7, "annulus off by %.3g", e);
    }
    /* glyph outlines at 0.08 em, round joins: inside where the distance to
     * the glyph's contours is at most 0.04 */
    {
        bb tt = { 0, 0, 0 };
        yol_font f;
        static const uint32_t gl[3] = { 2, 3, 7 };
        int gi, x, y, bad = 0, n = 0;
        tqs g = { 0, 0, 0 };
        make_tt(&tt, 0, 0);
        CHECK(yol_font_view(&f, tt.d, tt.n, 0, NULL, 0) == YOL_OK);
        for (gi = 0; gi < 3; gi++) {
            yol_stroke_desc d = sd(0.08, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 1e-5);
            yol_path_clear(&p);
            CHECK(yol_font_glyph(&g_cx, &f, gl[gi], &p, NULL) == YOL_OK);
            CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK && path_closed(&o));
            path_quads(&p, &g, 1, 0, 0);
            path_quads(&o, &b, 1, 0, 0);
            for (y = -1100; y < 150; y += 7) for (x = -100; x < 1100; x += 7) {
                double cx = (x + 0.5) / 1000, cy = (y + 0.5) / 1000, dm = 1e9;
                int k, w;
                for (k = 0; k < g.n; k++) dm = fmin(dm, dist_tq(&g.q[k], cx, cy));
                if (fabs(dm - 0.04) < 3e-5) continue;
                w = winding(&b, cx, cy);
                n++;
                if ((w != 0) != (dm <= 0.04) || (w != 0 && w != 1)) bad++;
            }
        }
        CHECKF(bad == 0, "glyph strokes: %d wrong of %d", bad, n);
        free(g.q); free(tt.d);
    }
    /* the inner join: two segments shorter than the half width, bevel join;
     * the point is in neither band, only in the triangle the bands leave */
    {
        yol_stroke_desc d = sd(0.2, YOL_JOIN_BEVEL, YOL_CAP_BUTT, YOL_STROKE, 1e-7);
        yol_path_clear(&p);
        yol_move(&p, 0, 0); yol_line(&p, 0.01, 0); yol_line(&p, 0.01, 0.01);
        yol_path_end(&p);
        CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_OK);
        path_quads(&o, &b, 1, 0, 0);
        CHECK(winding(&b, -0.03, 0.04) == 1 && winding(&b, -0.06, 0.06) == 0);
    }
    /* an impossible tolerance runs out of the approximation budget:
     * YOL_ERR_NUMERIC, not a hang */
    {
        yol_stroke_desc d = sd(0.1, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 1e-300);
        yol_path_clear(&p);
        yol_move(&p, 0, 0); yol_quad(&p, 0.5, 1, 1, 0);
        yol_path_end(&p);
        CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_ERR_NUMERIC && strstr(yol_error(&g_cx), "did not converge"));
    }
    CHECK(yol_stroke(&g_cx, &p, &p, NULL) == YOL_ERR_ARG);
    {
        yol_stroke_desc d = sd(0, 0, 0, 0, 0);
        CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_ERR_ARG);
        d.width = 1; d.miter_limit = 0.5;
        CHECK(yol_stroke(&g_cx, &p, &o, &d) == YOL_ERR_RANGE);
    }
    free(b.q);
    yol_path_free(&p); yol_path_free(&o);
}

/* --- T11: allocations ------------------------------------------------------------------ */

static long g_allocs;
static void* count_realloc(void* user, void* p, size_t n) {
    (void)user;
    g_allocs++;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

static void t11_allocs(void) {
    bb tt = { 0, 0, 0 };
    yol_ctx c;
    yol_ctx_desc cd;
    yol_font f;
    yol_path p;
    static float tex[1 << 20];
    static uint32_t words[1 << 18];
    int pass, i;
    long before = 0;
    static const uint32_t gl[6] = { 1, 2, 3, 4, 7, 10 };
    g_where = "T11 allocations";
    make_tt(&tt, 0, 0);
    memset(&cd, 0, sizeof cd);
    cd.realloc = count_realloc;
    CHECK(yol_open(&c, &cd));
    CHECK(yol_font_view(&f, tt.d, tt.n, 0, NULL, 0) == YOL_OK);
    yol_path_init(&p, &c);
    for (pass = 0; pass < 3; pass++) {
        yol_cset s;
        yol_cset_desc d;
        yol_path o;
        yol_stroke_desc sdd = sd(0.05, YOL_JOIN_ROUND, YOL_CAP_BUTT, YOL_STROKE, 0);
        yol_raster_desc rd;
        static uint8_t img[64 * 64];
        /* a set in the caller's storage: only the context allocates */
        s.texels = tex; s.cap_texels = (1 << 20) / 4; s.words = words; s.cap_words = 1 << 18;
        memset(&d, 0, sizeof d);
        d.n_glyphs = TT_NG;
        CHECK(yol_cset_open(&s, NULL, &d));
        yol_path_init(&o, &c);
        if (pass == 2) before = g_allocs;
        for (i = 0; i < 6; i++) {
            yol_path_clear(&p);
            CHECK(yol_font_glyph(&c, &f, gl[i], &p, NULL) == YOL_OK);
            CHECK(yol_cset_add(&s, gl[i], &p) == YOL_OK);
            memset(&rd, 0, sizeof rd);
            rd.scale = 40; rd.y = 50; rd.out = img; rd.w = 64; rd.h = 64; rd.stride = 64;
            CHECK(yol_raster(&c, &p, &rd, NULL) == YOL_OK);
        }
        if (pass == 2) CHECKF(g_allocs == before, "%ld allocator calls after warm-up", g_allocs - before);
        CHECK(cset_ok(&s));
        (void)sdd;
    }
    yol_path_free(&p);
    yol_close(&c);
    /* a fixed arena: no heap; too small is YOL_ERR_FULL, with the size */
    {
        /* yol_open() wants 16-byte alignment, which a double array does
         * not promise (macOS arm64 CI placed it at 8 mod 16) */
        static unsigned char raw[(1 << 19) + 16];
        void* arena = (void*)(((uintptr_t)raw + 15u) & ~(uintptr_t)15u);
        yol_path q;
        int rc;
        memset(&cd, 0, sizeof cd);
        cd.mem = arena; cd.mem_size = (size_t)1 << 19;
        CHECK(yol_open(&c, &cd));
        yol_path_init(&q, &c);
        CHECK(yol_font_glyph(&c, &f, 2, &q, NULL) == YOL_OK);
        CHECK(yol_resolve(&c, &q, &q) == YOL_OK && fabs(yol_path_area(&q)) > 0.4);
        yol_close(&c);
        cd.mem_size = 512;
        CHECK(yol_open(&c, &cd));
        yol_path_init(&q, &c);
        rc = yol_font_glyph(&c, &f, 10, &q, NULL);
        CHECK(rc == YOL_ERR_FULL && strstr(yol_error(&c), "needed"));
        yol_close(&c);
        cd.realloc = count_realloc;
        CHECK(!yol_open(&c, &cd) && strstr(yol_error(&c), "not both") != NULL);
        yol_close(&c);   /* after a failed open */
        yol_close(&c);   /* twice */
    }
    {   /* v0.3: close is safe on zeroed handles; every failure on a context leaves a message */
        yol_ctx z;
        yol_cset s, sz;
        yol_cset_desc sd;
        yol_path q, e;
        yol_box b;
        yol_raster_desc rd;
        volatile double zero = 0.0;
        memset(&z, 0, sizeof z);
        memset(&sz, 0, sizeof sz);
        yol_close(&z);
        yol_close(NULL);
        yol_cset_close(&sz);
        yol_cset_close(NULL);
        CHECK(yol_open(&z, NULL) && yol_error(&z)[0] == '\0');
        memset(&sd, 0, sizeof sd);
        sd.n_glyphs = 1u << 24;
        CHECK(!yol_cset_open(&s, &z, &sd) && strstr(yol_error(&z), "2^24") != NULL);
        yol_cset_close(&s);   /* after a failed open */
        yol_cset_close(&s);
        CHECK(!yol_cset_open(&s, &z, NULL) && strstr(yol_error(&z), "no desc") != NULL);
        sd.n_glyphs = 2;
        CHECK(yol_cset_open(&s, &z, &sd) && s.words != NULL);
        yol_cset_close(&s);
        CHECK(s.words == NULL && s.ctx == NULL);
        yol_cset_close(&s);   /* twice */
        /* bare codes of v0.2 now leave a generic message */
        yol_path_init(&q, &z);
        yol_path_init(&e, &z);
        z.err[0] = '\0';
        CHECK(yol_resolve(&z, NULL, &q) == YOL_ERR_ARG && strstr(yol_error(&z), "resolve: bad argument") != NULL);
        z.err[0] = '\0';
        CHECK(yol_stroke(&z, &q, &e, NULL) == YOL_ERR_ARG && strstr(yol_error(&z), "stroke") != NULL);
        z.err[0] = '\0';
        memset(&rd, 0, sizeof rd);
        CHECK(yol_raster(&z, NULL, &rd, &b) == YOL_ERR_ARG && strstr(yol_error(&z), "raster") != NULL);
        z.err[0] = '\0';
        CHECK(yol_font_glyph(&z, NULL, 0, &q, NULL) == YOL_ERR_ARG && strstr(yol_error(&z), "glyph") != NULL);
        z.err[0] = '\0';
        CHECK(yol_cset_add(&sz, 0, &q) == YOL_ERR_ARG);   /* no context: the code only */
        /* a specific message is kept, not replaced by the generic one */
        yol_move(&q, 0, 0); yol_line(&q, zero / zero, 0);
        CHECK(yol_path_end(&q) == YOL_ERR_ARG && strstr(yol_error(&z), "not finite") != NULL);
        yol_path_free(&q);
        yol_path_free(&e);
        yol_close(&z);
        yol_close(&z);
        CHECK(yol_error(&z)[0] == '\0');
    }
    printf("T11 allocations: none after warm-up; a fixed arena works and refuses when full\n");
    free(tt.d);
}

/* --- T12: parameter tables -------------------------------------------------------------- */

static void t12_params(void) {
    int n, i;
    const yol_param* t;
    g_where = "T12 params";
    t = yol_stroke_params(&n);
    CHECK(n == 6 && strcmp(t[0].name, "width") == 0 && t[0].offset == offsetof(yol_stroke_desc, width) && t[2].def == 4);
    for (i = 0; i < n; i++) CHECK(t[i].offset < sizeof(yol_stroke_desc) && t[i].doc && t[i].min <= t[i].max);
    t = yol_raster_params(&n);
    CHECK(n == 4 && t[3].offset == offsetof(yol_raster_desc, format));
    t = yol_cset_params(&n);
    CHECK(n == 5 && t[4].offset == offsetof(yol_cset_desc, keep_overlaps) && t[1].offset == offsetof(yol_cset_desc, nh));
    t = yol_glyph_params(&n);
    CHECK(n == 5 && t[4].def == 1e-4 && t[3].offset == offsetof(yol_glyph_desc, y_up));
    CHECK(strcmp(yol_version(), YOL_VERSION_STRING) == 0 && strcmp(yol_strerror(YOL_ERR_NUMERIC), "numeric failure") == 0);
}

/* --- Windows fonts, when present -------------------------------------------------------- */

static uint8_t* read_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    uint8_t* d;
    long l;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); l = ftell(fp); fseek(fp, 0, SEEK_SET);
    d = (uint8_t*)malloc((size_t)l);
    *n = d ? fread(d, 1, (size_t)l, fp) : 0;
    fclose(fp);
    return d;
}

static void system_fonts(void) {
    static const struct { const char* path; int step; } fonts[] = {
        { "C:/Windows/Fonts/segoeui.ttf", 1 }, { "C:/Windows/Fonts/segoeuil.ttf", 3 }, { "C:/Windows/Fonts/bahnschrift.ttf", 1 },
        { "C:/Windows/Fonts/arial.ttf", 2 }, { "C:/Windows/Fonts/Nirmala.ttc", 2 }, { "C:/Windows/Fonts/msyh.ttc", 13 },
        { "C:/Windows/Fonts/SourceHanSansJP-Normal.otf", 13 } };
    size_t k;
    int any = 0;
    yol_path p;
    g_where = "system fonts";
    yol_path_init(&p, &g_cx);
    for (k = 0; k < sizeof fonts / sizeof fonts[0]; k++) {
        size_t n;
        uint8_t* d = read_file(fonts[k].path, &n);
        yol_font f;
        yol_cset s;
        yol_cset_desc cd;
        char err[200];
        int g, built = 0, failed = 0, rasters = 0;
        double worst = 0;
        if (!d) continue;
        any = 1;
        CHECKF(yol_font_view(&f, d, n, 0, err, sizeof err) == YOL_OK, "%s: %s", fonts[k].path, err);
        memset(&cd, 0, sizeof cd);
        cd.n_glyphs = (uint32_t)f.n_glyphs;
        CHECK(yol_cset_open(&s, &g_cx, &cd));
        for (g = 0; g < f.n_glyphs; g += fonts[k].step) {
            int rc;
            yol_path_clear(&p);
            rc = yol_font_glyph(&g_cx, &f, (uint32_t)g, &p, NULL);
            if (rc == YOL_OK) rc = yol_cset_add(&s, (uint32_t)g, &p);
            if (rc < 0) { failed++; if (failed <= 3) fprintf(stderr, "  %s glyph %d: %s\n", fonts[k].path, g, yol_error(&g_cx)); continue; }
            built++;
            /* exact alpha at 24 px: in [0, 1], summing to the area */
            if (g % 97 == 0 && p.n_pts) {
                yol_raster_desc rd;
                yol_box box;
                double* img, sum = 0, area;
                int i, W, H;
                yol_path r;
                memset(&rd, 0, sizeof rd);
                rd.scale = 24; rd.format = YOL_ALPHA_F64;
                yol_raster(&g_cx, &p, &rd, &box);
                W = box.x1 - box.x0; H = box.y1 - box.y0;
                img = (double*)malloc((size_t)W * H * 8);
                rd.x = -box.x0; rd.y = -box.y0; rd.out = img; rd.w = W; rd.h = H; rd.stride = W * 8;
                CHECK(yol_raster(&g_cx, &p, &rd, NULL) == YOL_OK);
                for (i = 0; i < W * H; i++) { sum += img[i]; if (img[i] < 0 || img[i] > 1) failed++; }
                yol_path_init(&r, &g_cx);
                yol_resolve(&g_cx, &p, &r);
                area = yol_path_area(&r) * 576;
                yol_path_free(&r);
                worst = fmax(worst, fabs(sum - area) / (1 + area));
                rasters++;
                free(img);
            }
        }
        CHECK(cset_ok(&s));
        CHECKF(failed == 0, "%s: %d glyphs failed", fonts[k].path, failed);
        CHECKF(worst <= 1e-12, "%s: coverage sum off by %.3g", fonts[k].path, worst);
        printf("system font %s: %d glyphs into a set (every %d), %d failed, %d rasters, %u texels, %u words\n",
               fonts[k].path, built, fonts[k].step, failed, rasters, s.n_texels, s.n_words);
        yol_cset_close(&s);
        free(d);
    }
    if (!any) printf("system fonts: skipped (none of the Windows fonts is present)\n");
    yol_path_free(&p);
}


/* --- T10: the SVG subset --------------------------------------------------------- */

#define SVG_MAX 16
static yol_svg_layer g_layers[SVG_MAX];

static void svg_free(int n) { int k; for (k = 0; k < n; k++) yol_path_free(&g_layers[k].path); }

/* Parses text; returns the code or the layer count. */
static int svg_run(const char* text, double vb[4], char* err, size_t cap) {
    yol_svg_desc d;
    memset(&d, 0, sizeof d);
    d.layers = g_layers; d.max_layers = SVG_MAX;
    return yol_svg(&g_cx, text, strlen(text), &d, vb, err, cap);
}

/* One layer's area, or -1 for a code. */
static double svg_area1(const char* text) {
    int n = svg_run(text, NULL, NULL, 0);
    double a;
    if (n != 1) { if (n > 0) svg_free(n); return -1; }
    a = yol_path_area(&g_layers[0].path);
    svg_free(n);
    return a;
}

/* The two files' first layers cover the same pixels (exact alpha at 4 px a
 * unit, within 1e-9): same shape, not only the same area. */
static int svg_same(const char* a, const char* b) {
    yol_path pa;
    yol_raster_desc rd;
    static double ia[200 * 200], ib[200 * 200];
    int n, k, same = 1;
    yol_path_init(&pa, &g_cx);
    n = svg_run(a, NULL, NULL, 0);
    if (n < 1) return 0;
    yol_path_copy(&pa, &g_layers[0].path);
    svg_free(n);
    n = svg_run(b, NULL, NULL, 0);
    if (n < 1) { yol_path_free(&pa); return 0; }
    memset(&rd, 0, sizeof rd);
    rd.scale = 4; rd.format = YOL_ALPHA_F64; rd.w = 200; rd.h = 200; rd.stride = 200 * 8;
    rd.x = 100; rd.y = 100;
    rd.out = ia; yol_raster(&g_cx, &pa, &rd, NULL);
    rd.out = ib; yol_raster(&g_cx, &g_layers[0].path, &rd, NULL);
    for (k = 0; k < 200 * 200; k++) if (fabs(ia[k] - ib[k]) > 1e-9) same = 0;
    svg_free(n);
    yol_path_free(&pa);
    return same;
}

static void t10_svg(void) {
    char err[300];
    double vb[4], a, pi = 3.14159265358979323846;
    int n, k;
    g_where = "T10 svg";
    /* a rectangle, its color, the viewBox */
    n = svg_run("<?xml version=\"1.0\"?>\n<!-- a test -->\n<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"x\">\n"
                "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\"><title>t &amp; t</title>"
                "<rect x=\"10\" y=\"10\" width=\"30\" height=\"20\" fill=\"#ff0000\"/></svg>", vb, err, sizeof err);
    CHECKF(n == 1, "rect: %d %s", n, err);
    if (n == 1) {
        CHECK(yol_path_area(&g_layers[0].path) == 600 && g_layers[0].rgba == 0xff0000ffu && g_layers[0].source == YOL_SVG_FILL && g_layers[0].element == 0);
        CHECK((g_layers[0].path.flags & YOL_PATH_RESOLVED) && path_closed(&g_layers[0].path));
        CHECK(vb[0] == 0 && vb[1] == 0 && vb[2] == 100 && vb[3] == 50);
        svg_free(n);
    }
    /* width and height instead of a viewBox; a circle within the tolerance */
    n = svg_run("<svg width=\"200px\" height=\"100\"><circle cx=\"50\" cy=\"50\" r=\"10\" fill-opacity=\"0.5\"/></svg>", vb, err, sizeof err);
    CHECKF(n == 1 && vb[2] == 200 && vb[3] == 100, "circle: %d %s", n, err);
    if (n == 1) {
        CHECK(fabs(yol_path_area(&g_layers[0].path) - pi * 100) <= 0.02 * 2 * pi * 10 && g_layers[0].rgba == 0x00000080u);
        svg_free(n);
    }
    /* path grammar: implicit lineto after m, H, V, Z, packed numbers */
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><path d=\"m10,10 20,0 0,20 -20,0z\"/></svg>") == 400);
    CHECK(svg_same("<svg viewBox=\"0 0 100 100\"><path d=\"m10,10 20,0 0,20z\"/></svg>",
                   "<svg viewBox=\"0 0 100 100\"><path d=\"M10 10L30 10L30 30Z\"/></svg>"));
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><path d=\"M10 10H30V30H10Z\"/></svg>") == 400);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><path d=\"M.5.5L10.5.5 10.5 10.5.5 10.5z\"/></svg>") == 100);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><path d=\"M0,0l1e1,0,0,10-10-0z\"/></svg>") == 100);
    /* S and T reflect the last control point; relative commands */
    CHECK(svg_same("<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0C2 10 7 12 10 0S21-10 20 0Z\"/></svg>",
                   "<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0C2 10 7 12 10 0C13-12 21-10 20 0Z\"/></svg>"));
    CHECK(svg_same("<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0Q3 10 10 0T20 0T30 0Z\"/></svg>",
                   "<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0Q3 10 10 0Q17-10 20 0Q23 10 30 0Z\"/></svg>"));
    CHECK(svg_same("<svg viewBox=\"-50 -50 100 100\"><path d=\"M5 5c2 10 7 12 10 0s11-10 10 0q3 5 6 0t6 0l1 1z\"/></svg>",
                   "<svg viewBox=\"-50 -50 100 100\"><path d=\"M5 5C7 15 12 17 15 5S26-5 25 5Q28 10 31 5T37 5L38 6Z\"/></svg>"));
    CHECK(!svg_same("<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0Q3 10 10 0T20 0Z\"/></svg>",
                    "<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0Q3 10 10 0Q10-10 20 0Z\"/></svg>"));
    /* arcs, with packed flags */
    a = svg_area1("<svg viewBox=\"-50 -50 100 100\"><path d=\"M0 0A10 10 0 0 1 20 0Z\"/></svg>");
    CHECKF(fabs(a - 50 * pi) <= 0.01 * 20 * pi, "arc %.17g", a);
    CHECK(svg_same("<svg viewBox=\"-50 -50 100 100\"><path d=\"M5 5a10,10 0 0120,0z\"/></svg>",
                   "<svg viewBox=\"-50 -50 100 100\"><path d=\"M5 5A10 10 0 0 1 25 5Z\"/></svg>"));
    /* shapes: ellipse, rounded rect, polygon, polyline (filled closed) */
    a = svg_area1("<svg viewBox=\"0 0 100 100\"><ellipse cx=\"50\" cy=\"50\" rx=\"20\" ry=\"10\"/></svg>");
    CHECKF(fabs(a - 200 * pi) <= 0.01 * 2 * pi * 20, "ellipse %.17g", a);
    a = svg_area1("<svg viewBox=\"0 0 100 100\"><rect width=\"20\" height=\"10\" rx=\"2\"/></svg>");
    CHECKF(fabs(a - (200 - (4 - pi) * 4)) <= 0.01 * 60, "rounded rect %.17g", a);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><polygon points=\"0,0 10,0 10,10\"/></svg>") == 50);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><polyline points=\"0 0 10 0 10 10\"/></svg>") == 50);
    CHECK(svg_run("<svg viewBox=\"0 0 100 100\"><line x1=\"0\" y1=\"0\" x2=\"10\" y2=\"10\"/></svg>", NULL, NULL, 0) == 0);
    /* transforms, nested */
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><g transform=\"translate(10 0) scale(2 3)\"><rect width=\"1\" height=\"1\"/></g></svg>") == 6);
    CHECK(fabs(svg_area1("<svg viewBox=\"0 0 100 100\"><g transform=\"rotate(30, 5 5)\"><g transform=\"skewX(45)\"><rect width=\"10\" height=\"10\"/></g></g></svg>") - 100) < 1e-9);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><rect transform=\"matrix(2,0,0,2,0,0)\" width=\"10\" height=\"10\"/></svg>") == 400);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><rect transform=\"scale(-1 1)\" width=\"10\" height=\"10\"/></svg>") == 100);
    /* the order: translate after scale here, so the box starts at x = 10 */
    n = svg_run("<svg viewBox=\"0 0 100 100\"><g transform=\"translate(10 0) scale(2)\"><rect x=\"1\" width=\"1\" height=\"1\"/></g></svg>", NULL, NULL, 0);
    if (n == 1) {
        double mx = 1e9;
        for (k = 0; k < g_layers[0].path.n_pts; k++) mx = fmin(mx, g_layers[0].path.pts[2 * k]);
        CHECKF(mx == 12, "transform order: min x %.17g", mx);
        svg_free(n);
    } else CHECK(n == 1);
    /* the bounds: 70 nested groups, 70 attributes */
    {
        static char t[4096];
        size_t m = 0;
        int q;
        m += (size_t)snprintf(t + m, sizeof t - m, "<svg viewBox=\"0 0 9 9\">");
        for (q = 0; q < 70; q++) m += (size_t)snprintf(t + m, sizeof t - m, "<g>");
        for (q = 0; q < 70; q++) m += (size_t)snprintf(t + m, sizeof t - m, "</g>");
        snprintf(t + m, sizeof t - m, "</svg>");
        n = svg_run(t, NULL, err, sizeof err);
        CHECK(n == YOL_ERR_FORMAT && strstr(err, "deeper"));
        m = (size_t)snprintf(t, sizeof t, "<svg viewBox=\"0 0 9 9\"><rect");
        for (q = 0; q < 70; q++) m += (size_t)snprintf(t + m, sizeof t - m, " id%d=\"x\"", q);
        snprintf(t + m, sizeof t - m, "/></svg>");
        n = svg_run(t, NULL, err, sizeof err);
        CHECK(n == YOL_ERR_FORMAT && strstr(err, "attributes"));
    }
    /* fill-rule */
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><path fill-rule=\"evenodd\" d=\"M0 0H40V40H0ZM10 10H30V30H10Z\"/></svg>") == 1200);
    CHECK(svg_area1("<svg viewBox=\"0 0 100 100\"><path d=\"M0 0H40V40H0ZM10 10H30V30H10Z\"/></svg>") == 1600);
    /* inheritance; fill then stroke; SVG's default miter join */
    n = svg_run("<svg viewBox=\"0 0 100 100\"><g fill=\"blue\" stroke=\"red\" stroke-width=\"2\"><rect x=\"10\" y=\"10\" width=\"10\" height=\"10\"/></g></svg>", NULL, err, sizeof err);
    CHECKF(n == 2, "fill and stroke: %d %s", n, err);
    if (n == 2) {
        CHECK(g_layers[0].source == YOL_SVG_FILL && g_layers[0].rgba == 0x0000ffffu && yol_path_area(&g_layers[0].path) == 100);
        CHECK(g_layers[1].source == YOL_SVG_STROKE && g_layers[1].rgba == 0xff0000ffu && fabs(yol_path_area(&g_layers[1].path) - 80) < 1e-9);
        svg_free(n);
    }
    /* a stroke under an unequal scale: the pen scales too */
    n = svg_run("<svg viewBox=\"0 0 100 100\"><g transform=\"scale(2 1)\"><line x1=\"0\" y1=\"5\" x2=\"10\" y2=\"5\" stroke=\"black\" stroke-width=\"2\"/></g></svg>", NULL, err, sizeof err);
    CHECKF(n == 1 && fabs(yol_path_area(&g_layers[0].path) - 40) < 1e-9, "scaled stroke: %d %s", n, err);
    if (n > 0) svg_free(n);
    n = svg_run("<svg viewBox=\"0 0 100 100\"><polyline points=\"0 0 10 0 10 10\" fill=\"none\" stroke=\"#000\" stroke-linecap=\"round\" stroke-linejoin=\"bevel\" stroke-width=\"2\"/></svg>", NULL, err, sizeof err);
    CHECKF(n == 1 && fabs(yol_path_area(&g_layers[0].path) - (40 - 0.5 + pi)) < 0.05, "caps and joins: %d %s %.9g", n, err, n == 1 ? yol_path_area(&g_layers[0].path) : 0.0);
    if (n > 0) svg_free(n);
    /* colors */
    {
        static const struct { const char* fill; uint32_t rgba; } cs[] = {
            { "#abc", 0xaabbccffu }, { "#A0b1C2", 0xa0b1c2ffu }, { "rgb(10%, 20, 30)", 0x1a141effu }, { "orange", 0xffa500ffu }, { "teal", 0x008080ffu } };
        for (k = 0; k < 5; k++) {
            char t[200];
            snprintf(t, sizeof t, "<svg viewBox=\"0 0 9 9\"><rect width=\"1\" height=\"1\" fill=\"%s\"/></svg>", cs[k].fill);
            n = svg_run(t, NULL, NULL, 0);
            CHECKF(n == 1 && g_layers[0].rgba == cs[k].rgba, "color %s: %08x", cs[k].fill, n == 1 ? g_layers[0].rgba : 0u);
            if (n > 0) svg_free(n);
        }
        CHECK(svg_run("<svg viewBox=\"0 0 9 9\"><rect width=\"1\" height=\"1\" fill=\"none\"/></svg>", NULL, NULL, 0) == 0);
        n = svg_run("<svg viewBox=\"0 0 9 9\"><rect width=\"1\" height=\"1\" opacity=\"0.25\" fill-opacity=\"0.5\"/></svg>", NULL, NULL, 0);
        CHECK(n == 1 && (g_layers[0].rgba & 0xffu) == 32u);
        if (n > 0) svg_free(n);
    }
    /* refused by name */
    {
        static const struct { const char* body; const char* word; } rf[] = {
            { "<filter id=\"f\"/>", "<filter>" }, { "<mask/>", "<mask>" }, { "<clipPath/>", "<clipPath>" }, { "<text>hi</text>", "<text>" },
            { "<style>rect{}</style>", "<style>" }, { "<linearGradient/>", "<linearGradient>" }, { "<radialGradient/>", "<radialGradient>" },
            { "<pattern/>", "<pattern>" }, { "<image/>", "<image>" }, { "<use/>", "<use>" }, { "<symbol/>", "<symbol>" },
            { "<marker/>", "<marker>" }, { "<foreignObject/>", "<foreignObject>" }, { "<svg/>", "nested <svg>" },
            { "<rect width=\"1\" height=\"1\" style=\"fill:red\"/>", "style" }, { "<rect width=\"1\" height=\"1\" class=\"a\"/>", "class" },
            { "<rect width=\"1\" height=\"1\" clip-path=\"url(#c)\"/>", "clip-path" }, { "<rect width=\"1\" height=\"1\" mask=\"url(#m)\"/>", "mask" },
            { "<rect width=\"1\" height=\"1\" filter=\"url(#f)\"/>", "filter" }, { "<rect width=\"1\" height=\"1\" fill=\"url(#g)\"/>", "url()" },
            { "<rect width=\"1\" height=\"1\" stroke=\"red\" stroke-dasharray=\"1 1\"/>", "stroke-dasharray" },
            { "<rect width=\"1\" height=\"1\" vector-effect=\"non-scaling-stroke\"/>", "vector-effect" },
            { "<rect width=\"1\" height=\"1\" display=\"none\"/>", "display" }, { "<rect width=\"1\" height=\"1\" fill=\"currentColor\"/>", "currentColor" },
            { "<rect width=\"1\" height=\"1\" fill=\"hsl(0,0%,0%)\"/>", "color" }, { "<rect width=\"10%\" height=\"1\"/>", "units" },
            { "<g opacity=\"0.5\"><rect width=\"1\" height=\"1\"/></g>", "opacity on a group" },
            { "<rect width=\"1\" height=\"1\" stroke=\"red\" opacity=\"0.5\"/>", "both fill and stroke" },
            { "<defs><linearGradient/></defs>", "inside <defs>" }, { "<rect width=\"1\" height=\"1\"><animate/></rect>", "inside a shape" },
            { "<sodipodi:namedview/>", "<sodipodi:namedview>" }, { "<rect width=\"1\" height=\"1\" fill=\"&red;\"/>", "entity" } };
        for (k = 0; k < (int)(sizeof rf / sizeof rf[0]); k++) {
            char t[400];
            snprintf(t, sizeof t, "<svg viewBox=\"0 0 9 9\">%s</svg>", rf[k].body);
            n = svg_run(t, NULL, err, sizeof err);
            CHECKF(n == YOL_ERR_REFUSED && strstr(err, rf[k].word), "refuse %s: %d '%s'", rf[k].body, n, err);
            if (n > 0) svg_free(n);
        }
        n = svg_run("<!DOCTYPE svg [<!ENTITY a \"b\">]><svg viewBox=\"0 0 9 9\"/>", NULL, err, sizeof err);
        CHECK(n == YOL_ERR_REFUSED && strstr(err, "DOCTYPE"));
        n = svg_run("<svg viewBox=\"0 0 9 9\" transform=\"scale(2)\"/>", NULL, err, sizeof err);
        CHECK(n == YOL_ERR_REFUSED && strstr(err, "transform on <svg>"));
    }
    /* malformed */
    {
        static const char* const bad[] = { "", "hello", "<svg viewBox=\"0 0 9 9\">", "<svg viewBox=\"0 0 9\"/>", "<svg/>",
            "<svg viewBox=\"0 0 9 9\"><rect width=1/></svg>", "<svg viewBox=\"0 0 9 9\"><path d=\"10 10\"/></svg>",
            "<svg viewBox=\"0 0 9 9\"><path d=\"M0 0 L 1\"/></svg>", "<svg viewBox=\"0 0 9 9\"><path d=\"M0 0A1 1 0 2 0 1 1\"/></svg>",
            "<svg viewBox=\"0 0 9 9\"><polygon points=\"0 0 1\"/></svg>", "<svg viewBox=\"0 0 9 9\"><g transform=\"spin(3)\"/></svg>",
            "<rect/>", "<svg viewBox=\"0 0 9 9\"><!-- open </svg>" };
        for (k = 0; k < (int)(sizeof bad / sizeof bad[0]); k++) {
            n = svg_run(bad[k], NULL, err, sizeof err);
            CHECKF(n == YOL_ERR_FORMAT, "malformed %d: %d '%s'", k, n, err);
            if (n > 0) svg_free(n);
        }
    }
    /* full; and every layer into a curve set */
    {
        yol_svg_desc d;
        yol_cset s;
        yol_cset_desc cd;
        const char* t = "<svg viewBox=\"0 0 100 100\"><rect width=\"10\" height=\"10\" stroke=\"red\"/><circle cx=\"50\" cy=\"50\" r=\"20\" fill=\"lime\" stroke=\"navy\" stroke-width=\"3\"/></svg>";
        memset(&d, 0, sizeof d);
        d.layers = g_layers; d.max_layers = 3;
        CHECK(yol_svg(&g_cx, t, strlen(t), &d, NULL, err, sizeof err) == YOL_ERR_FULL);
        d.max_layers = 4;
        n = yol_svg(&g_cx, t, strlen(t), &d, NULL, err, sizeof err);
        CHECK(n == 4 && g_layers[3].element == 1);
        memset(&cd, 0, sizeof cd);
        cd.n_glyphs = 4;
        CHECK(yol_cset_open(&s, &g_cx, &cd));
        for (k = 0; k < n; k++) CHECK(yol_cset_add(&s, (uint32_t)k, &g_layers[k].path) == YOL_OK);
        CHECK(cset_ok(&s));
        yol_cset_close(&s);
        if (n > 0) svg_free(n);
    }
    printf("T10 svg: shapes, path grammar, transforms, styles, colors, refusals by name\n");
}

/* --- T13: SVG fuzz ----------------------------------------------------------------- */

static void t13_svg_fuzz(void) {
    static const char base[] =
        "<?xml version=\"1.0\"?><svg viewBox=\"0 0 100 100\" fill=\"#123\"><title>x</title>"
        "<g transform=\"translate(1,2) rotate(10) scale(1.5 1)\" stroke=\"rgb(1,2,3)\" stroke-width=\"2\" stroke-linejoin=\"round\">"
        "<path d=\"M10 10C20 0 30 20 40 10S60 0 70 10Q80 20 90 10T95 30A10 5 30 1 0 50 50ZM5 5h3v3h-3z\" fill-rule=\"evenodd\"/>"
        "<rect x=\"1\" y=\"2\" width=\"30\" height=\"20\" rx=\"3\"/><circle cx=\"50\" cy=\"50\" r=\"9\"/>"
        "<ellipse cx=\"20\" cy=\"70\" rx=\"9\" ry=\"4\" fill=\"none\"/><line x1=\"0\" y1=\"0\" x2=\"9\" y2=\"9\" stroke-linecap=\"square\"/>"
        "<polyline points=\"1 1 5 9 9 1\"/><polygon points=\"20 20 30 25 25 35\" stroke=\"none\"/></g><defs/></svg>";
    char buf[sizeof base + 64];
    int i, ok = 0, codes = 0;
    yol_svg_desc d;
    g_where = "T13 svg fuzz";
    memset(&d, 0, sizeof d);
    d.layers = g_layers; d.max_layers = SVG_MAX;
    {
        int n0 = yol_svg(&g_cx, base, sizeof base - 1, &d, NULL, NULL, 0);
        CHECKF(n0 > 0, "base: %d %s", n0, yol_error(&g_cx));
        if (n0 > 0) svg_free(n0);
    }
    for (i = 0; i < (g_long ? 20000 : 4000); i++) {
        size_t n = sizeof base - 1;
        int kind = (int)rndu(4), m, rc;
        memcpy(buf, base, n);
        if (kind == 0) for (m = 0; m < 1 + (int)rndu(4); m++) buf[rndu((uint32_t)n)] = (char)(32 + rndu(95));
        else if (kind == 1) n = rndu((uint32_t)n);
        else if (kind == 2) { static const char tok[] = "<>/=\"'&#;:()-.,0123456789eEMmZzAa "; for (m = 0; m < 1 + (int)rndu(6); m++) buf[rndu((uint32_t)n)] = tok[rndu((uint32_t)(sizeof tok - 1))]; }
        else { size_t at = rndu((uint32_t)n), len = rndu(20); if (at + len > n) len = n - at; memmove(buf + at, buf + at + len, n - at - len); n -= len; }
        rc = yol_svg(&g_cx, buf, n, &d, NULL, NULL, 0);
        CHECK(rc >= 0 || rc == YOL_ERR_FORMAT || rc == YOL_ERR_REFUSED || rc == YOL_ERR_RANGE || rc == YOL_ERR_NUMERIC || rc == YOL_ERR_ARG);
        if (rc >= 0) { ok++; svg_free(rc); } else codes++;
    }
    printf("T13 svg fuzz: %d mutated files, %d parsed, %d refused or malformed; no crash\n", ok + codes, ok, codes);
}

int main(int argc, char** argv) {
    int quick = (argc > 1 && strcmp(argv[1], "--quick") == 0) || getenv("YOL_TEST_QUICK") != NULL;
    g_long = argc > 1 && strcmp(argv[1], "--long") == 0;
    static void (*const tests[])(void) = { t1_vector, t12_params, t6_cubics, t4_fonts, t2_csets, t7_resolve, t8_raster, t9_strokes, t10_svg, t11_allocs, t5_fuzz, t13_svg_fuzz, system_fonts };
    size_t k;
    yol_open(&g_cx, NULL);
    for (k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        clock_t t0 = clock();
        if (quick && tests[k] == system_fonts) continue;
        tests[k]();
        if (getenv("YOL_TEST_TIMES")) printf("  (%.1f s)\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
    yol_close(&g_cx);
    free(g_xc);
    printf("outline_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
