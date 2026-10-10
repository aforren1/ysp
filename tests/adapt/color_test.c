/* color_test.c - self-checking test for ysp/color.h. Returns 0 when
 * every check passed, 1 after printing each failure.
 *
 * The calibration against published values (IEC 61966-2-1, CVRL's cone
 * tables, Psychtoolbox's ComputeDKL_M run in MATLAB R2023a), the
 * conversions against colour-science (tests/compare/color_refs.py), round
 * trips through every space, gamut edges, gamut mapping, device codes,
 * the participant luminance fit against a synthetic observer, every
 * refusal and the parameter tables. Until ysp/gfx.h v0.5 took its
 * calibration from this header, a second build compared the two codes bit
 * for bit (docs/color.md).
 *
 *     gcc -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o color_test tests/adapt/color_test.c -lm && ./color_test
 *     cl /nologo /W4 /WX /Iinclude tests\adapt\color_test.c
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_COLOR_IMPLEMENTATION
#include "ysp/color.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static const char* g_where = "";

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "color_test [%s]: FAIL line %d: %s\n", g_where, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_LE(v, lim) do { double v_ = (double)(v), l_ = (double)(lim); if (!(v_ <= l_)) { \
    fprintf(stderr, "color_test [%s]: FAIL line %d: %s = %.4g, limit %.4g\n", g_where, __LINE__, #v, v_, l_); g_failures++; } } while (0)

static double maxd(double a, double b) { return a > b ? a : b; }
static double absd3(const double* a, const double* b) {
    return maxd(fabs(a[0] - b[0]), maxd(fabs(a[1] - b[1]), fabs(a[2] - b[2])));
}

static uint32_t g_rng = 12345u;
static double rnd(void) {   /* a fixed LCG: the same draws everywhere */
    g_rng = g_rng * 1664525u + 1013904223u;
    return (double)(g_rng >> 8) / 16777216.0;
}

/* ---------------------------------------------------------- reference data */

/* Psychtoolbox-3 data and results, as in tests/adapt/gfx_test.c. */

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

/* colour-science 0.4.7: hex, then sRGB values, XYZ (D65, white Y = 1), xyY,
 * CIELAB, CIELCh, CIELUV, CIELChuv (white D65), Oklab, OkLCh */
static const struct ref_srgb { const char* hex; double srgb[3], xyz[3], xyy[3], lab[3], lch[3], luv[3], lchuv[3], ok[3], oklch[3]; } ref_srgb[] = {
    { "#3366cc", { 0.20000000000000001, 0.40000000000000002, 0.80000000000000004 }, { 0.1701429663087399, 0.14565432029905193, 0.59043445255984195 }, { 0.18774774591873683, 0.16072524720093581, 0.14565432029905193 }, { 45.033149225804692, 18.71938976550469, -57.851516281610536 }, { 45.033149225804692, 60.804716011789175, 287.93036079655803 }, { 45.033149225804692, -19.256763488047689, -88.181049730278147 }, { 45.033149225804692, 90.259184970663199, 257.68127843290102 }, { 0.53248221822003283, -0.022504866639656129, -0.16644266286170717 }, { 0.53248221822003283, 0.16795722384870684, 262.29968367948948 } },
    { "#ff8800", { 1, 0.53333333333333333, 0 }, { 0.50042853803221499, 0.38871448340402159, 0.048676731637677037 }, { 0.53360844276519948, 0.4144874130982239, 0.38871448340402159 }, { 68.658044019889388, 38.839212340679111, 74.984732471261566 }, { 68.658044019889388, 84.446400272782668, 62.617577888884014 }, { 68.658044019889388, 99.262047186629147, 64.082399220286433 }, { 68.658044019889388, 118.15036141082581, 32.84580677408686 }, { 0.74420101632476865, 0.1000687310974612, 0.15093771319993446 }, { 0.74420101632476865, 0.18109650523817866, 56.456405734740436 } },
    { "#808080", { 0.50196078431372548, 0.50196078431372548, 0.50196078431372548 }, { 0.2051658917495936, 0.21586050011389923, 0.23508455073194559 }, { 0.31269999999999998, 0.32899999999999996, 0.21586050011389923 }, { 53.585013452169022, 5.5511151231257827e-14, 0 }, { 53.585013452169022, 5.5511151231257827e-14, 0 }, { 53.585013452169022, 0, -3.8669355211140417e-14 }, { 53.585013452169022, 3.8669355211140417e-14, 270 }, { 0.59987008698325994, -1.3050521204216935e-05, -7.3877123015286513e-05 }, { 0.59987008698325994, 7.5020966460833411e-05, 259.9819555901845 } },
    { "#000000", { 0, 0, 0 }, { 0, 0, 0 }, { 0.31269999999999998, 0.32900000000000001, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, -0, -0 }, { 0, 0, 180 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { "#ffffff", { 1, 1, 1 }, { 0.95045592705167148, 0.99999999999999989, 1.0890577507598784 }, { 0.31269999999999998, 0.32900000000000001, 0.99999999999999989 }, { 100, 0, 0 }, { 100, 0, 0 }, { 100, -3.6082248300317588e-14, -7.2164496600635175e-14 }, { 100, 8.0682359980536373e-14, 243.43494882292202 }, { 0.9999988020105659, -2.1755553165390996e-05, -0.00012315505659374508 }, { 0.9999988020105659, 0.00012506187291953016, 259.98195559029449 } },
    { "#ff0000", { 1, 0, 0 }, { 0.41239079926595934, 0.2126390058715103, 0.019330818715591825 }, { 0.64000000000000001, 0.33000000000000002, 0.2126390058715103 }, { 53.237115595429358, 80.090113523103852, 67.203263511722128 }, { 53.237115595429358, 104.55001152926587, 39.999865154398115 }, { 53.237115595429358, 175.00982216288483, 37.765093625559807 }, { 53.237115595429358, 179.03809692362091, 12.177050630061149 }, { 0.62795361302884789, 0.22482856719742469, 0.12579221904949675 }, { 0.62795361302884789, 0.25762679791016208, 29.227136382616841 } },
    { "#00ff00", { 0, 1, 0 }, { 0.35758433938387796, 0.71516867876775592, 0.11919477979462595 }, { 0.29999999999999999, 0.59999999999999998, 0.71516867876775592 }, { 87.735519109660004, -86.18159689039895, 83.186620273629998 }, { 87.735519109660004, 119.78013789910383, 136.0130686850149 }, { 87.735519109660004, -83.067119714400576, 107.41811123934231 }, { 87.735519109660004, 135.78953199666856, 127.71501294924313 }, { 0.86643922605352208, -0.23391802061156955, 0.17941914883598714 }, { 0.86643922605352208, 0.29480310604853671, 142.51117284256142 } },
    { "#0000ff", { 0, 0, 1 }, { 0.18048078840183429, 0.072192315360733714, 0.95053215224966059 }, { 0.14999999999999999, 0.059999999999999998, 0.072192315360733714 }, { 32.300872903980178, 79.195270307404257, -107.85546553974265 }, { 32.300872903980178, 133.80841634911252, 306.2888032572933 }, { 32.300872903980178, -9.4024072148240645, -130.35108850356178 }, { 32.300872903980178, 130.68975298582811, 265.87432021817733 }, { 0.45201369026739663, -0.032432372380517216, -0.3116359835541917 }, { 0.45201369026739663, 0.31331907861478364, 264.05854055386931 } },
    { "#123456", { 0.070588235294117646, 0.20392156862745098, 0.33725490196078434 }, { 0.03156921519960211, 0.032563114098139168, 0.092665590846139623 }, { 0.2013369512212507, 0.20767567623510955, 0.032563114098139168 }, { 21.042472421009528, 1.0581235170462333, -24.10054889938209 }, { 21.042472421009528, 24.123765929656042, 272.51393039081665 }, { 21.042472421009528, -10.83016033133517, -27.648646066368094 }, { 21.042472421009528, 29.694107194959035, 248.60938776939491 }, { 0.31916821391268629, -0.02338620260344227, -0.06862711763998422 }, { 0.31916821391268629, 0.072502384428248176, 251.18232533601486 } },
    { "#fedcba", { 0.99607843137254903, 0.86274509803921573, 0.72941176470588232 }, { 0.75326200366516183, 0.75803647184825429, 0.57119684936658588 }, { 0.36171125796336007, 0.36400392490289696, 0.75803647184825429 }, { 89.768096816306311, 6.8121198020388851, 21.068471403647713 }, { 89.768096816306311, 22.142390645177674, 72.082298267452586 }, { 89.768096816306311, 23.242479853178061, 28.842545865728621 }, { 89.768096816306311, 37.041940037504069, 51.136740066835941 }, { 0.91438845909952315, 0.022875864098644675, 0.05380614606360537 }, { 0.91438845909952315, 0.058467140450664222, 66.967065554778813 } },
    { "#00ffff", { 0, 1, 1 }, { 0.53806512778571225, 0.78736099412848959, 1.0697269320442866 }, { 0.22464749252514271, 0.32873097309051369, 0.78736099412848959 }, { 91.114752316705363, -48.078888386977326, -14.128985262449456 }, { 91.114752316705363, 50.111951998240272, 196.3765264724403 }, { 91.114752316705363, -70.464379963877803, -15.205397466927034 }, { 91.114752316705363, 72.086288264974328, 192.17705063006122 }, { 0.90539875909735779, -0.14945453283605514, -0.039517809979664698 }, { 0.90539875909735779, 0.15459079756192595, 194.81082848609555 } },
    { "#7f7f80", { 0.49803921568627452, 0.49803921568627452, 0.50196078431372548 }, { 0.20237108011101793, 0.21249279694372067, 0.23458173845201161 }, { 0.31160589166979225, 0.32719105629486239, 0.21249279694372067 }, { 53.221242989390788, 0.19981766796517419, -0.54159353206923999 }, { 53.221242989390788, 0.57727866270136419, 290.25123522447183 }, { 53.221242989390788, -0.056526983152420267, -0.78366673718639757 }, { 53.221242989390788, 0.78570277764348539, 265.87432021817654 }, { 0.59680651961661646, 0.00040854342873541331, -0.0015097233856081901 }, { 0.59680651961661646, 0.0015640244352998912, 285.14204617760248 } },
};
/* Display P3 and Rec. 2020 (exact alpha and beta) encoded values to XYZ (D65, white Y = 1) */
static const struct ref_wide { double enc[3], p3[3], r2020[3]; } ref_wide[] = {
    { { 0.20000000000000001, 0.5, 0.90000000000000002 }, { 0.22905036007454327, 0.21807214216924811, 0.83167074522207018 }, { 0.20954759209437313, 0.23864901631389518, 0.86563979094600074 } },
    { { 1, 0, 0 }, { 0.48657094864821598, 0.2289745640697487, -3.9720755169334855e-17 }, { 0.63695804830129155, 0.26270021201126709, 4.9941065744660767e-17 } },
    { { 0.050000000000000003, 0.01, 0.59999999999999998 }, { 0.065262216547946755, 0.026693222396887876, 0.33258003273345715 }, { 0.069202824247555722, 0.026127771322926086, 0.38834336560577482 } },
    { { 0.69999999999999996, 0.69999999999999996, 0.69999999999999996 }, { 0.42579324185585676, 0.44798841244188331, 0.48788525282044609 }, { 0.46812423635475875, 0.49252597940746928, 0.53638923532430471 } },
};
/* Bradford, D65 to xy (0.300, 0.320): XYZ of #3366cc adapted */
static const double ref_white2[3] = { 0.9375, 1, 1.1875 };
static const double ref_bradford_3366cc[3] = { 0.17606124922921818, 0.14815675785374244, 0.6447170360104294 };
/* Stockman & Sharpe 10 degree rows (CVRL linss10e_1): 440, 570 nm */
static const double ref_ss10_440[3] = { 0.059461900000000026, 0.095761199999999935, 0.96695999999999971 }, ref_ss10_570[3] = { 0.99954299999999985, 0.7765260000000006, 0.00015568800000000018 };
/* CIELAB near the CIE's epsilon (216/24389 = 0.008856): a D65 gray at Y = 0.0089 and 0.0088 */
static const double ref_dark_89[6] = { 0.0084590577507598769, 0.0088999999999999999, 0.0096926139817629185, 8.0392727374874298, 0, 0 };
static const double ref_dark_88[6] = { 0.0083640121580547105, 0.0088000000000000005, 0.0095837082066869313, 7.9490074074074073, 0, 0 };

#ifndef YCOL_TEST_PIN
#define YCOL_TEST_PIN 0x86f31b63u
#endif

static double srgb_oetf(double l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }

static const float SRGB_XY[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };

/* Calibrations used throughout: static, 62 KB each. */
static ycol_cal cal_srgb;   /* nominal sRGB primaries, 80 cd/m2, gamma 2.2: xy, no spectra */
static ycol_cal cal_full;   /* the same with B_monitor's spectra: xy and spectra           */
static ycol_cal cal_bm;     /* B_monitor's spectra, luminance readings only                */
static ycol_cal cal_lumo;   /* luminance readings only: no xy, no spectra                  */

static void bm_spectra(float* r, float* g, float* b) {
    int i;
    for (i = 0; i < 81; i++) { r[i] = b_monitor[i][0]; g[i] = b_monitor[i][1]; b[i] = b_monitor[i][2]; }
}

static void make_cals(void) {
    float r[81], g[81], b[81];
    char err[200];
    int k, gun;
    bm_spectra(r, g, b);
    CHECK(ycol_cal_nominal(&cal_srgb, SRGB_XY, 80.0f, 2.2) == YCOL_OK);
    CHECK(ycol_cal_nominal(&cal_full, SRGB_XY, 80.0f, 2.2) == YCOL_OK);
    CHECK(ycol_cal_set_spectra(&cal_full, 380, 5, 81, r, g, b, NULL) == YCOL_OK);
    CHECK(ycol_cal_derive(&cal_full, err, sizeof err) == YCOL_OK);
    ycol_cal_init(&cal_bm);
    ycol_cal_add(&cal_bm, YCOL_GUN_BLACK, 0, 0, 0, 0);
    for (k = 0; k < 3; k++) ycol_cal_add(&cal_bm, k, 1, 1, 0, 0);
    CHECK(ycol_cal_set_spectra(&cal_bm, 380, 5, 81, r, g, b, NULL) == YCOL_OK);
    CHECK(ycol_cal_derive(&cal_bm, err, sizeof err) == YCOL_OK);
    ycol_cal_init(&cal_lumo);
    ycol_cal_add(&cal_lumo, YCOL_GUN_BLACK, 0, 0.2f, 0, 0);
    for (gun = 0; gun < 3; gun++)
        for (k = 1; k <= 16; k++) ycol_cal_add(&cal_lumo, gun, k / 16.0f, (float)(0.2 + 60.0 * pow(k / 16.0, 2.2)), 0, 0);
    CHECK(ycol_cal_derive(&cal_lumo, err, sizeof err) == YCOL_OK);
}

/* ycol_ctx_open() as a code and a message, as v0.1's ycol_ctx_init() gave
 * them, so the checks below compare codes. */
static int ctx_rc(ycol_ctx* cx, const ycol_ctx_desc* d, char* err, size_t cap) {
    bool ok = ycol_ctx_open(cx, d);
    if (err && cap) {   /* truncated to cap, as v0.1 wrote it */
        const char* m = ycol_ctx_error(cx);
        size_t k = strlen(m);
        if (k >= cap) k = cap - 1;
        memcpy(err, m, k);
        err[k] = '\0';
    }
    if (ok != (cx->status == YCOL_OK) || ok != (ycol_ctx_error(cx)[0] == '\0')) return -100;   /* inconsistent */
    return ok ? YCOL_OK : cx->status;
}

static int ctx_on(ycol_ctx* cx, const ycol_cal* c, double bg) {
    ycol_ctx_desc d;
    char err[200];
    int rc;
    memset(&d, 0, sizeof d);
    d.cal = c;
    d.background[0] = d.background[1] = d.background[2] = bg;
    rc = ctx_rc(cx, &d, err, sizeof err);
    if (rc < 0) fprintf(stderr, "ctx: %s\n", err);
    return rc;
}

/* --------------------------------------------------------------- the tests */

/* ysp/gfx.h v0.3's calibration checks, on the moved code. */
static void test_cal_published(void) {
    static ycol_cal c, c2;
    static const double iec[9] = { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 };
    static unsigned char bytes[sizeof(ycol_cal)];
    char err[200];
    double e = 0, e17 = 0, e64 = 0, l[3], m[9];
    float r[81], g[81], b[81];
    const float bg[3] = { 0.4f, 0.5f, 0.3f };
    int k, gun, rc;
    g_where = "calibration";
    CHECK(ycol_cal_nominal(&c, SRGB_XY, 1.0f, 2.2) == YCOL_OK);
    for (k = 0; k < 9; k++) e = maxd(e, fabs(c.rgb_to_xyz[k] - iec[k]));
    printf("sRGB primaries to XYZ, against IEC 61966-2-1's printed matrix: max |diff| %.2e\n", e);
    CHECK_LE(e, 1.2e-4);
    CHECK(c.flags & YCOL_CAL_NOMINAL);
    for (rc = 0; rc < 2; rc++) {
        int n = rc ? 64 : 17;
        double mm = 0;
        ycol_cal_init(&c2);
        ycol_cal_add(&c2, YCOL_GUN_BLACK, 0, 0, 0, 0);
        for (gun = 0; gun < 3; gun++)
            for (k = 1; k < n; k++) ycol_cal_add(&c2, gun, (float)k / (float)(n - 1), (float)(80.0 * srgb_eotf((double)k / (n - 1))), 0, 0);
        CHECK(ycol_cal_derive(&c2, err, sizeof err) == YCOL_OK);
        for (k = 0; k < YCOL_CAL_MAX_LUT; k++) mm = maxd(mm, fabs(c2.lut[1][k] - srgb_oetf((double)k / (YCOL_CAL_MAX_LUT - 1))));
        if (rc) e64 = mm; else e17 = mm;
    }
    printf("CLUT against sRGB's encoding: 17 levels %.2e, 64 levels %.2e\n", e17, e64);
    CHECK_LE(e17, 4e-3);
    CHECK_LE(e64, 1.1e-4);
    CHECK(ycol_cal_save(&c2, bytes, sizeof bytes) == (int)sizeof bytes);
    memset(&c, 0, sizeof c);
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes, err, sizeof err) == YCOL_OK);
    CHECK(memcmp(&c, &c2, sizeof c) == 0);
    CHECK(ycol_cal_check(&c, err, sizeof err) == YCOL_OK);
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes - 1, err, sizeof err) == YCOL_ERR_FORMAT);
    bytes[1000] ^= 1;
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes, err, sizeof err) == YCOL_ERR_FORMAT);
    CHECK(strstr(err, "CRC") != NULL);
    bytes[1000] ^= 1;
    c2.lut[0][100] += 1e-4f;
    ycol_cal_save(&c2, bytes, sizeof bytes);
    CHECK(ycol_cal_load(&c, bytes, sizeof bytes, err, sizeof err) == YCOL_ERR_FORMAT);
    CHECK(strstr(err, "CLUT") != NULL);
    ycol_cal_init(&c2);
    ycol_cal_add(&c2, YCOL_GUN_BLACK, 0, 0.5f, 0, 0);
    for (gun = 0; gun < 3; gun++) {
        ycol_cal_add(&c2, gun, 0.5f, 10.0f, 0, 0);
        ycol_cal_add(&c2, gun, 0.75f, 9.0f, 0, 0);
        ycol_cal_add(&c2, gun, 1.0f, 30.0f, 0, 0);
    }
    CHECK(ycol_cal_derive(&c2, err, sizeof err) == YCOL_ERR_RANGE);
    CHECK(strstr(err, "0.75") != NULL);

    /* cones and DKL against Psychtoolbox */
    ycol_cone_fundamentals(YCOL_CONES_SS2, 440.0, l);
    CHECK(fabs(l[0] / 4.02563e-2 - 1) < 1e-7 && fabs(l[1] / 6.47782e-2 - 1) < 1e-7 && fabs(l[2] / 9.91020e-1 - 1) < 1e-7);
    ycol_cone_fundamentals(YCOL_CONES_SS2, 570.0, l);
    CHECK(fabs(l[0] / 9.99993e-1 - 1) < 1e-7 && fabs(l[1] / 8.13509e-1 - 1) < 1e-7 && fabs(l[2] / 2.81800e-4 - 1) < 1e-7);
    ycol_cone_fundamentals(YCOL_CONES_SS2, 700.0, l);
    CHECK(l[2] == 0.0 && l[0] > 0.0);
    ycol_cone_fundamentals(YCOL_CONES_SS2, 389.0, l);
    CHECK(l[0] == 0.0);
    bm_spectra(r, g, b);
    (void)r; (void)g; (void)b;
    e = 0;
    for (k = 0; k < 9; k++) e = maxd(e, fabs(cal_bm.rgb_to_lms[k] - ref_rgb_to_lms[k]) / fabs(ref_rgb_to_lms[k]));
    printf("RGB to LMS against Psychtoolbox (B_monitor, SS2): max relative diff %.2e\n", e);
    CHECK_LE(e, 1e-6);
    CHECK(ycol_cal_dkl_matrix(&cal_bm, bg, m) == YCOL_OK);
    e = 0;
    for (k = 0; k < 9; k++) e = maxd(e, fabs(m[k] - ref_dkl[k]) / (fabs(ref_dkl[k]) + 1e-12));
    printf("DKL matrix against ComputeDKL_M: max relative diff %.2e\n", e);
    CHECK_LE(e, 1e-6);
    e = 0;
    for (k = 0; k < 3; k++) {
        float unit[3] = { 0, 0, 0 }, dir[3];
        double inc[3], lb[3] = { 0, 0, 0 }, v;
        int j;
        unit[k] = 1.0f;
        CHECK(ycol_cal_dir_dkl(&cal_bm, bg, unit, dir) == YCOL_OK);
        for (j = 0; j < 3; j++) e = maxd(e, fabs(dir[j] - ref_dkl_rgb[j * 3 + k]) / fabs(ref_dkl_rgb[j * 3 + k]));
        ycol_cal_lms(&cal_bm, bg, lb);
        for (j = 0; j < 3; j++)
            inc[j] = cal_bm.rgb_to_lms[j * 3] * dir[0] + cal_bm.rgb_to_lms[j * 3 + 1] * dir[1] + cal_bm.rgb_to_lms[j * 3 + 2] * dir[2];
        v = 0.68990272 * inc[0] + 0.34832189 * inc[1];
        if (k == 1) { CHECK(fabs(inc[2]) < 1e-6 * fabs(inc[0])); CHECK(fabs(v) < 1e-5 * fabs(inc[0])); }
        if (k == 2) { CHECK(fabs(inc[0]) < 1e-5 * fabs(inc[2])); CHECK(fabs(inc[1]) < 1e-5 * fabs(inc[2])); }
        if (k == 0) CHECK(fabs(inc[0] / lb[0] - inc[1] / lb[1]) < 1e-5 * fabs(inc[0] / lb[0]));
    }
    printf("DKL unit axes as RGB against Psychtoolbox: max relative diff %.2e\n", e);
    CHECK_LE(e, 1e-5);
    {
        float d3[3], dir[3] = { 0.1f, -0.2f, 0.05f }, bgm[3] = { 0.5f, 0.5f, 0.25f }, cc[3] = { 0.1f, 0, 0 }, cdir[3];
        CHECK(ycol_cal_dir_cone(&cal_srgb, bgm, cc, cdir) == YCOL_ERR_REFUSED);
        ycol_dkl_from_sph(90, 0, 2, d3);
        CHECK(fabs(d3[0] - 2) < 1e-6 && fabs(d3[1]) < 1e-6);
        ycol_dkl_from_sph(0, 90, 1, d3);
        CHECK(fabs(d3[2] - 1) < 1e-6 && fabs(d3[0]) < 1e-6);
        CHECK(fabsf(ycol_max_contrast(bgm, dir) - 2.5f) < 1e-6f);
    }
    {   /* the 10 degree table: CVRL rows through colour-science */
        double l10[3];
        ycol_cone_fundamentals(YCOL_CONES_SS10, 440.0, l10);
        CHECK(fabs(l10[0] / ref_ss10_440[0] - 1) < 1e-6 && fabs(l10[1] / ref_ss10_440[1] - 1) < 1e-6 && fabs(l10[2] / ref_ss10_440[2] - 1) < 1e-6);
        ycol_cone_fundamentals(YCOL_CONES_SS10, 570.0, l10);
        CHECK(fabs(l10[0] / ref_ss10_570[0] - 1) < 1e-6 && fabs(l10[1] / ref_ss10_570[1] - 1) < 1e-6 && fabs(l10[2] / ref_ss10_570[2] - 1) < 1e-6);
    }
    {   /* integrating SS2 at context time gives the stored matrix, bit for bit */
        double mm[9], bl[3];
        CHECK(ycol__integrate(&cal_full, YCOL_CONES_SS2, mm, bl) == 1);
        CHECK(memcmp(mm, cal_full.rgb_to_lms, sizeof mm) == 0 && memcmp(bl, cal_full.black_lms, sizeof bl) == 0);
    }
}

/* The calibration's bits: an FNV-1a hash of a calibration derived from
 * float literals (no libm in the inputs). Pinned where the arithmetic is
 * plain IEEE double with no contraction: x64 and WebAssembly. */
static void test_pinned(void) {
    static ycol_cal c;
    static const float lv[6] = { 0.1f, 0.25f, 0.4f, 0.6f, 0.8f, 1.0f };
    static const float yv[3][6] = { { 0.9f, 2.6f, 6.1f, 12.9f, 22.4f, 34.7f }, { 2.1f, 7.9f, 19.0f, 40.3f, 70.2f, 108.6f },
                                    { 0.6f, 1.3f, 2.9f, 6.0f, 10.1f, 15.8f } };
    float r[81], g[81], b[81];
    char err[200];
    const unsigned char* p = (const unsigned char*)&c;
    uint32_t h = 2166136261u;
    size_t i;
    int k, gun;
    g_where = "pinned";
    ycol_cal_init(&c);
    ycol_cal_add(&c, YCOL_GUN_BLACK, 0, 0.35f, 0.29f, 0.31f);
    for (gun = 0; gun < 3; gun++)
        for (k = 0; k < 6; k++) ycol_cal_add(&c, gun, lv[k], yv[gun][k] + 0.35f, SRGB_XY[gun][0], SRGB_XY[gun][1]);
    ycol_cal_add(&c, YCOL_GUN_WHITE, 1, 158.9f, 0.3127f, 0.329f);
    bm_spectra(r, g, b);
    ycol_cal_set_spectra(&c, 380, 5, 81, r, g, b, NULL);
    CHECK(ycol_cal_derive(&c, err, sizeof err) == YCOL_OK);
    for (i = 0; i < sizeof c; i++) h = (h ^ p[i]) * 16777619u;
    printf("calibration from literals: FNV-1a %08x, CRC %08x\n", (unsigned)h, (unsigned)c.crc);
#if defined(__x86_64__) || defined(_M_X64) || defined(__EMSCRIPTEN__)
    CHECK(h == YCOL_TEST_PIN);
#endif
    {   /* a black with light: XYZ and LMS are absolute, so device 0 is the black */
        ycol_ctx cx;
        ycol_color o;
        CHECK(ctx_on(&cx, &c, 0.5) == YCOL_OK);
        CHECK(ycol_convert(&cx, YCOL_RGB(0, 0, 0), YCOL_SPACE_XYZ, &o, NULL) == 0 && c.black_xyz[1] > 0.3);
        CHECK(fabs(o.u.xyz.X - c.black_xyz[0]) < 1e-12 && fabs(o.u.xyz.Y - c.black_xyz[1]) < 1e-12);
        CHECK(ycol_convert(&cx, YCOL_RGB(0, 0, 0), YCOL_SPACE_LMS, &o, NULL) == 0 && fabs(o.u.lms.l - c.black_lms[0]) < 1e-15);
        CHECK(fabs(cx.white_xyz[1] - (c.rgb_to_xyz[3] + c.rgb_to_xyz[4] + c.rgb_to_xyz[5] + c.black_xyz[1])) < 1e-12);
    }
}

static void test_ctx(void) {
    static ycol_cal bad;
    ycol_ctx cx;
    ycol_ctx_desc d;
    char err[300], line[1024];
    g_where = "context";
    memset(&d, 0, sizeof d);
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_ERR_ARG && strstr(err, "desc.cal") != NULL);
    memcpy(&bad, &cal_srgb, sizeof bad);
    bad.lut[0][5] += 1e-3f;   /* changed after it was sealed */
    d.cal = &bad;
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_ERR_FORMAT && strstr(err, "CRC") != NULL);
    d.cal = &cal_srgb;
    d.background[1] = 1.5;
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_ERR_RANGE && strstr(err, "background[1]") != NULL);
    d.background[1] = 0.5;
    d.cones = 7;
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_ERR_ARG);
    d.cones = 0;
    d.white[0] = 1.0;
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_ERR_RANGE);
    d.white[0] = 0.0;
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_OK);
    CHECK(cx.can == (YCOL_CAN_RGB | YCOL_CAN_XYZ));
    CHECK(ycol_ctx_describe(&cx, line, sizeof line) > 0);
    printf("%s\n", line);
    CHECK(strstr(line, "NOMINAL") != NULL && strstr(line, "relative to the display") != NULL &&
          strstr(line, "no chromatic adaptation") != NULL && strstr(line, "standard luminance") != NULL);
    CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
    CHECK(cx.can == (YCOL_CAN_RGB | YCOL_CAN_XYZ | YCOL_CAN_LMS | YCOL_CAN_BG));
    CHECK(ctx_on(&cx, &cal_lumo, 0.5) == YCOL_OK);
    CHECK(cx.can == YCOL_CAN_RGB && cx.why_no_xyz && cx.why_no_lms);
    CHECK(ctx_on(&cx, &cal_bm, 0.0) == YCOL_OK);   /* black background, black 0: no cone contrast */
    CHECK(cx.can == (YCOL_CAN_RGB | YCOL_CAN_LMS) && strstr(cx.why_no_bg, "above 0") != NULL);
    {   /* the id follows every input */
        uint32_t id0;
        double bg[3] = { 0.5, 0.5, 0.25 };
        CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
        id0 = cx.id;
        CHECK(ycol_ctx_set_background(&cx, bg) == YCOL_OK && cx.id != id0);
        bg[2] = 0.5;
        CHECK(ycol_ctx_set_background(&cx, bg) == YCOL_OK && cx.id == id0);
        bg[2] = -0.1;
        CHECK(ycol_ctx_set_background(&cx, bg) == YCOL_ERR_RANGE);
        CHECK(cx.status == YCOL_ERR_RANGE && strstr(ycol_ctx_error(&cx), "outside 0..1") != NULL);
        CHECK(cx.id == id0);   /* a refused background changes nothing */
        bg[2] = 0.5;
        {   /* the contrast spaces follow the background */
            ycol_ctx fresh;
            CHECK(ctx_on(&fresh, &cal_full, 0.25) == YCOL_OK);
            bg[0] = bg[1] = bg[2] = 0.25;
            CHECK(ycol_ctx_set_background(&cx, bg) == YCOL_OK);
            CHECK(memcmp(cx.dkl, fresh.dkl, sizeof cx.dkl) == 0 && memcmp(cx.bg_lms, fresh.bg_lms, sizeof cx.bg_lms) == 0);
            CHECK(cx.id == fresh.id);
        }
    }
    {   /* v0.2 open and close: close on a zeroed context, after a failed open, twice */
        ycol_ctx z;
        double bg2[3] = { 0.5, 0.5, 0.5 };
        memset(&z, 0, sizeof z);
        ycol_ctx_close(&z);
        ycol_ctx_close(NULL);
        CHECK(ycol_ctx_error(&z)[0] == '\0' && ycol_ctx_error(NULL)[0] == '\0');
        CHECK(ycol_ctx_set_background(&z, bg2) == YCOL_ERR_ARG && z.status == YCOL_ERR_ARG &&
              strstr(ycol_ctx_error(&z), "not open") != NULL);
        CHECK(!ycol_ctx_open(&z, NULL) && z.status == YCOL_ERR_ARG && strstr(ycol_ctx_error(&z), "NULL desc") != NULL);
        CHECK(!ycol_ctx_open(NULL, NULL));
        ycol_ctx_close(&z);
        ycol_ctx_close(&z);
        CHECK(z.status == YCOL_OK && z.can == 0 && ycol_ctx_error(&z)[0] == '\0');
        CHECK(ctx_on(&z, &cal_full, 0.5) == YCOL_OK && z.can != 0);
        ycol_ctx_close(&z);
        CHECK(z.can == 0 && z.desc.cal == NULL);
        ycol_ctx_close(&z);
    }
    {   /* MacLeod-Boynton's scale: colour-science's tables give 1 / 26.9108 and 1 / 18.0247 */
        ycol_ctx c10;
        d.cal = &cal_full;
        d.cones = YCOL_CONES_SS10;
        CHECK(ctx_rc(&c10, &d, err, sizeof err) == YCOL_OK);
        CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
        printf("MacLeod-Boynton s scale: SS2 %.7f, SS10 %.7f\n", cx.mb_k, c10.mb_k);
        CHECK(fabs(cx.mb_k / 0.037159819 - 1) < 1e-6 && fabs(c10.mb_k / 0.055479334 - 1) < 1e-6);
        CHECK(c10.can == cx.can && memcmp(c10.rgb_to_lms, cx.rgb_to_lms, sizeof cx.rgb_to_lms) != 0);
        CHECK(fabs(c10.w[0] - 0.69283932) < 1e-12 && fabs(c10.w[1] - 0.34967567) < 1e-12);
    }
}

/* One conversion from XYZ against its reference, and back; the hue only
 * where the chroma is not 0. */
static double ref_one(const ycol_ctx* cx, ycol_color c, int sp, const double* ref, int hue, const double* xyz, double* e_back) {
    ycol_color o, back;
    double e;
    CHECK(ycol_convert(cx, c, sp, &o, NULL) == YCOL_OK);
    e = maxd(fabs(o.u.v[0] - ref[0]), fabs(o.u.v[1] - ref[1]));
    if (!hue) e = maxd(e, fabs(o.u.v[2] - ref[2]));
    else if (ref[1] > 1e-9) e = maxd(e, fabs(fmod(o.u.v[2] - ref[2] + 540.0, 360.0) - 180.0));
    CHECK(ycol_convert(cx, o, YCOL_SPACE_XYZ, &back, NULL) == YCOL_OK);
    *e_back = maxd(*e_back, absd3(back.u.v, xyz));
    return e;
}

/* Conversions against colour-science, on a context whose white is D65 at
 * Y = 1 and whose D65 sources are absolute at Y = 1: then each result
 * depends on the calibration only through rounding. */
static void test_refs(void) {
    static const double d65[3] = { 0.3127 / 0.3290, 1.0, (1.0 - 0.3127 - 0.3290) / 0.3290 };
    ycol_ctx cx;
    ycol_ctx_desc d;
    char err[200];
    double e_xyz = 0, e_xyy = 0, e_lab = 0, e_lch = 0, e_luv = 0, e_lchuv = 0, e_ok = 0, e_oklch = 0, e_back = 0, e_wide = 0, e;
    size_t i;
    int k;
    g_where = "colour-science";
    memset(&d, 0, sizeof d);
    d.cal = &cal_srgb;
    d.background[0] = d.background[1] = d.background[2] = 0.5;
    memcpy(d.white, d65, sizeof d65);
    d.src_white_Y = 1.0;
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_OK);
    for (i = 0; i < sizeof ref_srgb / sizeof ref_srgb[0]; i++) {
        const struct ref_srgb* r = &ref_srgb[i];
        ycol_color c = ycol_hex(r->hex), o;
        CHECK(c.space == YCOL_SPACE_SRGB && absd3(c.u.v, r->srgb) == 0.0);
        CHECK(ycol_convert(&cx, c, YCOL_SPACE_XYZ, &o, NULL) == YCOL_OK);
        e_xyz = maxd(e_xyz, absd3(o.u.v, r->xyz));
        c = ycol_make(YCOL_SPACE_XYZ, r->xyz[0], r->xyz[1], r->xyz[2]);
e_lab = maxd(e_lab, ref_one(&cx, c, YCOL_SPACE_CIELAB, r->lab, 0, r->xyz, &e_back));
        e_lch = maxd(e_lch, ref_one(&cx, c, YCOL_SPACE_CIELCH, r->lch, 1, r->xyz, &e_back));
        e_luv = maxd(e_luv, ref_one(&cx, c, YCOL_SPACE_CIELUV, r->luv, 0, r->xyz, &e_back));
        e_lchuv = maxd(e_lchuv, ref_one(&cx, c, YCOL_SPACE_CIELCHUV, r->lchuv, 1, r->xyz, &e_back));
        e_ok = maxd(e_ok, ref_one(&cx, c, YCOL_SPACE_OKLAB, r->ok, 0, r->xyz, &e_back));
        e_oklch = maxd(e_oklch, ref_one(&cx, c, YCOL_SPACE_OKLCH, r->oklch, 1, r->xyz, &e_back));
        if (r->xyz[1] > 0) e_xyy = maxd(e_xyy, ref_one(&cx, c, YCOL_SPACE_XYY, r->xyy, 0, r->xyz, &e_back));
        /* and back to the sRGB values */
        CHECK(ycol_convert(&cx, c, YCOL_SPACE_SRGB, &o, NULL) == YCOL_OK);
        e_back = maxd(e_back, absd3(o.u.v, r->srgb));
    }
    {   /* CIELAB on each side of the CIE's epsilon */
        const double* dk[2] = { ref_dark_89, ref_dark_88 };
        for (i = 0; i < 2; i++) {
            ycol_color o;
            CHECK(ycol_convert(&cx, ycol_make(YCOL_SPACE_XYZ, dk[i][0], dk[i][1], dk[i][2]), YCOL_SPACE_CIELAB, &o, NULL) == 0);
            e_lab = maxd(e_lab, absd3(o.u.v, dk[i] + 3));
        }
    }
    for (i = 0; i < sizeof ref_wide / sizeof ref_wide[0]; i++) {
        const struct ref_wide* r = &ref_wide[i];
        ycol_color o;
        CHECK(ycol_convert(&cx, ycol_make(YCOL_SPACE_DISPLAY_P3, r->enc[0], r->enc[1], r->enc[2]), YCOL_SPACE_XYZ, &o, NULL) == YCOL_OK);
        e_wide = maxd(e_wide, absd3(o.u.v, r->p3));
        CHECK(ycol_convert(&cx, ycol_make(YCOL_SPACE_REC2020, r->enc[0], r->enc[1], r->enc[2]), YCOL_SPACE_XYZ, &o, NULL) == YCOL_OK);
        e_wide = maxd(e_wide, absd3(o.u.v, r->r2020));
        CHECK(ycol_convert(&cx, o, YCOL_SPACE_REC2020, &o, NULL) == YCOL_OK);
        e_back = maxd(e_back, absd3(o.u.v, r->enc));
    }
    printf("against colour-science 0.4.7: XYZ %.1e, xyY %.1e, CIELAB %.1e, CIELCh %.1e, CIELUV %.1e, CIELChuv %.1e, "
           "Oklab %.1e, OkLCh %.1e, P3 and Rec.2020 %.1e; back to XYZ or the encoded values %.1e\n",
           e_xyz, e_xyy, e_lab, e_lch, e_luv, e_lchuv, e_ok, e_oklch, e_wide, e_back);
    CHECK_LE(e_xyz, 1e-12); CHECK_LE(e_xyy, 1e-12); CHECK_LE(e_lab, 1e-10); CHECK_LE(e_lch, 1e-9);
    CHECK_LE(e_luv, 1e-10); CHECK_LE(e_lchuv, 1e-9); CHECK_LE(e_ok, 1e-12); CHECK_LE(e_oklch, 1e-9);
    CHECK_LE(e_wide, 1e-12); CHECK_LE(e_back, 1e-10);
    {   /* Bradford: D65 sources onto a white at xy (0.300, 0.320) */
        ycol_color o;
        memcpy(d.white, ref_white2, sizeof d.white);
        d.adapt = YCOL_ADAPT_BRADFORD;
        CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_OK);
        CHECK(ycol_convert(&cx, ycol_hex("#3366cc"), YCOL_SPACE_XYZ, &o, NULL) == YCOL_OK);
        e = absd3(o.u.v, ref_bradford_3366cc);
        printf("Bradford against colour-science: %.1e\n", e);
        CHECK_LE(e, 1e-12);
        CHECK(ycol_ctx_describe(&cx, err, sizeof err) == YCOL_ERR_FULL);   /* too short a buffer is reported */
    }
    {   /* relative D65 sources on a nominal sRGB display: sRGB white and black are the display's */
        ycol_gamut g;
        ycol_rgb w, z;
        CHECK(ctx_on(&cx, &cal_srgb, 0.5) == YCOL_OK);
        w = ycol_to_rgb(&cx, ycol_hex("#ffffff"), &g);
        z = ycol_to_rgb(&cx, ycol_hex("#000"), NULL);
        CHECK(fabs(w.r - 1) < 1e-6 && fabs(w.g - 1) < 1e-6 && fabs(w.b - 1) < 1e-6 && g.status == 0);
        CHECK(z.r == 0 && z.g == 0 && z.b == 0);
        for (k = 0; k < 3; k++) {
            double m[9];
            CHECK(ycol_prim_to_rgb(&cx, YCOL_PRIM_BT709, m, NULL) == YCOL_OK);
            CHECK(fabs(m[k * 4] - 1) < 1e-6);
        }
    }
}

static const int ALL_SPACES[] = { YCOL_SPACE_RGB, YCOL_SPACE_DEVICE, YCOL_SPACE_XYZ, YCOL_SPACE_XYY, YCOL_SPACE_CIELAB,
                                  YCOL_SPACE_CIELCH, YCOL_SPACE_CIELUV, YCOL_SPACE_CIELCHUV, YCOL_SPACE_OKLAB, YCOL_SPACE_OKLCH,
                                  YCOL_SPACE_SRGB, YCOL_SPACE_DISPLAY_P3, YCOL_SPACE_REC2020, YCOL_SPACE_LMS, YCOL_SPACE_CONE,
                                  YCOL_SPACE_DKL, YCOL_SPACE_DKL_CART, YCOL_SPACE_MB };
#define N_SPACES ((int)(sizeof ALL_SPACES / sizeof ALL_SPACES[0]))

static void test_roundtrip(void) {
    ycol_ctx cx[3];
    double worst[N_SPACES];
    int s, i, j, k;
    char name[32];
    g_where = "round trip";
    CHECK(ctx_on(&cx[0], &cal_full, 0.5) == YCOL_OK);
    {   /* the 10 degree cones, a participant-like luminance, Bradford, absolute sources, a gray background */
        static ycol_lum l;
        ycol_ctx_desc d;
        double w[3] = { 0.75, 0.3, 0.01 };
        char err[200];
        CHECK(ycol_lum_stated(&l, YCOL_CONES_SS10, w) == YCOL_OK);
        memset(&d, 0, sizeof d);
        d.cal = &cal_full; d.cones = YCOL_CONES_SS10; d.lum = &l; d.adapt = YCOL_ADAPT_BRADFORD; d.src_white_Y = 80;
        d.background[0] = 0.3; d.background[1] = 0.6; d.background[2] = 0.2;
        CHECK(ctx_rc(&cx[1], &d, err, sizeof err) == YCOL_OK);
        d.white[0] = 70; d.white[1] = 75; d.white[2] = 90; d.adapt = 0; d.lum = NULL; d.cones = 0; d.src_white_Y = 0;
        CHECK(ctx_rc(&cx[2], &d, err, sizeof err) == YCOL_OK);
    }
    for (s = 0; s < N_SPACES; s++) worst[s] = 0;
    for (j = 0; j < 3; j++)
        for (i = 0; i < 20000; i++) {
            ycol_rgb in, out;
            in.r = rnd(); in.g = rnd(); in.b = rnd();
            if (i == 0) in.r = in.g = in.b = 0.5;   /* the background itself */
            for (s = 0; s < N_SPACES; s++) {
                int st;
                ycol_color c = ycol_from_rgb(&cx[j], in, ALL_SPACES[s], &st);
                ycol_gamut g;
                double a[3], b[3];
                if (st < 0) { CHECK(st == YCOL_OK); continue; }
                out = ycol_to_rgb(&cx[j], c, &g);
                CHECK(g.status == YCOL_OK);
                a[0] = in.r; a[1] = in.g; a[2] = in.b; b[0] = out.r; b[1] = out.g; b[2] = out.b;
                worst[s] = maxd(worst[s], absd3(a, b));
            }
        }
    for (s = 0; s < N_SPACES; s++) {
        int n;
        const ycol_space_info* si = ycol_spaces(&n);
        snprintf(name, sizeof name, "%s", si[ALL_SPACES[s] - 1].name);
        printf("  round trip rgb -> %-10s -> rgb: max |diff| %.1e\n", name, worst[s]);
        CHECK_LE(worst[s], 1e-12);
    }
    {   /* the poles: achromatic flags and defined values */
        ycol_color c;
        int st;
        ycol_rgb gray = { 0.5, 0.5, 0.5 };
        c = ycol_from_rgb(&cx[0], gray, YCOL_SPACE_DKL, &st);
        CHECK(st == 0 && c.u.dkl.contrast == 0 && (c.flags & YCOL_F_ACHROMATIC));
        c = ycol_from_rgb(&cx[0], gray, YCOL_SPACE_OKLCH, &st);   /* the display's white: D65 to float precision */
        CHECK(st == 0 && c.u.lch.C < 2e-4);   /* Ottosson's M1 puts D65 at C 6.8e-5 * L */
        c = ycol_from_rgb(&cx[0], gray, YCOL_SPACE_CONE, &st);
        CHECK(st == 0 && c.u.v[0] == 0 && c.u.v[1] == 0 && c.u.v[2] == 0);
        c = ycol_make(YCOL_SPACE_OKLAB, 0.1, 0.4, 0.3);   /* a cone response below 0: cube roots of negatives */
        CHECK(ycol_convert(&cx[0], c, YCOL_SPACE_OKLAB, &c, NULL) == 0);
        CHECK(fabs(c.u.lab.L - 0.1) < 1e-12 && fabs(c.u.lab.a - 0.4) < 1e-12 && fabs(c.u.lab.b - 0.3) < 1e-12);
        c = ycol_make(YCOL_SPACE_DKL, 90, 0, 0.2);   /* pure luminance */
        CHECK(ycol_convert(&cx[0], c, YCOL_SPACE_DKL, &c, NULL) == 0);
        CHECK(fabs(c.u.dkl.elev - 90) < 1e-6 && c.u.dkl.azim == 0 && (c.flags & YCOL_F_ACHROMATIC));
        c = ycol_make(YCOL_SPACE_XYZ, 0, 0, 0);
        CHECK(ycol_convert(&cx[0], c, YCOL_SPACE_XYY, &c, NULL) == 0 && (c.flags & YCOL_F_ACHROMATIC) && c.u.xyy.Y == 0);
        CHECK(ycol_convert(&cx[0], ycol_make(YCOL_SPACE_XYZ, 30, 30, 30), YCOL_SPACE_LMS, &c, NULL) == 0 &&
              (c.flags & YCOL_F_VIA_DEVICE));
        CHECK(ycol_convert(&cx[0], ycol_make(YCOL_SPACE_CIELAB, 50, 0, 0), YCOL_SPACE_OKLAB, &c, NULL) == 0 &&
              !(c.flags & YCOL_F_VIA_DEVICE));
    }
    {   /* convert_n against single conversions, with a stride and a refusal */
        double in[4 * 4], out[3 * 4];
        uint8_t fl[4];
        for (k = 0; k < 16; k++) in[k] = rnd();
        in[4 * 2 + 0] = 2.0;   /* a device value outside 0..1 */
        CHECK(ycol_convert_n(&cx[0], YCOL_SPACE_DEVICE, in, 4, YCOL_SPACE_DKL, out, 0, 4, fl) == 4);
        for (k = 0; k < 4; k++) {
            ycol_color o;
            int rc = ycol_convert(&cx[0], ycol_make(YCOL_SPACE_DEVICE, in[4 * k], in[4 * k + 1], in[4 * k + 2]), YCOL_SPACE_DKL, &o, NULL);
            if (k == 2) CHECK(rc == YCOL_ERR_RANGE && fl[k] == 2 && out[3 * k] != out[3 * k]);
            else CHECK(rc == 0 && absd3(o.u.v, out + 3 * k) == 0 && fl[k] <= 1);
        }
    }
}

static void test_gamut(void) {
    ycol_ctx cx, ce;
    ycol_gamut g;
    ycol_rgb r;
    double bg1[3] = { 1.0, 0.5, 0.5 };
    int i, k;
    g_where = "gamut";
    CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
    r = ycol_to_rgb(&cx, YCOL_RGB(1, 0.5, 0), &g);
    CHECK(g.status == 0 && g.in && g.margin[0] == 0 && g.margin[2] == 0 && g.distance == 0 && g.scale == 1 && !g.below && !g.above);
    r = ycol_to_rgb(&cx, YCOL_RGB(1 + 1e-15, 0.5, 0.5), &g);
    CHECK(g.in && g.above == 1u && g.distance > 0);
    r = ycol_to_rgb(&cx, YCOL_RGB(1 + 1e-8, 0.5, 0.5), &g);   /* inside the tolerance: in, distance exact */
    CHECK(g.in && g.above == 1u && fabs(g.distance - 1e-8) < 1e-15);
    r = ycol_to_rgb(&cx, YCOL_RGB(1 + 9.5e-7, 0.5, -9.5e-7), &g);
    CHECK(g.in && g.above == 1u && g.below == 4u);
    r = ycol_to_rgb(&cx, YCOL_RGB(1 + 9.6e-7, 0.5, 0.5), &g);   /* just past 2^-20 */
    CHECK(!g.in && g.above == 1u && fabs(g.distance - 9.6e-7) < 1e-15);
    r = ycol_to_rgb(&cx, YCOL_RGB(0.5, -9.6e-7, 0.5), &g);
    CHECK(!g.in && g.below == 2u);
    {   /* a source's gamut corners on a nominal display with its primaries: in */
        static ycol_cal cn;
        static const float pr[3][4][2] = {
            { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } },
            { { 0.680f, 0.320f }, { 0.265f, 0.690f }, { 0.150f, 0.060f }, { 0.3127f, 0.3290f } },
            { { 0.708f, 0.292f }, { 0.170f, 0.797f }, { 0.131f, 0.046f }, { 0.3127f, 0.3290f } } };
        static const int sp[3] = { YCOL_SPACE_SRGB, YCOL_SPACE_DISPLAY_P3, YCOL_SPACE_REC2020 };
        ycol_ctx cn_cx;
        double worst = 0;
        int j, c, n_in = 0;
        for (j = 0; j < 3; j++) {
            CHECK(ycol_cal_nominal(&cn, pr[j], 80.0f, 2.2) == YCOL_OK);
            CHECK(ctx_on(&cn_cx, &cn, 0.5) == YCOL_OK);
            for (c = 0; c < 8; c++) {
                ycol_to_rgb(&cn_cx, ycol_make(sp[j], c & 1, (c >> 1) & 1, (c >> 2) & 1), &g);
                worst = maxd(worst, g.distance);
                n_in += g.in;
            }
        }
        printf("sRGB, P3 and Rec. 2020 cube corners on nominal displays with those primaries: %d of 24 in gamut, "
               "largest distance %.2g (%.1f times 2^-24)\n", n_in, worst, worst * 16777216.0);
        CHECK(n_in == 24);
    }
    r = ycol_to_rgb(&cx, YCOL_RGB(0.5, -0.1, 0.5), &g);
    CHECK(!g.in && g.below == 2u && fabs(g.distance - 0.1) < 1e-15 && fabs(g.scale - 0.5 / 0.6) < 1e-15);
    CHECK(r.g == -0.1);   /* unclipped */
    r = ycol_to_dir(&cx, YCOL_RGB(0.7, 0.5, 0.5), &g);
    CHECK(g.in && fabs(r.r - 0.2) < 1e-15 && fabs(g.margin[0] - 0.3) < 1e-15 && fabs(g.scale - 2.5) < 1e-12);
    r = ycol_to_dir(&cx, YCOL_RGB(1.1, 0.5, 0.5), &g);   /* 0.1 out at both ends of the swing */
    CHECK(!g.in && g.below == 1u && g.above == 1u && fabs(g.distance - 0.1) < 1e-12);
    r = ycol_to_rgb(&cx, YCOL_RGB(0.5, 0.5, 0.5), &g);
    CHECK(g.in && g.scale == HUGE_VAL);
    CHECK(ycol_max_scale(&cx, YCOL_RGB(0.5, 0.5, 0.5), YCOL_SYMMETRIC) == HUGE_VAL);
    r = ycol_to_rgb(&cx, YCOL_RGB(0.5, NAN, 0.5), &g);
    CHECK(g.status == YCOL_ERR_ARG && r.r != r.r && g.why != NULL);
    CHECK(ycol_max_scale(&cx, YCOL_RGB(1, 1, 1), 5) == YCOL_ERR_ARG);
    /* a background on a face: no room toward it */
    CHECK(ycol_ctx_set_background(&cx, bg1) == YCOL_OK);
    CHECK(ycol_max_scale(&cx, YCOL_RGB(1.2, 0.5, 0.5), YCOL_ONE_SIDED) == 0.0);
    CHECK(ycol_max_scale(&cx, YCOL_RGB(0.8, 0.5, 0.5), YCOL_SYMMETRIC) == 0.0);
    CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
    {   /* max_scale against a bisection on `in`, random DKL directions, both modes */
        double worst = 0;
        for (i = 0; i < 400; i++) {
            ycol_color c = ycol_make(YCOL_SPACE_DKL, rnd() * 180 - 90, rnd() * 360, 1.0);
            int sides = i & 1, it;
            double kk = ycol_max_scale(&cx, c, sides), lo = 0, hi = 1e3;
            CHECK(kk > 0 && kk < 1e3);
            for (it = 0; it < 200; it++) {
                double mid = 0.5 * (lo + hi);
                ycol_color m = c;
                m.u.dkl.contrast = mid;
                if (sides) ycol_to_dir(&cx, m, &g); else ycol_to_rgb(&cx, m, &g);
                if (g.distance == 0) lo = mid; else hi = mid;
            }
            worst = maxd(worst, fabs(lo - kk) / kk);
        }
        printf("max_scale against a bisection on the gamut: max relative diff %.1e\n", worst);
        CHECK_LE(worst, 1e-12);
    }
    {   /* rings */
        double ring[72], ok[72];
        CHECK(ycol_max_ring(&cx, YCOL_PLANE_DKL, 0, YCOL_SYMMETRIC, 72, ring) == 0);
        for (i = 0; i < 72; i++) CHECK(ring[i] == ycol_max_scale(&cx, ycol_make(YCOL_SPACE_DKL, 0, 5.0 * i, 1), YCOL_SYMMETRIC));
        CHECK(ycol_max_ring(&cx, YCOL_PLANE_CONE, 0, YCOL_ONE_SIDED, 72, ring) == 0);
        for (i = 0; i < 72; i++) {
            double t = 5.0 * i * 3.14159265358979323846 / 180;
            CHECK(fabs(ring[i] / ycol_max_scale(&cx, ycol_make(YCOL_SPACE_CONE, cos(t), sin(t), 0), YCOL_ONE_SIDED) - 1) < 1e-12);
        }
        CHECK(ycol_max_ring(&cx, YCOL_PLANE_OKLCH, 0.6, 0, 72, ok) == 0);
        for (i = 0; i < 72; i++) {
            ycol_to_rgb(&cx, ycol_make(YCOL_SPACE_OKLCH, 0.6, ok[i], 5.0 * i), &g);
            CHECK(g.in && ok[i] > 0.01 && ok[i] < 0.4);
            ycol_to_rgb(&cx, ycol_make(YCOL_SPACE_OKLCH, 0.6, ok[i] * (1 + 1e-6), 5.0 * i), &g);
            CHECK(g.distance > 0);
        }
        CHECK(ycol_max_ring(&cx, YCOL_PLANE_CIELCH, 120, 0, 4, ok) == 0 && ok[0] == -1 && ok[3] == -1);
        CHECK(ycol_max_ring(&cx, 99, 0, 0, 4, ok) == YCOL_ERR_ARG);
    }
    {   /* ce: no spectra */
        CHECK(ctx_on(&ce, &cal_srgb, 0.5) == YCOL_OK);
        CHECK(ycol_max_scale(&ce, YCOL_DKL(.azim = 90, .contrast = 1), YCOL_SYMMETRIC) == YCOL_ERR_REFUSED);
        double four[4];
        CHECK(ycol_max_ring(&ce, YCOL_PLANE_DKL, 0, 0, 4, four) == YCOL_ERR_REFUSED);
    }
    (void)k;
}

static void test_map(void) {
    ycol_ctx cx;
    ycol_gamut g, g0;
    int i, n_out = 0, sp;
    double e_lh = 0, e_h = 0, e_lum = 0, e_dir = 0;
    g_where = "map";
    CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
    for (i = 0; i < 2000; i++) {
        ycol_color c = ycol_make(YCOL_SPACE_OKLCH, 0.35 + 0.5 * rnd(), 0.4 * rnd(), 360 * rnd()), o;
        ycol_rgb in = ycol_to_rgb(&cx, c, &g0), m;
        int method = 1 + i % 5;
        m = ycol_map(&cx, c, method, &g);
        if (g.status < 0) { CHECK(method == YCOL_MAP_CHROMA_DKL || method == YCOL_MAP_CHROMA_OKLCH || method == YCOL_MAP_CHROMA_CIELCH); continue; }
        if (g0.in) { CHECK(m.r == in.r && m.g == in.g && m.b == in.b && g.kept == 1.0); continue; }
        n_out++;
        ycol_to_rgb(&cx, YCOL_RGB(m.r, m.g, m.b), &g0);
        CHECK(g0.in);
        CHECK(g.distance > 0 && !g.in);
        if (method == YCOL_MAP_CHROMA_OKLCH) {
            int st;
            o = ycol_from_rgb(&cx, m, YCOL_SPACE_OKLCH, &st);
            e_lh = maxd(e_lh, fabs(o.u.lch.L - c.u.lch.L));
            if (o.u.lch.C > 1e-6) e_h = maxd(e_h, fabs(fmod(o.u.lch.h - c.u.lch.h + 540, 360) - 180));
            CHECK(g.kept >= 0 && g.kept < 1);
        } else if (method == YCOL_MAP_CHROMA_DKL) {
            int st;
            ycol_color a = ycol_from_rgb(&cx, in, YCOL_SPACE_DKL_CART, &st), b = ycol_from_rgb(&cx, m, YCOL_SPACE_DKL_CART, &st);
            e_lum = maxd(e_lum, fabs(a.u.dkl_cart.lum - b.u.dkl_cart.lum));
            e_dir = maxd(e_dir, fabs(a.u.dkl_cart.lm * b.u.dkl_cart.s - a.u.dkl_cart.s * b.u.dkl_cart.lm));
        } else if (method == YCOL_MAP_SCALE) {
            double d0[3] = { in.r - 0.5, in.g - 0.5, in.b - 0.5 }, d1[3] = { m.r - 0.5, m.g - 0.5, m.b - 0.5 };
            e_dir = maxd(e_dir, fabs(d0[0] * d1[1] - d0[1] * d1[0]) + fabs(d0[1] * d1[2] - d0[2] * d1[1]));
            CHECK(fabs(g.kept - g.scale) == 0 && g.kept < 1);
        } else if (method == YCOL_MAP_CLIP) {
            CHECK(g.kept != g.kept);
        }
    }
    printf("map: %d colors outside mapped; OkLCh L kept within %.1e, h within %.1e deg; DKL luminance part kept within %.1e; "
           "directions kept within %.1e\n", n_out, e_lh, e_h, e_lum, e_dir);
    CHECK(n_out > 500);
    CHECK_LE(e_lh, 1e-12); CHECK_LE(e_h, 1e-8); CHECK_LE(e_lum, 1e-12); CHECK_LE(e_dir, 1e-12);
    /* unreachable */
    ycol_map(&cx, YCOL_OKLCH(.L = 1.3, .C = 0.1, .h = 30), YCOL_MAP_CHROMA_OKLCH, &g);
    CHECK(g.status == YCOL_ERR_RANGE && strstr(g.why, "gray") != NULL);
    ycol_map(&cx, YCOL_DKL(.elev = 90, .azim = 0, .contrast = 5), YCOL_MAP_CHROMA_DKL, &g);
    CHECK(g.status == YCOL_ERR_RANGE && strstr(g.why, "luminance part") != NULL);
    ycol_map(&cx, YCOL_RGB(2, 0, 0), 0, &g);
    CHECK(g.status == YCOL_ERR_ARG);
    for (sp = 0; sp < 1; sp++) {
        ycol_rgb m = ycol_map(&cx, YCOL_RGB(1.5, -0.25, 0.5), YCOL_MAP_CLIP, &g);
        CHECK(m.r == 1 && m.g == 0 && m.b == 0.5 && fabs(g.distance - 0.5) < 1e-15);
    }
}

static void test_device(void) {
    static ycol_cal cs[5];
    const char* name[5] = { "gamma 2.2 nominal", "sRGB, 17 levels", "sRGB, 64 levels", "gamma 4, 9 levels", "B_monitor, 1 level" };
    char err[200];
    int i, k, gun, bad = 0;
    g_where = "device";
    memcpy(&cs[0], &cal_srgb, sizeof cs[0]);
    for (i = 1; i < 4; i++) {
        int n = i == 1 ? 17 : (i == 2 ? 64 : 9);
        ycol_cal_init(&cs[i]);
        ycol_cal_add(&cs[i], YCOL_GUN_BLACK, 0, 0.1f, 0, 0);
        for (gun = 0; gun < 3; gun++)
            for (k = 1; k < n; k++) {
                double v = (double)k / (n - 1);
                ycol_cal_add(&cs[i], gun, (float)v, (float)(0.1 + 90.0 * (i == 3 ? pow(v, 4.0) : srgb_eotf(v))), 0, 0);
            }
        CHECK(ycol_cal_derive(&cs[i], err, sizeof err) == YCOL_OK);
    }
    memcpy(&cs[4], &cal_bm, sizeof cs[4]);
    for (i = 0; i < 5; i++) {
        ycol_ctx cx;
        int nbad = 0;
        CHECK(ctx_on(&cx, &cs[i], 0.5) == YCOL_OK);
        for (k = 0; k < 256; k++) {
            ycol_gamut g;
            ycol_rgb r = ycol_to_rgb(&cx, YCOL_DEVICE(k / 255.0, k / 255.0, k / 255.0), &g);
            uint32_t code[3] = { 0, 0, 0 };
            CHECK(g.in);
            CHECK(ycol_output_code(&cs[i], r, 8, code) == YCOL_OK);
            for (gun = 0; gun < 3; gun++) if (code[gun] != (uint32_t)k) nbad++;
        }
        printf("  device codes through the CLUT and back (%s): %d of 768 differ\n", name[i], nbad);
        bad += nbad;
    }
    CHECK(bad == 0);
    {
        uint32_t code[3];
        ycol_rgb r = { -0.5, 2.0, 0.0 };
        CHECK(ycol_output_code(&cal_srgb, r, 8, code) == 0 && code[0] == 0 && code[1] == 255 && code[2] == 0);
        CHECK(ycol_output_code(&cal_srgb, r, 7, code) == YCOL_ERR_ARG);
        CHECK(ycol_output_code(&cal_srgb, r, 10, code) == 0 && code[1] == 1023);
        /* rounding: a device value 0.502 of a code above k writes k + 1, 0.498 writes k */
        {
            ycol_ctx cx;
            int j, nbad = 0;
            CHECK(ctx_on(&cx, &cal_srgb, 0.5) == YCOL_OK);
            for (j = 0; j < 255; j++) {
                ycol_rgb up = ycol_to_rgb(&cx, YCOL_DEVICE((j + 0.502) / 255.0, (j + 0.498) / 255.0, 0), NULL);
                CHECK(ycol_output_code(&cal_srgb, up, 8, code) == 0);
                nbad += code[0] != (uint32_t)j + 1 || code[1] != (uint32_t)j;
            }
            CHECK(nbad == 0);
        }
    }
}

static void test_lum(void) {
    static ycol_lum l, l2;
    static unsigned char bytes[sizeof(ycol_lum)];
    const double wtrue[3] = { 0.75, 0.30, 0.0 }, wtrue_s[3] = { 0.70, 0.32, 0.03 };
    ycol_ctx cx;
    ycol_ctx_desc d;
    char err[300];
    double e;
    int i, k;
    g_where = "luminance";
    CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
    /* null pairs of a synthetic observer: a = bg + u, b = bg - u with w.(M u) = 0 */
    for (i = 0; i < 2; i++) {
        const double* wt = i ? wtrue_s : wtrue;
        int s, ns = i ? 3 : 1;
        ycol_lum_init(&l, YCOL_CONES_SS2, YCOL_LUM_HFP);
        if (i) l.flags |= YCOL_LUM_FIT_S;
        snprintf(l.participant, sizeof l.participant, "P%02d", i + 1);
        for (s = 0; s < ns; s++) {
            double u[3], v[3] = { 0.03, 0.03, 0.03 }, mu[3], mv[3], t, a[3], b[3];
            u[0] = s == 2 ? 0.02 : 0.08; u[1] = s == 1 ? 0.0 : -0.06; u[2] = s == 1 ? 0.09 : 0.01;
            ycol__mul3(cal_full.rgb_to_lms, u, mu);
            ycol__mul3(cal_full.rgb_to_lms, v, mv);
            t = -(wt[0] * mu[0] + wt[1] * mu[1] + wt[2] * mu[2]) / (wt[0] * mv[0] + wt[1] * mv[1] + wt[2] * mv[2]);
            for (k = 0; k < 3; k++) { u[k] += t * v[k]; a[k] = 0.5 + u[k]; b[k] = 0.5 - u[k]; }
            CHECK(ycol_lum_add(&l, a, b, cx.bg, 0.01, 5) == YCOL_OK);
        }
        CHECK(ycol_lum_derive(&l, &cal_full, err, sizeof err) == YCOL_OK);
        {
            double n1 = sqrt(l.w[0] * l.w[0] + l.w[1] * l.w[1] + l.w[2] * l.w[2]), n2 = sqrt(wt[0] * wt[0] + wt[1] * wt[1] + wt[2] * wt[2]);
            e = 0;
            for (k = 0; k < 3; k++) e = maxd(e, fabs(l.w[k] / n1 - wt[k] / n2));
            printf("luminance fit, %s: weights %.8f %.8f %.8f, direction within %.1e of the observer's, |resid| %.1e\n",
                   i ? "FIT_S, 3 settings" : "1 setting", l.w[0], l.w[1], l.w[2], e, fabs(l.resid[0]));
            CHECK_LE(e, 1e-9);
            CHECK(fabs(l.resid[0]) < 1e-12);
        }
        {   /* an equal-energy spectrum keeps the standard luminance */
            double sum[3] = { 0, 0, 0 }, lt[3], ws[3];
            int nm;
            for (nm = 390; nm <= 830; nm++) { ycol_cone_fundamentals(YCOL_CONES_SS2, nm, lt); for (k = 0; k < 3; k++) sum[k] += lt[k]; }
            ycol_cone_luminance(YCOL_CONES_SS2, ws);
            e = (l.w[0] * sum[0] + l.w[1] * sum[1] + l.w[2] * sum[2]) / (ws[0] * sum[0] + ws[1] * sum[1]) - 1;
            CHECK(fabs(e) < 1e-12);
        }
        /* the context with it: the L-M axis is isoluminant for the observer */
        memset(&d, 0, sizeof d);
        d.cal = &cal_full; d.lum = &l;
        d.background[0] = d.background[1] = d.background[2] = 0.5;
        {
            ycol_ctx cl;
            ycol_rgb u;
            double mu[3], du[3];
            CHECK(ctx_rc(&cl, &d, err, sizeof err) == YCOL_OK);
            u = ycol_to_dir(&cl, YCOL_DKL_CART(.lm = 0.1), NULL);
            du[0] = u.r; du[1] = u.g; du[2] = u.b;
            ycol__mul3(cal_full.rgb_to_lms, du, mu);
            e = fabs(wt[0] * mu[0] + wt[1] * mu[1] + wt[2] * mu[2]) / (fabs(wt[0] * mu[0]) + fabs(wt[1] * mu[1]));
            CHECK_LE(e, 1e-12);
            char line[1024];
            CHECK(ycol_ctx_describe(&cl, line, sizeof line) > 0 && strstr(line, "P0") != NULL);
            {   /* the luminance axis: S contrast equals luminance contrast, w's S weight included */
                ycol_rgb lu = ycol_to_dir(&cl, YCOL_DKL_CART(.lum = 0.1), NULL);
                double dl[3], ml[3], bl[3], vb;
                dl[0] = lu.r; dl[1] = lu.g; dl[2] = lu.b;
                ycol__mul3(cal_full.rgb_to_lms, dl, ml);
                memcpy(bl, cl.bg_lms, sizeof bl);
                vb = l.w[0] * bl[0] + l.w[1] * bl[1] + l.w[2] * bl[2];
                e = fabs((l.w[0] * ml[0] + l.w[1] * ml[1] + l.w[2] * ml[2]) / vb - ml[2] / bl[2]) / fabs(ml[2] / bl[2]);
                CHECK_LE(e, 1e-12);
            }
            {   /* the id carries the record */
                ycol_ctx c0;
                CHECK(ctx_on(&c0, &cal_full, 0.5) == YCOL_OK && c0.id != cl.id);
            }
        }
    }
    /* record round trip and checks */
    CHECK(ycol_lum_save(&l, bytes, sizeof bytes) == (int)sizeof bytes);
    CHECK(ycol_lum_load(&l2, bytes, sizeof bytes, err, sizeof err) == YCOL_OK && memcmp(&l, &l2, sizeof l) == 0);
    CHECK(ycol_lum_check(&l2, &cal_full, err, sizeof err) == YCOL_OK);
    l2.w[0] *= 1.001;
    l2.crc = 0;
    ycol_lum_save(&l2, NULL, 0);
    CHECK(ycol_lum_check(&l2, &cal_full, err, sizeof err) == YCOL_ERR_FORMAT);
    bytes[200] ^= 1;
    CHECK(ycol_lum_load(&l2, bytes, sizeof bytes, err, sizeof err) == YCOL_ERR_FORMAT);
    CHECK(ycol_lum_describe(&l, err, sizeof err) > 0 && strstr(err, "HFP") != NULL);
    /* refusals */
    {
        double a[3] = { 0.6, 0.4, 0.5 }, b[3] = { 0.4, 0.6, 0.5 };
        ycol_lum_init(&l2, YCOL_CONES_SS2, YCOL_LUM_HFP);
        ycol_lum_add(&l2, a, a, NULL, 0, 1);
        CHECK(ycol_lum_derive(&l2, &cal_full, err, sizeof err) == YCOL_ERR_RANGE && strstr(err, "equal") != NULL);
        ycol_lum_init(&l2, YCOL_CONES_SS2, YCOL_LUM_HFP);
        l2.flags |= YCOL_LUM_FIT_S;
        ycol_lum_add(&l2, a, b, NULL, 0, 1);
        CHECK(ycol_lum_derive(&l2, &cal_full, err, sizeof err) == YCOL_ERR_RANGE);
        l2.flags = 0;
        CHECK(ycol_lum_derive(&l2, &cal_srgb, err, sizeof err) == YCOL_ERR_REFUSED && strstr(err, "spectra") != NULL);
        /* red against green: on B_monitor green has more L and more M, so no
         * null with positive weights exists */
        CHECK(ycol_lum_derive(&l2, &cal_full, err, sizeof err) == YCOL_ERR_RANGE && strstr(err, "not a luminance") != NULL);
        a[1] = 0.6; b[1] = 0.4;   /* brighter against darker: no null with positive weights */
        ycol_lum_init(&l2, YCOL_CONES_SS2, YCOL_LUM_HFP);
        ycol_lum_add(&l2, a, b, NULL, 0, 1);
        CHECK(ycol_lum_derive(&l2, &cal_full, err, sizeof err) == YCOL_ERR_RANGE && strstr(err, "not a luminance") != NULL);
        memcpy(&l2, &l, sizeof l2);
        l2.cal_crc ^= 1;   /* settings from another calibration */
        CHECK(ycol_lum_derive(&l2, &cal_full, err, sizeof err) == YCOL_ERR_REFUSED && strstr(err, "settings were made") != NULL);
        CHECK(ycol_lum_stated(&l2, YCOL_CONES_SS10, wtrue) == YCOL_OK);
        memset(&d, 0, sizeof d);
        d.cal = &cal_full; d.lum = &l2;
        CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_ERR_REFUSED && strstr(err, "SS10") != NULL);
    }
    {   /* the probe's two lights */
        double a[3], b[3];
        CHECK(ctx_on(&cx, &cal_full, 0.5) == YCOL_OK);
        CHECK(ycol_lum_pair(&cx, 0, 10, 0.05, a, b) == YCOL_OK);
        for (k = 0; k < 3; k++) CHECK(fabs((a[k] - 0.5) + (b[k] - 0.5)) < 1e-15);
        CHECK(ctx_on(&cx, &cal_srgb, 0.5) == YCOL_OK);
        CHECK(ycol_lum_pair(&cx, 0, 10, 0.05, a, b) == YCOL_ERR_REFUSED);
    }
}

/* The context's contrast directions equal the raw calls' bit for bit. */
static void test_raw_parity(void) {
    const float bgf[3] = { 0.4f, 0.5f, 0.3f };
    ycol_ctx cx;
    ycol_ctx_desc d;
    char err[200];
    double m[9];
    int i, k, same = 1;
    g_where = "raw parity";
    memset(&d, 0, sizeof d);
    d.cal = &cal_full;
    for (k = 0; k < 3; k++) d.background[k] = bgf[k];
    CHECK(ctx_rc(&cx, &d, err, sizeof err) == YCOL_OK);
    CHECK(ycol_cal_dkl_matrix(&cal_full, bgf, m) == YCOL_OK);
    CHECK(memcmp(m, cx.dkl, sizeof m) == 0);
    for (i = 0; i < 200; i++) {
        float v[3], a[3], b[3];
        for (k = 0; k < 3; k++) v[k] = (float)(rnd() * 0.2 - 0.1);
        CHECK(ycol_cal_dir_cone(&cal_full, bgf, v, a) == 0);
        ycol_store3f(b, ycol_to_dir(&cx, ycol_make(YCOL_SPACE_CONE, v[0], v[1], v[2]), NULL));
        same &= memcmp(a, b, sizeof a) == 0;
        CHECK(ycol_cal_dir_dkl(&cal_full, bgf, v, a) == 0);
        ycol_store3f(b, ycol_to_dir(&cx, ycol_make(YCOL_SPACE_DKL_CART, v[0], v[1], v[2]), NULL));
        same &= memcmp(a, b, sizeof a) == 0;
    }
    printf("context cone and DKL directions against the raw calls: %s\n", same ? "bit for bit" : "DIFFERENT");
    CHECK(same);
}

static void test_refusals(void) {
    ycol_ctx cl, cb;
    ycol_gamut g;
    int s, st;
    g_where = "refusals";
    CHECK(ctx_on(&cl, &cal_lumo, 0.5) == YCOL_OK);
    CHECK(ctx_on(&cb, &cal_bm, 0.0) == YCOL_OK);
    for (s = 0; s < N_SPACES; s++) {
        int sp = ALL_SPACES[s], n;
        const ycol_space_info* si = ycol_spaces(&n);
        ycol_color c = ycol_make(sp, 0.1, 0.1, 0.1);
        ycol_to_rgb(&cl, c, &g);
        if (si[sp - 1].needs & YCOL_CAN_XYZ) CHECK(g.status == YCOL_ERR_REFUSED && strstr(g.why, "chromaticities") != NULL);
        else if (si[sp - 1].needs & YCOL_CAN_LMS) CHECK(g.status == YCOL_ERR_REFUSED && strstr(g.why, "spectra") != NULL);
        else CHECK(g.status == YCOL_OK);
        ycol_from_rgb(&cl, ycol_to_rgb(&cl, YCOL_RGB(0.2, 0.3, 0.4), NULL), sp, &st);
        CHECK(si[sp - 1].needs ? st == YCOL_ERR_REFUSED : st == YCOL_OK);
        ycol_to_rgb(&cb, c, &g);
        if (si[sp - 1].needs & YCOL_CAN_BG) CHECK(g.status == YCOL_ERR_REFUSED && strstr(g.why, "above 0") != NULL);
    }
    ycol_to_rgb(&cl, ycol_hex("#12345"), &g);
    CHECK(g.status == YCOL_ERR_REFUSED && strstr(g.why, "no space") != NULL);
    ycol_to_rgb(&cl, ycol_make(99, 0, 0, 0), &g);
    CHECK(g.status == YCOL_ERR_ARG);
    ycol_to_rgb(&cl, YCOL_DEVICE(1.5, 0, 0), &g);
    CHECK(g.status == YCOL_ERR_RANGE && strstr(g.why, "device") != NULL);
    CHECK(ctx_on(&cb, &cal_full, 0.5) == YCOL_OK);
    ycol_to_rgb(&cb, YCOL_MB(.l = 0.7, .s = 0.02, .lum = -1), &g);
    CHECK(g.status == YCOL_ERR_RANGE);
    ycol_to_rgb(&cb, YCOL_XYY(.x = 0.3, .y = 0, .Y = 10), &g);
    CHECK(g.status == YCOL_ERR_RANGE);
    ycol_from_rgb(&cb, ycol_to_rgb(&cb, YCOL_RGB(0, 0, 0), NULL), YCOL_SPACE_MB, &st);
    CHECK(st == YCOL_ERR_RANGE);   /* no light, no MacLeod-Boynton */
    CHECK(ycol_convert(NULL, YCOL_RGB(0, 0, 0), YCOL_SPACE_XYZ, NULL, &g) == YCOL_ERR_ARG);
}

static void test_values(void) {
    ycol_color c;
    char buf[256], hx[8];
    int n, i;
    const ycol_space_info* si = ycol_spaces(&n);
    const ycol_param* p;
    g_where = "values and tables";
    c = ycol_hex("#3366cc");
    CHECK(c.space == YCOL_SPACE_SRGB && c.u.rgb.r == 0x33 / 255.0 && c.u.rgb.b == 0xcc / 255.0);
    CHECK(ycol_hex_format(c, hx) == 0 && strcmp(hx, "#3366cc") == 0);
    c = ycol_hex("aBc");
    CHECK(c.space == YCOL_SPACE_SRGB && c.u.rgb.r == 10 / 15.0 && c.u.rgb.g == 11 / 15.0);
    CHECK(ycol_hex_format(c, hx) == 0 && strcmp(hx, "#aabbcc") == 0);
    CHECK(ycol_hex("#ggg").space == YCOL_SPACE_NONE && ycol_hex("#1234567").space == YCOL_SPACE_NONE && ycol_hex(NULL).space == 0);
    CHECK(ycol_hex_format(YCOL_SRGB(1.01, 0, 0), hx) == YCOL_ERR_RANGE);
    CHECK(ycol_hex_format(YCOL_RGB(1, 0, 0), hx) == YCOL_ERR_ARG);
    c = YCOL_DKL(.elev = 0, .azim = 90, .contrast = 0.1);
    CHECK(c.space == YCOL_SPACE_DKL && c.u.dkl.azim == 90 && c.u.dkl.contrast == 0.1 && c.flags == 0);
    CHECK(ycol_format(c, buf, sizeof buf) > 0 && strcmp(buf, "dkl(0 90 0.10000000000000001)") == 0);
    { volatile size_t small = 5; CHECK(ycol_format(c, buf, small) == YCOL_ERR_FULL); }
    c = YCOL_RGB(.r = 0.25, .g = 0.5);
    CHECK(c.u.rgb.r == 0.25 && c.u.rgb.g == 0.5 && c.u.rgb.b == 0);
    CHECK(n == YCOL_SPACE_COUNT - 1);
    for (i = 0; i < n; i++) {
        CHECK(si[i].space == i + 1);
        CHECK(ycol_space_from_name(si[i].name) == si[i].space);
    }
    CHECK(ycol_space_from_name("lab") == YCOL_SPACE_NONE && ycol_space_from_name("cielab") == YCOL_SPACE_CIELAB);
    p = ycol_desc_params(&n);
    CHECK(n == 9);
    for (i = 0; i < n; i++) CHECK(p[i].offset + sizeof(double) <= sizeof(ycol_ctx_desc) && p[i].min <= p[i].def && p[i].def <= p[i].max);
    CHECK(strcmp(ycol_strerror(YCOL_ERR_REFUSED), "refused: the calibration or context cannot support it") == 0);
    CHECK(strcmp(ycol_version(), YCOL_VERSION_STRING) == 0);
    CHECK(ycol_cal_describe(&cal_full, buf, sizeof buf) > 0 && strstr(buf, "NOMINAL") != NULL);
    {
        static ycol_cal cn;
        float r[81], g[81], b[81];
        char err[100];
        memcpy(&cn, &cal_srgb, sizeof cn);
        bm_spectra(r, g, b);
        CHECK(ycol_cal_set_spectra_nominal(&cn, 380, 5, 81, r, g, b, NULL) == 0 && (cn.flags & YCOL_CAL_SPECTRA_NOMINAL));
        CHECK(ycol_cal_derive(&cn, err, sizeof err) == 0);
        CHECK(ycol_cal_describe(&cn, buf, sizeof buf) > 0 && strstr(buf, "datasheet") != NULL);
    }
}

int main(void) {
    make_cals();
    test_cal_published();
    test_pinned();
    test_ctx();
    test_refs();
    test_roundtrip();
    test_gamut();
    test_map();
    test_device();
    test_lum();
    test_raw_parity();
    test_refusals();
    test_values();
    if (g_failures) {
        fprintf(stderr, "color_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("color_test: all checks passed\n");
    return 0;
}
