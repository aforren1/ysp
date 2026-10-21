/* gfx_test.c - self-checking test for ysp/gfx.h. Returns 0 when every
 * check passed, 1 after printing each failure.
 *
 * Two halves:
 *   CPU   the calibration math against published values (IEC 61966-2-1 for
 *         sRGB, CVRL's cone fundamentals, Psychtoolbox's ComputeDKL_M run in
 *         MATLAB), the canonical file, the noise hash, the coordinate
 *         transform, the parameter table, the shader wrapper, the null
 *         backend and the ring records. Needs nothing.
 *   GL    every stimulus rendered offscreen into an RGBA32F scene and read
 *         back, against a CPU reference in double at pixel centers, on every
 *         GL ES 3.0 renderer it can open through tests/adapt/
 *         gfx_headless.h: ANGLE on D3D11 hardware, D3D11 WARP and
 *         Vulkan SwiftShader on Windows (YSCR_ANGLE_DIR), Mesa llvmpipe
 *         on Linux. Pixel positions come from ygfx_local(), the inverse
 *         of the one coordinate transform, never from hard-coded positions.
 * With no GL it says so and passes, unless YGFX_TEST_REQUIRE_GL=1.
 * YGFX_TEST_DEVICES=warp,swiftshader,hardware,mesa picks renderers; with
 * none named it runs the first that opens. YGFX_TEST_FULL=1 runs every
 * renderer and every condition (run it before a report); the default is
 * the quick run, about a minute (docs/gfx.md, v0.7).
 * YGFX_TEST_THREADS sets the CPU references' threads.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o gfx_test tests/adapt/gfx_test.c -lm -pthread -ldl && ./gfx_test
 *     cl /nologo /W4 /WX /Iinclude tests\adapt\gfx_test.c
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   /* getenv() under /W4 /WX */
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"   /* the outline builder's sets against the test builder's (v0.7); before
                             * ysp/gfx.h: ygfx_text_make() (v0.15) */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#include "gfx_headless.h"
#include "gfx_cset_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* FNV-1a 64 of ygfx_shader_wrap()'s text for the three test bodies (v0.10.3). */
#define WRAP_PIN0 0xb50a66cbc59d9fbdULL
#define WRAP_PIN1 0xb4617a390cfca6b2ULL
#define WRAP_PIN2 0x56a3b341e7ed296cULL
#if defined(_WIN32)
    #include <direct.h>
    #if defined(_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable: 4201)   /* the SDK's nameless unions */
    #endif
    #include <d3d11_1.h>   /* the D3D11 import: textures on ANGLE's device, shared ones */
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #endif
    #define TEST_RMDIR(p) _rmdir(p)
#else
    #include <unistd.h>
    #define TEST_RMDIR(p) rmdir(p)
#endif

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static const char* g_where = "cpu";

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "gfx_test [%s]: FAIL line %d: %s\n", g_where, __LINE__, #cond); g_failures++; } } while (0)

/* The CPU references are most of the test's time: rows run on threads
 * (YGFX_TEST_THREADS, default the hardware threads up to 16; 1 = none).
 * A row's function writes only its own row of an output array; the checks
 * reduce the arrays afterwards, in row order, so the results do not depend
 * on the thread count. */
/* YGFX_TEST_FULL=1: every renderer, every condition (docs/gfx.md,
 * v0.7); the default is the quick run of the edit-test loop. */
static int cs_full(void) { const char* e = getenv("YGFX_TEST_FULL"); return e && e[0] == '1'; }

typedef void (*par_fn)(void* ctx, int row);
typedef struct par_arg { par_fn f; void* ctx; int n, t, nt; } par_arg;
static void par_run(par_arg* a) { int j; for (j = a->t; j < a->n; j += a->nt) a->f(a->ctx, j); }
#if defined(_WIN32)
static DWORD WINAPI par_main(LPVOID p) { par_run((par_arg*)p); return 0; }
#elif !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <pthread.h>
#define PAR_PTHREADS 1
static void* par_main(void* p) { par_run((par_arg*)p); return NULL; }
#endif
static int par_threads(void) {
    static int n = 0;
    const char* e = getenv("YGFX_TEST_THREADS");
    if (n) return n;
    n = e && e[0] ? atoi(e) : 0;
#if defined(_WIN32)
    if (n <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); n = (int)si.dwNumberOfProcessors; }
#elif defined(PAR_PTHREADS) && defined(_SC_NPROCESSORS_ONLN)
    if (n <= 0) n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}
static void par_rows(par_fn f, void* ctx, int n) {
    par_arg a[16];
    int nt = par_threads(), t, started[16] = { 0 };
#if defined(_WIN32)
    HANDLE h[16];
#elif defined(PAR_PTHREADS)
    pthread_t h[16];
#endif
    for (t = 0; t < nt; t++) { a[t].f = f; a[t].ctx = ctx; a[t].n = n; a[t].t = t; a[t].nt = nt; }
    for (t = 1; t < nt; t++) {
#if defined(_WIN32)
        h[t] = CreateThread(NULL, 0, par_main, &a[t], 0, NULL);
        started[t] = h[t] != NULL;
#elif defined(PAR_PTHREADS)
        started[t] = pthread_create(&h[t], NULL, par_main, &a[t]) == 0;
#endif
        if (!started[t]) par_run(&a[t]);   /* no thread: this one runs it */
    }
    par_run(&a[0]);
    for (t = 1; t < nt; t++) {
        if (!started[t]) continue;
#if defined(_WIN32)
        WaitForSingleObject(h[t], INFINITE);
        CloseHandle(h[t]);
#elif defined(PAR_PTHREADS)
        pthread_join(h[t], NULL);
#endif
    }
}
#define CHECK_LE(v, lim) do { double v_ = (double)(v), l_ = (double)(lim); if (!(v_ <= l_)) { \
    fprintf(stderr, "gfx_test [%s]: FAIL line %d: %s = %.4g, limit %.4g\n", g_where, __LINE__, #v, v_, l_); g_failures++; } } while (0)

static double maxd(double a, double b) { return a > b ? a : b; }

/* ---------------------------------------------------------- reference data */

/* Psychtoolbox-3's B_monitor (PsychColorimetricMatFiles), 380 to 780 nm at
 * 5 nm, the three primaries. */
static const float b_monitor[81][3] = {
    {2.54448866e-05f, 1.81754529e-05f, 0.000218614355f}, {1.70652778e-05f, 1.62274359e-05f, 0.00033646906f}, {1.73548826e-05f, 1.97121607e-05f, 0.000524075586f},
    {1.10430579e-05f, 2.10026232e-05f, 0.000785003663f}, {1.68514725e-05f, 2.47077267e-05f, 0.00112989339f}, {2.79205103e-05f, 3.02806071e-05f, 0.00162449058f},
    {3.68489024e-05f, 4.3492429e-05f, 0.00231158115f}, {4.61356456e-05f, 5.90593486e-05f, 0.00321444369f}, {6.40115125e-05f, 7.92486369e-05f, 0.00426287615f},
    {7.87895945e-05f, 0.000104398287f, 0.00536524553f}, {9.38017677e-05f, 0.000125842801f, 0.00629551011f}, {0.000104837476f, 0.000147268436f, 0.00699425461f},
    {0.000112854764f, 0.000169745887f, 0.00746987017f}, {0.000114891959f, 0.000190823113f, 0.0076538175f}, {0.000113321335f, 0.000219694306f, 0.00751941252f},
    {0.000112543437f, 0.000266895291f, 0.00715063755f}, {0.000115063854f, 0.000339613208f, 0.00661929067f}, {0.000164001973f, 0.000461715278f, 0.00595535941f},
    {0.00016166626f, 0.000648717897f, 0.00517660815f}, {0.000120422241f, 0.000935842356f, 0.00432736373f}, {9.11649293e-05f, 0.00134511289f, 0.00350724567f},
    {0.000119135588f, 0.00186204566f, 0.00284930202f}, {0.000173795432f, 0.00248474805f, 0.0022775671f}, {0.000217951362f, 0.00319023907f, 0.0018092097f},
    {0.000130290168f, 0.00396391904f, 0.00140759439f}, {0.000122895089f, 0.00469058849f, 0.00108401518f}, {0.000260265236f, 0.00530546436f, 0.000855244478f},
    {0.000242082122f, 0.00582612983f, 0.000675835781f}, {0.000125001984f, 0.00619461866f, 0.000536908805f}, {0.000119160078f, 0.00638613652f, 0.000421890475f},
    {0.000200565443f, 0.00641397414f, 0.000340849067f}, {0.000596439756f, 0.00634772689f, 0.000284081004f}, {0.000647111018f, 0.00618893013f, 0.00023846896f},
    {0.000250648378f, 0.00593243205f, 0.000196789675f}, {0.000247729627f, 0.00556196501f, 0.00016515944f}, {0.000325163104f, 0.00514279496f, 0.000142684411f},
    {0.000199194325f, 0.00460565852f, 0.000119278831f}, {0.000160859763f, 0.00399303906f, 9.87605758e-05f}, {0.000128333928f, 0.003296585f, 7.91288e-05f},
    {0.000217279486f, 0.00271931692f, 6.52297389e-05f}, {0.000692523735f, 0.00221373642f, 5.69836332e-05f}, {0.00121961917f, 0.00176864745f, 5.11066997e-05f},
    {0.00186081802f, 0.00140740744f, 4.68066546e-05f}, {0.00217286748f, 0.00115532229f, 4.334209e-05f}, {0.000777411976f, 0.000937724927f, 2.86030878e-05f},
    {0.000531028716f, 0.00075858179f, 2.25191296e-05f}, {0.00243411125f, 0.000613520213f, 3.55863737e-05f}, {0.00581217444f, 0.000521860978f, 6.10288797e-05f},
    {0.00935351762f, 0.000454585774f, 8.77379022e-05f}, {0.0160540849f, 0.000436561159f, 0.000141387244f}, {0.00646433981f, 0.000278232127f, 5.95202758e-05f},
    {0.00109971325f, 0.000179542211f, 1.49869678e-05f}, {0.000321805723f, 0.000136373934f, 7.90651162e-06f}, {0.000206917831f, 0.00010714503f, 5.72250985e-06f},
    {0.000194161332f, 8.52289108e-05f, 5.93693812e-06f}, {0.00019565519f, 6.70061216e-05f, 6.69041293e-06f}, {0.000166207001f, 5.45137183e-05f, 5.55323827e-06f},
    {0.000173404739f, 4.39852646e-05f, 4.75026921e-06f}, {0.000219653503f, 3.87610868e-05f, 5.56665309e-06f}, {0.000185579946f, 3.26363917e-05f, 5.4201717e-06f},
    {0.000377182835f, 3.00902558e-05f, 6.73444907e-06f}, {0.000782160371f, 2.79583629e-05f, 9.95153023e-06f}, {0.000641983526f, 2.25897484e-05f, 9.5864052e-06f},
    {0.00121424651f, 2.84244951e-05f, 1.59049557e-05f}, {0.00716850812f, 7.78583087e-05f, 5.98901973e-05f}, {0.0110984043f, 0.000112950694f, 9.41880103e-05f},
    {0.00310588215f, 3.90380642e-05f, 2.99187701e-05f}, {0.000240500983f, 1.10132229e-05f, 6.52342284e-06f}, {0.00017952384f, 9.10364834e-06f, 8.86790287e-06f},
    {0.000148740834f, 8.25892437e-06f, 8.14868661e-06f}, {0.000108493237f, 9.04293725e-06f, 1.11748754e-05f}, {9.72366149e-05f, 1.08798554e-05f, 9.62156905e-06f},
    {9.05450273e-05f, 9.37974836e-06f, 9.52454068e-06f}, {9.33611452e-05f, 1.00271812e-05f, 1.21351602e-05f}, {8.26694132e-05f, 1.13333422e-05f, 1.32683446e-05f},
    {7.27135662e-05f, 1.295684e-05f, 1.20727194e-05f}, {8.11668954e-05f, 1.47604411e-05f, 1.60195486e-05f}, {6.6912716e-05f, 1.75663715e-05f, 1.53622966e-05f},
    {7.04220745e-05f, 2.08366916e-05f, 2.80584074e-05f}, {7.34996324e-05f, 1.47124747e-05f, 4.57011947e-05f}, {6.55843052e-05f, 1.81984221e-05f, 5.75373665e-05f},
};

/* tests/compare/gfx_dkl.m, run in MATLAB R2023a with Psychtoolbox's own
 * ComputeDKL_M.m on CVRL linss2_10e_1 and B_monitor resampled linearly to
 * 1 nm, background RGB (0.4, 0.5, 0.3), luminance 0.68990272 L + 0.34832189 M. */
static const double ref_rgb_to_lms[9] = {
    0.15758320328193681, 0.36737427350739571, 0.050179703670330963,
    0.057646833363934384, 0.38451094060277291, 0.072627135835000883,
    0.0065563050333017972, 0.034056269070785766, 0.33055572889735307 };
static const double ref_dkl[9] = {
    4.5402989511704481, 2.2923311736424345, 0,
    2.8824328742926806, -3.182368078038305, 0,
    -2.6213428216596335, -1.3234780201742304, 8.4162774860657272 };
/* the unit DKL axes as RGB increments, by column: lum, L-M, S */
static const double ref_dkl_rgb[9] = {
    0.23094010767585041, 3.080313307241469, 0.06864359988388552,
    0.28867513459481281, -1.0311128493218227, -0.079473690462270277,
    0.17320508075688773, 0.045137269434734333, 0.36627380390229192 };

static double srgb_eotf(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double srgb_oetf(double l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }

/* --------------------------------------------------------------- CPU tests */

static void test_noise_cpu(void) {
    float buf[64 * 48];
    uint32_t h = 2166136261u;
    int i, below = 0;
    double sum = 0, sq = 0;
    ygfx_noise_fill(buf, 64, 48, 7, YGFX_UNIFORM);
    for (i = 0; i < 64 * 48; i++) {
        uint32_t b;
        memcpy(&b, &buf[i], 4);
        h = (h ^ b) * 16777619u;
        CHECK(buf[i] > -1.0f && buf[i] < 1.0f);
        sum += buf[i];
        below += buf[i] < 0;
    }
    /* Pinned: the same bits on every compiler and platform CI builds. */
    printf("noise uniform seed 7, 64x48: FNV-1a %08x, mean %.4f\n", (unsigned)h, sum / (64 * 48));
    CHECK(h == 0x5f8bf9f7u);   /* v0.9: triple32 and ygfx_hash2() */
    CHECK(fabs(sum / (64 * 48)) < 0.03);
    CHECK(below > 1400 && below < 1672);
    ygfx_noise_fill(buf, 64, 48, 7, YGFX_BINARY);
    for (i = 0; i < 64 * 48; i++) CHECK(buf[i] == 1.0f || buf[i] == -1.0f);
    ygfx_noise_gauss(buf, 64, 48, 3);
    sum = sq = 0;
    for (i = 0; i < 64 * 48; i++) { sum += buf[i]; sq += (double)buf[i] * buf[i]; }
    sum /= 64 * 48;
    sq = sq / (64 * 48) - sum * sum;
    CHECK(fabs(sum) < 0.06);
    CHECK(fabs(sq - 1.0) < 0.08);
    CHECK(ygfx_hash(0) == ygfx_hash(0) && ygfx_hash(1) != ygfx_hash(2));
    {   /* triple32 (v0.9) maps 0 to 0; ygfx_hash2() as written,
         * from the hash alone, and injective over a 65536 x 65536 window */
        uint32_t k = ygfx_hash(5u), m = ygfx_hash(k ^ 0x9E3779B9u);
        CHECK(ygfx_hash(0u) == 0u);
        CHECK(ygfx_hash2(-3, 70000, 5u) == ygfx_hash(((uint32_t)-3 + ((uint32_t)70000 << 16)) * (k | 1u) ^ m));
        CHECK(ygfx_hash2(1, 0, 5u) != ygfx_hash2(0, 1, 5u) && ygfx_hash2(65535, 0, 5u) != ygfx_hash2(0, 1, 5u) - 1u);
    }
    {   /* GAUSSIAN (v0.7): the table against quantiles found by bisection on
         * erfc (none of the header's arithmetic), its moments, and 10^6
         * checks against the normal CDF */
        const float* t = ygfx__gauss_table();
        static double q[65536];
        double ss = 0, k, worst = 0, var = 0, d = 0, m1 = 0, m2 = 0, beyond3 = 0;
        long bad_order = 0, bad_sym = 0;
        int n = 1000000;
        for (i = 0; i < 65536; i++) {
            double p = ((double)i + 0.5) / 65536.0, lo = -10, hi = 10;
            int it;
            for (it = 0; it < 200; it++) { double mid = 0.5 * (lo + hi); if (0.5 * erfc(-mid / sqrt(2.0)) < p) lo = mid; else hi = mid; }
            q[i] = 0.5 * (lo + hi);
            ss += q[i] * q[i];
        }
        k = 1.0 / sqrt(ss / 65536.0);
        for (i = 0; i < 65536; i++) {
            double want = q[i] * k;
            worst = maxd(worst, fabs(t[i] - want) / maxd(fabs(want), 1e-3));
            var += (double)t[i] * t[i];
            if (i && !(t[i] > t[i - 1])) bad_order++;
            if (t[65535 - i] != -t[i]) bad_sym++;
        }
        var /= 65536.0;
        for (i = 0; i < n; i++) {   /* checks along rows of a 1000 x 1000 field */
            double v = ygfx_noise_value(i % 1000, i / 1000, 11, YGFX_GAUSSIAN);
            m1 += v; m2 += v * v; beyond3 += fabs(v) > 3.0;
        }
        {   /* Kolmogorov-Smirnov: the empirical CDF from a histogram on the
             * 65536 table values (each value is one bin), against Phi */
            static long cnt[65536];
            long acc = 0;
            memset(cnt, 0, sizeof cnt);
            for (i = 0; i < n; i++) {
                uint32_t hh = ygfx_hash2(i % 1000, i / 1000, 11u);
                cnt[hh >> 16]++;
            }
            for (i = 0; i < 65536; i++) {
                double f0 = (double)acc / n, f1, phi = 0.5 * erfc(-(double)t[i] / sqrt(2.0));
                acc += cnt[i];
                f1 = (double)acc / n;
                d = maxd(d, maxd(fabs(f0 - phi), fabs(f1 - phi)));
            }
        }
        m1 /= n; m2 = m2 / n - m1 * m1;
        printf("noise gaussian: table against bisection on erfc %.2e (relative), order wrong %ld, symmetry wrong %ld, "
               "variance %.9f, largest %.4f; 10^6 checks: mean %+.4f, SD %.4f, beyond 3 SD %.5f, KS D %.5f\n",
               worst, bad_order, bad_sym, var, (double)t[65535], m1, sqrt(m2), beyond3 / n, d);
        CHECK_LE(worst, 2e-7);
        CHECK(bad_order == 0 && bad_sym == 0);
        CHECK_LE(fabs(var - 1.0), 1e-6);
        CHECK_LE(fabs(m1), 0.005);
        CHECK_LE(fabs(sqrt(m2) - 1.0), 0.005);
        CHECK_LE(d, 1.63 / sqrt((double)n));   /* the 1 % critical value */
        ygfx_noise_fill(buf, 64, 48, 7, YGFX_GAUSSIAN);
        h = 2166136261u;
        for (i = 0; i < 64 * 48; i++) {
            uint32_t b;
            memcpy(&b, &buf[i], 4);
            h = (h ^ b) * 16777619u;
        }
        /* Pinned as the uniform's: a C library whose log differs in the last
         * bit could change a table entry; CI's builds would show it here */
        printf("noise gaussian seed 7, 64x48: FNV-1a %08x\n", (unsigned)h);
        CHECK(h == 0x30886f1eu);   /* v0.9 */
        h = 2166136261u;
        for (i = 0; i < 65536; i++) {
            uint32_t bb;
            memcpy(&bb, &t[i], 4);
            h = (h ^ bb) * 16777619u;
        }
        printf("noise gaussian table: FNV-1a %08x\n", (unsigned)h);
        CHECK(h == 0x19ac94b1u);
    }
}

static void test_cal_srgb(void) {
    static ycol_cal c, c2;
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    /* IEC 61966-2-1:1999, as published (4 decimals) */
    static const double iec[9] = { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 };
    static unsigned char bytes[sizeof(ycol_cal)];
    char err[200];
    double e = 0, e17 = 0, e256 = 0;
    int k, gun, rc;
    CHECK(ycol_cal_nominal(&c, xy, 1.0f, 2.2) == YGFX_OK);
    for (k = 0; k < 9; k++) e = maxd(e, fabs(c.rgb_to_xyz[k] - iec[k]));
    printf("sRGB primaries to XYZ, against IEC 61966-2-1's printed matrix: max |diff| %.2e\n", e);
    /* 0.9505 is printed; the primaries and D65 give 0.95030 */
    CHECK_LE(e, 1.2e-4);      /* measured 3.9e-5 */
    CHECK(c.flags & YCOL_CAL_NOMINAL);
    CHECK(fabs(c.white_err) < 1e-4);
    /* The CLUT of a display with sRGB's transfer function is sRGB's
     * encoding, from 17 and from 64 levels per gun (the file holds 256
     * readings in all). */
    for (rc = 0; rc < 2; rc++) {
        int n = rc ? 64 : 17;
        double m = 0;
        ycol_cal_init(&c2);
        ycol_cal_add(&c2, YCOL_GUN_BLACK, 0, 0, 0, 0);
        for (gun = 0; gun < 3; gun++)
            for (k = 1; k < n; k++) ycol_cal_add(&c2, gun, (float)k / (float)(n - 1), (float)(80.0 * srgb_eotf((double)k / (n - 1))), 0, 0);
        CHECK(ycol_cal_derive(&c2, err, sizeof err) == YGFX_OK);
        for (k = 0; k < YCOL_CAL_MAX_LUT; k++) m = maxd(m, fabs(c2.lut[1][k] - srgb_oetf((double)k / (YCOL_CAL_MAX_LUT - 1))));
        if (rc) e256 = m; else e17 = m;
    }
    printf("CLUT against sRGB's encoding: 17 levels per gun max |diff| %.2e, 64 levels %.2e\n", e17, e256);
    CHECK_LE(e17, 4e-3);      /* measured 1.30e-3 */
    CHECK_LE(e256, 1.1e-4);   /* measured 3.64e-5 */
    /* round trip, then every refusal */
    CHECK(ycol_cal_save(&c2, bytes, sizeof bytes) == (int)sizeof bytes);
    memset(&c, 0, sizeof c);
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes, err, sizeof err) == YGFX_OK);
    CHECK(memcmp(&c, &c2, sizeof c) == 0);
    CHECK(ycol_cal_check(&c, err, sizeof err) == YGFX_OK);
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes - 1, err, sizeof err) == YGFX_ERR_FORMAT);
    bytes[1000] ^= 1;
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes, err, sizeof err) == YGFX_ERR_FORMAT);
    CHECK(strstr(err, "CRC") != NULL);
    bytes[1000] ^= 1;
    c2.lut[0][100] += 1e-4f;   /* a table its readings do not give */
    ycol_cal_save(&c2, bytes, sizeof bytes);
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes, err, sizeof err) == YGFX_ERR_FORMAT);
    CHECK(strstr(err, "CLUT") != NULL);
    /* readings that do not rise are refused, with the level */
    ycol_cal_init(&c2);
    ycol_cal_add(&c2, YCOL_GUN_BLACK, 0, 0.5f, 0, 0);
    for (gun = 0; gun < 3; gun++) {
        ycol_cal_add(&c2, gun, 0.5f, 10.0f, 0, 0);
        ycol_cal_add(&c2, gun, 0.75f, 9.0f, 0, 0);
        ycol_cal_add(&c2, gun, 1.0f, 30.0f, 0, 0);
    }
    CHECK(ycol_cal_derive(&c2, err, sizeof err) == YGFX_ERR_RANGE);
    CHECK(strstr(err, "0.75") != NULL);
    /* no cone space without spectra */
    {
        float bg[3] = { 0.5f, 0.5f, 0.5f }, cc[3] = { 0.1f, 0, 0 }, dir[3];
        CHECK(ycol_cal_dir_cone(&c, bg, cc, dir) == YGFX_ERR_REFUSED);
    }
}

static void test_cones_dkl(void) {
    static ycol_cal c;
    float r[81], g[81], b[81];
    double l[3], m[9], e = 0;
    char err[200];
    const float bg[3] = { 0.4f, 0.5f, 0.3f };
    int i, k;
    /* CVRL linss2_10e_1 rows, as published */
    ycol_cone_fundamentals(YCOL_CONES_SS2, 440.0, l);
    CHECK(fabs(l[0] / 4.02563e-2 - 1) < 1e-7 && fabs(l[1] / 6.47782e-2 - 1) < 1e-7 && fabs(l[2] / 9.91020e-1 - 1) < 1e-7);
    ycol_cone_fundamentals(YCOL_CONES_SS2, 570.0, l);
    CHECK(fabs(l[0] / 9.99993e-1 - 1) < 1e-7 && fabs(l[1] / 8.13509e-1 - 1) < 1e-7 && fabs(l[2] / 2.81800e-4 - 1) < 1e-7);
    ycol_cone_fundamentals(YCOL_CONES_SS2, 700.0, l);
    CHECK(l[2] == 0.0 && l[0] > 0.0);
    ycol_cone_fundamentals(YCOL_CONES_SS2, 389.0, l);
    CHECK(l[0] == 0.0);
    for (i = 0; i < 81; i++) { r[i] = b_monitor[i][0]; g[i] = b_monitor[i][1]; b[i] = b_monitor[i][2]; }
    ycol_cal_init(&c);
    ycol_cal_add(&c, YCOL_GUN_BLACK, 0, 0, 0, 0);
    for (k = 0; k < 3; k++) ycol_cal_add(&c, k, 1, 1, 0, 0);
    CHECK(ycol_cal_set_spectra(&c, 380, 5, 81, r, g, b, NULL) == YGFX_OK);
    CHECK(ycol_cal_derive(&c, err, sizeof err) == YGFX_OK);
    for (k = 0; k < 9; k++) e = maxd(e, fabs(c.rgb_to_lms[k] - ref_rgb_to_lms[k]) / fabs(ref_rgb_to_lms[k]));
    printf("RGB to LMS against Psychtoolbox (B_monitor, SS2): max relative diff %.2e\n", e);
    CHECK_LE(e, 1e-6);
    CHECK(ycol_cal_dkl_matrix(&c, bg, m) == YGFX_OK);
    e = 0;
    for (k = 0; k < 9; k++) e = maxd(e, fabs(m[k] - ref_dkl[k]) / (fabs(ref_dkl[k]) + 1e-12));
    printf("DKL matrix against ComputeDKL_M: max relative diff %.2e\n", e);
    CHECK_LE(e, 1e-6);
    e = 0;
    for (k = 0; k < 3; k++) {
        float unit[3] = { 0, 0, 0 }, dir[3];
        double lb[3] = { 0, 0, 0 }, lc[3] = { 0, 0, 0 }, inc[3], v;
        int j;
        unit[k] = 1.0f;
        CHECK(ycol_cal_dir_dkl(&c, bg, unit, dir) == YGFX_OK);
        for (j = 0; j < 3; j++) e = maxd(e, fabs(dir[j] - ref_dkl_rgb[j * 3 + k]) / fabs(ref_dkl_rgb[j * 3 + k]));
        /* the axes isolate: L-M keeps S and luminance, S keeps L and M
         * (the cone increment of the float dir, in double) */
        ycol_cal_lms(&c, bg, lb);
        for (j = 0; j < 3; j++)
            inc[j] = c.rgb_to_lms[j * 3] * dir[0] + c.rgb_to_lms[j * 3 + 1] * dir[1] + c.rgb_to_lms[j * 3 + 2] * dir[2];
        (void)lc;
        v = 0.68990272 * inc[0] + 0.34832189 * inc[1];
        if (k == 1) { CHECK(fabs(inc[2]) < 1e-6 * fabs(inc[0])); CHECK(fabs(v) < 1e-5 * fabs(inc[0])); }
        if (k == 2) { CHECK(fabs(inc[0]) < 1e-5 * fabs(inc[2])); CHECK(fabs(inc[1]) < 1e-5 * fabs(inc[2])); }
        if (k == 0) CHECK(fabs(inc[0] / lb[0] - inc[1] / lb[1]) < 1e-5 * fabs(inc[0] / lb[0]));
    }
    printf("DKL unit axes as RGB against Psychtoolbox: max relative diff %.2e\n", e);
    CHECK_LE(e, 1e-5);
    {   /* cone contrast: 10% L only */
        float cc[3] = { 0.1f, 0, 0 }, dir[3], p[3];
        double lb[3] = { 0, 0, 0 }, lc[3] = { 0, 0, 0 };
        int j;
        CHECK(ycol_cal_dir_cone(&c, bg, cc, dir) == YGFX_OK);
        for (j = 0; j < 3; j++) p[j] = bg[j] + dir[j];
        ycol_cal_lms(&c, bg, lb);
        ycol_cal_lms(&c, p, lc);
        CHECK(fabs(lc[0] / lb[0] - 1.1) < 1e-5);
        CHECK(fabs(lc[1] / lb[1] - 1.0) < 1e-5 && fabs(lc[2] / lb[2] - 1.0) < 1e-5);
    }
    {
        float d3[3], dir[3] = { 0.1f, -0.2f, 0.05f }, bgm[3] = { 0.5f, 0.5f, 0.25f };
        ycol_dkl_from_sph(90, 0, 2, d3);
        CHECK(fabs(d3[0] - 2) < 1e-6 && fabs(d3[1]) < 1e-6);
        ycol_dkl_from_sph(0, 90, 1, d3);
        CHECK(fabs(d3[2] - 1) < 1e-6 && fabs(d3[0]) < 1e-6);
        CHECK(fabsf(ycol_max_contrast(bgm, dir) - 2.5f) < 1e-6f);
    }
}

/* v0.16: the enum table. The name switches have no default, so -Wswitch
 * (gcc and clang -Wall; CI's -Werror) stops the build when an enum gains a
 * value, and the value check then fails until its row lists it. */
static const char* align_name(int v) {
    switch ((ygfx_align)v) {
    case YGFX_CENTER: return "center";
    case YGFX_TOP_LEFT: return "top_left";
    case YGFX_TOP: return "top";
    case YGFX_TOP_RIGHT: return "top_right";
    case YGFX_LEFT: return "left";
    case YGFX_RIGHT: return "right";
    case YGFX_BOTTOM_LEFT: return "bottom_left";
    case YGFX_BOTTOM: return "bottom";
    case YGFX_BOTTOM_RIGHT: return "bottom_right";
    }
    return NULL;
}
static const char* shape_name(int v) {
    switch ((ygfx_shape_kind)v) {
    case YGFX_RECT: return "rect";
    case YGFX_CIRCLE: return "circle";
    case YGFX_ANNULUS: return "annulus";
    case YGFX_LINE: return "line";
    case YGFX_POLYGON: return "polygon";
    case YGFX_CROSS: return "cross";
    case YGFX_NO_APERTURE: return "no_aperture";
    case YGFX_MASK_TEX: return "mask_tex";
    case YGFX_RRECT: return "rrect";
    case YGFX_ARC: return "arc";
    case YGFX_PIE: return "pie";
    case YGFX_CAPSULE: return "capsule";
    case YGFX_NGON: return "ngon";
    case YGFX_STAR: return "star";
    case YGFX_ELLIPSE: return "ellipse";
    case YGFX_QBEZIER: return "qbezier";
    case YGFX_POLYLINE: return "polyline";
    case YGFX_COMPOUND: return "compound";
    }
    return NULL;
}
static const char* edge_name(int v) {
    switch ((ygfx_edge)v) {
    case YGFX_EDGE_HARD: return "hard";
    case YGFX_EDGE_COSINE: return "cosine";
    case YGFX_EDGE_GAUSSIAN: return "gaussian";
    }
    return NULL;
}
static const char* dist_name(int v) {
    switch ((ygfx_noise_dist)v) {
    case YGFX_UNIFORM: return "uniform";
    case YGFX_BINARY: return "binary";
    case YGFX_GAUSSIAN: return "gaussian";
    case YGFX_SIMPLEX: return "simplex";
    }
    return NULL;
}
static const char* stroke_align_name(int v) {
    switch ((ygfx_stroke_align)v) {
    case YGFX_STROKE_CENTER: return "center";
    case YGFX_STROKE_INSIDE: return "inside";
    case YGFX_STROKE_OUTSIDE: return "outside";
    }
    return NULL;
}
static const char* join_name(int v) {
    switch ((ygfx_join)v) {
    case YGFX_JOIN_ROUND: return "round";
    case YGFX_JOIN_MITER: return "miter";
    case YGFX_JOIN_BEVEL: return "bevel";
    }
    return NULL;
}
static const char* cap_name(int v) {
    switch ((ygfx_cap)v) {
    case YGFX_CAP_BUTT: return "butt";
    case YGFX_CAP_ROUND: return "round";
    case YGFX_CAP_SQUARE: return "square";
    }
    return NULL;
}
static const char* blend_name(int v) {
    switch ((ygfx_blend_mode)v) {
    case YGFX_BLEND_MODE_OVER: return "over";
    case YGFX_BLEND_MODE_ADD: return "add";
    case YGFX_BLEND_MODE_MULTIPLY: return "multiply";
    }
    return NULL;
}

/* A row's "name=value" list: rising, each the C enum's name, the values in
 * want (bit v) and nothing else. want 0: every value name_of knows. */
static int enum_values_ok(const ygfx_param* p, const char* (*name_of)(int), uint64_t want) {
    const char* s = p->values;
    uint64_t seen = 0;
    int prev = -1, v;
    if (!want)
        for (v = 0; v < 64; v++)
            if (name_of(v)) want |= 1ull << v;
    if (!s) return 0;
    while (*s) {
        const char* eq = strchr(s, '=');
        const char* end = strchr(s, ',');
        char name[48];
        if (!end) end = s + strlen(s);
        if (!eq || eq > end || (size_t)(eq - s) >= sizeof name) return 0;
        memcpy(name, s, (size_t)(eq - s));
        name[eq - s] = '\0';
        v = atoi(eq + 1);
        if (v <= prev || v >= 64 || v < p->min || v > p->max || !name_of(v) || strcmp(name_of(v), name)) return 0;
        seen |= 1ull << v;
        prev = v;
        s = *end ? end + 1 : end;
    }
    return seen == want && prev == (int)p->max;
}

/* What row e must name: its ygfx_stim field and, for a flag, the bit; the
 * kinds whose descs have the field. */
typedef struct enum_field {
    size_t off; uint32_t flag; const char* (*name_of)(int); uint64_t want; uint32_t kinds;
} enum_field;

static int enum_read(const ygfx_stim* s, int e, uint32_t flag) {
    switch (e) {
    case YGFX_E_PLACE: return s->place;
    case YGFX_E_ANCHOR: {
        int i;
        for (i = 0; i < 9; i++) {
            ygfx_stim t;
            ygfx_shape_desc sd;
            memset(&sd, 0, sizeof sd);
            sd.anchor = (ygfx_align)i;
            t = ygfx_shape(&sd);
            if (t.ax == s->ax && t.ay == s->ay) return i;
        }
        return -1;
    }
    case YGFX_E_SHAPE: case YGFX_E_APERTURE: case YGFX_E_DOTS_APERTURE: return s->shape;
    case YGFX_E_EDGE: return s->edge;
    case YGFX_E_SQUARE: case YGFX_E_DIST: return s->wave;
    case YGFX_E_OCTAVES: return s->gn_octaves;
    case YGFX_E_STROKE_ALIGN: return s->stroke_align;
    case YGFX_E_JOIN: return s->join;
    case YGFX_E_CAP: return s->cap;
    case YGFX_E_BLEND: return s->blend;
    case YGFX_E_DASH_SNAP: return s->dash_snap;
    default: return (s->flags & flag) != 0;
    }
}

static void test_enum_params(void) {
#define EK(k) (1u << (k))
#define EK_APT (EK(YGFX_SHAPE) | EK(YGFX_GRATING) | EK(YGFX_NOISE) | EK(YGFX_DOTS) | EK(YGFX_USER))
    static const enum_field want[YGFX_E_COUNT] = {
        { offsetof(ygfx_stim, place), 0, align_name, 0, 0x1FEu },
        { offsetof(ygfx_stim, ax), 0, align_name, 0, 0x1FEu },
        { offsetof(ygfx_stim, shape), 0, shape_name, 0, EK(YGFX_SHAPE) },
        { offsetof(ygfx_stim, shape), 0, shape_name, 0xFFull,   /* RECT to MASK_TEX: v0.1's apertures */
          EK(YGFX_GRATING) | EK(YGFX_NOISE) | EK(YGFX_USER) },
        { offsetof(ygfx_stim, shape), 0, shape_name, (1ull << YGFX_RECT) | (1ull << YGFX_CIRCLE) | (1ull << YGFX_NO_APERTURE),
          EK(YGFX_DOTS) },
        { offsetof(ygfx_stim, edge), 0, edge_name, 0, EK_APT },
        { offsetof(ygfx_stim, wave), 0, NULL, 0, EK(YGFX_GRATING) },
        { offsetof(ygfx_stim, wave), 0, dist_name, 0, EK(YGFX_NOISE) },
        { offsetof(ygfx_stim, gn_octaves), 0, NULL, 0, EK(YGFX_NOISE) },
        { offsetof(ygfx_stim, stroke_align), 0, stroke_align_name, 0, EK_APT },
        { offsetof(ygfx_stim, join), 0, join_name, 0, EK(YGFX_SHAPE) },
        { offsetof(ygfx_stim, cap), 0, cap_name, 0, EK(YGFX_SHAPE) },
        { offsetof(ygfx_stim, blend), 0, blend_name, 0, EK(YGFX_SHAPE) | EK(YGFX_TEXT) },
        { offsetof(ygfx_stim, dash_snap), 0, NULL, 0, EK(YGFX_SHAPE) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_MODULATION, NULL, 0, EK(YGFX_IMAGE) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_LINEAR, NULL, 0, EK(YGFX_IMAGE) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_ADD, NULL, 0, EK(YGFX_IMAGE) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_PREMULTIPLIED, NULL, 0, EK(YGFX_IMAGE) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_COVERAGE, NULL, 0, EK(YGFX_IMAGE) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_ALIASED, NULL, 0, EK(YGFX_TEXT) },
        { offsetof(ygfx_stim, flags), YGFX_STIM_RAYS, NULL, 0, EK(YGFX_TEXT) },
    };
#undef EK
#undef EK_APT
    ygfx_stim zero[YGFX_KIND_COUNT];
    int n = -1, e, k, j, v, rows = 0;
    const ygfx_param* p = ygfx_enum_params(&n);
    g_where = "enum_params";
    CHECK(p != NULL && n == YGFX_E_COUNT && ygfx_enum_params(NULL) == p);
    /* each kind's stimulus from a zero desc: what a zero field gives */
    memset(zero, 0, sizeof zero);
    {
        ygfx_shape_desc a; ygfx_grating_desc b; ygfx_gabor_desc c; ygfx_dots_desc d; ygfx_image_desc i;
        ygfx_noise_desc o; ygfx_crun_desc t; ygfx_user_desc u;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c); memset(&d, 0, sizeof d);
        memset(&i, 0, sizeof i); memset(&o, 0, sizeof o); memset(&t, 0, sizeof t); memset(&u, 0, sizeof u);
        zero[YGFX_SHAPE] = ygfx_shape(&a);
        zero[YGFX_GRATING] = ygfx_grating(&b);
        zero[YGFX_GABOR] = ygfx_gabor(&c);
        zero[YGFX_DOTS] = ygfx_dots(&d);
        zero[YGFX_IMAGE] = ygfx_image(NULL, &i);
        zero[YGFX_NOISE] = ygfx_noise(&o);
        zero[YGFX_TEXT] = ygfx_crun(NULL, &t);
        zero[YGFX_USER] = ygfx_user(&u);
    }
    for (k = 1; k < YGFX_KIND_COUNT; k++) CHECK(zero[k].kind == k);
    CHECK(ygfx_enum_get(NULL, 0) == YGFX_ERR_ARG && ygfx_enum_set(NULL, 0, 0) == YGFX_ERR_ARG);
    CHECK(ygfx_enum_get(&zero[YGFX_SHAPE], -1) == YGFX_ERR_ARG && ygfx_enum_get(&zero[YGFX_SHAPE], YGFX_E_COUNT) == YGFX_ERR_ARG);
    for (e = 0; e < n; e++) {
        const ygfx_param* r = &p[e];
        int is_enum = !strcmp(r->type, "enum");
        CHECK(r->name && r->type && r->unit && r->doc && r->doc[0]);
        CHECK(r->min <= r->def && r->def <= r->max);
        CHECK(r->offset == want[e].off);
        CHECK(r->kinds == want[e].kinds);
        CHECK(is_enum || !strcmp(r->type, "bool") || !strcmp(r->type, "i32"));
        CHECK(is_enum == (r->values != NULL) && is_enum == (want[e].name_of != NULL));
        if (is_enum && !enum_values_ok(r, want[e].name_of, want[e].want)) {
            fprintf(stderr, "gfx_test [%s]: FAIL: %s's values [%s] are not its enum's\n", g_where, r->name, r->values);
            g_failures++;
        }
        /* no kind reads two rows of one name */
        for (j = 0; j < e; j++) CHECK(strcmp(p[j].name, r->name) != 0 || (p[j].kinds & r->kinds) == 0);
        for (k = 1; k < YGFX_KIND_COUNT; k++) {
            ygfx_stim s = zero[k];
            if (!(r->kinds & (1u << k))) {
                CHECK(ygfx_enum_get(&s, e) == YGFX_ERR_ARG && ygfx_enum_set(&s, e, (int)r->def) == YGFX_ERR_ARG);
                continue;
            }
            rows++;
            /* the default is what a zero desc field gives */
            if (ygfx_enum_get(&s, e) != (int)r->def || enum_read(&s, e, want[e].flag) != (int)r->def) {
                fprintf(stderr, "gfx_test [%s]: FAIL: %s on kind %d: a zero field gives %d, the table says %g\n",
                        g_where, r->name, k, enum_read(&s, e, want[e].flag), r->def);
                g_failures++;
            }
            /* every value it takes lands in its field, and nothing else */
            for (v = (int)r->min - 1; v <= (int)r->max + 1; v++) {
                int takes = v >= r->min && v <= r->max;
                if (is_enum) {
                    char key[16];
                    snprintf(key, sizeof key, "=%d", v);
                    takes = 0;
                    {
                        const char* f = r->values;
                        size_t kl = strlen(key);
                        while ((f = strstr(f, key)) != NULL) {
                            if (f[kl] == ',' || f[kl] == '\0') takes = 1;
                            f += kl;
                        }
                    }
                }
                s = zero[k];
                if (!takes) { CHECK(ygfx_enum_set(&s, e, v) == YGFX_ERR_ARG && !memcmp(&s, &zero[k], sizeof s)); continue; }
                CHECK(ygfx_enum_set(&s, e, v) == YGFX_OK);
                CHECK(ygfx_enum_get(&s, e) == v && enum_read(&s, e, want[e].flag) == v);
                CHECK(ygfx_enum_set(&s, e, (int)r->def) == YGFX_OK && !memcmp(&s, &zero[k], sizeof s));
            }
        }
    }
    {   /* an anchor between the nine points is no value */
        ygfx_stim s = zero[YGFX_SHAPE];
        s.ax = 0.25f;
        CHECK(ygfx_enum_get(&s, YGFX_E_ANCHOR) == YGFX_ERR_ARG);
    }
    printf("enum params: %d rows, %d row-kind pairs: ok\n", n, rows);
}

static void test_tables(void) {
    int n = 0, m = 0, i, j;
    const ygfx_param* p = ygfx_params(&n);
    const yscr_param* d = ygfx_desc_params(&m);
    ygfx_stim s;
    ygfx_bind b[3];
    float v[4] = { 0.25f, 3.0f, 0.0f, 9.0f };
    char buf[64];
    CHECK(n == YGFX_P_COUNT && m > 5 && d);
    for (i = 0; i < n; i++) {
        CHECK(p[i].offset + sizeof(float) <= sizeof(ygfx_stim));
        CHECK(p[i].values == NULL && !strcmp(p[i].type, "f32"));   /* v0.16: floats only */
        CHECK(p[i].min <= p[i].def && p[i].def <= p[i].max);
        for (j = 0; j < i; j++) CHECK(strcmp(p[i].name, p[j].name) != 0);
    }
    CHECK(p[YGFX_P_X].offset == offsetof(ygfx_stim, x));
    CHECK(p[YGFX_P_AY].offset == offsetof(ygfx_stim, ay));
    CHECK(p[YGFX_P_CONTRAST].offset == offsetof(ygfx_stim, contrast));
    CHECK(p[YGFX_P_P0 + 31].offset == offsetof(ygfx_stim, p[31]));
    s = ygfx_gabor(NULL);
    memset(b, 0, sizeof b);
    b[0].stim = &s; b[0].param = YGFX_P_CONTRAST; b[0].channel = 0;
    b[1].stim = &s; b[1].param = YGFX_P_X; b[1].channel = 1;
    b[2].stim = &s; b[2].param = YGFX_P_COUNT; b[2].channel = 3;   /* bad */
    CHECK(ygfx_apply(b, 3, v) == YGFX_ERR_ARG && s.contrast == 1.0f && s.x == 0.0f);
    CHECK(ygfx_apply(b, 2, v) == 2 && s.contrast == 0.25f && s.x == 3.0f);
    /* the wrapper */
    {
        int len = ygfx_shader_wrap("float ysp_main(vec2 p) { return 0.0; }", YGFX_MODULATION, NULL, 0);
        char* t;
        CHECK(len < -1000);
        t = (char*)malloc((size_t)(-len - 1000) + 1);
        CHECK(ygfx_shader_wrap("float ysp_main(vec2 p) { return 0.0; }", YGFX_MODULATION, t, (size_t)(-len - 1000) + 1) == -len - 1000);
        CHECK(strstr(t, "#line 1\nfloat ysp_main") != NULL && strncmp(t, "#version 300 es", 15) == 0);
        free(t);
        CHECK(ygfx_shader_wrap("x", YGFX_COLOR, buf, sizeof buf) < 0);
    }
    /* v0.10.3: the contract's version, and the wrapper's text pinned per
     * mode. A different text fails here: if what the wrapper does to a
     * body changed, raise YGFX_SHADER_CONTRACT; then update the pins. */
    {
        static const uint64_t pin[3] = { WRAP_PIN0, WRAP_PIN1, WRAP_PIN2 };
        static const char* const bodies[3] = { "float ysp_main(vec2 p) { return 0.0; }",
                                               "vec4 ysp_main(vec2 p) { return vec4(1.0); }",
                                               "vec3 ysp_main(vec2 p) { return vec3(0.0); }" };
        static char wt[65536];
        int md;
        volatile int contract = YGFX_SHADER_CONTRACT;   /* volatile: MSVC C4127 on a constant condition */
        CHECK(contract == 2);
        for (md = 0; md < 3; md++) {
            int wn = ygfx_shader_wrap(bodies[md], (ygfx_shader_mode)md, wt, sizeof wt);
            uint64_t h = 0xCBF29CE484222325ULL;
            int k;
            for (k = 0; k < wn; k++) { h ^= (uint8_t)wt[k]; h *= 0x100000001B3ULL; }
            if (wn <= 0 || h != pin[md]) printf("wrapper text, mode %d: FNV-1a 64 0x%016llxULL\n", md, (unsigned long long)h);
            CHECK(wn > 0 && h == pin[md]);
        }
    }
}

/* v0.17: the gfx's own desc table (yscr_param's shape, with values). */
#include "param_table_check.h"

static void ptc_fail(int line, const char* what) {
    fprintf(stderr, "gfx_test [%s]: FAIL line %d: %s\n", g_where, line, what);
    g_failures++;
}

/* No default, so -Wswitch (CI's -Werror) stops the build when an enum
 * gains a value. cones is ysp/color.h's. */
static const char* desc_units_name(int v) {
    switch ((ygfx_units)v) {
    case YGFX_PX: return "px";
    case YGFX_DEG: return "deg";
    }
    return NULL;
}
static const char* desc_dither_name(int v) {
    switch ((ygfx_dither)v) {
    case YGFX_DITHER_NONE: return "none";
    case YGFX_DITHER_ORDERED: return "ordered";
    case YGFX_DITHER_NOISE: return "noise";
    }
    return NULL;
}
static const char* desc_output_name(int v) {
    switch ((ygfx_output)v) {
    case YGFX_OUT_8: return "out_8";
    case YGFX_OUT_10: return "out_10";
    case YGFX_OUT_MONO_PP: return "out_mono_pp";
    case YGFX_OUT_COLOR_PP: return "out_color_pp";
    }
    return NULL;
}
static const char* desc_stereo_name(int v) {
    switch ((ygfx_stereo)v) {
    case YGFX_MONO: return "mono";
    case YGFX_SIDE_BY_SIDE: return "side_by_side";
    case YGFX_ROW_INTERLEAVED: return "row_interleaved";
    case YGFX_FRAME_SEQUENTIAL: return "frame_sequential";
    }
    return NULL;
}
static const char* desc_cones_name(int v) {
    switch ((ycol_cones)v) {
    case YCOL_CONES_SS2: return "ss2";
    case YCOL_CONES_SS10: return "ss10";
    }
    return NULL;
}

#define GD_F(n, f, e) { n, offsetof(ygfx_desc, f), sizeof(((ygfx_desc*)0)->f), e }
static const ptc_slot g_gd_fields[] = {
    GD_F("background.r", background[0], NULL), GD_F("background.g", background[1], NULL),
    GD_F("background.b", background[2], NULL), GD_F("units", units, desc_units_name),
    GD_F("view.distance_mm", view.distance_mm, NULL), GD_F("view.width_mm", view.width_mm, NULL),
    GD_F("dither", dither, desc_dither_name), GD_F("seed", seed, NULL), GD_F("output", output, desc_output_name),
    GD_F("stereo", stereo, desc_stereo_name), GD_F("max_draws", max_draws, NULL), GD_F("cones", cones, desc_cones_name),
};
#undef GD_F
#define GD_NF ((int)(sizeof g_gd_fields / sizeof g_gd_fields[0]))

/* Every row is a real field with its C enum's values, and its default is
 * what open() makes of a zero field: max_draws from the open gfx (a SIM
 * screen and the null backend); every other zero stays 0. */
static void test_desc_params(void) {
    static yscr_screen scr;
    static ygfx_gfx g;
    ptc_row rows[16];
    yscr_desc d;
    ygfx_desc gd;
    int n = -1, i;
    const yscr_param* p = ygfx_desc_params(&n);
    g_where = "desc_params";
    CHECK(p != NULL && n == GD_NF && ygfx_desc_params(NULL) == p);
    if (!p || n > 16) return;
    for (i = 0; i < n; i++) PTC_ROW(rows[i], p[i]);
    ptc_check_slots(rows, n, g_gd_fields, GD_NF);
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 1000000;
    CHECK(yscr_open(&scr, &d));
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    CHECK(ygfx_open(&g, &gd));
    for (i = 0; i < n; i++) {
        double want = !strcmp(p[i].name, "max_draws") ? (double)g.max_draws : 0.0;
        if (want != p[i].def) {
            fprintf(stderr, "gfx_test [%s]: FAIL: row %s: a zero field means %g, the table says %g\n", g_where, p[i].name,
                    want, p[i].def);
            g_failures++;
        }
    }
    ygfx_close(&g);
    yscr_close(&scr);
}
#undef GD_NF

/* The transform, without GL: a 800 x 600 simulated screen and the null backend. */
static void test_coordinates(void) {
    static yscr_screen scr;
    static ygfx_gfx g;
    yscr_desc d;
    ygfx_desc gd;
    ygfx_stim s;
    ygfx_shape_desc sd;
    float x, y, x0, y0, x1, y1, lx, ly;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 1000000;
    CHECK(yscr_open(&scr, &d));
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    CHECK(ygfx_open(&g, &gd));
    ygfx_size(&g, &x, &y);
    CHECK(x == 800 && y == 600);
    ygfx_center(&g, &x, &y);
    CHECK(x == 400 && y == 300);
    s = ygfx_shape(NULL);
    s.x = 100;
    ygfx_resolve(&g, &s, &x, &y);
    CHECK(x == 500 && y == 300);                       /* .x = 100: right of the center */
    memset(&sd, 0, sizeof sd);
    sd.place = YGFX_TOP_LEFT; sd.x = 10; sd.y = 10; sd.w = 100; sd.h = 50;
    s = ygfx_shape(&sd);
    ygfx_resolve(&g, &s, &x, &y);
    CHECK(x == 10 && y == 10);                         /* in from the corner, y down */
    ygfx_bounds(&g, &s, &x0, &y0, &x1, &y1);
    CHECK(x0 == -40 && y0 == -15 && x1 == 60 && y1 == 35);   /* anchored at its center */
    sd.anchor = YGFX_TOP_LEFT; sd.x = 0; sd.y = 0;
    s = ygfx_shape(&sd);
    CHECK(s.ax == 0.0f && s.ay == 0.0f);
    ygfx_bounds(&g, &s, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 100 && y1 == 50);
    ygfx_local(&g, &s, 50, 25, &lx, &ly);
    CHECK(lx == 0 && ly == 0);
    ygfx_local(&g, &s, 60, 35, &lx, &ly);
    CHECK(lx == 10 && ly == 10);                       /* local axes are the screen's at ori 0 */
    CHECK(ygfx_hit(&g, &s, 99.5f, 49.5f) && !ygfx_hit(&g, &s, 100.5f, 10));
    /* ori 90 about the top-left anchor: clockwise, so the box hangs below
     * the anchor and to its left */
    s.ori = 90;
    ygfx_bounds(&g, &s, &x0, &y0, &x1, &y1);
    CHECK(fabsf(x0 + 50) < 1e-4f && fabsf(y0) < 1e-4f && fabsf(x1) < 1e-4f && fabsf(y1 - 100) < 1e-4f);
    ygfx_local(&g, &s, -10, 80, &lx, &ly);           /* local x runs down the screen */
    CHECK(fabsf(lx - 30) < 1e-4f && fabsf(ly + 15) < 1e-4f);  /* local y points left */
    sd.anchor = YGFX_BOTTOM_RIGHT; sd.place = YGFX_BOTTOM_RIGHT;
    s = ygfx_shape(&sd);
    ygfx_bounds(&g, &s, &x0, &y0, &x1, &y1);
    CHECK(x0 == 700 && y0 == 550 && x1 == 800 && y1 == 600);
    ygfx_close(&g);
    /* degrees: one scale factor */
    gd.units = YGFX_DEG;
    gd.view.distance_mm = 573.0f;
    gd.view.width_mm = 400.0f;
    CHECK(ygfx_open(&g, &gd));
    s = ygfx_shape(NULL);
    s.x = 1;
    ygfx_resolve(&g, &s, &x, &y);
    CHECK(fabs(x - (400.0 + 2.0 * 573.0 * tan(3.14159265358979 / 180.0))) < 1e-3);
    ygfx_close(&g);
    yscr_close(&scr);
}

/* The null backend runs the frame; the ring gets the open and clipped records. */
static void test_null_and_ring(void) {
    static yscr_screen scr;
    static ygfx_gfx g;
    static unsigned char mem[64 * 64];
    yrt_ring ring;
    yrt_ring_desc rd;
    yrt_event ev[16];
    yscr_desc d;
    ygfx_desc gd;
    yscr_frame f;
    ygfx_gabor_desc gab;
    ygfx_stim s;
    float px[4];
    int n, i, opens = 0, clips = 0;
    memset(&ring, 0, sizeof ring);
    memset(&rd, 0, sizeof rd);
    rd.memory = mem;
    rd.bytes = sizeof mem;
    CHECK(yrt_ring_open(&ring, &rd));
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 1000000;
    CHECK(yscr_open(&scr, &d));
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.ring = &ring;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    CHECK(ygfx_open(&g, &gd));
    memset(&gab, 0, sizeof gab);
    gab.sigma = 10; gab.sf = 0.1f; gab.contrast = 1.5f;   /* leaves 0..1 */
    s = ygfx_gabor(&gab);
    CHECK(ygfx_end(&g) == YGFX_ERR_ORDER);
    CHECK(strstr(ygfx_error(&g), "ysp_gfx: end: ") != NULL);   /* v0.12: a message for each failure */
    CHECK(ygfx_draw(&g, &s) == YGFX_ERR_ORDER);
    CHECK(strstr(ygfx_error(&g), "ysp_gfx: draw: ") != NULL);
    CHECK(ygfx_texture(&g, NULL).id == 0 && strstr(ygfx_error(&g), "ysp_gfx: texture: ") != NULL);
    for (i = 0; i < 2; i++) {
        CHECK(yscr_begin(&scr, &f) == YSCR_OK);
        CHECK(ygfx_begin(&g, &f) == YGFX_OK);
        CHECK(ygfx_begin(&g, &f) == YGFX_ERR_ORDER);
        CHECK(strstr(ygfx_error(&g), "ysp_gfx: begin: ") != NULL);
        CHECK(ygfx_texture_update(&g, ygfx_texture(&g, NULL), 0, 0, 1, 1, px, 0) == YGFX_ERR_ORDER);
        CHECK(strstr(ygfx_error(&g), "ysp_gfx: texture_update: ") != NULL);
        if (i == 0) CHECK(ygfx_draw(&g, &s) == YGFX_OK);
        CHECK(ygfx_end(&g) == YGFX_OK);
        CHECK(strstr(ygfx_error(&g), "ysp_gfx: texture_update: ") != NULL);   /* a success keeps it */
        CHECK(yscr_flip(&scr) == YSCR_OK);
    }
    CHECK(ygfx_clipped(&g) == 1);
    CHECK(ygfx_prime(&g) == YGFX__N_BUILTIN + YGFX__N_VSPEC + 1);   /* v0.11: and the output stage */
    CHECK(ygfx_read_scene(&g, 0, 0, 1, 1, px) == YGFX_ERR_NOT_IMPLEMENTED);
    n = yrt_ring_drain(&ring, ev, 16);
    for (i = 0; i < n; i++) {
        if (ev[i].source != YRT_SRC_GFX) continue;
        if (ev[i].kind == YGFX_EV_OPEN) opens++;
        if (ev[i].kind == YGFX_EV_CLIPPED) { clips++; CHECK(ev[i].aux == 1 && ev[i].u.i64[0] == 0); }
    }
    CHECK(opens == 1 && clips == 1);   /* none for the frame without a clip */
    ygfx_close(&g);
    yscr_close(&scr);
    /* refusals */
    gd.output = YGFX_OUT_10;
    CHECK(!ygfx_open(&g, &gd) && strstr(ygfx_error(&g), "8-bit") != NULL);
    gd.output = YGFX_OUT_8;
    gd.stereo = YGFX_SIDE_BY_SIDE;
    CHECK(!ygfx_open(&g, &gd));
    gd.stereo = YGFX_MONO;
    gd.screen = NULL;
    CHECK(!ygfx_open(&g, &gd));
}

/* ------------------------------------------------- v0.12 capture, CPU half */

static unsigned char* t_slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    unsigned char* b;
    long k;
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    k = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char*)malloc(k > 0 ? (size_t)k : 1);
    if (b && k > 0 && fread(b, 1, (size_t)k, f) != (size_t)k) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (size_t)k;
    return b;
}

/* A PPM's pixels (RGB) and its comment lines, joined by '\n', or 0. */
static int t_read_ppm(const char* path, int* w, int* h, unsigned char* rgb, size_t cap, char* note, size_t ncap) {
    size_t n, i = 3;
    unsigned char* b = t_slurp(path, &n);
    int ok = 0;
    if (note) note[0] = '\0';
    if (!b || n < 3 || memcmp(b, "P6\n", 3) != 0) { free(b); return 0; }
    while (i < n && b[i] == '#') {
        size_t j = i;
        while (j < n && b[j] != '\n') j++;
        if (note) {
            size_t at = strlen(note), add = j - i - 2 + (at ? 1 : 0);
            if (at + add < ncap) {
                if (at) note[at++] = '\n';
                memcpy(note + at, b + i + 2, j - i - 2);
                note[at + j - i - 2] = '\0';
            }
        }
        i = j + 1;
    }
    if (sscanf((const char*)b + i, "%d %d", w, h) == 2) {
        int lines = 0;
        while (i < n && lines < 2) if (b[i++] == '\n') lines++;
        if ((size_t)(*w) * (size_t)(*h) * 3 == n - i && n - i <= cap) { memcpy(rgb, b + i, n - i); ok = 1; }
    }
    free(b);
    return ok;
}

typedef struct cap_seen {
    int     n, bad_order, rec_mismatch, with_record, stride_bad;
    int64_t idx[64];
    int     byte0[64];
    int64_t last;
} cap_seen;
static void cap_on_frame(void* ctx, const ygfx_capture_frame* fr) {
    cap_seen* s = (cap_seen*)ctx;
    if (s->n > 0 && fr->index <= s->last) s->bad_order++;
    s->last = fr->index;
    if (fr->has_record) { s->with_record++; if (fr->record.index != fr->index) s->rec_mismatch++; }
    if (fr->stride != (ptrdiff_t)fr->w * 4 && fr->stride != -(ptrdiff_t)fr->w * 4) s->stride_bad++;
    if (s->n < 64) { s->idx[s->n] = fr->index; s->byte0[s->n] = fr->rgba[0]; }
    s->n++;
}

static void test_v12_capture_cpu(void) {
    static unsigned char px[1500 * 20 * 4], back[1500 * 20 * 3];
    static yscr_screen scr;
    static ygfx_gfx g;
    static unsigned char mem[64 * 128];
    char dir[64], path[160], note[512];
    yrt_ring ring;
    yrt_ring_desc rd;
    yrt_event ev[128];
    yscr_desc d;
    ygfx_desc gd;
    yscr_frame f;
    ygfx_capture_desc cd;
    ygfx_capture_stats st;
    cap_seen seen;
    int i, k, w, h, bad, drops, flip;
    g_where = "cpu v0.12 capture";
    snprintf(dir, sizeof dir, "ygfx_test_cap_%u", (unsigned)(yrt_now_ns() & 0xFFFFFF));

    /* the writer: top first and bottom first, a row wider than the
     * conversion chunk (1024 px), a comment of two lines */
    for (i = 0; i < 1500 * 20 * 4; i++) px[i] = (unsigned char)((i * 7 + (i >> 9)) & 0xFF);
    CHECK(ygfx_image_write(NULL, px, 4, 4, 0, NULL, NULL, 0) == YGFX_ERR_ARG);
    CHECK(ygfx_image_write("x.ppm", px, 0, 4, 0, NULL, note, sizeof note) == YGFX_ERR_ARG);
    CHECK(strstr(note, "image_write: a path") != NULL);
    CHECK(ygfx_image_write("no_such_dir_ygfx/x.ppm", px, 4, 4, 0, NULL, note, sizeof note) == YGFX_ERR_IO);
    CHECK(strstr(note, "cannot create 'no_such_dir_ygfx/x.ppm'") != NULL);
    for (flip = 0; flip < 2; flip++) {
        const int W0 = 1500, H0 = 20;
        const unsigned char* top = flip ? px + (size_t)(H0 - 1) * W0 * 4 : px;
        ptrdiff_t stride = flip ? -(ptrdiff_t)W0 * 4 : 0;
        int ok;
        snprintf(path, sizeof path, "ygfx_test_img_%d.ppm", flip);
        CHECK(ygfx_image_write(path, top, W0, H0, stride, "line one\nline two", NULL, 0) == YGFX_OK);
        w = h = 0;
        ok = t_read_ppm(path, &w, &h, back, sizeof back, note, sizeof note);
        CHECK(ok && w == W0 && h == H0);
        CHECK(strcmp(note, "line one\nline two") == 0);
        for (bad = 0, k = 0; ok && k < W0 * H0; k++) {
            int y = k / W0, x = k % W0, sy = flip ? H0 - 1 - y : y;
            const unsigned char* s = px + ((size_t)sy * W0 + (size_t)x) * 4;
            bad += back[3 * k] != s[0] || back[3 * k + 1] != s[1] || back[3 * k + 2] != s[2];
        }
        CHECK(bad == 0);
        remove(path);
    }

    /* the core on SIM, through the null backend's buffers */
    memset(&ring, 0, sizeof ring);
    memset(&rd, 0, sizeof rd);
    rd.memory = mem;
    rd.bytes = sizeof mem;
    CHECK(yrt_ring_open(&ring, &rd));
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 1000000;
    CHECK(yscr_open(&scr, &d));
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.ring = &ring;
    CHECK(ygfx_open(&g, &gd));
    memset(&cd, 0, sizeof cd);
    CHECK(ygfx_capture_mark(&g) == YGFX_ERR_ORDER);   /* not started */
    CHECK(strstr(ygfx_error(&g), "capture_mark: no capture is on") != NULL);
    CHECK(ygfx_capture_pump(&g, 0) == YGFX_ERR_ORDER);
    CHECK(strstr(ygfx_error(&g), "capture_pump: no capture is on") != NULL);
    CHECK(ygfx_capture_stop(&g, &st) == YGFX_ERR_ORDER);
    CHECK(strstr(ygfx_error(&g), "capture_stop: no capture is on") != NULL);
    CHECK(ygfx_capture_start(&g, NULL) == YGFX_ERR_ARG);   /* no specific message: the call's */
    CHECK(strstr(ygfx_error(&g), "ysp_gfx: capture_start: bad argument") != NULL);
    cd.every = -1;
    CHECK(ygfx_capture_start(&g, &cd) == YGFX_ERR_ARG);
    CHECK(strstr(ygfx_error(&g), "capture: every >= 0") != NULL);
    {
        float sw, sh;
        ygfx_size(&g, &sw, &sh);
        CHECK(sw >= 64 && sh >= 32);
        cd.every = 0; cd.x = (int)sw - 4; cd.w = 8; cd.h = 8;   /* past the right edge */
    }
    CHECK(ygfx_capture_start(&g, &cd) == YGFX_ERR_ARG);
    cd.x = 0; cd.slots = YGFX_CAPTURE_MAX_SLOTS + 1;
    CHECK(ygfx_capture_start(&g, &cd) == YGFX_ERR_ARG);

    /* manual: every 3rd frame and a mark, 4 slots, files with their records */
    ygfx__test_cap_fill = 1;
    memset(&seen, 0, sizeof seen);
    memset(&cd, 0, sizeof cd);
    snprintf(path, sizeof path, "%s/sub/f", dir);
    cd.every = 3; cd.slots = 4; cd.manual = true; cd.path = path;
    cd.x = 8; cd.y = 4; cd.w = 16; cd.h = 8;
    cd.on_frame = cap_on_frame; cd.ctx = &seen;
    CHECK(ygfx_capture_start(&g, &cd) == YGFX_OK);
    CHECK(ygfx_capture_start(&g, &cd) == YGFX_ERR_ORDER);
    /* frames 0..13: chosen 0, 3, 6, 9 (4 slots full), mark 10, 12: dropped */
    for (i = 0; i < 14; i++) {
        CHECK(yscr_begin(&scr, &f) == YSCR_OK);
        CHECK(f.index == i);
        CHECK(ygfx_begin(&g, &f) == YGFX_OK);
        if (i == 10) CHECK(ygfx_capture_mark(&g) == YGFX_OK);
        CHECK(ygfx_capture_pump(&g, 0) == YGFX_ERR_ORDER);   /* inside a frame */
        CHECK(ygfx_end(&g) == YGFX_OK);
        CHECK(yscr_flip(&scr) == YSCR_OK);
    }
    CHECK(ygfx_capture_mark(&g) == YGFX_ERR_ORDER);   /* outside a frame */
    ygfx_capture_get_stats(&g, &st);
    CHECK(st.chosen == 6 && st.read == 4 && st.dropped == 2 && st.written == 0 && st.busy == 4 && st.busy_max == 4);
    CHECK(st.manual == 1 && st.slots == 4);
    CHECK(seen.n == 0);   /* manual: nothing is written in a frame */
    CHECK(ygfx_capture_pump(&g, 1) == 1);   /* the oldest only */
    CHECK(seen.n == 1 && seen.idx[0] == 0);
    CHECK(ygfx_capture_pump(&g, 0) == 3);
    CHECK(seen.n == 4 && seen.bad_order == 0 && seen.rec_mismatch == 0 && seen.with_record == 4);
    for (k = 0; k < 4 && k < seen.n; k++) CHECK(seen.idx[k] == 3 * k && seen.byte0[k] == k + 1);   /* read k + 1's fill */
    CHECK(seen.stride_bad == 0);
    /* frame 15: a mark; its record comes at the next begin */
    CHECK(yscr_begin(&scr, &f) == YSCR_OK);
    CHECK(ygfx_begin(&g, &f) == YGFX_OK);
    CHECK(ygfx_capture_mark(&g) == YGFX_OK);   /* index 14: also not every 3rd */
    CHECK(ygfx_end(&g) == YGFX_OK);
    CHECK(yscr_flip(&scr) == YSCR_OK);
    CHECK(ygfx_capture_pump(&g, 0) == 0);   /* no record yet */
    {   /* another frame's record does not attach to it */
        yscr_frame hf;
        yscr_record other;
        memset(&hf, 0, sizeof hf);
        memset(&other, 0, sizeof other);
        other.index = 9999;
        hf.index = 98; hf.done = &other; hf.n_done = 1;   /* not a multiple of 3 */
        CHECK(ygfx_begin(&g, &hf) == YGFX_OK);
        CHECK(ygfx_end(&g) == YGFX_OK);
        CHECK(ygfx_capture_pump(&g, 0) == 0);
    }
    {   /* a frame filled by hand (done NULL) gets no record: written at once */
        yscr_frame hf;
        memset(&hf, 0, sizeof hf);
        hf.index = 100;
        CHECK(ygfx_begin(&g, &hf) == YGFX_OK);
        CHECK(ygfx_capture_mark(&g) == YGFX_OK);
        CHECK(ygfx_end(&g) == YGFX_OK);
        CHECK(ygfx_capture_pump(&g, 0) == 1);
        CHECK(seen.n == 5 && seen.idx[4] == 100);
    }
    CHECK(ygfx_capture_stop(&g, &st) == 6);   /* frame 14 written without its record */
    CHECK(st.written == 6 && st.no_record == 2 && st.dropped == 2 && st.failed == 0 && st.busy == 0);
    CHECK(seen.n == 6 && seen.idx[5] == 14);
    ygfx_capture_get_stats(&g, &st);
    CHECK(st.slots == 0);
    {   /* the files: the fill, and the record in the comment */
        static const int idx[6] = { 0, 3, 6, 9, 14, 100 };
        for (k = 0; k < 6; k++) {
            snprintf(note, sizeof note, "%s%08d.ppm", path, idx[k]);
            w = h = 0;
            CHECK(t_read_ppm(note, &w, &h, back, sizeof back, note + 200, 300) && w == 16 && h == 8 && back[0] == k + 1);
            CHECK(strstr(note + 200, k < 4 ? "record: onset" : "record: none") != NULL);
            snprintf(note, sizeof note, "%s%08d.ppm", path, idx[k]);
            remove(note);
        }
    }
    snprintf(note, sizeof note, "%s/sub", dir);
    TEST_RMDIR(note);
    TEST_RMDIR(dir);
    drops = 0;
    k = yrt_ring_drain(&ring, ev, 128);
    for (i = 0; i < k; i++)
        if (ev[i].source == YRT_SRC_GFX && ev[i].kind == YGFX_EV_CAPTURE_DROP) {
            drops++;
            CHECK(ev[i].u.i64[0] == (drops == 1 ? 10 : 12) && ev[i].aux == (uint32_t)drops);
        }
    CHECK(drops == 2);

#if !defined(YRT_NO_THREADS) && (!defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__))
    /* the writer thread: every frame, 3 slots, each copy done after 2 polls;
     * no files. Every chosen frame is written or counted as dropped. */
    ygfx__test_cap_delay = 2;
    memset(&seen, 0, sizeof seen);
    memset(&cd, 0, sizeof cd);
    cd.every = 1; cd.slots = 3;
    cd.on_frame = cap_on_frame; cd.ctx = &seen;
    CHECK(ygfx_capture_start(&g, &cd) == YGFX_OK);
    for (i = 0; i < 40; i++) {
        CHECK(yscr_begin(&scr, &f) == YSCR_OK);
        CHECK(ygfx_begin(&g, &f) == YGFX_OK);
        CHECK(ygfx_end(&g) == YGFX_OK);
        CHECK(yscr_flip(&scr) == YSCR_OK);
    }
    k = ygfx_capture_stop(&g, &st);
    CHECK(k == (int)st.written);
    CHECK(st.manual == 0 && st.chosen == 40 && st.written + st.dropped == 40 && st.failed == 0);
    CHECK(st.written >= 10 && st.dropped >= 1 && st.busy_max == 3);
    CHECK(seen.n == (int)st.written && seen.bad_order == 0 && seen.rec_mismatch == 0);
    CHECK(st.no_record <= 3 && seen.with_record == seen.n - (int)st.no_record);
    printf("v0.12 capture on SIM, every frame, 3 slots, copies done after 2 polls: %llu written, %llu dropped, %llu without a record; "
           "the capture's state %u bytes\n",
           (unsigned long long)st.written, (unsigned long long)st.dropped, (unsigned long long)st.no_record, (unsigned)sizeof(ygfx__cap));
#endif
    ygfx__test_cap_delay = 0;
    ygfx__test_cap_fill = 0;
    ygfx_close(&g);
    yscr_close(&scr);

    /* a version 3 backend has no capture calls */
    {
        static ygfx_backend v3;
        static ygfx__null v3ctx;
        v3 = ygfx__null_backend;
        v3.version = 3;
        memset(&d, 0, sizeof d);
        d.backend = YSCR_BACKEND_SIM;
        d.sim_period_ns = 1000000;
        CHECK(yscr_open(&scr, &d));
        memset(&gd, 0, sizeof gd);
        gd.screen = &scr;
        gd.backend = &v3;
        gd.backend_ctx = &v3ctx;
        CHECK(ygfx_open(&g, &gd));
        memset(&cd, 0, sizeof cd);
        cd.every = 1;
        CHECK(ygfx_capture_start(&g, &cd) == YGFX_ERR_NOT_IMPLEMENTED);
        ygfx_close(&g);
        v3.version = 2;
        CHECK(!ygfx_open(&g, &gd));
        yscr_close(&scr);
    }
}

/* ---------------------------------------------------------------- GL tests */

#define W 320
#define H 200

typedef struct rig {
    ygfx_headless hl;
    yscr_screen   scr;
    ygfx_gfx      g;
    yscr_frame    f;
    float           scene[W * H * 4];
    uint8_t         out[W * H * 4];
} rig;

static rig* R;

/* What every renderer measured, for docs/gfx.md. */
typedef struct stats {
    double shape_soft, grating, gabor, square_bad, dots, image, user;
    long   hard_bad, noise_bad, out_bad, lut_bad, dither_bad, hit_bad, ties;
    double anchor_diff, overlap, batch_diff, dither_mean;
} stats;

/* What v0.2's checks measured, per renderer. */
typedef struct stats2 {
    double stroke_soft, mask_gpu, sprite_lin, tint, group_diff, target_diff, add_diff;
    long   stroke_hard_bad, stroke_hit_bad, miter_bad, sprite_bad, bleed, group_hit_bad, target_bad;
    long   refused_ok, refused_n, epoch_bad, epoch_control_bad, persist_bad, clut_bad, lost_ok, lost_bad;
    long   edt_bad, alpha_scene_bad, alpha_control, passes_bad;
    double mask_sdf_cpu;   /* the distance texture against the analytic disc, texels */
} stats2;

static stats2 S2;

/* desc.cache for every GL open but the cache part's own (v0.4) */
static const ygfx_cache* test_cache(void);

static int gl_open(int bg_gray, ygfx_format fmt, ygfx_dither dither) {
    ygfx_desc gd;
    memset(&gd, 0, sizeof gd);
    gd.screen = &R->scr;
    gd.background[0] = gd.background[1] = gd.background[2] = bg_gray ? 0.5f : 0.0f;
    ygfx__test_scene32 = fmt == YGFX_RGBA32F;
    gd.dither = dither;
    gd.seed = 99;
    gd.cache = test_cache();
    if (!ygfx_open(&R->g, &gd)) {
        fprintf(stderr, "gfx_test [%s]: open: %s\n", g_where, ygfx_error(&R->g));
        g_failures++;
        return 0;
    }
    return 1;
}

/* One frame: draw, read the scene and the output back, flip. */
static void gl_frame(const ygfx_stim* s, int n) {
    int rc;
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
    rc = ygfx_draw_n(&R->g, s, n);
    CHECK(rc == YGFX_OK);
    CHECK(ygfx_end(&R->g) == YGFX_OK);
    CHECK(ygfx_read_scene(&R->g, 0, 0, W, H, R->scene) == YGFX_OK);
    CHECK(ygfx_read_output(&R->g, 0, 0, W, H, R->out) == YGFX_OK);
    CHECK(yscr_flip(&R->scr) == YSCR_OK);
}

static double cov(int edge, double w, double d) {
    if (edge == YGFX_EDGE_HARD || w <= 0) return d <= 0 ? 1.0 : 0.0;
    if (edge == YGFX_EDGE_COSINE) {
        if (d <= -0.5 * w) return 1.0;
        if (d >= 0.5 * w) return 0.0;
        return 0.5 + 0.5 * cos(3.14159265358979323846 * (d + 0.5 * w) / w);
    }
    return 0.5 * erfc(d / (w * sqrt(2.0)));
}

/* The signed distance of screen pixel (i, j)'s center to the stimulus's
 * aperture, through the transform's inverse; inside the quad or not. */
static double pix_sdf(const ygfx_stim* s, int i, int j, int* in_quad, double* lxo, double* lyo) {
    float lx, ly, hx, hy, sp[3];
    int shape;
    ygfx_local(&R->g, s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
    shape = ygfx__geom(&R->g, s, &hx, &hy, sp);
    if (lxo) *lxo = lx;
    if (lyo) *lyo = ly;
    if (in_quad) *in_quad = fabsf(lx) <= hx && fabsf(ly) <= hy;
    if (shape == YGFX_NO_APERTURE || s->kind == YGFX_GABOR) return -1e30;
    {
        float ml;
        ygfx__miter(s, shape, &ml);
        return ygfx__sdf(shape, lx, ly, hx, hy, sp, s->p, R->g.ppu * ygfx__scale(s), ml, 0.0);
    }
}

static void gl_shapes(stats* st) {
    static const float tri[6] = { -40, 30, 40, 30, 0, -35 };
    int shape, e, o, a;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (shape = YGFX_RECT; shape <= YGFX_CROSS; shape++)
        for (e = 0; e < 3; e++)
            for (o = 0; o < 2; o++)
                for (a = 0; a < 2; a++) {
                    ygfx_shape_desc sd;
                    ygfx_stim s;
                    int i, j;
                    memset(&sd, 0, sizeof sd);
                    sd.shape = (ygfx_shape_kind)shape;
                    sd.edge = (ygfx_edge)e;
                    sd.edge_width = e == YGFX_EDGE_COSINE ? 6.0f : 1.5f;
                    sd.w = shape == YGFX_LINE ? 100.0f : 80.0f;
                    sd.h = shape == YGFX_RECT ? 50.0f : (shape == YGFX_CIRCLE || shape == YGFX_ANNULUS ? 80.0f : 70.0f);
                    sd.ori = o ? 30.0f : 0.0f;
                    sd.x = a ? -50.0f : 3.25f;
                    sd.y = a ? -40.0f : -2.5f;
                    sd.anchor = a ? YGFX_TOP_LEFT : YGFX_CENTER;
                    sd.color[0] = sd.color[1] = sd.color[2] = 1.0f;
                    if (shape == YGFX_RECT) sd.shape_p[1] = 8;
                    if (shape == YGFX_ANNULUS) sd.shape_p[0] = 20;
                    if (shape == YGFX_LINE) sd.shape_p[0] = 6;
                    if (shape == YGFX_CROSS) sd.shape_p[0] = 10;
                    if (shape == YGFX_POLYGON) { sd.shape_p[0] = 3; sd.vertices = tri; }
                    s = ygfx_shape(&sd);
                    gl_frame(&s, 1);
                    for (j = 0; j < H; j++)
                        for (i = 0; i < W; i++) {
                            double d = pix_sdf(&s, i, j, NULL, NULL, NULL);
                            double want = cov(e, sd.edge_width, d), got = R->scene[(j * W + i) * 4];
                            int hit = ygfx_hit(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f);
                            if (fabs(d) < 1e-3) { st->ties++; continue; }
                            if (e == YGFX_EDGE_HARD) st->hard_bad += fabs(got - want) > 1e-6;
                            else st->shape_soft = maxd(st->shape_soft, fabs(got - want));
                            st->hit_bad += hit != (got >= 0.5);
                        }
                }
    ygfx_close(&R->g);
}

static void gl_gratings(stats* st) {
    static const float sfs[3] = { 0.05f, 0.13f, 0.4f }, oris[3] = { 0, 37, 90 }, phases[3] = { 0, 0.25f, 0.7f };
    int k, sq;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (sq = 0; sq < 2; sq++)
        for (k = 0; k < 9; k++) {
            ygfx_grating_desc gd;
            ygfx_stim s;
            int i, j;
            memset(&gd, 0, sizeof gd);
            gd.w = gd.h = 150;
            gd.sf = sfs[k % 3]; gd.ori = oris[k / 3]; gd.phase = phases[(k + 1) % 3];
            gd.aperture = YGFX_CIRCLE; gd.edge = YGFX_EDGE_COSINE; gd.edge_width = 8;
            gd.square = sq;
            gd.x = 5;
            s = ygfx_grating(&gd);
            gl_frame(&s, 1);
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    double lx, ly, d = pix_sdf(&s, i, j, NULL, &lx, &ly), x = s.sf * lx + s.phase;
                    double c = cos(2.0 * 3.14159265358979323846 * x), g;
                    double want, got = R->scene[(j * W + i) * 4];
                    (void)ly;
                    if (sq) {
                        double fr = x - floor(x);
                        if (fabs(c) < 1e-3) continue;   /* a transition at the pixel center */
                        g = (fr < 0.25 || fr >= 0.75) ? 1.0 : -1.0;
                    } else {
                        g = c;
                    }
                    want = 0.5 + 0.5 * g * cov(YGFX_EDGE_COSINE, 8, d);
                    if (sq) st->square_bad = maxd(st->square_bad, fabs(got - want));
                    else st->grating = maxd(st->grating, fabs(got - want));
                }
        }
    ygfx_close(&R->g);
}

static double gabor_ref(const ygfx_stim* s, int i, int j, int* in) {
    double lx, ly;
    pix_sdf(s, i, j, in, &lx, &ly);
    return cos(2.0 * 3.14159265358979323846 * (s->sf * lx + s->phase)) *
           exp(-(lx * lx + s->aspect * s->aspect * ly * ly) / (2.0 * s->sigma * s->sigma));
}

static void gl_gabors(stats* st) {
    static float a1[W * H * 4];
    ygfx_gabor_desc gd;
    ygfx_stim s, t, two[2];
    int i, j, k;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (k = 0; k < 4; k++) {
        memset(&gd, 0, sizeof gd);
        gd.sigma = 18; gd.sf = 0.08f; gd.phase = 0.3f;
        gd.ori = k & 1 ? 45.0f : 0.0f;
        gd.aspect = k & 2 ? 1.5f : 0.0f;
        gd.x = -20.5f; gd.y = 7;
        s = ygfx_gabor(&gd);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                int in;
                double g = gabor_ref(&s, i, j, &in);
                double want = in ? 0.5 + 0.5 * g : 0.5;
                st->gabor = maxd(st->gabor, fabs(R->scene[(j * W + i) * 4] - want));
            }
    }
    /* The anchor moves the box, not the pattern: the same box placed by its
     * center and by its top-left corner gives the same pixels. */
    memset(&gd, 0, sizeof gd);
    gd.sigma = 15; gd.sf = 0.1f; gd.phase = 0.15f;
    gd.x = 11; gd.y = -6;
    s = ygfx_gabor(&gd);
    gl_frame(&s, 1);
    memcpy(a1, R->scene, sizeof a1);
    gd.anchor = YGFX_TOP_LEFT;
    gd.x = 11 - 60; gd.y = -6 - 60;            /* the 8-sigma box is 120 x 120 */
    t = ygfx_gabor(&gd);
    gl_frame(&t, 1);
    for (i = 0; i < W * H * 4; i++) st->anchor_diff = maxd(st->anchor_diff, fabs(a1[i] - R->scene[i]));
    /* overlapping modulations add */
    two[0] = s;
    two[1] = s;
    two[1].x += 25; two[1].ori = 60; two[1].sf = 0.05f;
    gl_frame(&two[1], 1);
    for (i = 0; i < W * H * 4; i += 4) a1[i] += R->scene[i] - 0.5f;   /* a1 still holds s alone */
    gl_frame(two, 2);
    for (i = 0; i < W * H * 4; i += 4) st->overlap = maxd(st->overlap, fabs(a1[i] - R->scene[i]));
    ygfx_close(&R->g);
}

static void gl_dots(stats* st) {
    static const float pos[12] = { -60, -40, 0, 0, 50, 30, 70, -50, -30, 45, 200, 0 };
    ygfx_dots_desc dd;
    ygfx_stim s;
    ygfx_buf b;
    int i, j, k;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    b = ygfx_buffer(&R->g, sizeof pos);
    CHECK(b.id != 0);
    CHECK(ygfx_buffer_update(&R->g, b, 0, pos, sizeof pos) == YGFX_OK);
    memset(&dd, 0, sizeof dd);
    dd.buf = b; dd.count = 6; dd.dot_size = 10; dd.edge = YGFX_EDGE_COSINE; dd.edge_width = 2;
    dd.color[0] = dd.color[1] = dd.color[2] = 1;
    dd.aperture = YGFX_CIRCLE; dd.w = 200;    /* the last dot is outside */
    dd.ori = 20; dd.x = 8; dd.y = 3;
    s = ygfx_dots(&dd);
    gl_frame(&s, 1);
    for (j = 0; j < H; j++)
        for (i = 0; i < W; i++) {
            float lx, ly;
            double want = 0;
            ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
            for (k = 0; k < 5; k++) {
                double dx = lx - pos[2 * k], dy = ly - pos[2 * k + 1];
                want = maxd(want, cov(YGFX_EDGE_COSINE, 2, sqrt(dx * dx + dy * dy) - 5));
            }
            st->dots = maxd(st->dots, fabs(R->scene[(j * W + i) * 4] - want));
        }
    {   /* ring dots: a stroke on each dot, in a field with no aperture */
        int e, al;
        for (e = 0; e < 2; e++)
            for (al = 0; al < 3; al++) {
                float a1, a2;
                dd.aperture = YGFX_NO_APERTURE;
                dd.edge = e ? YGFX_EDGE_COSINE : YGFX_EDGE_HARD;
                dd.stroke = 3; dd.stroke_align = (ygfx_stroke_align)al;
                s = ygfx_dots(&dd);
                ygfx__band(&s, &a1, &a2);
                gl_frame(&s, 1);
                for (j = 0; j < H; j++)
                    for (i = 0; i < W; i++) {
                        float lx, ly;
                        double want = 0, got = R->scene[(j * W + i) * 4];
                        int tie = 0;
                        ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                        for (k = 0; k < 6; k++) {
                            double dx = lx - pos[2 * k], dy = ly - pos[2 * k + 1], d = sqrt(dx * dx + dy * dy) - 5;
                            want = maxd(want, cov(dd.edge, 2, d - a2) - cov(dd.edge, 2, d - a1));
                            tie |= fabs(d - a2) < 1e-3 || fabs(d - a1) < 1e-3;
                        }
                        if (e) S2.stroke_soft = maxd(S2.stroke_soft, fabs(got - want));
                        else if (!tie) S2.stroke_hard_bad += fabs(got - want) > 1e-6;
                    }
            }
    }
    ygfx_close(&R->g);
}

static void gl_images(stats* st) {
    static const ygfx_format fmts[] = { YGFX_R8, YGFX_RG8, YGFX_RGBA8, YGFX_R16F, YGFX_RGBA16F,
                                          YGFX_R32F, YGFX_RGBA32F, YGFX_R16UI };
    unsigned char u8[8 * 4 * 4];
    uint16_t u16[8 * 4];
    float f32[8 * 4 * 4];
    int fi, k;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (fi = 0; fi < (int)(sizeof fmts / sizeof fmts[0]); fi++) {
        ygfx_format fm = fmts[fi];
        int ch = fm == YGFX_RG8 ? 2 : ((fm == YGFX_RGBA8 || fm == YGFX_RGBA16F || fm == YGFX_RGBA32F) ? 4 : 1);
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        ygfx_stim s;
        ygfx_tex t;
        const void* data;
        int x, y;
        for (k = 0; k < 8 * 4 * 4; k++) {
            u8[k] = (unsigned char)((k * 37 + 11) & 255);
            f32[k] = (float)((k * 13) % 64) / 64.0f;   /* exact in half */
        }
        for (k = 0; k < 8 * 4; k++) u16[k] = (uint16_t)(k * 2039 + 7);
        if (ch == 2) for (k = 0; k < 8 * 4; k++) u8[2 * k + 1] = 255;
        if (ch == 4) for (k = 0; k < 8 * 4; k++) { u8[4 * k + 3] = 255; f32[4 * k + 3] = 1.0f; }
        data = fm == YGFX_R16UI ? (const void*)u16 : ((fm == YGFX_R8 || fm == YGFX_RG8 || fm == YGFX_RGBA8) ? (const void*)u8 : (const void*)f32);
        memset(&td, 0, sizeof td);
        td.w = 8; td.h = 4; td.format = fm; td.data = data;
        t = ygfx_texture(&R->g, &td);
        CHECK(t.id != 0);
        memset(&idd, 0, sizeof idd);
        idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 10; idd.y = 20;
        s = ygfx_image(&R->g, &idd);
        gl_frame(&s, 1);
        for (y = 0; y < 4; y++)
            for (x = 0; x < 8; x++) {
                const float* px = &R->scene[((20 + y) * W + 10 + x) * 4];
                int c;
                for (c = 0; c < 3; c++) {
                    double want;
                    int src = ch == 4 ? c : 0;
                    if (fm == YGFX_R16UI) want = (float)u16[y * 8 + x] * (1.0f / 65535.0f);
                    else if (data == u8) want = u8[(y * 8 + x) * ch + src] / 255.0;
                    else want = f32[(y * 8 + x) * ch + src];
                    st->image = maxd(st->image, fabs(px[c] - want));
                }
            }
        ygfx_texture_free(&R->g, t);
    }
    ygfx_close(&R->g);
}

static void gl_noise(stats* st) {
    ygfx_noise_desc nd;
    ygfx_stim s;
    int i, j, dist;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (dist = 0; dist < 3; dist++) {   /* UNIFORM, BINARY, GAUSSIAN */
        memset(&nd, 0, sizeof nd);
        nd.w = 64; nd.h = 48; nd.check = 2; nd.seed = 1234 + dist;
        nd.dist = (ygfx_noise_dist)dist;
        nd.aperture = YGFX_NO_APERTURE;
        nd.dir[0] = nd.dir[1] = nd.dir[2] = 1.0f;
        nd.ori = dist ? 90.0f : 0.0f;
        nd.x = -30; nd.y = 10;
        s = ygfx_noise(&nd);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                float lx, ly;
                int cx, cy;
                ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                if (fabsf(lx) >= 32 || fabsf(ly) >= 24) continue;
                cx = (int)floor((lx + 32.0) / 2.0);
                cy = (int)floor((ly + 24.0) / 2.0);
                st->noise_bad += R->scene[(j * W + i) * 4] != ygfx_noise_value(cx, cy, nd.seed, nd.dist);
            }
    }
    ygfx_close(&R->g);
}

/* An image of known values into the scene, then the output stage. */
static ygfx_stim ramp_image(ygfx_tex* t, const float* v, int n) {
    ygfx_texture_desc td;
    ygfx_image_desc idd;
    memset(&td, 0, sizeof td);
    td.w = n; td.h = 1; td.format = YGFX_R32F; td.data = v;
    *t = ygfx_texture(&R->g, &td);
    memset(&idd, 0, sizeof idd);
    idd.tex = *t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT;
    idd.h = 16;   /* each texel 1 px wide, 16 px tall */
    return ygfx_image(&R->g, &idd);
}

static void gl_output(stats* st) {
    static float v[256];
    static const float lut[3 * 5] = { 0, 0.1f, 0.35f, 0.7f, 1, 0, 0.5f, 0.7f, 0.86f, 1, 0, 0.25f, 0.5f, 0.75f, 1 };
    ygfx_tex t;
    ygfx_stim s;
    int k, c, i, j;
    for (k = 0; k < 256; k++) v[k] = (float)k / 255.0f;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    s = ramp_image(&t, v, 256);
    gl_frame(&s, 1);
    for (k = 0; k < 256; k++) for (c = 0; c < 3; c++) st->out_bad += R->out[(5 * W + k) * 4 + c] != k;
    ygfx_texture_free(&R->g, t);
    /* rounds to nearest: 0.005 of a code above each halfway point goes up */
    for (k = 0; k < 255; k++) v[k] = ((float)k + 0.505f) / 255.0f;
    v[255] = 1.0f;
    s = ramp_image(&t, v, 256);
    gl_frame(&s, 1);
    for (k = 0; k < 255; k++) for (c = 0; c < 3; c++) st->out_bad += R->out[(5 * W + k) * 4 + c] != k + 1;
    ygfx_texture_free(&R->g, t);
    for (k = 0; k < 256; k++) v[k] = (float)k / 255.0f;
    s = ramp_image(&t, v, 256);
    /* a CLUT */
    CHECK(ygfx_set_lut(&R->g, lut, 5) == YGFX_OK);
    gl_frame(&s, 1);
    for (k = 0; k < 256; k++)
        for (c = 0; c < 3; c++) {
            double x = v[k] * 4.0, d, code;
            int i0 = (int)x < 3 ? (int)x : 3;
            d = lut[c * 5 + i0] + (lut[c * 5 + i0 + 1] - lut[c * 5 + i0]) * (x - i0);
            code = d * 255.0 + 0.5;
            if (fabs(code - floor(code) - 0.5) > 0.5 - 1e-3) { st->ties++; continue; }
            st->lut_bad += R->out[(5 * W + k) * 4 + c] != (int)floor(code);
        }
    ygfx_close(&R->g);
    /* ordered dither: Bayer by bit interleaving, on the scene's grid */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_ORDERED)) return;
    /* every threshold of the 8 x 8 matrix: 64 fractional levels across */
    for (k = 0; k < 256; k++) v[k] = (100.0f + ((float)(k % 64) + 0.25f) / 64.0f) / 255.0f;
    s = ramp_image(&t, v, 256);
    s.h = 64;
    gl_frame(&s, 1);
    for (j = 0; j < 64; j++)
        for (i = 0; i < 256; i++) {
            int x = i & 7, y = (H - 1 - j) & 7, z = x ^ y;
            int b = ((z & 1) << 5) | ((x & 1) << 4) | ((z & 2) << 2) | ((x & 2) << 1) | ((z & 4) >> 1) | ((x & 4) >> 2);
            double code = (double)v[i] * 255.0 + (b + 0.5) / 64.0;
            if (fabs(code - floor(code)) < 1e-3) { st->ties++; continue; }
            st->dither_bad += R->out[(j * W + i) * 4] != (int)floor(code);
        }
    ygfx_close(&R->g);
    /* noise dither: unbiased, a new pattern each frame */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NOISE)) return;
    for (k = 0; k < 256; k++) v[k] = 100.3f / 255.0f;
    s = ramp_image(&t, v, 256);
    s.h = 64;
    {
        static uint8_t first[64 * 256];
        double sum = 0;
        long changed = 0;
        gl_frame(&s, 1);
        for (j = 0; j < 64; j++) for (i = 0; i < 256; i++) { first[j * 256 + i] = R->out[(j * W + i) * 4]; sum += first[j * 256 + i]; }
        gl_frame(&s, 1);
        for (j = 0; j < 64; j++) for (i = 0; i < 256; i++) { changed += R->out[(j * W + i) * 4] != first[j * 256 + i]; sum += R->out[(j * W + i) * 4]; }
        st->dither_mean = sum / (2.0 * 64 * 256) - 100.3;
        CHECK(changed > 1000);
    }
    ygfx_close(&R->g);
}

static void gl_user_batch_rows(stats* st) {
    static float a1[W * H * 4];
    ygfx_pipeline_desc pd;
    ygfx_pipe p;
    ygfx_user_desc ud;
    ygfx_grating_desc gd;
    ygfx_stim s[300], u, gr;
    ygfx_shape_desc sd;
    static const float tri3[6] = { -5, 4, 5, 4, 0, -5 };
    static const unsigned char texa[16] = { 10, 200, 30, 90, 250, 0, 60, 120, 5, 15, 25, 35, 45, 55, 65, 75 };
    static const unsigned char texb[16] = { 255, 0, 255, 0, 128, 128, 128, 128, 1, 2, 3, 4, 5, 6, 7, 8 };
    ygfx_tex ta, tb;
    ygfx_texture_desc tdd;
    uint32_t seed = 5;
    int i, j;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    /* a user shader that is a grating, against the grating */
    memset(&pd, 0, sizeof pd);
    pd.body = "float ysp_main(vec2 p) {\n    return cos(2.0 * ysp_PI * fract(ysp_param(0) * p.x + ysp_param(1)));\n}\n";
    pd.mode = YGFX_MODULATION;
    p = ygfx_pipeline(&R->g, &pd);
    CHECK(p.id != 0);
    memset(&ud, 0, sizeof ud);
    {
        static const float pp[2] = { 0.07f, 0.4f };
        ud.pipe = p; ud.w = ud.h = 120; ud.ori = 15; ud.p = pp; ud.n_p = 2;
    }
    u = ygfx_user(&ud);
    gl_frame(&u, 1);
    memcpy(a1, R->scene, sizeof a1);
    memset(&gd, 0, sizeof gd);
    gd.w = gd.h = 120; gd.ori = 15; gd.sf = 0.07f; gd.phase = 0.4f;
    gr = ygfx_grating(&gd);
    gl_frame(&gr, 1);
    for (i = 0; i < W * H * 4; i++) st->user = maxd(st->user, fabs(a1[i] - R->scene[i]));
    /* the compiler's log names the body's own line */
    pd.body = "float ysp_main(vec2 p) {\n    float a = 1.0;\n    return a + bogus;\n}\n";
    p = ygfx_pipeline(&R->g, &pd);
    CHECK(p.id == 0 && strstr(ygfx_error(&R->g), "0:3") != NULL);   /* ANGLE 0:3:, Mesa 0:3( */
    if (p.id != 0 || !strstr(ygfx_error(&R->g), "0:3")) fprintf(stderr, "  log: %s\n", ygfx_error(&R->g));
    /* batched and one draw each: the same bits */
    memset(&tdd, 0, sizeof tdd);
    tdd.w = 4; tdd.h = 4; tdd.format = YGFX_R8;
    tdd.data = texa;
    ta = ygfx_texture(&R->g, &tdd);
    tdd.data = texb;
    tb = ygfx_texture(&R->g, &tdd);
    for (i = 0; i < 300; i++) {
        seed = seed * 1664525u + 1013904223u;
        if ((seed & 0x7000) == 0x7000) {   /* an image, from one of two textures */
            ygfx_image_desc idd;
            memset(&idd, 0, sizeof idd);
            idd.tex = (seed & 0x8000) ? ta : tb;
            idd.w = idd.h = 12;
            idd.x = (float)((int)((seed >> 3) % W) - W / 2); idd.y = (float)((int)((seed >> 11) % H) - H / 2);
            s[i] = ygfx_image(&R->g, &idd);
        } else if (seed & 0x100) {
            memset(&sd, 0, sizeof sd);
            sd.shape = (ygfx_shape_kind)((seed >> 9) % 6);
            sd.shape_p[0] = 3;
            sd.w = (float)(10 + (seed >> 12) % 30); sd.h = (float)(10 + (seed >> 17) % 30);
            sd.x = (float)((int)((seed >> 3) % W) - W / 2); sd.y = (float)((int)((seed >> 11) % H) - H / 2);
            sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 3;
            sd.color[0] = (float)(seed % 7) / 7.0f; sd.color[1] = 0.3f; sd.color[2] = 0.6f; sd.opacity = 0.5f;
            sd.vertices = tri3;
            s[i] = ygfx_shape(&sd);
        } else {
            ygfx_gabor_desc gb;
            memset(&gb, 0, sizeof gb);
            gb.sigma = (float)(3 + seed % 6); gb.sf = 0.1f; gb.contrast = 0.1f;
            gb.x = (float)((int)((seed >> 3) % W) - W / 2); gb.y = (float)((int)((seed >> 11) % H) - H / 2);
            s[i] = ygfx_gabor(&gb);
        }
    }
    gl_frame(s, 300);
    memcpy(a1, R->scene, sizeof a1);
    R->g.max_batch = 1;
    gl_frame(s, 300);
    R->g.max_batch = YGFX__BATCH;
    for (i = 0; i < W * H * 4; i++) st->batch_diff = maxd(st->batch_diff, fabs(a1[i] - R->scene[i]));
    ygfx_close(&R->g);
    /* rows: a rect placed 3, 4 px in from the top-left corner is there in
     * the scene and in the output */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&sd, 0, sizeof sd);
    sd.place = YGFX_TOP_LEFT; sd.anchor = YGFX_TOP_LEFT; sd.x = 3; sd.y = 4; sd.w = 10; sd.h = 6;
    sd.color[0] = sd.color[1] = sd.color[2] = 1;
    s[0] = ygfx_shape(&sd);
    gl_frame(s, 1);
    for (j = 0; j < 14; j++)
        for (i = 0; i < 16; i++) {
            int in = i >= 3 && i < 13 && j >= 4 && j < 10;
            CHECK((R->scene[(j * W + i) * 4] == 1.0f) == in);
            CHECK((R->out[(j * W + i) * 4] == 255) == in);
        }
    ygfx_close(&R->g);
}


/* ---------------------------------------------------------------- v0.2 */


/* The band's coverage on the CPU: the fill's coverage at a2 minus at a1. */
static double band_ref(const ygfx_stim* s, int i, int j, int* tie) {
    float lx, ly, hx, hy, sp[3], a1, a2, ml;
    int shape;
    double k = R->g.ppu * ygfx__scale(s), d1, d2, w = s->edge == YGFX_EDGE_HARD ? 0.0 : s->edge_width;
    ygfx_local(&R->g, s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
    shape = ygfx__geom(&R->g, s, &hx, &hy, sp);
    ygfx__miter(s, shape, &ml);
    if (s->stroke <= 0.0f) { a1 = -1e30f; a2 = 0.0f; } else ygfx__band(s, &a1, &a2);
    d2 = ygfx__sdf(shape, lx, ly, hx, hy, sp, s->p, k, ml, a2);
    if (s->stroke <= 0.0f) { *tie = fabs(d2) < 1e-3; return cov(s->edge, w, d2); }
    d1 = ygfx__sdf(shape, lx, ly, hx, hy, sp, s->p, k, ml, a1);
    *tie = fabs(d2 - a2) < 1e-3 || fabs(d1 - a1) < 1e-3;
    return cov(s->edge, w, d2 - a2) - cov(s->edge, w, d1 - a1);
}

static void gl_strokes(stats* st) {
    static const float tri[6] = { -40, 30, 40, 30, 0, -35 };
    static const float sharp[6] = { -60, 20, 60, 20, 0, 5 };    /* a 22 degree apex at the bottom */
    static const float concave[8] = { -30, -30, 30, -30, 0, 0, -30, 30 };
    int shape, e, al, o, jn;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (shape = YGFX_RECT; shape <= YGFX_CROSS; shape++)
        for (e = 0; e < 3; e++)
            for (al = 0; al < 3; al++)
                for (o = 0; o < 2; o++)
                    for (jn = 0; jn < 2; jn++) {
                        ygfx_shape_desc sd;
                        ygfx_stim s;
                        int i, j, tie;
                        int miter_ok = shape == YGFX_RECT || shape == YGFX_CROSS || shape == YGFX_POLYGON;
                        if (jn && !miter_ok) continue;
                        memset(&sd, 0, sizeof sd);
                        sd.shape = (ygfx_shape_kind)shape;
                        sd.edge = (ygfx_edge)e;
                        sd.edge_width = e == YGFX_EDGE_COSINE ? 3.0f : 1.0f;
                        sd.w = shape == YGFX_LINE ? 100.0f : 80.0f;
                        sd.h = shape == YGFX_RECT ? 50.0f : (shape == YGFX_CIRCLE || shape == YGFX_ANNULUS ? 80.0f : 70.0f);
                        sd.ori = o ? 30.0f : 0.0f;
                        sd.x = 2.25f; sd.y = -1.5f;
                        sd.color[0] = sd.color[1] = sd.color[2] = 1.0f;
                        sd.stroke = 6.0f;
                        sd.stroke_align = (ygfx_stroke_align)al;
                        sd.join = jn ? YGFX_JOIN_MITER : YGFX_JOIN_ROUND;
                        if (shape == YGFX_RECT && !jn) sd.shape_p[1] = 8;
                        if (shape == YGFX_ANNULUS) sd.shape_p[0] = 20;
                        if (shape == YGFX_LINE) sd.shape_p[0] = 6;
                        if (shape == YGFX_CROSS) sd.shape_p[0] = 14;
                        if (shape == YGFX_POLYGON) { sd.shape_p[0] = 3; sd.vertices = tri; }
                        s = ygfx_shape(&sd);
                        gl_frame(&s, 1);
                        for (j = 0; j < H; j++)
                            for (i = 0; i < W; i++) {
                                double want = band_ref(&s, i, j, &tie), got = R->scene[(j * W + i) * 4];
                                int hit = ygfx_hit(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f);
                                if (tie) { st->ties++; continue; }
                                if (e == YGFX_EDGE_HARD) {
                                    S2.stroke_hard_bad += fabs(got - want) > 1e-6;
                                    S2.stroke_hit_bad += hit != (got >= 0.5);
                                } else {
                                    S2.stroke_soft = maxd(S2.stroke_soft, fabs(got - want));
                                }
                            }
                    }
    {   /* the alignments by hand, not through the header's helpers: a
         * circle of radius 40, a hard 6 px band, along +x from its center */
        static const double r[4] = { 35.5, 38.5, 41.5, 44.5 };
        static const int want[3][4] = { { 0, 1, 1, 0 },     /* CENTER: 37 to 43  */
                                        { 1, 1, 0, 0 },     /* INSIDE: 34 to 40  */
                                        { 0, 0, 1, 1 } };   /* OUTSIDE: 40 to 46 */
        ygfx_shape_desc sd;
        ygfx_stim s;
        int a, q;
        for (a = 0; a < 3; a++) {
            memset(&sd, 0, sizeof sd);
            sd.shape = YGFX_CIRCLE; sd.w = 80; sd.stroke = 6; sd.stroke_align = (ygfx_stroke_align)a;
            sd.color[0] = sd.color[1] = sd.color[2] = 1;
            s = ygfx_shape(&sd);
            gl_frame(&s, 1);
            for (q = 0; q < 4; q++) {
                int i = (int)(W / 2 + r[q] - 0.5);
                S2.miter_bad += (R->scene[(H / 2 * W + i) * 4] == 1.0f) != want[a][q];
            }
        }
    }
    {   /* a thin band (narrower than the edge) never reaches 1 */
        ygfx_shape_desc sd;
        ygfx_stim s;
        int i, j, tie;
        double peak = 0;
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_CIRCLE; sd.w = 120; sd.edge = YGFX_EDGE_GAUSSIAN; sd.edge_width = 2;
        sd.stroke = 1; sd.color[0] = sd.color[1] = sd.color[2] = 1;
        s = ygfx_shape(&sd);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                double got = R->scene[(j * W + i) * 4];
                S2.stroke_soft = maxd(S2.stroke_soft, fabs(got - band_ref(&s, i, j, &tie)));
                peak = maxd(peak, got);
            }
        CHECK(peak > 0.15 && peak < 0.25);   /* 2 erf(0.25 / sqrt 2) = 0.197 */
    }
    {   /* MITER: the corner pixel a round join leaves empty is covered; an
         * acute corner is beveled at the limit */
        ygfx_shape_desc sd;
        ygfx_stim s;
        float cx, cy;
        int px, py, tie, k;
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_RECT; sd.w = 60; sd.h = 40; sd.stroke = 8; sd.stroke_align = YGFX_STROKE_OUTSIDE;
        sd.color[0] = sd.color[1] = sd.color[2] = 1;
        sd.join = YGFX_JOIN_MITER;
        s = ygfx_shape(&sd);
        gl_frame(&s, 1);
        ygfx_resolve(&R->g, &s, &cx, &cy);
        px = (int)(cx + 30 + 6); py = (int)(cy + 20 + 6);   /* 6, 6 px out of the corner: Chebyshev 6, Euclidean 8.5 */
        S2.miter_bad += R->scene[(py * W + px) * 4] != 1.0f;
        sd.join = YGFX_JOIN_ROUND;
        s = ygfx_shape(&sd);
        gl_frame(&s, 1);
        S2.miter_bad += R->scene[(py * W + px) * 4] != 0.0f;
        /* a limit below sqrt 2 bevels a right angle: |dx| + |dy| <= 8 out
         * of the corner, so (6, 6) is empty; RECT and CROSS against the CPU */
        sd.join = YGFX_JOIN_MITER; sd.miter_limit = 1.2f;
        for (k = 0; k < 2; k++) {
            int i, j;
            sd.shape = k ? YGFX_CROSS : YGFX_RECT;
            sd.shape_p[0] = k ? 14.0f : 0.0f;
            sd.ori = k ? 20.0f : 0.0f;
            s = ygfx_shape(&sd);
            gl_frame(&s, 1);
            if (!k) S2.miter_bad += R->scene[(py * W + px) * 4] != 0.0f || R->scene[((py - 3) * W + px - 3) * 4] != 1.0f;
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    double want = band_ref(&s, i, j, &tie);
                    if (!tie) S2.miter_bad += fabs(R->scene[(j * W + i) * 4] - want) > 1e-6;
                }
        }
        sd.shape_p[0] = 0; sd.ori = 0; sd.miter_limit = 0;
        /* the sharp triangle, beveled */
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_POLYGON; sd.shape_p[0] = 3; sd.vertices = sharp; sd.w = 120; sd.h = 30;
        sd.stroke = 6; sd.stroke_align = YGFX_STROKE_OUTSIDE; sd.join = YGFX_JOIN_MITER; sd.y = -40;
        sd.color[0] = sd.color[1] = sd.color[2] = 1;
        s = ygfx_shape(&sd);
        gl_frame(&s, 1);
        {
            int i, j;
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    double want = band_ref(&s, i, j, &tie);
                    if (!tie) S2.miter_bad += fabs(R->scene[(j * W + i) * 4] - want) > 1e-6;
                }
        }
        /* refusals: MITER on a curve and on a concave polygon */
        sd.vertices = concave; sd.shape_p[0] = 4;
        s = ygfx_shape(&sd);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        S2.refused_n++; S2.refused_ok += ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        sd.shape = YGFX_CIRCLE;
        s = ygfx_shape(&sd);
        S2.refused_n++; S2.refused_ok += ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    ygfx_close(&R->g);
}

/* The CPU's bilinear interpolation of a distance texture, as the shader does
 * it: texel centers at + 0.5, indices clamped to the texture. */
static double bilinear_ref(const float* t, int tw, int th, double x, double y) {
    double fx = x - 0.5, fy = y - 0.5, ix = floor(fx), iy = floor(fy), wx = fx - ix, wy = fy - iy;
    int x0 = (int)ix, y0 = (int)iy, x1 = x0 + 1, y1 = y0 + 1;
    double c00, c10, c01, c11;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw - 1) x1 = tw - 1;
    if (y1 > th - 1) y1 = th - 1;
    if (x0 > tw - 1) x0 = tw - 1;
    if (y0 > th - 1) y0 = th - 1;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    c00 = t[y0 * tw + x0]; c10 = t[y0 * tw + x1]; c01 = t[y1 * tw + x0]; c11 = t[y1 * tw + x1];
    return (c00 + (c10 - c00) * wx) + ((c01 + (c11 - c01) * wx) - (c00 + (c10 - c00) * wx)) * wy;
}

static void gl_masks(stats* st) {
    enum { MW = 64, PAD = 8, TW = MW + 2 * PAD };
    static uint8_t mask[MW * MW];
    static float sdf[TW * TW];
    int x, y, k, sc;
    ygfx_texture_desc td;
    ygfx_tex t;
    (void)st;
    for (y = 0; y < MW; y++)
        for (x = 0; x < MW; x++) {
            double dx = x + 0.5 - 32, dy = y + 0.5 - 32;
            mask[y * MW + x] = (uint8_t)(dx * dx + dy * dy <= 24.0 * 24.0 ? 255 : 0);
        }
    {   /* the distance transform against brute force on 20 random masks:
         * every texel to the nearest texel center of the other kind, the
         * padding outside */
        static uint8_t rm[23 * 17];
        static float ro[31 * 25];
        uint32_t seed = 12345u;
        int m, rw, rh, rp, RW, RH, i, j, u, v2;
        for (m = 0; m < 20; m++) {
            rw = 5 + m % 19; rh = 3 + (m * 7) % 15; rp = m % 5;
            RW = rw + 2 * rp; RH = rh + 2 * rp;
            for (i = 0; i < rw * rh; i++) {
                seed = seed * 1664525u + 1013904223u;
                rm[i] = (uint8_t)((seed >> 24) < (uint32_t)(40 + 10 * m) ? 255 : (seed >> 16) & 127);
            }
            rm[0] = 255; rm[rw * rh - 1] = 0;   /* both kinds, always */
            CHECK(ygfx_sdf_from_mask(ro, rm, rw, rh, rp) == YGFX_OK);
            for (j = 0; j < RH; j++)
                for (i = 0; i < RW; i++) {
                    int mx = i - rp, my = j - rp;
                    int in0 = mx >= 0 && my >= 0 && mx < rw && my < rh && rm[my * rw + mx] >= 128;
                    double best = 1e30;
                    float want;
                    for (v2 = 0; v2 < RH; v2++)
                        for (u = 0; u < RW; u++) {
                            int nx = u - rp, ny = v2 - rp;
                            int in1 = nx >= 0 && ny >= 0 && nx < rw && ny < rh && rm[ny * rw + nx] >= 128;
                            if (in1 != in0) {
                                double d2 = (double)(u - i) * (u - i) + (double)(v2 - j) * (v2 - j);
                                if (d2 < best) best = d2;
                            }
                        }
                    want = in0 ? (float)(0.5 - sqrt(best)) : (float)(sqrt(best) - 0.5);
                    S2.edt_bad += ro[j * RW + i] != want;
                }
        }
    }
    CHECK(ygfx_sdf_from_mask(sdf, mask, MW, MW, PAD) == YGFX_OK);
    for (y = 0; y < TW; y++)
        for (x = 0; x < TW; x++) {
            double dx = x + 0.5 - (32 + PAD), dy = y + 0.5 - (32 + PAD);
            S2.mask_sdf_cpu = maxd(S2.mask_sdf_cpu, fabs(sdf[y * TW + x] - (sqrt(dx * dx + dy * dy) - 24.0)));
        }
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&td, 0, sizeof td);
    td.w = TW; td.h = TW; td.format = YGFX_R32F; td.data = sdf;
    t = ygfx_texture(&R->g, &td);
    CHECK(t.id != 0);
    for (sc = 1; sc <= 2; sc++)
        for (k = 0; k < 2; k++) {
            ygfx_shape_desc sd;
            ygfx_stim s;
            int i, j;
            double box = TW * sc;
            memset(&sd, 0, sizeof sd);
            sd.shape = YGFX_MASK_TEX; sd.mask = t; sd.w = sd.h = (float)box;
            sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 2;
            sd.ori = 17; sd.x = 3;
            if (k) { sd.stroke = 5; sd.stroke_align = YGFX_STROKE_CENTER; }
            sd.color[0] = sd.color[1] = sd.color[2] = 1;
            s = ygfx_shape(&sd);
            gl_frame(&s, 1);
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    float lx, ly;
                    double tx, ty, d, want;
                    ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                    if (fabs(lx) > 0.5 * box || fabs(ly) > 0.5 * box) continue;
                    tx = (lx + 0.5 * box) / sc;
                    ty = (ly + 0.5 * box) / sc;
                    d = bilinear_ref(sdf, TW, TW, tx, ty) * sc;
                    want = k ? cov(1, 2, d - 2.5) - cov(1, 2, d + 2.5) : cov(1, 2, d);
                    S2.mask_gpu = maxd(S2.mask_gpu, fabs(R->scene[(j * W + i) * 4] - want));
                }
        }
    {   /* a non-uniform fit is refused */
        ygfx_shape_desc sd;
        ygfx_stim s;
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_MASK_TEX; sd.mask = t; sd.w = 80; sd.h = 60;
        s = ygfx_shape(&sd);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        S2.refused_n++; S2.refused_ok += ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    ygfx_close(&R->g);
}

static void gl_sprites(stats* st) {
    enum { AW = 24 };
    static unsigned char atlas[AW * AW * 4];
    ygfx_texture_desc td;
    ygfx_tex t;
    int sp, mode;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&td, 0, sizeof td);
    td.w = AW; td.h = AW; td.format = YGFX_RGBA8;
    t = ygfx_texture(&R->g, &td);
    for (sp = 0; sp < 9; sp++) {
        int sx = (sp % 3) * 8, sy = (sp / 3) * 8, x, y;
        /* this sprite's texels: blue 0; every other texel: blue 255 */
        for (y = 0; y < AW; y++)
            for (x = 0; x < AW; x++) {
                unsigned char* q = &atlas[(y * AW + x) * 4];
                int own = x >= sx && x < sx + 8 && y >= sy && y < sy + 8;
                q[0] = (unsigned char)(17 * x + 3 * y + 11 * sp);
                q[1] = (unsigned char)(5 * x + 23 * y);
                q[2] = own ? 0 : 255;
                q[3] = 255;
            }
        CHECK(ygfx_texture_update(&R->g, t, 0, 0, AW, AW, atlas, 0) == YGFX_OK);
        for (mode = 0; mode < 3; mode++) {
            ygfx_image_desc idd;
            ygfx_stim s;
            float scale = mode == 0 ? 1.0f : (mode == 1 ? 3.0f : 2.5f);
            int i, j;
            memset(&idd, 0, sizeof idd);
            idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 7; idd.y = 5;
            idd.src[0] = (float)sx; idd.src[1] = (float)sy; idd.src[2] = 8; idd.src[3] = 8;
            idd.w = idd.h = 8 * scale;
            idd.linear = mode == 2;
            s = ygfx_image(&R->g, &idd);
            gl_frame(&s, 1);
            for (j = 5; j < 5 + (int)(8 * scale); j++)
                for (i = 7; i < 7 + (int)(8 * scale); i++) {
                    const float* px = &R->scene[(j * W + i) * 4];
                    double u = (i + 0.5 - 7) / scale, v = (j + 0.5 - 5) / scale;
                    S2.bleed += px[2] != 0.0f;
                    if (mode < 2) {
                        int tx = sx + (int)floor(u), ty = sy + (int)floor(v);
                        S2.sprite_bad += fabs(px[0] - atlas[(ty * AW + tx) * 4] / 255.0) > 2.2e-7;
                    } else {
                        /* the same clamped bilinear on the CPU, red channel */
                        static float red[AW * AW];
                        double fx = sx + u - 0.5, fy = sy + v - 0.5, ix = floor(fx), iy = floor(fy), wx = fx - ix, wy = fy - iy;
                        int x0 = (int)ix, y0 = (int)iy, x1 = x0 + 1, y1 = y0 + 1, q;
                        double want;
                        for (q = 0; q < AW * AW; q++) red[q] = atlas[q * 4] / 255.0f;
                        if (x0 < sx) x0 = sx;
                        if (y0 < sy) y0 = sy;
                        if (x1 > sx + 7) x1 = sx + 7;
                        if (y1 > sy + 7) y1 = sy + 7;
                        if (x0 > sx + 7) x0 = sx + 7;
                        if (y0 > sy + 7) y0 = sy + 7;
                        want = (red[y0 * AW + x0] + (red[y0 * AW + x1] - red[y0 * AW + x0]) * wx) * (1 - wy) +
                               (red[y1 * AW + x0] + (red[y1 * AW + x1] - red[y1 * AW + x0]) * wx) * wy;
                        S2.sprite_lin = maxd(S2.sprite_lin, fabs(px[0] - want));
                    }
                }
        }
    }
    {   /* a flipbook: binding src_x moves the frame */
        ygfx_image_desc idd;
        ygfx_stim s;
        ygfx_bind b;
        float vals[1] = { 16.0f };
        memset(&idd, 0, sizeof idd);
        idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT;
        idd.src[2] = 8; idd.src[3] = 8;
        s = ygfx_image(&R->g, &idd);
        memset(&b, 0, sizeof b);
        b.stim = &s; b.param = YGFX_P_SRC_X; b.channel = 0;
        CHECK(ygfx_apply(&b, 1, vals) == 1 && s.src[0] == 16.0f);
        gl_frame(&s, 1);
        S2.sprite_bad += fabs(R->scene[0] - atlas[16 * 4] / 255.0) > 2.2e-7;
    }
    ygfx_close(&R->g);
}

static void gl_tint(stats* st) {
    static const unsigned char rgba[4 * 4 * 4] = {
        200, 100, 50, 255,  10, 20, 30, 128,  255, 255, 255, 64,  0, 0, 0, 255,
        1, 2, 3, 4,  90, 180, 45, 200,  60, 70, 80, 90,  120, 130, 140, 150,
        33, 66, 99, 255,  250, 5, 125, 32,  77, 88, 99, 16,  160, 170, 180, 190,
        11, 22, 33, 44,  55, 66, 77, 88,  99, 111, 122, 133,  144, 155, 166, 177 };
    static const float g16[4 * 4] = { -1, -0.5f, 0, 0.5f, 1, 0.25f, -0.25f, 0.75f, -0.75f, 0.125f, -0.125f, 0.375f,
                                      -0.375f, 0.625f, -0.625f, 0.875f };
    ygfx_texture_desc td;
    ygfx_image_desc idd;
    ygfx_stim s;
    ygfx_tex tc, tm;
    int x, y, c;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&td, 0, sizeof td);
    td.w = 4; td.h = 4; td.format = YGFX_RGBA8; td.data = rgba;
    tc = ygfx_texture(&R->g, &td);
    td.format = YGFX_R32F; td.data = g16;
    tm = ygfx_texture(&R->g, &td);
    memset(&idd, 0, sizeof idd);
    idd.tex = tc; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT;
    idd.tint[0] = 0.5f; idd.tint[1] = 1.0f; idd.tint[2] = 0.25f; idd.tint[3] = 0.6f;
    s = ygfx_image(&R->g, &idd);
    gl_frame(&s, 1);   /* over black */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            for (c = 0; c < 3; c++) {
                const unsigned char* q = &rgba[(y * 4 + x) * 4];
                double a = q[3] / 255.0 * 0.6, want = q[c] / 255.0 * idd.tint[c] * a;
                S2.tint = maxd(S2.tint, fabs(R->scene[(y * W + x) * 4 + c] - want));
            }
    ygfx_close(&R->g);
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    tm = ygfx_texture(&R->g, &td);
    memset(&idd, 0, sizeof idd);
    idd.tex = tm; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.modulation = true;
    idd.contrast = 0.8f; idd.tint[0] = 1.0f; idd.tint[1] = 0.5f; idd.tint[2] = 0.0f; idd.tint[3] = 0.5f;
    s = ygfx_image(&R->g, &idd);
    gl_frame(&s, 1);   /* over mid-gray; dir 0 = the background */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            for (c = 0; c < 3; c++) {
                double want = 0.5 + 0.8 * g16[y * 4 + x] * 0.5 * idd.tint[c] * 0.5;
                S2.tint = maxd(S2.tint, fabs(R->scene[(y * W + x) * 4 + c] - want));
            }
    {   /* ADD: the increment times contrast, tint.rgb and tint.a */
        static float inc[4 * 4 * 4];
        for (x = 0; x < 64; x++) inc[x] = 0.5f * g16[x / 4] * (x % 4 == 1 ? -1.0f : 1.0f);
        td.format = YGFX_RGBA32F; td.data = inc;
        tm = ygfx_texture(&R->g, &td);
        memset(&idd, 0, sizeof idd);
        idd.tex = tm; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.add = true;
        idd.contrast = 0.8f; idd.tint[0] = 1.0f; idd.tint[1] = 0.5f; idd.tint[2] = 0.25f; idd.tint[3] = 0.5f;
        s = ygfx_image(&R->g, &idd);
        gl_frame(&s, 1);
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                for (c = 0; c < 3; c++) {
                    double want = 0.5 + 0.8 * (double)inc[(y * 4 + x) * 4 + c] * idd.tint[c] * 0.5;
                    S2.tint = maxd(S2.tint, fabs(R->scene[(y * W + x) * 4 + c] - want));
                }
    }
    ygfx_close(&R->g);
}

static void gl_groups(stats* st) {
    static float a1[W * H * 4];
    ygfx_group gr;
    ygfx_group_desc gdsc;
    ygfx_stim m[3], eq[3];
    ygfx_shape_desc sd;
    ygfx_gabor_desc gb;
    int k, i, j;
    (void)st;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&gdsc, 0, sizeof gdsc);
    gdsc.x = 10; gdsc.y = -5; gdsc.ori = 25; gdsc.scale = 1.5f; gdsc.opacity = 0.5f;
    gdsc.w = 100; gdsc.h = 60; gdsc.anchor = YGFX_TOP_LEFT;
    gr = ygfx_group_make(&gdsc);
    memset(&sd, 0, sizeof sd);
    sd.place = YGFX_TOP_LEFT; sd.x = 10; sd.y = 10; sd.w = 20; sd.h = 10; sd.ori = 10;
    sd.color[0] = 0.9f; sd.color[1] = 0.1f; sd.color[2] = 0.3f; sd.group = &gr;
    m[0] = ygfx_shape(&sd);
    sd.shape = YGFX_CIRCLE; sd.place = YGFX_CENTER; sd.x = 5; sd.y = 0; sd.w = sd.h = 16;
    sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 3; sd.stroke = 2;
    m[1] = ygfx_shape(&sd);
    memset(&gb, 0, sizeof gb);
    gb.sigma = 6; gb.sf = 0.1f; gb.contrast = 0.5f; gb.group = &gr;
    m[2] = ygfx_gabor(&gb);
    m[2].place = YGFX_BOTTOM_RIGHT; m[2].x = -20; m[2].y = -15;
    /* the same stimuli placed alone, from the group's transform done by hand */
    for (k = 0; k < 3; k++) {
        float ax, ay;
        ygfx_resolve(&R->g, &m[k], &ax, &ay);
        eq[k] = m[k];
        eq[k].group = NULL;
        eq[k].place = YGFX_TOP_LEFT;
        eq[k].x = ax; eq[k].y = ay;
        eq[k].ori = m[k].ori + 25.0f;
        eq[k].w *= 1.5f; eq[k].h *= 1.5f;
        eq[k].sf /= 1.5f; eq[k].sigma *= 1.5f;
        eq[k].gate *= 0.5f;
    }
    for (k = 0; k < 3; k++) {
        gl_frame(&m[k], 1);
        memcpy(a1, R->scene, sizeof a1);
        gl_frame(&eq[k], 1);
        for (i = 0; i < W * H * 4; i++) S2.group_diff = maxd(S2.group_diff, fabs(a1[i] - R->scene[i]));
    }
    {   /* by hand: the rect member's anchor */
        float ax, ay;
        double c = cos(25 * 3.14159265358979323846 / 180), s = sin(25 * 3.14159265358979323846 / 180);
        double qx = (10 + 0 - 0) * 1.5, qy = (10 + 0 - 0) * 1.5;   /* place TOP_LEFT of the box; pivot its top-left */
        ygfx_resolve(&R->g, &m[0], &ax, &ay);
        CHECK(fabs(ax - (W / 2.0 + 10 + c * qx - s * qy)) < 1e-3 && fabs(ay - (H / 2.0 - 5 + s * qx + c * qy)) < 1e-3);
    }
    {   /* hit through the group, against the GPU's hard coverage */
        ygfx_stim h = m[0];
        h.gate = 2.0f;    /* group opacity 0.5: full coverage reads 0.4 x 2 x ... */
        gl_frame(&h, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                int tie;
                double want = band_ref(&h, i, j, &tie);
                int hit = ygfx_hit(&R->g, &h, (float)i + 0.5f, (float)j + 0.5f);
                if (!tie) S2.group_hit_bad += hit != (want >= 0.5);
            }
    }
    {   /* a binding on the group moves every member */
        ygfx_bind b;
        float v[1] = { 40.0f }, x0[3], y0[3], x1, y1;
        for (k = 0; k < 3; k++) ygfx_resolve(&R->g, &m[k], &x0[k], &y0[k]);
        memset(&b, 0, sizeof b);
        b.group = &gr; b.param = YGFX_G_X; b.channel = 0;
        CHECK(ygfx_apply(&b, 1, v) == 1);
        for (k = 0; k < 3; k++) {
            ygfx_resolve(&R->g, &m[k], &x1, &y1);
            CHECK(fabsf(x1 - x0[k] - 30.0f) < 1e-3f && fabsf(y1 - y0[k]) < 1e-3f);
        }
    }
    ygfx_close(&R->g);
}

static int g_mess_n;   /* reset per renderer in gl_targets */

/* A frame with no readback: a read drops the GL state cache by itself. */
static void frame_noread(const ygfx_stim* s, int n) {
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
    CHECK(ygfx_draw_n(&R->g, s, n) == YGFX_OK);
    CHECK(ygfx_end(&R->g) == YGFX_OK);
    CHECK(yscr_flip(&R->scr) == YSCR_OK);
}
static void mess(void* ctx, const yscr_present_info* info) {
    void (YGFX__APIENTRY * use)(unsigned int) = NULL;
    void (YGFX__APIENTRY * bindva)(unsigned int) = NULL;
    void (YGFX__APIENTRY * dis)(unsigned int) = NULL;
    void (YGFX__APIENTRY * bbase)(unsigned int, unsigned int, unsigned int) = NULL;
    void (YGFX__APIENTRY * active)(unsigned int) = NULL;
    void (YGFX__APIENTRY * bindtex)(unsigned int, unsigned int) = NULL;
    yscr_proc p;
    (void)info;
    p = yscr_gl_proc((yscr_screen*)ctx, "glUseProgram");      memcpy(&use, &p, sizeof p);
    p = yscr_gl_proc((yscr_screen*)ctx, "glBindVertexArray"); memcpy(&bindva, &p, sizeof p);
    p = yscr_gl_proc((yscr_screen*)ctx, "glDisable");         memcpy(&dis, &p, sizeof p);
    p = yscr_gl_proc((yscr_screen*)ctx, "glBindBufferBase");  memcpy(&bbase, &p, sizeof p);
    p = yscr_gl_proc((yscr_screen*)ctx, "glActiveTexture");   memcpy(&active, &p, sizeof p);
    p = yscr_gl_proc((yscr_screen*)ctx, "glBindTexture");     memcpy(&bindtex, &p, sizeof p);
    use(0); bindva(0); dis(0x0BE2); bbase(0x8A11, 0, 0); bbase(0x8A11, 1, 0);
    active(0x84C0); bindtex(0x0DE1, 0);
    active(0x84C1); bindtex(0x0DE1, 0);
    g_mess_n++;
}

static void gl_targets(stats* st) {
    static float a1[W * H * 4], rb[64 * 48 * 4];
    static uint8_t o1[W * H * 4];
    ygfx_target_desc tdsc;
    ygfx_tex t;
    ygfx_shape_desc sd;
    ygfx_stim shapes[3], img, direct[3];
    int k, i, j;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&tdsc, 0, sizeof tdsc);
    tdsc.w = 64; tdsc.h = 48;
    t = ygfx_target(&R->g, &tdsc);
    CHECK(t.id != 0);
    memset(&sd, 0, sizeof sd);
    sd.place = YGFX_TOP_LEFT; sd.anchor = YGFX_TOP_LEFT; sd.x = 2; sd.y = 3; sd.w = 10; sd.h = 6;
    sd.color[0] = 0.5f; sd.color[1] = 0.25f; sd.color[2] = 0.75f;
    shapes[0] = ygfx_shape(&sd);
    sd.x = 30; sd.y = 20; sd.w = 20; sd.h = 20; sd.shape = YGFX_CIRCLE; sd.color[0] = 1; sd.color[1] = 0.125f; sd.color[2] = 0;
    shapes[1] = ygfx_shape(&sd);
    sd.x = 20; sd.y = 30; sd.shape = YGFX_RECT; sd.w = 12; sd.h = 4; sd.color[0] = 0; sd.color[1] = 1; sd.color[2] = 0.5f; sd.opacity = 0.5f;
    shapes[2] = ygfx_shape(&sd);
    /* frame 1: draw into the target, then the target 1:1 at (5, 7) */
    {
        ygfx_image_desc idd;
        static const float zero[4] = { 0, 0, 0, 0 };
        memset(&idd, 0, sizeof idd);
        idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 5; idd.y = 7;
        img = ygfx_image(&R->g, &idd);
        CHECK(img.flags & YGFX_STIM_PREMULTIPLIED);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_ERR_ORDER);      /* not nested */
        CHECK(ygfx_draw_n(&R->g, shapes, 3) == YGFX_OK);
        S2.refused_n++; S2.refused_ok += ygfx_draw(&R->g, &img) == YGFX_ERR_ORDER;   /* its own target */
        CHECK(ygfx_end_target(&R->g) == YGFX_OK);
        CHECK(ygfx_draw(&R->g, &img) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(ygfx_read_scene(&R->g, 0, 0, W, H, R->scene) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        CHECK(ygfx_begin_target(&R->g, t, NULL) == YGFX_ERR_ORDER);       /* outside a frame */
        CHECK(ygfx_read_target(&R->g, t, 0, 0, 64, 48, rb) == YGFX_OK);
        /* rows top first: the first rect's top-left texel is (2, 3) */
        S2.target_bad += rb[(3 * 64 + 2) * 4] != 0.5f || rb[(3 * 64 + 1) * 4] != 0.0f || rb[(2 * 64 + 2) * 4] != 0.0f;
        S2.target_bad += rb[(3 * 64 + 2) * 4 + 3] != 1.0f;   /* premultiplied alpha accumulated */
        memcpy(a1, R->scene, sizeof a1);
    }
    for (k = 0; k < 3; k++) { direct[k] = shapes[k]; direct[k].x += 5; direct[k].y += 7; }
    gl_frame(direct, 3);
    for (i = 0; i < W * H * 4; i++) S2.target_diff = maxd(S2.target_diff, fabs(a1[i] - R->scene[i]));
    /* persistence: sampled on later frames without drawing into it */
    for (k = 0; k < 4; k++) gl_frame(&img, 1);
    for (i = 0; i < W * H * 4; i++) S2.persist_bad += a1[i] != R->scene[i];
    {   /* a clear color fills the target; a pass with no clear keeps it */
        static const float c[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        int pass;
        for (pass = 0; pass < 2; pass++) {
            CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
            CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
            CHECK(ygfx_begin_target(&R->g, t, pass ? NULL : c) == YGFX_OK);
            if (pass) CHECK(ygfx_draw(&R->g, &shapes[0]) == YGFX_OK);
            CHECK(ygfx_end_target(&R->g) == YGFX_OK);
            CHECK(ygfx_end(&R->g) == YGFX_OK);
            CHECK(yscr_flip(&R->scr) == YSCR_OK);
            CHECK(ygfx_read_target(&R->g, t, 0, 0, 64, 48, rb) == YGFX_OK);
            for (i = 0; i < 64 * 48; i++) {
                int x = i % 64, y = i / 64, in = x >= 2 && x < 12 && y >= 3 && y < 9;
                if (pass && in) S2.target_bad += rb[i * 4] != 0.5f || rb[i * 4 + 2] != 0.75f;
                else for (k = 0; k < 4; k++) S2.target_bad += rb[i * 4 + k] != c[k];
            }
        }
    }
    ygfx_close(&R->g);

    /* ADD: 30 gabors rendered once into a float target, added each frame */
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&tdsc, 0, sizeof tdsc);
    tdsc.w = W; tdsc.h = H;
    t = ygfx_target(&R->g, &tdsc);
    {
        ygfx_stim gabs[30];
        ygfx_gabor_desc gb;
        ygfx_image_desc idd;
        static const float zero[4] = { 0, 0, 0, 0 };
        for (k = 0; k < 30; k++) {
            memset(&gb, 0, sizeof gb);
            gb.sigma = 8; gb.sf = 0.07f; gb.contrast = 0.3f; gb.ori = (float)(k * 13);
            gb.x = (float)((k * 37) % 280 - 140); gb.y = (float)((k * 53) % 160 - 80);
            gabs[k] = ygfx_gabor(&gb);
        }
        memset(&idd, 0, sizeof idd);
        idd.tex = t; idd.add = true;
        img = ygfx_image(&R->g, &idd);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, gabs, 30) == YGFX_OK);
        CHECK(ygfx_end_target(&R->g) == YGFX_OK);
        CHECK(ygfx_draw(&R->g, &img) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(ygfx_read_scene(&R->g, 0, 0, W, H, a1) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        gl_frame(gabs, 30);
        for (i = 0; i < W * H * 4; i += 4) S2.add_diff = maxd(S2.add_diff, fabs(a1[i] - R->scene[i]));
    }
    ygfx_close(&R->g);

    /* the CLUT once: through a target the output codes equal direct drawing */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    {
        static const float lut[3 * 5] = { 0, 0.1f, 0.35f, 0.7f, 1, 0, 0.5f, 0.7f, 0.86f, 1, 0, 0.25f, 0.5f, 0.75f, 1 };
        ygfx_image_desc idd;
        static const float zero[4] = { 0, 0, 0, 0 };
        CHECK(ygfx_set_lut(&R->g, lut, 5) == YGFX_OK);
        memset(&tdsc, 0, sizeof tdsc);
        tdsc.w = 64; tdsc.h = 48;
        t = ygfx_target(&R->g, &tdsc);
        memset(&idd, 0, sizeof idd);
        idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT;
        img = ygfx_image(&R->g, &idd);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, shapes, 2) == YGFX_OK);
        CHECK(ygfx_end_target(&R->g) == YGFX_OK);
        CHECK(ygfx_draw(&R->g, &img) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(ygfx_read_output(&R->g, 0, 0, W, H, o1) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        gl_frame(shapes, 2);
        for (i = 0; i < W * H * 4; i++) S2.clut_bad += o1[i] != R->out[i];
    }
    ygfx_close(&R->g);

    /* RGBA8 targets read back as their codes */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    {
        static const float zero[4] = { 0, 0, 0, 0 };
        memset(&tdsc, 0, sizeof tdsc);
        tdsc.w = 64; tdsc.h = 48; tdsc.format = YGFX_RGBA8;
        t = ygfx_target(&R->g, &tdsc);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_OK);
        CHECK(ygfx_draw(&R->g, &shapes[0]) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);   /* end() closes an open target pass */
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        CHECK(ygfx_read_target(&R->g, t, 0, 0, 64, 48, rb) == YGFX_OK);
        S2.target_bad += rb[(3 * 64 + 2) * 4] != 128.0f / 255.0f && rb[(3 * 64 + 2) * 4] != 127.0f / 255.0f;
        S2.target_bad += rb[(3 * 64 + 2) * 4 + 1] != 64.0f / 255.0f;
    }
    ygfx_close(&R->g);

    /* the GL epoch: a present hook that changes GL state; the next frame must
     * be the reference all the same. The control: the same changes made
     * behind ysp_gfx's back with no epoch, which must show. */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    gl_frame(shapes, 3);
    memcpy(o1, R->out, sizeof o1);
#if YGFX__EPOCH
    g_mess_n = 0;
    yscr_on_present(&R->scr, mess, &R->scr);
    frame_noread(shapes, 3);    /* fills the cache; the hook then changes GL */
    frame_noread(shapes, 3);
    yscr_on_present(&R->scr, NULL, NULL);
    CHECK(ygfx_read_output(&R->g, 0, 0, W, H, R->out) == YGFX_OK);
    for (i = 0; i < W * H * 4; i++) S2.epoch_bad += o1[i] != R->out[i];
    CHECK(g_mess_n == 2);
    frame_noread(shapes, 3);    /* the control: the same change, no epoch */
    mess(&R->scr, NULL);
    frame_noread(shapes, 3);
    CHECK(ygfx_read_output(&R->g, 0, 0, W, H, R->out) == YGFX_OK);
    for (i = 0; i < W * H * 4; i++) S2.epoch_control_bad += o1[i] != R->out[i];
    {   /* a new context, simulated by moving the screen's generation: the
         * next GL call reports it and the handle closes without deleting
         * (the names would belong to the new context); close and open again
         * give the reference frame */
        uint32_t keep = R->scr.gl_generation;
        R->scr.gl_generation = keep + 1;
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, shapes, 3) == YGFX_OK);
        S2.lost_ok += ygfx_end(&R->g) == YGFX_ERR_LOST;
        S2.lost_ok += !ygfx_is_open(&R->g);
        S2.lost_ok += strstr(ygfx_error(&R->g), "context is new") != NULL;
        /* v0.12: the message is the last failure's, this read's */
        S2.lost_ok += ygfx_read_output(&R->g, 0, 0, W, H, R->out) == YGFX_ERR_CLOSED &&
                      strstr(ygfx_error(&R->g), "read_output") != NULL;
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        R->scr.gl_generation = keep;
        ygfx_close(&R->g);
        if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
        gl_frame(shapes, 3);
        for (i = 0; i < W * H * 4; i++) S2.lost_bad += o1[i] != R->out[i];
    }
#endif
    ygfx_close(&R->g);
    (void)j;
}

/* v0.1's OVER kept the destination's alpha; v0.2's accumulates it, which a
 * target needs. The scene's rgb and the output codes must not move: the same
 * frame through a backend that puts v0.1's blend back after each apply. */
static ygfx__gl g_v01_ctx;
static ygfx_backend g_v01_be;
static void v01_apply(void* c, const ygfx_bindings* b) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__gl_backend.apply(c, b);
    if (gl->c_blend == YGFX_BLEND_OVER)
        gl->f.BlendFuncSeparate(YGFX__GL_ONE, YGFX__GL_ONE_MINUS_SRC_ALPHA, YGFX__GL_ZERO, YGFX__GL_ONE);
}

static void gl_alpha_passes(stats* st) {
    static float s1[W * H * 4], t1[64 * 48 * 4], t2[64 * 48 * 4];
    static uint8_t o1[W * H * 4];
    static const unsigned char rgba[2 * 2 * 4] = { 200, 100, 50, 255, 10, 20, 30, 128, 255, 255, 255, 64, 1, 2, 3, 4 };
    static const float zero[4] = { 0, 0, 0, 0 };
    ygfx_stim s[4];
    int v, i, k;
    (void)st;
    g_v01_be = ygfx__gl_backend;
    g_v01_be.apply = v01_apply;
    for (v = 0; v < 2; v++) {
        ygfx_desc gd;
        ygfx_shape_desc sd;
        ygfx_image_desc idd;
        ygfx_texture_desc td;
        ygfx_target_desc rd;
        ygfx_tex tex, tg;
        memset(&gd, 0, sizeof gd);
        gd.screen = &R->scr;
        gd.background[0] = 0.3f; gd.background[1] = 0.5f; gd.background[2] = 0.7f;
        ygfx__test_scene32 = 1;
        if (v) { gd.backend = &g_v01_be; gd.backend_ctx = &g_v01_ctx; }
        else gd.cache = test_cache();
        if (!ygfx_open(&R->g, &gd)) { fprintf(stderr, "gfx_test [%s]: open: %s\n", g_where, ygfx_error(&R->g)); g_failures++; return; }
        memset(&td, 0, sizeof td);
        td.w = 2; td.h = 2; td.format = YGFX_RGBA8; td.data = rgba;
        tex = ygfx_texture(&R->g, &td);
        memset(&rd, 0, sizeof rd);
        rd.w = 64; rd.h = 48;
        tg = ygfx_target(&R->g, &rd);
        memset(&sd, 0, sizeof sd);
        sd.w = 120; sd.h = 80; sd.color[0] = 0.9f; sd.color[1] = 0.2f; sd.color[2] = 0.1f; sd.opacity = 0.6f;
        sd.edge = YGFX_EDGE_GAUSSIAN; sd.edge_width = 2;
        s[0] = ygfx_shape(&sd);
        sd.shape = YGFX_CIRCLE; sd.x = 30; sd.w = 90; sd.color[0] = 0.1f; sd.color[1] = 0.8f; sd.opacity = 0.35f;
        s[1] = ygfx_shape(&sd);
        memset(&idd, 0, sizeof idd);
        idd.tex = tex; idd.w = 100; idd.h = 100; idd.x = -40; idd.linear = true;
        s[2] = ygfx_image(&R->g, &idd);
        s[3] = s[1]; s[3].opacity = 0.5f;
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, s, 3) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, tg, zero) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, s, 2) == YGFX_OK);
        CHECK(ygfx_end_target(&R->g) == YGFX_OK);
        CHECK(ygfx_draw(&R->g, &s[3]) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(ygfx_read_scene(&R->g, 0, 0, W, H, R->scene) == YGFX_OK);
        CHECK(ygfx_read_output(&R->g, 0, 0, W, H, R->out) == YGFX_OK);
        CHECK(ygfx_read_target(&R->g, tg, 0, 0, 64, 48, v ? t2 : t1) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        if (!v) { memcpy(s1, R->scene, sizeof s1); memcpy(o1, R->out, sizeof o1); ygfx_close(&R->g); continue; }
        for (i = 0; i < W * H * 4; i++) {
            if (i % 4 != 3) S2.alpha_scene_bad += s1[i] != R->scene[i];
            S2.alpha_scene_bad += o1[i] != R->out[i];
        }
        /* the control: in a target, cleared to alpha 0, the two blends differ */
        for (i = 3; i < 64 * 48 * 4; i += 4) S2.alpha_control += t1[i] != t2[i];
        /* at most YGFX_MAX_PASSES target passes a frame */
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        for (k = 0; k < YGFX_MAX_PASSES; k++) {
            S2.passes_bad += ygfx_begin_target(&R->g, tg, NULL) != YGFX_OK;
            S2.passes_bad += ygfx_end_target(&R->g) != YGFX_OK;
        }
        S2.passes_bad += ygfx_begin_target(&R->g, tg, NULL) != YGFX_ERR_ORDER;
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        ygfx_close(&R->g);
    }
}

/* -------------------------------------------- the edge model against truth */

/* The checks above compare the GPU with the CPU form of the same function,
 * so they cannot see where that function is not the Euclidean distance, nor
 * where a profile of the distance is not a blur of the shape. These compare
 * with references that do not share the header's arithmetic: the exact
 * Gaussian blur of the hard shape, and the Euclidean distance by brute force
 * over the boundary's segments. Each deviation must be at most the analytic
 * value plus the renderer's tolerance, and at least a stated fraction of it
 * (the control: the reference must be able to see the deviation). */

typedef struct stats3 {
    double vtx_blur, disc_blur[2], disc_pred[2], miter_bound, miter_pred;
} stats3;

static stats3 S3;

static double Phi(double x) { return 0.5 * erfc(-x / sqrt(2.0)); }

/* The hard disc of radius R blurred by a Gaussian of SD sg, at distance r
 * from its center: a 1-D integral over y = R sin t, composite Simpson. */
static double blurred_disc(double r, double rad, double sg) {
    const int n = 2000;
    const double pi = 3.14159265358979323846, h = pi / n;
    double sum = 0;
    int k;
    for (k = 0; k <= n; k++) {
        double t = -0.5 * pi + k * h, y = rad * sin(t), hx = rad * cos(t);
        double f = (Phi((hx - r) / sg) - Phi((-hx - r) / sg)) * exp(-0.5 * y * y / (sg * sg)) / (sg * sqrt(2 * pi)) * rad * cos(t);
        sum += f * (k == 0 || k == n ? 1 : (k % 2 ? 4 : 2));
    }
    return sum * h / 3.0;
}

/* Signed Euclidean distance to the boundary of an axis-aligned box, by brute
 * force over its four edge segments; inside negative. */
static double seg_dist(double px, double py, double ax, double ay, double bx, double by) {
    double ex = bx - ax, ey = by - ay, t = ((px - ax) * ex + (py - ay) * ey) / (ex * ex + ey * ey);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return sqrt((px - ax - t * ex) * (px - ax - t * ex) + (py - ay - t * ey) * (py - ay - t * ey));
}
static double box_brute(double px, double py, double x0, double y0, double x1, double y1) {
    double d = seg_dist(px, py, x0, y0, x1, y0);
    d = fmin(d, seg_dist(px, py, x1, y0, x1, y1));
    d = fmin(d, seg_dist(px, py, x1, y1, x0, y1));
    d = fmin(d, seg_dist(px, py, x0, y1, x0, y0));
    return (px > x0 && px < x1 && py > y0 && py < y1) ? -d : d;
}

static void gl_edge_truth(stats* st) {
    ygfx_shape_desc sd;
    ygfx_stim s;
    int i, j, k;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    /* a sharp rect, GAUSSIAN SD 2 px, its top-left vertex on a pixel center */
    memset(&sd, 0, sizeof sd);
    sd.place = YGFX_TOP_LEFT; sd.anchor = YGFX_TOP_LEFT; sd.x = 40.5f; sd.y = 30.5f; sd.w = 120; sd.h = 80;
    sd.edge = YGFX_EDGE_GAUSSIAN; sd.edge_width = 2;
    sd.color[0] = sd.color[1] = sd.color[2] = 1;
    for (k = 0; k < 2; k++) {
        sd.join = k ? YGFX_JOIN_MITER : YGFX_JOIN_ROUND;
        s = ygfx_shape(&sd);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                double x = i + 0.5, y = j + 0.5, got = R->scene[(j * W + i) * 4];
                if (!k) {   /* the exact blur of the rect: a product of erf */
                    double b = (Phi((x - 40.5) / 2) - Phi((x - 160.5) / 2)) * (Phi((y - 30.5) / 2) - Phi((y - 110.5) / 2));
                    S3.vtx_blur = maxd(S3.vtx_blur, fabs(got - b));
                } else {    /* MITER's field against the Euclidean distance */
                    double d = box_brute(x, y, 40.5, 30.5, 160.5, 110.5);
                    S3.miter_bound = maxd(S3.miter_bound, fabs(got - cov(YGFX_EDGE_GAUSSIAN, 2, d)));
                }
            }
    }
    /* the predicted MITER deviation, on a fine grid outside a corner */
    S3.miter_pred = 0;
    for (j = 0; j <= 400; j++)
        for (i = 0; i <= 400; i++) {
            double x = i * 0.02, y = j * 0.02;
            S3.miter_pred = maxd(S3.miter_pred, cov(YGFX_EDGE_GAUSSIAN, 2, x > y ? x : y) -
                                                cov(YGFX_EDGE_GAUSSIAN, 2, sqrt(x * x + y * y)));
        }
    /* discs of R / sigma 5 and 20, centered off the pixel grid */
    for (k = 0; k < 2; k++) {
        enum { NT = 4801 };
        static double tab[NT];
        const double Rr = k ? 40.0 : 10.0, sg = 2.0, r0 = Rr - 12.0, dr = 24.0 / (NT - 1);
        int q;
        for (q = 0; q < NT; q++) tab[q] = blurred_disc(r0 + q * dr, Rr, sg);
        S3.disc_pred[k] = 0;
        for (q = 0; q < NT; q++) S3.disc_pred[k] = maxd(S3.disc_pred[k], fabs(cov(YGFX_EDGE_GAUSSIAN, sg, r0 + q * dr - Rr) - tab[q]));
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_CIRCLE; sd.w = (float)(2 * Rr); sd.x = 0.3f; sd.y = -0.2f;
        sd.edge = YGFX_EDGE_GAUSSIAN; sd.edge_width = (float)sg;
        sd.color[0] = sd.color[1] = sd.color[2] = 1;
        s = ygfx_shape(&sd);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                float lx, ly;
                double r, u, b;
                int q0;
                ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                r = sqrt((double)lx * lx + (double)ly * ly);
                if (r < r0 || r >= r0 + 24.0) continue;
                u = (r - r0) / dr; q0 = (int)u;
                if (q0 >= NT - 1) q0 = NT - 2;
                b = tab[q0] + (tab[q0 + 1] - tab[q0]) * (u - q0);
                S3.disc_blur[k] = maxd(S3.disc_blur[k], fabs(R->scene[(j * W + i) * 4] - b));
            }
    }
    ygfx_close(&R->g);
}

/* ---------------------------------------------------------------- v0.3 */

typedef struct stats4 {
    double soft, cpu_brute, dash_soft;   /* T1; T2 distance error (px), CPU form against brute force */
    double ell_quarter;                  /* the quarter perimeter against Simpson, relative (v0.7) */
    long   hard_bad, hit_bad, ties, n_shapes;
    char   worst[64];
} stats4;

static stats4 S4;

/* The shader's ysp_along_ (dashes and trim), in double, from the block. */
static double along_ref(const float* b, double cm, double nn, double hh, double s) {
    int fl = (int)b[23], cap = (int)b[26], k;
    int dsh = (fl & 1) != 0, trm = (fl & 2) != 0;
    double P = (double)b[16] + b[17], u = dsh ? s - b[18] - P * floor((s - b[18]) / P) : 0.0;
    int prof = (int)b[21];
    double w = b[22];
    if (cap != 1) {
        double ex = cap == 2 ? hh : 0.0, a = 1.0;
        if (dsh) {
            a = 0.0;
            for (k = -1; k <= 1; k++) { double uk = u + k * P; a += cov(prof, w, uk - b[16] - ex) - cov(prof, w, uk + ex); }
        }
        if (trm) a *= cov(prof, w, s - b[25] - ex) - cov(prof, w, s - b[24] + ex);
        return cm * a;
    } else {
        double ga = 0.0;
        if (dsh) { ga = 1e300; for (k = -1; k <= 1; k++) ga = fmin(ga, fmax(fabs(u + k * P - 0.5 * b[16]) - 0.5 * b[16], 0.0)); }
        if (trm) ga = fmax(ga, fmax(fabs(s - 0.5 * ((double)b[24] + b[25])) - 0.5 * ((double)b[25] - b[24]), 0.0));
        return cov(prof, w, sqrt(ga * ga + nn * nn) - hh);
    }
}

/* The main layer's coverage of a vector stimulus on the CPU, at a pixel. */
static double vec_ref(const float* b, const ygfx_stim* s, int i, int j, int* tie) {
    float lx, ly;
    double f0[4], f1[4], d, hw = b[15], c = b[11], cm, w = b[22];
    int prof = (int)b[21], fl = (int)b[23];
    ygfx_local(&R->g, s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
    ygfx__vfield(b, lx, ly, f0, f1);
    d = f0[0] - b[10];
    *tie = hw > 0 ? (fabs(d - c - hw) < 1e-3 || fabs(d - c + hw) < 1e-3) : fabs(d) < 1e-3;
    cm = hw > 0 ? cov(prof, w, d - c - hw) - cov(prof, w, d - c + hw) : cov(prof, w, d);
    if (fl & 3) {
        double P = (double)b[16] + b[17], u = f0[3] - b[18] - P * floor((f0[3] - b[18]) / P);
        cm = along_ref(b, cm, hw > 0 ? d - c : d + b[35], hw > 0 ? hw : b[35], f0[3]);
        /* a pixel on a dash's end is a tie too */
        if ((fl & 1) && (fabs(u) < 2e-2 || fabs(u - b[16]) < 2e-2 || fabs(u - P) < 2e-2)) *tie = 1;
        if ((fl & 2) && (fabs(f0[3] - b[24]) < 2e-2 || fabs(f0[3] - b[25]) < 2e-2)) *tie = 1;
    }
    return cm;
}

/* One stimulus against the CPU form, every pixel. */
typedef struct vc_ctx { const float* blk; const ygfx_stim* s; int hard; double* want; signed char* tie; signed char* hit; } vc_ctx;
static void vc_row(void* p, int j) {
    const vc_ctx* c = (const vc_ctx*)p;
    int i, tie;
    for (i = 0; i < W; i++) {
        c->want[j * W + i] = vec_ref(c->blk, c->s, i, j, &tie);
        c->tie[j * W + i] = (signed char)tie;
        /* the hit test only where it is checked: it packs the block at each call */
        c->hit[j * W + i] = (signed char)(c->hard ? ygfx_hit(&R->g, c->s, (float)i + 0.5f, (float)j + 0.5f) : 0);
    }
}
static void vec_check(const ygfx_stim* s, const char* what) {
    static float blk[16 * 64];
    static double wants[W * H];
    static signed char ties[W * H], hits[W * H];
    double ext[2], worst = 0;
    const char* err = "";
    int clip, i, j, tie, n;
    vc_ctx vc;
    n = ygfx__vpack(&R->g, s, blk, ext, &clip, &err);
    if (n < 0) { fprintf(stderr, "gfx_test [%s]: %s refused: %s\n", g_where, what, err); g_failures++; return; }
    gl_frame(s, 1);
    S4.n_shapes++;
    vc.blk = blk; vc.s = s; vc.hard = s->edge == YGFX_EDGE_HARD; vc.want = wants; vc.tie = ties; vc.hit = hits;
    par_rows(vc_row, &vc, H);
    for (j = 0; j < H; j++)
        for (i = 0; i < W; i++) {
            double want = wants[j * W + i], got = R->scene[(j * W + i) * 4];
            int hit = hits[j * W + i];
            if (ties[j * W + i]) { S4.ties++; continue; }
            if (s->edge == YGFX_EDGE_HARD) {
                if (fabs(got - want) > 1e-6) { S4.hard_bad++; if (S4.hard_bad < 40 && getenv("YGFX_TEST_VERBOSE")) fprintf(stderr, "  hard %s at %d %d: got %g want %g\n", what, i, j, got, want); }
                S4.hit_bad += hit != (got >= 0.5);
            } else {
                worst = maxd(worst, fabs(got - want));
            }
        }
    /* coverage that depends on the arc length (dashes, trim) apart: its
     * float error grows with the length along the boundary */
    if ((int)blk[23] & 3) S4.dash_soft = maxd(S4.dash_soft, worst);
    else if (worst > S4.soft) { S4.soft = worst; snprintf(S4.worst, sizeof S4.worst, "%s", what); }
    if (worst > 1e-3 && getenv("YGFX_TEST_VERBOSE")) {
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                double want = vec_ref(blk, s, i, j, &tie), got = R->scene[(j * W + i) * 4];
                if (!tie && fabs(got - want) > 0.5 * worst) {
                    float lx, ly;
                    double f0[4], f1[4];
                    ygfx_local(&R->g, s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                    ygfx__vfield(blk, lx, ly, f0, f1);
                    fprintf(stderr, "  %s: worst %.3g at %d %d local %.2f %.2f: got %.5f want %.5f (d %.4f s %.3f)\n",
                            what, worst, i, j, lx, ly, got, want, f0[0], f0[3]);
                    j = H; break;
                }
            }
    }
}

static void gl_v03_kinds(stats* st) {
    static const float qb[6] = { -70, 40, 0, -80, 70, 40 };
    static const float zig[8] = { -110, 30, -40, -40, 20, 35, 100, -20 };
    static const float conc[10] = { -60, -50, 60, -50, 60, 50, 0, 0, -60, 50 };
    static const float tri[6] = { -60, 45, 60, 45, 0, -55 };
    ygfx_shape_desc base[24];
    const char* names[24];
    int nb = 0, k, e, o, st2;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(base, 0, sizeof base);
#define ADD_(nm) (names[nb] = nm, &base[nb++])
    { ygfx_shape_desc* d = ADD_("rrect"); d->shape = YGFX_RRECT; d->w = 120; d->h = 80; d->shape_p[0] = 5; d->shape_p[1] = 20; d->shape_p[3] = 35; }
    { ygfx_shape_desc* d = ADD_("circle"); d->shape = YGFX_CIRCLE; d->w = 110; d->dash[0] = 14; d->dash[1] = 9; d->stroke = 5; }
    { ygfx_shape_desc* d = ADD_("annulus"); d->shape = YGFX_ANNULUS; d->w = 130; d->shape_p[0] = 30; d->dash[0] = 20; d->dash[1] = 7; d->stroke = 4; }
    { ygfx_shape_desc* d = ADD_("rect dashed"); d->shape = YGFX_RECT; d->w = 130; d->h = 70; d->shape_p[1] = 10; d->dash[0] = 12; d->dash[1] = 6; d->dash_offset = 3; d->stroke = 4; }
    { ygfx_shape_desc* d = ADD_("line dashed"); d->shape = YGFX_LINE; d->w = 200; d->shape_p[0] = 12; d->dash[0] = 18; d->dash[1] = 10; }
    { ygfx_shape_desc* d = ADD_("capsule"); d->shape = YGFX_CAPSULE; d->w = 160; d->shape_p[0] = 24; d->shape_p[1] = 9; }
    { ygfx_shape_desc* d = ADD_("arc"); d->shape = YGFX_ARC; d->w = 150; d->shape_p[0] = 250; d->shape_p[1] = 16; d->shape_p[2] = 30; }
    { ygfx_shape_desc* d = ADD_("arc trimmed"); d->shape = YGFX_ARC; d->w = 150; d->shape_p[0] = 300; d->shape_p[1] = 16; d->trim[0] = 0.2f; d->trim[1] = 0.7f; d->cap = YGFX_CAP_ROUND; }
    { ygfx_shape_desc* d = ADD_("pie 120"); d->shape = YGFX_PIE; d->w = 150; d->shape_p[0] = 120; d->shape_p[2] = -60; }
    { ygfx_shape_desc* d = ADD_("pie 300"); d->shape = YGFX_PIE; d->w = 150; d->shape_p[0] = 300; d->shape_p[2] = 10; }
    { ygfx_shape_desc* d = ADD_("ngon 5"); d->shape = YGFX_NGON; d->w = 150; d->shape_p[0] = 5; }
    { ygfx_shape_desc* d = ADD_("ngon 6 round"); d->shape = YGFX_NGON; d->w = 150; d->shape_p[0] = 6; d->shape_p[1] = 14; d->dash[0] = 15; d->dash[1] = 8; d->dash_snap = true; d->stroke = 4; }
    { ygfx_shape_desc* d = ADD_("star 5"); d->shape = YGFX_STAR; d->w = 160; d->shape_p[0] = 5; d->shape_p[1] = 0.45f; }
    { ygfx_shape_desc* d = ADD_("star 7 round"); d->shape = YGFX_STAR; d->w = 160; d->shape_p[0] = 7; d->shape_p[1] = 0.5f; d->shape_p[2] = 5; }
    { ygfx_shape_desc* d = ADD_("ellipse 2:1"); d->shape = YGFX_ELLIPSE; d->w = 180; d->h = 90; d->dash[0] = 16; d->dash[1] = 8; d->stroke = 4; }
    { ygfx_shape_desc* d = ADD_("ellipse 10:1"); d->shape = YGFX_ELLIPSE; d->w = 200; d->h = 20; }
    { ygfx_shape_desc* d = ADD_("qbezier"); d->shape = YGFX_QBEZIER; d->path = qb; d->n_path = 3; d->shape_p[0] = 10; d->cap = YGFX_CAP_BUTT; d->dash[0] = 20; d->dash[1] = 6; }
    { ygfx_shape_desc* d = ADD_("polyline miter"); d->shape = YGFX_POLYLINE; d->path = zig; d->n_path = 4; d->shape_p[0] = 12; d->join = YGFX_JOIN_MITER; d->cap = YGFX_CAP_SQUARE; }
    { ygfx_shape_desc* d = ADD_("polyline round"); d->shape = YGFX_POLYLINE; d->path = zig; d->n_path = 4; d->shape_p[0] = 12; d->cap = YGFX_CAP_ROUND; d->trim[0] = 0.1f; d->trim[1] = 0.8f; }
    { ygfx_shape_desc* d = ADD_("polyline bevel"); d->shape = YGFX_POLYLINE; d->path = zig; d->n_path = 4; d->shape_p[0] = 12; d->join = YGFX_JOIN_BEVEL; }
    { ygfx_shape_desc* d = ADD_("polygon concave fillet"); d->shape = YGFX_POLYGON; d->vertices = conc; d->shape_p[0] = 5; d->shape_p[1] = 8; }
    { ygfx_shape_desc* d = ADD_("polygon triangle fillet"); d->shape = YGFX_POLYGON; d->vertices = tri; d->shape_p[0] = 3; d->shape_p[1] = 12; d->dash[0] = 25; d->dash[1] = 10; d->stroke = 5; }
#undef ADD_
    for (k = 0; k < nb; k++)
        for (e = 0; e < 3; e++)
            for (o = 0; o < 2; o++)
                for (st2 = 0; st2 < 2; st2++) {
                    ygfx_shape_desc d = base[k];
                    ygfx_stim s;
                    char what[96];
                    if (st2 && d.stroke > 0) continue;
                    if (st2 && (d.shape == YGFX_POLYLINE || d.shape == YGFX_QBEZIER) && d.trim[1] > 0) continue;
                    d.edge = (ygfx_edge)e; d.edge_width = e == YGFX_EDGE_COSINE ? 4.0f : 1.5f;
                    d.ori = o ? 25.0f : 0.0f; d.x = 3.25f; d.y = -2.5f;
                    if (st2) { d.stroke = 4; d.stroke_align = YGFX_STROKE_CENTER; d.dash[0] = d.dash[0]; }
                    d.color[0] = d.color[1] = d.color[2] = 1.0f;
                    s = ygfx_shape(&d);
                    snprintf(what, sizeof what, "%s e%d o%d s%d", names[k], e, o, st2);
                    vec_check(&s, what);
                }
    {   /* compounds: each operation, onion, a smooth blend */
        static ygfx_prim pr[3];
        static const int ops[7] = { YGFX_OP_UNION, YGFX_OP_INTERSECT, YGFX_OP_SUBTRACT, YGFX_OP_XOR,
                                    YGFX_OP_SMOOTH_UNION, YGFX_OP_SMOOTH_INTERSECT, YGFX_OP_SMOOTH_SUBTRACT };
        ygfx_compound_desc cd;
        ygfx_stim s;
        char what[64];
        for (k = 0; k < 7; k++)
            for (e = 0; e < 3; e++) {
                memset(pr, 0, sizeof pr);
                pr[0].shape = YGFX_CIRCLE; pr[0].x = -30; pr[0].w = 110;
                pr[1].shape = YGFX_RRECT; pr[1].x = 35; pr[1].y = 10; pr[1].w = 100; pr[1].h = 60; pr[1].ori = 20;
                pr[1].p[0] = pr[1].p[2] = 10; pr[1].op = (uint8_t)ops[k]; pr[1].k = 25;
                pr[2].shape = YGFX_STAR; pr[2].y = -50; pr[2].w = 60; pr[2].p[0] = 5; pr[2].p[1] = 0.5f; pr[2].op = YGFX_OP_UNION;
                pr[2].onion = k == 3 ? 4.0f : 0.0f;
                memset(&cd, 0, sizeof cd);
                cd.prims = pr; cd.n = 3; cd.w = 200; cd.h = 160; cd.ori = e == 2 ? 15.0f : 0.0f; cd.x = 1.5f;
                cd.edge = (ygfx_edge)e; cd.edge_width = e == YGFX_EDGE_COSINE ? 4.0f : 1.5f;
                cd.color[0] = cd.color[1] = cd.color[2] = 1;
                if (k == 6) cd.stroke = 3;
                s = ygfx_compound(&cd);
                snprintf(what, sizeof what, "compound op%d e%d", ops[k], e);
                vec_check(&s, what);
            }
    }
    ygfx_close(&R->g);
}

/* --- brute force: a boundary as pieces (segments and arcs), in local px */

typedef struct piece { int arc; double ax, ay, bx, by, cx, cy, r, t0, dt; } piece;
static piece PC[8192];
static int NPC;
static double PX_[70000], PY_[70000];   /* the boundary as a polygon, for inside */
static int NPX;
static int PC_OPEN;                     /* an open curve: distance minus PC_HW, no sign test */
static double PC_HW;

static void pc_reset(void) { NPC = 0; NPX = 0; PC_OPEN = 0; PC_HW = 0; }
static void pc_seg(double ax, double ay, double bx, double by) {
    piece* p = &PC[NPC++];
    memset(p, 0, sizeof *p);
    p->ax = ax; p->ay = ay; p->bx = bx; p->by = by;
    PX_[NPX] = ax; PY_[NPX++] = ay;
}
static void pc_arc(double cx, double cy, double r, double t0, double dt) {
    piece* p = &PC[NPC++];
    int k;
    memset(p, 0, sizeof *p);
    p->arc = 1; p->cx = cx; p->cy = cy; p->r = r; p->t0 = t0; p->dt = dt;
    for (k = 0; k < 256; k++) { double t = t0 + dt * k / 256.0; PX_[NPX] = cx + r * cos(t); PY_[NPX++] = cy + r * sin(t); }
}
static double piece_dist(const piece* p, double x, double y) {
    if (!p->arc) return seg_dist(x, y, p->ax, p->ay, p->bx, p->by);
    {
        double a = atan2(y - p->cy, x - p->cx), rel = a - p->t0, tw = 2 * 3.14159265358979323846;
        if (p->dt < 0) rel = -rel;
        rel -= tw * floor(rel / tw);
        if (rel <= fabs(p->dt)) return fabs(sqrt((x - p->cx) * (x - p->cx) + (y - p->cy) * (y - p->cy)) - p->r);
        {
            double e0x = p->cx + p->r * cos(p->t0), e0y = p->cy + p->r * sin(p->t0);
            double e1x = p->cx + p->r * cos(p->t0 + p->dt), e1y = p->cy + p->r * sin(p->t0 + p->dt);
            return fmin(sqrt((x - e0x) * (x - e0x) + (y - e0y) * (y - e0y)), sqrt((x - e1x) * (x - e1x) + (y - e1y) * (y - e1y)));
        }
    }
}
/* Boxes of 32 pieces and of 32 polygon edges, so that a pixel skips what
 * cannot be nearer than its best so far, or cannot cross its row: the
 * same minimum and the same crossings as the loop over everything. */
enum { PC_CH = 32 };
static double PCB[8192 / PC_CH + 1][4], PXB[70000 / PC_CH + 1][2];
static void pc_prep(void) {
    int c, k;
    for (c = 0; c * PC_CH < NPC; c++) {
        double* b = PCB[c];
        b[0] = b[1] = 1e300; b[2] = b[3] = -1e300;
        for (k = c * PC_CH; k < NPC && k < (c + 1) * PC_CH; k++) {
            const piece* p = &PC[k];
            double lx = p->arc ? p->cx - p->r : fmin(p->ax, p->bx), hx = p->arc ? p->cx + p->r : fmax(p->ax, p->bx);
            double ly = p->arc ? p->cy - p->r : fmin(p->ay, p->by), hy = p->arc ? p->cy + p->r : fmax(p->ay, p->by);
            b[0] = fmin(b[0], lx); b[1] = fmin(b[1], ly); b[2] = fmax(b[2], hx); b[3] = fmax(b[3], hy);
        }
    }
    for (c = 0; c * PC_CH < NPX; c++) {   /* edge k runs from point k - 1 */
        PXB[c][0] = 1e300; PXB[c][1] = -1e300;
        for (k = c * PC_CH - 1; k < NPX && k < (c + 1) * PC_CH; k++) {
            int q = k < 0 ? NPX - 1 : k;
            PXB[c][0] = fmin(PXB[c][0], PY_[q]); PXB[c][1] = fmax(PXB[c][1], PY_[q]);
        }
    }
}
static double pc_dist_lim(double x, double y, double lim) {
    /* past cap the value is out of the caller's reach: any value past it will do */
    double cap = (PC_OPEN ? lim + PC_HW : lim) + 1.0, d = cap;
    int k, in = 0, j, c;
    for (c = 0; c * PC_CH < NPC; c++) {
        const double* b = PCB[c];
        double ex = fmax(fmax(b[0] - x, x - b[2]), 0.0), ey = fmax(fmax(b[1] - y, y - b[3]), 0.0);
        if (ex * ex + ey * ey >= d * d) continue;
        for (k = c * PC_CH; k < NPC && k < (c + 1) * PC_CH; k++) d = fmin(d, piece_dist(&PC[k], x, y));
    }
    if (PC_OPEN) return d - PC_HW;
    if (d > lim) return d;   /* out of the caller's reach: the sign does not matter */
    for (c = 0; c * PC_CH < NPX; c++) {
        if (y < PXB[c][0] || y >= PXB[c][1]) continue;   /* a crossing needs a point above and one not */
        for (k = c * PC_CH; k < NPX && k < (c + 1) * PC_CH; k++) {
            j = k ? k - 1 : NPX - 1;
            if ((PY_[k] > y) != (PY_[j] > y) && x < (PX_[j] - PX_[k]) * (y - PY_[k]) / (PY_[j] - PY_[k]) + PX_[k]) in = !in;
        }
    }
    return in ? -d : d;
}

/* A closed polygon with every corner rounded to radius r (a fillet tangent
 * to both edges, convex or concave corner alike), written apart from the
 * header's. */
static void pc_fillet(const double* v, int n, double r) {
    int i;
    double pin[64][2], pout[64][2], c[64][2], t0[64], dt[64];
    for (i = 0; i < n; i++) {
        int ip = (i + n - 1) % n, in = (i + 1) % n;
        double u1x = v[2 * i] - v[2 * ip], u1y = v[2 * i + 1] - v[2 * ip + 1], u2x = v[2 * in] - v[2 * i], u2y = v[2 * in + 1] - v[2 * i + 1];
        double l1 = sqrt(u1x * u1x + u1y * u1y), l2 = sqrt(u2x * u2x + u2y * u2y), dl, td, bx, by, bl, a0, a1, d;
        u1x /= l1; u1y /= l1; u2x /= l2; u2y /= l2;
        dl = acos(-(u1x * u2x + u1y * u2y));
        td = r / tan(0.5 * dl);
        pin[i][0] = v[2 * i] - u1x * td; pin[i][1] = v[2 * i + 1] - u1y * td;
        pout[i][0] = v[2 * i] + u2x * td; pout[i][1] = v[2 * i + 1] + u2y * td;
        bx = u2x - u1x; by = u2y - u1y; bl = sqrt(bx * bx + by * by);
        c[i][0] = v[2 * i] + bx / bl * r / sin(0.5 * dl); c[i][1] = v[2 * i + 1] + by / bl * r / sin(0.5 * dl);
        a0 = atan2(pin[i][1] - c[i][1], pin[i][0] - c[i][0]); a1 = atan2(pout[i][1] - c[i][1], pout[i][0] - c[i][0]);
        d = a1 - a0;
        while (d > 3.14159265358979323846) d -= 2 * 3.14159265358979323846;
        while (d < -3.14159265358979323846) d += 2 * 3.14159265358979323846;
        t0[i] = a0; dt[i] = d;
    }
    for (i = 0; i < n; i++) {
        int in = (i + 1) % n;
        pc_arc(c[i][0], c[i][1], r, t0[i], dt[i]);
        pc_seg(pout[i][0], pout[i][1], pin[in][0], pin[in][1]);
    }
}

static void pc_poly(const double* v, int n) {
    int i;
    for (i = 0; i < n; i++) pc_seg(v[2 * i], v[2 * i + 1], v[2 * ((i + 1) % n)], v[2 * ((i + 1) % n) + 1]);
}

typedef struct stats5 {
    double t2_cov, t2_dist, t2_qb_cov, t2_qb_dist, t3_union, t3_union_pred, t3_smooth, t3_connect, t3_poly, t4_exact, t4_fd, fx, blend;
    double dash_period, paint_rgb, paint_oklab, paint_dkl, paint_vertex, msdf, glyph, batch;
    long   dash_count_bad, trim_bad, refused_ok, refused_n;
    char   t2_worst[48];
} stats5;
static stats5 S5;

/* GPU coverage against F(brute-force Euclidean d), pixels whose distance is
 * within reach of the edge; *dist gets the CPU form's distance error. */
typedef struct tc_ctx { const float* blk; const ygfx_stim* s; double sg; double* db; double* f; } tc_ctx;
static void tc_row(void* p, int j) {
    const tc_ctx* c = (const tc_ctx*)p;
    int i;
    for (i = 0; i < W; i++) {
        float lx, ly;
        double f0[4], f1[4];
        ygfx_local(&R->g, c->s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
        c->db[j * W + i] = pc_dist_lim(lx, ly, 6 * c->sg);
        if (c->f && fabs(c->db[j * W + i]) <= 6 * c->sg) { ygfx__vfield(c->blk, lx, ly, f0, f1); c->f[j * W + i] = f0[0]; }
    }
}
static double truth_check(const ygfx_stim* s, double sg, double* dist) {
    static float blk[16 * 64];
    static double dbs[W * H], fs[W * H];
    double ext[2], worst = 0.0;
    const char* err;
    int clip, i, j;
    tc_ctx tc;
    if (ygfx__vpack(&R->g, s, blk, ext, &clip, &err) < 0) { fprintf(stderr, "truth: %s\n", err); g_failures++; return 1; }
    gl_frame(s, 1);
    tc.blk = blk; tc.s = s; tc.sg = sg; tc.db = dbs; tc.f = dist ? fs : NULL;
    pc_prep();
    par_rows(tc_row, &tc, H);
    for (j = 0; j < H; j++)
        for (i = 0; i < W; i++) {
            double db = dbs[j * W + i];
            if (fabs(db) > 6 * sg) continue;
            worst = maxd(worst, fabs(R->scene[(j * W + i) * 4] - cov(YGFX_EDGE_GAUSSIAN, sg, db)));
            if (dist) *dist = maxd(*dist, fabs(fs[j * W + i] - db));
        }
    return worst;
}

/* The signed distance to a sampled zero set, exact within 13 px (the
 * check uses 12), 13 or more past it; 1e300 where the field is far. The
 * points sit in 2 px cells, so a pixel looks only at the cells within 13 px. */
enum { ZS_GX = 94, ZS_GY = 64 };
typedef struct zs_ctx {
    const float* blk; const ygfx_stim* s; const double *zx, *zy; int nz; double* best;
    int start[ZS_GX * ZS_GY + 1], idx[60000];
} zs_ctx;
static int zs_cx(double x) { int c = (int)floor((x + 94.0) * 0.5); return c < 0 ? 0 : (c > ZS_GX - 1 ? ZS_GX - 1 : c); }
static int zs_cy(double y) { int c = (int)floor((y + 64.0) * 0.5); return c < 0 ? 0 : (c > ZS_GY - 1 ? ZS_GY - 1 : c); }
static void zs_grid(zs_ctx* c) {
    static int fill[ZS_GX * ZS_GY];
    int z, k;
    memset(c->start, 0, sizeof c->start);
    for (z = 0; z < c->nz; z++) c->start[zs_cy(c->zy[z]) * ZS_GX + zs_cx(c->zx[z]) + 1]++;
    for (k = 0; k < ZS_GX * ZS_GY; k++) c->start[k + 1] += c->start[k];
    memcpy(fill, c->start, sizeof fill);
    for (z = 0; z < c->nz; z++) c->idx[fill[zs_cy(c->zy[z]) * ZS_GX + zs_cx(c->zx[z])]++] = z;
}
static void zs_row(void* p, int j) {
    const zs_ctx* c = (const zs_ctx*)p;
    int i, z, cx, cy;
    for (i = 0; i < W; i++) {
        float lx, ly;
        double f0[4], f1[4], best = 13.0 * 13.0;
        c->best[j * W + i] = 1e300;
        ygfx_local(&R->g, c->s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
        ygfx__vfield(c->blk, lx, ly, f0, f1);
        if (fabs(f0[0]) > 14) continue;
        for (cy = zs_cy(ly - 13.0); cy <= zs_cy(ly + 13.0); cy++)
            for (cx = zs_cx(lx - 13.0); cx <= zs_cx(lx + 13.0); cx++)
                for (z = c->start[cy * ZS_GX + cx]; z < c->start[cy * ZS_GX + cx + 1]; z++) {
                    int q = c->idx[z];
                    best = fmin(best, (lx - c->zx[q]) * (lx - c->zx[q]) + (ly - c->zy[q]) * (ly - c->zy[q]));
                }
        c->best[j * W + i] = sqrt(best) * (f0[0] > 0 ? 1 : -1);
    }
}

static void gl_v03_truth(stats* st) {
    const double PI_ = 3.14159265358979323846;
    ygfx_shape_desc d;
    ygfx_stim s;
    int o, k;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (o = 0; o < 2; o++)
        for (k = 0; k < 12; k++) {
            const char* nm = "";
            double w;
            memset(&d, 0, sizeof d);
            d.edge = YGFX_EDGE_GAUSSIAN; d.edge_width = 1.5f; d.ori = o ? 25.0f : 0.0f; d.x = 3.25f; d.y = -2.5f;
            d.color[0] = d.color[1] = d.color[2] = 1;
            pc_reset();
            switch (k) {
            case 0:
                nm = "rrect"; d.shape = YGFX_RRECT; d.w = 120; d.h = 80; d.shape_p[0] = 5; d.shape_p[1] = 20; d.shape_p[3] = 35;
                pc_seg(-55, -40, 40, -40); pc_arc(40, -20, 20, -PI_ / 2, PI_ / 2); pc_seg(60, -20, 60, 40);
                pc_seg(60, 40, -25, 40); pc_arc(-25, 5, 35, PI_ / 2, PI_ / 2); pc_seg(-60, 5, -60, -35);
                pc_arc(-55, -35, 5, PI_, PI_ / 2);
                break;
            case 1: {
                double x1 = -56, x2 = 71, D = 127, sn = 15.0 / 127.0, cs = sqrt(1 - sn * sn), ph = asin(sn);
                nm = "capsule"; d.shape = YGFX_CAPSULE; d.w = 160; d.shape_p[0] = 24; d.shape_p[1] = 9;
                pc_arc(x1, 0, 24, PI_ / 2 - ph, PI_ + 2 * ph);
                pc_seg(x1 + 24 * sn, -24 * cs, x2 + 9 * sn, -9 * cs);
                pc_arc(x2, 0, 9, -PI_ / 2 + ph, PI_ - 2 * ph);
                pc_seg(x2 + 9 * sn, 9 * cs, x1 + 24 * sn, 24 * cs);
                (void)D;
                break;
            }
            case 2: {
                double a0 = 30 * PI_ / 180, a1 = 280 * PI_ / 180;
                nm = "arc"; d.shape = YGFX_ARC; d.w = 150; d.shape_p[0] = 250; d.shape_p[1] = 16; d.shape_p[2] = 30;
                pc_arc(0, 0, 75, a0, a1 - a0); pc_arc(67 * cos(a1), 67 * sin(a1), 8, a1, PI_);
                pc_arc(0, 0, 59, a1, a0 - a1); pc_arc(67 * cos(a0), 67 * sin(a0), 8, a0 + PI_, PI_);
                break;
            }
            case 3: case 4: {
                double sw = (k == 3 ? 120 : 300) * PI_ / 180, a0 = (k == 3 ? -60 : 10) * PI_ / 180;
                nm = k == 3 ? "pie 120" : "pie 300"; d.shape = YGFX_PIE; d.w = 150; d.shape_p[0] = k == 3 ? 120.0f : 300.0f;
                d.shape_p[2] = k == 3 ? -60.0f : 10.0f;
                pc_seg(0, 0, 75 * cos(a0), 75 * sin(a0)); pc_arc(0, 0, 75, a0, sw); pc_seg(75 * cos(a0 + sw), 75 * sin(a0 + sw), 0, 0);
                break;
            }
            case 5: {   /* a rounded hexagon: the inset polygon dilated, apart from the fillet code */
                double an = PI_ / 6, Ri = 75 - 14 / cos(an), v[12], nx, ny;
                int q;
                nm = "ngon 6 round"; d.shape = YGFX_NGON; d.w = 150; d.shape_p[0] = 6; d.shape_p[1] = 14;
                for (q = 0; q < 6; q++) { double a = an + 2 * an * q; v[2 * q] = Ri * sin(a); v[2 * q + 1] = Ri * cos(a); }
                for (q = 0; q < 6; q++) {
                    int qn = (q + 1) % 6;
                    double mx = 0.5 * (v[2 * q] + v[2 * qn]), my = 0.5 * (v[2 * q + 1] + v[2 * qn + 1]), ml = sqrt(mx * mx + my * my);
                    double nqx, nqy, pmx, pmy, pl, a0, a1, da;
                    int qp = (q + 5) % 6;
                    nx = mx / ml; ny = my / ml;
                    pc_seg(v[2 * q] + 14 * nx, v[2 * q + 1] + 14 * ny, v[2 * qn] + 14 * nx, v[2 * qn + 1] + 14 * ny);
                    pmx = 0.5 * (v[2 * qn] + v[2 * ((qn + 1) % 6)]); pmy = 0.5 * (v[2 * qn + 1] + v[2 * ((qn + 1) % 6) + 1]);
                    pl = sqrt(pmx * pmx + pmy * pmy); nqx = pmx / pl; nqy = pmy / pl;
                    a0 = atan2(ny, nx); a1 = atan2(nqy, nqx); da = a1 - a0;
                    while (da > PI_) da -= 2 * PI_;
                    while (da < -PI_) da += 2 * PI_;
                    pc_arc(v[2 * qn], v[2 * qn + 1], 14, a0, da);
                    (void)qp;
                }
                break;
            }
            case 6: case 7: {
                int n = k == 6 ? 5 : 7, q;
                double an = PI_ / n, rho = k == 6 ? 0.45 : 0.5, v[28];
                nm = k == 6 ? "star 5" : "star 7 round"; d.shape = YGFX_STAR; d.w = 160; d.shape_p[0] = (float)n; d.shape_p[1] = (float)rho;
                if (k == 7) d.shape_p[2] = 5;
                for (q = 0; q < n; q++) {
                    double ai = 2 * an * q, at = an + 2 * an * q;
                    v[4 * q] = 80 * rho * sin(ai); v[4 * q + 1] = 80 * rho * cos(ai);
                    v[4 * q + 2] = 80 * sin(at); v[4 * q + 3] = 80 * cos(at);
                }
                if (k == 6) pc_poly(v, 2 * n); else pc_fillet(v, 2 * n, 5);
                break;
            }
            case 8: case 9: {
                double a = 90, b = k == 8 ? 45 : 10;
                int q;
                nm = k == 8 ? "ellipse 2:1" : "ellipse 9:1"; d.shape = YGFX_ELLIPSE; d.w = (float)(2 * a); d.h = (float)(2 * b);
                for (q = 0; q < 8192; q++) {
                    double t0 = 2 * PI_ * q / 8192, t1 = 2 * PI_ * (q + 1) / 8192;
                    pc_seg(a * cos(t0), b * sin(t0), a * cos(t1), b * sin(t1));
                }
                break;
            }
            case 10: {
                static const float conc[10] = { -60, -50, 60, -50, 60, 50, 0, 0, -60, 50 };
                double v[10];
                int q;
                nm = "polygon fillet"; d.shape = YGFX_POLYGON; d.vertices = conc; d.shape_p[0] = 5; d.shape_p[1] = 8;
                for (q = 0; q < 10; q++) v[q] = conc[q];
                pc_fillet(v, 5, 8);
                break;
            }
            case 11: {
                static const float qb[6] = { -70, 40, 0, -80, 70, 40 };
                int q;
                nm = "qbezier"; d.shape = YGFX_QBEZIER; d.path = qb; d.n_path = 3; d.shape_p[0] = 10; d.cap = YGFX_CAP_ROUND;
                for (q = 0; q < 8192; q++) {
                    double t0 = q / 8192.0, t1 = (q + 1) / 8192.0;
                    pc_seg((1 - t0) * (1 - t0) * -70 + 2 * (1 - t0) * t0 * 0 + t0 * t0 * 70, (1 - t0) * (1 - t0) * 40 + 2 * (1 - t0) * t0 * -80 + t0 * t0 * 40,
                           (1 - t1) * (1 - t1) * -70 + 2 * (1 - t1) * t1 * 0 + t1 * t1 * 70, (1 - t1) * (1 - t1) * 40 + 2 * (1 - t1) * t1 * -80 + t1 * t1 * 40);
                }
                PC_OPEN = 1; PC_HW = 5;
                break;
            }
            }
            s = ygfx_shape(&d);
            if (k == 11) {   /* the Bezier: chords within 0.005 px of the curve */
                S5.t2_qb_cov = maxd(S5.t2_qb_cov, truth_check(&s, 1.5, &S5.t2_qb_dist));
                continue;
            }
            w = truth_check(&s, 1.5, &S5.t2_dist);
            if (w > S5.t2_cov) { S5.t2_cov = w; snprintf(S5.t2_worst, sizeof S5.t2_worst, "%s o%d", nm, o); }
        }
    /* T3: a union's reflex crease (an L of two rects), against the L's
     * Euclidean distance */
    {
        static ygfx_prim pr[2];
        ygfx_compound_desc cd;
        static const double L[12] = { -60, -20, 20, -20, 20, -80, 60, -80, 60, 20, -60, 20 };
        int i, j;
        memset(pr, 0, sizeof pr);
        pr[0].shape = YGFX_RECT; pr[0].x = 0; pr[0].y = 0; pr[0].w = 120; pr[0].h = 40;
        pr[1].shape = YGFX_RECT; pr[1].x = 40; pr[1].y = -30; pr[1].w = 40; pr[1].h = 100; pr[1].op = YGFX_OP_UNION;
        memset(&cd, 0, sizeof cd);
        cd.prims = pr; cd.n = 2; cd.w = 120; cd.h = 100; cd.y = 25;
        cd.edge = YGFX_EDGE_GAUSSIAN; cd.edge_width = 2; cd.color[0] = cd.color[1] = cd.color[2] = 1;
        s = ygfx_compound(&cd);
        pc_reset();
        pc_poly(L, 6);
        S5.t3_union = truth_check(&s, 2.0, NULL);
        S5.t3_union_pred = 0;
        for (j = 0; j <= 400; j++)   /* inside a reflex right angle: min of the two lines against the corner */
            for (i = 0; i <= 400; i++) {
                double x = i * 0.02, y = j * 0.02;
                S5.t3_union_pred = maxd(S5.t3_union_pred, cov(YGFX_EDGE_GAUSSIAN, 2, -sqrt(x * x + y * y)) - cov(YGFX_EDGE_GAUSSIAN, 2, -(x > y ? x : y)));
            }
    }
    /* T3: smooth unions, gradient-normalized, against the distance to the
     * blended shape's own boundary (its zero set, sampled at 0.05 px) */
    {
        static ygfx_prim pr[2];
        static float blk[16 * 64];
        static double zx[60000], zy[60000], zbest[W * H];
        ygfx_compound_desc cd;
        int q;
        for (q = 0; q < 2; q++) {
            double ext[2], D = q ? 90.0 : 80.0, worst = 0;
            const char* err;
            int clip, nz = 0, i, j;
            memset(pr, 0, sizeof pr);
            pr[0].shape = YGFX_CIRCLE; pr[0].x = (float)(-D / 2); pr[0].w = 80;
            pr[1].shape = YGFX_CIRCLE; pr[1].x = (float)(D / 2); pr[1].w = 80; pr[1].op = YGFX_OP_SMOOTH_UNION; pr[1].k = 20;
            memset(&cd, 0, sizeof cd);
            cd.prims = pr; cd.n = 2; cd.w = 200; cd.h = 100;
            cd.edge = YGFX_EDGE_GAUSSIAN; cd.edge_width = 2; cd.color[0] = cd.color[1] = cd.color[2] = 1;
            s = ygfx_compound(&cd);
            ygfx__vpack(&R->g, &s, blk, ext, &clip, &err);
            for (j = 0; j < 2400 && nz < 59000; j++) {   /* sign changes along rows, 0.05 px */
                double y = -60 + j * 0.05, prev = 0, f0[4], f1[4];
                for (i = 0; i < 3600; i++) {
                    double x = -90 + i * 0.05;
                    ygfx__vfield(blk, x, y, f0, f1);
                    if (i && (f0[0] > 0) != (prev > 0) && nz < 59000) { zx[nz] = x - 0.05 * f0[0] / (f0[0] - prev); zy[nz++] = y; }
                    prev = f0[0];
                }
            }
            for (i = 0; i < 3600 && nz < 59000; i++) {   /* and along columns */
                double x = -90 + i * 0.05, prev = 0, f0[4], f1[4];
                for (j = 0; j < 2400; j++) {
                    double y = -60 + j * 0.05;
                    ygfx__vfield(blk, x, y, f0, f1);
                    if (j && (f0[0] > 0) != (prev > 0) && nz < 59000) { zy[nz] = y - 0.05 * f0[0] / (f0[0] - prev); zx[nz++] = x; }
                    prev = f0[0];
                }
            }
            gl_frame(&s, 1);
            {
                static zs_ctx zc;
                zc.blk = blk; zc.s = &s; zc.zx = zx; zc.zy = zy; zc.nz = nz; zc.best = zbest;
                zs_grid(&zc);
                par_rows(zs_row, &zc, H);
            }
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    double best = zbest[j * W + i];
                    if (fabs(best) > 12) continue;
                    worst = maxd(worst, fabs(R->scene[(j * W + i) * 4] - cov(YGFX_EDGE_GAUSSIAN, 2, best)));
                }
            if (q) S5.t3_connect = worst; else S5.t3_smooth = worst;
        }
    }
    ygfx_close(&R->g);
}

/* The shader's main with every effect, in double: premultiplied rgba. */
static void fx_ref(const float* b, const ygfx_stim* s, int i, int j, double out[4]) {
    float lx, ly;
    double f0[4], f1[4], d, d1, hw = b[15], c = b[11], cm, w = b[22], acc[4], col[3];
    int prof = (int)b[21], fl = (int)b[23], k;
    const float* vd = b + 36;
    ygfx_local(&R->g, s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
    ygfx__vfield(b, lx, ly, f0, f1);
    d = f0[0] - b[10]; d1 = f1[0] - b[10];
    cm = hw > 0 ? cov(prof, w, d - c - hw) - cov(prof, w, d - c + hw) : cov(prof, w, d);
    for (k = 0; k < 3; k++) col[k] = b[12 + k];
    for (k = 0; k < 3; k++) acc[k] = col[k] * cm;
    acc[3] = cm;
    if (fl & 32) {
        int o = (int)b[32];
        double r[4] = { 0, 0, 0, 0 }, t[4], a;
        const float* Dp = vd + 4 * o;
#define OVER_(T) do { int q_; for (q_ = 0; q_ < 4; q_++) r[q_] = (T)[q_] + r[q_] * (1 - (T)[3]); } while (0)
#define GAUSS_(x, sg) ((sg) <= 0 ? ((x) <= 0 ? 1.0 : 0.0) : 0.5 * erfc((x) / ((sg) * sqrt(2.0))))
        if (Dp[2] > 0) {
            if (fl & 64) {
                double hx = vd[8] + Dp[1], hy = vd[9] + Dp[1], sg = Dp[0], px = lx - b[28], py = ly - b[29];
                a = (0.5 * erfc(-(hx - px) / (sg * sqrt(2.0))) - 0.5 * erfc(-(-hx - px) / (sg * sqrt(2.0)))) *
                    (0.5 * erfc(-(hy - py) / (sg * sqrt(2.0))) - 0.5 * erfc(-(-hy - py) / (sg * sqrt(2.0))));
            } else a = GAUSS_(d1 - Dp[1], Dp[0]);
            a *= Dp[2];
            for (k = 0; k < 3; k++) r[k] = Dp[4 + k] * a;
            r[3] = a;
        }
        if (Dp[10] > 0) { a = GAUSS_(d - Dp[9], Dp[8]) * Dp[10]; for (k = 0; k < 3; k++) t[k] = Dp[12 + k] * a; t[3] = a; OVER_(t); }
        OVER_(acc);
        if (Dp[18] > 0) { a = cov(prof, w, d) * GAUSS_(-(d1 + Dp[17]), Dp[16]) * Dp[18]; for (k = 0; k < 3; k++) t[k] = Dp[20 + k] * a; t[3] = a; OVER_(t); }
        for (k = 0; k < 2; k++) {
            const float* B = Dp + 24 + 8 * k;
            if (B[2] > 0) { int q; a = (cov(prof, w, d - B[1]) - cov(prof, w, d - B[0])) * B[2]; for (q = 0; q < 3; q++) t[q] = B[4 + q] * a; t[3] = a; OVER_(t); }
        }
        for (k = 0; k < 4; k++) acc[k] = r[k];
#undef OVER_
#undef GAUSS_
    }
    for (k = 0; k < 4; k++) out[k] = acc[k] * b[8] * b[9];
}

static void gl_v03_fx(stats* st) {
    static float blk[16 * 64];
    static float a1[W * H * 4];
    ygfx_shape_desc d;
    ygfx_fx fx;
    ygfx_stim s;
    double ext[2];
    const char* err;
    int clip, i, j, k;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    /* T6: every effect in one pass against the same layers on the CPU */
    memset(&fx, 0, sizeof fx);
    fx.dx = 7; fx.dy = 5;
    fx.drop.sigma = 4; fx.drop.spread = 1; fx.drop.opacity = 0.6f; fx.drop.color[2] = 0.3f;
    fx.glow.sigma = 3; fx.glow.spread = 2; fx.glow.opacity = 0.5f; fx.glow.color[0] = 0.9f; fx.glow.color[1] = 0.7f;
    fx.inner.sigma = 2; fx.inner.opacity = 0.7f;
    fx.band[0].a1 = -2; fx.band[0].a2 = 0; fx.band[0].opacity = 1; fx.band[0].color[0] = fx.band[0].color[1] = fx.band[0].color[2] = 1;
    fx.band[1].a1 = 4; fx.band[1].a2 = 6; fx.band[1].opacity = 0.5f; fx.band[1].color[1] = 1;
    memset(&d, 0, sizeof d);
    d.shape = YGFX_RRECT; d.w = 140; d.h = 90; d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 16;
    d.color[0] = 0.3f; d.color[1] = 0.35f; d.color[2] = 0.4f; d.opacity = 0.9f;
    d.edge = YGFX_EDGE_COSINE; d.edge_width = 2; d.fx = &fx;
    for (k = 0; k < 2; k++) {
        d.ori = k ? 20.0f : 0.0f;
        s = ygfx_shape(&d);
        ygfx__vpack(&R->g, &s, blk, ext, &clip, &err);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                double want[4];
                int c;
                fx_ref(blk, &s, i, j, want);
                for (c = 0; c < 3; c++) S5.fx = maxd(S5.fx, fabs(R->scene[(j * W + i) * 4 + c] - want[c]));
            }
    }
    /* T4: the exact blur of a sharp rect, and F(d)'s shadow against it */
    for (k = 0; k < 2; k++) {
        memset(&fx, 0, sizeof fx);
        fx.dx = 40; fx.dy = 25; fx.drop.sigma = 5; fx.drop.opacity = 1; fx.drop.color[0] = fx.drop.color[1] = fx.drop.color[2] = 1;
        fx.flags = k ? 0u : YGFX_FX_EXACT_BLUR;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RECT; d.w = 100; d.h = 60; d.x = -30; d.y = -20; d.fx = &fx;   /* black, hard: it hides the shadow under it */
        s = ygfx_shape(&d);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                float lx, ly;
                double px, py, b;
                ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                if (fabs(lx) <= 51 && fabs(ly) <= 31) continue;   /* the fill */
                px = lx - 40; py = ly - 25;
                b = (Phi((50 - px) / 5) - Phi((-50 - px) / 5)) * (Phi((30 - py) / 5) - Phi((-30 - py) / 5));
                if (k) S5.t4_fd = maxd(S5.t4_fd, fabs(R->scene[(j * W + i) * 4] - b));
                else S5.t4_exact = maxd(S5.t4_exact, fabs(R->scene[(j * W + i) * 4] - b));
            }
    }
    ygfx_close(&R->g);
    /* T9: MULTIPLY and ADD on a gray background, against the exact forms */
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (k = 0; k < 2; k++) {
        static const float c3[3] = { 0.2f, 0.6f, 1.0f };
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RECT; d.w = 80; d.h = 60; d.opacity = 0.8f; memcpy(d.color, c3, sizeof c3);
        d.blend = k ? YGFX_BLEND_MODE_ADD : YGFX_BLEND_MODE_MULTIPLY;
        s = ygfx_shape(&d);
        gl_frame(&s, 1);
        for (i = 0; i < 3; i++) {
            double got = R->scene[(H / 2 * W + W / 2) * 4 + i];
            double want = k ? 0.5 + 0.8 * c3[i] : 0.5 * (1 - 0.8 + 0.8 * c3[i]);
            S5.blend = maxd(S5.blend, fabs(got - want));
        }
        S5.blend = maxd(S5.blend, fabs(R->scene[0] - 0.5));   /* outside: the background */
    }
    {   /* a modulation cannot take a blend */
        ygfx_gabor_desc gd;
        memset(&gd, 0, sizeof gd);
        gd.sigma = 10; gd.sf = 0.1f;
        s = ygfx_gabor(&gd);
        s.blend = YGFX_BLEND_MODE_MULTIPLY;
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        S5.refused_n++; S5.refused_ok += ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    /* T12: stimuli with extension blocks among batched v0.2 ones, against
     * one draw each */
    {
        static ygfx_stim many[40];
        static ygfx_prim pr[6];
        ygfx_compound_desc cd;
        for (k = 0; k < 40; k++) {
            memset(&d, 0, sizeof d);
            d.shape = (ygfx_shape_kind)(k % 3 == 0 ? YGFX_CIRCLE : YGFX_RECT); d.w = 20; d.h = 14;
            d.x = (float)(k % 10 * 28 - 130); d.y = (float)(k / 10 * 40 - 70); d.edge = YGFX_EDGE_COSINE; d.edge_width = 2;
            d.color[0] = 0.1f * (k % 7); d.color[1] = 0.5f; d.opacity = 0.7f;
            many[k] = ygfx_shape(&d);
        }
        memset(pr, 0, sizeof pr);
        for (k = 0; k < 6; k++) { pr[k].shape = YGFX_CIRCLE; pr[k].x = (float)(k * 20 - 50); pr[k].w = 26; pr[k].op = YGFX_OP_SMOOTH_UNION; pr[k].k = 8; }
        memset(&cd, 0, sizeof cd);
        cd.prims = pr; cd.n = 6; cd.w = 150; cd.h = 40; cd.color[0] = 1; cd.edge = YGFX_EDGE_GAUSSIAN; cd.edge_width = 1;
        many[13] = ygfx_compound(&cd);
        cd.y = 50; many[27] = ygfx_compound(&cd);
        gl_frame(many, 40);
        memcpy(a1, R->scene, sizeof a1);
        R->g.max_batch = 1;
        gl_frame(many, 40);
        R->g.max_batch = YGFX__BATCH;
        for (i = 0; i < W * H * 4; i++) S5.batch = maxd(S5.batch, fabs(a1[i] - R->scene[i]));
    }
    /* T7: a dash offset by one period draws the same; a snapped circle has
     * a whole number of dashes; trim keeps its stretch of the boundary */
    {
        float P;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_CIRCLE; d.w = 150; d.stroke = 4; d.dash[0] = 13; d.dash[1] = 7; d.dash_snap = true; d.color[0] = 1;
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.5f;
        s = ygfx_shape(&d);
        ygfx__vpack(&R->g, &s, blk, ext, &clip, &err);
        P = blk[16] + blk[17];
        gl_frame(&s, 1);
        memcpy(a1, R->scene, sizeof a1);
        s.dash_offset = P;
        gl_frame(&s, 1);
        for (i = 0; i < W * H * 4; i++) S5.dash_period = maxd(S5.dash_period, fabs(a1[i] - R->scene[i]));
        {   /* count dashes around the ring: rises of the red channel at r = 75 */
            int rises = 0, prev = -1, want = (int)floor(ygfx_length(&R->g, &s) / 20.0 + 0.5);
            for (k = 0; k < 4000; k++) {
                double a = 2 * 3.14159265358979323846 * k / 4000.0;
                int x = (int)floor(W / 2 + 75 * cos(a)), y = (int)floor(H / 2 + 75 * sin(a)), on = R->scene[(y * W + x) * 4] > 0.5;
                if (prev == 0 && on) rises++;
                prev = on;
            }
            S5.dash_count_bad += abs(rises - want) > 1;
            if (abs(rises - want) > 1) fprintf(stderr, "  dashes %d, want %d\n", rises, want);
        }
        d.dash[0] = 0; d.trim[0] = 0.0f; d.trim[1] = 0.5f; d.edge = YGFX_EDGE_HARD;
        s = ygfx_shape(&d);
        gl_frame(&s, 1);
        for (k = 0; k < 360; k++) {   /* clockwise from +x: the first half on, the second off */
            double a = (k + 0.5) * 3.14159265358979323846 / 180.0;
            int x = (int)floor(W / 2 + 75 * cos(a)), y = (int)floor(H / 2 + 75 * sin(a)), on = R->scene[(y * W + x) * 4] > 0.5;
            if (k == 0 || k == 179 || k == 180 || k == 359) continue;
            S5.trim_bad += on != (k < 180);
        }
    }
    {   /* refusals: dashes on a fill, on a compound; 57 prims; MITER outside a polyline */
        static ygfx_prim pr[57];
        ygfx_compound_desc cd;
        ygfx_stim r[4];
        memset(&d, 0, sizeof d);
        d.shape = YGFX_CIRCLE; d.w = 50; d.dash[0] = 5; d.dash[1] = 5;
        r[0] = ygfx_shape(&d);
        memset(pr, 0, sizeof pr);
        for (k = 0; k < 57; k++) { pr[k].shape = YGFX_CIRCLE; pr[k].w = 10; }
        memset(&cd, 0, sizeof cd);
        cd.prims = pr; cd.n = 57;
        r[1] = ygfx_compound(&cd);
        cd.n = 2; r[2] = ygfx_compound(&cd); r[2].stroke = 2; r[2].dash[0] = 4; r[2].dash[1] = 4;
        d.dash[0] = 0; d.shape = YGFX_STAR; d.shape_p[0] = 5; d.join = YGFX_JOIN_MITER;
        r[3] = ygfx_shape(&d);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        for (k = 0; k < 4; k++) { S5.refused_n++; S5.refused_ok += ygfx_draw(&R->g, &r[k]) == YGFX_ERR_ARG; }
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    ygfx_close(&R->g);
}

/* Oklab from linear device RGB through a calibration's XYZ (white Y = 1),
 * Ottosson's published matrices, in double: written apart from the header. */
static void ok_from_rgb(const ycol_cal* c, const double rgb[3], double lab[3]) {
    static const double m1[9] = { 0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715, 0.0361456387,
                                  0.0482003018, 0.2643662691, 0.6338517070 };
    static const double m2[9] = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    double xyz[3], lms[3], yw = c->rgb_to_xyz[3] + c->rgb_to_xyz[4] + c->rgb_to_xyz[5];
    int i, k;
    for (i = 0; i < 3; i++) { xyz[i] = 0; for (k = 0; k < 3; k++) xyz[i] += c->rgb_to_xyz[3 * i + k] * rgb[k] / yw; }
    for (i = 0; i < 3; i++) { lms[i] = 0; for (k = 0; k < 3; k++) lms[i] += m1[3 * i + k] * xyz[k]; lms[i] = cbrt(lms[i]); }
    for (i = 0; i < 3; i++) { lab[i] = 0; for (k = 0; k < 3; k++) lab[i] += m2[3 * i + k] * lms[k]; }
}

/* ... and back, by Newton on the forward map (no inverse matrices shared
 * with the header). */
static void ok_to_rgb(const ycol_cal* c, const double lab[3], double rgb[3]) {
    int it, i, j;
    rgb[0] = rgb[1] = rgb[2] = 0.5;
    for (it = 0; it < 30; it++) {
        double f[3], J[9], inv[9], det, h = 1e-7;
        ok_from_rgb(c, rgb, f);
        for (j = 0; j < 3; j++) {
            double r2[3], f2[3];
            memcpy(r2, rgb, sizeof r2);
            r2[j] += h;
            ok_from_rgb(c, r2, f2);
            for (i = 0; i < 3; i++) J[3 * i + j] = (f2[i] - f[i]) / h;
        }
        det = J[0] * (J[4] * J[8] - J[5] * J[7]) - J[1] * (J[3] * J[8] - J[5] * J[6]) + J[2] * (J[3] * J[7] - J[4] * J[6]);
        inv[0] = (J[4] * J[8] - J[5] * J[7]) / det; inv[1] = (J[2] * J[7] - J[1] * J[8]) / det; inv[2] = (J[1] * J[5] - J[2] * J[4]) / det;
        inv[3] = (J[5] * J[6] - J[3] * J[8]) / det; inv[4] = (J[0] * J[8] - J[2] * J[6]) / det; inv[5] = (J[2] * J[3] - J[0] * J[5]) / det;
        inv[6] = (J[3] * J[7] - J[4] * J[6]) / det; inv[7] = (J[1] * J[6] - J[0] * J[7]) / det; inv[8] = (J[0] * J[4] - J[1] * J[3]) / det;
        for (i = 0; i < 3; i++) {
            double e = 0;
            for (j = 0; j < 3; j++) e += inv[3 * i + j] * (f[j] - lab[j]);
            rgb[i] -= e;
        }
    }
}

static void gl_v03_paint(stats* st) {
    static ycol_cal cal, cal2;
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    ygfx_desc gd;
    ygfx_shape_desc d;
    ygfx_paint pt;
    ygfx_stim s;
    int i, j, k, sp;
    (void)st;
    CHECK(ycol_cal_nominal(&cal, xy, 80.0f, 2.2) == YGFX_OK);
    {   /* a calibration with spectra for DKL: Psychtoolbox's B_monitor */
        float r[81], g[81], b[81];
        char err[200];
        for (i = 0; i < 81; i++) { r[i] = b_monitor[i][0]; g[i] = b_monitor[i][1]; b[i] = b_monitor[i][2]; }
        ycol_cal_init(&cal2);
        ycol_cal_add(&cal2, YCOL_GUN_BLACK, 0, 0, 0, 0);
        for (k = 0; k < 3; k++) ycol_cal_add(&cal2, k, 1, 1, 0, 0);
        ycol_cal_set_spectra(&cal2, 380, 5, 81, r, g, b, NULL);
        CHECK(ycol_cal_derive(&cal2, err, sizeof err) == YGFX_OK);
    }
    for (sp = 0; sp < 3; sp++) {
        memset(&gd, 0, sizeof gd);
        gd.screen = &R->scr;
        gd.background[0] = 0.4f; gd.background[1] = 0.5f; gd.background[2] = 0.3f;
        gd.cal = sp == 2 ? &cal2 : (sp == 1 ? &cal : NULL);
        gd.cache = test_cache();
        ygfx__test_scene32 = 1;
        if (!ygfx_open(&R->g, &gd)) { fprintf(stderr, "paint open: %s\n", ygfx_error(&R->g)); g_failures++; return; }
        for (k = 0; k < 3; k++) {   /* LINEAR, RADIAL, ANGULAR */
            memset(&pt, 0, sizeof pt);
            pt.kind = (uint8_t)(YGFX_PAINT_LINEAR + k); pt.space = (uint8_t)sp; pt.n = 3;
            if (k == 0) { pt.x0 = -60; pt.y0 = -20; pt.x1 = 60; pt.y1 = 20; }
            if (k == 1) { pt.x0 = 5; pt.y0 = -3; pt.x1 = 70; }
            if (k == 2) { pt.x0 = 0; pt.y0 = 0; pt.x1 = 30; }
            pt.stops[0].t = 0; pt.stops[0].color[0] = 0.55f; pt.stops[0].color[1] = 0.45f; pt.stops[0].color[2] = 0.25f;
            pt.stops[1].t = 0.4f; pt.stops[1].color[0] = 0.30f; pt.stops[1].color[1] = 0.55f; pt.stops[1].color[2] = 0.35f;
            pt.stops[2].t = 1; pt.stops[2].color[0] = 0.40f; pt.stops[2].color[1] = 0.45f; pt.stops[2].color[2] = 0.45f;
            memset(&d, 0, sizeof d);
            d.shape = YGFX_RECT; d.w = 160; d.h = 120; d.paint = &pt; d.ori = 10;
            s = ygfx_shape(&d);
            gl_frame(&s, 1);
            for (j = 0; j < H; j += 3)
                for (i = 0; i < W; i += 3) {
                    float lx, ly;
                    double t, a[3], b[3], m[3], want[3], u;
                    int q, seg;
                    ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                    if (fabs(lx) > 79 || fabs(ly) > 59) continue;
                    if (k == 0) t = ((lx + 60) * 120 + (ly + 20) * 40) / (120.0 * 120 + 40 * 40);
                    else if (k == 1) t = sqrt((lx - 5) * (lx - 5) + (ly + 3) * (ly + 3)) / 70;
                    else { t = (atan2(ly, lx) - 30 * 3.14159265358979323846 / 180) / (2 * 3.14159265358979323846); t -= floor(t); }
                    if (k == 2 && (fabs(t) < 1e-3 || fabs(t - 1) < 1e-3)) continue;   /* the angular seam */
                    t = t < 0 ? 0 : (t > 1 ? 1 : t);
                    seg = t < 0.4 ? 0 : 1;
                    u = seg ? (t - 0.4) / 0.6 : t / 0.4;
                    for (q = 0; q < 3; q++) { a[q] = pt.stops[seg].color[q]; b[q] = pt.stops[seg + 1].color[q]; }
                    if (sp == 0) for (q = 0; q < 3; q++) want[q] = a[q] + (b[q] - a[q]) * u;
                    else if (sp == 1) {
                        double la[3], lb[3];
                        ok_from_rgb(&cal, a, la); ok_from_rgb(&cal, b, lb);
                        for (q = 0; q < 3; q++) m[q] = la[q] + (lb[q] - la[q]) * u;
                        ok_to_rgb(&cal, m, want);
                    } else {   /* DKL polar about the background, through ycol_cal_dir_dkl */
                        double ea[3], eb[3], dk[3], inv[9], M[9];
                        float dir[3] = { 0, 0, 0 }, unit[3];   /* newer gcc cannot see the call fill it */
                        int c2, r2;
                        for (c2 = 0; c2 < 3; c2++) {   /* the RGB increment of each DKL unit axis, then its inverse */
                            unit[0] = unit[1] = unit[2] = 0; unit[c2] = 1;
                            ycol_cal_dir_dkl(&cal2, gd.background, unit, dir);
                            for (r2 = 0; r2 < 3; r2++) M[3 * r2 + c2] = dir[r2];
                        }
                        ycol__inv3(M, inv);
                        for (q = 0; q < 2; q++) {
                            double* e3 = q ? eb : ea;
                            const double* cc = q ? b : a;
                            double inc[3], v[3], rr;
                            for (r2 = 0; r2 < 3; r2++) inc[r2] = cc[r2] - gd.background[r2];
                            for (r2 = 0; r2 < 3; r2++) v[r2] = inv[3 * r2] * inc[0] + inv[3 * r2 + 1] * inc[1] + inv[3 * r2 + 2] * inc[2];
                            rr = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                            e3[0] = asin(v[0] / rr); e3[1] = atan2(v[2], v[1]); e3[2] = rr;
                        }
                        while (eb[1] - ea[1] > 3.14159265358979323846) eb[1] -= 2 * 3.14159265358979323846;
                        while (eb[1] - ea[1] < -3.14159265358979323846) eb[1] += 2 * 3.14159265358979323846;
                        for (q = 0; q < 3; q++) m[q] = ea[q] + (eb[q] - ea[q]) * u;
                        dk[0] = m[2] * sin(m[0]); dk[1] = m[2] * cos(m[0]) * cos(m[1]); dk[2] = m[2] * cos(m[0]) * sin(m[1]);
                        for (q = 0; q < 3; q++) want[q] = gd.background[q] + M[3 * q] * dk[0] + M[3 * q + 1] * dk[1] + M[3 * q + 2] * dk[2];
                    }
                    for (q = 0; q < 3; q++) {
                        double e = fabs(R->scene[(j * W + i) * 4 + q] - want[q]);
                        if (sp == 0) S5.paint_rgb = maxd(S5.paint_rgb, e);
                        else if (sp == 1) S5.paint_oklab = maxd(S5.paint_oklab, e);
                        else S5.paint_dkl = maxd(S5.paint_dkl, e);
                    }
                }
        }
        if (sp == 0) {
            /* VERTEX on a triangle: Wachspress coordinates are barycentric there */
            static const float tri[6] = { -70, 50, 80, 40, -10, -60 };
            static const float vc[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            memset(&pt, 0, sizeof pt);
            pt.kind = YGFX_PAINT_VERTEX; pt.vertex_colors = vc;
            memset(&d, 0, sizeof d);
            d.shape = YGFX_POLYGON; d.vertices = tri; d.shape_p[0] = 3; d.paint = &pt;
            s = ygfx_shape(&d);
            gl_frame(&s, 1);
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    float lx, ly;
                    double l0, l1, l2, den;
                    ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                    den = (tri[3] - tri[5]) * (tri[0] - tri[4]) + (tri[4] - tri[2]) * (tri[1] - tri[5]);
                    l0 = ((tri[3] - tri[5]) * (lx - tri[4]) + (tri[4] - tri[2]) * (ly - tri[5])) / den;
                    l1 = ((tri[5] - tri[1]) * (lx - tri[4]) + (tri[0] - tri[4]) * (ly - tri[5])) / den;
                    l2 = 1 - l0 - l1;
                    if (l0 < 0.01 || l1 < 0.01 || l2 < 0.01) continue;   /* inside, off the edge */
                    S5.paint_vertex = maxd(S5.paint_vertex, fabs(R->scene[(j * W + i) * 4] - l0));
                    S5.paint_vertex = maxd(S5.paint_vertex, fabs(R->scene[(j * W + i) * 4 + 1] - l1));
                    S5.paint_vertex = maxd(S5.paint_vertex, fabs(R->scene[(j * W + i) * 4 + 2] - l2));
                }
            /* no calibration: OKLAB and DKL_POLAR are refused */
            memset(&pt, 0, sizeof pt);
            pt.kind = YGFX_PAINT_LINEAR; pt.space = YGFX_SPACE_OKLAB; pt.n = 2; pt.x1 = 10;
            memset(&d, 0, sizeof d);
            d.shape = YGFX_RECT; d.w = 20; d.h = 20; d.paint = &pt;
            s = ygfx_shape(&d);
            CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
            CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
            S5.refused_n++; S5.refused_ok += ygfx_draw(&R->g, &s) == YGFX_ERR_REFUSED;
            pt.space = YGFX_SPACE_DKL_POLAR;
            S5.refused_n++; S5.refused_ok += ygfx_draw(&R->g, &s) == YGFX_ERR_REFUSED;
            CHECK(ygfx_end(&R->g) == YGFX_OK);
            CHECK(yscr_flip(&R->scr) == YSCR_OK);
        }
        ygfx_close(&R->g);
    }
}

/* An MSDF texture whose three channels hold the same distance (to a rounded
 * box): the median is then that distance, and the GPU's decoding is
 * checked against the CPU's bilinear of the same texels. */
static void gl_v03_msdf(stats* st) {
    enum { TW = 96, TH = 48 };
    static float tex[TW * TH * 4];
    ygfx_texture_desc td;
    ygfx_tex t;
    ygfx_shape_desc d;
    ygfx_stim s;
    int i, j, k;
    const double range = 16;
    (void)st;
    for (j = 0; j < TH; j++)
        for (i = 0; i < TW; i++) {
            /* two glyphs side by side: boxes in each 48 x 48 half */
            double x = i + 0.5 - (i < 48 ? 24 : 72), y = j + 0.5 - 24, dd, v;
            double hx = i < 48 ? 10 : 14, hy = i < 48 ? 14 : 8, qx = fabs(x) - hx + 3, qy = fabs(y) - hy + 3;
            dd = sqrt(fmax(qx, 0) * fmax(qx, 0) + fmax(qy, 0) * fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - 3;
            v = 0.5 - dd / range;
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            /* three channels apart, their median the distance (red); alpha too */
            tex[(j * TW + i) * 4] = (float)v;
            tex[(j * TW + i) * 4 + 1] = (float)(v + 0.2 > 1 ? 1 : v + 0.2);
            tex[(j * TW + i) * 4 + 2] = (float)(v - 0.15 < 0 ? 0 : v - 0.15);
            tex[(j * TW + i) * 4 + 3] = (float)v;
        }
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&td, 0, sizeof td);
    td.w = TW; td.h = TH; td.format = YGFX_RGBA32F; td.data = tex; td.sdf = YGFX_SDF_MSDF; td.sdf_range = (float)range;
    t = ygfx_texture(&R->g, &td);
    CHECK(t.id != 0);
    for (k = 0; k < 2; k++) {   /* the second glyph by its src rectangle, at 2 px per texel */
        double sc = 2.0, sx0 = 48;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_MASK_TEX; d.mask = t; d.w = (float)(48 * sc); d.h = (float)(48 * sc); d.ori = k ? 15.0f : 0.0f; d.x = 1.25f;
        d.edge = YGFX_EDGE_GAUSSIAN; d.edge_width = 1.5f; d.color[0] = 1;
        s = ygfx_shape(&d);
        s.src[0] = 48; s.src[1] = 0; s.src[2] = 48; s.src[3] = 48;
        if (k) { s.stroke = 3; s.join = YGFX_JOIN_MITER; }
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                float lx, ly;
                double tx, ty, fx, fy, wx, wy, c00, c10, c01, c11, m, dd, want, a1v, a2v;
                int x0, y0, x1, y1;
                ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
                if (fabs(lx) > 48 || fabs(ly) > 48) continue;
                tx = sx0 + (lx + 48) / sc; ty = (ly + 48) / sc;
                fx = tx - 0.5; fy = ty - 0.5; x0 = (int)floor(fx); y0 = (int)floor(fy); wx = fx - x0; wy = fy - y0;
                x1 = x0 + 1; y1 = y0 + 1;
                if (x0 < 48) x0 = 48;
                if (x1 > 95) x1 = 95;
                if (x0 > 95) x0 = 95;
                if (x1 < 48) x1 = 48;
                if (y0 < 0) y0 = 0;
                if (y1 > 47) y1 = 47;
                if (y0 > 47) y0 = 47;
                if (y1 < 0) y1 = 0;
                c00 = tex[(y0 * TW + x0) * 4]; c10 = tex[(y0 * TW + x1) * 4]; c01 = tex[(y1 * TW + x0) * 4]; c11 = tex[(y1 * TW + x1) * 4];
                m = (c00 + (c10 - c00) * wx) * (1 - wy) + (c01 + (c11 - c01) * wx) * wy;
                dd = (0.5 - m) * range * sc;
                if (k) { a1v = -1.5; a2v = 1.5; want = cov(2, 1.5, dd - a2v) - cov(2, 1.5, dd - a1v); }
                else want = cov(2, 1.5, dd);
                S5.msdf = maxd(S5.msdf, fabs(R->scene[(j * W + i) * 4] - want));
            }
    }
    {   /* the range rule at 1 px per texel and range 16: a Gaussian reaches
         * 5 SD, which must stay within 8 - 1.5 = 6.5 px */
        int rc[3];
        memset(&d, 0, sizeof d);
        d.shape = YGFX_MASK_TEX; d.mask = t; d.w = 96; d.h = 48; d.edge = YGFX_EDGE_GAUSSIAN; d.color[0] = 1;
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        d.edge_width = 1.25f; s = ygfx_shape(&d); rc[0] = ygfx_draw(&R->g, &s);          /* 6.25 <= 6.5 at 1 px per texel */
        d.edge_width = 1.4f; s = ygfx_shape(&d); rc[1] = ygfx_draw(&R->g, &s);           /* 7 > 6.5 */
        if (rc[1] == YGFX_ERR_RANGE) CHECK(strstr(ygfx_error(&R->g), "range") != NULL);
        d.edge_width = 0; d.stroke = 2; s = ygfx_shape(&d); rc[2] = ygfx_draw(&R->g, &s); /* MSDF, ROUND join */
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        S5.refused_n += 3;
        S5.refused_ok += (rc[0] == YGFX_OK) + (rc[1] == YGFX_ERR_RANGE) + (rc[2] == YGFX_ERR_ARG);
    }
    {   /* a glyph run of 3 against the same glyphs drawn one by one */
        static ygfx_glyph gl3[3];
        static float a1[W * H * 4];
        ygfx_glyphs_desc gd;
        ygfx_buf b;
        ygfx_stim one[3];
        memset(gl3, 0, sizeof gl3);
        for (k = 0; k < 3; k++) { gl3[k].x = (float)(k * 50); gl3[k].y = (float)(k * 7); gl3[k].sx = (float)(k % 2 * 48); gl3[k].sw = 48; gl3[k].sh = 48; }
        b = ygfx_buffer(&R->g, sizeof gl3);
        CHECK(ygfx_buffer_update(&R->g, b, 0, gl3, sizeof gl3) == YGFX_OK);
        memset(&gd, 0, sizeof gd);
        gd.atlas = t; gd.buf = b; gd.count = 3; gd.scale = 2; gd.place = YGFX_TOP_LEFT; gd.anchor = YGFX_TOP_LEFT;
        gd.x = 20; gd.y = 30; gd.w = 150; gd.h = 62; gd.edge = YGFX_EDGE_COSINE; gd.edge_width = 2; gd.color[1] = 1;
        s = ygfx_glyphs(&gd);
        gl_frame(&s, 1);
        memcpy(a1, R->scene, sizeof a1);
        for (k = 0; k < 3; k++) {
            memset(&d, 0, sizeof d);
            d.shape = YGFX_MASK_TEX; d.mask = t; d.w = 96; d.h = 96; d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT;
            d.x = 20.0f + gl3[k].x; d.y = 30.0f + gl3[k].y; d.edge = YGFX_EDGE_COSINE; d.edge_width = 2; d.color[1] = 1;
            one[k] = ygfx_shape(&d);
            one[k].src[0] = gl3[k].sx; one[k].src[2] = 48; one[k].src[3] = 48;
        }
        gl_frame(one, 3);
        for (i = 0; i < W * H * 4; i++) S5.glyph = maxd(S5.glyph, fabs(a1[i] - R->scene[i]));
    }
    ygfx_close(&R->g);
}

/* v0.3's CPU half: field bindings, the new tables, refusals before GL. */
/* An ellipse's quarter perimeter by composite Simpson in double, 20000
 * panels: none of the header's arithmetic (v0.7 replaced its quadrature
 * by the arithmetic-geometric mean). */
static double ell_quarter_ref(double a, double b) {
    const int n = 20000;
    double h = 0.5 * 3.14159265358979323846 / n, sum = 0;
    int k;
    for (k = 0; k <= n; k++) {
        double u = k * h, f = sqrt(a * a * sin(u) * sin(u) + b * b * cos(u) * cos(u));
        sum += f * (k == 0 || k == n ? 1 : (k % 2 ? 4 : 2));
    }
    return sum * h / 3.0;
}

static void test_v03_cpu(void) {
    int n = 0, i, j;
    const ygfx_param* t;
    ygfx_prim pr[2];
    ygfx_fx fx;
    ygfx_bind b[3];
    float v[3] = { 12.0f, 0.25f, 7.0f };
    memset(pr, 0, sizeof pr);
    memset(&fx, 0, sizeof fx);
    memset(b, 0, sizeof b);
    b[0].field = &pr[1].x; b[0].channel = 0;
    b[1].field = &fx.drop.opacity; b[1].channel = 1;
    b[2].stim = NULL; b[2].param = 0; b[2].channel = 2;   /* bad: no stim, no field */
    {   /* the ellipse's quarter perimeter (dashes and trim on ELLIPSE) */
        static const double asp[7] = { 1, 1.001, 1.7, 2, 4.5, 10, 20 }, sc[3] = { 0.5, 45, 900 };
        double worst = 0;
        for (i = 0; i < 7; i++)
            for (j = 0; j < 3; j++) {
                double a = sc[j] * asp[i], bb = sc[j], r = ell_quarter_ref(a, bb);
                worst = maxd(worst, fabs(ygfx__ell_quarter(a, bb) - r) / r);
                worst = maxd(worst, fabs(ygfx__ell_quarter(bb, a) - r) / r);
            }
        S4.ell_quarter = worst;
        CHECK_LE(worst, 1e-12);
    }
    CHECK(ygfx_apply(b, 3, v) == YGFX_ERR_ARG && pr[1].x == 0.0f);
    CHECK(ygfx_apply(b, 2, v) == 2 && pr[1].x == 12.0f && fx.drop.opacity == 0.25f);
    t = ygfx_prim_params(&n);
    CHECK(n == 11);
    for (i = 0; i < n; i++) {
        CHECK(t[i].name && t[i].offset + sizeof(float) <= sizeof(ygfx_prim) && t[i].min <= t[i].def && t[i].def <= t[i].max);
        for (j = 0; j < i; j++) CHECK(strcmp(t[i].name, t[j].name) != 0);
    }
    t = ygfx_fx_params(&n);
    CHECK(n == 32);
    for (i = 0; i < n; i++) {
        CHECK(t[i].name && t[i].offset + sizeof(float) <= sizeof(ygfx_fx));
        for (j = 0; j < i; j++) CHECK(strcmp(t[i].name, t[j].name) != 0);
    }
    CHECK(t[2].offset == offsetof(ygfx_fx, drop.sigma) && t[31].offset == offsetof(ygfx_fx, band[1].opacity));
    t = ygfx_paint_params(&n);
    CHECK(n == 28);
    for (i = 0; i < n; i++) CHECK(t[i].name && t[i].offset + sizeof(float) <= sizeof(ygfx_paint));
    t = ygfx_params(&n);
    CHECK(t[YGFX_P_TRIM_END].offset == offsetof(ygfx_stim, trim[1]));
    {   /* Oklab's L is relative to white: a round trip cannot see a lost
         * white scale, so the white itself must map to L = 1, a = b = 0 */
        static ygfx_gfx gw;
        static ycol_cal cw;
        static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
        static const float white[3] = { 1.0f, 1.0f, 1.0f };
        double lab[3];
        memset(&gw, 0, sizeof gw);
        CHECK(ycol_cal_nominal(&cw, xy, 80.0f, 2.2) == YGFX_OK);
        {   /* PAINT's spaces come from ysp/color.h's context (v0.5) */
            ycol_ctx_desc cd;
            memset(&cd, 0, sizeof cd);
            cd.cal = &cw;
            CHECK(ycol_ctx_open(&gw.color, &cd));
            ygfx__paint_spaces(&gw);
        }
        CHECK(gw.has_oklab);
        ygfx__to_space(&gw, YGFX_SPACE_OKLAB, white, lab);
        CHECK(fabs(lab[0] - 1.0) < 1e-3 && fabs(lab[1]) < 1e-3 && fabs(lab[2]) < 1e-3);
    }
}

/* ------------------------------------------------------------- v0.4 cache */

/* A program cache in memory: the test's desc.cache, and the store the
 * corruption checks reach into. */
#define MC_MAX 64
typedef struct memcache { int n; uint64_t key[MC_MAX]; unsigned char* data[MC_MAX]; size_t len[MC_MAX]; long loads, stores; } memcache;
static size_t mc_load(void* u, uint64_t key, void* dst, size_t cap) {
    memcache* m = (memcache*)u;
    int i;
    m->loads++;
    for (i = 0; i < m->n; i++)
        if (m->key[i] == key) {
            if (dst && cap >= m->len[i]) memcpy(dst, m->data[i], m->len[i]);
            return m->len[i];
        }
    return 0;
}
static int mc_store(void* u, uint64_t key, const void* data, size_t n) {
    memcache* m = (memcache*)u;
    int i;
    unsigned char* p = (unsigned char*)malloc(n);
    if (!p) return -1;
    memcpy(p, data, n);
    m->stores++;
    for (i = 0; i < m->n && m->key[i] != key; i++) {}
    if (i == MC_MAX) { free(p); return -1; }
    if (i == m->n) m->n++;
    else free(m->data[i]);
    m->key[i] = key; m->data[i] = p; m->len[i] = n;
    return 0;
}
static void mc_clear(memcache* m) {
    int i;
    for (i = 0; i < m->n; i++) free(m->data[i]);
    memset(m, 0, sizeof *m);
}

/* A backend whose "binary" is a hash of the program's text: the cache's
 * logic, every reject path included, without GL. */
#define FB_PROGS (11 + YGFX__N_VSPEC)
static ygfx__null g_fb_ctx;
static ygfx_backend g_fb;
static uint64_t g_fb_hash[256];
static int g_fb_refuse;
static const char* g_fb_vendor = "fake vendor";
static const char* g_fb_renderer = "fake renderer";
static const char* g_fb_version = "fake 1";
static int fb_open(void* c, const ygfx_backend_open* in, ygfx_backend_caps* caps, char* err, size_t cap) {
    int rc = ygfx__null_backend.open(c, in, caps, err, cap);
    snprintf(caps->vendor, sizeof caps->vendor, "%s", g_fb_vendor);
    snprintf(caps->version, sizeof caps->version, "%s", g_fb_version);
    snprintf(caps->renderer, sizeof caps->renderer, "%s", g_fb_renderer);
    caps->binary_format = 0x1234;
    return rc;
}
static int fb_make(void* c, const ygfx_pipeline_src* d, uint32_t* id, char* err, size_t cap) {
    uint64_t h = ygfx__fnv64(d->vs, strlen(d->vs)) ^ (ygfx__fnv64(d->fs, strlen(d->fs)) * 3u);
    int loaded = d->binary && d->binary_size == 64 && d->binary_format == 0x1234 && !g_fb_refuse &&
                 memcmp(d->binary, &h, sizeof h) == 0;
    ygfx__null_backend.pipeline_make(c, d, id, err, cap);
    if (*id < 256) g_fb_hash[*id] = h;
    return loaded;
}
static int fb_binary(void* c, uint32_t id, void* out, size_t cap, uint32_t* format) {
    (void)c;
    if (out && cap >= 64) { memset(out, 0xA5, 64); memcpy(out, &g_fb_hash[id & 255], 8); *format = 0x1234; }
    return 64;
}

typedef struct stats6 {
    /* cache: rejects that fell back, loads after a cold open, frames equal */
    long cache_bad, cache_cases;
    double open_cold_ms, open_warm_ms;
    int programs;
    /* the gallery's findings (v0.4 fixes) */
    double alpha_lin, alpha_old, first_diff, shape_p_diff;
    long dots_px;
    long zero_ok, zero_n, zero_poly_px, zero_dots_px;
    /* VIDEO */
    double vid_1x, vid_trc, vid_prim, vid_lin, vid_part, rebind_diff, import_gl_diff, import_d3d_diff;
    long vid_cases, codes_bad, vid_refused_ok, vid_refused_n, d3d_ran;
    double gray_diff;               /* v0.10.5: R8 and RG8 with a stated transfer */
    long gray_cases, gray_refused_ok, gray_refused_n;
    double shared_diff[2];          /* NT handle + keyed mutex; legacy handle + event query */
    const char* shared_why[2];
    /* INSTANCES */
    double inst_diff[5], pal_frac, csd_err;
    long pal_bad, hit_bad, hit_n, hit_ties, bind_bad, inst_refused_ok, inst_refused_n;
    long order_bad, order_frames, order_saved, order_pass_ok;
    long refused_ok, refused_n;
} stats6;
static stats6 S6;

static int fb_open_gfx(ygfx_gfx* g, const ygfx_cache* c, ygfx_programs* st) {
    static yscr_screen scr;
    yscr_desc d;
    ygfx_desc gd;
    int ok;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    if (!yscr_is_open(&scr) && !yscr_open(&scr, &d)) return 0;
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr; gd.width = 64; gd.height = 64;
    gd.backend = &g_fb; gd.backend_ctx = &g_fb_ctx; gd.cache = c;
    ok = ygfx_open(g, &gd);
    ygfx_program_stats(g, st);
    return ok;
}

static ygfx_pipe user_pipe(ygfx_gfx* g, const char* body) {
    ygfx_pipeline_desc d;
    memset(&d, 0, sizeof d);
    d.body = body;
    return ygfx_pipeline(g, &d);
}

/* v0.11.1 (OPEN START): ygfx_open_start() submits every program on a
 * backend that compiles in parallel and returns; ygfx_open() finishes it.
 * On a backend that does not, the start submits nothing and ygfx_open()
 * does it all. */
static int g_fb_parallel, g_fb_makes, g_fb_finishes, g_fb_closes;
static void fb_close(void* c) {
    g_fb_closes++;
    ygfx__null_backend.close(c);
}
static int fb_open_p(void* c, const ygfx_backend_open* in, ygfx_backend_caps* caps, char* err, size_t cap) {
    int rc = fb_open(c, in, caps, err, cap);
    caps->parallel_compile = g_fb_parallel != 0;
    return rc;
}
static int fb_make_p(void* c, const ygfx_pipeline_src* d, uint32_t* id, char* err, size_t cap) {
    g_fb_makes++;
    return fb_make(c, d, id, err, cap);
}
static int fb_finish(void* c, uint32_t id, char* err, size_t cap) {
    (void)c; (void)id; (void)err; (void)cap;
    g_fb_finishes++;
    return YGFX_OK;
}

static void test_v11_start_cpu(void) {
    static ygfx_gfx g;
    static yscr_screen scr;
    yscr_desc d;
    ygfx_desc gd;
    char line[700];
    g_fb = ygfx__null_backend;
    g_fb.open = fb_open_p; g_fb.pipeline_make = fb_make_p; g_fb.pipeline_binary = fb_binary;
    g_fb.pipeline_finish = fb_finish; g_fb.close = fb_close;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    CHECK(yscr_open(&scr, &d));
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr; gd.width = 64; gd.height = 64;
    gd.backend = &g_fb; gd.backend_ctx = &g_fb_ctx;
    /* parallel: every program submitted at the start, finished at open */
    g_fb_parallel = 1; g_fb_makes = g_fb_finishes = 0;
    CHECK(ygfx_open_start(&g, &gd));
    CHECK(!ygfx_is_open(&g));
    CHECK(g_fb_makes == FB_PROGS && g_fb_finishes == 0);
    CHECK(ygfx_prime(&g) == YGFX_ERR_CLOSED);       /* nothing else until open */
    CHECK(!ygfx_open_start(&g, &gd));                /* already started */
    CHECK(strstr(ygfx_error(&g), "already open") != NULL);
    gd.background[0] = 2.0f;                         /* the start's desc counts */
    CHECK(ygfx_open(&g, &gd));
    gd.background[0] = 0.0f;
    CHECK(ygfx_is_open(&g));
    CHECK(g_fb_makes == FB_PROGS && g_fb_finishes == FB_PROGS);
    ygfx_describe(&g, line, sizeof line);
    CHECK(strstr(line, ", open_start=parallel") != NULL);
    ygfx_close(&g);
    /* serial: nothing at the start */
    g_fb_parallel = 0; g_fb_makes = g_fb_finishes = 0;
    CHECK(ygfx_open_start(&g, &gd));
    CHECK(!ygfx_is_open(&g));
    CHECK(g_fb_makes == 0 && g_fb_finishes == 0);
    gd.background[0] = 0.25f;                        /* made from the start's desc */
    CHECK(ygfx_open(&g, &gd));
    gd.background[0] = 0.0f;
    CHECK(g.bg[0] == 0.0f);
    CHECK(g_fb_makes == FB_PROGS && g_fb_finishes == FB_PROGS);
    ygfx_describe(&g, line, sizeof line);
    CHECK(strstr(line, ", open_start=serial(no GL_KHR_parallel_shader_compile)") != NULL);
    ygfx_close(&g);
    /* the blocking open: no token */
    g_fb_parallel = 1;
    CHECK(ygfx_open(&g, &gd));
    ygfx_describe(&g, line, sizeof line);
    CHECK(strstr(line, "open_start") == NULL);
    ygfx_close(&g);
    /* a start refused (a bad desc) leaves a closed handle */
    gd.background[0] = 2.0f;
    CHECK(!ygfx_open_start(&g, &gd));
    gd.background[0] = 0.0f;
    CHECK(ygfx_open(&g, &gd));
    ygfx_close(&g);
    /* a started handle closed: its objects are deleted */
    g_fb_closes = 0;
    CHECK(ygfx_open_start(&g, &gd));
    ygfx_close(&g);
    CHECK(g_fb_closes == 1);
    /* the screen closed after the start (yscr_open() failed after its
     * hook): open names it, and nothing is deleted in a context that is
     * gone */
    CHECK(ygfx_open_start(&g, &gd));
    yscr_close(&scr);
    g_fb_finishes = g_fb_closes = 0;
    CHECK(!ygfx_open(&g, NULL));
    CHECK(strstr(ygfx_error(&g), "the screen closed after ygfx_open_start()") != NULL);
    CHECK(g_fb_finishes == 0 && g_fb_closes == 0);
    ygfx_close(&g);
    CHECK(yscr_open(&scr, &d));
    CHECK(ygfx_open_start(&g, &gd));
    yscr_close(&scr);
    ygfx_close(&g);
    CHECK(g_fb_closes == 0 && !ygfx_is_open(&g));
    g_fb_parallel = 0;
}

/* v0.10.1: the per-user folder from the environment, and none (never a
 * shared one) when the environment gives no private one. The process's
 * environment is changed and put back. */
static void test_default_cache_dir(void) {
    char d[512], few[8];
    CHECK(ygfx_default_cache_dir(NULL, sizeof d) == YGFX_ERR_ARG);
    CHECK(ygfx_default_cache_dir(d, 0) == YGFX_ERR_ARG);
    few[0] = 'x';
    CHECK(ygfx_default_cache_dir(few, sizeof few) == YGFX_ERR_ARG && few[0] == '\0');
#if defined(_WIN32)
    {
        wchar_t old[512];
        DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", old, 512);
        /* a non-ASCII user name comes out as UTF-8; the trailing slash goes */
        SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\t\u00e9st\\AppData\\Local\\");
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_OK &&
              strcmp(d, "C:\\Users\\t\xc3\xa9st\\AppData\\Local\\ysp\\progcache") == 0);
        CHECK(ygfx_default_cache_dir(d, 30) == YGFX_ERR_ARG && d[0] == '\0');   /* does not fit */
        SetEnvironmentVariableW(L"LOCALAPPDATA", L"\\\\server\\share");
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_OK && strcmp(d, "\\\\server\\share\\ysp\\progcache") == 0);
        SetEnvironmentVariableW(L"LOCALAPPDATA", L"relative\\x");
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
        SetEnvironmentVariableW(L"LOCALAPPDATA", NULL);
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
        SetEnvironmentVariableW(L"LOCALAPPDATA", k > 0 && k < 512 ? old : NULL);
        if (k > 0 && k < 512) CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_OK);
    }
#elif defined(__EMSCRIPTEN__)
    /* WebGL has no program binaries: never a folder, whatever the
     * environment says. */
    CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
#else
    {
        char home[512] = "", xdg[512] = "";
        const char* e = getenv("HOME");
        int had_home = e != NULL, had_xdg;
        if (e) snprintf(home, sizeof home, "%s", e);
        e = getenv("XDG_CACHE_HOME");
        had_xdg = e != NULL;
        if (e) snprintf(xdg, sizeof xdg, "%s", e);
        setenv("HOME", "/nonexistent-ygfx/home//", 1);
        setenv("XDG_CACHE_HOME", "/nonexistent-ygfx/xdg/", 1);
#if defined(__APPLE__)
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_OK && strcmp(d, "/nonexistent-ygfx/home/Library/Caches/ysp/progcache") == 0);
#else
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_OK && strcmp(d, "/nonexistent-ygfx/xdg/ysp/progcache") == 0);
        setenv("XDG_CACHE_HOME", "relative", 1);   /* the spec: ignored */
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_OK && strcmp(d, "/nonexistent-ygfx/home/.cache/ysp/progcache") == 0);
        setenv("XDG_CACHE_HOME", "/tmp", 1);       /* 1777: refused, no other folder tried */
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
        unsetenv("XDG_CACHE_HOME");
#endif
        setenv("HOME", "/tmp", 1);
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
        setenv("HOME", "home", 1);
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
        unsetenv("HOME");
        CHECK(ygfx_default_cache_dir(d, sizeof d) == YGFX_ERR_ARG && d[0] == '\0');
        if (had_home) setenv("HOME", home, 1);
        if (had_xdg) setenv("XDG_CACHE_HOME", xdg, 1); else unsetenv("XDG_CACHE_HOME");
    }
#endif
}

static void test_v04_cache_cpu(void) {
    static ygfx_gfx g;
    static memcache mc;
    ygfx_cache c;
    ygfx_programs st;
    int i;
    g_fb = ygfx__null_backend;
    g_fb.open = fb_open; g_fb.pipeline_make = fb_make; g_fb.pipeline_binary = fb_binary;
    c.load = mc_load; c.store = mc_store; c.user = &mc;
    mc_clear(&mc);
    /* cold, then warm */
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.compiled == FB_PROGS && st.loaded == 0 && st.stored == FB_PROGS && st.rejected == 0 && mc.n == FB_PROGS);
    {   /* a user pipeline goes through the cache too */
        ygfx_pipe p = user_pipe(&g, "float ysp_main(vec2 p) { return 0.5; }\n");
        CHECK(p.id != 0);
        ygfx_program_stats(&g, &st);
        CHECK(st.compiled == FB_PROGS + 1 && st.stored == FB_PROGS + 1);
    }
    ygfx_close(&g);
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.loaded == FB_PROGS && st.compiled == 0 && st.rejected == 0 && st.stored == 0);
    {   /* v0.10.1: the describe line names the cache and gives hits and misses */
        char line[600], want[80];
        ygfx_describe(&g, line, sizeof line);
        snprintf(want, sizeof want, "program cache (caller): %d hit / 0 miss (0 rejected)", FB_PROGS);
        CHECK(strstr(line, want) != NULL);
    }
    CHECK(user_pipe(&g, "float ysp_main(vec2 p) { return 0.5; }\n").id != 0);
    ygfx_program_stats(&g, &st);
    CHECK(st.loaded == FB_PROGS + 1);
    ygfx_close(&g);
    /* another driver: a different key, so misses, not rejects */
    g_fb_vendor = "another vendor";
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.compiled == FB_PROGS && st.rejected == 0 && st.loaded == 0);
    ygfx_close(&g);
    g_fb_vendor = "fake vendor";
    g_fb_renderer = "fake renderer, another driver version";   /* ANGLE puts the driver here */
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.compiled == FB_PROGS && st.rejected == 0 && st.loaded == 0);
    ygfx_close(&g);
    g_fb_renderer = "fake renderer";
    g_fb_version = "fake 2";   /* another ANGLE build */
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.compiled == FB_PROGS && st.rejected == 0 && st.loaded == 0);
    ygfx_close(&g);
    g_fb_version = "fake 1";
    /* a byte of each payload flipped: the hash rejects it, compiles, stores
     * a good one again */
    for (i = 0; i < mc.n; i++) mc.data[i][mc.len[i] - 1] ^= 1;
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.rejected == FB_PROGS && st.compiled == FB_PROGS && st.loaded == 0 && st.stored == FB_PROGS);
    ygfx_close(&g);
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.loaded == FB_PROGS);
    ygfx_close(&g);
    /* a header byte (the magic), a truncation */
    for (i = 0; i < mc.n; i++) { if (i & 1) mc.data[i][0] ^= 1; else mc.len[i] -= 3; }
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.rejected == FB_PROGS && st.compiled == FB_PROGS);
    ygfx_close(&g);
    /* the driver refuses an entry that passes every check */
    g_fb_refuse = 1;
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.rejected == FB_PROGS && st.compiled == FB_PROGS && st.loaded == 0);
    ygfx_close(&g);
    g_fb_refuse = 0;
    /* every key the same: each program finds another's entry; the second
     * hash catches it */
    mc_clear(&mc);
    ygfx__test_key_mask = 0;
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.compiled == FB_PROGS && mc.n == 1);
    ygfx_close(&g);
    CHECK(fb_open_gfx(&g, &c, &st));
    CHECK(st.loaded == 0 && st.rejected == FB_PROGS && st.compiled == FB_PROGS);   /* each compiled program is stored before the next loads */
    ygfx_close(&g);
    ygfx__test_key_mask = ~(uint64_t)0;
    /* no cache, a cache without store */
    CHECK(fb_open_gfx(&g, NULL, &st));
    CHECK(st.compiled == FB_PROGS && st.stored == 0 && mc.loads > 0);
    {
        char line[600], want[80];
        ygfx_describe(&g, line, sizeof line);
        snprintf(want, sizeof want, "program cache off: 0 hit / %d miss (0 rejected)", FB_PROGS);
        CHECK(strstr(line, want) != NULL);
    }
    ygfx_close(&g);
    mc_clear(&mc);
    /* the file cache's arguments */
    {
        ygfx_file_cache fc;
        char big[600];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(ygfx_file_cache_init(&fc, NULL) == NULL);
        CHECK(ygfx_file_cache_init(&fc, "") == NULL);
        CHECK(ygfx_file_cache_init(&fc, big) == NULL);
        CHECK(ygfx_file_cache_init(&fc, "x/y/") == &fc.cache && strcmp(fc.dir, "x/y") == 0);
    }
    test_default_cache_dir();
}

/* Draws one frame of stimuli from most programs and the output stage. */
static void cache_frame(float* scene, uint8_t* out) {
    static const unsigned char px[4 * 4 * 4] = { 10, 200, 30, 255, 250, 0, 0, 255, 0, 250, 0, 128, 0, 0, 250, 255,
                                                 90, 90, 90, 255, 1, 2, 3, 4, 200, 100, 50, 255, 7, 7, 7, 7,
                                                 10, 200, 30, 255, 250, 0, 0, 255, 0, 250, 0, 128, 0, 0, 250, 255,
                                                 90, 90, 90, 255, 1, 2, 3, 4, 200, 100, 50, 255, 7, 7, 7, 7 };
    ygfx_texture_desc td;
    ygfx_tex t;
    ygfx_stim s[6];
    ygfx_gabor_desc gb;
    ygfx_shape_desc sd;
    ygfx_image_desc id;
    ygfx_noise_desc nd;
    ygfx_grating_desc gr;
    memset(&td, 0, sizeof td);
    td.w = 4; td.h = 4; td.format = YGFX_RGBA8; td.data = px;
    t = ygfx_texture(&R->g, &td);
    memset(&gb, 0, sizeof gb);
    gb.x = -80; gb.sf = 1 / 16.0f; gb.sigma = 12; gb.contrast = 0.4f; gb.ori = 20;
    s[0] = ygfx_gabor(&gb);
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_CIRCLE; sd.x = 60; sd.w = 50; sd.edge = YGFX_EDGE_GAUSSIAN; sd.edge_width = 2;
    sd.color[0] = 0.9f; sd.color[1] = 0.2f; sd.color[2] = 0.1f;
    s[1] = ygfx_shape(&sd);
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_RRECT; sd.y = 60; sd.w = 120; sd.h = 40; sd.shape_p[0] = 6; sd.shape_p[1] = 12; sd.shape_p[2] = 3;
    sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 3; sd.color[0] = 0.2f; sd.color[1] = 0.7f; sd.color[2] = 0.3f;
    s[2] = ygfx_shape(&sd);
    memset(&id, 0, sizeof id);
    id.tex = t; id.x = 120; id.y = -50; id.w = 40; id.h = 40; id.linear = true;
    s[3] = ygfx_image(&R->g, &id);
    memset(&nd, 0, sizeof nd);
    nd.x = -120; nd.y = -60; nd.w = 40; nd.check = 4; nd.seed = 3; nd.contrast = 0.3f;
    s[4] = ygfx_noise(&nd);
    memset(&gr, 0, sizeof gr);
    gr.y = -60; gr.w = 60; gr.h = 30; gr.sf = 1 / 9.0f; gr.contrast = 0.2f; gr.aperture = YGFX_CIRCLE;
    s[5] = ygfx_grating(&gr);
    gl_frame(s, 6);
    memcpy(scene, R->scene, sizeof R->scene);
    memcpy(out, R->out, sizeof R->out);
    ygfx_texture_free(&R->g, t);
}

static memcache g_mc;   /* the GL parts' desc.cache, per renderer */
static int g_use_mc = 1;
static const ygfx_cache* test_cache(void) {
    static ygfx_cache c;
    c.load = mc_load; c.store = mc_store; c.user = &g_mc;
    return g_use_mc ? &c : NULL;
}

/* The file cache with its keys recorded, so the test can remove its files. */
typedef struct filekeys { ygfx_cache c; const ygfx_cache* inner; uint64_t keys[64]; int n; } filekeys;
static size_t fk_load(void* u, uint64_t key, void* dst, size_t cap) {
    filekeys* f = (filekeys*)u;
    return f->inner->load(f->inner->user, key, dst, cap);
}
static int fk_store(void* u, uint64_t key, const void* data, size_t n) {
    filekeys* f = (filekeys*)u;
    if (f->n < 64) f->keys[f->n++] = key;
    return f->inner->store(f->inner->user, key, data, n);
}
static filekeys g_fk = { { fk_load, fk_store, &g_fk }, NULL, { 0 }, 0 };

static void gl_v04_cache(stats* st) {
    static float ref_s[W * H * 4], s1[W * H * 4];
    static uint8_t ref_o[W * H * 4], o1[W * H * 4];
    static memcache mc;
    ygfx_cache c;
    ygfx_programs ps;
    ygfx_desc gd;
    int k, i, n;
    long before;
    uint64_t t0;
    (void)st;
    c.load = mc_load; c.store = mc_store; c.user = &mc;
    mc_clear(&mc);
    g_use_mc = 0;
    /* the reference: no cache. A cold open costs 3 to 4 s on ANGLE's D3D11:
     * the default takes case 0's frame, compiled while storing, instead */
    if (cs_full()) {
        t0 = yrt_now_ns();
        if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_ORDERED)) { g_use_mc = 1; return; }
        S6.open_cold_ms = (double)(yrt_now_ns() - t0) * 1e-6;
        cache_frame(ref_s, ref_o);
        ygfx_close(&R->g);
    }
    /* cases: 0 cold, 1 warm, 2 payload flipped, 3 warm again, 4 collisions
     * cold, 5 collisions warm, 6 another identity, 7 driver refuses a
     * re-hashed truncated payload */
    for (k = 0; k < 8; k++) {
        ygfx__test_key_mask = (k == 4 || k == 5) ? 0 : ~(uint64_t)0;
        ygfx__test_ident = k >= 6 ? "another driver" : "";
        if (k == 4) mc_clear(&mc);
        if (k == 2) for (i = 0; i < mc.n; i++) mc.data[i][mc.len[i] / 2 + 24] ^= 0x55;
        if (k == 7)
            for (i = 0; i < mc.n; i++) {
                /* a valid header over a binary of another driver build: the
                 * first byte of ANGLE's build hash changed. (Half a binary under
                 * a valid header crashed ANGLE: only the payload hash keeps a
                 * damaged entry from the driver.) */
                ygfx__prog_head h;
                memcpy(&h, mc.data[i], sizeof h);
                mc.data[i][sizeof h] ^= 0x01;
                h.payload_hash = ygfx__fnv64(mc.data[i] + sizeof h, h.payload_bytes);
                memcpy(mc.data[i], &h, sizeof h);
            }
        memset(&gd, 0, sizeof gd);
        gd.screen = &R->scr; gd.dither = YGFX_DITHER_ORDERED; gd.seed = 99; gd.cache = &c;
        ygfx__test_scene32 = 1;
        t0 = yrt_now_ns();
        if (!ygfx_open(&R->g, &gd)) { CHECK(0); continue; }
        if (k == 1) S6.open_warm_ms = (double)(yrt_now_ns() - t0) * 1e-6;
        if (k == 0 && !cs_full()) S6.open_cold_ms = (double)(yrt_now_ns() - t0) * 1e-6;
        ygfx_program_stats(&R->g, &ps);
        n = (int)(ps.loaded + ps.compiled);
        S6.programs = n;
        S6.cache_cases++;
        before = S6.cache_bad;
        switch (k) {
        case 0: case 6: S6.cache_bad += !(ps.compiled == (uint32_t)n && ps.stored == (uint32_t)n && ps.rejected == 0); break;
        case 1: case 3: S6.cache_bad += !(ps.loaded == (uint32_t)n && ps.compiled == 0); break;
        case 2: S6.cache_bad += !(ps.rejected == (uint32_t)n && ps.compiled == (uint32_t)n); break;
        case 4: S6.cache_bad += !(ps.compiled == (uint32_t)n && mc.n >= 1); break;
        case 5: S6.cache_bad += !(ps.loaded == 1 && ps.rejected == (uint32_t)n - 1); break;
        case 7: S6.cache_bad += !(ps.rejected == (uint32_t)n && ps.loaded == 0); break;
        }
        if (k == 1) {   /* a user pipeline: compiled, then loaded */
            ygfx_pipe p = user_pipe(&R->g, "float ysp_main(vec2 p) { return 0.25; }\n");
            ygfx_program_stats(&R->g, &ps);
            S6.cache_bad += !(p.id && ps.compiled == 1 && ps.stored == 1);
        }
        if (k == 2) user_pipe(&R->g, "float ysp_main(vec2 p) { return 0.25; }\n");   /* its entry was damaged too */
        if (k == 3) {
            ygfx_pipe p = user_pipe(&R->g, "float ysp_main(vec2 p) { return 0.25; }\n");
            ygfx_program_stats(&R->g, &ps);
            S6.cache_bad += !(p.id && ps.compiled == 0 && ps.loaded == (uint32_t)n + 1);
        }
        cache_frame(s1, o1);
        if (k == 0 && !cs_full()) { memcpy(ref_s, s1, sizeof s1); memcpy(ref_o, o1, sizeof o1); }
        S6.cache_bad += memcmp(s1, ref_s, sizeof s1) != 0 || memcmp(o1, ref_o, sizeof o1) != 0;
        if (S6.cache_bad != before)
            fprintf(stderr, "  cache case %d: loaded %u compiled %u rejected %u stored %u; frame %s\n", k, ps.loaded, ps.compiled,
                    ps.rejected, ps.stored, memcmp(s1, ref_s, sizeof s1) || memcmp(o1, ref_o, sizeof o1) ? "differs" : "equal");
        ygfx_close(&R->g);
    }
    ygfx__test_key_mask = ~(uint64_t)0;
    ygfx__test_ident = "";
    /* the file cache: cold, warm, and a folder that cannot be made; the
     * files it wrote are removed after */
    {
        ygfx_file_cache fc;
        char dir[300], top[100], path[400];
        FILE* f;
        snprintf(top, sizeof top, "ygfx_test_cache_%u", (unsigned)(yrt_now_ns() & 0xFFFFFF));
        snprintf(dir, sizeof dir, "%s/sub", top);
        g_fk.n = 0;
        for (k = 0; k < (cs_full() ? 3 : 2); k++) {
            memset(&gd, 0, sizeof gd);
            gd.screen = &R->scr; gd.dither = YGFX_DITHER_ORDERED; gd.seed = 99;
            if (k == 2) {   /* a file where the folder would be */
                f = fopen("ygfx_test_cache_file", "wb");
                if (f) fclose(f);
                snprintf(dir, sizeof dir, "ygfx_test_cache_file/sub");
            }
            g_fk.inner = ygfx_file_cache_init(&fc, dir);
            CHECK(g_fk.inner != NULL);
            gd.cache = &g_fk.c;
            if (!ygfx_open(&R->g, &gd)) { CHECK(0); continue; }
            ygfx_program_stats(&R->g, &ps);
            n = (int)(ps.loaded + ps.compiled);
            S6.cache_cases++;
            before = S6.cache_bad;
            if (k == 0) S6.cache_bad += !(ps.compiled == (uint32_t)n && ps.stored == (uint32_t)n);
            if (k == 1) S6.cache_bad += !(ps.loaded == (uint32_t)n);
            if (k == 2) S6.cache_bad += !(ps.store_failed == (uint32_t)n && ps.stored == 0);
            cache_frame(s1, o1);
            S6.cache_bad += memcmp(s1, ref_s, sizeof s1) != 0 || memcmp(o1, ref_o, sizeof o1) != 0;
            if (S6.cache_bad != before)
                fprintf(stderr, "  file cache case %d: loaded %u compiled %u stored %u failed %u\n", k, ps.loaded, ps.compiled,
                        ps.stored, ps.store_failed);
            ygfx_close(&R->g);
        }
        remove("ygfx_test_cache_file");
        for (i = 0; i < g_fk.n; i++) {
            snprintf(path, sizeof path, "%s/sub/%08x%08x.yspprog", top, (unsigned)(g_fk.keys[i] >> 32), (unsigned)(g_fk.keys[i] & 0xFFFFFFFFu));
            remove(path);
        }
        snprintf(path, sizeof path, "%s/sub", top);
        TEST_RMDIR(path);
        TEST_RMDIR(top);
    }
    mc_clear(&mc);
    g_use_mc = 1;
}

/* ------------------------------------------------------------- v0.4 video */

/* A planar frame on the CPU and its light in double, from the definitions
 * (H.273's matrices and ranges, the transfers' formulas), none of the
 * header's arithmetic. */
typedef struct vframe {
    int fmt, w, h;
    uint8_t y[64 * 64], u[64 * 64], v[64 * 64];   /* NV12: u holds CbCr pairs */
    ygfx_encoding e;
} vframe;

static int vf_cw(const vframe* f) { return (f->w + 1) / 2; }
static int vf_ch(const vframe* f) { return (f->h + 1) / 2; }

/* plane 0 Y, 1 Cb, 2 Cr, as values 0..1 */
static double vf_at(const vframe* f, int plane, int x, int y) {
    int w = plane ? vf_cw(f) : f->w, h = plane ? vf_ch(f) : f->h;
    x = x < 0 ? 0 : (x > w - 1 ? w - 1 : x);
    y = y < 0 ? 0 : (y > h - 1 ? h - 1 : y);
    if (plane == 0) return f->y[y * f->w + x] / 255.0;
    if (f->fmt == YGFX_NV12) return f->u[(y * vf_cw(f) + x) * 2 + plane - 1] / 255.0;
    return (plane == 1 ? f->u : f->v)[y * vf_cw(f) + x] / 255.0;
}

static double vf_bilin(const vframe* f, int plane, double cx, double cy) {
    double fx = cx - 0.5, fy = cy - 0.5, ix = floor(fx), iy = floor(fy), wx = fx - ix, wy = fy - iy;
    int x0 = (int)ix, y0 = (int)iy;
    double a = vf_at(f, plane, x0, y0), b = vf_at(f, plane, x0 + 1, y0), c = vf_at(f, plane, x0, y0 + 1), d = vf_at(f, plane, x0 + 1, y0 + 1);
    return (a + (b - a) * wx) * (1 - wy) + (c + (d - c) * wx) * wy;
}

static double trc_ref(double v, int tr) {
    if (tr == YGFX_TRC_BT1886) return pow(v, 2.4);
    if (tr == YGFX_TRC_SRGB) return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    if (tr == YGFX_TRC_GAMMA22) return pow(v, 2.2);
    return v;   /* LINEAR, and DEVICE under an identity CLUT */
}

/* The light of luma point (lx, ly), texel-edge coordinates; M the primaries'
 * matrix (NULL = identity). */
static void vf_ref(const vframe* f, double lx, double ly, int lin, const double* M, double out[3]) {
    const ygfx_encoding* e = &f->e;
    double kr = 0.299, kb = 0.114, kg, Y, cb, cr, rgb[3], l[3];
    double ox = e->siting == YGFX_SITING_LEFT || e->siting == YGFX_SITING_TOP_LEFT ? 0.25 : 0.0;
    double oy = e->siting == YGFX_SITING_TOP_LEFT ? 0.25 : 0.0, cx, cy;
    int lim = e->range == YGFX_RANGE_LIMITED, k;
    if (e->matrix == YGFX_MATRIX_BT709) { kr = 0.2126; kb = 0.0722; }
    if (e->matrix == YGFX_MATRIX_BT2020) { kr = 0.2627; kb = 0.0593; }
    kg = 1 - kr - kb;
    if (!lin) { lx = floor(lx) + 0.5; ly = floor(ly) + 0.5; }
    Y = lin ? vf_bilin(f, 0, lx, ly) : vf_at(f, 0, (int)floor(lx), (int)floor(ly));
    if (e->chroma_nearest) {
        int qx = (int)floor(lx) / 2, qy = (int)floor(ly) / 2;
        cb = vf_at(f, 1, qx, qy); cr = vf_at(f, 2, qx, qy);
    } else {
        cx = lx / 2 + ox; cy = ly / 2 + oy;
        cb = vf_bilin(f, 1, cx, cy); cr = vf_bilin(f, 2, cx, cy);
    }
    Y = lim ? (Y * 255 - 16) / 219 : Y;
    cb = lim ? (cb * 255 - 128) / 224 : (cb * 255 - 128) / 255;
    cr = lim ? (cr * 255 - 128) / 224 : (cr * 255 - 128) / 255;
    rgb[0] = Y + 2 * (1 - kr) * cr;
    rgb[2] = Y + 2 * (1 - kb) * cb;
    rgb[1] = (Y - kr * rgb[0] - kb * rgb[2]) / kg;   /* the luma equation, not the header's form */
    for (k = 0; k < 3; k++) l[k] = trc_ref(rgb[k] < 0 ? 0 : (rgb[k] > 1 ? 1 : rgb[k]), e->transfer);
    for (k = 0; k < 3; k++) out[k] = M ? M[k * 3] * l[0] + M[k * 3 + 1] * l[1] + M[k * 3 + 2] * l[2] : l[k];
}

static uint32_t g_lcg = 12345u;
static int rnd(int lo, int hi) { g_lcg = g_lcg * 1664525u + 1013904223u; return lo + (int)((g_lcg >> 8) % (uint32_t)(hi - lo + 1)); }

static void vf_fill(vframe* f, int fmt, int w, int h) {
    int i, n = ((w + 1) / 2) * ((h + 1) / 2);
    f->fmt = fmt; f->w = w; f->h = h;
    for (i = 0; i < w * h; i++) f->y[i] = (uint8_t)(i % 17 == 0 ? (i % 2 ? 0 : 255) : rnd(16, 235));
    for (i = 0; i < 2 * n; i++) f->u[i] = (uint8_t)(i % 13 == 0 ? (i % 2 ? 0 : 255) : rnd(16, 240));
    for (i = 0; i < n; i++) f->v[i] = (uint8_t)rnd(16, 240);
}

static ygfx_planes vf_planes(const vframe* f) {
    ygfx_planes p;
    memset(&p, 0, sizeof p);
    p.data[0] = f->y; p.data[1] = f->u; p.data[2] = f->v;
    return p;
}

/* Draws frame f at (5, 5) top-left, scale px per texel; the largest |scene -
 * reference| over the image's pixels. */
static double vf_check(ygfx_tex t, const vframe* f, double scale, int lin, const double* M) {
    ygfx_image_desc idd;
    ygfx_stim s;
    double worst = 0, ref[3];
    int i, j, c;
    memset(&idd, 0, sizeof idd);
    idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 5; idd.y = 5;
    idd.w = (float)(f->w * scale); idd.h = (float)(f->h * scale); idd.linear = lin != 0;
    s = ygfx_image(&R->g, &idd);
    gl_frame(&s, 1);
    for (j = 5; j < 5 + (int)(f->h * scale); j++)
        for (i = 5; i < 5 + (int)(f->w * scale); i++) {
            vf_ref(f, (i + 0.5 - 5) / scale, (j + 0.5 - 5) / scale, lin, M, ref);
            for (c = 0; c < 3; c++) worst = maxd(worst, fabs(R->scene[(j * W + i) * 4 + c] - ref[c]));
        }
    return worst;
}

static ygfx_tex vf_texture(const vframe* f) {
    ygfx_texture_desc td;
    ygfx_planes p = vf_planes(f);
    memset(&td, 0, sizeof td);
    td.w = f->w; td.h = f->h; td.format = (ygfx_format)f->fmt; td.enc = f->e; td.planes = &p;
    return ygfx_texture(&R->g, &td);
}

#if defined(_WIN32)
/* ANGLE's D3D11 device through EGL_EXT_device_query, as yscr_native()
 * gives it in a window. */
static void* d3d11_device(void) {
    typedef unsigned (YGFX_HL__API *qa_fn)(void*, int32_t, intptr_t*);
    qa_fn qda = (qa_fn)ygfx_hl__sym(&R->hl, "eglQueryDisplayAttribEXT");
    qa_fn qdev = (qa_fn)ygfx_hl__sym(&R->hl, "eglQueryDeviceAttribEXT");
    intptr_t edev = 0, d3d = 0;
    if (!qda || !qdev || !qda(R->hl.dpy, 0x322C, &edev) || !edev) return NULL;      /* EGL_DEVICE_EXT */
    if (!qdev((void*)edev, 0x33A1, &d3d)) return NULL;                                /* EGL_D3D11_DEVICE_ANGLE */
    return (void*)d3d;
}
#endif

#if defined(_WIN32)
/* COM from C and C++ alike, and the IIDs here so nothing links dxguid. */
#ifdef __cplusplus
    #define TCOM(o, m, ...) ((o)->m(__VA_ARGS__))
    #define TCOM0(o, m)     ((o)->m())
    #define TIID(x)         (x)
#else
    #define TCOM(o, m, ...) ((o)->lpVtbl->m((o), __VA_ARGS__))
    #define TCOM0(o, m)     ((o)->lpVtbl->m(o))
    #define TIID(x)         (&(x))
#endif
#define TREL(o) do { if (o) { TCOM0((o), Release); (o) = NULL; } } while (0)
static const IID t_IID_IDXGIDevice      = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
static const IID t_IID_IDXGIResource    = {0x035f3ab4,0x482e,0x4e50,{0xb4,0x1f,0x8a,0x7f,0x8b,0xd8,0x96,0x0b}};
static const IID t_IID_IDXGIResource1   = {0x30961379,0x4609,0x4a41,{0x99,0x8e,0x54,0xfe,0x56,0x7e,0xe0,0xc1}};
static const IID t_IID_IDXGIKeyedMutex  = {0x9d8e1289,0xd7b3,0x465f,{0x81,0x26,0x25,0x0e,0x34,0x9a,0xf8,0x5d}};
static const IID t_IID_ID3D11Texture2D  = {0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
static const IID t_IID_ID3D11Device1    = {0xa04bfb29,0x08ef,0x43d6,{0xa4,0x9c,0xa9,0xbd,0xbd,0xcb,0xe6,0x86}};

/* The video worker's SHARED path: a second D3D11 device on ANGLE's adapter
 * writes an NV12 frame on its GPU; ANGLE's device opens the texture by its
 * shared handle; ysp_gfx imports it. variant 0: an NT handle with a keyed
 * mutex (the producer releases key 1 after its write, the consumer acquires
 * it before ygfx_end() and gives key 0 back after the frame); variant 1:
 * a legacy shared handle, ordered by the producer's flush and an event query
 * it waits on. The largest |scene - reference|, or -1 when a step failed
 * (*why names it). */
static double d3d11_shared_case(ID3D11Device* dev, int variant, const vframe* fr, const char** why) {
    typedef HRESULT (WINAPI *create_fn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    static uint8_t nv[64 * 36];
    HMODULE lib = GetModuleHandleW(L"d3d11.dll");
    create_fn create = lib ? (create_fn)(yscr_proc)GetProcAddress(lib, "D3D11CreateDevice") : NULL;
    IDXGIDevice* xd = NULL;
    IDXGIAdapter* ad = NULL;
    ID3D11Device* pd = NULL;
    ID3D11DeviceContext* pc = NULL;
    ID3D11Texture2D *pt = NULL, *ct = NULL;
    IDXGIKeyedMutex *pkm = NULL, *ckm = NULL;
    ID3D11Query* q = NULL;
    HANDLE h = NULL;
    D3D11_TEXTURE2D_DESC td;
    double worst = -1;
    int y;
    *why = "";
    memset(nv, 0, sizeof nv);
    for (y = 0; y < fr->h; y++) memcpy(nv + y * 64, fr->y + y * fr->w, (size_t)fr->w);
    for (y = 0; y < vf_ch(fr); y++) memcpy(nv + 64 * 24 + y * 64, fr->u + y * vf_cw(fr) * 2, (size_t)vf_cw(fr) * 2);
    if (!create) { *why = "D3D11CreateDevice"; return -1; }
    if (FAILED(TCOM(dev, QueryInterface, TIID(t_IID_IDXGIDevice), (void**)&xd)) || FAILED(TCOM(xd, GetAdapter, &ad))) { *why = "the adapter"; goto out; }
    if (FAILED(create(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &pd, NULL, &pc))) { *why = "a second device"; goto out; }
    memset(&td, 0, sizeof td);
    td.Width = 38; td.Height = 24; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = variant == 0 ? (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) : D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(TCOM(pd, CreateTexture2D, &td, NULL, &pt))) { *why = "a shared NV12 texture"; goto out; }
    /* the producer's write, on its GPU */
    if (variant == 0) {
        if (FAILED(TCOM(pt, QueryInterface, TIID(t_IID_IDXGIKeyedMutex), (void**)&pkm)) || FAILED(TCOM(pkm, AcquireSync, 0, 1000))) { *why = "the producer's key"; goto out; }
    }
    TCOM(pc, UpdateSubresource, (ID3D11Resource*)pt, 0, NULL, nv, 64, 0);
    if (variant == 0) {
        TCOM(pkm, ReleaseSync, 1);
    } else {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_EVENT; qd.MiscFlags = 0;
        if (FAILED(TCOM(pd, CreateQuery, &qd, &q))) { *why = "an event query"; goto out; }
        TCOM(pc, End, (ID3D11Asynchronous*)q);
        TCOM0(pc, Flush);
        while (TCOM(pc, GetData, (ID3D11Asynchronous*)q, NULL, 0, 0) == S_FALSE) {}
    }
    /* the handle, opened on ANGLE's device */
    if (variant == 0) {
        IDXGIResource1* r1 = NULL;
        ID3D11Device1* d1 = NULL;
        if (FAILED(TCOM(pt, QueryInterface, TIID(t_IID_IDXGIResource1), (void**)&r1)) ||
            FAILED(TCOM(r1, CreateSharedHandle, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h))) {
            TREL(r1); *why = "an NT handle"; goto out;
        }
        TREL(r1);
        if (FAILED(TCOM(dev, QueryInterface, TIID(t_IID_ID3D11Device1), (void**)&d1)) ||
            FAILED(TCOM(d1, OpenSharedResource1, h, TIID(t_IID_ID3D11Texture2D), (void**)&ct))) {
            TREL(d1); *why = "OpenSharedResource1"; goto out;
        }
        TREL(d1);
        if (FAILED(TCOM(ct, QueryInterface, TIID(t_IID_IDXGIKeyedMutex), (void**)&ckm))) { *why = "the consumer's key"; goto out; }
    } else {
        IDXGIResource* r0 = NULL;
        if (FAILED(TCOM(pt, QueryInterface, TIID(t_IID_IDXGIResource), (void**)&r0)) || FAILED(TCOM(r0, GetSharedHandle, &h))) {
            TREL(r0); *why = "a shared handle"; goto out;
        }
        TREL(r0);
        if (FAILED(TCOM(dev, OpenSharedResource, h, TIID(t_IID_ID3D11Texture2D), (void**)&ct))) { h = NULL; *why = "OpenSharedResource"; goto out; }
        h = NULL;   /* a legacy handle is not closed */
    }
    {
        ygfx_import_desc id;
        ygfx_image_desc idd;
        ygfx_tex m;
        ygfx_stim s;
        double ref[3];
        int i2, j2, c;
        memset(&id, 0, sizeof id);
        id.kind = YGFX_IMPORT_D3D11; id.w = 38; id.h = 24; id.format = YGFX_NV12; id.enc = fr->e; id.d3d11_tex = ct;
        m = ygfx_texture_import(&R->g, &id);
        if (!m.id) { *why = "ygfx_texture_import"; goto out; }
        memset(&idd, 0, sizeof idd);
        idd.tex = m; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 5; idd.y = 5;
        idd.src[2] = 37; idd.src[3] = 23;
        s = ygfx_image(&R->g, &idd);
        /* the consumer takes key 1 before ANGLE samples, and gives key 0
         * back after the frame (the readback waited for the GPU) */
        if (ckm && FAILED(TCOM(ckm, AcquireSync, 1, 1000))) { ygfx_texture_free(&R->g, m); *why = "the consumer's AcquireSync"; goto out; }
        gl_frame(&s, 1);
        if (ckm) TCOM(ckm, ReleaseSync, 0);
        worst = 0;
        for (j2 = 5; j2 < 5 + 23; j2++)
            for (i2 = 5; i2 < 5 + 37; i2++) {
                vf_ref(fr, i2 + 0.5 - 5, j2 + 0.5 - 5, 0, NULL, ref);
                for (c = 0; c < 3; c++) worst = maxd(worst, fabs(R->scene[(j2 * W + i2) * 4 + c] - ref[c]));
            }
        ygfx_texture_free(&R->g, m);
    }
out:
    TREL(ckm); TREL(ct); TREL(pkm); TREL(q); TREL(pt); TREL(pc); TREL(pd); TREL(ad); TREL(xd);
    if (h) CloseHandle(h);
    return worst;
}
#endif

static void gl_v04_video(stats* st) {
    static vframe f, f2;
    static ycol_cal cal;
    static const float xy[4][2] = { { 0.600f, 0.370f }, { 0.356f, 0.550f }, { 0.155f, 0.110f }, { 0.3135f, 0.3291f } };   /* the panel's EDID */
    ygfx_tex t, t2;
    int fmt, mat, rng, sit, cn, k, i;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    /* every format, matrix, range, siting and chroma filter at 1:1 */
    for (fmt = YGFX_NV12; fmt <= YGFX_I420; fmt++)
        for (mat = YGFX_MATRIX_BT601; mat <= YGFX_MATRIX_BT2020; mat++)
            for (rng = YGFX_RANGE_LIMITED; rng <= YGFX_RANGE_FULL; rng++)
                for (sit = YGFX_SITING_LEFT; sit <= YGFX_SITING_TOP_LEFT; sit++)
                    for (cn = 0; cn < 2; cn++) {
                        vf_fill(&f, fmt, (mat + sit) % 2 ? 37 : 36, cn ? 23 : 24);
                        memset(&f.e, 0, sizeof f.e);
                        f.e.matrix = (uint8_t)mat; f.e.range = (uint8_t)rng; f.e.transfer = YGFX_TRC_LINEAR;
                        f.e.primaries = YGFX_PRIM_DEVICE; f.e.siting = (uint8_t)sit; f.e.chroma_nearest = (uint8_t)cn;
                        t = vf_texture(&f);
                        CHECK(t.id != 0);
                        S6.vid_1x = maxd(S6.vid_1x, vf_check(t, &f, 1.0, 0, NULL));
                        S6.vid_cases++;
                        ygfx_texture_free(&R->g, t);
                    }
    /* each transfer; the linear path drawn at 2.5x; an RGB texture encoded */
    vf_fill(&f, YGFX_NV12, 37, 23);
    memset(&f.e, 0, sizeof f.e);
    f.e.matrix = YGFX_MATRIX_BT709; f.e.range = YGFX_RANGE_LIMITED; f.e.primaries = YGFX_PRIM_DEVICE; f.e.siting = YGFX_SITING_LEFT;
    for (k = YGFX_TRC_DEVICE; k <= YGFX_TRC_GAMMA22; k++) {
        f.e.transfer = (uint8_t)k;
        t = vf_texture(&f);
        S6.vid_trc = maxd(S6.vid_trc, vf_check(t, &f, 1.0, 0, NULL));
        ygfx_texture_free(&R->g, t);
    }
    f.e.transfer = YGFX_TRC_SRGB;
    t = vf_texture(&f);
    S6.vid_lin = vf_check(t, &f, 2.5, 1, NULL);
    /* a rectangle of one plane updated, then all of them */
    for (i = 0; i < 6 * 4; i++) f.y[(3 + i / 6) * f.w + 10 + i % 6] = (uint8_t)(40 + 7 * i);
    {
        uint8_t blk[6 * 4];
        int q;
        for (q = 0; q < 24; q++) blk[q] = f.y[(3 + q / 6) * f.w + 10 + q % 6];
        CHECK(ygfx_texture_update_plane(&R->g, t, 0, 10, 3, 6, 4, blk, 6) == YGFX_OK);
    }
    S6.vid_part = vf_check(t, &f, 1.0, 0, NULL);
    vf_fill(&f, YGFX_NV12, 37, 23);
    {
        ygfx_planes p = vf_planes(&f);
        CHECK(ygfx_texture_update_planes(&R->g, t, &p) == YGFX_OK);
    }
    S6.vid_part = maxd(S6.vid_part, vf_check(t, &f, 1.0, 0, NULL));
    /* rebind: shows the other's texels, then its own, then its own again
     * when the other goes */
    vf_fill(&f2, YGFX_NV12, 37, 23);
    f2.e = f.e;
    t2 = vf_texture(&f2);
    CHECK(ygfx_texture_rebind(&R->g, t, t2) == YGFX_OK);
    S6.rebind_diff = vf_check(t, &f2, 1.0, 0, NULL);
    CHECK(ygfx_texture_rebind(&R->g, t, t) == YGFX_OK);
    S6.rebind_diff = maxd(S6.rebind_diff, vf_check(t, &f, 1.0, 0, NULL));
    CHECK(ygfx_texture_rebind(&R->g, t, t2) == YGFX_OK);
    ygfx_texture_free(&R->g, t2);
    S6.rebind_diff = maxd(S6.rebind_diff, vf_check(t, &f, 1.0, 0, NULL));
    /* import: the GL name of a texture ysp_gfx made, as an NV12 import; freeing
     * the import leaves the source */
    {
        ygfx__gl* gl = (ygfx__gl*)R->g.bctx;
        ygfx_import_desc id;
        ygfx_tex m;
        memset(&id, 0, sizeof id);
        id.kind = YGFX_IMPORT_GL; id.w = f.w; id.h = f.h; id.format = YGFX_NV12; id.enc = f.e;
        id.gl_tex[0] = gl->tex[R->g.tex[t.id - 1].bid - 1].tex;
        id.gl_tex[1] = gl->tex[R->g.tex[t.id - 1].plane_bid[0] - 1].tex;
        m = ygfx_texture_import(&R->g, &id);
        CHECK(m.id != 0);
        S6.import_gl_diff = vf_check(m, &f, 1.0, 0, NULL);
        ygfx_texture_free(&R->g, m);
        S6.import_gl_diff = maxd(S6.import_gl_diff, vf_check(t, &f, 1.0, 0, NULL));
    }
#if defined(_WIN32)
    if (ygfx_features(&R->g) & YGFX_FEAT_IMPORT_NV12) {   /* the zero-copy path's shape: an NV12 array slice */
        ID3D11Device* dev = (ID3D11Device*)d3d11_device();
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd[2];
        static uint8_t nv[2][64 * 96];
        ID3D11Texture2D* arr = NULL;
        int s2, y;
        for (s2 = 0; s2 < 2; s2++) {
            const vframe* src = s2 ? &f : &f2;   /* slice 1 holds f */
            memset(nv[s2], 0, sizeof nv[s2]);
            for (y = 0; y < src->h; y++) memcpy(nv[s2] + y * 64, src->y + y * src->w, (size_t)src->w);
            for (y = 0; y < vf_ch(src); y++) memcpy(nv[s2] + 64 * 24 + y * 64, src->u + y * vf_cw(src) * 2, (size_t)vf_cw(src) * 2);
            sd[s2].pSysMem = nv[s2]; sd[s2].SysMemPitch = 64; sd[s2].SysMemSlicePitch = 0;
        }
        memset(&td, 0, sizeof td);
        /* NV12 needs even sizes: the frame is the 37 x 23 corner of a 38 x 24 texture */
        td.Width = 38; td.Height = 24; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        /* the chroma plane starts at row 24 of each slice's memory */
        if (dev) {
#ifdef __cplusplus
            dev->CreateTexture2D(&td, sd, &arr);
#else
            dev->lpVtbl->CreateTexture2D(dev, &td, sd, &arr);
#endif
        }
        CHECK(arr != NULL);
        if (arr) {
            ygfx_import_desc id;
            ygfx_tex m;
            ygfx_image_desc idd;
            ygfx_stim s;
            vframe g38 = f;   /* the import is 38 x 24: compare its 37 x 23 corner */
            memset(&id, 0, sizeof id);
            id.kind = YGFX_IMPORT_D3D11; id.w = 38; id.h = 24; id.format = YGFX_NV12; id.enc = f.e;
            id.d3d11_tex = arr; id.d3d11_slice = 1;
            m = ygfx_texture_import(&R->g, &id);
            CHECK(m.id != 0);
            memset(&idd, 0, sizeof idd);
            idd.tex = m; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 5; idd.y = 5;
            idd.src[2] = 37; idd.src[3] = 23;
            s = ygfx_image(&R->g, &idd);
            gl_frame(&s, 1);
            {
                double ref[3];
                int i2, j2, c;
                for (j2 = 5; j2 < 5 + 22; j2++)          /* the last row and column read the padding's chroma */
                    for (i2 = 5; i2 < 5 + 36; i2++) {
                        vf_ref(&g38, i2 + 0.5 - 5, j2 + 0.5 - 5, 0, NULL, ref);
                        for (c = 0; c < 3; c++) S6.import_d3d_diff = maxd(S6.import_d3d_diff, fabs(R->scene[(j2 * W + i2) * 4 + c] - ref[c]));
                    }
            }
            ygfx_texture_free(&R->g, m);
            S6.d3d_ran = 1;
            for (s2 = 0; s2 < 2; s2++) S6.shared_diff[s2] = d3d11_shared_case(dev, s2, &f, &S6.shared_why[s2]);
#ifdef __cplusplus
            arr->Release();
#else
            arr->lpVtbl->Release(arr);
#endif
        }
    }
#endif
    /* v0.10.5: gray textures (R8, RG8) with a stated transfer, as a pack
     * stores an 8-bit gray PNG: the level in r through the transfer's
     * formula, alpha in g over the black background */
    {
        static uint8_t px[128 * 2];
        static const int trs[2] = { YGFX_TRC_SRGB, YGFX_TRC_GAMMA22 };
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        ygfx_stim s;
        ygfx_tex gt;
        int fm, tr, rc[5];
        for (fm = 0; fm < 2; fm++)
            for (tr = 0; tr < 2; tr++) {
                int ch = fm ? 2 : 1;
                for (i = 0; i < 128; i++) {
                    px[i * ch] = (uint8_t)(2 * i + (tr ? 0 : 1));                  /* every code, across the two transfers */
                    if (ch == 2) px[i * 2 + 1] = (uint8_t)(255 - (i * 37) % 256);   /* alphas from 255 down, scattered */
                }
                memset(&td, 0, sizeof td);
                td.w = 128; td.h = 1; td.format = fm ? YGFX_RG8 : YGFX_R8; td.data = px;
                td.enc.matrix = YGFX_MATRIX_RGB; td.enc.range = YGFX_RANGE_FULL; td.enc.transfer = (uint8_t)trs[tr];
                td.enc.primaries = YGFX_PRIM_DEVICE;
                gt = ygfx_texture(&R->g, &td);
                CHECK(gt.id != 0);
                memset(&idd, 0, sizeof idd);
                idd.tex = gt; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.y = 3;
                s = ygfx_image(&R->g, &idd);
                gl_frame(&s, 1);
                for (i = 0; i < 128; i++) {
                    double a = ch == 2 ? px[i * 2 + 1] / 255.0 : 1.0, want = trc_ref(px[i * ch] / 255.0, trs[tr]) * a;
                    int c;
                    for (c = 0; c < 3; c++) S6.gray_diff = maxd(S6.gray_diff, fabs(R->scene[(3 * W + i) * 4 + c] - want));
                }
                S6.gray_cases++;
                ygfx_texture_free(&R->g, gt);
            }
        /* other primaries on gray; an encoding on R16UI; an encoded gray
         * texture as a modulation and as coverage; drawn as a color image */
        memset(&td, 0, sizeof td);
        td.w = 4; td.h = 4; td.format = YGFX_R8;
        td.enc.matrix = YGFX_MATRIX_RGB; td.enc.range = YGFX_RANGE_FULL; td.enc.transfer = YGFX_TRC_SRGB;
        td.enc.primaries = YGFX_PRIM_BT709;
        rc[0] = ygfx_texture(&R->g, &td).id == 0 && strstr(ygfx_error(&R->g), "gray") != NULL;
        td.enc.primaries = YGFX_PRIM_DEVICE; td.format = YGFX_R16UI;
        rc[1] = ygfx_texture(&R->g, &td).id == 0;
        td.format = YGFX_R8;
        gt = ygfx_texture(&R->g, &td);
        CHECK(gt.id != 0);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        memset(&idd, 0, sizeof idd);
        idd.tex = gt; idd.modulation = true;
        s = ygfx_image(&R->g, &idd);
        rc[2] = ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        idd.modulation = false; idd.coverage = true;
        s = ygfx_image(&R->g, &idd);
        rc[3] = ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        idd.coverage = false;
        s = ygfx_image(&R->g, &idd);
        rc[4] = ygfx_draw(&R->g, &s) == YGFX_OK;   /* the control: as a color image */
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        ygfx_texture_free(&R->g, gt);
        for (k = 0; k < 5; k++) {
            S6.gray_refused_n++;
            S6.gray_refused_ok += rc[k];
            if (!rc[k]) fprintf(stderr, "  gray case %d not as the manual says\n", k);
        }
    }
    /* refusals */
    {
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        ygfx_stim s;
        int rc[6];
        memset(&td, 0, sizeof td);
        td.w = 16; td.h = 16; td.format = YGFX_NV12;
        rc[0] = ygfx_texture(&R->g, &td).id == 0;                          /* no encoding */
        td.enc = f.e; td.enc.primaries = YGFX_PRIM_BT709;
        rc[1] = ygfx_texture(&R->g, &td).id == 0 && strstr(ygfx_error(&R->g), "PRIM_DEVICE") != NULL;   /* no calibration */
        td.enc = f.e; td.enc.siting = YGFX_SITING_NONE;
        rc[2] = ygfx_texture(&R->g, &td).id == 0;
        rc[3] = ygfx_texture_update(&R->g, t, 0, 0, 4, 4, f.y, 0) == YGFX_ERR_ARG;
        memset(&idd, 0, sizeof idd);
        idd.tex = t; idd.modulation = true;
        s = ygfx_image(&R->g, &idd);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        rc[4] = ygfx_draw(&R->g, &s) == YGFX_ERR_ARG;
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        td.format = YGFX_I420; td.enc = f.e;
        t2 = ygfx_texture(&R->g, &td);
        rc[5] = ygfx_texture_rebind(&R->g, t, t2) == YGFX_ERR_ARG;
        ygfx_texture_free(&R->g, t2);
        for (k = 0; k < 6; k++) { S6.vid_refused_n++; S6.vid_refused_ok += rc[k]; }
    }
    ygfx_texture_free(&R->g, t);
    ygfx_close(&R->g);
    /* with a calibration: primaries through its chromaticities, and DEVICE
     * codes that come out of the output stage as they went in */
    CHECK(ycol_cal_nominal(&cal, xy, 80.0f, 2.2) == YGFX_OK);
    {
        ygfx_desc gd;
        memset(&gd, 0, sizeof gd);
        gd.screen = &R->scr; gd.cal = &cal; gd.cache = test_cache();
        ygfx__test_scene32 = 1;
        if (!ygfx_open(&R->g, &gd)) { CHECK(0); return; }
    }
    for (k = YGFX_PRIM_BT709; k <= YGFX_PRIM_BT2020; k += 3) {   /* BT.709, BT.2020 */
        static const double pr[2][8] = { { 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 },
                                         { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 } };
        const double* p = pr[k == YGFX_PRIM_BT709 ? 0 : 1];
        double S[9], D[9], Di[9], M[9], ws[3], wd[3], ss[3], sdv[3], yw;
        int a, b2, c;
        /* source RGB to XYZ with white Y = 1; the display's from the
         * calibration, scaled to white Y = 1; M = display^-1 source */
        for (c = 0; c < 3; c++) { S[c] = p[2 * c] / p[2 * c + 1]; S[3 + c] = 1; S[6 + c] = (1 - p[2 * c] - p[2 * c + 1]) / p[2 * c + 1]; }
        ws[0] = p[6] / p[7]; ws[1] = 1; ws[2] = (1 - p[6] - p[7]) / p[7];
        CHECK(ycol__inv3(S, Di));
        ygfx__mul3(Di, ws, ss);
        for (c = 0; c < 9; c++) S[c] *= ss[c % 3];
        yw = cal.rgb_to_xyz[3] + cal.rgb_to_xyz[4] + cal.rgb_to_xyz[5];
        for (c = 0; c < 9; c++) D[c] = cal.rgb_to_xyz[c] / yw;
        CHECK(ycol__inv3(D, Di));
        for (a = 0; a < 3; a++)
            for (b2 = 0; b2 < 3; b2++) { M[a * 3 + b2] = 0; for (c = 0; c < 3; c++) M[a * 3 + b2] += Di[a * 3 + c] * S[c * 3 + b2]; }
        (void)wd; (void)sdv;
        vf_fill(&f, YGFX_I420, 37, 23);
        memset(&f.e, 0, sizeof f.e);
        f.e.matrix = YGFX_MATRIX_BT709; f.e.range = YGFX_RANGE_LIMITED; f.e.transfer = YGFX_TRC_BT1886;
        f.e.primaries = (uint8_t)k; f.e.siting = YGFX_SITING_CENTER;
        t = vf_texture(&f);
        CHECK(t.id != 0);
        S6.vid_prim = maxd(S6.vid_prim, vf_check(t, &f, 1.0, 0, M));
        ygfx_texture_free(&R->g, t);
    }
    {   /* 256 gray codes as an RGB texture with the DEVICE transfer */
        static uint8_t codes[256 * 4];
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        ygfx_stim s;
        for (i = 0; i < 256; i++) { codes[i * 4] = codes[i * 4 + 1] = codes[i * 4 + 2] = (uint8_t)i; codes[i * 4 + 3] = 255; }
        memset(&td, 0, sizeof td);
        td.w = 256; td.h = 1; td.format = YGFX_RGBA8; td.data = codes;
        td.enc.matrix = YGFX_MATRIX_RGB; td.enc.range = YGFX_RANGE_FULL; td.enc.transfer = YGFX_TRC_DEVICE;
        td.enc.primaries = YGFX_PRIM_DEVICE;
        t = ygfx_texture(&R->g, &td);
        CHECK(t.id != 0);
        for (k = 0; k < 2; k++) {   /* columns 0..127, then 128..255 (the image is wider than the scene) */
            memset(&idd, 0, sizeof idd);
            idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.y = 3;
            idd.src[0] = (float)(128 * k); idd.src[2] = 128; idd.src[3] = 1;
            s = ygfx_image(&R->g, &idd);
            gl_frame(&s, 1);
            for (i = 0; i < 128; i++) {
                const uint8_t* o = &R->out[(3 * W + i) * 4];
                int want = 128 * k + i;
                S6.codes_bad += o[0] != want || o[1] != want || o[2] != want;
            }
        }
        ygfx_texture_free(&R->g, t);
    }
    ygfx_close(&R->g);
}

/* ---------------------------------------------------------- v0.4 instances */

/* The vertex shader's cosine and sine of degrees, in float as the GPU runs
 * it, against double (the doc's number). */
static void csd_float(float deg, float* c, float* s) {
    float t = deg / 360.0f, q, r, r2, sv, cv;
    int qi;
    t -= floorf(t + 0.5f);
    q = floorf(t * 4.0f + 0.5f);
    r = (t - 0.25f * q) * 6.28318530717959f;
    r2 = r * r;
    sv = r * (1.0f + r2 * (-0.166666666666667f + r2 * (0.00833333333333333f + r2 * (-0.000198412698412698f + r2 * 2.75573192239859e-6f))));
    cv = 1.0f + r2 * (-0.5f + r2 * (0.0416666666666667f + r2 * (-0.00138888888888889f + r2 * 2.48015873015873e-5f)));
    qi = (int)q & 3;
    *c = qi == 0 ? cv : (qi == 1 ? -sv : (qi == 2 ? -cv : sv));
    *s = qi == 0 ? sv : (qi == 1 ? cv : (qi == 2 ? -sv : -cv));
}

/* Element i drawn alone: the template in a one-point group at the
 * element's anchor, turned and scaled there (the test's own composition,
 * from the manual's definitions). */
static void test_elem(const ygfx_stim* t, int i, ygfx_stim* e, ygfx_group* gr, int mod) {
    const ygfx_inst* it = &t->inst[i];
    const uint32_t f = t->inst_fields;
    const double deg = 3.14159265358979323846 / 180.0;
    float ax, ay;
    double th = t->ori * deg;
    ygfx_resolve(&R->g, t, &ax, &ay);
    *e = *t;
    e->inst = NULL; e->n_inst = 0;
    memset(gr, 0, sizeof *gr);
    gr->place = YGFX_TOP_LEFT; gr->visible = 1; gr->opacity = 1;
    gr->x = ax + ((f & YGFX_I_XY) ? (float)(it->x * cos(th) - it->y * sin(th)) : 0.0f);
    gr->y = ay + ((f & YGFX_I_XY) ? (float)(it->x * sin(th) + it->y * cos(th)) : 0.0f);
    gr->scale = (f & YGFX_I_SCALE) ? it->scale : 1.0f;
    e->group = gr;
    e->place = YGFX_CENTER;
    e->x = e->y = 0;
    e->ori = t->ori + ((f & YGFX_I_ORI) ? it->ori : 0.0f);
    if (f & YGFX_I_PHASE) e->phase += it->phase;
    if (f & YGFX_I_CONTRAST) { e->contrast *= it->contrast; e->opacity *= it->contrast; }
    if (f & YGFX_I_GATE) e->gate *= it->gate;
    if (f & YGFX_I_COLOR) {
        int i0 = (int)floor(it->color), i1 = i0 + 1 < (int)t->n_palette ? i0 + 1 : i0, k;
        double fr = it->color - i0;
        float* d = mod ? e->dir : (t->kind == YGFX_IMAGE ? e->tint : e->color);
        for (k = 0; k < 3; k++) d[k] = (float)((1.0 - fr) * t->palette[3 * i0 + k] + fr * t->palette[3 * i1 + k]);
    }
}

/* The largest |instanced - elements drawn alone| over the scene. */
static double inst_vs_alone(const ygfx_stim* t, int mod) {
    static float a1[W * H * 4];
    ygfx_stim e[32];
    ygfx_group gr[32];
    double worst = 0;
    int i, n = (int)t->n_inst;
    gl_frame(t, 1);
    memcpy(a1, R->scene, sizeof a1);
    for (i = 0; i < n && i < 32; i++) test_elem(t, i, &e[i], &gr[i], mod);
    gl_frame(e, n < 32 ? n : 32);
    for (i = 0; i < W * H * 4; i++) worst = maxd(worst, fabs(a1[i] - R->scene[i]));
    return worst;
}

static void fill_elems(ygfx_inst* a, int nx, int ny, float dx, float dy, int fields, double sc_lo, double sc_hi, int npal) {
    int i;
    ygfx_inst_grid(a, nx, ny, dx, dy);
    for (i = 0; i < nx * ny; i++) {
        a[i].x += (float)rnd(-30, 30) * 0.1f;
        a[i].y += (float)rnd(-30, 30) * 0.1f;
        if (fields & YGFX_I_ORI) a[i].ori = (float)rnd(-3600, 3600) * 0.1f;
        if (fields & YGFX_I_PHASE) a[i].phase = (float)rnd(0, 100) * 0.01f;
        if (fields & YGFX_I_CONTRAST) a[i].contrast = (float)rnd(20, 100) * 0.01f;
        if (fields & YGFX_I_SCALE) a[i].scale = (float)(sc_lo + (sc_hi - sc_lo) * rnd(0, 100) * 0.01);
        if (fields & YGFX_I_GATE) a[i].gate = (float)rnd(30, 100) * 0.01f;
        if (fields & YGFX_I_COLOR) a[i].color = i % 3 == 0 ? (float)rnd(0, npal - 1) : (float)rnd(0, (npal - 1) * 100) * 0.01f;
    }
}

static void gl_v04_inst(stats* st) {
    static ygfx_inst el[9000], el2[9000];
    static const float pal[3 * 6] = { 0.9f, 0.1f, 0.1f, 0.1f, 0.8f, 0.2f, 0.2f, 0.2f, 0.9f, 0.7f, 0.7f, 0.1f,
                                      0.05f, 0.6f, 0.6f, 0.95f, 0.95f, 0.95f };
    static const float dirs[3 * 3] = { 0.4f, 0.4f, 0.4f, 0.4f, -0.3f, 0.1f, 0.1f, 0.2f, -0.4f };
    ygfx_instances_desc id;
    ygfx_stim t, s;
    ygfx_tex tex;
    const uint32_t ALL = YGFX_I_XY | YGFX_I_ORI | YGFX_I_CONTRAST | YGFX_I_SCALE | YGFX_I_GATE | YGFX_I_COLOR;
    int i, k;
    (void)st;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&id, 0, sizeof id);
    id.inst = el; id.n = 24; id.palette = pal; id.n_palette = 6;
    /* gabors: every field, dir from the palette */
    {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.ori = 10; d.sf = 1 / 7.0f; d.sigma = 5; d.contrast = 0.5f;
        t = ygfx_gabor(&d);
        id.palette = dirs; id.n_palette = 3;
        id.fields = ALL | YGFX_I_PHASE;
        fill_elems(el, 6, 4, 50, 48, (int)id.fields, 0.6, 1.4, 3);
        s = ygfx_instances(&R->g, &t, &id);
        CHECK(s.n_inst == 24);
        S6.inst_diff[0] = inst_vs_alone(&s, 1);
        {   /* two instanced stimuli in one frame read their own elements */
            static float a2[W * H * 4];
            ygfx_stim two[2], e[24];
            ygfx_group gr[24];
            two[0] = s;
            two[0].n_inst = 12;
            two[1] = s;
            two[1].inst = el + 12;
            two[1].n_inst = 12;
            gl_frame(two, 2);
            memcpy(a2, R->scene, sizeof a2);
            for (i = 0; i < 12; i++) { test_elem(&two[0], i, &e[i], &gr[i], 1); test_elem(&two[1], i, &e[12 + i], &gr[12 + i], 1); }
            gl_frame(e, 24);
            for (i = 0; i < W * H * 4; i++) S6.inst_diff[0] = maxd(S6.inst_diff[0], fabs(a2[i] - R->scene[i]));
        }
        id.palette = pal; id.n_palette = 6;
    }
    {   /* a grating in a cosine-edged circle; noise (no scale) */
        ygfx_grating_desc d;
        ygfx_noise_desc nd;
        memset(&d, 0, sizeof d);
        d.w = 30; d.sf = 1 / 6.0f; d.contrast = 0.4f; d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 3; d.ori = -20;
        t = ygfx_grating(&d);
        id.fields = YGFX_I_XY | YGFX_I_ORI | YGFX_I_PHASE | YGFX_I_SCALE | YGFX_I_CONTRAST;
        fill_elems(el, 6, 4, 50, 48, (int)id.fields, 0.7, 1.3, 6);
        s = ygfx_instances(&R->g, &t, &id);
        S6.inst_diff[1] = inst_vs_alone(&s, 1);
        memset(&nd, 0, sizeof nd);
        nd.w = 30; nd.check = 3; nd.seed = 5; nd.contrast = 0.3f; nd.aperture = YGFX_RECT;
        t = ygfx_noise(&nd);
        id.fields = YGFX_I_XY | YGFX_I_ORI | YGFX_I_GATE;
        fill_elems(el, 6, 4, 50, 48, (int)id.fields, 1, 1, 6);
        s = ygfx_instances(&R->g, &t, &id);
        S6.inst_diff[1] = maxd(S6.inst_diff[1], inst_vs_alone(&s, 1));
    }
    {   /* shapes: a stroked v0.2 cross, a vector RRECT with an offset */
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_CROSS; d.w = 30; d.h = 24; d.shape_p[0] = 6; d.stroke = 2; d.edge = YGFX_EDGE_GAUSSIAN; d.edge_width = 1.0f;
        d.color[0] = 0.5f; d.color[1] = 0.5f; d.color[2] = 0.5f; d.ori = 15;
        t = ygfx_shape(&d);
        id.fields = ALL;
        fill_elems(el, 6, 4, 50, 48, (int)id.fields, 0.7, 1.3, 6);
        s = ygfx_instances(&R->g, &t, &id);
        S6.inst_diff[2] = inst_vs_alone(&s, 0);
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RRECT; d.w = 30; d.h = 20; d.shape_p[0] = 2; d.shape_p[1] = 6; d.shape_p[2] = 0; d.shape_p[3] = 4;
        d.offset = 1; d.edge = YGFX_EDGE_COSINE; d.edge_width = 2; d.color[1] = 0.7f;
        t = ygfx_shape(&d);
        s = ygfx_instances(&R->g, &t, &id);
        CHECK(s.n_inst == 24);
        S6.inst_diff[2] = maxd(S6.inst_diff[2], inst_vs_alone(&s, 0));
    }
    {   /* an image, linear, tinted by the palette */
        static unsigned char px[8 * 8 * 4];
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        for (i = 0; i < 8 * 8 * 4; i++) px[i] = (unsigned char)(i % 4 == 3 ? 255 : (i * 37) % 251);
        memset(&td, 0, sizeof td);
        td.w = 8; td.h = 8; td.format = YGFX_RGBA8; td.data = px;
        tex = ygfx_texture(&R->g, &td);
        memset(&idd, 0, sizeof idd);
        idd.tex = tex; idd.w = 24; idd.h = 24; idd.linear = true;
        t = ygfx_image(&R->g, &idd);
        id.fields = ALL;
        fill_elems(el, 6, 4, 50, 48, (int)id.fields, 0.7, 1.3, 6);
        s = ygfx_instances(&R->g, &t, &id);
        S6.inst_diff[3] = inst_vs_alone(&s, 0);
        ygfx_texture_free(&R->g, tex);
    }
    {   /* a user shader sees the same local coordinates and size either way */
        ygfx_user_desc ud;
        ygfx_pipeline_desc pd;
        ygfx_pipe p;
        memset(&pd, 0, sizeof pd);
        pd.mode = YGFX_COLOR;
        pd.body = "vec4 ysp_main(vec2 p) {\n"
                  "    return vec4(0.5 + 0.4 * cos(3.0 * p.x / ysp_size.x), 0.5 + 0.02 * p.y, ysp_color.b, 1.0);\n"
                  "}\n";
        p = ygfx_pipeline(&R->g, &pd);
        if (!p.id) fprintf(stderr, "user pipeline: %s\n", ygfx_error(&R->g));
        CHECK(p.id != 0);
        memset(&ud, 0, sizeof ud);
        ud.pipe = p; ud.w = 30; ud.h = 22; ud.aperture = YGFX_CIRCLE; ud.edge = YGFX_EDGE_COSINE; ud.edge_width = 2;
        ud.color[2] = 0.3f; ud.ori = 25;
        t = ygfx_user(&ud);
        t.ax = 0.3f; t.ay = 0.6f;   /* turned and scaled about a corner-ward anchor */
        id.fields = YGFX_I_XY | YGFX_I_ORI | YGFX_I_SCALE | YGFX_I_COLOR;
        fill_elems(el, 6, 4, 50, 48, (int)id.fields, 0.5, 1.5, 6);
        s = ygfx_instances(&R->g, &t, &id);
        CHECK(s.n_inst == 24);
        S6.inst_diff[4] = inst_vs_alone(&s, 0);
        ygfx_pipeline_free(&R->g, p);
    }
    {   /* integer palette entries exactly; fractions mixed in linear light */
        ygfx_shape_desc d;
        double fr_worst = 0;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RECT; d.w = 20; d.h = 20; d.color[0] = 1;
        t = ygfx_shape(&d);
        id.fields = YGFX_I_XY | YGFX_I_COLOR;
        ygfx_inst_grid(el, 8, 4, 36, 40);
        for (i = 0; i < 32; i++) el[i].color = i < 16 ? (float)(i % 6) : (float)(i % 5) + 0.37f;
        id.n = 32;
        s = ygfx_instances(&R->g, &t, &id);
        gl_frame(&s, 1);
        for (i = 0; i < 32; i++) {
            float x, y;
            const float* px;
            ygfx_inst_resolve(&R->g, &s, i, &x, &y);
            px = &R->scene[((int)y * W + (int)x) * 4];
            for (k = 0; k < 3; k++) {
                if (i < 16) S6.pal_bad += px[k] != pal[3 * (i % 6) + k];
                else {
                    int i0 = i % 5;
                    fr_worst = maxd(fr_worst, fabs(px[k] - ((1 - 0.37) * pal[3 * i0 + k] + 0.37 * pal[3 * i0 + 3 + k])));
                }
            }
        }
        S6.pal_frac = fr_worst;
        id.n = 24;
    }
    {   /* ygfx_hit_index against the GPU: 16 overlapping hard discs, one
         * palette entry each (the scene's color names the element), one with
         * gate 0 */
        static const float pal16[3 * 16] = { 0.1f, 0.2f, 0.3f, 0.2f, 0.3f, 0.4f, 0.3f, 0.4f, 0.5f, 0.4f, 0.5f, 0.6f, 0.5f, 0.6f, 0.7f,
                                             0.6f, 0.7f, 0.8f, 0.7f, 0.8f, 0.9f, 0.8f, 0.9f, 0.1f, 0.9f, 0.1f, 0.2f, 0.15f, 0.25f, 0.35f,
                                             0.25f, 0.35f, 0.45f, 0.35f, 0.45f, 0.55f, 0.45f, 0.55f, 0.65f, 0.55f, 0.65f, 0.75f,
                                             0.65f, 0.75f, 0.85f, 0.75f, 0.85f, 0.95f };
        ygfx_shape_desc d;
        int j;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RECT; d.w = 60; d.h = 36;
        t = ygfx_shape(&d);
        id.fields = YGFX_I_XY | YGFX_I_ORI | YGFX_I_SCALE | YGFX_I_COLOR | YGFX_I_GATE;
        id.palette = pal16; id.n_palette = 16; id.n = 16;
        for (i = 0; i < 16; i++) {
            memset(&el[i], 0, sizeof el[i]);
            el[i].x = (float)rnd(-100, 100); el[i].y = (float)rnd(-60, 60); el[i].ori = (float)rnd(0, 359);
            el[i].scale = (float)rnd(60, 160) * 0.01f; el[i].color = (float)i; el[i].gate = i == 7 ? 0.0f : 1.0f; el[i].contrast = 1;
        }
        s = ygfx_instances(&R->g, &t, &id);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                const float* px = &R->scene[(j * W + i) * 4];
                int h = ygfx_hit_index(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f), want = -1, q;
                /* a pixel within 1e-3 px of an edge is a tie */
                if (ygfx_hit_index(&R->g, &s, (float)i + 0.501f, (float)j + 0.5f) != h ||
                    ygfx_hit_index(&R->g, &s, (float)i + 0.499f, (float)j + 0.5f) != h ||
                    ygfx_hit_index(&R->g, &s, (float)i + 0.5f, (float)j + 0.501f) != h ||
                    ygfx_hit_index(&R->g, &s, (float)i + 0.5f, (float)j + 0.499f) != h) { S6.hit_ties++; continue; }
                for (q = 0; q < 16; q++)
                    if (px[0] == pal16[3 * q] && px[1] == pal16[3 * q + 1] && px[2] == pal16[3 * q + 2]) want = q;
                S6.hit_bad += h != want;
                S6.hit_n += h >= 0;
            }
        id.palette = pal; id.n_palette = 6; id.n = 24;
    }
    {   /* bindings: the template's field moves all, an element's field one */
        ygfx_gabor_desc d;
        ygfx_bind b[2];
        float v[2] = { 0.25f, 33.0f };
        float x0, y0, x1, y1;
        memset(&d, 0, sizeof d);
        d.sf = 0.1f; d.sigma = 4; d.contrast = 0.5f;
        t = ygfx_gabor(&d);
        id.fields = YGFX_I_XY;
        ygfx_inst_grid(el, 4, 1, 40, 0);
        id.n = 4;
        s = ygfx_instances(&R->g, &t, &id);
        memset(b, 0, sizeof b);
        b[0].stim = &s; b[0].param = YGFX_P_CONTRAST; b[0].channel = 0;
        b[1].field = &el[2].x; b[1].channel = 1;
        ygfx_inst_resolve(&R->g, &s, 2, &x0, &y0);
        CHECK(ygfx_apply(b, 2, v) == 2);
        ygfx_inst_resolve(&R->g, &s, 2, &x1, &y1);
        S6.bind_bad += s.contrast != 0.25f || fabs((x1 - x0) - (33.0f - 20.0f)) > 1e-4;
        id.n = 24;
    }
    {   /* refusals; capacity is per frame and never truncates */
        ygfx_dots_desc dd;
        ygfx_shape_desc d;
        ygfx_noise_desc nd;
        ygfx_compound_desc cd;
        ygfx_prim pr[8];
        int rc[8];
        memset(&dd, 0, sizeof dd);
        t = ygfx_dots(&dd);
        rc[0] = ygfx_instances(&R->g, &t, &id).n_inst == 0;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_CIRCLE; d.w = 10;
        t = ygfx_shape(&d);
        id.fields = YGFX_I_PHASE;
        rc[1] = ygfx_instances(&R->g, &t, &id).n_inst == 0;
        id.fields = YGFX_I_COLOR; id.palette = NULL;
        rc[2] = ygfx_instances(&R->g, &t, &id).n_inst == 0;
        id.palette = pal;
        memset(&nd, 0, sizeof nd);
        nd.w = 10;
        t = ygfx_noise(&nd);
        id.fields = YGFX_I_SCALE;
        rc[3] = ygfx_instances(&R->g, &t, &id).n_inst == 0;
        memset(pr, 0, sizeof pr);
        for (i = 0; i < 8; i++) { pr[i].shape = YGFX_CIRCLE; pr[i].x = (float)(i * 5); pr[i].w = 6; }
        memset(&cd, 0, sizeof cd);
        cd.prims = pr; cd.n = 8; cd.w = 50; cd.h = 10;
        t = ygfx_compound(&cd);
        id.fields = YGFX_I_XY;
        rc[4] = ygfx_instances(&R->g, &t, &id).n_inst == 0;   /* extension blocks */
        id.n = 20000;
        t = ygfx_shape(&d);
        rc[5] = ygfx_instances(&R->g, &t, &id).n_inst == 0;   /* past max_instances */
        {   /* 9000 + 9000 > 16384 in one frame */
            ygfx_stim a, b2;
            id.n = 9000; id.inst = el;
            ygfx_inst_grid(el, 90, 100, 1, 1);
            a = ygfx_instances(&R->g, &t, &id);
            id.inst = el2;
            ygfx_inst_grid(el2, 90, 100, 1, 1);
            b2 = ygfx_instances(&R->g, &t, &id);
            CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
            CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
            rc[6] = ygfx_draw(&R->g, &a) == YGFX_OK;
            rc[7] = ygfx_draw(&R->g, &b2) == YGFX_ERR_FULL && strstr(ygfx_error(&R->g), "max_instances") != NULL;
            CHECK(ygfx_end(&R->g) == YGFX_OK);
            CHECK(yscr_flip(&R->scr) == YSCR_OK);
            id.inst = el;
        }
        for (k = 0; k < 8; k++) { S6.inst_refused_n++; S6.inst_refused_ok += rc[k]; if (!rc[k]) fprintf(stderr, "  refusal %d not made\n", k); }
    }
    ygfx_close(&R->g);
}

/* The vertex shader's cosine and sine of degrees against double. */
static void test_v04_inst_cpu(void) {
    double worst = 0;
    long k;
    ygfx_inst a[6];
    for (k = -720000; k <= 720000; k++) {
        float c, s, deg = (float)k * 0.001f;
        double r = (double)deg * (3.14159265358979323846 / 180.0);
        csd_float(deg, &c, &s);
        worst = maxd(worst, maxd(fabs(c - cos(r)), fabs(s - sin(r))));
    }
    S6.csd_err = worst;
    CHECK_LE(worst, 1e-6);
    ygfx_inst_grid(a, 3, 2, 10, 4);
    CHECK(a[0].x == -10 && a[0].y == -2 && a[5].x == 10 && a[5].y == 2 && a[4].scale == 1 && a[4].gate == 1 && a[4].color == 0);
}

/* ------------------------------------------------------------ v0.4 order */

/* One frame of n stimuli, in call order (reorder off) and reordered: the
 * scene and the output codes must be equal bit for bit. Returns the draw
 * calls saved; *bad counts the frames that differed. */
static long order_frame(const ygfx_stim* s, int n, long* bad) {
    static float a1[W * H * 4];
    static uint8_t o1[W * H * 4];
    uint64_t d0, d1, d2;
    d0 = R->g.draws;
    R->g.no_reorder = 1;
    gl_frame(s, n);
    d1 = R->g.draws;
    memcpy(a1, R->scene, sizeof a1);
    memcpy(o1, R->out, sizeof o1);
    R->g.no_reorder = 0;
    gl_frame(s, n);
    d2 = R->g.draws;
    *bad += memcmp(a1, R->scene, sizeof a1) != 0 || memcmp(o1, R->out, sizeof o1) != 0;
    return (long)((d1 - d0) - (d2 - d1));
}

/* A stimulus of kind k (0 gabor, 1 soft circle, 2 RRECT, 3 grating, 4
 * image) at x, y from the top-left, turned. */
static ygfx_stim order_stim(int k, float x, float y, float ori, ygfx_tex tex) {
    ygfx_stim s;
    switch (k) {
    case 0: {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.sf = 1 / 6.0f; d.sigma = 4; d.contrast = 0.3f; d.ori = ori;
        s = ygfx_gabor(&d);
        break;
    }
    case 1: {
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.shape = YGFX_CIRCLE; d.w = 18; d.edge = YGFX_EDGE_GAUSSIAN;
        d.edge_width = 1.5f; d.color[0] = 0.9f; d.color[1] = 0.3f; d.color[2] = 0.2f; d.opacity = 0.8f;
        s = ygfx_shape(&d);
        break;
    }
    case 2: {
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.shape = YGFX_RRECT; d.w = 22; d.h = 12; d.ori = ori;
        d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 3; d.edge = YGFX_EDGE_COSINE; d.edge_width = 2;
        d.color[0] = 0.2f; d.color[1] = 0.4f; d.color[2] = 0.9f; d.opacity = 0.7f;
        s = ygfx_shape(&d);
        break;
    }
    case 3: {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 20; d.sf = 1 / 5.0f; d.contrast = 0.25f; d.aperture = YGFX_CIRCLE;
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 2; d.ori = ori;
        s = ygfx_grating(&d);
        break;
    }
    default: {
        ygfx_image_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.tex = tex; d.x = x; d.y = y; d.w = 16; d.h = 16; d.ori = ori; d.linear = true; d.opacity = 0.9f;
        s = ygfx_image(&R->g, &d);
        break;
    }
    }
    return s;
}

static void gl_v04_order(stats* st) {
    static ygfx_stim s[400];
    static unsigned char px[4 * 4 * 4];
    ygfx_texture_desc td;
    ygfx_tex tex;
    int i, r;
    (void)st;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_ORDERED)) return;
    for (i = 0; i < 64; i++) px[i] = (unsigned char)(i % 4 == 3 ? 200 : i * 13);
    memset(&td, 0, sizeof td);
    td.w = 4; td.h = 4; td.format = YGFX_RGBA8; td.data = px;
    tex = ygfx_texture(&R->g, &td);
    /* sparse (a grid: nothing overlaps) and dense (random: most overlap) */
    for (r = 0; r < 6; r++) {
        int n = r < 3 ? 160 : 400;
        for (i = 0; i < n; i++) {
            float x, y;
            if (r < 3) { x = 12.0f + (float)(i % 16) * 19.0f; y = 12.0f + (float)(i / 16) * 19.0f; }
            else { x = (float)rnd(0, W); y = (float)rnd(0, H); }
            s[i] = order_stim(rnd(0, 4), x, y, (float)rnd(0, 359), tex);
        }
        S6.order_saved += order_frame(s, n, &S6.order_bad);
        S6.order_frames++;
    }
    /* a draw that passes several others and joins an earlier run: the
     * second circle overlaps the gabor three draws back, not its neighbor;
     * it must stay after the gabor */
    s[0] = order_stim(1, 40, 40, 0, tex);
    s[1] = order_stim(0, 120, 40, 0, tex);
    s[2] = order_stim(2, 200, 40, 0, tex);
    s[3] = order_stim(3, 260, 40, 0, tex);
    s[4] = order_stim(1, 126, 44, 0, tex);   /* on the gabor: a new run */
    s[5] = order_stim(2, 200, 120, 0, tex);  /* free: passes two, joins the RRECT */
    s[6] = order_stim(1, 40, 120, 0, tex);   /* joins s[4]'s run */
    {
        long saved = order_frame(s, 7, &S6.order_bad);
        S6.order_frames++;
        S6.order_pass_ok += saved == 2;      /* 7 draws become 5 */
    }
    /* fringes that meet at the bounds' edge: a Gaussian-edged circle next
     * to an RRECT, their quads from overlapping to touching, the soft
     * edges reaching into the shared pixels */
    for (r = 0; r < 9; r++) {
        /* the RRECT's quad reaches 13 px from its center, the circle's 17.5
         * (radius 9 + 5 SD + 1 px): they meet at 30.5 px; from 24 to 32 px
         * their soft edges share pixels or the quads' margins touch */
        float d = 24.0f + (float)r;
        s[0] = order_stim(1, 40, 160, 0, tex);
        s[1] = order_stim(2, 120, 100, 0, tex);
        s[2] = order_stim(1, 120 + d, 100.5f, 0, tex);
        S6.order_saved += order_frame(s, 3, &S6.order_bad);
        S6.order_frames++;
    }
    /* a turned box's bounds: an RRECT 40 x 40 at 45 degrees reaches 0.707
     * x its diagonal across, more than its half width; its corner touches
     * the circle's fringe, and a free copy of it far away is the run it
     * may not join */
    for (r = 0; r < 4; r++) {
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.shape = YGFX_RRECT; d.w = 40; d.h = 40; d.ori = 45;
        d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 1; d.edge = YGFX_EDGE_COSINE; d.edge_width = 2;
        d.color[1] = 0.8f; d.opacity = 0.6f;
        d.x = 40; d.y = 150;
        s[0] = ygfx_shape(&d);
        s[1] = order_stim(1, 120, 100, 0, tex);
        d.x = 120.0f + 42.0f + (float)r; d.y = 100;
        s[2] = ygfx_shape(&d);
        S6.order_saved += order_frame(s, 3, &S6.order_bad);
        S6.order_frames++;
    }
    ygfx_texture_free(&R->g, tex);
    ygfx_close(&R->g);
}

/* The calibration recipe of the manual, as written: derive() seals the CRC,
 * so check() and open take it with no save() between. */
static void test_v04_fixes_cpu(void) {
    static ycol_cal c;
    static float s[81];
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    char err[200];
    int i;
    for (i = 0; i < 81; i++) s[i] = 0.01f + 0.001f * (float)i;
    CHECK(ycol_cal_nominal(&c, xy, 80.0f, 2.2) == YGFX_OK);
    CHECK(ycol_cal_set_spectra(&c, 380, 5, 81, s, s, s, NULL) == YGFX_OK);
    CHECK(c.crc == 0);                       /* unsealed by the change */
    CHECK(ycol_cal_derive(&c, err, sizeof err) == YGFX_OK);
    CHECK(ycol_cal_check(&c, err, sizeof err) == YGFX_OK);
    CHECK(c.crc == ygfx__crc32(&c, offsetof(ycol_cal, crc)));   /* sealed, not left at 0 */
    {
        static ygfx_gfx g;
        static yscr_screen scr;
        yscr_desc d;
        ygfx_desc gd;
        memset(&d, 0, sizeof d);
        d.backend = YSCR_BACKEND_SIM;
        CHECK(yscr_open(&scr, &d));
        memset(&gd, 0, sizeof gd);
        gd.screen = &scr; gd.width = 64; gd.height = 64; gd.cal = &c;
        CHECK(ygfx_open(&g, &gd));
        CHECK(ygfx_calibrated(&g) && ygfx_screen(&g) == &scr);
        ygfx_close(&g);
        CHECK(!ygfx_calibrated(&g) && ygfx_screen(&g) == NULL);
        yscr_close(&scr);
    }
    c.readings[3].Y *= 1.01f;               /* changed by hand after derive */
    CHECK(ycol_cal_check(&c, err, sizeof err) == YGFX_ERR_FORMAT);
}

/* The gallery's findings: straight-alpha sprites filtered linearly, the
 * range rule on DIST atlases, a run from its first record, apertures'
 * shape_p in the descs. */
static void gl_v04_fixes(stats* st) {
    static float a1[W * H * 4];
    static unsigned char spr[4 * 4 * 4];
    ygfx_texture_desc td;
    ygfx_tex t;
    ygfx_image_desc idd;
    ygfx_stim s;
    int i, j, k;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    /* a 2 x 2 opaque middle, a clear border, one half-clear border texel */
    memset(spr, 0, sizeof spr);
    for (j = 1; j < 3; j++)
        for (i = 1; i < 3; i++) { unsigned char* q = &spr[(j * 4 + i) * 4]; q[0] = 200; q[1] = 100; q[2] = 50; q[3] = 255; }
    spr[(0 * 4 + 1) * 4 + 0] = 255; spr[(0 * 4 + 1) * 4 + 1] = 255; spr[(0 * 4 + 1) * 4 + 2] = 255; spr[(0 * 4 + 1) * 4 + 3] = 64;
    memset(&td, 0, sizeof td);
    td.w = 4; td.h = 4; td.format = YGFX_RGBA8; td.data = spr;
    t = ygfx_texture(&R->g, &td);
    memset(&idd, 0, sizeof idd);
    idd.tex = t; idd.place = YGFX_TOP_LEFT; idd.anchor = YGFX_TOP_LEFT; idd.x = 10; idd.y = 10; idd.w = 64; idd.h = 64;
    idd.linear = true;
    s = ygfx_image(&R->g, &idd);
    gl_frame(&s, 1);
    for (j = 10; j < 74; j++)
        for (i = 10; i < 74; i++) {
            /* the scene over black is the premultiplied color: the bilinear of
             * rgb x alpha, clamped to the texture */
            double u = (i + 0.5 - 10) / 16.0, v = (j + 0.5 - 10) / 16.0;
            double fx = u - 0.5, fy = v - 0.5, ix = floor(fx), iy = floor(fy), wx = fx - ix, wy = fy - iy;
            int x0 = (int)ix, y0 = (int)iy, x1 = x0 + 1, y1 = y0 + 1, c;
            x0 = x0 < 0 ? 0 : (x0 > 3 ? 3 : x0); x1 = x1 < 0 ? 0 : (x1 > 3 ? 3 : x1);
            y0 = y0 < 0 ? 0 : (y0 > 3 ? 3 : y0); y1 = y1 < 0 ? 0 : (y1 > 3 ? 3 : y1);
            for (c = 0; c < 3; c++) {
                double p[4], st_[4], a[4], want, old;
                int q, xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
                for (q = 0; q < 4; q++) {
                    const unsigned char* tx = &spr[(ys[q] * 4 + xs[q]) * 4];
                    a[q] = tx[3] / 255.0; st_[q] = tx[c] / 255.0; p[q] = st_[q] * a[q];
                }
                want = (p[0] + (p[1] - p[0]) * wx) * (1 - wy) + (p[2] + (p[3] - p[2]) * wx) * wy;
                /* v0.3's: the straight colors and the alphas apart, then multiplied */
                old = ((st_[0] + (st_[1] - st_[0]) * wx) * (1 - wy) + (st_[2] + (st_[3] - st_[2]) * wx) * wy) *
                      ((a[0] + (a[1] - a[0]) * wx) * (1 - wy) + (a[2] + (a[3] - a[2]) * wx) * wy);
                S6.alpha_lin = maxd(S6.alpha_lin, fabs(R->scene[(j * W + i) * 4 + c] - want));
                S6.alpha_old = maxd(S6.alpha_old, fabs(old - want));
            }
        }
    ygfx_texture_free(&R->g, t);
    {   /* DIST atlases: the range rule, and a run from its first record */
        enum { AW = 64 };
        static float dist[AW * AW];
        static ygfx_glyph gl4[4];
        static float dots[10 * 2];
        ygfx_glyphs_desc gd;
        ygfx_dots_desc dd;
        ygfx_shape_desc sd;
        ygfx_buf b4, b2, d10, d5;
        ygfx_tex nr, wr;
        ygfx_stim r[3];
        int rc[6];
        for (j = 0; j < AW; j++)
            for (i = 0; i < AW; i++) {   /* two discs of radius 8 texels, 16 apart, in 32 x 64 cells */
                double cx = i < 32 ? 16 : 48, x = i + 0.5 - cx, y = j + 0.5 - 32;
                dist[j * AW + i] = (float)(sqrt(x * x + y * y) - 8.0);
            }
        memset(&td, 0, sizeof td);
        td.w = AW; td.h = AW; td.format = YGFX_R32F; td.data = dist;
        nr = ygfx_texture(&R->g, &td);              /* no range stated */
        td.sdf = YGFX_SDF_DIST; td.sdf_range = 16;  /* padding 8 texels */
        wr = ygfx_texture(&R->g, &td);
        memset(gl4, 0, sizeof gl4);
        for (k = 0; k < 4; k++) { gl4[k].x = (float)(k * 40); gl4[k].y = (float)(k * 5); gl4[k].sx = (float)(k % 2 * 32); gl4[k].sw = 32; gl4[k].sh = 64; }
        b4 = ygfx_buffer(&R->g, sizeof gl4);
        b2 = ygfx_buffer(&R->g, 2 * sizeof gl4[0]);
        CHECK(ygfx_buffer_update(&R->g, b4, 0, gl4, sizeof gl4) == YGFX_OK);
        CHECK(ygfx_buffer_update(&R->g, b2, 0, gl4 + 2, 2 * sizeof gl4[0]) == YGFX_OK);
        memset(&gd, 0, sizeof gd);
        gd.atlas = wr; gd.buf = b4; gd.first = 2; gd.count = 2; gd.scale = 1; gd.place = YGFX_TOP_LEFT; gd.anchor = YGFX_TOP_LEFT;
        gd.x = 20; gd.y = 30; gd.w = 160; gd.h = 80; gd.edge = YGFX_EDGE_COSINE; gd.edge_width = 2; gd.color[0] = 1;
        r[0] = ygfx_glyphs(&gd);
        gd.buf = b2; gd.first = 0;
        r[1] = ygfx_glyphs(&gd);
        gl_frame(&r[0], 1);
        memcpy(a1, R->scene, sizeof a1);
        gl_frame(&r[1], 1);
        for (i = 0; i < W * H * 4; i++) S6.first_diff = maxd(S6.first_diff, fabs(a1[i] - R->scene[i]));
        /* the same with dots: 5 from the sixth against a buffer of those 5 */
        for (i = 0; i < 20; i++) dots[i] = (float)(i * 7 % 50) - 25.0f;
        d10 = ygfx_buffer(&R->g, sizeof dots);
        d5 = ygfx_buffer(&R->g, sizeof dots / 2);
        CHECK(ygfx_buffer_update(&R->g, d10, 0, dots, sizeof dots) == YGFX_OK);
        CHECK(ygfx_buffer_update(&R->g, d5, 0, dots + 10, sizeof dots / 2) == YGFX_OK);
        memset(&dd, 0, sizeof dd);
        dd.buf = d10; dd.first = 5; dd.count = 5; dd.dot_size = 6; dd.edge = YGFX_EDGE_COSINE; dd.edge_width = 1; dd.color[2] = 1;
        r[0] = ygfx_dots(&dd);
        dd.buf = d5; dd.first = 0;
        r[1] = ygfx_dots(&dd);
        gl_frame(&r[0], 1);
        memcpy(a1, R->scene, sizeof a1);
        gl_frame(&r[1], 1);
        for (i = 0; i < W * H * 4; i++) S6.first_diff = maxd(S6.first_diff, fabs(a1[i] - R->scene[i]));
        for (i = 0; i < W * H; i++) S6.dots_px += R->scene[i * 4 + 2] > 0.5f;   /* they were drawn */
        /* refusals: a run past its buffer; a run or a rectangle on a DIST
         * atlas with no range; a reach past the padding. A whole-texture
         * DIST mask with no range stays v0.2's (drawn). */
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        gd.buf = b4; gd.first = 3; gd.count = 2; r[2] = ygfx_glyphs(&gd); rc[0] = ygfx_draw(&R->g, &r[2]);
        gd.atlas = nr; gd.first = 0; r[2] = ygfx_glyphs(&gd); rc[1] = ygfx_draw(&R->g, &r[2]);
        gd.atlas = wr; gd.edge = YGFX_EDGE_GAUSSIAN; gd.edge_width = 1.5f; r[2] = ygfx_glyphs(&gd); rc[2] = ygfx_draw(&R->g, &r[2]);
        gd.edge_width = 1.0f; r[2] = ygfx_glyphs(&gd); rc[3] = ygfx_draw(&R->g, &r[2]);   /* 5 px <= 6.5 */
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_MASK_TEX; sd.mask = nr; sd.w = 32; sd.h = 64; sd.color[0] = 1;
        r[2] = ygfx_shape(&sd);
        r[2].src[2] = 32; r[2].src[3] = 64;
        rc[4] = ygfx_draw(&R->g, &r[2]);
        sd.w = 64; r[2] = ygfx_shape(&sd); rc[5] = ygfx_draw(&R->g, &r[2]);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        S6.refused_n += 6;
        S6.refused_ok += (rc[0] == YGFX_ERR_ARG) + (rc[1] == YGFX_ERR_RANGE) + (rc[2] == YGFX_ERR_RANGE) +
                         (rc[3] == YGFX_OK) + (rc[4] == YGFX_ERR_RANGE) + (rc[5] == YGFX_OK);
        ygfx_texture_free(&R->g, nr);
        ygfx_texture_free(&R->g, wr);
        ygfx_buffer_free(&R->g, b4); ygfx_buffer_free(&R->g, b2);
        ygfx_buffer_free(&R->g, d10); ygfx_buffer_free(&R->g, d5);
    }
    {   /* a stimulus that can cover no pixel is refused with a message, never
         * drawn as nothing; empty fields, runs and element arrays are legal;
         * zero defaults draw what the manual says (a polygon's box from its
         * vertices, a dot field with no aperture) */
        static const float tri[6] = { -20, 15, 20, 15, 0, -18 };
        static float dxy[10] = { -30, 0, -10, 5, 10, -5, 30, 0, 0, 20 };
        ygfx_grating_desc gr;
        ygfx_noise_desc nd;
        ygfx_user_desc ud;
        ygfx_shape_desc sd;
        ygfx_dots_desc dd;
        ygfx_gabor_desc gd;
        ygfx_image_desc idd2;
        ygfx_instances_desc ins;
        ygfx_stim z[14];
        ygfx_buf db;
        int rc[14], want[14], q, nz = 0;
        ygfx_pipeline_desc pd;
        ygfx_pipe up;
        memset(&pd, 0, sizeof pd);
        pd.body = "float ysp_main(vec2 p) { return 0.5; }\n";
        up = ygfx_pipeline(&R->g, &pd);
        db = ygfx_buffer(&R->g, sizeof dxy);
        CHECK(ygfx_buffer_update(&R->g, db, 0, dxy, sizeof dxy) == YGFX_OK);
        memset(&gr, 0, sizeof gr); gr.sf = 0.1f;                         z[nz] = ygfx_grating(&gr); want[nz++] = YGFX_ERR_ARG;
        memset(&nd, 0, sizeof nd);                                        z[nz] = ygfx_noise(&nd); want[nz++] = YGFX_ERR_ARG;
        memset(&ud, 0, sizeof ud); ud.pipe = up;                          z[nz] = ygfx_user(&ud); want[nz++] = YGFX_ERR_ARG;
        memset(&td, 0, sizeof td); td.w = 4; td.h = 4; td.format = YGFX_RGBA8; t = ygfx_texture(&R->g, &td);
        memset(&idd2, 0, sizeof idd2); idd2.tex = t;                      z[nz] = ygfx_image(NULL, &idd2); want[nz++] = YGFX_ERR_ARG;   /* no g: no size */
        memset(&sd, 0, sizeof sd); sd.shape = YGFX_CIRCLE;              z[nz] = ygfx_shape(&sd); want[nz++] = YGFX_ERR_ARG;
        memset(&sd, 0, sizeof sd); sd.shape = YGFX_LINE; sd.w = 20;     z[nz] = ygfx_shape(&sd); want[nz++] = YGFX_ERR_ARG;
        memset(&sd, 0, sizeof sd); sd.shape = YGFX_ANNULUS; sd.w = 20; sd.shape_p[0] = 10; z[nz] = ygfx_shape(&sd); want[nz++] = YGFX_ERR_ARG;
        memset(&gd, 0, sizeof gd); gd.sf = 0.1f;                          z[nz] = ygfx_gabor(&gd); want[nz++] = YGFX_ERR_ARG;
        memset(&dd, 0, sizeof dd); dd.buf = db; dd.count = 5;             z[nz] = ygfx_dots(&dd); want[nz++] = YGFX_ERR_ARG;   /* dot_size 0 */
        dd.dot_size = 4; dd.aperture = YGFX_CIRCLE;                     z[nz] = ygfx_dots(&dd); want[nz++] = YGFX_ERR_ARG;   /* a CIRCLE of no w */
        dd.count = 0;                                                     z[nz] = ygfx_dots(&dd); want[nz++] = YGFX_OK;        /* empty: legal */
        memset(&ins, 0, sizeof ins); ins.n = 0;
        memset(&sd, 0, sizeof sd); sd.shape = YGFX_CIRCLE; sd.w = 10; sd.color[2] = 1;   /* blue: must not show */
        z[nz] = ygfx_shape(&sd);
        z[nz] = ygfx_instances(&R->g, &z[nz], &ins); want[nz++] = YGFX_OK;   /* refused at make: draws nothing */
        memset(&sd, 0, sizeof sd); sd.shape = YGFX_POLYGON; sd.shape_p[0] = 3; sd.vertices = tri; sd.color[0] = 1;
        sd.x = -80;                                                       z[nz] = ygfx_shape(&sd); want[nz++] = YGFX_OK;   /* box from its vertices */
        memset(&dd, 0, sizeof dd); dd.buf = db; dd.count = 5; dd.dot_size = 5; dd.color[1] = 1; dd.x = 80;
        z[nz] = ygfx_dots(&dd); want[nz++] = YGFX_OK;                 /* no aperture by the zero default */
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        for (q = 0; q < nz; q++) {
            rc[q] = ygfx_draw(&R->g, &z[q]);
            S6.zero_n++;
            S6.zero_ok += rc[q] == want[q] && (want[q] == YGFX_OK || strstr(ygfx_error(&R->g), "ysp_gfx: ") != NULL);
            if (rc[q] != want[q]) fprintf(stderr, "  zero-area case %d: %d, want %d (%s)\n", q, rc[q], want[q], ygfx_error(&R->g));
        }
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(ygfx_read_scene(&R->g, 0, 0, W, H, R->scene) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        for (q = 0; q < W * H; q++) { S6.zero_poly_px += R->scene[q * 4] > 0.5f; S6.zero_dots_px += R->scene[q * 4 + 1] > 0.5f; }
        S6.zero_ok -= R->scene[((H / 2) * W + W / 2) * 4 + 2] != 0.0f;   /* the refused array's template is not drawn */
        ygfx_buffer_free(&R->g, db);
        ygfx_pipeline_free(&R->g, up);
        ygfx_texture_free(&R->g, t);
    }
    {   /* an aperture's shape_p from the desc, against the field set by hand */
        ygfx_grating_desc gr;
        ygfx_noise_desc nd;
        ygfx_stim a[2], b[2];
        memset(&gr, 0, sizeof gr);
        gr.w = 80; gr.sf = 1 / 10.0f; gr.contrast = 0.3f; gr.aperture = YGFX_ANNULUS; gr.x = -60;
        a[0] = ygfx_grating(&gr);
        a[0].shape_p[0] = 15;
        gr.shape_p[0] = 15;
        b[0] = ygfx_grating(&gr);
        memset(&nd, 0, sizeof nd);
        nd.w = 70; nd.h = 50; nd.contrast = 0.3f; nd.aperture = YGFX_RECT; nd.x = 60; nd.check = 3;
        a[1] = ygfx_noise(&nd);
        a[1].shape_p[1] = 12;
        nd.shape_p[1] = 12;
        b[1] = ygfx_noise(&nd);
        gl_frame(a, 2);
        memcpy(a1, R->scene, sizeof a1);
        gl_frame(b, 2);
        for (i = 0; i < W * H * 4; i++) S6.shape_p_diff = maxd(S6.shape_p_diff, fabs(a1[i] - R->scene[i]));
    }
    ygfx_close(&R->g);
}

/* v0.5: ysp/color.h's context in open(); OKLAB paint on absolute XYZ, the
 * display's black included, against a reference written apart from the
 * header; desc.cones and desc.lum refusals. */
typedef struct stats7 {
    double oklab_black, black_shift;
    long   ctx_bad, refused_ok, refused_n, ran;
} stats7;
static stats7 S7;

/* Oklab of linear device RGB, Ottosson's published matrices. old = 0: the
 * calibration's absolute XYZ (black included) against its white (black
 * included); old = 1: v0.4's form, black removed. */
static void okb_from_rgb(const ycol_cal* c, int old, const double rgb[3], double lab[3]) {
    static const double m1[9] = { 0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715, 0.0361456387,
                                  0.0482003018, 0.2643662691, 0.6338517070 };
    static const double m2[9] = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    double yw = c->rgb_to_xyz[3] + c->rgb_to_xyz[4] + c->rgb_to_xyz[5] + (old ? 0.0 : c->black_xyz[1]), xyz[3], lms[3];
    int i, k;
    for (i = 0; i < 3; i++) {
        xyz[i] = old ? 0.0 : c->black_xyz[i];
        for (k = 0; k < 3; k++) xyz[i] += c->rgb_to_xyz[3 * i + k] * rgb[k];
        xyz[i] /= yw;
    }
    for (i = 0; i < 3; i++) { lms[i] = 0; for (k = 0; k < 3; k++) lms[i] += m1[3 * i + k] * xyz[k]; lms[i] = cbrt(lms[i]); }
    for (i = 0; i < 3; i++) { lab[i] = 0; for (k = 0; k < 3; k++) lab[i] += m2[3 * i + k] * lms[k]; }
}

/* A x = b for a 3 x 3, by Gaussian elimination with partial pivoting:
 * the test's own solve, nothing shared with either header. */
static void solve3(const double* A, const double* b, double* x) {
    double M[3][4];
    int i, j, k;
    for (i = 0; i < 3; i++) { for (j = 0; j < 3; j++) M[i][j] = A[3 * i + j]; M[i][3] = b[i]; }
    for (k = 0; k < 3; k++) {
        int piv = k;
        for (i = k + 1; i < 3; i++) if (fabs(M[i][k]) > fabs(M[piv][k])) piv = i;
        for (j = 0; j < 4; j++) { double t = M[k][j]; M[k][j] = M[piv][j]; M[piv][j] = t; }
        for (i = k + 1; i < 3; i++) {
            double f = M[i][k] / M[k][k];
            for (j = k; j < 4; j++) M[i][j] -= f * M[k][j];
        }
    }
    for (i = 2; i >= 0; i--) {
        double t = M[i][3];
        for (j = i + 1; j < 3; j++) t -= M[i][j] * x[j];
        x[i] = t / M[i][i];
    }
}

/* ... and back: Lab to the cube roots (M2 solved), cubed, to XYZ (M1
 * solved), to device RGB (the calibration's matrix solved). */
static void okb_to_rgb(const ycol_cal* c, int old, const double lab[3], double rgb[3]) {
    static const double m1[9] = { 0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715, 0.0361456387,
                                  0.0482003018, 0.2643662691, 0.6338517070 };
    static const double m2[9] = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    double yw = c->rgb_to_xyz[3] + c->rgb_to_xyz[4] + c->rgb_to_xyz[5] + (old ? 0.0 : c->black_xyz[1]), l[3], xyz[3];
    int k;
    solve3(m2, lab, l);
    for (k = 0; k < 3; k++) l[k] = l[k] * l[k] * l[k];
    solve3(m1, l, xyz);
    for (k = 0; k < 3; k++) xyz[k] = xyz[k] * yw - (old ? 0.0 : c->black_xyz[k]);
    solve3(c->rgb_to_xyz, xyz, rgb);
}

static void gl_v05_color(stats* st) {
    static ycol_cal cal3;
    static ycol_lum lum10;
    static const float xy[3][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f } };
    static const float peak[3] = { 17.0f, 57.0f, 5.8f };
    ygfx_desc gd;
    ygfx_shape_desc d;
    ygfx_paint pt;
    ygfx_stim s;
    char err[200];
    int i, j, k, q;
    (void)st;
    S7.ran = 1;
    /* a display whose black has light: 0.5 cd/m2 under a white of about 80 */
    ycol_cal_init(&cal3);
    ycol_cal_add(&cal3, YCOL_GUN_BLACK, 0, 0.5f, 0.31f, 0.33f);
    for (k = 0; k < 3; k++)
        for (i = 1; i <= 8; i++)
            ycol_cal_add(&cal3, k, i / 8.0f, (float)(0.5 + peak[k] * pow(i / 8.0, 2.2)), xy[k][0], xy[k][1]);
    CHECK(ycol_cal_derive(&cal3, err, sizeof err) == YCOL_OK);
    memset(&gd, 0, sizeof gd);
    gd.screen = &R->scr;
    gd.background[0] = 0.4f; gd.background[1] = 0.5f; gd.background[2] = 0.3f;
    gd.cal = &cal3;
    gd.cache = test_cache();
    ygfx__test_scene32 = 1;
    if (!ygfx_open(&R->g, &gd)) { fprintf(stderr, "v0.5 open: %s\n", ygfx_error(&R->g)); g_failures++; return; }
    {   /* ygfx_color() is the context the same desc gives */
        const ycol_ctx* cx = ygfx_color(&R->g);
        ycol_ctx mine;
        ycol_ctx_desc cd;
        memset(&cd, 0, sizeof cd);
        cd.cal = &cal3;
        for (k = 0; k < 3; k++) cd.background[k] = gd.background[k];
        S7.ctx_bad += !cx || !ycol_ctx_open(&mine, &cd) || mine.id != cx->id ||
                      memcmp(mine.ok_rgb_to_lms, cx->ok_rgb_to_lms, sizeof mine.ok_rgb_to_lms) != 0;
    }
    memset(&pt, 0, sizeof pt);
    pt.kind = YGFX_PAINT_LINEAR; pt.space = YGFX_SPACE_OKLAB; pt.n = 3;
    pt.x0 = -60; pt.y0 = -20; pt.x1 = 60; pt.y1 = 20;
    pt.stops[0].t = 0; pt.stops[0].color[0] = 0.55f; pt.stops[0].color[1] = 0.45f; pt.stops[0].color[2] = 0.25f;
    pt.stops[1].t = 0.4f; pt.stops[1].color[0] = 0.30f; pt.stops[1].color[1] = 0.55f; pt.stops[1].color[2] = 0.35f;
    pt.stops[2].t = 1; pt.stops[2].color[0] = 0.02f; pt.stops[2].color[1] = 0.03f; pt.stops[2].color[2] = 0.05f;
    memset(&d, 0, sizeof d);
    d.shape = YGFX_RECT; d.w = 160; d.h = 120; d.paint = &pt; d.ori = 10;
    s = ygfx_shape(&d);
    gl_frame(&s, 1);
    for (j = 0; j < H; j += 3)
        for (i = 0; i < W; i += 3) {
            float lx, ly;
            double t, u, a[3], b[3], la[3], lb[3], m[3], want[3], was[3];
            int seg;
            ygfx_local(&R->g, &s, (float)i + 0.5f, (float)j + 0.5f, &lx, &ly);
            if (fabs(lx) > 79 || fabs(ly) > 59) continue;
            t = ((lx + 60) * 120 + (ly + 20) * 40) / (120.0 * 120 + 40 * 40);
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            seg = t < 0.4 ? 0 : 1;
            u = seg ? (t - 0.4) / 0.6 : t / 0.4;
            for (q = 0; q < 3; q++) { a[q] = pt.stops[seg].color[q]; b[q] = pt.stops[seg + 1].color[q]; }
            okb_from_rgb(&cal3, 0, a, la); okb_from_rgb(&cal3, 0, b, lb);
            for (q = 0; q < 3; q++) m[q] = la[q] + (lb[q] - la[q]) * u;
            okb_to_rgb(&cal3, 0, m, want);
            okb_from_rgb(&cal3, 1, a, la); okb_from_rgb(&cal3, 1, b, lb);
            for (q = 0; q < 3; q++) m[q] = la[q] + (lb[q] - la[q]) * u;
            okb_to_rgb(&cal3, 1, m, was);
            if (getenv("YGFX_TEST_VERBOSE") && (want[0] != want[0] || was[0] != was[0] || R->scene[(j * W + i) * 4] != R->scene[(j * W + i) * 4]))
                fprintf(stderr, "  v0.5 nan at %d %d: t %g scene %g want %g was %g\n", i, j, t, R->scene[(j * W + i) * 4], want[0], was[0]);
            for (q = 0; q < 3; q++) {
                S7.oklab_black = maxd(S7.oklab_black, fabs(R->scene[(j * W + i) * 4 + q] - want[q]));
                S7.black_shift = maxd(S7.black_shift, fabs(want[q] - was[q]));
            }
        }
    ygfx_close(&R->g);
    /* refusals: desc.cones without a calibration; a lum record in other cone units */
    memset(&gd, 0, sizeof gd);
    gd.screen = &R->scr;
    gd.cache = test_cache();
    gd.cones = YCOL_CONES_SS10;
    S7.refused_n++;
    if (!ygfx_open(&R->g, &gd)) S7.refused_ok += strstr(ygfx_error(&R->g), "desc.cal") != NULL;
    else ygfx_close(&R->g);
    {
        const double w[3] = { 0.7, 0.33, 0.0 };
        CHECK(ycol_lum_stated(&lum10, YCOL_CONES_SS10, w) == YCOL_OK);
        gd.cones = 0;
        gd.cal = &cal3;
        gd.lum = &lum10;
        S7.refused_n++;
        if (!ygfx_open(&R->g, &gd)) S7.refused_ok += strstr(ygfx_error(&R->g), "SS10") != NULL;
        else ygfx_close(&R->g);
    }
    /* no calibration: no context */
    if (gl_open(1, YGFX_RGBA16F, YGFX_DITHER_NONE)) {
        S7.ctx_bad += ygfx_color(&R->g) != NULL;
        ygfx_close(&R->g);
    }
}

static void gl_v06_text(stats* st);
typedef struct stats8 {
    double mean[2][4];         /* rays, exact + rays: the mean edge error per size, worst rotation */
    double max_edge, off, exact, alone, buffer, target, added, jump, jump_ray, jump_corner, pal_frac, cpu;
    double rays_exact, rays_same;   /* .rays against the exact area; against the program without it */
    double ol_exact, ol_same;       /* ysp/outline.h's set: against the exact coverage; against the test builder's */
    int    ol_ran;
    double cached[4];               /* a run cached in a target against drawn: RGBA16F over black, gray;
                                     * RGBA32F; an RGBA32F target in an RGBA16F scene over gray */
    long   hit_bad, hit_n, pal_bad, top_bad, refused_ok, refused_n;
    int    ran;
} stats8;
static stats8 S8;
#define CS_NCOND 64
static float* cs_xr[8][CS_NCOND];
static int    cs_xr_dev = -1;
typedef struct cs_xd { double max; long n_1e3; int i, j, cond; } cs_xd;
static cs_xd cs_xdiff[8];
static void gl_v06_blur(stats* st);
static void gl_v08(stats* st);
static void s10_report(void);
static void gl_v10_simplex(stats* st);
static void gl_v10_user_tex(stats* st);
static void gl_v11_prime(stats* st);
static void gl_v11_start(stats* st);
static void gl_v12_capture(stats* st);
static void gl_v15_first(stats* st);
static void s11_report(void);
static void test_v10_simplex_cpu(void);
typedef struct stats9 {
    double rect[3][2][6];      /* format x supersample x sigma: max error against the erf product */
    double cover;              /* an R16F result drawn as tinted coverage against its values */
    double edge;               /* a rect at the layer's corner: outside is empty */
    long   refused_ok, refused_n;
    int    ran;
} stats9;
static stats9 S9;
static int cs_dev;

/* --------------------------------------------------------------- kinds */

/* The specialized vector programs: each stimulus drawn alone by the generic
 * program (area threshold out of reach) and as the pick routes it
 * (threshold 0), the RGBA32F scenes compared bit for bit; the program each
 * draw took against the one the case expects. */
typedef struct statsk {
    long   ran, n, differ, wrong_pick, used[YGFX__N_VSPEC], threshold_bad, inst_bad;
    double maxd;
} statsk;
static statsk SK;

/* -1: the generic program; else the index in ygfx__vspecs. */
static int kinds_took(void) {
    int c, i, took = -2;
    for (c = 0; c < (int)R->g.n_cmds; c++) {
        int t = -1;
        for (i = 0; i < YGFX__N_VSPEC; i++) if (R->g.cmds[c].pipe == R->g.vspec[i]) t = i;
        if (R->g.cmds[c].pipe == R->g.builtin[YGFX__B_VECTOR] || t >= 0) took = t;
    }
    return took;
}

static void kinds_one(const ygfx_stim* s, int want) {
    static float ref[W * H * 4];
    int i, took;
    ygfx__vspec_area = 1e300;
    gl_frame(s, 1);
    memcpy(ref, R->scene, sizeof ref);
    SK.wrong_pick += kinds_took() != -1;
    ygfx__vspec_area = 0.0;
    gl_frame(s, 1);
    took = kinds_took();
    if (took >= 0) SK.used[took]++;
    SK.wrong_pick += took != want;
    for (i = 0; i < W * H * 4; i++)
        if (memcmp(&ref[i], &R->scene[i], sizeof ref[i]) != 0) { SK.differ++; SK.maxd = maxd(SK.maxd, fabs(ref[i] - R->scene[i])); }
    if (getenv("YGFX_TEST_VERBOSE") && took != want) fprintf(stderr, "  kinds: stimulus %ld took %d, wanted %d\n", SK.n, took, want);
    SK.n++;
}

static void gl_v05_kinds(stats* st) {
    static ygfx_prim pr[6];
    static float path[8] = { -60, -20, -10, 30, 40, -25, 70, 20 }, tri[6] = { -60, -40, 60, -30, 0, 50 };
    static ygfx_fx fx;
    static ygfx_paint pt;
    static ygfx_group grp;
    ygfx_shape_desc d;
    ygfx_compound_desc cd;
    ygfx_group_desc gd;
    ygfx_stim s;
    int i, e;
    static const ygfx_edge edges[3] = { YGFX_EDGE_COSINE, YGFX_EDGE_GAUSSIAN, YGFX_EDGE_HARD };
    (void)st;
    SK.ran = 1;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&fx, 0, sizeof fx);
    fx.dx = 4; fx.dy = 3; fx.drop.sigma = 3; fx.drop.opacity = 0.5f; fx.glow.sigma = 2; fx.glow.spread = 1; fx.glow.opacity = 0.4f;
    fx.glow.color[1] = 0.9f; fx.inner.sigma = 2; fx.inner.opacity = 0.5f; fx.band[0].a1 = -2; fx.band[0].opacity = 1;
    fx.band[1].a1 = 1; fx.band[1].a2 = 3; fx.band[1].opacity = 0.7f; fx.band[1].color[2] = 1;
    memset(&pt, 0, sizeof pt);
    pt.kind = YGFX_PAINT_LINEAR; pt.n = 2; pt.x0 = -50; pt.x1 = 50; pt.stops[0].color[0] = 0.9f; pt.stops[1].t = 1; pt.stops[1].color[2] = 0.9f;
    memset(&gd, 0, sizeof gd);
    gd.x = 5; gd.y = -3; gd.ori = 30; gd.scale = 1.3f; gd.opacity = 0.8f;
    grp = ygfx_group_make(&gd);
    for (e = 0; e < 3; e++) {
        /* RRECT, no dash, trim, paint or fx: specialized program 0 */
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RRECT; d.w = 140; d.h = 90; d.ori = 17; d.edge = edges[e]; d.edge_width = e == 1 ? 1.5f : 2.0f;
        d.shape_p[0] = 10; d.shape_p[1] = 20; d.shape_p[2] = 5; d.shape_p[3] = 30;
        d.color[0] = 0.7f; d.color[1] = 0.4f; d.color[2] = 0.2f;
        s = ygfx_shape(&d); kinds_one(&s, 0);
        d.stroke = 6; d.stroke_align = (ygfx_stroke_align)e; s = ygfx_shape(&d); kinds_one(&s, 0);
        d.stroke = 0; d.offset = 3; d.opacity = 0.7f; s = ygfx_shape(&d); s.gate = 0.8f; kinds_one(&s, 0);
        d.offset = 0; d.opacity = 0; d.group = &grp; s = ygfx_shape(&d); kinds_one(&s, 0);
        d.group = NULL; d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 0; s = ygfx_shape(&d); kinds_one(&s, 0);
        /* RRECT with paint, fx or dashes: the generic program */
        d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 12;
        d.paint = &pt; s = ygfx_shape(&d); kinds_one(&s, -1);
        d.paint = NULL; d.fx = &fx; s = ygfx_shape(&d); kinds_one(&s, -1);
        d.fx = NULL; d.stroke = 4; d.dash[0] = 12; d.dash[1] = 6; s = ygfx_shape(&d); kinds_one(&s, -1);
        /* a dashed or trimmed CIRCLE: specialized program 1 */
        memset(&d, 0, sizeof d);
        d.shape = YGFX_CIRCLE; d.w = 150; d.stroke = 6; d.stroke_align = (ygfx_stroke_align)e; d.edge = edges[e]; d.edge_width = 1.5f;
        d.color[0] = 0.2f; d.color[1] = 0.8f; d.color[2] = 0.5f; d.dash[0] = 20; d.dash[1] = 10;
        s = ygfx_shape(&d); kinds_one(&s, 1);
        d.dash_snap = true; d.dash_offset = 7; d.cap = YGFX_CAP_ROUND; s = ygfx_shape(&d); kinds_one(&s, 1);
        d.dash[0] = d.dash[1] = 0; d.dash_snap = false; d.trim[0] = 0.1f; d.trim[1] = 0.7f; d.stroke = 8; s = ygfx_shape(&d); kinds_one(&s, 1);
        d.dash[0] = 9; d.dash[1] = 5; d.cap = YGFX_CAP_SQUARE; d.ori = 40; s = ygfx_shape(&d); kinds_one(&s, 1);
        d.group = &grp; s = ygfx_shape(&d); kinds_one(&s, 1);
        d.group = NULL; d.paint = &pt; s = ygfx_shape(&d); kinds_one(&s, -1);
        d.paint = NULL; d.fx = &fx; s = ygfx_shape(&d); kinds_one(&s, -1);
        /* a compound of CIRCLEs, every op: specialized program 2, fx too */
        for (i = 0; i < 6; i++) {
            memset(&pr[i], 0, sizeof pr[i]);
            pr[i].shape = YGFX_CIRCLE; pr[i].w = 50.0f + 7.0f * (float)i; pr[i].x = (float)(i * 23 - 60); pr[i].y = (float)((i % 3) * 17 - 15);
            pr[i].op = (uint8_t)i; pr[i].k = 9;
        }
        pr[2].onion = 3;
        memset(&cd, 0, sizeof cd);
        cd.prims = pr; cd.n = 6; cd.w = 200; cd.h = 120; cd.edge = edges[e]; cd.edge_width = 2; cd.ori = 12;
        cd.color[0] = 0.5f; cd.color[1] = 0.5f; cd.color[2] = 0.9f;
        s = ygfx_compound(&cd); kinds_one(&s, 2);
        cd.fx = &fx; s = ygfx_compound(&cd); kinds_one(&s, 2);
        cd.onion = 2; cd.offset = 1.5f; cd.stroke = 3; s = ygfx_compound(&cd); kinds_one(&s, 2);
        cd.group = &grp; s = ygfx_compound(&cd); kinds_one(&s, 2);
        cd.group = NULL; cd.paint = &pt; s = ygfx_compound(&cd); kinds_one(&s, -1);
        cd.paint = NULL; pr[4].shape = YGFX_RRECT; pr[4].h = 40; s = ygfx_compound(&cd); kinds_one(&s, -1);
        /* every other vector kind keeps the generic program */
        {
            static const ygfx_shape_kind kinds[] = { YGFX_ARC, YGFX_PIE, YGFX_CAPSULE, YGFX_NGON, YGFX_STAR,
                                                       YGFX_ELLIPSE, YGFX_POLYLINE, YGFX_QBEZIER, YGFX_POLYGON, YGFX_ANNULUS };
            int q;
            for (q = 0; q < (int)(sizeof kinds / sizeof kinds[0]); q++) {
                memset(&d, 0, sizeof d);
                d.shape = kinds[q]; d.w = 130; d.h = 80; d.edge = edges[e]; d.edge_width = 2; d.ori = 9;
                d.color[0] = 0.3f; d.color[1] = 0.6f; d.color[2] = 0.6f;
                d.shape_p[0] = 200; d.shape_p[1] = 14; d.shape_p[2] = 30;
                if (kinds[q] == YGFX_CAPSULE) { d.shape_p[0] = 20; d.shape_p[1] = 12; }
                if (kinds[q] == YGFX_NGON || kinds[q] == YGFX_STAR) { d.shape_p[0] = 6; d.shape_p[1] = kinds[q] == YGFX_STAR ? 0.5f : 4; d.shape_p[2] = 3; }
                if (kinds[q] == YGFX_POLYLINE) { d.path = path; d.n_path = 4; d.shape_p[0] = 10; }
                if (kinds[q] == YGFX_QBEZIER) { d.path = path; d.n_path = 3; d.shape_p[0] = 10; }
                if (kinds[q] == YGFX_POLYGON) { d.vertices = tri; d.shape_p[0] = 3; d.shape_p[1] = 8; }
                if (kinds[q] == YGFX_ANNULUS) { d.shape_p[0] = 40; d.stroke = 4; d.dash[0] = 10; d.dash[1] = 5; }
                s = ygfx_shape(&d); kinds_one(&s, -1);
                if (kinds[q] != YGFX_POLYGON && kinds[q] != YGFX_ANNULUS) {   /* and with dashes */
                    d.stroke = 4; d.dash[0] = 10; d.dash[1] = 5;
                    s = ygfx_shape(&d); kinds_one(&s, -1);
                }
            }
        }
    }
    {   /* the area threshold: a small RRECT stays generic, a large one does not */
        int took_small, took_large;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RRECT; d.w = 40; d.h = 30; d.edge = YGFX_EDGE_COSINE; d.edge_width = 2; d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 6;
        d.color[0] = 0.5f;
        ygfx__vspec_area = 10000.0;
        s = ygfx_shape(&d); gl_frame(&s, 1); took_small = kinds_took();
        d.w = 140; d.h = 90; s = ygfx_shape(&d); gl_frame(&s, 1); took_large = kinds_took();
        SK.threshold_bad = (took_small != -1) + (took_large != 0);
        /* an element array keeps the generic program at any size */
        {
            static ygfx_inst el[2];
            ygfx_instances_desc id;
            ygfx_stim t;
            memset(el, 0, sizeof el);
            el[0].x = -60; el[1].x = 60;
            memset(&id, 0, sizeof id);
            id.inst = el; id.n = 2; id.fields = YGFX_I_XY;
            ygfx__vspec_area = 0.0;
            t = ygfx_instances(&R->g, &s, &id);
            SK.inst_bad += t.n_inst != 2;
            if (t.n_inst == 2) {
                gl_frame(&t, 1);
                for (i = 0; i < (int)R->g.n_cmds; i++) SK.inst_bad += R->g.cmds[i].pipe == R->g.vspec[0];
            }
        }
    }
    ygfx__vspec_area = 65536.0;
    ygfx_close(&R->g);
}

static int gl_suite(const char* name, ygfx_hl_device dev) {
    yscr_desc d;
    stats st;
    char line[400];
    memset(&st, 0, sizeof st);
    memset(R, 0, sizeof *R);
    R->hl.w = W; R->hl.h = H; R->hl.device = dev;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_CUSTOM;
    d.presenter = &ygfx_headless_presenter;
    d.presenter_ctx = &R->hl;
    d.sim_period_ns = 1000000;
    if (!yscr_open(&R->scr, &d)) {
        printf("GL %s: not available: %s\n", name, yscr_error(&R->scr));
        return 0;
    }
    g_where = name;
    printf("GL %s: %s\n", name, R->hl.renderer);
    cs_dev = (int)dev;
    {
        static void (*const parts[])(stats*) = { gl_shapes, gl_gratings, gl_gabors, gl_dots, gl_images,
                                                 gl_noise, gl_output, gl_user_batch_rows,
                                                 gl_strokes, gl_masks, gl_sprites, gl_tint, gl_groups, gl_targets,
                                                 gl_alpha_passes, gl_edge_truth, gl_v03_kinds, gl_v03_truth, gl_v03_fx, gl_v03_paint, gl_v03_msdf,
                                                 gl_v04_cache, gl_v04_fixes, gl_v04_video, gl_v04_inst, gl_v04_order, gl_v05_color,
                                                 gl_v06_text, gl_v06_blur, gl_v05_kinds, gl_v08, gl_v10_simplex, gl_v10_user_tex,
                                                 gl_v11_prime, gl_v11_start, gl_v12_capture, gl_v15_first };
        static const char* const names[] = { "shapes", "gratings", "gabors", "dots", "images", "noise",
                                             "output", "user+batch+rows", "strokes", "masks", "sprites", "tint",
                                             "groups", "targets", "alpha+passes", "edge truth", "v0.3 kinds", "v0.3 truth", "v0.3 fx", "v0.3 paint", "v0.3 msdf",
                                             "v0.4 cache", "v0.4 fixes", "v0.4 video", "v0.4 inst", "v0.4 order", "v0.5 color",
                                             "v0.6 text", "v0.6 blur", "vspec", "v0.8", "v0.10 simplex", "v0.10 user tex",
                                             "v0.11 prime", "v0.11.1 start", "v0.12 capture", "v0.15 first" };
        int k;
        memset(&S2, 0, sizeof S2);
        memset(&S3, 0, sizeof S3);
        memset(&S4, 0, sizeof S4);
        memset(&S5, 0, sizeof S5);
        memset(&S6, 0, sizeof S6);
        memset(&S7, 0, sizeof S7);
        memset(&S8, 0, sizeof S8);
        memset(&SK, 0, sizeof SK);
        mc_clear(&g_mc);
        printf("  seconds:");
        for (k = 0; k < (int)(sizeof parts / sizeof parts[0]); k++) {
            uint64_t t0 = yrt_now_ns();
            const char* only = getenv("YGFX_TEST_PARTS");   /* a development seam: run parts by name */
            if (only && only[0] && !strstr(only, names[k])) continue;
            parts[k](&st);
            printf(" %s %.1f", names[k], (double)(yrt_now_ns() - t0) * 1e-9);
            fflush(stdout);
        }
        printf("\n");
    }
    if (gl_open(0, YGFX_RGBA16F, YGFX_DITHER_NONE)) {
        ygfx_describe(&R->g, line, sizeof line);
        printf("  %s\n", line);
        ygfx_close(&R->g);
    }
    yscr_close(&R->scr);
    printf("  measured: shapes soft-edge max %.2e, hard-edge wrong pixels %ld, hit vs GPU wrong %ld (ties skipped %ld)\n",
           st.shape_soft, st.hard_bad, st.hit_bad, st.ties);
    printf("  measured: grating max %.2e, square wave max %.2e, gabor max %.2e, user = grating %.2e\n",
           st.grating, st.square_bad, st.gabor, st.user);
    printf("  measured: dots max %.2e, image max %.2e, noise wrong %ld, identity codes wrong %ld, CLUT codes wrong %ld\n",
           st.dots, st.image, st.noise_bad, st.out_bad, st.lut_bad);
    printf("  measured: ordered dither wrong %ld, noise dither mean - target %+.4f, anchor change %.2e, "
           "overlap - sum %.2e, batched - single %.2e\n",
           st.dither_bad, st.dither_mean, st.anchor_diff, st.overlap, st.batch_diff);
    /* Tolerances: about 3x the largest error measured on a renderer of the
     * same kind (docs/gfx.md has the table per renderer); SwiftShader's
     * transcendentals are about 10x coarser and get their own; exact where
     * the arithmetic is exact. */
    {
        const int ss = dev == YGFX_HL_SWIFTSHADER;
        CHECK_LE(st.shape_soft, ss ? 3e-4 : 5e-5);
        CHECK(st.hard_bad == 0);
        CHECK(st.hit_bad == 0);
        CHECK_LE(st.grating, ss ? 4e-4 : 5e-5);
        CHECK_LE(st.square_bad, ss ? 1.5e-4 : 2e-5);
        CHECK_LE(st.gabor, ss ? 3e-4 : 4e-6);
        CHECK_LE(st.user, 1e-6);
        CHECK_LE(st.dots, ss ? 3e-4 : 5e-5);
        CHECK_LE(st.image, 2.2e-7);
    }
    CHECK(st.noise_bad == 0);
    CHECK(st.out_bad == 0);
    CHECK(st.lut_bad == 0);
    CHECK(st.dither_bad == 0);
    /* the same on every renderer (the hash is exact); 32768 draws of a
     * Bernoulli(0.3) code step: SD 0.0025, so 4 SD */
    CHECK_LE(fabs(st.dither_mean), 0.01);
    CHECK_LE(st.anchor_diff, 1e-6);
    CHECK_LE(st.overlap, 4e-7);
    CHECK(st.batch_diff == 0.0);
    printf("  v0.2: stroke soft max %.2e, hard wrong %ld, hit wrong %ld, miter wrong %ld; mask GPU max %.2e (distance "
           "texture vs the disc %.3f texel, vs brute force wrong %ld); sprites wrong %ld, bleed %ld, linear max %.2e; tint max %.2e\n",
           S2.stroke_soft, S2.stroke_hard_bad, S2.stroke_hit_bad, S2.miter_bad, S2.mask_gpu, S2.mask_sdf_cpu, S2.edt_bad,
           S2.sprite_bad, S2.bleed, S2.sprite_lin, S2.tint);
    printf("  v0.2: group vs alone max %.2e, group hit wrong %ld; target vs direct max %.2e, target wrong %ld, "
           "persist wrong %ld, ADD vs direct max %.2e, CLUT codes wrong %ld; refusals %ld of %ld; epoch wrong %ld "
           "(control without the epoch: %ld wrong); new context %ld of 4, then wrong %ld\n"
           "  v0.2: OVER with alpha against v0.1's, scene rgb and codes wrong %ld (control: target alpha differs at %ld "
           "texels); pass limit wrong %ld\n",
           S2.group_diff, S2.group_hit_bad, S2.target_diff, S2.target_bad, S2.persist_bad, S2.add_diff, S2.clut_bad,
           S2.refused_ok, S2.refused_n, S2.epoch_bad, S2.epoch_control_bad, S2.lost_ok, S2.lost_bad,
           S2.alpha_scene_bad, S2.alpha_control, S2.passes_bad);
    {
        const int ss = dev == YGFX_HL_SWIFTSHADER;
        CHECK_LE(S2.stroke_soft, ss ? 3e-4 : 5e-5);
        CHECK(S2.stroke_hard_bad == 0);
        CHECK(S2.stroke_hit_bad == 0);
        CHECK(S2.miter_bad == 0);
        CHECK_LE(S2.mask_gpu, ss ? 3e-4 : 5e-5);
        /* a binary mask places its boundary only to within half a texel's
         * diagonal (0.71), whatever the transform */
        CHECK_LE(S2.mask_sdf_cpu, 0.75);
        CHECK(S2.edt_bad == 0);
        CHECK(S2.sprite_bad == 0);
        CHECK(S2.bleed == 0);
        CHECK_LE(S2.sprite_lin, 2.5e-6);
        CHECK_LE(S2.tint, 2e-7);
        CHECK_LE(S2.group_diff, 2e-7);
        CHECK(S2.group_hit_bad == 0);
        CHECK(S2.target_diff == 0.0);
        CHECK(S2.target_bad == 0);
        CHECK(S2.persist_bad == 0);
        CHECK_LE(S2.add_diff, 8e-4);   /* the target holds RGBA16F */
        CHECK(S2.clut_bad == 0);
        CHECK(S2.refused_ok == S2.refused_n);
        CHECK(S2.epoch_bad == 0);
        CHECK(S2.alpha_scene_bad == 0);
        CHECK(S2.alpha_control > 0);
        CHECK(S2.passes_bad == 0);
#if YGFX__EPOCH
        CHECK(S2.epoch_control_bad > 0);
        CHECK(S2.lost_ok == 4);
        CHECK(S2.lost_bad == 0);
#endif
    }
    printf("  edge model against truth: 90 deg vertex, F(d) - Gaussian blur max %.4f (analytic 0.25); disc R/sigma 5 %.4f "
           "(analytic %.4f), 20 %.4f (%.4f); MITER field - F(Euclidean d) max %.4f (analytic %.4f)\n",
           S3.vtx_blur, S3.disc_blur[0], S3.disc_pred[0], S3.disc_blur[1], S3.disc_pred[1], S3.miter_bound, S3.miter_pred);
    {
        const double tol = dev == YGFX_HL_SWIFTSHADER ? 3e-4 : 5e-5;
        /* the pixel grid samples the vertex exactly; the disc and the
         * corner field only near their worst point */
        CHECK_LE(S3.vtx_blur, 0.25 + tol);
        CHECK(S3.vtx_blur >= 0.25 - tol);
        CHECK_LE(S3.disc_blur[0], S3.disc_pred[0] + tol);
        CHECK(S3.disc_blur[0] >= 0.9 * S3.disc_pred[0]);
        CHECK_LE(S3.disc_blur[1], S3.disc_pred[1] + tol);
        CHECK(S3.disc_blur[1] >= 0.9 * S3.disc_pred[1]);
        CHECK_LE(S3.miter_bound, S3.miter_pred + tol);
        CHECK(S3.miter_bound >= 0.5 * S3.miter_pred);
    }
    printf("  v0.3 kinds (%ld stimuli): soft max %.2e (%s), with dashes or trim %.2e, hard wrong %ld, hit wrong %ld (ties %ld)\n",
           S4.n_shapes, S4.soft, S4.worst, S4.dash_soft, S4.hard_bad, S4.hit_bad, S4.ties);
    {
        const int ss = dev == YGFX_HL_SWIFTSHADER;
        CHECK_LE(S4.soft, ss ? 3e-4 : 5e-5);
        CHECK_LE(S4.dash_soft, ss ? 1e-2 : 1.5e-3);
    }
    printf("  v0.3 truth: QBEZIER (chords) GPU - F(brute force) %.2e, distance error %.4f px\n", S5.t2_qb_cov, S5.t2_qb_dist);
    printf("  v0.3 truth: exact kinds, GPU - F(brute-force Euclidean d) max %.2e (%s), the CPU form's distance error %.2e px\n"
           "  v0.3 bounds: union crease %.4f (analytic %.4f), smooth union settled %.4f, connecting %.4f\n"
           "  v0.3 blur: EXACT_BLUR rect shadow - erf product %.2e; F(d) shadow - blur %.4f\n"
           "  v0.3 fx in one pass - CPU layers %.2e; blend %.2e; batched with extension blocks - single %.2e; "
           "dash offset by a period %.2e, dash count wrong %ld, trim wrong %ld; refusals %ld of %ld\n",
           S5.t2_cov, S5.t2_worst, S5.t2_dist, S5.t3_union, S5.t3_union_pred, S5.t3_smooth, S5.t3_connect,
           S5.t4_exact, S5.t4_fd, S5.fx, S5.blend, S5.batch, S5.dash_period, S5.dash_count_bad, S5.trim_bad,
           S5.refused_ok, S5.refused_n);
    {
        const int ss = dev == YGFX_HL_SWIFTSHADER;
        /* the exact kinds' brute force has its own sampling error: ellipses
         * and the Bezier as 8192 chords, 1e-5 px */
        CHECK_LE(S5.t2_cov, ss ? 3e-4 : 6e-5);
        CHECK_LE(S5.t2_dist, 1e-3);
        CHECK_LE(S5.t2_qb_dist, 0.0055);
        CHECK_LE(S5.t2_qb_cov, 2e-3);
        CHECK_LE(S5.t3_union, S5.t3_union_pred + (ss ? 3e-4 : 5e-5));
        CHECK(S5.t3_union >= 0.5 * S5.t3_union_pred);
        CHECK_LE(S5.t3_smooth, 0.03);
        CHECK_LE(S5.t3_connect, 0.2);
        CHECK(S5.t3_connect >= 0.03);
        CHECK_LE(S5.t4_exact, ss ? 3e-4 : 5e-5);
        CHECK(S5.t4_fd > 0.1);
        CHECK_LE(S5.fx, ss ? 3e-4 : 5e-5);
        CHECK_LE(S5.blend, 2e-7);
        CHECK(S5.batch == 0.0);
        CHECK_LE(S5.dash_period, ss ? 2e-2 : 2e-3);
        CHECK(S5.dash_count_bad == 0);
        CHECK(S5.trim_bad == 0);
        CHECK(S5.refused_ok == S5.refused_n);
    }
    printf("  v0.3 paint: RGB %.2e, OKLAB against Ottosson through the calibration %.2e, DKL_POLAR against "
           "ycol_cal_dir_dkl %.2e, VERTEX against barycentric %.2e\n"
           "  v0.3 MSDF against the CPU's bilinear median %.2e; glyph run against single draws %.2e\n",
           S5.paint_rgb, S5.paint_oklab, S5.paint_dkl, S5.paint_vertex, S5.msdf, S5.glyph);
    {
        const int ss = dev == YGFX_HL_SWIFTSHADER;
        CHECK_LE(S5.paint_rgb, 2e-6);
        CHECK_LE(S5.paint_oklab, ss ? 1e-4 : 2e-5);
        CHECK_LE(S5.paint_dkl, ss ? 1e-3 : 1e-4);
        CHECK_LE(S5.paint_vertex, 2e-5);
        CHECK_LE(S5.msdf, ss ? 3e-4 : 5e-5);
        CHECK_LE(S5.glyph, 1e-5);
        CHECK(S4.hard_bad == 0);
        CHECK(S4.hit_bad == 0);
    }
    if (S7.ran) {
        const int ss7 = dev == YGFX_HL_SWIFTSHADER;
        printf("  v0.5 color: OKLAB with the display's black against absolute XYZ %.2e (v0.4's black-free form would move this "
               "gradient by %.2e); context from open() against one built apart: wrong %ld; refusals %ld of %ld\n",
               S7.oklab_black, S7.black_shift, S7.ctx_bad, S7.refused_ok, S7.refused_n);
        CHECK_LE(S7.oklab_black, ss7 ? 1e-4 : 2e-5);
        CHECK(S7.black_shift > 1e-4);
        CHECK(S7.ctx_bad == 0);
        CHECK(S7.refused_ok == S7.refused_n);
    }
    if (SK.ran) {
        printf("  kinds: %ld stimuli drawn by the generic and the picked program: values differ %ld (max %.2e); "
               "wrong pick %ld; specialized draws %ld, %ld, %ld; threshold wrong %ld; element arrays wrong %ld\n",
               SK.n, SK.differ, SK.maxd, SK.wrong_pick, SK.used[0], SK.used[1], SK.used[2], SK.threshold_bad, SK.inst_bad);
        CHECK(SK.differ == 0);
        CHECK(SK.wrong_pick == 0);
        CHECK(SK.used[0] > 0 && SK.used[1] > 0 && SK.used[2] > 0);
        CHECK(SK.threshold_bad == 0);
        CHECK(SK.inst_bad == 0);
    }
    printf("  v0.4 cache: %d programs, open without a cache %.0f ms, warm %.0f ms; %ld cases, wrong %ld\n",
           S6.programs, S6.open_cold_ms, S6.open_warm_ms, S6.cache_cases, S6.cache_bad);
    CHECK(S6.cache_bad == 0);
    printf("  v0.4 fixes: straight alpha, linear, against the premultiplied bilinear %.2e (v0.3's way: %.2e); a run from its "
           "first record against its own buffer %.2e; shape_p by desc against by hand %.2e; refusals %ld of %ld\n",
           S6.alpha_lin, S6.alpha_old, S6.first_diff, S6.shape_p_diff, S6.refused_ok, S6.refused_n);
    CHECK_LE(S6.alpha_lin, 2.5e-6);
    CHECK(S6.first_diff == 0.0);
    if (S6.refused_n) CHECK(S6.dots_px > 50);   /* when the part ran */
    printf("  v0.4 no-pixel stimuli: %ld of %ld refused or drawn as the manual says; a polygon with no box %ld px, dots with no aperture %ld px\n",
           S6.zero_ok, S6.zero_n, S6.zero_poly_px, S6.zero_dots_px);
    CHECK(S6.zero_ok == S6.zero_n);
    if (S6.zero_n) CHECK(S6.zero_poly_px > 100 && S6.zero_dots_px > 20);
    CHECK(S6.shape_p_diff == 0.0);
    CHECK(S6.refused_ok == S6.refused_n);
    printf("  v0.4 video: %ld cases at 1:1 (NV12, I420 x matrix x range x siting x chroma filter) %.2e; transfers %.2e; "
           "linear at 2.5x %.2e; plane updates %.2e; primaries through the calibration %.2e; DEVICE codes wrong %ld of 256; "
           "rebind %.2e; GL import %.2e; D3D11 NV12 slice import %s %.2e; refusals %ld of %ld\n",
           S6.vid_cases, S6.vid_1x, S6.vid_trc, S6.vid_lin, S6.vid_part, S6.vid_prim, S6.codes_bad, S6.rebind_diff,
           S6.import_gl_diff, S6.d3d_ran ? "ran" : "not run", S6.import_d3d_diff, S6.vid_refused_ok, S6.vid_refused_n);
    {
        CHECK_LE(S6.vid_1x, 1e-6);
        CHECK_LE(S6.vid_trc, 2e-6);
        CHECK_LE(S6.vid_lin, 2e-5);
        CHECK_LE(S6.vid_part, 2e-6);
        CHECK_LE(S6.vid_prim, 4e-6);
        CHECK(S6.codes_bad == 0);
        CHECK_LE(S6.rebind_diff, 2e-6);
        CHECK_LE(S6.import_gl_diff, 2e-6);
        CHECK_LE(S6.import_d3d_diff, 2e-6);
        CHECK(S6.vid_refused_ok == S6.vid_refused_n);
        printf("  v0.10.5 gray: R8 and RG8 with sRGB and gamma 2.2 transfers, %ld cases, %.2e; refusals and control %ld of %ld\n",
               S6.gray_cases, S6.gray_diff, S6.gray_refused_ok, S6.gray_refused_n);
        CHECK_LE(S6.gray_diff, 2e-6);
        CHECK(S6.gray_refused_ok == S6.gray_refused_n);
        if (S6.vid_refused_n) CHECK(S6.gray_cases == 4 && S6.gray_refused_n == 5);
#if defined(_WIN32)
        if (dev != YGFX_HL_SWIFTSHADER && S6.vid_refused_n) CHECK(S6.d3d_ran == 1);
        if (S6.d3d_ran) {
            int v;
            for (v = 0; v < 2; v++) {
                printf("  v0.4 video: NV12 from a second D3D11 device by %s: %s",
                       v ? "a legacy shared handle, after its flush and event query" : "an NT handle with a keyed mutex",
                       S6.shared_diff[v] >= 0 ? "read back, max " : "not run: ");
                if (S6.shared_diff[v] >= 0) printf("%.2e\n", S6.shared_diff[v]);
                else printf("%s failed\n", S6.shared_why[v]);
                if (S6.shared_diff[v] >= 0) CHECK_LE(S6.shared_diff[v], 2e-6);
            }
        }
#endif
    }
    printf("  v0.4 instances against the elements drawn alone: gabor %.2e, grating and noise %.2e, shapes %.2e, image %.2e, "
           "user %.2e; palette integers wrong %ld, fractions %.2e; hit_index wrong %ld of %ld (ties %ld); bindings wrong %ld; "
           "refusals %ld of %ld\n",
           S6.inst_diff[0], S6.inst_diff[1], S6.inst_diff[2], S6.inst_diff[3], S6.inst_diff[4], S6.pal_bad, S6.pal_frac,
           S6.hit_bad, S6.hit_n, S6.hit_ties, S6.bind_bad, S6.inst_refused_ok, S6.inst_refused_n);
    {
        int k;
        for (k = 0; k < 5; k++) CHECK_LE(S6.inst_diff[k], 3e-5);   /* 3x the worst renderer (user, 9.2e-6) */
        CHECK(S6.pal_bad == 0);
        CHECK_LE(S6.pal_frac, 2e-7);
        CHECK(S6.hit_bad == 0);
        CHECK(S6.bind_bad == 0);
        CHECK(S6.inst_refused_ok == S6.inst_refused_n);
    }
    printf("  v0.4 order: %ld frames reordered against call order, %ld differed; %ld draw calls saved; a draw that passes "
           "several and joins an earlier run: %s\n", S6.order_frames, S6.order_bad, S6.order_saved,
           S6.order_pass_ok ? "joined only where nothing it passed overlaps" : "WRONG");
    if (S6.order_frames) {
        CHECK(S6.order_bad == 0);
        CHECK(S6.order_saved > 0);
        CHECK(S6.order_pass_ok == 1);
    }
    if (S8.ran) {
        int v;
        printf("  v0.6 curve runs: exact area on resolved glyphs at rotation 0, max %.2e; mean edge error, worst rotation, rays "
               "%.4f %.4f %.4f %.4f, with exact area %.4f %.4f %.4f %.4f (8, 12, 24, 48 px); largest edge error %.3f, off the "
               "edges %.3f\n", S8.exact, S8.mean[0][0], S8.mean[0][1], S8.mean[0][2], S8.mean[0][3], S8.mean[1][0], S8.mean[1][1],
               S8.mean[1][2], S8.mean[1][3], S8.max_edge, S8.off);
        printf("  v0.6 curve runs: items alone against one run %.2e, a buffer against copied items %.2e, a target against the scene "
               "%.2e, a glyph added at run time %.2e; palette integers wrong %ld, fractions %.2e; hit wrong %ld of %ld, topmost wrong "
               "%ld; a 0.005 px move changes a pixel by at most %.4f (exact area), %.4f (rays, a smooth outline), %.4f (rays at "
               "corners); refusals %ld of %ld\n", S8.alone, S8.buffer, S8.target, S8.added, S8.pal_bad, S8.pal_frac, S8.hit_bad,
               S8.hit_n, S8.top_bad, S8.jump, S8.jump_ray, S8.jump_corner, S8.refused_ok, S8.refused_n);
        CHECK_LE(S8.exact, 5e-5);
        CHECK_LE(S8.mean[0][0], 0.035); CHECK_LE(S8.mean[0][1], 0.024); CHECK_LE(S8.mean[0][2], 0.012); CHECK_LE(S8.mean[0][3], 0.005);
        CHECK_LE(S8.mean[1][0], 0.035); CHECK_LE(S8.mean[1][1], 0.024); CHECK_LE(S8.mean[1][2], 0.012); CHECK_LE(S8.mean[1][3], 0.005);
        CHECK_LE(S8.max_edge, 0.6);
        CHECK_LE(S8.off, 0.3);
        CHECK(S8.alone == 0.0);
        CHECK(S8.buffer == 0.0);
        CHECK_LE(S8.target, 1e-3);   /* the target holds RGBA16F */
        CHECK(S8.added == 0.0);
        CHECK(S8.pal_bad == 0);
        CHECK_LE(S8.pal_frac, 2e-7);
        CHECK(S8.hit_bad == 0);
        CHECK(S8.top_bad == 0);
        CHECK_LE(S8.jump, 0.02);       /* the move's own change: 0.005 px x a slope near 1 */
        CHECK_LE(S8.jump_ray, 0.05);
        printf("  v0.6 curve runs: the rays against their CPU form in double, every glyph at 24 px: %.2e\n", S8.cpu);
        printf("  v0.6 curve runs: .rays against the program without the exact area %.2e, against the exact area %.2e\n",
               S8.rays_same, S8.rays_exact);
        CHECK(S8.rays_same == 0.0);
        CHECK(S8.rays_exact > 1e-3);
        if (S8.ol_ran) {
            printf("  v0.6 curve runs: ysp/outline.h's set of the test glyphs (resolved), 12 px: against the exact coverage %.2e "
                   "(every glyph); against the test builder's set where both are resolved %.2e\n", S8.ol_exact, S8.ol_same);
            CHECK_LE(S8.ol_exact, 5e-5);
            CHECK_LE(S8.ol_same, 5e-5);
        }
        printf("  v0.6 curve runs: cached in a target and composited 1:1, against drawn into the scene: RGBA16F over black %.2e, "
               "over gray %.2e; RGBA32F %.2e; an RGBA32F target in an RGBA16F scene over gray %.2e\n", S8.cached[0], S8.cached[1],
               S8.cached[2], S8.cached[3]);
        CHECK(S8.cached[0] == 0.0);
        CHECK_LE(S8.cached[1], 4.9e-4);   /* one f16 step below 1 is 4.88e-4 */
        CHECK(S8.cached[2] == 0.0);
        CHECK(S8.cached[3] == 0.0);
        CHECK_LE(S8.cpu, 1e-3);
        CHECK(S8.refused_ok == S8.refused_n);
        if (cs_dev != cs_xr_dev)
            for (v = 0; v < 2; v++) {
                printf("  v0.6 curve runs, %s, against the first renderer pixel by pixel: max %.2e, %ld pixels above 1e-3\n",
                       v ? "exact area" : "rays", cs_xdiff[v].max, cs_xdiff[v].n_1e3);
                /* a pixel whose ray passes a corner within rounding takes the
                 * corner's jump on one renderer and not on another: a few */
                CHECK(cs_xdiff[v].n_1e3 <= 3);
            }
        memset(cs_xdiff, 0, sizeof cs_xdiff);
        memset(&S8, 0, sizeof S8);
    }
    s10_report();
    s11_report();
    if (S9.ran) {
        static const char* const fn[3] = { "RGBA32F", "RGBA16F", "R16F" };
        int f, q;
        for (f = 0; f < 3; f++)
            for (q = 0; q < 2; q++)
                printf("  v0.6 blur %s %dx: rect against the erf product, sigma 0.5 1 2 4 8 16: %.2e %.2e %.2e %.2e %.2e %.2e\n",
                       fn[f], q + 1, S9.rect[f][q][0], S9.rect[f][q][1], S9.rect[f][q][2], S9.rect[f][q][3], S9.rect[f][q][4],
                       S9.rect[f][q][5]);
        printf("  v0.6 blur: an R16F result drawn as tinted coverage %.2e; a rect at the layer's corner, sigma 4 %.2e; refusals %ld of %ld\n",
               S9.cover, S9.edge, S9.refused_ok, S9.refused_n);
        {   /* about 1.25 times the measured error (docs/gfx.md); f16 storage limits the larger sigma */
            static const double lim[3][2][6] = {
                { { 0.26, 0.04, 0.011, 2.8e-3, 7e-4, 2e-4 }, { 0.048, 0.0096, 2.7e-3, 7e-4, 1.8e-4, 5e-5 } },
                { { 0.26, 0.04, 0.011, 3.0e-3, 1.2e-3, 1e-3 }, { 0.048, 0.010, 2.8e-3, 1.25e-3, 1.2e-3, 1.1e-3 } },
                { { 0.26, 0.04, 0.011, 3.0e-3, 1.2e-3, 1e-3 }, { 0.048, 0.010, 2.8e-3, 1.25e-3, 1.2e-3, 1.1e-3 } } };
            int k;
            for (f = 0; f < 3; f++) for (q = 0; q < 2; q++) for (k = 0; k < 6; k++) CHECK_LE(S9.rect[f][q][k], lim[f][q][k]);
        }
        CHECK_LE(S9.cover, 1e-3);   /* R16F texels */
        CHECK_LE(S9.edge, 2.8e-3);
        CHECK(S9.refused_ok == S9.refused_n);
        memset(&S9, 0, sizeof S9);
    }
    mc_clear(&g_mc);
    g_where = "cpu";
    return 1;
}

/* ------------------------------------------------- v0.6 curve sets (CPU) */

/* The format's test vector (docs/gfx.md, "Curve sets: the format",
 * appendix): a square with a square hole, nonzero, 2 x 2 bands. The outline
 * builder's first test compares its bytes with these. */
static const double cs_sq_outer[] = { 0, 0, 0, 0,  1, 0, 1, 0,  1, 1, 1, 1,  0, 1, 0, 1 };
static const double cs_sq_hole[]  = { 0.25, 0.25, 0.25, 0.25,  0.25, 0.75, 0.25, 0.75,
                                      0.75, 0.75, 0.75, 0.75,  0.75, 0.25, 0.75, 0.25 };

static void cs_appendix(tcs_set* s) {
    const tcs_contour c[2] = { { cs_sq_outer, 8 }, { cs_sq_hole, 8 } };
    tcs_init(s, 1);
    tcs_glyph(s, 0, c, 2, 2, 2, 0);
}

/* The test glyphs, in em units, y down; every one is drawn by the GL part
 * too. Lines have p1 = p0. */
#define CS_N 12
static double cs_pts[CS_N][4][96];
static int    cs_np[CS_N][4], cs_nc[CS_N];
static uint32_t cs_flags[CS_N];

static void cs_line(int g, int c, double x, double y) {   /* a line from x, y */
    double* p = cs_pts[g][c] + cs_np[g][c];
    p[0] = x; p[1] = y; p[2] = x; p[3] = y;
    cs_np[g][c] += 4;
}
static void cs_quad(int g, int c, double x, double y, double cx, double cy) {
    double* p = cs_pts[g][c] + cs_np[g][c];
    p[0] = x; p[1] = y; p[2] = cx; p[3] = cy;
    cs_np[g][c] += 4;
}
static void cs_rect(int g, int c, double x0, double y0, double x1, double y1, int ccw) {
    if (!ccw) { cs_line(g, c, x0, y0); cs_line(g, c, x1, y0); cs_line(g, c, x1, y1); cs_line(g, c, x0, y1); }
    else { cs_line(g, c, x0, y0); cs_line(g, c, x0, y1); cs_line(g, c, x1, y1); cs_line(g, c, x1, y0); }
}
/* an ellipse of 8 quadratics, clockwise on the screen, the first curve
 * from angle ph */
static void cs_ellipse_ph(int g, int c, double cx, double cy, double rx, double ry, double ph) {
    int k;
    const double pi = 3.14159265358979323846, m = 1.0 / cos(pi / 8);
    for (k = 0; k < 8; k++) {
        double a = ph + k * pi / 4, b = a + pi / 8;
        cs_quad(g, c, cx + rx * cos(a), cy + ry * sin(a), cx + rx * m * cos(b), cy + ry * m * sin(b));
    }
}
static void cs_ellipse(int g, int c, double cx, double cy, double rx, double ry) {
    cs_ellipse_ph(g, c, cx, cy, rx, ry, 0.0);
}

static void cs_glyphs(void) {
    static int made = 0;
    int k;
    const double pi = 3.14159265358979323846;
    if (made) return;
    made = 1;
    memset(cs_np, 0, sizeof cs_np);
    cs_flags[0] = cs_flags[1] = cs_flags[2] = cs_flags[7] = cs_flags[8] = cs_flags[10] = cs_flags[11] = YGFX_CSET_RESOLVED;
    /* 0 a rect; 1 the square with a hole; 2 a circle with its extrema inside
     * curves, so that rays near them cross one curve twice */
    cs_rect(0, 0, 0.1, -0.7, 0.6, 0.0, 0); cs_nc[0] = 1;
    memcpy(cs_pts[1][0], cs_sq_outer, sizeof cs_sq_outer); cs_np[1][0] = 16;
    memcpy(cs_pts[1][1], cs_sq_hole, sizeof cs_sq_hole); cs_np[1][1] = 16; cs_nc[1] = 2;
    cs_ellipse_ph(2, 0, 0.5, -0.35, 0.4, 0.4, pi / 8); cs_nc[2] = 1;
    /* 3 two overlapping rects (winding 2 in the overlap); 4 the same, even-odd */
    cs_rect(3, 0, 0.0, -0.7, 0.5, -0.1, 0); cs_rect(3, 1, 0.3, -0.5, 0.8, 0.0, 0); cs_nc[3] = 2;
    memcpy(cs_pts[4], cs_pts[3], sizeof cs_pts[3]); memcpy(cs_np[4], cs_np[3], sizeof cs_np[3]); cs_nc[4] = 2;
    cs_flags[4] = YGFX_CSET_EVENODD;
    /* 5 a pentagram (self-intersecting), nonzero; 6 the same, even-odd */
    for (k = 0; k < 5; k++) {
        double a = -pi / 2 + k * 4 * pi / 5;
        cs_line(5, 0, 0.5 + 0.45 * cos(a), -0.4 + 0.45 * sin(a));
    }
    cs_nc[5] = 1;
    memcpy(cs_pts[6], cs_pts[5], sizeof cs_pts[5]); memcpy(cs_np[6], cs_np[5], sizeof cs_np[5]); cs_nc[6] = 1;
    cs_flags[6] = YGFX_CSET_EVENODD;
    /* 7 thin stems: 0.025 em (0.3 px at 12 px per em) and 0.0667 em */
    cs_rect(7, 0, 0.1, -0.7, 0.125, 0.0, 0); cs_rect(7, 1, 0.4, -0.7, 0.4667, 0.0, 0); cs_nc[7] = 2;
    /* 8 a flat ellipse (tight curvature at its ends) and a slanted bar */
    cs_ellipse(8, 0, 0.45, -0.55, 0.42, 0.08);
    cs_line(8, 1, 0.1, -0.05); cs_line(8, 1, 0.3, -0.4); cs_line(8, 1, 0.42, -0.4); cs_line(8, 1, 0.22, -0.05);
    cs_nc[8] = 2;
    /* 9 a CJK-like grid of strokes, overlapping at the crossings */
    for (k = 0; k < 2; k++) cs_rect(9, k, 0.05, -0.65 + k * 0.4, 0.85, -0.58 + k * 0.4, 0);
    for (k = 0; k < 2; k++) cs_rect(9, 2 + k, 0.2 + k * 0.4, -0.8, 0.27 + k * 0.4, 0.0, 0);
    cs_nc[9] = 4;
    /* 10 a ring far from the origin, where f16 would hold 0.03 em: f32 holds it */
    cs_ellipse(10, 0, 60.5, -40.4, 0.4, 0.4); cs_ellipse(10, 1, 60.5, -40.4, 0.18, 0.18);
    {   /* the hole the other way round */
        double t[96];
        int n = cs_np[10][1], j;
        for (j = 0; j < n / 4; j++) {
            int s = (n / 4 - j) % (n / 4);
            int e = (n / 4 - j - 1);
            t[4 * j] = cs_pts[10][1][4 * s]; t[4 * j + 1] = cs_pts[10][1][4 * s + 1];
            t[4 * j + 2] = cs_pts[10][1][4 * e + 2]; t[4 * j + 3] = cs_pts[10][1][4 * e + 3];
        }
        memcpy(cs_pts[10][1], t, (size_t)n * sizeof(double));
    }
    cs_nc[10] = 2;
    /* 11 a rect with a degenerate curve (all three points equal) in it */
    cs_line(11, 0, 0.1, -0.6); cs_line(11, 0, 0.7, -0.6); cs_quad(11, 0, 0.7, -0.1, 0.7, -0.1);
    cs_line(11, 0, 0.7, -0.1); cs_line(11, 0, 0.1, -0.1);
    cs_nc[11] = 1;
}

/* The test set: glyphs 0 .. CS_N - 1, then glyph CS_N empty and CS_N + 1
 * absent. bands 0 = the builder's default; flags | extra (backward lists). */
static void cs_build(tcs_set* s, int bands, uint32_t extra) {
    int g;
    cs_glyphs();
    tcs_init(s, CS_N + 2);
    for (g = 0; g < CS_N; g++) {
        tcs_contour c[4];
        int k;
        for (k = 0; k < cs_nc[g]; k++) { c[k].pts = cs_pts[g][k]; c[k].n = cs_np[g][k] / 2; }
        CHECK(tcs_glyph(s, (uint32_t)g, c, cs_nc[g], bands, bands, cs_flags[g] | extra) == 0);
    }
    CHECK(tcs_glyph(s, CS_N, NULL, 0, 0, 0, 0) == 0);
}

/* The winding number by brute force over the contours, with roots from the
 * quadratic formula and the half-open [0, 1) rule: none of the header's
 * arithmetic. Points are the floats the set holds. */
static int cs_wind_ref(int g, double x, double y) {
    int c, j, w = 0;
    for (c = 0; c < cs_nc[g]; c++) {
        int n = cs_np[g][c] / 4;
        for (j = 0; j < n; j++) {
            const double* p = cs_pts[g][c] + 4 * j;
            const double* q = cs_pts[g][c] + 4 * ((j + 1) % n);
            double x0 = (float)p[0], y0 = (float)p[1], x1 = (float)p[2], y1 = (float)p[3], x2 = (float)q[0], y2 = (float)q[1];
            double A = y0 - 2 * y1 + y2, B = 2 * (y1 - y0), C = y0 - y, ts[2];
            int nt = 0, r;
            if (fabs(A) < 1e-300) { if (B != 0) ts[nt++] = -C / B; }
            else {
                double D = B * B - 4 * A * C;
                if (D >= 0) { ts[nt++] = (-B - sqrt(D)) / (2 * A); ts[nt++] = (-B + sqrt(D)) / (2 * A); }
            }
            for (r = 0; r < nt; r++) {
                double t = ts[r], xt, dy;
                if (!(t >= 0 && t < 1)) continue;
                xt = (1 - t) * (1 - t) * x0 + 2 * t * (1 - t) * x1 + t * t * x2;
                dy = 2 * A * t + B;
                if (xt > x && dy != 0) w += dy > 0 ? 1 : -1;
            }
        }
    }
    return w;
}

/* The test glyphs through ysp/outline.h: the same contours as paths, a set
 * whose glyphs it resolved (overlaps removed). */
static yol_ctx cs_ol_cx;
static yol_cset cs_ol;
static int cs_ol_ok = -1;
static void cs_ol_build(void) {
    int g, c, j;
    yol_path pa;
    yol_cset_desc od;
    if (cs_ol_ok >= 0) return;
    cs_glyphs();
    cs_ol_ok = 0;
    if (!yol_open(&cs_ol_cx, NULL)) return;
    memset(&od, 0, sizeof od);
    od.n_glyphs = CS_N;
    if (!yol_cset_open(&cs_ol, &cs_ol_cx, &od)) return;
    yol_path_init(&pa, &cs_ol_cx);
    for (g = 0; g < CS_N; g++) {
        yol_path_clear(&pa);
        pa.rule = (cs_flags[g] & YGFX_CSET_EVENODD) ? YOL_EVENODD : YOL_NONZERO;
        for (c = 0; c < cs_nc[g]; c++) {
            int n = cs_np[g][c] / 4;
            for (j = 0; j < n; j++) {
                const double* q = cs_pts[g][c] + 4 * j;
                const double* e = cs_pts[g][c] + 4 * ((j + 1) % n);
                /* the floats the test builder stores, so both sets hold one shape */
                double x0 = (float)q[0], y0 = (float)q[1], cx = (float)q[2], cy = (float)q[3], x1 = (float)e[0], y1 = (float)e[1];
                if (j == 0) yol_move(&pa, x0, y0);
                if (cx == x0 && cy == y0) yol_line(&pa, x1, y1);
                else yol_quad(&pa, cx, cy, x1, y1);
            }
            yol_close_path(&pa);
        }
        if (yol_path_end(&pa) != YOL_OK || yol_cset_add(&cs_ol, (uint32_t)g, &pa) != YOL_OK) { yol_path_free(&pa); return; }
    }
    yol_path_free(&pa);
    cs_ol_ok = 1;
}

static uint32_t cs_rng = 777u;
static double cs_u(void) { cs_rng = cs_rng * 1664525u + 1013904223u; return (cs_rng >> 8) * (1.0 / 16777216.0); }

/* Appendix A's words, as the builder writes them (checked in by hand once,
 * then compared bit for bit). */
static const uint32_t cs_appendix_words[] = {
    0x43505359u, 1, 1, 0, 0, 0, 0, 0,  12, 0, 0, 0,
    0x00000000u, 0x00000000u, 0x3f800000u, 0x3f800000u, 0x00020002u, 0, 0, 10,
    36, 6, 0, 0,  42, 6, 0, 0,  48, 6, 0, 0,  54, 6, 0, 0,
    0, 1, 7, 8, 5, 3,      1, 2, 6, 7, 5, 3,
    2, 3, 5, 6, 8, 0,      1, 2, 6, 7, 8, 0,
};

static void test_v06_cset_cpu(void) {
    tcs_set s;
    ygfx_cset_desc d;
    char msg[256];
    long wrong = 0, n = 0;
    int g, k, bands, bwd;
    /* the test vector */
    cs_appendix(&s);
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    CHECK(ygfx_cset_check(&d, msg, sizeof msg) == YGFX_OK);
    if (getenv("YGFX_TEST_VERBOSE")) {
        uint32_t i;
        printf("v0.6 appendix A: %u texels, %u words:", s.n_texels, s.n_words);
        for (i = 0; i < s.n_words; i++) printf("%s%u", i % 12 ? " " : "\n  ", s.words[i]);
        printf("\n");
    }
    CHECK(s.n_texels == 10);
    CHECK(s.n_words == sizeof cs_appendix_words / 4 && memcmp(s.words, cs_appendix_words, sizeof cs_appendix_words) == 0);
    CHECK(ygfx_cset_winding(&d, 0, 0.1, 0.1) == 1);
    CHECK(ygfx_cset_winding(&d, 0, 0.5, 0.5) == 0);
    CHECK(ygfx_cset_winding(&d, 0, 1.5, 0.5) == 0);
    CHECK(ygfx_cset_winding(&d, 1, 0.5, 0.5) == 0);   /* past the table */
    /* one fault per rule: each refused, with the word or the glyph named */
    {
        static const struct { int word; uint32_t value; const char* says; } F[] = {
            { 0, 0x12345678u, "magic" }, { 1, 2, "version" }, { 5, 1, "reserved" }, { 2, 1u << 24, "glyph table" },
            { 8, 13, "multiple of 4" }, { 8, 4, "multiple of 4" }, { 13, 0x7fc00000u, "not finite" },
            { 16, 0x00000002u, "one band count" }, { 14, 0, "bbox" }, { 14, 0x3f000000u, "passes its bbox" },
            { 17, 8, "reserved bits" }, { 22, 36, "backward list" }, { 19, 30, "texels" },
            { 20, 9999, "passes the words" }, { 36, 9, "not a curve" }, { 36, 4, "not sorted" }, { 36, 5, "not sorted" },
        };
        size_t f;
        for (f = 0; f < sizeof F / sizeof F[0]; f++) {
            uint32_t* w2 = (uint32_t*)malloc(4u * s.n_words);
            ygfx_cset_desc d2 = d;
            int rc;
            memcpy(w2, s.words, 4u * s.n_words);
            w2[F[f].word] = F[f].value;
            if (F[f].word == 22) w2[23] = 1;   /* a backward list without the flag */
            d2.words = w2;
            rc = ygfx_cset_check(&d2, msg, sizeof msg);
            if (rc != YGFX_ERR_FORMAT || !strstr(msg, F[f].says)) {
                fprintf(stderr, "gfx_test [cpu]: FAIL v0.6 check fault %d (word %d = 0x%x): rc %d, \"%s\"\n",
                        (int)f, F[f].word, F[f].value, rc, msg);
                g_failures++;
            }
            free(w2);
        }
        {   /* a texel that is not finite */
            float* t2 = (float*)malloc(16u * s.n_texels);
            ygfx_cset_desc d2 = d;
            memcpy(t2, s.texels, 16u * s.n_texels);
            t2[13] = (float)(1e300 * 1e300);
            d2.texels = t2;
            CHECK(ygfx_cset_check(&d2, msg, sizeof msg) == YGFX_ERR_FORMAT && strstr(msg, "texel 3"));
            free(t2);
        }
    }
    tcs_free(&s);
    /* the test set: the CPU winding against brute force at random points,
     * with the builder's default bands, 1, 3 and 16 bands, backward lists */
    for (bands = 0; bands < 4; bands++)
        for (bwd = 0; bwd < 2; bwd++) {
            static const int nb[4] = { 0, 1, 3, 16 };
            cs_build(&s, nb[bands], bwd ? YGFX_CSET_BACKWARD : 0u);
            memset(&d, 0, sizeof d);
            d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
            k = ygfx_cset_check(&d, msg, sizeof msg);
            if (k != YGFX_OK) { fprintf(stderr, "gfx_test [cpu]: FAIL v0.6 test set: %s\n", msg); g_failures++; }
            for (g = 0; g < CS_N; g++) {
                uint32_t o = s.words[8 + g];
                double x0 = s.texels ? (double)ygfx__u2f(s.words[o]) : 0, y0 = ygfx__u2f(s.words[o + 1]);
                double x1 = ygfx__u2f(s.words[o + 2]), y1 = ygfx__u2f(s.words[o + 3]);
                double mx = 0.1 * (x1 - x0), my = 0.1 * (y1 - y0);
                int nc = 0, c, j;
                for (c = 0; c < cs_nc[g]; c++) nc += cs_np[g][c] / 4;
                for (k = 0; k < 2000 + 4 * nc; k++) {
                    double x = x0 - mx + (x1 - x0 + 2 * mx) * cs_u(), y = y0 - my + (y1 - y0 + 2 * my) * cs_u(), yr = y;
                    int a, b;
                    if (k >= 2000) {   /* on a curve's endpoint row: on the ray is not above, as at y just past it */
                        for (c = 0, j = (k - 2000) / 4; j >= cs_np[g][c] / 4; c++) j -= cs_np[g][c] / 4;
                        y = (float)cs_pts[g][c][4 * j + 1];
                        yr = y + 1e-9;
                    }
                    a = ygfx_cset_winding(&d, (uint32_t)g, x, y); b = cs_wind_ref(g, x, yr);
                    n++;
                    if (a != b) {
                        wrong++;
                        if (getenv("YGFX_TEST_VERBOSE")) printf("  v0.6 winding: glyph %d at %.9g %.9g: %d, brute force %d\n", g, x, y, a, b);
                    }
                }
            }
            CHECK(ygfx_cset_winding(&d, CS_N, 0.3, -0.3) == 0);       /* empty */
            CHECK(ygfx_cset_winding(&d, CS_N + 1, 0.3, -0.3) == 0);   /* absent */
            CHECK(ygfx_cset_winding(&d, CS_N + 9, 0.3, -0.3) == 0);   /* past the table */
            tcs_free(&s);
        }
    printf("v0.6 curve sets: appendix A checked bit for bit; the CPU winding against brute force wrong at %ld of %ld points\n",
           wrong, n);
    CHECK(wrong == 0);
    {   /* ysp/outline.h's set of the same contours: overlaps resolved, the
         * format checked, its fill equal to the brute force's */
        long ow = 0, on = 0;
        cs_ol_build();
        if (cs_ol_ok) {
            memset(&d, 0, sizeof d);
            d.texels = cs_ol.texels; d.n_texels = cs_ol.n_texels; d.words = cs_ol.words; d.n_words = cs_ol.n_words;
            CHECK(ygfx_cset_check(&d, msg, sizeof msg) == YGFX_OK);
            for (g = 0; g < CS_N; g++) {
                uint32_t o = d.words[8 + g];
                double x0 = ygfx__u2f(d.words[o]), y0 = ygfx__u2f(d.words[o + 1]);
                double x1 = ygfx__u2f(d.words[o + 2]), y1 = ygfx__u2f(d.words[o + 3]);
                CHECK((d.words[o + 5] & YGFX_CSET_RESOLVED) != 0);
                for (k = 0; k < 2000; k++) {
                    double x = x0 + (x1 - x0) * cs_u(), y = y0 + (y1 - y0) * cs_u();
                    int a = ygfx_cset_winding(&d, (uint32_t)g, x, y), b = cs_wind_ref(g, x, y);
                    int fill = (cs_flags[g] & YGFX_CSET_EVENODD) ? (b & 1) != 0 : b != 0;
                    on++;
                    ow += (a != 0) != fill || (a != 0 && a != 1);
                }
            }
        } else {
            fprintf(stderr, "gfx_test [cpu]: FAIL v0.6 ysp/outline.h set: %s\n", yol_error(&cs_ol_cx));
            g_failures++;
        }
        printf("v0.6 curve sets: ysp/outline.h's set of the test glyphs passes the check; its winding 0 or 1 and its fill "
               "against brute force wrong at %ld of %ld points\n", ow, on);
        CHECK(ow == 0);
    }
}

/* ------------------------------------------------- v0.6 runs (GL) */

/* The exact box-filter coverage of a pixel by a glyph, in double: the
 * curves mapped to the screen, each scanline's covered length in the pixel
 * from its crossings and the fill rule on its integer winding, integrated
 * over y by adaptive Gauss-Kronrod between the y of every endpoint and
 * extremum. None of the header's arithmetic. */
typedef struct cs_scr { double p[6]; } cs_scr;   /* a curve on the screen */
static cs_scr cs_sc[256];
static int    cs_nsc, cs_eo;

/* glyph g's curves through S = o + M em */
static void cs_map(int g, const double o[2], const double M[4]) {
    int c, j;
    cs_nsc = 0;
    cs_eo = (cs_flags[g] & YGFX_CSET_EVENODD) != 0;
    for (c = 0; c < cs_nc[g]; c++) {
        int n = cs_np[g][c] / 4;
        for (j = 0; j < n; j++) {
            const double* p = cs_pts[g][c] + 4 * j;
            const double* q = cs_pts[g][c] + 4 * ((j + 1) % n);
            double e[6];
            int k;
            e[0] = (float)p[0]; e[1] = (float)p[1]; e[2] = (float)p[2]; e[3] = (float)p[3]; e[4] = (float)q[0]; e[5] = (float)q[1];
            for (k = 0; k < 3; k++) {
                cs_sc[cs_nsc].p[2 * k] = o[0] + M[0] * e[2 * k] + M[1] * e[2 * k + 1];
                cs_sc[cs_nsc].p[2 * k + 1] = o[1] + M[2] * e[2 * k] + M[3] * e[2 * k + 1];
            }
            cs_nsc++;
        }
    }
}

static int cs_dcmp(const void* a, const void* b) {
    double x = ((const double*)a)[0], y = ((const double*)b)[0];
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* the covered length of [x0, x1] on the scanline y; rows: the curves that
 * meet the pixel's rows (nrow of them), or all when nrow < 0 */
static double cs_line_cov(const int* rows, int nrow, double y, double x0, double x1) {
    double xs[1024];
    int n = 0, k, w = 0;
    double len = 0.0, prev = -1e300;
    int nk = nrow < 0 ? cs_nsc : nrow;
    for (k = 0; k < nk; k++) {
        const double* p = cs_sc[nrow < 0 ? k : rows[k]].p;
        double A = p[1] - 2 * p[3] + p[5], B = 2 * (p[3] - p[1]), C = p[1] - y, ts[2];
        int nt = 0, r;
        if (fabs(A) < 1e-14 * (fabs(B) + 1e-300)) { if (B != 0) ts[nt++] = -C / B; }
        else {
            double D = B * B - 4 * A * C;
            if (D >= 0) { double sq = sqrt(D), q = -0.5 * (B + (B >= 0 ? sq : -sq)); ts[nt++] = q / A; if (q != 0) ts[nt++] = C / q; }
        }
        for (r = 0; r < nt && n < 510; r++) {
            double t = ts[r], dy;
            if (!(t >= 0 && t < 1)) continue;
            dy = 2 * A * t + B;
            if (dy == 0) continue;
            xs[2 * n] = (1 - t) * (1 - t) * p[0] + 2 * t * (1 - t) * p[2] + t * t * p[4];
            xs[2 * n + 1] = dy > 0 ? 1 : -1;
            n++;
        }
    }
    for (k = 1; k < n; k++) {   /* few crossings: insertion sort */
        double x = xs[2 * k], sg = xs[2 * k + 1];
        int m = k - 1;
        while (m >= 0 && xs[2 * m] > x) { xs[2 * m + 2] = xs[2 * m]; xs[2 * m + 3] = xs[2 * m + 1]; m--; }
        xs[2 * m + 2] = x; xs[2 * m + 3] = sg;
    }
    for (k = 0; k <= n; k++) {
        double x = k < n ? xs[2 * k] : 1e300;
        int in = cs_eo ? (w & 1) != 0 : w != 0;
        if (in) {
            double a = prev > x0 ? prev : x0, b = x < x1 ? x : x1;
            if (b > a) len += b - a;
        }
        if (k < n) { w += (int)xs[2 * k + 1]; prev = x; }
    }
    return len;
}

static const double cs_gk_x[8] = { 0.991455371120812639, 0.949107912342758525, 0.864864423359769073, 0.741531185599394440,
                                   0.586087235467691130, 0.405845151377397167, 0.207784955007898468, 0.0 };
static const double cs_gk_wk[8] = { 0.022935322010529225, 0.063092092629978553, 0.104790010322250184, 0.140653259715525919,
                                    0.169004726639267903, 0.190350578064785410, 0.204432940075298892, 0.209482141084727828 };
static const double cs_gk_wg[4] = { 0.129484966168869693, 0.279705391489276668, 0.381830050505118945, 0.417959183673469388 };

static double cs_gk(const int* rows, int nrow, double a, double b, double x0, double x1, int depth) {
    double c = 0.5 * (a + b), h = 0.5 * (b - a), k = 0, gs = 0;
    int i;
    for (i = 0; i < 8; i++) {
        double f1 = cs_line_cov(rows, nrow, c - h * cs_gk_x[i], x0, x1), f2 = i < 7 ? cs_line_cov(rows, nrow, c + h * cs_gk_x[i], x0, x1) : 0;
        k += cs_gk_wk[i] * (i < 7 ? f1 + f2 : f1);
        if (i % 2 == 1) gs += cs_gk_wg[i / 2] * (f1 + f2);
        if (i == 7) gs += cs_gk_wg[3] * f1;
    }
    k *= h; gs *= h;
    if (depth > 8 || fabs(k - gs) < 1e-9) return k;
    return cs_gk(rows, nrow, a, c, x0, x1, depth + 1) + cs_gk(rows, nrow, c, b, x0, x1, depth + 1);
}

/* pixel (i, j)'s exact coverage */
static double cs_pix_ref(int i, int j) {
    double br[1100];
    int nb = 0, k, rows[256], nrow = 0;
    double y0 = j, y1 = j + 1, cov = 0;
    br[nb++] = y0; br[nb++] = y1;
    for (k = 0; k < cs_nsc && nb < 1090; k++) {
        const double* p = cs_sc[k].p;
        double ys[3], den = p[1] - 2 * p[3] + p[5];
        int m;
        ys[0] = p[1]; ys[1] = p[5]; ys[2] = y0 - 1;
        if (den != 0) { double t = (p[1] - p[3]) / den; if (t > 0 && t < 1) ys[2] = (1 - t) * (1 - t) * p[1] + 2 * t * (1 - t) * p[3] + t * t * p[5]; }
        for (m = 0; m < 3; m++) if (ys[m] > y0 && ys[m] < y1) br[nb++] = ys[m];
        {   /* where the curve crosses the pixel's sides, the scanline's length has a kink */
            int sd;
            for (sd = 0; sd < 2; sd++) {
                double X = i + sd, A = p[0] - 2 * p[2] + p[4], B = 2 * (p[2] - p[0]), C = p[0] - X, D;
                double ts[2];
                int nt = 0, r;
                if (A == 0) { if (B != 0) ts[nt++] = -C / B; }
                else if ((D = B * B - 4 * A * C) >= 0) { ts[nt++] = (-B - sqrt(D)) / (2 * A); ts[nt++] = (-B + sqrt(D)) / (2 * A); }
                for (r = 0; r < nt; r++) {
                    double t = ts[r], yy;
                    if (!(t >= 0 && t <= 1)) continue;
                    yy = (1 - t) * (1 - t) * p[1] + 2 * t * (1 - t) * p[3] + t * t * p[5];
                    if (yy > y0 && yy < y1) br[nb++] = yy;
                }
            }
        }
    }
    qsort(br, (size_t)nb, sizeof(double), cs_dcmp);
    for (k = 0; k < cs_nsc; k++) {
        const double* p = cs_sc[k].p;
        double ly = p[1] < p[3] ? (p[1] < p[5] ? p[1] : p[5]) : (p[3] < p[5] ? p[3] : p[5]);
        double hy = p[1] > p[3] ? (p[1] > p[5] ? p[1] : p[5]) : (p[3] > p[5] ? p[3] : p[5]);
        if (hy >= y0 && ly <= y1) rows[nrow++] = k;
    }
    for (k = 0; k + 1 < nb; k++)
        if (br[k + 1] > br[k]) cov += cs_gk(rows, nrow, br[k], br[k + 1], i, i + 1, 0);
    return cov;
}

/* whether any curve's hull meets pixel (i, j) grown by a margin */
static int cs_pix_near(int i, int j, double m) {
    int k;
    for (k = 0; k < cs_nsc; k++) {
        const double* p = cs_sc[k].p;
        double lx = p[0], hx = p[0], ly = p[1], hy = p[1];
        int q;
        for (q = 1; q < 3; q++) {
            if (p[2 * q] < lx) lx = p[2 * q];
            if (p[2 * q] > hx) hx = p[2 * q];
            if (p[2 * q + 1] < ly) ly = p[2 * q + 1];
            if (p[2 * q + 1] > hy) hy = p[2 * q + 1];
        }
        if (hx >= i - m && lx <= i + 1 + m && hy >= j - m && ly <= j + 1 + m) return 1;
    }
    return 0;
}

/* The probe's conditions: every test glyph at a size, a rotation (each
 * item's ori about its glyph box's center) and a subpixel offset, in one run;
 * the scene against the exact coverage. Returns the edge pixels' max and
 * mean error, and the largest error off the edges. */
typedef struct cs_err { double max, mean, off, oref, ogot; long n_edge; int wi, wj, oi, oj; } cs_err;

static int cs_layout(tcs_set* s, double size, double rot, double dx, double dy, ygfx_citem* it, double org[][2], double* cell) {
    int g, n = 0;
    *cell = 1.6 * size;
    for (g = 0; g < CS_N; g++) {
        uint32_t o = s->words[8 + g];
        double bx0 = ygfx__u2f(s->words[o]), by0 = ygfx__u2f(s->words[o + 1]);
        double bx1 = ygfx__u2f(s->words[o + 2]), by1 = ygfx__u2f(s->words[o + 3]);
        int per = (int)((W - 8) / *cell);
        double cx = 4 + (g % per) * *cell + 0.5 * *cell + dx, cy = 4 + (g / per) * *cell + 0.5 * *cell + dy;
        if (4 + (g / per + 1) * *cell > H) break;
        memset(&it[n], 0, sizeof it[n]);
        /* the glyph box's center on the cell's center */
        it[n].x = (float)(cx - size * 0.5 * (bx0 + bx1));
        it[n].y = (float)(cy - size * 0.5 * (by0 + by1));
        it[n].glyph = (float)g;
        it[n].ori = (float)rot;
        it[n].scale = 1; it[n].gate = 1; it[n].contrast = 1;
        org[n][0] = it[n].x; org[n][1] = it[n].y;
        n++;
    }
    return n;
}

/* The exact coverage of every pixel by every item of the layout, summed
 * (cells' margins overlap), cached per condition: the reference does not
 * depend on the variant drawn. owner: the item whose curves pass the pixel. */
static float* cs_ref_cache[CS_NCOND];
static signed char* cs_own_cache[CS_NCOND];

/* one pixel of one item's box (a box has too few rows to share them out):
 * pixels of an item are apart, items run in turn */
typedef struct cr_ctx { int i0, nw, j0, k; float* ref; signed char* own; } cr_ctx;
static void cr_pix(void* p, int idx) {
    const cr_ctx* c = (const cr_ctx*)p;
    int i = c->i0 + idx % c->nw, j = c->j0 + idx / c->nw;
    double r;
    if (i < 0 || j < 0 || i >= W || j >= H) return;
    if (cs_pix_near(i, j, 1e-9)) { r = cs_pix_ref(i, j); c->own[j * W + i] = (signed char)c->k; }
    else r = cs_line_cov(NULL, -1, j + 0.5, i + 0.5 - 1e-9, i + 0.5 + 1e-9) > 0 ? 1.0 : 0.0;
    c->ref[j * W + i] += (float)r;
}

static void cs_reference(tcs_set* s, const ygfx_citem* it, int n, double size, double rot, int cond) {
    int k;
    float* ref;
    signed char* own;
    if (cs_ref_cache[cond]) return;
    ref = (float*)calloc((size_t)W * H, sizeof(float));
    own = (signed char*)malloc((size_t)W * H);
    if (!ref || !own) { free(ref); free(own); return; }
    if (getenv("YGFX_TEST_REFDIR")) {   /* a development seam: references kept on disk */
        char path[600];
        FILE* fp;
        snprintf(path, sizeof path, "%s/ref%02d.bin", getenv("YGFX_TEST_REFDIR"), cond);
        fp = fopen(path, "rb");
        if (fp) {
            size_t a = fread(ref, sizeof(float), (size_t)W * H, fp), b = fread(own, 1, (size_t)W * H, fp);
            fclose(fp);
            if (a == (size_t)W * H && b == (size_t)W * H) { cs_ref_cache[cond] = ref; cs_own_cache[cond] = own; return; }
        }
    }
    memset(own, -1, (size_t)W * H);
    for (k = 0; k < n; k++) {
        int g = (int)it[k].glyph;
        uint32_t o = s->words[8 + g];
        double bc[2], o2[2], M[4], c = cos(rot * 3.14159265358979323846 / 180), sn = sin(rot * 3.14159265358979323846 / 180);
        double lo[2] = { 1e9, 1e9 }, hi[2] = { -1e9, -1e9 };
        int q;
        bc[0] = 0.5 * (ygfx__u2f(s->words[o]) + ygfx__u2f(s->words[o + 2]));
        bc[1] = 0.5 * (ygfx__u2f(s->words[o + 1]) + ygfx__u2f(s->words[o + 3]));
        /* S = it + size (bc + R (em - bc)), R turning +x toward +y */
        M[0] = size * c; M[1] = -size * sn; M[2] = size * sn; M[3] = size * c;
        o2[0] = it[k].x + size * bc[0] - (M[0] * bc[0] + M[1] * bc[1]);
        o2[1] = it[k].y + size * bc[1] - (M[2] * bc[0] + M[3] * bc[1]);
        cs_map(g, o2, M);
        for (q = 0; q < cs_nsc; q++) {
            int m;
            for (m = 0; m < 3; m++) {
                if (cs_sc[q].p[2 * m] < lo[0]) lo[0] = cs_sc[q].p[2 * m];
                if (cs_sc[q].p[2 * m] > hi[0]) hi[0] = cs_sc[q].p[2 * m];
                if (cs_sc[q].p[2 * m + 1] < lo[1]) lo[1] = cs_sc[q].p[2 * m + 1];
                if (cs_sc[q].p[2 * m + 1] > hi[1]) hi[1] = cs_sc[q].p[2 * m + 1];
            }
        }
        {
            cr_ctx cr;
            cr.i0 = (int)floor(lo[0]) - 1; cr.nw = (int)floor(hi[0]) + 1 - cr.i0 + 1; cr.j0 = (int)floor(lo[1]) - 1; cr.k = k;
            cr.ref = ref; cr.own = own;
            par_rows(cr_pix, &cr, cr.nw * ((int)floor(hi[1]) + 1 - cr.j0 + 1));
        }
    }
    cs_ref_cache[cond] = ref;
    cs_own_cache[cond] = own;
    if (getenv("YGFX_TEST_REFDIR")) {
        char path[600];
        FILE* fp;
        snprintf(path, sizeof path, "%s/ref%02d.bin", getenv("YGFX_TEST_REFDIR"), cond);
        fp = fopen(path, "wb");
        if (fp) { fwrite(ref, sizeof(float), (size_t)W * H, fp); fwrite(own, 1, (size_t)W * H, fp); fclose(fp); }
    }
}

static cs_err cs_measure(const ygfx_citem* it, int cond, double* gmax) {
    cs_err e;
    int i, j;
    double sum = 0;
    const float* ref = cs_ref_cache[cond];
    const signed char* own = cs_own_cache[cond];
    memset(&e, 0, sizeof e);
    if (!ref) return e;
    for (j = 0; j < H; j++)
        for (i = 0; i < W; i++) {
            double r = ref[j * W + i], got = R->scene[(j * W + i) * 4], d = fabs(got - r);
            if (r > 1e-6 && r < 1 - 1e-6) {
                e.n_edge++;
                sum += d;
                if (d > e.max) { e.max = d; e.wi = i; e.wj = j; }
                if (own[j * W + i] >= 0) {
                    int g = (int)it[own[j * W + i]].glyph;
                    if (d > gmax[g]) gmax[g] = d;
                }
            } else if (d > e.off) { e.off = d; e.oi = i; e.oj = j; e.oref = r; e.ogot = got; }
        }
    e.mean = e.n_edge ? sum / (double)e.n_edge : 0;
    return e;
}

/* The first renderer's scenes, per variant and condition: every later
 * renderer is compared with it pixel by pixel. */

static void cs_cross(int v, int cond, int dev) {
    float* keep;
    int i;
    if (cs_xr_dev < 0) cs_xr_dev = dev;
    if (dev == cs_xr_dev) {
        if (!cs_xr[v][cond]) cs_xr[v][cond] = (float*)malloc(sizeof(float) * W * H);
        if (cs_xr[v][cond]) for (i = 0; i < W * H; i++) cs_xr[v][cond][i] = R->scene[4 * i];
        return;
    }
    keep = cs_xr[v][cond];
    if (!keep) return;
    for (i = 0; i < W * H; i++) {
        double d = fabs((double)keep[i] - R->scene[4 * i]);
        if (d > 1e-3) cs_xdiff[v].n_1e3++;
        if (d > cs_xdiff[v].max) { cs_xdiff[v].max = d; cs_xdiff[v].i = i % W; cs_xdiff[v].j = i / W; cs_xdiff[v].cond = cond; }
    }
}


/* YGFX_TEST_FULL=1: every condition of the text part (4 sizes, 3
 * rotations, 2 offsets: 24 exact references, about 25 s of CPU on 12
 * threads). The default runs 5 of them, chosen so that every mutant of
 * tests/mutate/gfx.toml is still caught (docs/gfx.md, v0.7). */
static int cs_cond_on(int si, int ri, int oi) {
    static const int def[5][3] = { { 0, 1, 0 }, { 1, 0, 1 }, { 1, 2, 0 }, { 2, 1, 1 }, { 3, 0, 0 } };
    int k;
    if (cs_full()) return 1;
    for (k = 0; k < 5; k++) if (def[k][0] == si && def[k][1] == ri && def[k][2] == oi) return 1;
    return 0;
}

/* One variant of the text program over the probe's conditions: sizes,
 * rotations and two subpixel offsets. */
static void cs_variant(tcs_set* s, int v3, int xi, const char* name) {
    ygfx_cset_desc d;
    ygfx_cset set;
    static const double sizes[4] = { 8, 12, 24, 48 }, rots[3] = { 0, 15, 45 };
    static const double offs[2][2] = { { 0, 0 }, { 0.37, 0.61 } };
    double gmax[CS_N], gmax0[CS_N], mx[4][3], mn[4][3];
    int si, ri, oi, g;
    ygfx__text_v3 = v3;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    memset(&d, 0, sizeof d);
    d.texels = s->texels; d.n_texels = s->n_texels; d.words = s->words; d.n_words = s->n_words;
    set = ygfx_cset_make(&R->g, &d);
    if (!set.id) { fprintf(stderr, "gfx_test [%s]: FAIL v0.6 cset_make: %s\n", g_where, ygfx_error(&R->g)); g_failures++; ygfx_close(&R->g); return; }
    for (g = 0; g < CS_N; g++) gmax[g] = gmax0[g] = 0;
    for (si = 0; si < 4; si++)
        for (ri = 0; ri < 3; ri++) {
            double m = 0, sum = 0;
            long n_e = 0;
            for (oi = 0; oi < 2; oi++) {
                const int cond = (si * 3 + ri) * 2 + oi;
                ygfx_citem it[CS_N];
                if (!cs_cond_on(si, ri, oi)) continue;
                double org[CS_N][2], cell;
                int n = cs_layout(s, sizes[si], rots[ri], offs[oi][0], offs[oi][1], it, org, &cell);
                ygfx_crun_desc rd;
                ygfx_stim run;
                cs_err e;
                memset(&rd, 0, sizeof rd);
                rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_TOP_LEFT; rd.set = set; rd.items = it; rd.n = n;
                rd.size = (float)sizes[si]; rd.w = W; rd.h = H; rd.color[0] = rd.color[1] = rd.color[2] = 1; rd.fields = YGFX_I_ORI;
                run = ygfx_crun(&R->g, &rd);
                cs_reference(s, it, n, sizes[si], rots[ri], cond);
                gl_frame(&run, 1);
                cs_cross(xi, cond, cs_dev);
                e = cs_measure(it, cond, ri == 0 ? gmax0 : gmax);
                if (e.max > m) m = e.max;
                sum += e.mean * (double)e.n_edge; n_e += e.n_edge;
                if (e.off > S8.off) S8.off = e.off;
                if (getenv("YGFX_TEST_VERBOSE"))
                    printf("    %s %2.0f px rot %2.0f off %d: edge max %.3f at %d %d, mean %.4f; off-edge %.2e at %d %d (ref %g got %g)\n",
                           name, sizes[si], rots[ri], oi, e.max, e.wi, e.wj, e.mean, e.off, e.oi, e.oj, e.oref, e.ogot);
            }
            mx[si][ri] = m;
            mn[si][ri] = n_e ? sum / (double)n_e : 0;
            if (m > S8.max_edge) S8.max_edge = m;
            if (mn[si][ri] > S8.mean[xi][si]) S8.mean[xi][si] = mn[si][ri];
        }
    printf("  v0.6 text %s, max / mean edge error by size (rows 8, 12, 24, 48 px) and rotation (0, 15, 45)%s:\n", name,
           cs_full() ? "" : "; the default 5 of 24 conditions, 0 = not run");
    for (si = 0; si < 4; si++)
        printf("    %2.0f px: %.3f / %.4f   %.3f / %.4f   %.3f / %.4f\n", sizes[si],
               mx[si][0], mn[si][0], mx[si][1], mn[si][1], mx[si][2], mn[si][2]);
    printf("    per glyph max, turned:");
    for (g = 0; g < CS_N; g++) printf(" %d:%.3f", g, gmax[g]);
    printf("\n    per glyph max, rotation 0:");
    for (g = 0; g < CS_N; g++) printf(" %d:%.2e", g, gmax0[g]);
    printf("\n");
    if (v3)   /* the resolved glyphs: exact area at rotation 0 */
        for (g = 0; g < CS_N; g++)
            if ((cs_flags[g] & YGFX_CSET_RESOLVED) && gmax0[g] > S8.exact) S8.exact = gmax0[g];
    ygfx_close(&R->g);
}

/* the scene's red channel, for bit-for-bit comparisons */
static void cs_red(float* out) { int i; for (i = 0; i < W * H; i++) out[i] = R->scene[4 * i]; }
static double cs_rdiff(const float* a) {
    int i;
    double m = 0;
    for (i = 0; i < W * H; i++) { double d = fabs((double)a[i] - R->scene[4 * i]); if (d > m) m = d; }
    return m;
}

static ygfx_crun_desc cs_rd(ygfx_cset set, ygfx_citem* it, int n, float size) {
    ygfx_crun_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_TOP_LEFT; rd.set = set; rd.items = it; rd.n = n;
    rd.size = size; rd.w = W; rd.h = H; rd.color[0] = rd.color[1] = rd.color[2] = 1;
    return rd;
}

/* The ray path of the text program on the CPU, in double, step for step:
 * for diagnosing a pixel (YGFX_TEST_VERBOSE). em = c + J^-1 (pixel -
 * screen center) for an item without turn or scale at origin (ox, oy) and
 * px per em sp; returns the coverage and fills the two rays. */
static double cpu_hp(double z, double a, double b) {
    double w0 = a < b ? a : b, w1 = a < b ? b : a, u = z + 0.5 * (a + b), r;
    if (u <= 0) return 0;
    if (u >= a + b) return 1;
    if (w0 < 1e-6) { double v = u / w1; return v < 0 ? 0 : (v > 1 ? 1 : v); }
    if (u < w0) return u * u / (2 * w0 * w1);
    if (u <= w1) return (u - 0.5 * w0) / w1;
    r = a + b - u;
    return 1 - r * r / (2 * w0 * w1);
}
static double cpu_cl(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static void cpu_ray(const tcs_set* s, int g, double px, double py, double sp, int hz, double* cov, double* wgt) {
    const uint32_t* w = s->words;
    uint32_t o = w[8 + g], nh = w[o + 4] & 0xFFFF, nv = w[o + 4] >> 16, fl = w[o + 5];
    double bb[4], E[2], F[2], c = 0, wg = 0, sg, hpx = 0.5, cap = 1.0;
    int nb = hz ? (int)nh : (int)nv, kb, j, end, bw, k;
    for (k = 0; k < 4; k++) bb[k] = ygfx__u2f(w[o + k]);
    {
        double ac = hz ? py : px, lo = hz ? bb[1] : bb[0], hi = hz ? bb[3] : bb[2];
        double al = hz ? px : py, alo = hz ? bb[0] : bb[1], ahi = hz ? bb[2] : bb[3];
        kb = (int)floor((ac - lo) * nb / (hi - lo));
        if (kb < 0) kb = 0;
        if (kb > nb - 1) kb = nb - 1;
        bw = (fl & 2) && al < 0.5 * (alo + ahi);
    }
    {
        uint32_t dw = o + 8 + 4 * (uint32_t)(hz ? kb : (int)nh + kb);
        j = (int)w[dw + (bw ? 2 : 0)]; end = j + (int)w[dw + (bw ? 3 : 1)];
    }
    E[0] = hz ? 1 : 0; E[1] = hz ? 0 : 1; F[0] = hz ? 0 : -1; F[1] = hz ? 1 : 0;
    sg = bw ? -1 : 1;
    E[0] *= sg; E[1] *= sg;
    for (; j < end; j++) {
        int r = (int)w[j];
        const float* T = s->texels + 4 * (size_t)r;
        double q[6], u[3], v[3], a, b, D, sD, qq, tr = 0, tf = 0, au, bu;
        int s1, s2, s3, two, fall, rise, m;
        for (m = 0; m < 3; m++) { q[2 * m] = T[2 * m] - px; q[2 * m + 1] = T[2 * m + 1] - py; }
        for (m = 0; m < 3; m++) { u[m] = q[2 * m] * E[0] + q[2 * m + 1] * E[1]; v[m] = q[2 * m] * F[0] + q[2 * m + 1] * F[1]; }
        if ((u[0] > u[1] ? (u[0] > u[2] ? u[0] : u[2]) : (u[1] > u[2] ? u[1] : u[2])) < -cap / sp) break;
        s1 = v[0] > 0; s2 = v[1] > 0; s3 = v[2] > 0;
        two = s1 == s3 && s2 != s1;
        fall = (s1 && !s3) || two; rise = (!s1 && s3) || two;
        if (!fall && !rise) continue;
        a = v[0] - 2 * v[1] + v[2]; b = v[0] - v[1];
        D = b * b - a * v[0];
        if (two && D < 0) continue;
        sD = sqrt(D > 0 ? D : 0);
        qq = b + (b >= 0 ? sD : -sD);
        if (qq != 0) { if (b >= 0) { tr = qq / a; tf = v[0] / qq; } else { tf = qq / a; tr = v[0] / qq; } }
        au = u[0] - 2 * u[1] + u[2]; bu = u[0] - u[1];
        for (m = 0; m < 2; m++) {
            double t = m ? tf : tr, sgn = m ? -1 : 1, up, A2[2], B2[2], Tn[2], dr[2], e2[2], de, f, wv, kk, tl;
            if (m ? !fall : !rise) continue;
            up = ((au * t - 2 * bu) * t + u[0]) * sp;
            A2[0] = q[0] - 2 * q[2] + q[4]; A2[1] = q[1] - 2 * q[3] + q[5]; B2[0] = q[0] - q[2]; B2[1] = q[1] - q[3];
            if (B2[0] == 0 && B2[1] == 0) { Tn[0] = A2[0]; Tn[1] = A2[1]; } else { Tn[0] = A2[0] * t - B2[0]; Tn[1] = A2[1] * t - B2[1]; }
            dr[0] = t * (t * A2[0] - 2 * B2[0]); dr[1] = t * (t * A2[1] - 2 * B2[1]);
            e2[0] = q[4] - q[0]; e2[1] = q[5] - q[1];
            de = sp * sqrt(fmin(dr[0] * dr[0] + dr[1] * dr[1], (dr[0] - e2[0]) * (dr[0] - e2[0]) + (dr[1] - e2[1]) * (dr[1] - e2[1])));
            tl = Tn[0] * Tn[0] + Tn[1] * Tn[1];
            if (fabs(up) > cap || !(tl > 0)) { c += up > 0 ? sgn : 0; continue; }
            {
                double n0 = -Tn[1] / sqrt(tl), n1 = Tn[0] / sqrt(tl), ce = fabs(n0 * E[0] + n1 * E[1]), aa = fabs(n0), bb2 = fabs(n1), z = up * ce;
                f = cpu_hp(z, aa, bb2);
                wv = ce * cpu_cl(1 - fabs(z) / (0.5 * (aa + bb2)));
                kk = de; (void)kk; (void)hpx;
            }
            c += sgn * f;
            if (wv > wg) wg = wv;
            if (getenv("YGFX_TEST_CPU"))
                printf("      %s ray, curve %d: root %s t %.4f up %.4f de %.3f f %.4f w %.4f\n", hz ? "x" : "y", r, m ? "fall" : "rise", t, up, de, f, wv);
        }
    }
    *cov = sg * c;
    *wgt = wg;
}

/* the shader's mix of the two rays */
static double cpu_cov(const tcs_set* s, int g, double ex, double ey, double sp) {
    double cx, wx, cy, wy, fx, fy, sw, e = 1.0 / 64.0, t;
    const uint32_t o = s->words[8 + g];
    const int eo = (s->words[o + 5] & 1) != 0;
    cpu_ray(s, g, ex, ey, sp, 1, &cx, &wx);
    cpu_ray(s, g, ex, ey, sp, 0, &cy, &wy);
    cx = fabs(cx); cy = fabs(cy);
    fx = eo ? 1 - fabs(1 - fmod(cx, 2)) : (cx < 1 ? cx : 1);
    fy = eo ? 1 - fabs(1 - fmod(cy, 2)) : (cy < 1 ? cy : 1);
    sw = wx + wy; t = sw / e < 1 ? sw / e : 1;
    return 0.5 * (fx + fy) + ((sw > 0 ? (fx * wx + fy * wy) / sw : 0) - 0.5 * (fx + fy)) * t;
}

/* one refusal: counted, and named when it was not refused */
static void cs_refused(int k, int ok) {
    S8.refused_n++;
    S8.refused_ok += ok != 0;
    if (!ok) printf("    v0.6 refusal %d was not refused\n", k);
}

static void gl_v06_text(stats* st) {
    static float ref[W * H], ref2[W * H];
    tcs_set s;
    ygfx_cset_desc d;
    ygfx_cset set;
    ygfx_citem it[CS_N];
    double org[CS_N][2], cell;
    int n, i, j, k;
    (void)st;
    S8.ran = 1;
    cs_build(&s, 0, 0);
    cs_variant(&s, 0, 0, "rays");
    tcs_free(&s);
    cs_build(&s, 0, YGFX_CSET_BACKWARD);
    cs_variant(&s, 1, 1, "rays and exact area, backward lists");
    ygfx__text_v3 = 1;

    /* ysp/outline.h's set of the same glyphs (overlaps resolved, so every
     * glyph takes the exact area) at 12 px, rotation 0: against the exact
     * coverage, and against the test builder's set on the glyphs that both
     * hold resolved */
    cs_ol_build();
    if (cs_ol_ok == 1 && gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) {
        static float mine[W * H];
        const int cond = (1 * 3 + 0) * 2 + 1;   /* 12 px, rotation 0, offset 1: a default condition */
        double gm[CS_N];
        ygfx_cset_desc od;
        ygfx_cset os, ts;
        ygfx_stim run;
        cs_err e;
        n = cs_layout(&s, 12, 0, 0.37, 0.61, it, org, &cell);
        cs_reference(&s, it, n, 12, 0, cond);
        memset(&d, 0, sizeof d);
        d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
        ts = ygfx_cset_make(&R->g, &d);
        memset(&od, 0, sizeof od);
        od.texels = cs_ol.texels; od.n_texels = cs_ol.n_texels; od.words = cs_ol.words; od.n_words = cs_ol.n_words;
        os = ygfx_cset_make(&R->g, &od);
        CHECK(ts.id != 0 && os.id != 0);
        {
            ygfx_crun_desc rd = cs_rd(ts, it, n, 12);
            run = ygfx_crun(&R->g, &rd);
            gl_frame(&run, 1);
            cs_red(mine);
            rd.set = os;
            run = ygfx_crun(&R->g, &rd);
            gl_frame(&run, 1);
        }
        for (k = 0; k < CS_N; k++) gm[k] = 0;
        e = cs_measure(it, cond, gm);
        S8.ol_exact = maxd(e.max, e.off);
        S8.ol_same = 0;
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                int ow = cs_own_cache[cond] ? cs_own_cache[cond][j * W + i] : -1;
                if (ow >= 0 && (cs_flags[(int)it[ow].glyph] & YGFX_CSET_RESOLVED))
                    S8.ol_same = maxd(S8.ol_same, fabs((double)mine[j * W + i] - R->scene[4 * (j * W + i)]));
            }
        S8.ol_ran = 1;
        ygfx_cset_free(&R->g, ts);
        ygfx_cset_free(&R->g, os);
        ygfx_close(&R->g);
    }

    /* runs: every item alone against all in one run; copied against a
     * buffer; reordered against call order; a target against the scene */
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) { tcs_free(&s); return; }
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words; d.keep = true;
    set = ygfx_cset_make(&R->g, &d);
    CHECK(set.id != 0);
    n = cs_layout(&s, 24, 15, 0.21, 0.43, it, org, &cell);
    for (k = 0; k < n; k++) { it[k].scale = 1.0f + 0.05f * (float)(k % 3); it[k].contrast = 1.0f - 0.1f * (float)(k % 4); it[k].color = (float)(k % 3); }
    {
        static const float pal[9] = { 1, 1, 1, 0.5f, 0.25f, 0.125f, 0.2f, 0.9f, 0.4f };
        ygfx_crun_desc rd = cs_rd(set, it, n, 24);
        ygfx_stim one, many[CS_N], fromb;
        ygfx_buf b = ygfx_buffer(&R->g, sizeof(ygfx_citem) * (size_t)n);
        rd.fields = YGFX_I_ORI | YGFX_I_SCALE | YGFX_I_CONTRAST | YGFX_I_COLOR;
        rd.palette = pal; rd.n_palette = 3;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        cs_red(ref);
        for (k = 0; k < n; k++) { rd.items = &it[k]; rd.n = 1; many[k] = ygfx_crun(&R->g, &rd); }
        gl_frame(many, n);
        S8.alone = cs_rdiff(ref);
        R->g.no_reorder = 1;
        gl_frame(many, n);
        S8.alone = maxd(S8.alone, cs_rdiff(ref));
        R->g.no_reorder = 0;
        ygfx_buffer_update(&R->g, b, 0, it, sizeof(ygfx_citem) * (size_t)n);
        rd.items = NULL; rd.buf = b; rd.n = n;
        fromb = ygfx_crun(&R->g, &rd);
        gl_frame(&fromb, 1);
        S8.buffer = cs_rdiff(ref);
        /* the palette: an integer is its entry, bit for bit; a fraction mixes
         * two; contrast scales alpha. A rect glyph's interior over black. */
        {
            ygfx_citem pi[3];
            ygfx_crun_desc pd;
            ygfx_stim ps;
            const uint32_t o = s.words[8];
            const double cx = 0.5 * (ygfx__u2f(s.words[o]) + ygfx__u2f(s.words[o + 2]));
            const double cy = 0.5 * (ygfx__u2f(s.words[o + 1]) + ygfx__u2f(s.words[o + 3]));
            static const float want[3][3] = { { 0.5f, 0.25f, 0.125f }, { 0.35f, 0.575f, 0.2625f }, { 0.5f, 0.5f, 0.5f } };
            memset(pi, 0, sizeof pi);
            for (k = 0; k < 3; k++) {
                pi[k].x = (float)(50 + 100 * k - 48 * cx); pi[k].y = (float)(100 - 48 * cy);
                pi[k].contrast = k == 2 ? 0.5f : 1.0f; pi[k].color = k == 0 ? 1.0f : (k == 1 ? 1.5f : 0.0f);
            }
            pd = cs_rd(set, pi, 3, 48);
            pd.fields = YGFX_I_COLOR | YGFX_I_CONTRAST; pd.palette = pal; pd.n_palette = 3;
            pd.aliased = true;   /* coverage 1 exactly: the color is the palette's */
            ps = ygfx_crun(&R->g, &pd);
            gl_frame(&ps, 1);
            for (k = 0; k < 3; k++) {
                const float* px = R->scene + 4 * (100 * W + 50 + 100 * k);
                int q;
                for (q = 0; q < 3; q++) {
                    double dd = fabs((double)px[q] - want[k][q]);
                    if (k == 0 && dd != 0.0) S8.pal_bad++;
                    if (k > 0) S8.pal_frac = maxd(S8.pal_frac, dd);
                }
            }
            /* two items on one place: the topmost is the later */
            pi[1].x = pi[0].x; pi[1].y = pi[0].y;
            pd.items = pi; pd.n = 2; pd.fields = 0;
            ps = ygfx_crun(&R->g, &pd);
            S8.top_bad = ygfx_hit_index(&R->g, &ps, 50.5f, 100.5f) != 1;
            S8.top_bad += ygfx_hit_index(&R->g, &ps, 250.5f, 100.5f) != -1;
        }
        /* hits: with the set's arrays kept, ygfx_hit() against aliased coverage */
        rd.items = it; rd.buf.id = 0; rd.aliased = true;
        rd.fields = YGFX_I_ORI | YGFX_I_SCALE;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        S8.hit_bad = 0;
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                int gpu = R->scene[4 * (j * W + i)] > 0.5f, cpu = ygfx_hit(&R->g, &one, (float)i + 0.5f, (float)j + 0.5f);
                if (gpu != cpu) S8.hit_bad++;
                S8.hit_n++;
            }
        ygfx_buffer_free(&R->g, b);
    }
    /* a run through a target against the scene: the same values */
    {
        ygfx_target_desc td;
        ygfx_tex t;
        static float tg[W * H * 4];
        ygfx_crun_desc rd = cs_rd(set, it, n, 24);
        ygfx_stim one;
        static const float zero[4] = { 0, 0, 0, 0 };
        rd.fields = YGFX_I_ORI;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        cs_red(ref);
        memset(&td, 0, sizeof td);
        td.w = W; td.h = H;
        t = ygfx_target(&R->g, &td);
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_OK);
        CHECK(ygfx_draw(&R->g, &one) == YGFX_OK);
        CHECK(ygfx_end_target(&R->g) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        CHECK(ygfx_read_target(&R->g, t, 0, 0, W, H, tg) == YGFX_OK);
        S8.target = 0;
        for (i = 0; i < W * H; i++) S8.target = maxd(S8.target, fabs((double)tg[4 * i] - ref[i]));   /* RGBA16F: half precision */
        ygfx_texture_free(&R->g, t);
    }
    /* a glyph added at run time draws as when it was there at make */
    {
        tcs_set s2;
        ygfx_cset_desc d2;
        ygfx_cset set2;
        ygfx_crun_desc rd;
        ygfx_stim one;
        uint32_t gid = 2;
        cs_glyphs();
        tcs_init(&s2, CS_N + 2);
        for (k = 0; k < CS_N; k++) {
            tcs_contour c[4];
            int q;
            if ((uint32_t)k == gid) continue;
            for (q = 0; q < cs_nc[k]; q++) { c[q].pts = cs_pts[k][q]; c[q].n = cs_np[k][q] / 2; }
            tcs_glyph(&s2, (uint32_t)k, c, cs_nc[k], 0, 0, cs_flags[k] | YGFX_CSET_BACKWARD);
        }
        memset(&d2, 0, sizeof d2);
        d2.texels = s2.texels; d2.n_texels = s2.n_texels; d2.words = s2.words; d2.n_words = s2.n_words;
        d2.cap_texels = s2.n_texels + 4096; d2.cap_words = s2.n_words + 65536;
        set2 = ygfx_cset_make(&R->g, &d2);
        CHECK(set2.id != 0);
        {
            tcs_contour c[4];
            int q;
            for (q = 0; q < cs_nc[gid]; q++) { c[q].pts = cs_pts[gid][q]; c[q].n = cs_np[gid][q] / 2; }
            tcs_glyph(&s2, gid, c, cs_nc[gid], 0, 0, cs_flags[gid] | YGFX_CSET_BACKWARD);
        }
        d2.texels = s2.texels; d2.n_texels = s2.n_texels; d2.words = s2.words; d2.n_words = s2.n_words;
        rd = cs_rd(set, it, n, 24);
        rd.fields = YGFX_I_ORI;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        cs_red(ref2);
        rd.set = set2;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        {   /* before the add: every glyph but the absent one */
            double before = cs_rdiff(ref2);
            CHECK(before > 0.1);
        }
        CHECK(ygfx_cset_add(&R->g, set2, &d2, &gid, 1) == YGFX_OK);
        gl_frame(&one, 1);
        S8.added = cs_rdiff(ref2);
        /* past its room */
        d2.n_texels = 0x40000000u;   /* the room is checked before any texel is read */
        cs_refused(12, ygfx_cset_add(&R->g, set2, &d2, &gid, 1) == YGFX_ERR_FULL);
        ygfx_cset_free(&R->g, set2);
        tcs_free(&s2);
    }
    /* refusals: each names its reason */
    {
        ygfx_crun_desc rd = cs_rd(set, it, n, 24);
        ygfx_stim r1;
        ygfx_citem bad[1];
        int rc;
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        rd.set.id = 0; r1 = ygfx_crun(&R->g, &rd);
        cs_refused(1, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "curve set"));
        rd = cs_rd(set, it, n, 0); r1 = ygfx_crun(&R->g, &rd);
        cs_refused(2, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "size"));
        rd = cs_rd(set, it, n, 24); r1 = ygfx_crun(&R->g, &rd); r1.edge = YGFX_EDGE_GAUSSIAN; r1.edge_width = 2;
        cs_refused(3, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "box filter"));
        r1.edge = YGFX_EDGE_HARD; r1.edge_width = 0; r1.stroke = 2;
        cs_refused(4, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "box filter"));
        memset(bad, 0, sizeof bad);
        bad[0].glyph = 1.5f;
        rd = cs_rd(set, bad, 1, 24); r1 = ygfx_crun(&R->g, &rd);
        cs_refused(5, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "glyph"));
        bad[0].glyph = (float)(CS_N + 5);
        cs_refused(6, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "glyph"));
        rd = cs_rd(set, it, n, 24); rd.fields = YGFX_I_COLOR; r1 = ygfx_crun(&R->g, &rd);
        cs_refused(7, ygfx_draw(&R->g, &r1) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "palette"));
        /* inside a frame: no set made, none added */
        cs_refused(8, ygfx_cset_make(&R->g, &d).id == 0);
        rc = ygfx_cset_add(&R->g, set, &d, NULL, 0);
        cs_refused(9, rc == YGFX_ERR_ORDER);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        /* a run as an instances template */
        {
            ygfx_inst el[2];
            ygfx_instances_desc idd;
            ygfx_stim tp;
            rd = cs_rd(set, it, n, 24);
            r1 = ygfx_crun(&R->g, &rd);
            ygfx_inst_grid(el, 2, 1, 10, 10);
            memset(&idd, 0, sizeof idd);
            idd.inst = el; idd.n = 2;
            tp = ygfx_instances(&R->g, &r1, &idd);
            cs_refused(10, tp.n_inst == 0);
        }
        /* a malformed set */
        {
            ygfx_cset_desc d3 = d;
            uint32_t* w2 = (uint32_t*)malloc(4u * d.n_words);
            memcpy(w2, d.words, 4u * d.n_words);
            w2[1] = 7;
            d3.words = w2;
            cs_refused(11, ygfx_cset_make(&R->g, &d3).id == 0 && strstr(ygfx_error(&R->g), "version"));
            free(w2);
        }
    }
    /* the GPU against the CPU form of the rays (cpu_ray: the shader in
     * double), every glyph at rotation 0 */
    {
        ygfx_citem one_it[1];
        ygfx_crun_desc rd;
        ygfx_stim one;
        int g2;
        ygfx_cset_free(&R->g, set);
        ygfx_close(&R->g);
        ygfx__text_v3 = 0;
        if (gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) {
            set = ygfx_cset_make(&R->g, &d);
            for (g2 = 0; g2 < CS_N; g2++) {
                const uint32_t o = s.words[8 + g2];
                double bx0 = ygfx__u2f(s.words[o]), by0 = ygfx__u2f(s.words[o + 1]);
                double bx1 = ygfx__u2f(s.words[o + 2]), by1 = ygfx__u2f(s.words[o + 3]);
                memset(one_it, 0, sizeof one_it);
                one_it[0].glyph = (float)g2;
                one_it[0].x = (float)(100.37 - 24 * bx0); one_it[0].y = (float)(60.21 - 24 * by0);
                rd = cs_rd(set, one_it, 1, 24);
                one = ygfx_crun(&R->g, &rd);
                gl_frame(&one, 1);
                for (j = 58; j < 60 + (int)(24 * (by1 - by0)) + 3; j++)
                    for (i = 98; i < 100 + (int)(24 * (bx1 - bx0)) + 3; i++) {
                        double c = cpu_cov(&s, g2, (i + 0.5 - one_it[0].x) / 24.0, (j + 0.5 - one_it[0].y) / 24.0, 24.0);
                        S8.cpu = maxd(S8.cpu, fabs(c - R->scene[4 * (j * W + i)]));
                    }
            }
        }
    }
    /* .rays (v0.7): where the exact area would apply, a run with it draws
     * what the program without the exact area draws, bit for bit */
    {
        static float ra[W * H], rb[W * H];
        ygfx_citem ri[CS_N];
        ygfx_crun_desc rd;
        ygfx_stim one;
        int nr = cs_layout(&s, 24, 0, 0.21, 0.43, ri, org, &cell);
        ygfx_cset_free(&R->g, set);   /* the CPU form above left the program without the exact area */
        ygfx_close(&R->g);
        ygfx__text_v3 = 1;
        if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) { tcs_free(&s); return; }
        set = ygfx_cset_make(&R->g, &d);
        rd = cs_rd(set, ri, nr, 24);
        rd.rays = true;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        cs_red(ra);
        rd.rays = false;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        cs_red(rb);
        S8.rays_exact = cs_rdiff(ra);
        ygfx_cset_free(&R->g, set);
        ygfx_close(&R->g);
        ygfx__text_v3 = 0;
        S8.rays_same = 1;
        if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) { tcs_free(&s); return; }
        set = ygfx_cset_make(&R->g, &d);
        rd.set = set;
        one = ygfx_crun(&R->g, &rd);
        gl_frame(&one, 1);
        S8.rays_same = cs_rdiff(ra);
        (void)rb;
    }
    /* continuity: one glyph moved 0.005 px at a time. The exact area and the
     * rays on a smooth outline change a pixel by no more than the move can;
     * the rays at a corner jump where a ray passes the corner, which the
     * report gives */
    {
        static const struct { int glyph; double rot; int v3; } C[5] = { { 0, 0, 1 }, { 2, 0, 1 }, { 2, 15, 0 }, { 2, 45, 0 }, { 9, 15, 0 } };
        int ci, step;
        for (ci = 0; ci < 5; ci++) {
            ygfx_citem one_it[1];
            ygfx_crun_desc rd;
            ygfx_stim one;
            double jmax = 0;
            if (C[ci].v3 != ygfx__text_v3) {   /* a program per variant: a new open */
                ygfx_cset_free(&R->g, set);
                ygfx_close(&R->g);
                ygfx__text_v3 = C[ci].v3;
                if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) break;
                set = ygfx_cset_make(&R->g, &d);
            }
            memset(one_it, 0, sizeof one_it);
            one_it[0].glyph = (float)C[ci].glyph; one_it[0].ori = (float)C[ci].rot;
            for (step = 0; step < 100; step++) {
                one_it[0].x = 100.0f + 0.005f * (float)step; one_it[0].y = 150.0f + 0.0031f * (float)step;
                rd = cs_rd(set, one_it, 1, 24);
                rd.fields = YGFX_I_ORI;
                one = ygfx_crun(&R->g, &rd);
                gl_frame(&one, 1);
                if (step) jmax = maxd(jmax, cs_rdiff(ref));
                cs_red(ref);
            }
            if (ci < 2) S8.jump = maxd(S8.jump, jmax);
            else if (ci < 4) S8.jump_ray = maxd(S8.jump_ray, jmax);
            else S8.jump_corner = jmax;
            if (getenv("YGFX_TEST_VERBOSE")) printf("    continuity: glyph %d rotation %.0f exact %d: %.4f\n", C[ci].glyph, C[ci].rot, C[ci].v3, jmax);
        }
        ygfx__text_v3 = 1;
    }
    /* static text cached (v0.7): a run drawn once into a target and
     * composited at 1:1 on whole px, against the run drawn into the scene.
     * The text program works in px with y down, so the target pass computes
     * the scene's coverage bits; the composite is exact (texelFetch, times
     * 1). Over black the same bits. Over gray an RGBA16F target has rounded
     * alpha before the blend: one f16 step; an RGBA32F target has not */
    {
        int bg;
        S8.cached[0] = S8.cached[1] = S8.cached[2] = S8.cached[3] = 1;
        for (bg = 0; bg < 4; bg++) {
            static float direct[W * H * 4];
            ygfx_target_desc td;
            ygfx_image_desc idd;
            ygfx_tex t;
            ygfx_stim one, img;
            ygfx_crun_desc rd;
            static const float zero[4] = { 0, 0, 0, 0 };
            ygfx_cset_free(&R->g, set);
            ygfx_close(&R->g);
            if (!gl_open(bg == 1 || bg == 3, bg == 2 ? YGFX_RGBA32F : YGFX_RGBA16F, YGFX_DITHER_NONE)) { tcs_free(&s); return; }
            set = ygfx_cset_make(&R->g, &d);
            n = cs_layout(&s, 12, 0, 0, 0, it, org, &cell);
            rd = cs_rd(set, it, n, 12);
            one = ygfx_crun(&R->g, &rd);
            gl_frame(&one, 1);
            memcpy(direct, R->scene, sizeof direct);
            memset(&td, 0, sizeof td);
            td.w = W; td.h = H; td.format = bg >= 2 ? YGFX_RGBA32F : YGFX_RGBA16F;
            t = ygfx_target(&R->g, &td);
            CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
            CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
            CHECK(ygfx_begin_target(&R->g, t, zero) == YGFX_OK);
            CHECK(ygfx_draw(&R->g, &one) == YGFX_OK);
            CHECK(ygfx_end_target(&R->g) == YGFX_OK);
            CHECK(ygfx_end(&R->g) == YGFX_OK);
            CHECK(yscr_flip(&R->scr) == YSCR_OK);
            memset(&idd, 0, sizeof idd);
            idd.tex = t;
            img = ygfx_image(&R->g, &idd);
            gl_frame(&img, 1);
            S8.cached[bg] = 0;
            for (i = 0; i < W * H * 4; i++)   /* rgb: the scene's alpha is never read */
                if ((i & 3) != 3) S8.cached[bg] = maxd(S8.cached[bg], fabs((double)R->scene[i] - direct[i]));
            ygfx_texture_free(&R->g, t);
        }
    }
    ygfx_cset_free(&R->g, set);
    ygfx_close(&R->g);
    tcs_free(&s);
}

/* ------------------------------------------------- v0.6 blur (GL) */


/* The rect [x0, x1] x [y0, y1] blurred by a Gaussian of SD s, at a point:
 * a product of erf differences. */
static double bl_rect(double x, double y, double x0, double x1, double y0, double y1, double s) {
    double k = 1.0 / (s * sqrt(2.0));
    return 0.25 * (erf((x1 - x) * k) - erf((x0 - x) * k)) * (erf((y1 - y) * k) - erf((y0 - y) * k));
}

/* ------------------------------------------------- v0.8 (GL) */

/* Setup passes and the automatic rays program. */
typedef struct stats10 {
    int    ran;
    double setup_target, setup_comp, next_scene, next_out, blur_same;
    long   frames_moved, refused_ok, refused_n;
    double auto_diff[5];   /* auto against the exact-area program, bit for bit */
    long   auto_pick_bad;  /* runs on the program the rule does not name */
} stats10;
static stats10 S10;

static void s10_refuse(int ok) { S10.refused_n++; S10.refused_ok += ok != 0; }

static void gl_v08(stats* st) {
    static float ta[W * H * 4], tb[W * H * 4], sa[W * H * 4], oa8[W * H * 4];
    static uint8_t oa[W * H * 4];
    tcs_set s;
    ygfx_cset_desc d;
    ygfx_cset set;
    ygfx_citem it[CS_N];
    double org[CS_N][2], cell;
    ygfx_target_desc td;
    ygfx_tex t1, t2;
    ygfx_crun_desc rd;
    ygfx_stim page, img, gab;
    ygfx_gabor_desc gd;
    ygfx_image_desc id;
    static const float zero[4] = { 0, 0, 0, 0 };
    int n, i, k;
    uint64_t frames0;
    (void)st; (void)oa8;
    memset(&S10, 0, sizeof S10);
    S10.ran = 1;
    cs_build(&s, 0, YGFX_CSET_BACKWARD);
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) { tcs_free(&s); return; }
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    set = ygfx_cset_make(&R->g, &d);
    n = cs_layout(&s, 12, 0, 0, 0, it, org, &cell);
    rd = cs_rd(set, it, n, 12);
    page = ygfx_crun(&R->g, &rd);
    memset(&td, 0, sizeof td);
    td.w = W; td.h = H; td.format = YGFX_RGBA32F;
    t1 = ygfx_target(&R->g, &td);
    t2 = ygfx_target(&R->g, &td);
    memset(&id, 0, sizeof id);
    id.tex = t1;
    img = ygfx_image(&R->g, &id);
    memset(&gd, 0, sizeof gd);
    gd.x = 20; gd.sigma = 12; gd.sf = 1 / 9.0f; gd.contrast = 0.4f; gd.ori = 30;
    gab = ygfx_gabor(&gd);

    /* a frame of reference: the gabor and the run in the scene */
    {
        ygfx_stim two[2];
        two[0] = gab; two[1] = page;
        gl_frame(two, 2);
        memcpy(sa, R->scene, sizeof sa);
        memcpy(oa, R->out, sizeof oa);
    }
    frames0 = R->g.frames;
    /* the page at setup into t1 */
    CHECK(ygfx_begin_setup(&R->g) == YGFX_OK);
    CHECK(ygfx_begin_target(&R->g, t1, zero) == YGFX_OK);
    CHECK(ygfx_draw(&R->g, &page) == YGFX_OK);
    CHECK(ygfx_end_target(&R->g) == YGFX_OK);
    CHECK(ygfx_end_setup(&R->g) == YGFX_OK);
    S10.frames_moved = (long)(R->g.frames - frames0);
    /* the next frame: as if no setup pass had run */
    {
        ygfx_stim two[2];
        two[0] = gab; two[1] = page;
        gl_frame(two, 2);
        S10.next_scene = 0; S10.next_out = 0;
        for (i = 0; i < W * H * 4; i++) S10.next_scene = maxd(S10.next_scene, fabs((double)R->scene[i] - sa[i]));
        S10.next_out = memcmp(R->out, oa, sizeof oa) != 0;
    }
    /* the same page in a frame into t2: the targets bit for bit, and each
     * composited 1:1 */
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
    CHECK(ygfx_begin_target(&R->g, t2, zero) == YGFX_OK);
    CHECK(ygfx_draw(&R->g, &page) == YGFX_OK);
    CHECK(ygfx_end_target(&R->g) == YGFX_OK);
    CHECK(ygfx_end(&R->g) == YGFX_OK);
    CHECK(yscr_flip(&R->scr) == YSCR_OK);
    CHECK(ygfx_read_target(&R->g, t1, 0, 0, W, H, ta) == YGFX_OK);
    CHECK(ygfx_read_target(&R->g, t2, 0, 0, W, H, tb) == YGFX_OK);
    S10.setup_target = memcmp(ta, tb, sizeof ta) != 0;
    gl_frame(&img, 1);
    memcpy(sa, R->scene, sizeof sa);
    img.tex = t2;
    gl_frame(&img, 1);
    S10.setup_comp = memcmp(sa, R->scene, sizeof sa) != 0;
    img.tex = t1;
    /* a blur applied at setup against one applied in a frame */
    {
        ygfx_blur b1, b2;
        ygfx_blur_desc bd;
        static float r1[W * H * 4], r2[W * H * 4];
        memset(&bd, 0, sizeof bd);
        bd.w = 200; bd.h = 120; bd.format = YGFX_RGBA16F; bd.sigma = 2.5f;
        CHECK(ygfx_blur_make(&R->g, &b1, &bd) == YGFX_OK);
        CHECK(ygfx_blur_make(&R->g, &b2, &bd) == YGFX_OK);
        rd.place = YGFX_CENTER; rd.anchor = YGFX_CENTER; rd.w = 0; rd.h = 0; rd.n = 3;
        {
            ygfx_stim word = ygfx_crun(&R->g, &rd);
            frames0 = R->g.frames;
            CHECK(ygfx_begin_setup(&R->g) == YGFX_OK);
            CHECK(ygfx_begin_target(&R->g, b1.layer, zero) == YGFX_OK);
            CHECK(ygfx_draw(&R->g, &word) == YGFX_OK);
            CHECK(ygfx_end_target(&R->g) == YGFX_OK);
            CHECK(ygfx_blur_apply(&R->g, &b1) == YGFX_OK);
            CHECK(ygfx_end_setup(&R->g) == YGFX_OK);
            S10.frames_moved += (long)(R->g.frames - frames0);
            CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
            CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
            CHECK(ygfx_begin_target(&R->g, b2.layer, zero) == YGFX_OK);
            CHECK(ygfx_draw(&R->g, &word) == YGFX_OK);
            CHECK(ygfx_end_target(&R->g) == YGFX_OK);
            CHECK(ygfx_blur_apply(&R->g, &b2) == YGFX_OK);
            CHECK(ygfx_end(&R->g) == YGFX_OK);
            CHECK(yscr_flip(&R->scr) == YSCR_OK);
        }
        CHECK(ygfx_read_target(&R->g, b1.result, 0, 0, 200, 120, r1) == YGFX_OK);
        CHECK(ygfx_read_target(&R->g, b2.result, 0, 0, 200, 120, r2) == YGFX_OK);
        S10.blur_same = memcmp(r1, r2, sizeof(float) * 4 * 200 * 120) != 0;
        ygfx_blur_free(&R->g, &b1);
        ygfx_blur_free(&R->g, &b2);
        rd = cs_rd(set, it, n, 12);
    }
    /* refusals */
    CHECK(ygfx_begin_setup(&R->g) == YGFX_OK);
    s10_refuse(ygfx_draw(&R->g, &page) == YGFX_ERR_ORDER);           /* the scene is a frame's */
    s10_refuse(ygfx_end(&R->g) == YGFX_ERR_ORDER);                   /* a setup pass ends with end_setup */
    s10_refuse(ygfx_begin(&R->g, &R->f) == YGFX_ERR_ORDER);          /* no frame inside it */
    s10_refuse(ygfx_begin_setup(&R->g) == YGFX_ERR_ORDER);           /* nor a second setup pass */
    CHECK(ygfx_begin_target(&R->g, t1, NULL) == YGFX_OK);
    s10_refuse(ygfx_begin_target(&R->g, t2, NULL) == YGFX_ERR_ORDER); /* not nested */
    CHECK(ygfx_end_target(&R->g) == YGFX_OK);
    for (k = 1; k < YGFX_MAX_PASSES; k++) { CHECK(ygfx_begin_target(&R->g, t2, NULL) == YGFX_OK); CHECK(ygfx_end_target(&R->g) == YGFX_OK); }
    s10_refuse(ygfx_begin_target(&R->g, t2, NULL) == YGFX_ERR_ORDER); /* the 16 passes */
    CHECK(ygfx_end_setup(&R->g) == YGFX_OK);
    s10_refuse(ygfx_end_setup(&R->g) == YGFX_ERR_ORDER);             /* no setup pass open */
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
    s10_refuse(ygfx_begin_setup(&R->g) == YGFX_ERR_ORDER);           /* not inside a frame */
    s10_refuse(ygfx_end_setup(&R->g) == YGFX_ERR_ORDER);
    CHECK(ygfx_end(&R->g) == YGFX_OK);
    CHECK(yscr_flip(&R->scr) == YSCR_OK);

    /* the rays program automatically: every case drawn by the rule and by
     * the exact-area program (the seam), bit for bit; the program each took */
    {
        static float ref[W * H];
        ygfx_buf b = ygfx_buffer(&R->g, sizeof(ygfx_citem) * CS_N);
        int c;
        for (c = 0; c < 5; c++) {
            ygfx_stim run;
            int want_rays, a;
            n = cs_layout(&s, 24, c == 0 ? 15 : 0, 0.21, 0.43, it, org, &cell);
            rd = cs_rd(set, it, n, 24);
            if (c == 0) { rd.ori = 15; want_rays = 1; }                                   /* the run turned */
            else if (c == 1) { for (k = 0; k < n; k++) it[k].ori = 7.0f + 11.0f * (float)k; rd.fields = YGFX_I_ORI; want_rays = 1; }
            else if (c == 2) {                                                             /* one item on a quarter turn */
                for (k = 0; k < n; k++) it[k].ori = k == 1 ? 90.0f : 7.0f + 11.0f * (float)k;
                rd.fields = YGFX_I_ORI; want_rays = 0;
            } else if (c == 3) { rd.aliased = true; want_rays = 1; }                      /* aliased, not turned */
            else {                                                                         /* turned items in a buffer */
                for (k = 0; k < n; k++) it[k].ori = 7.0f + 11.0f * (float)k;
                ygfx_buffer_update(&R->g, b, 0, it, sizeof(ygfx_citem) * (size_t)n);
                rd.items = NULL; rd.buf = b; rd.fields = YGFX_I_ORI; want_rays = 0;
            }
            run = ygfx_crun(&R->g, &rd);
            for (a = 1; a >= 0; a--) {
                ygfx__text_auto = a;
                gl_frame(&run, 1);
                if (a) {
                    cs_red(ref);
                    S10.auto_pick_bad += (R->g.cmds[0].pipe == R->g.text_rays_pipe) != want_rays;
                } else {
                    S10.auto_diff[c] = cs_rdiff(ref);
                    S10.auto_pick_bad += R->g.cmds[0].pipe != R->g.text_pipe;
                }
            }
            ygfx__text_auto = 1;
            for (k = 0; k < n; k++) it[k].ori = 0;
        }
        ygfx_buffer_free(&R->g, b);
    }
    ygfx_texture_free(&R->g, t1);
    ygfx_texture_free(&R->g, t2);
    ygfx_cset_free(&R->g, set);
    ygfx_close(&R->g);
    tcs_free(&s);
}

static void s10_report(void) {
    if (!S10.ran) return;
    printf("  v0.8 setup passes: a page at setup against in a frame: target %s, composite %s; the next frame against one "
           "without a setup pass: scene %.2e, output %s; a blur at setup against in a frame: %s; frames counted by setup "
           "passes %ld; refusals %ld of %ld\n",
           S10.setup_target != 0.0 ? "differs" : "equal", S10.setup_comp != 0.0 ? "differs" : "equal", S10.next_scene,
           S10.next_out != 0.0 ? "differs" : "equal", S10.blur_same != 0.0 ? "differs" : "equal", S10.frames_moved, S10.refused_ok, S10.refused_n);
    printf("  v0.8 automatic rays: against the exact-area program, run turned %.2e, items turned %.2e, one item on a quarter "
           "turn %.2e, aliased %.2e, buffer %.2e; wrong program %ld\n", S10.auto_diff[0], S10.auto_diff[1], S10.auto_diff[2],
           S10.auto_diff[3], S10.auto_diff[4], S10.auto_pick_bad);
    CHECK(S10.setup_target == 0);
    CHECK(S10.setup_comp == 0);
    CHECK(S10.next_scene == 0.0);
    CHECK(S10.next_out == 0);
    CHECK(S10.blur_same == 0);
    CHECK(S10.frames_moved == 0);
    CHECK(S10.refused_ok == S10.refused_n && S10.refused_n == 9);
    CHECK(S10.auto_diff[0] == 0.0 && S10.auto_diff[1] == 0.0 && S10.auto_diff[2] == 0.0 && S10.auto_diff[3] == 0.0 &&
          S10.auto_diff[4] == 0.0);
    CHECK(S10.auto_pick_bad == 0);
    S10.ran = 0;
}

/* ------------------------------------------------- v0.10 SIMPLEX */

/* The integer simplex in double from the same Q12 lattice point: the same
 * lattice, keys and gradients, none of the integer rounding. Its distance
 * from ygfx_simplex_value() is the integer form's quantization. */
static double gn_ref(double x, double y, double z, uint32_t S) {
    static const int G[16][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 },
                                  { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 }, { 1, 1, 0 }, { -1, 1, 0 },
                                  { 0, -1, 1 }, { 0, -1, -1 } };
    double v[3], sk, x0[3], sum = 0;
    int64_t c[3];
    int e[3], i1[3], i2[3], j, k;
    v[0] = x; v[1] = y; v[2] = z;
    sk = (x + y + z) / 3.0;
    for (j = 0; j < 3; j++) c[j] = (int64_t)floor(v[j] + sk);
    for (j = 0; j < 3; j++) x0[j] = v[j] - (double)c[j] + (double)(c[0] + c[1] + c[2]) / 6.0;
    for (j = 0; j < 3; j++) e[j] = x0[j] >= x0[(j + 1) % 3];
    for (j = 0; j < 3; j++) { i1[j] = e[j] * (1 - e[(j + 2) % 3]); i2[j] = 1 - e[(j + 2) % 3] * (1 - e[j]); }
    for (k = 0; k < 4; k++) {
        int o[3];
        double d[3], t;
        for (j = 0; j < 3; j++) {
            o[j] = k == 0 ? 0 : (k == 1 ? i1[j] : (k == 2 ? i2[j] : 1));
            d[j] = x0[j] - o[j] + k / 6.0;
        }
        t = 0.6 - (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (t > 0) {
            uint32_t cz = (uint32_t)(c[2] + o[2]), kk = ygfx_hash(cz ^ S), m = ygfx_hash(kk ^ 0x9E3779B9u);
            uint32_t h = ygfx_hash(((uint32_t)(c[0] + o[0]) + ((uint32_t)(c[1] + o[1]) << 16)) * (kk | 1u) ^ m);
            const int* g = G[h >> 28];
            t *= t;
            sum += t * t * (g[0] * d[0] + g[1] * d[1] + g[2] * d[2]);
        }
    }
    return 32.0 * sum;
}

typedef struct stats11 {
    int    ran;
    long   bad, n, refused_ok, refused_n;
    double quant, turned_max, turned_mean, sd[3];
} stats11;
static stats11 S11;

/* CPU: the integer form against double, one octave, at every lattice point
 * of a 512 x 512 field (Q12 inputs as the GPU makes them) */
static void test_v10_simplex_cpu(void) {
    double worst = 0;
    uint32_t S = ygfx_hash(7u);
    int i, j;
    for (j = 0; j < 512; j++)
        for (i = 0; i < 512; i++) {
            uint32_t r = 16384u / 23u, vx = ((2u * (uint32_t)i + 1u) * r) >> 1, vy = ((2u * (uint32_t)j + 1u) * r) >> 1, vz = 22222u;
            int32_t a = ygfx__gn_i(vx + 0x10000000u, vy + 0x10000000u, vz + 0x10000000u, S);
            double want = gn_ref((vx + 0x10000000u) / 16384.0, (vy + 0x10000000u) / 16384.0, (vz + 0x10000000u) / 16384.0, S);
            worst = maxd(worst, fabs(a * (32.0 / 1073741824.0) - want));
        }
    S11.quant = worst;
    printf("v0.10 simplex: the integer form against double on the same lattice points, one octave: max %.2e\n", worst);
    CHECK_LE(worst, 2e-3);
    {   /* the SD by octaves the manual states (1024 x 1024, 4 seeds there;
         * 512 x 512, one seed here: within 0.02) */
        static ygfx_gfx g0;
        static const int oc[3] = { 1, 4, 8 };
        static const double sd_doc[3] = { 0.425, 0.261, 0.246 };
        int q;
        g0.ppu = 1.0f;
        for (q = 0; q < 3; q++) {
            ygfx_noise_desc nd;
            ygfx_stim st;
            double sum = 0, sq = 0;
            memset(&nd, 0, sizeof nd);
            nd.w = 512; nd.h = 512; nd.dist = YGFX_SIMPLEX; nd.scale = 32; nd.octaves = oc[q]; nd.seed = 3;
            st = ygfx_noise(&nd);
            for (j = 0; j < 512; j++)
                for (i = 0; i < 512; i++) { double v = ygfx_simplex_value(&g0, &st, i, j); sum += v; sq += v * v; }
            sum /= 512.0 * 512.0;
            S11.sd[q] = sqrt(sq / (512.0 * 512.0) - sum * sum);
            CHECK_LE(fabs(S11.sd[q] - sd_doc[q]), 0.02);
        }
        printf("v0.10 simplex: SD of 1, 4, 8 octaves %.3f %.3f %.3f (manual 0.425 0.261 0.246)\n", S11.sd[0], S11.sd[1], S11.sd[2]);
    }
}

static void gl_v10_simplex(stats* st) {
    static const struct { float scale, z, lac, gain; int oct; uint32_t seed; float x, y, w, h; } C[6] = {
        { 16, 0.0f, 2.0f, 0.5f, 1, 7, 20, 10, 200, 150 },
        { 40, 3.37f, 2.0f, 0.5f, 4, 9, 7, 3, 300, 190 },
        { 23, 1.5f, 1.7f, 0.8f, 8, 12345, 0, 0, 320, 200 },
        { 5, -2.25f, 2.0f, 0.6f, 3, 1, 50, 40, 120, 100 },
        { 200, 0.75f, 3.0f, 0.4f, 2, 77, 10, 10, 300, 180 },
        { 2, 0.0f, 1.0f, 1.0f, 1, 3, 30, 30, 100, 100 },
    };
    ygfx_noise_desc nd;
    ygfx_stim s;
    int c, i, j;
    (void)st;
    memset(&S11, 0, sizeof S11);
    S11.ran = 1;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (c = 0; c < 6; c++) {
        memset(&nd, 0, sizeof nd);
        nd.place = YGFX_TOP_LEFT; nd.anchor = YGFX_TOP_LEFT;
        nd.x = C[c].x; nd.y = C[c].y; nd.w = C[c].w; nd.h = C[c].h;
        nd.dist = YGFX_SIMPLEX; nd.scale = C[c].scale; nd.z = C[c].z; nd.octaves = C[c].oct;
        nd.lacunarity = C[c].lac; nd.gain = C[c].gain; nd.seed = C[c].seed;
        nd.aperture = YGFX_NO_APERTURE; nd.dir[0] = nd.dir[1] = nd.dir[2] = 1;
        s = ygfx_noise(&nd);
        gl_frame(&s, 1);
        for (j = 0; j < (int)C[c].h; j++)
            for (i = 0; i < (int)C[c].w; i++) {
                int px = (int)C[c].x + i, py = (int)C[c].y + j;
                float got, want = ygfx_simplex_value(&R->g, &s, i, j);
                if (px >= W || py >= H) continue;
                got = R->scene[(py * W + px) * 4];
                S11.n++;
                if (got != want) {
                    S11.bad++;
                    if (S11.bad < 5 && getenv("YGFX_TEST_VERBOSE")) printf("  simplex %d at %d %d: got %.9g want %.9g\n", c, i, j, got, want);
                }
            }
    }
    {   /* turned: the same field, its values in range, its mean near 0 */
        double sum = 0;
        long n = 0;
        memset(&nd, 0, sizeof nd);
        nd.w = 280; nd.h = 180; nd.ori = 30; nd.dist = YGFX_SIMPLEX; nd.scale = 12; nd.octaves = 3; nd.seed = 5;
        nd.aperture = YGFX_NO_APERTURE; nd.dir[0] = nd.dir[1] = nd.dir[2] = 1;
        s = ygfx_noise(&nd);
        gl_frame(&s, 1);
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                double v = R->scene[(j * W + i) * 4];
                S11.turned_max = maxd(S11.turned_max, fabs(v));
                sum += v; n++;
            }
        S11.turned_mean = fabs(sum / n);
    }
    {   /* refusals */
        ygfx_stim b[6];
        memset(&nd, 0, sizeof nd);
        nd.w = 50; nd.dist = YGFX_SIMPLEX; nd.scale = 10; nd.dir[0] = 1;
        for (i = 0; i < 6; i++) b[i] = ygfx_noise(&nd);
        b[0].gn_scale = 0;
        b[1].gn_octaves = 9;
        b[2].gn_lacunarity = 9;
        b[3].gn_gain = 1.5f;
        b[4].shape = YGFX_POLYGON; b[4].shape_p[0] = 3;   /* refused before its vertices are read */
        b[5].gn_scale = 0.001f;   /* a cell below 1/32 px */
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        for (i = 0; i < 6; i++) { S11.refused_n++; S11.refused_ok += ygfx_draw(&R->g, &b[i]) == YGFX_ERR_ARG; }
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    ygfx_close(&R->g);
}

/* v0.10.4: a user shader samples stim.tex as ysp_tex0, an uploaded texture
 * or a render target (docs/rig_spec.md 15.6). Before v0.10.4 a USER draw
 * bound no texture, so ysp_tex0 read whatever the previous draw left on
 * unit 0: the first frame below puts another texture there. */
static void gl_v10_user_tex(stats* st) {
    static const char* body =
        "float ysp_main(vec2 p) {\n"
        "    ivec2 t = ivec2(floor(p + ysp_size.xy));\n"
        "    return texelFetch(ysp_tex0, t, 0).r;\n"
        "}\n";
    static float a1[W * H * 4];
    static const float clear[4] = { 0.3f, 0.6f, 0.9f, 1.0f };
    unsigned char texa[16 * 16], texb[16 * 16];
    ygfx_pipeline_desc pd;
    ygfx_user_desc ud;
    ygfx_image_desc idd;
    ygfx_texture_desc tdd;
    ygfx_target_desc tgd;
    ygfx_stim u, img, other, bad;
    ygfx_tex ta, tb, tg;
    double d_img = 0, d_tg = 0, want;
    int i, j;
    (void)st;
    if (!gl_open(1, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    for (i = 0; i < 16 * 16; i++) { texa[i] = (unsigned char)((i * 37 + 11) & 255); texb[i] = (unsigned char)(255 - texa[i]); }
    memset(&tdd, 0, sizeof tdd);
    tdd.w = 16; tdd.h = 16; tdd.format = YGFX_R8;
    tdd.data = texa;
    ta = ygfx_texture(&R->g, &tdd);
    tdd.data = texb;
    tb = ygfx_texture(&R->g, &tdd);
    memset(&tgd, 0, sizeof tgd);
    tgd.w = 16; tgd.h = 16;
    tg = ygfx_target(&R->g, &tgd);
    CHECK(ta.id != 0 && tb.id != 0 && tg.id != 0);
    memset(&pd, 0, sizeof pd);
    pd.body = body; pd.mode = YGFX_MODULATION; pd.name = "tex0";
    memset(&ud, 0, sizeof ud);
    ud.pipe = ygfx_pipeline(&R->g, &pd);
    CHECK(ud.pipe.id != 0);
    ud.w = ud.h = 16; ud.contrast = 0.5f; ud.tex = ta;
    u = ygfx_user(&ud);
    CHECK(u.tex.id == ta.id);
    /* the reference: the same texture as a modulation image */
    memset(&idd, 0, sizeof idd);
    idd.tex = ta; idd.modulation = true; idd.contrast = 0.5f;
    img = ygfx_image(&R->g, &idd);
    gl_frame(&img, 1);
    memcpy(a1, R->scene, sizeof a1);
    idd.tex = tb; idd.x = 60;
    other = ygfx_image(&R->g, &idd);
    gl_frame(&other, 1);                  /* unit 0 now holds tb */
    gl_frame(&u, 1);
    for (i = 0; i < W * H * 4; i++) d_img = maxd(d_img, fabs(a1[i] - R->scene[i]));
    CHECK_LE(d_img, 1e-6);
    /* a target, filled at setup, as ysp_tex0 */
    CHECK(ygfx_begin_setup(&R->g) == YGFX_OK);
    CHECK(ygfx_begin_target(&R->g, tg, clear) == YGFX_OK);
    CHECK(ygfx_end_target(&R->g) == YGFX_OK);
    CHECK(ygfx_end_setup(&R->g) == YGFX_OK);
    u.tex = tg;
    gl_frame(&u, 1);
    /* the texel as stored: 0.3 as a half float rounds down on D3D11 and to
     * the nearest on SwiftShader (0.29980 against 0.30005) */
    {
        float texel[4] = { 0, 0, 0, 0 };
        CHECK(ygfx_read_target(&R->g, tg, 8, 8, 1, 1, texel) == YGFX_OK);
        CHECK(fabs(texel[0] - 0.3) < 2.5e-4);
        want = 0.5 * (1.0 + 0.5 * texel[0]);
    }
    for (j = H / 2 - 8; j < H / 2 + 8; j++)
        for (i = W / 2 - 8; i < W / 2 + 8; i++) d_tg = maxd(d_tg, fabs(R->scene[(j * W + i) * 4] - want));
    CHECK_LE(d_tg, 1e-6);
    /* refused: a draw into the target it samples, and a texture of no gfx */
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
    CHECK(ygfx_begin_target(&R->g, tg, NULL) == YGFX_OK);
    CHECK(ygfx_draw(&R->g, &u) == YGFX_ERR_ORDER);
    CHECK(ygfx_end_target(&R->g) == YGFX_OK);
    bad = u; bad.tex.id = YGFX_MAX_TEXTURES + 1;
    CHECK(ygfx_draw(&R->g, &bad) == YGFX_ERR_ARG);
    CHECK(ygfx_end(&R->g) == YGFX_OK);
    CHECK(yscr_flip(&R->scr) == YSCR_OK);
    printf("  measured: user ysp_tex0 against the image %.2e, against the target %.2e\n", d_img, d_tg);
    ygfx_close(&R->g);
}

/* v0.11 PRIME: every program drawn once as a zero-size quad; the frames
 * before and after are bit-identical, and it is refused inside a frame. */
static void gl_v11_prime(stats* st) {
    static float a_scene[W * H * 4];
    static unsigned char a_out[W * H * 4];
    static const char* body = "float ysp_main(vec2 p) { return sin(p.x * 0.2); }\n";
    ygfx_shape_desc sd;
    ygfx_gabor_desc gd;
    ygfx_pipeline_desc pd;
    ygfx_user_desc ud;
    ygfx_instances_desc id;
    ygfx_inst el[4];
    ygfx_stim s[4];
    long scene_bad = 0, out_bad = 0;
    int i, n;
    (void)st;
    if (!gl_open(1, YGFX_RGBA16F, YGFX_DITHER_NONE)) return;
    CHECK(ygfx_prime(NULL) == YGFX_ERR_CLOSED);
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_CIRCLE; sd.w = 20; sd.color[0] = 0.8f; sd.x = -30;
    s[0] = ygfx_shape(&sd);
    memset(&gd, 0, sizeof gd);
    gd.sigma = 4; gd.sf = 0.125f; gd.contrast = 0.3f; gd.x = 20;
    s[1] = ygfx_gabor(&gd);
    memset(&pd, 0, sizeof pd);
    pd.body = body; pd.mode = YGFX_MODULATION; pd.name = "prime";
    memset(&ud, 0, sizeof ud);
    ud.pipe = ygfx_pipeline(&R->g, &pd);
    CHECK(ud.pipe.id != 0);
    ud.w = ud.h = 16; ud.contrast = 0.4f; ud.y = 20;
    s[2] = ygfx_user(&ud);
    ygfx_inst_grid(el, 2, 2, 10, 10);
    memset(&id, 0, sizeof id);
    id.inst = el; id.n = 4; id.fields = YGFX_I_XY;
    sd.x = 0; sd.y = -20; sd.w = 6;
    s[3] = ygfx_shape(&sd);
    s[3] = ygfx_instances(&R->g, &s[3], &id);
    CHECK(s[3].n_inst == 4);
    gl_frame(s, 4);
    memcpy(a_scene, R->scene, sizeof a_scene);
    memcpy(a_out, R->out, sizeof a_out);
    n = ygfx_prime(&R->g);
    /* 10 built-in, 3 specialized, 1 instanced, 1 user, the output stage */
    CHECK(n == YGFX__N_BUILTIN + YGFX__N_VSPEC + 1 + 1 + 1);
    gl_frame(s, 4);
    for (i = 0; i < W * H * 4; i++) {
        scene_bad += memcmp(&a_scene[i], &R->scene[i], sizeof(float)) != 0;
        out_bad += a_out[i] != R->out[i];
    }
    CHECK(scene_bad == 0 && out_bad == 0);
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
    CHECK(ygfx_prime(&R->g) == YGFX_ERR_ORDER);
    CHECK(ygfx_end(&R->g) == YGFX_OK);
    CHECK(yscr_flip(&R->scr) == YSCR_OK);
    printf("  measured: prime drew %d programs; frames before and after: %ld scene and %ld output values differ\n",
           n, scene_bad, out_bad);
    ygfx_close(&R->g);
}

/* v0.11.1 (OPEN START): the start and the finish draw what a blocking open
 * draws, bit for bit, with the parallel compile and without; a screen
 * opened with desc.on_context ends its open with the default framebuffer
 * bound, whatever the hook bound. */
typedef struct v111_hook { ygfx_desc d; int ok, calls; } v111_hook;
static void v111_start(void* ctx, yscr_screen* s) {
    v111_hook* h = (v111_hook*)ctx;
    h->calls++;
    h->d.screen = s;
    h->ok = ygfx_open_start(&R->g, &h->d);
}
static void gl_v11_start(stats* st) {
    static float a_scene[W * H * 4];
    static unsigned char a_out[W * H * 4];
    static v111_hook hk;
    ygfx_shape_desc sd;
    ygfx_gabor_desc gb;
    ygfx_stim s[2];
    ygfx_desc d;
    yscr_desc sdesc;
    char line[700];
    long bad[3] = { 0, 0, 0 };
    int k, i, fb = -1, parallel = 0;
    (void)st;
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_CIRCLE; sd.w = 20; sd.color[0] = 0.8f; sd.x = -30;
    s[0] = ygfx_shape(&sd);
    memset(&gb, 0, sizeof gb);
    gb.sigma = 4; gb.sf = 0.125f; gb.contrast = 0.3f; gb.x = 20;
    s[1] = ygfx_gabor(&gb);
    if (!gl_open(1, YGFX_RGBA16F, YGFX_DITHER_NONE)) return;
    gl_frame(s, 2);
    memcpy(a_scene, R->scene, sizeof a_scene);
    memcpy(a_out, R->out, sizeof a_out);
    ygfx_close(&R->g);
    memset(&d, 0, sizeof d);
    d.screen = &R->scr;
    d.background[0] = d.background[1] = d.background[2] = 0.5f;
    d.seed = 99;
    d.cache = test_cache();
    for (k = 0; k < 3; k++) {
        if (k < 2) {   /* the start on the open screen; then the seam: no parallel compile */
            ygfx__test_serial = k;
            CHECK(ygfx_open_start(&R->g, &d));
            CHECK(!ygfx_is_open(&R->g));
            CHECK(ygfx_open(&R->g, NULL));
            ygfx__test_serial = 0;
        } else {       /* the screen again, with the start in its hook */
            yscr_close(&R->scr);
            memset(&sdesc, 0, sizeof sdesc);
            sdesc.backend = YSCR_BACKEND_CUSTOM;
            sdesc.presenter = &ygfx_headless_presenter;
            sdesc.presenter_ctx = &R->hl;
            sdesc.sim_period_ns = 1000000;
            sdesc.on_context = v111_start;
            sdesc.on_context_ctx = &hk;
            hk.d = d;
            hk.calls = 0;
            if (!yscr_open(&R->scr, &sdesc)) {
                fprintf(stderr, "gfx_test [%s]: reopen: %s\n", g_where, yscr_error(&R->scr));
                g_failures++;
                return;
            }
            CHECK(hk.calls == 1 && hk.ok);
            {
                typedef void (YGFX__APIENTRY * geti_fn)(unsigned, int*);
                geti_fn geti = (geti_fn)yscr_gl_proc(&R->scr, "glGetIntegerv");
                if (geti) geti(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fb);
                CHECK(fb == 0);
            }
            CHECK(ygfx_open(&R->g, NULL));
        }
        ygfx_describe(&R->g, line, sizeof line);
        if (k == 0) parallel = strstr(line, ", open_start=parallel") != NULL;
        CHECK(strstr(line, k == 1 ? ", open_start=serial(no GL_KHR_parallel_shader_compile)"
                                  : parallel ? ", open_start=parallel" : ", open_start=serial(") != NULL);
        gl_frame(s, 2);
        for (i = 0; i < W * H * 4; i++)
            bad[k] += memcmp(&a_scene[i], &R->scene[i], sizeof(float)) != 0 || a_out[i] != R->out[i];
        CHECK(bad[k] == 0);
        ygfx_close(&R->g);
    }
    printf("  measured: open_start %s; values that differ from a blocking open: %ld, serial %ld, in the hook %ld; "
           "framebuffer after the hook's open %d\n", parallel ? "parallel" : "serial (no KHR_parallel_shader_compile)",
           bad[0], bad[1], bad[2], fb);
}

/* v0.15 (FIRST TRIAL): ygfx_open_screen() opens the screen and the gfx
 * and primes; it refuses a hook that is taken. ygfx_show_until_key()
 * draws until max_ns without input (no SDL here: no key can come).
 * ygfx_text_make() with the system's font: the items, the lines, the
 * wrap, ink inside its bounds and none outside, the text program primed
 * by the first text only; a font that does not open is named. The part
 * reopens R->scr and leaves it open for gl_suite. */
static yscr_record v15_recs[64];
static int v15_n;
static void v15_rec(void* ctx, const yscr_record* r, const yscr_trigger_result* t, int n) {
    (void)ctx; (void)t; (void)n;
    if (v15_n < 64) v15_recs[v15_n++] = *r;
}

static void gl_v15_first(stats* st) {
    static ygfx_text t, t2, t3;
    yscr_desc sd;
    ygfx_desc d;
    ygfx_shape_desc shd;
    ygfx_stim s;
    int64_t i0;
    int rc, i, j, k, ink_in = 0, ink_out = 0;
    float x0, y0, x1, y1;
    (void)st;
    memset(&sd, 0, sizeof sd);
    sd.backend = YSCR_BACKEND_CUSTOM;
    sd.presenter = &ygfx_headless_presenter;
    sd.presenter_ctx = &R->hl;
    sd.sim_period_ns = 1000000;
    memset(&d, 0, sizeof d);
    d.background[0] = d.background[1] = d.background[2] = 0.5f;
    d.cache = test_cache();
    yscr_close(&R->scr);
    sd.on_context = v111_start;   /* taken: refused before anything opens */
    CHECK(!ygfx_open_screen(&R->g, &R->scr, &sd, &d));
    CHECK(strstr(ygfx_error(&R->g), "on_context") != NULL);
    CHECK(!yscr_is_open(&R->scr));
    CHECK(!ygfx_open_screen(&R->g, NULL, &sd, &d));
    sd.on_context = NULL;
    if (!ygfx_open_screen(&R->g, &R->scr, &sd, &d)) {
        fprintf(stderr, "gfx_test [%s]: open_screen: %s\n", g_where, ygfx_error(&R->g));
        g_failures++;
        if (!yscr_is_open(&R->scr)) yscr_open(&R->scr, &sd);
        return;
    }
    CHECK(ygfx_is_open(&R->g) && yscr_is_open(&R->scr) && ygfx_screen(&R->g) == &R->scr);

    /* the loop: arguments, then 5 ms of 1 ms frames with nothing to end it */
    memset(&shd, 0, sizeof shd);
    shd.shape = YGFX_CIRCLE; shd.w = 20; shd.color[0] = 0.8f;
    s = ygfx_shape(&shd);
    CHECK(ygfx_show_until_key(NULL, &s, 1, NULL, 0, 0, NULL) == YGFX_ERR_ARG);
    CHECK(ygfx_show_until_key(&R->g, &s, -1, NULL, 0, 0, NULL) == YGFX_ERR_ARG);
    CHECK(ygfx_show_until_key(&R->g, NULL, 1, NULL, 0, 0, NULL) == YGFX_ERR_ARG);
    CHECK(ygfx_show_until_key(&R->g, &s, 1, NULL, 1, 0, NULL) == YGFX_ERR_ARG);
    CHECK(ygfx_show_until_key(&R->g, &s, 1, NULL, 0, -1, NULL) == YGFX_ERR_ARG);
    CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
    i0 = R->f.index;
    CHECK(yscr_flip(&R->scr) == YSCR_OK);
    v15_n = 0;
    yscr_on_flip(&R->scr, v15_rec, NULL);
    rc = ygfx_show_until_key(&R->g, &s, 1, NULL, 0, YRT_MS(5), NULL);
    CHECK(rc == YGFX_SHOW_TIME);
    gl_frame(&s, 1);
    gl_frame(&s, 1);                /* the loop's last record is in */
    yscr_on_flip(&R->scr, NULL, NULL);
    {   /* the loop's frames: the last is the first predicted 5 ms or more
         * after its first. The record's target is the onset predicted at
         * begin, which the loop compares; its planned vblank is later when
         * the frame came late (LATE_TARGET: 9 of 20 runs on Mesa llvmpipe
         * under WSL failed on planned). */
        int64_t first = 0, last = 0, before = 0;
        int nf = 0;
        for (k = 0; k < v15_n; k++)
            if (v15_recs[k].index > i0 && v15_recs[k].index < R->f.index - 1) {
                if (!nf) first = v15_recs[k].target;
                before = last;
                last = v15_recs[k].target;
                nf++;
            }
        CHECK(nf >= 2 && last - first >= YRT_MS(5) && before - first < YRT_MS(5));
    }

    /* text */
    {
        ygfx_text_desc td;
        memset(&td, 0, sizeof td);
        td.text = "Ab Cd\nEf";
        td.font = "no/such/font.ttf";
        CHECK(ygfx_text_make(&R->g, &t, &td) == YGFX_ERR_IO);
        CHECK(strstr(ygfx_error(&R->g), "no/such/font.ttf") != NULL);
        CHECK(t.items == NULL && t.n == 0);
        td.text = NULL;
        td.font = NULL;
        CHECK(ygfx_text_make(&R->g, &t, &td) == YGFX_ERR_ARG);
        td.text = "Ab Cd\nEf";
        td.size = 20;
        td.color[0] = td.color[1] = td.color[2] = 1.0f;
        rc = ygfx_text_make(&R->g, &t, &td);
        if (rc == YGFX_ERR_IO) {
            printf("  v0.15 text: no system font (%s): the text checks did not run\n", ygfx_error(&R->g));
        } else {
            CHECK(rc == YGFX_OK);
            CHECK(t.n == 6 && t.lines == 2);
            CHECK(t.primed > 0);                       /* a new gfx: the text program is new */
            CHECK(t.w > 0 && t.w <= 0.8f * W && t.h > 20);
            CHECK(t.items[4].y > t.items[0].y + 20);   /* "Ef" on the second line, 1.25 em down */
            CHECK(fabsf(t.items[4].y - t.items[0].y - 25.0f) < 1e-3f);
            CHECK(t.items[0].x == 0.0f && t.items[4].x > 0.0f);   /* the wider line at 0, the other centered */
            gl_frame(&t.stim, 1);
            ygfx_bounds(&R->g, &t.stim, &x0, &y0, &x1, &y1);
            for (j = 0; j < H; j++)
                for (i = 0; i < W; i++) {
                    const uint8_t* p = &R->out[((size_t)j * W + (size_t)i) * 4];   /* top row first */
                    int ink = p[0] != R->out[0] || p[1] != R->out[1] || p[2] != R->out[2];
                    int in = i >= (int)floorf(x0) - 1 && i <= (int)ceilf(x1) + 1 && j >= (int)floorf(y0) - 1 &&
                             j <= (int)ceilf(y1) + 1;
                    if (ink && in) ink_in++;
                    if (ink && !in) ink_out++;
                }
            CHECK(ink_in > 50 && ink_out == 0);
            /* a second text: the program is there, no prime; a wrap at
             * spaces, one word a line */
            td.text = "aaa aaa aaa";
            td.max_w = 50;
            CHECK(ygfx_text_make(&R->g, &t2, &td) == YGFX_OK);
            CHECK(t2.primed == 0 && t2.lines == 3 && t2.n == 9);
            for (k = 0; k < 9; k += 3) CHECK(t2.items[k].x == t2.items[0].x);   /* centered, equal widths */
            td.max_w = 0;
            td.text = "";
            CHECK(ygfx_text_make(&R->g, &t3, &td) == YGFX_OK && t3.n == 0 && t3.lines == 1);
            printf("  v0.15 text: %d items on %d lines, %.0f x %.0f px, %d programs primed, %.1f ms to build; "
                   "%d ink pixels inside the bounds, %d outside\n", t.n, t.lines, t.w, t.h, t.primed,
                   (double)t.build_ns / 1e6, ink_in, ink_out);
        }
        ygfx_text_free(&R->g, &t);
        ygfx_text_free(&R->g, &t);   /* twice: safe */
        ygfx_text_free(&R->g, &t2);
        ygfx_text_free(&R->g, &t3);
        CHECK(t.items == NULL && t.set.id == 0);
    }
    ygfx_close(&R->g);
}

static void s11_report(void) {
    if (!S11.ran) return;
    printf("  v0.10 simplex: GPU against ygfx_simplex_value(), 6 fields: wrong %ld of %ld; turned: largest |value| %.3f, "
           "mean %.4f; refusals %ld of %ld\n", S11.bad, S11.n, S11.turned_max, S11.turned_mean, S11.refused_ok, S11.refused_n);
    CHECK(S11.bad == 0 && S11.n > 100000);
    CHECK_LE(S11.turned_max, 1.0);
    CHECK_LE(S11.turned_mean, 0.05);
    CHECK(S11.refused_ok == S11.refused_n && S11.refused_n == 6);
    S11.ran = 0;
}

static void gl_v06_blur(stats* st) {
    static const ygfx_format fm[3] = { YGFX_RGBA32F, YGFX_RGBA16F, YGFX_R16F };
    static const double sig[6] = { 0.5, 1, 2, 4, 8, 16 };
    static float res[W * H * 4];
    int f, ss, k;
    (void)st;
    if (!gl_open(0, YGFX_RGBA32F, YGFX_DITHER_NONE)) return;
    S9.ran = 1;
    for (f = 0; f < 3; f++)
        for (ss = 1; ss <= 2; ss++) {
            ygfx_blur bl;
            ygfx_blur_desc bd;
            ygfx_shape_desc sd;
            ygfx_stim rect;
            memset(&bd, 0, sizeof bd);
            bd.w = W; bd.h = H; bd.format = fm[f]; bd.supersample = ss;
            if (ygfx_blur_make(&R->g, &bl, &bd) != YGFX_OK) {
                fprintf(stderr, "gfx_test [%s]: FAIL v0.6 blur_make: %s\n", g_where, ygfx_error(&R->g));
                g_failures++;
                continue;
            }
            /* a rect on whole pixels: its point samples are its box coverage */
            memset(&sd, 0, sizeof sd);
            sd.place = YGFX_TOP_LEFT; sd.anchor = YGFX_TOP_LEFT; sd.shape = YGFX_RECT;
            sd.x = 120; sd.y = 70; sd.w = 80; sd.h = 60; sd.color[0] = sd.color[1] = sd.color[2] = 1;
            rect = ygfx_shape(&sd);
            for (k = 0; k < 6; k++) {
                static const float zero[4] = { 0, 0, 0, 0 };
                int i, j;
                double mx = 0;
                CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
                CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
                CHECK(ygfx_begin_target(&R->g, bl.layer, zero) == YGFX_OK);
                CHECK(ygfx_draw(&R->g, &rect) == YGFX_OK);
                CHECK(ygfx_end_target(&R->g) == YGFX_OK);
                bl.sigma = (float)sig[k];
                CHECK(ygfx_blur_apply(&R->g, &bl) == YGFX_OK);
                CHECK(ygfx_end(&R->g) == YGFX_OK);
                CHECK(yscr_flip(&R->scr) == YSCR_OK);
                CHECK(ygfx_read_target(&R->g, bl.result, 0, 0, W, H, res) == YGFX_OK);
                for (j = 0; j < H; j++)
                    for (i = 0; i < W; i++) {
                        double d = fabs(res[(j * W + i) * 4] - bl_rect(i + 0.5, j + 0.5, 120, 200, 70, 130, sig[k]));
                        if (d > mx) mx = d;
                    }
                S9.rect[f][ss - 1][k] = mx;
            }
            if (f == 0 && ss == 1) {   /* a rect at the layer's corner: the plane outside is empty */
                static const float zero[4] = { 0, 0, 0, 0 };
                ygfx_stim corner = rect;
                int q;
                corner.x = 0; corner.y = 0; corner.w = 40; corner.h = 60;
                CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
                CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
                CHECK(ygfx_begin_target(&R->g, bl.layer, zero) == YGFX_OK);
                CHECK(ygfx_draw(&R->g, &corner) == YGFX_OK);
                CHECK(ygfx_end_target(&R->g) == YGFX_OK);
                bl.sigma = 4;
                CHECK(ygfx_blur_apply(&R->g, &bl) == YGFX_OK);
                CHECK(ygfx_end(&R->g) == YGFX_OK);
                CHECK(yscr_flip(&R->scr) == YSCR_OK);
                CHECK(ygfx_read_target(&R->g, bl.result, 0, 0, W, H, res) == YGFX_OK);
                for (q = 0; q < W * H; q++)
                    S9.edge = maxd(S9.edge, fabs(res[4 * q] - bl_rect(q % W + 0.5, q / W + 0.5, 0, 40, 0, 60, 4)));
            }
            if (f == 2 && ss == 1) {   /* the coverage image: tint.rgb, coverage x tint.a */
                int q;
                bl.image.place = YGFX_TOP_LEFT; bl.image.ax = 0; bl.image.ay = 0;
                bl.image.tint[0] = 0.2f; bl.image.tint[1] = 0.4f; bl.image.tint[2] = 0.6f; bl.image.tint[3] = 0.5f;
                gl_frame(&bl.image, 1);
                for (q = 0; q < W * H; q++) {
                    double a = 0.5 * res[4 * q];
                    S9.cover = maxd(S9.cover, fabs(R->scene[4 * q] - 0.2 * a));
                    S9.cover = maxd(S9.cover, fabs(R->scene[4 * q + 2] - 0.6 * a));
                }
            }
            ygfx_blur_free(&R->g, &bl);
        }
    {   /* refusals */
        ygfx_blur bl;
        ygfx_blur_desc bd;
        static const float zero[4] = { 0, 0, 0, 0 };
        memset(&bd, 0, sizeof bd);
        bd.w = 64; bd.h = 32; bd.format = YGFX_RGBA8;
        S9.refused_n++; S9.refused_ok += ygfx_blur_make(&R->g, &bl, &bd) == YGFX_ERR_ARG;
        bd.format = YGFX_R16F; bd.supersample = 3;
        S9.refused_n++; S9.refused_ok += ygfx_blur_make(&R->g, &bl, &bd) == YGFX_ERR_ARG;
        bd.supersample = 0; bd.sigma = 0.2f;
        CHECK(ygfx_blur_make(&R->g, &bl, &bd) == YGFX_OK);
        S9.refused_n++; S9.refused_ok += ygfx_blur_apply(&R->g, &bl) == YGFX_ERR_ORDER;   /* outside a frame */
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        S9.refused_n++; S9.refused_ok += ygfx_blur_apply(&R->g, &bl) == YGFX_ERR_ARG && strstr(ygfx_error(&R->g), "box");
        bl.sigma = 40;
        S9.refused_n++; S9.refused_ok += ygfx_blur_apply(&R->g, &bl) == YGFX_ERR_ARG;
        bl.sigma = 1;
        {
            ygfx_blur b2;
            S9.refused_n++; S9.refused_ok += ygfx_blur_make(&R->g, &b2, &bd) == YGFX_ERR_ORDER;   /* inside a frame */
        }
        CHECK(ygfx_begin_target(&R->g, bl.layer, zero) == YGFX_OK);
        S9.refused_n++; S9.refused_ok += ygfx_blur_apply(&R->g, &bl) == YGFX_ERR_ORDER;   /* inside a target pass */
        CHECK(ygfx_end_target(&R->g) == YGFX_OK);
        {   /* the 16 passes: 14 used, the blur's 2 fit; 15 used, they do not */
            int q;
            for (q = 0; q < 13; q++) { CHECK(ygfx_begin_target(&R->g, bl.layer, NULL) == YGFX_OK); CHECK(ygfx_end_target(&R->g) == YGFX_OK); }
            S9.refused_n++; S9.refused_ok += ygfx_blur_apply(&R->g, &bl) == YGFX_OK;
            S9.refused_n++; S9.refused_ok += ygfx_blur_apply(&R->g, &bl) == YGFX_ERR_ORDER;
        }
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
        ygfx_blur_free(&R->g, &bl);
    }
    ygfx_close(&R->g);
}

/* v0.12 (CAPTURE): what the writer gets is ygfx_read_output()'s bytes of the
 * same frame, bit for bit, rows top first, whole and as a rectangle; each
 * frame tagged with its index and its flip record. */
#define CAP_N 6
typedef struct gl_cap_store {
    int     n, rec_bad, with_record;
    int64_t idx[CAP_N];
    int     rect[CAP_N][4];
    unsigned char px[CAP_N][W * H * 4];
} gl_cap_store;
static void gl_cap_on_frame(void* ctx, const ygfx_capture_frame* fr) {
    gl_cap_store* s = (gl_cap_store*)ctx;
    int y;
    if (fr->has_record) { s->with_record++; if (fr->record.index != fr->index) s->rec_bad++; }
    if (s->n >= CAP_N) { s->n++; return; }
    s->idx[s->n] = fr->index;
    s->rect[s->n][0] = fr->x; s->rect[s->n][1] = fr->y; s->rect[s->n][2] = fr->w; s->rect[s->n][3] = fr->h;
    for (y = 0; y < fr->h; y++) memcpy(s->px[s->n] + (size_t)y * fr->w * 4, fr->rgba + (ptrdiff_t)y * fr->stride, (size_t)fr->w * 4);
    s->n++;
}

static void gl_v12_capture(stats* st) {
    static unsigned char want[CAP_N][W * H * 4];
    static gl_cap_store cs;
    ygfx_shape_desc sd;
    ygfx_capture_desc cd;
    ygfx_capture_stats cst;
    ygfx_stim s[2];
    long bad = 0, rect_bad = 0;
    int k, i, y, rc;
    (void)st;
    if (!gl_open(1, YGFX_RGBA16F, YGFX_DITHER_NONE)) return;
    memset(&sd, 0, sizeof sd);
    sd.place = YGFX_TOP_LEFT; sd.anchor = YGFX_TOP_LEFT;   /* top-left: a turned row order shows */
    sd.shape = YGFX_RECT; sd.w = 40; sd.h = 20; sd.y = 12; sd.color[0] = 0.9f; sd.color[1] = 0.2f;
    s[0] = ygfx_shape(&sd);
    sd.place = YGFX_BOTTOM_RIGHT; sd.anchor = YGFX_BOTTOM_RIGHT;
    sd.shape = YGFX_CIRCLE; sd.w = 30; sd.x = -8; sd.y = -6; sd.color[0] = 0.1f; sd.color[2] = 0.7f;
    s[1] = ygfx_shape(&sd);
    memset(&cs, 0, sizeof cs);
    memset(&cd, 0, sizeof cd);
    cd.every = 1; cd.slots = CAP_N;
    cd.on_frame = gl_cap_on_frame; cd.ctx = &cs;
    rc = ygfx_capture_start(&R->g, &cd);
    CHECK(rc == YGFX_OK);
    if (rc != YGFX_OK) { fprintf(stderr, "  capture: %s\n", ygfx_error(&R->g)); ygfx_close(&R->g); return; }
    for (k = 0; k < CAP_N; k++) {
        s[0].x = 10.0f + 7.0f * (float)k;   /* each frame differs */
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, s, 2) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        /* also checks that the copy left no pack buffer bound */
        CHECK(ygfx_read_output(&R->g, 0, 0, W, H, want[k]) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    CHECK(ygfx_capture_stop(&R->g, &cst) == CAP_N);
    CHECK(cst.dropped == 0 && cst.failed == 0 && cs.n == CAP_N && cs.rec_bad == 0 && cs.with_record >= CAP_N - 3);
    for (k = 0; k < CAP_N && k < cs.n; k++) {
        CHECK(cs.idx[k] == R->f.index - (CAP_N - 1) + k);
        bad += memcmp(cs.px[k], want[k], sizeof want[k]) != 0;
    }
    CHECK(bad == 0);
    /* a rectangle, marked frames only, no writer thread */
    memset(&cs, 0, sizeof cs);
    memset(&cd, 0, sizeof cd);
    cd.x = 7; cd.y = 11; cd.w = 45; cd.h = 17; cd.manual = true;
    cd.on_frame = gl_cap_on_frame; cd.ctx = &cs;
    CHECK(ygfx_capture_start(&R->g, &cd) == YGFX_OK);
    for (k = 0; k < 4; k++) {
        s[0].x = 3.0f + 5.0f * (float)k;
        CHECK(yscr_begin(&R->scr, &R->f) == YSCR_OK);
        CHECK(ygfx_begin(&R->g, &R->f) == YGFX_OK);
        CHECK(ygfx_draw_n(&R->g, s, 2) == YGFX_OK);
        if (k == 1) CHECK(ygfx_capture_mark(&R->g) == YGFX_OK);
        CHECK(ygfx_end(&R->g) == YGFX_OK);
        if (k == 1) CHECK(ygfx_read_output(&R->g, 0, 0, W, H, want[0]) == YGFX_OK);
        CHECK(yscr_flip(&R->scr) == YSCR_OK);
    }
    CHECK(ygfx_capture_pump(&R->g, 0) == 1);   /* its record came with a later begin */
    CHECK(ygfx_capture_stop(&R->g, &cst) == 1 && cst.no_record == 0 && cst.chosen == 1);
    CHECK(cs.n == 1 && cs.rect[0][0] == 7 && cs.rect[0][1] == 11 && cs.rect[0][2] == 45 && cs.rect[0][3] == 17);
    for (y = 0; y < 17; y++)
        for (i = 0; i < 45 * 4; i++) rect_bad += cs.px[0][(size_t)y * 45 * 4 + (size_t)i] != want[0][((size_t)(11 + y) * W + 7) * 4 + (size_t)i];
    CHECK(rect_bad == 0);
    printf("  measured: capture: %d whole frames and a %dx%d rectangle against ygfx_read_output: %ld frames and %ld bytes differ\n",
           CAP_N, 45, 17, bad, rect_bad);
    ygfx_close(&R->g);
}

#if defined(__SANITIZE_ADDRESS__)
    #define SANITIZED 1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define SANITIZED 1
    #endif
#endif
#ifndef SANITIZED
    #define SANITIZED 0
#endif

static int sanitized(void) { return SANITIZED; }   /* not a constant: C4127 */

/* ran: renderers that ran so far. With no list the default runs the first
 * renderer that opens (hardware, else WARP, else SwiftShader); a list, or
 * YGFX_TEST_FULL=1, runs each one named, or all. */
static int wanted(const char* name, int ran) {
    const char* e = getenv("YGFX_TEST_DEVICES");
    /* Under ASan, Mesa leaves 112 bytes from a module it unloads, which a
     * suppression cannot name: GL runs only when asked for by name. */
    if (sanitized() && (!e || !e[0])) return 0;
    if (e && e[0]) return strstr(e, name) != NULL;
    return cs_full() || ran == 0;
}

int main(void) {
    int ran = 0;
    const char* req = getenv("YGFX_TEST_REQUIRE_GL");
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash keeps what was printed */
    test_noise_cpu();
    test_cal_srgb();
    test_cones_dkl();
    test_tables();
    test_enum_params();
    test_desc_params();
    test_coordinates();
    test_null_and_ring();
    test_v12_capture_cpu();
    test_v03_cpu();
    test_v04_cache_cpu();
    test_v11_start_cpu();
    test_v04_fixes_cpu();
    test_v04_inst_cpu();
    test_v06_cset_cpu();
    test_v10_simplex_cpu();
    printf("v0.4 instances: the shader's cosine and sine of degrees, -720 to 720, against double: max %.2e\n", S6.csd_err);
    printf("v0.7 ellipse: the quarter perimeter against Simpson's rule, aspect 1 to 20: max relative %.2e\n", S4.ell_quarter);
    R = (rig*)calloc(1, sizeof *R);
    if (!R) return 1;
#if defined(_WIN32)
    if (wanted("hardware", ran)) ran += gl_suite("ANGLE D3D11 hardware", YGFX_HL_HARDWARE);
    if (wanted("warp", ran)) ran += gl_suite("ANGLE D3D11 WARP", YGFX_HL_WARP);
    if (wanted("swiftshader", ran)) ran += gl_suite("ANGLE Vulkan SwiftShader", YGFX_HL_SWIFTSHADER);
#else
    if (wanted("mesa", ran)) ran += gl_suite("Mesa llvmpipe", YGFX_HL_MESA_SOFTWARE);
#endif
    free(R);
    if (cs_ol_ok == 1) yol_cset_close(&cs_ol);
    if (cs_ol_ok >= 0) yol_close(&cs_ol_cx);
    if (!ran) {
        printf("no GL ES 3.0 renderer opened%s: the GL checks did not run\n",
               sanitized() ? " (a sanitizer build runs GL only with YGFX_TEST_DEVICES)" : "");
        if (req && req[0] == '1') { fprintf(stderr, "gfx_test: FAIL: YGFX_TEST_REQUIRE_GL=1 and no GL\n"); g_failures++; }
    }
    if (g_failures) { fprintf(stderr, "gfx_test: %d failure(s)\n", g_failures); return 1; }
    printf("gfx_test: all checks passed (%d renderer(s))\n", ran);
    return 0;
}
