#include "trials_jitter_golden.h"

/* trials_test_jitter.h - the v0.2.1 part of trials_test.c (included
 * after trials_test_v02.h, whose helpers it uses): jitter. Each
 * distribution against its CDF by chi-square, the truncation and the frame
 * snapping exactly, the draw order against a log of the generator, replay
 * by ytr_restore() and by save/load, per-condition columns, the data
 * line, the rules text, and every refusal. The per-frame hazard of a
 * snapped exponential is measured and printed for the manual. */

#if YTR_MAX_JITTERS >= 4
/* A generator that logs every variate it hands out. */
typedef struct jt_log {
    uint64_t state;
    int      n;
    double   u[1 << 15];
} jt_log;

static jt_log g_jl;

static double jt_rng(void* ctx) {
    jt_log* l = (jt_log*)ctx;
    double u = ytr_splitmix(&l->state);
    if (l->n < (int)(sizeof(l->u) / sizeof(l->u[0]))) l->u[l->n] = u;
    l->n++;
    return u;
}
#endif

/* Chi-square of counts against expected values; bins with an expectation
 * below 5 are merged into the next. Returns the statistic, *df its df. */
static double jt_chi(const double* obs, const double* expv, int n, int* df) {
    double chi = 0.0, o = 0.0, e = 0.0;
    int i, bins = 0;
    for (i = 0; i < n; i++) {
        o += obs[i];
        e += expv[i];
        if (e >= 5.0 || i == n - 1) {
            if (e > 0.0) {
                chi += (o - e) * (o - e) / e;
                bins++;
            }
            o = e = 0.0;
        }
    }
    *df = bins - 1;
    return chi;
}

/* --------------------------------------------------- call level: values */

static void test_jitter_call(void) {
    static const double vals[4] = { 0.3, 0.5, 0.7, 0.5 };
    ytr_jitter_desc j;
    ytr_jitter_value v;
    char err[256];
    uint64_t st = 99;

    /* Refusals, each by name. */
    j = ytr_uniform("x", 1.2, 0.8);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    CHECK_HAS(err, "lo <= hi");
    j = ytr_uniform("x", -0.1, 0.8);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    j = ytr_uniform("x", 0.1, 2e6);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    j = ytr_exponential("x", 0.5, 2.0, 0.0);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    CHECK_HAS(err, "scale");
    j = ytr_choice("x", NULL, 2);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    CHECK_HAS(err, "CHOICE needs 1 to 32 values");
    j = ytr_choice("x", vals, 0);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    j = ytr_uniform("x", 0.8, 1.2);
    j.rate_den = 1;
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    CHECK_HAS(err, "rate_den needs rate_num");
    j = ytr_frames(ytr_uniform("x", 0.801, 0.81), 60, 1);
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    CHECK_HAS(err, "no whole frame of 60/1 Hz lies in [0.801, 0.81] s");
    j = ytr_uniform("x", 0.8, 1.2);
    j.dist = (ytr_jitter_dist)2;
    j.scale = 0.1;
    CHECK(ytr_jitter_check(&j, err, sizeof(err)));
    j.lo_column = "lo";
    CHECK(!ytr_jitter_check(&j, err, sizeof(err)));
    CHECK_HAS(err, "columns need a session");
    v = ytr_jitter_map(&j, 0.5);
    CHECK(v.s != v.s && v.ns == -1 && v.frames == -1);
    CHECK(!ytr_jitter_check(NULL, err, sizeof(err)));

    /* Values at fixed variates. */
    j = ytr_uniform("x", 0.8, 1.2);
    v = ytr_jitter_map(&j, 0.0);
    CHECK(v.s == 0.8 && v.ns == 800000000 && v.frames == -1);
    v = ytr_jitter_map(&j, 0.5);
    CHECK_I(v.ns, 1000000000);
    v = ytr_jitter_map(&j, 1.5);              /* clamped */
    CHECK(v.s == 1.2);
    v = ytr_jitter_map(&j, -3.0);
    CHECK(v.s == 0.8);
    j = ytr_choice("x", vals, 4);
    CHECK(ytr_jitter_map(&j, 0.0).s == 0.3 && ytr_jitter_map(&j, 0.26).s == 0.5 &&
          ytr_jitter_map(&j, 0.74).s == 0.7 && ytr_jitter_map(&j, 0.9999).s == 0.5 &&
          ytr_jitter_map(&j, 0.5).s == 0.7);
    j = ytr_exponential("x", 0.5, 2.0, 0.4);
    CHECK(ytr_jitter_map(&j, 0.0).s == 0.5);
    v = ytr_jitter_map(&j, 0.999999999);       /* folded into [lo, hi) */
    CHECK(v.s >= 0.5 && v.s < 2.0);
    /* E = 0.4 (-ln(1 - u)) for u = 0.9 is 0.921034037 s: lo + E. */
    v = ytr_jitter_map(&j, 0.9);
    CHECK(v.ns >= 1421034036 && v.ns <= 1421034038);
    /* 60000/1001 Hz: frame k lasts 1001/60000 s; 0.8 s is frame 47.95 so
     * the first frame inside is 48 (0.8008 s), the last 71 (1.184517 s). */
    j = ytr_frames(ytr_uniform("x", 0.8, 1.2), 60000, 1001);
    v = ytr_jitter_map(&j, 0.0);
    CHECK_I(v.frames, 48);
    CHECK_I(v.ns, 800800000);
    v = ytr_jitter_map(&j, 0.999);
    CHECK_I(v.frames, 71);
    CHECK_I(v.ns, 1184516667);                  /* 71 x 1001 / 60000 s, rounded */
    /* An end that is itself a frame counts as inside, though in binary
     * 0.07 x 100 is 7.000000000000001 and 0.29 x 100 is 28.999999999999996. */
    j = ytr_frames(ytr_uniform("x", 0.8, 1.2), 60, 0);
    CHECK_I(ytr_jitter_map(&j, 0.0).frames, 48);
    CHECK_I(ytr_jitter_map(&j, 0.9999).frames, 72);
    j = ytr_frames(ytr_uniform("x", 0.07, 0.29), 100, 1);
    CHECK_I(ytr_jitter_map(&j, 0.0).frames, 7);
    CHECK_I(ytr_jitter_map(&j, 0.99999).frames, 29);
    j = ytr_frames(ytr_choice("x", vals, 4), 60, 1);
    CHECK_I(ytr_jitter_map(&j, 0.0).frames, 18);
    CHECK_I(ytr_jitter_map(&j, 0.5).frames, 42);
    /* draw = one rng call, then map. */
    j = ytr_uniform("x", 0.8, 1.2);
    st = 7;
    v = ytr_jitter_draw(&j, ytr_splitmix, &st);
    {
        uint64_t s2 = 7;
        double u = ytr_splitmix(&s2);
        CHECK(v.s == ytr_jitter_map(&j, u).s);
        CHECK(st == s2);
    }
    CHECK(ytr_jitter_draw(&j, NULL, &st).ns == -1);
}

/* ----------------------------------------- the fixed-point arithmetic */

/* -ln(y / 2^53) in Q58 at 123 points, rounded from mpmath at 300 bits by
 * tests/compare/trials_jitter_ref.py points. */
static const struct { uint64_t y, f; } jt_log_ref[123] = {
    { 1ULL, 10588661846808449218ULL },
    { 2ULL, 10388875774227157724ULL },
    { 3ULL, 10272008413600747036ULL },
    { 4ULL, 10189089701645866229ULL },
    { 5ULL, 10124772951914742760ULL },
    { 7ULL, 10027791432588802797ULL },
    { 67108864ULL, 5394223959694870356ULL },
    { 4503599627370495ULL, 199786072581291559ULL },
    { 4503599627370496ULL, 199786072581291495ULL },
    { 4503599627370497ULL, 199786072581291431ULL },
    { 9007199254740990ULL, 64ULL },
    { 9007199254740991ULL, 32ULL },
    { 9007199254740992ULL, 0ULL },
    { 6389235553277986ULL, 98981062403888977ULL },
    { 5247015844214380ULL, 155749512207126737ULL },
    { 5045126719322906ULL, 167058730540779763ULL },
    { 1834057343384190ULL, 458716780115339885ULL },
    { 1858955568418470ULL, 454830233452949886ULL },
    { 103225219118641ULL, 1288063002875387843ULL },
    { 3430947144671368ULL, 278196447330246569ULL },
    { 3171623608235946ULL, 300849257301446001ULL },
    { 5685132717758645ULL, 132634886469474693ULL },
    { 3550171412489833ULL, 268350633438026079ULL },
    { 6531665938802835ULL, 92626331420959778ULL },
    { 8708164734762874ULL, 9731555030507203ULL },
    { 4225968183611692ULL, 218125768978517001ULL },
    { 4777658645809109ULL, 182759263968857553ULL },
    { 852575311892273ULL, 679508272858795494ULL },
    { 872166373219268ULL, 672960066948909858ULL },
    { 1569693415184852ULL, 503579929488113000ULL },
    { 303508330434670ULL, 977207734456472542ULL },
    { 660767782519921ULL, 752966401900400595ULL },
    { 474829916341594ULL, 848210918180868842ULL },
    { 2065795322148402ULL, 424421787966985705ULL },
    { 868190224751888ULL, 674277094463560026ULL },
    { 5821449764615295ULL, 125805308349878885ULL },
    { 954460396200253ULL, 646971501279299935ULL },
    { 3466315464101482ULL, 275240401157826017ULL },
    { 2369913111059658ULL, 384836805629717305ULL },
    { 7028093565982379ULL, 71512461395275213ULL },
    { 572756961230766ULL, 794166337343681848ULL },
    { 8328809033859306ULL, 22569505462920577ULL },
    { 6258215230134364ULL, 104953078215359592ULL },
    { 1093515695567864ULL, 607770140146721099ULL },
    { 7334098105851055ULL, 59228381698276452ULL },
    { 6272290115916669ULL, 104305568603027632ULL },
    { 4187519560680979ULL, 220760143338540817ULL },
    { 8544474367291348ULL, 15201089483088195ULL },
    { 5339293729199256ULL, 150724538588669626ULL },
    { 7218241828042912ULL, 63817884724498824ULL },
    { 1350597201717713ULL, 546910598365645514ULL },
    { 7674902785887453ULL, 46136617715720705ULL },
    { 862338426481536ULL, 676226409135992715ULL },
    { 1ULL, 10588661846808449218ULL },
    { 2485453ULL, 6344191273084906702ULL },
    { 2352219493ULL, 4369046593048309945ULL },
    { 756377019317ULL, 2705040575062604947ULL },
    { 228679ULL, 7031877516816921214ULL },
    { 1ULL, 10588661846808449218ULL },
    { 292776ULL, 6960659094551647729ULL },
    { 632ULL, 8729896030972716047ULL },
    { 10ULL, 9924986879333451265ULL },
    { 773071122455236ULL, 707723287038625630ULL },
    { 4141610335069462ULL, 223937562238072206ULL },
    { 57723113ULL, 5437648307566564042ULL },
    { 12015778946ULL, 3898982812490861009ULL },
    { 352102ULL, 6907476815822492326ULL },
    { 68860108ULL, 5386798888762458617ULL },
    { 5ULL, 10124772951914742760ULL },
    { 6973508874904811ULL, 73759782130416660ULL },
    { 36899ULL, 7557648583017525323ULL },
    { 229ULL, 9022498109820177946ULL },
    { 813ULL, 8657307598612006979ULL },
    { 479842243ULL, 4827234096436515824ULL },
    { 614002791923ULL, 2765148275890481547ULL },
    { 13ULL, 9849365528697219139ULL },
    { 97ULL, 9270091180689053889ULL },
    { 263715647286ULL, 3008739850540813060ULL },
    { 2658ULL, 8315870434092646463ULL },
    { 13032702653555ULL, 1884541760371148039ULL },
    { 75730133218693ULL, 1377338629952681939ULL },
    { 614463085ULL, 4755957184602626543ULL },
    { 36145279167156ULL, 1590522403198674217ULL },
    { 11727ULL, 7888044346608568706ULL },
    { 31661934180959ULL, 1628693099261734366ULL },
    { 204922565ULL, 5072467767731880930ULL },
    { 8251661ULL, 5998326472920799163ULL },
    { 29029453ULL, 5635762514086099972ULL },
    { 385ULL, 8872756281244010551ULL },
    { 79214530612ULL, 3355397890392875421ULL },
    { 1450392994065202ULL, 526363322441903546ULL },
    { 46846134525229ULL, 1515777882977160979ULL },
    { 2178578520166541ULL, 409100235648788503ULL },
    { 4538783999459328ULL, 197543023314412548ULL },
    { 4538783999459327ULL, 197543023314412612ULL },
    { 4855443348258816ULL, 178104397582562980ULL },
    { 4855443348258815ULL, 178104397582563040ULL },
    { 5172102697058304ULL, 159894319003336933ULL },
    { 5172102697058303ULL, 159894319003336989ULL },
    { 5488762045857792ULL, 142766684168816707ULL },
    { 5488762045857791ULL, 142766684168816760ULL },
    { 5805421394657280ULL, 126599996097837529ULL },
    { 5805421394657279ULL, 126599996097837579ULL },
    { 6122080743456768ULL, 111292131170246474ULL },
    { 6122080743456767ULL, 111292131170246521ULL },
    { 6438740092256256ULL, 96756427259715502ULL },
    { 6438740092256255ULL, 96756427259715547ULL },
    { 6755399441055744ULL, 82918711954880808ULL },
    { 6755399441055743ULL, 82918711954880850ULL },
    { 7072058789855232ULL, 69715012153245487ULL },
    { 7072058789855231ULL, 69715012153245528ULL },
    { 7388718138654720ULL, 57089765747985401ULL },
    { 7388718138654719ULL, 57089765747985440ULL },
    { 7705377487454208ULL, 44994408856766044ULL },
    { 7705377487454207ULL, 44994408856766082ULL },
    { 8022036836253696ULL, 33386247759141513ULL },
    { 8022036836253695ULL, 33386247759141549ULL },
    { 8338696185053184ULL, 22227549350771089ULL },
    { 8338696185053183ULL, 22227549350771123ULL },
    { 8655355533852672ULL, 11484801208666067ULL },
    { 8655355533852671ULL, 11484801208666100ULL },
    { 8972014882652160ULL, 1128104673551053ULL },
    { 8972014882652159ULL, 1128104673551085ULL },
};

static void test_jitter_fixed(void) {
    size_t i;
    int64_t worst = 0;
    for (i = 0; i < sizeof(jt_log_ref) / sizeof(jt_log_ref[0]); i++) {
        int64_t d = (int64_t)(ytr__neglog(jt_log_ref[i].y) - jt_log_ref[i].f);
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    /* Measured 0.71 units of 2^-58 against the exact value; the reference
     * is rounded, so 1 unit. */
    CHECK(worst <= 1);
    CHECK(ytr__neglog((uint64_t)1 << 53) == 0);
    /* Seconds to ns from the double's bits, nearest, ties up. */
    CHECK_I(ytr__s_to_ns(0.8), 800000000);
    CHECK_I(ytr__s_to_ns(1.2), 1200000000);
    CHECK_I(ytr__s_to_ns(1e-9), 1);
    CHECK_I(ytr__s_to_ns(0.0), 0);
    CHECK_I(ytr__s_to_ns(-0.0), 0);
    CHECK(ytr__s_to_ns(1e6) == 1000000000000000LL);
    CHECK(ytr__s_to_ns(1.5e-9) == 1);             /* the double is 1.49999999999999999 ns */
    CHECK(ytr__s_to_ns(2.5e-9) == 3);             /* and this one 2.50000000000000005 */
    CHECK(ytr__s_to_ns(4.9406564584124654e-324) == 0);
    /* 53 bits of the variate; frames to ns; the frames inside [lo, hi]. */
    CHECK(ytr__u53(0.5) == ((uint64_t)1 << 52));
    CHECK(ytr__u53(1.0) == ((uint64_t)1 << 53) - 1u);
    CHECK(ytr__u53(-1.0) == 0);
    CHECK_I(ytr__frame_ns(71, 60000, 1001), 1184516667);
    CHECK_I(ytr__frame_ns(1, 3, 1), 333333333);
    CHECK_I(ytr__frame_ns(2, 3, 1), 666666667);
    {
        int64_t klo, khi;
        ytr__frames_in(70000000, 290000000, 100, 1, &klo, &khi);
        CHECK(klo == 7 && khi == 29);
        ytr__frames_in(800000000, 1200000000, 60000, 1001, &klo, &khi);
        CHECK(klo == 48 && khi == 71);
        ytr__frames_in(0, 0, 60, 1, &klo, &khi);
        CHECK(klo == 0 && khi == 0);
        /* frame 1 of 3 Hz is 333333333 ns: inside [333333333, 333333333] */
        ytr__frames_in(333333333, 333333333, 3, 1, &klo, &khi);
        CHECK(klo == 1 && khi == 1);
        ytr__frames_in(333333334, 666666666, 3, 1, &klo, &khi);
        CHECK(klo > khi);
        /* 2e9 Hz: frame 1 is at 0.5 ns, which rounds to 1 ns: outside [0, 0] */
        ytr__frames_in(0, 0, 2000000000, 1, &klo, &khi);
        CHECK(klo == 0 && khi == 0);
    }
    printf("  jitter fixed point: -ln(1 - u) within %d unit of 2^-58 at %d mpmath points; ns, frames exact\n",
           (int)worst, (int)(sizeof(jt_log_ref) / sizeof(jt_log_ref[0])));
}

/* ------------------------------------------- call level: distributions */

static void test_jitter_dist(void) {
    enum { N = 200000, B = 20 };
    static double obs[200], ex[200];
    ytr_jitter_desc j;
    ytr_jitter_value v;
    uint64_t st = 12345;
    double chi, mean, lo = 0.5, hi = 2.0, sc = 0.4, c, worst = 0.0;
    int i, k, df, nk;
    int64_t klo, khi;

    /* Continuous uniform: 20 equal bins. */
    j = ytr_uniform("u", 0.8, 1.2);
    memset(obs, 0, sizeof(obs));
    for (i = 0; i < N; i++) {
        v = ytr_jitter_draw(&j, ytr_splitmix, &st);
        CHECK(v.s >= 0.8 && v.s <= 1.2);
        k = (int)((v.s - 0.8) / 0.4 * B);
        if (k > B - 1) k = B - 1;
        obs[k]++;
    }
    for (k = 0; k < B; k++) ex[k] = (double)N / B;
    chi = jt_chi(obs, ex, B, &df);
    CHECK(chi < chi_crit(df));
    if (chi / chi_crit(df) > worst) worst = chi / chi_crit(df);

    /* Truncated exponential: 20 equal-width bins against the CDF
     * F(x) = (1 - e^-(x-lo)/s) / (1 - e^-(hi-lo)/s); the mean against
     * lo + s - (hi-lo) e^-(hi-lo)/s / (1 - e^-(hi-lo)/s). */
    j = ytr_exponential("e", lo, hi, sc);
    memset(obs, 0, sizeof(obs));
    mean = 0.0;
    for (i = 0; i < N; i++) {
        v = ytr_jitter_draw(&j, ytr_splitmix, &st);
        CHECK(v.s >= lo && v.s <= hi);
        mean += v.s;
        k = (int)((v.s - lo) / (hi - lo) * B);
        if (k > B - 1) k = B - 1;
        obs[k]++;
    }
    c = 1.0 - exp(-(hi - lo) / sc);
    for (k = 0; k < B; k++) {
        double a = lo + (hi - lo) * k / B, b = lo + (hi - lo) * (k + 1) / B;
        ex[k] = N * (exp(-(a - lo) / sc) - exp(-(b - lo) / sc)) / c;
    }
    chi = jt_chi(obs, ex, B, &df);
    CHECK(chi < chi_crit(df));
    if (chi / chi_crit(df) > worst) worst = chi / chi_crit(df);
    mean /= N;
    {
        double want = lo + sc - (hi - lo) * exp(-(hi - lo) / sc) / c;
        /* SD of the truncated exponential is below s; 5 standard errors. */
        CHECK(fabs(mean - want) < 5.0 * sc / sqrt((double)N));
    }

    /* Choice with a repeated value: 0.5 is twice as likely. */
    {
        static const double vals[4] = { 0.3, 0.5, 0.7, 0.5 };
        j = ytr_choice("c", vals, 4);
        memset(obs, 0, sizeof(obs));
        for (i = 0; i < N; i++) {
            v = ytr_jitter_draw(&j, ytr_splitmix, &st);
            obs[v.s == 0.3 ? 0 : v.s == 0.5 ? 1 : 2]++;
        }
        ex[0] = N * 0.25;
        ex[1] = N * 0.5;
        ex[2] = N * 0.25;
        chi = jt_chi(obs, ex, 3, &df);
        CHECK(chi < chi_crit(df));
        if (chi / chi_crit(df) > worst) worst = chi / chi_crit(df);
    }

    /* Snapped uniform at 60000/1001 Hz: every frame from 48 to 71 equally
     * likely, and ns exactly k x 1001e9 / 60000 rounded. */
    j = ytr_frames(ytr_uniform("f", 0.8, 1.2), 60000, 1001);
    memset(obs, 0, sizeof(obs));
    for (i = 0; i < N; i++) {
        v = ytr_jitter_draw(&j, ytr_splitmix, &st);
        CHECK(v.frames >= 48 && v.frames <= 71);
        if (v.frames < 48 || v.frames > 71) break;
        obs[v.frames - 48]++;
        {
            int64_t num = v.frames * 1001 * 1000000000LL, d = v.ns * 60000 - num;
            CHECK(d > -30000 && d <= 30000);
        }
    }
    for (k = 0; k < 24; k++) ex[k] = (double)N / 24;
    chi = jt_chi(obs, ex, 24, &df);
    CHECK(chi < chi_crit(df));
    if (chi / chi_crit(df) > worst) worst = chi / chi_crit(df);

    /* Snapped exponential at 60 Hz, 0.5 to 2.0 s, scale 0.4: frames 30 to
     * 120, a truncated geometric with q = e^-(1/60)/0.4. */
    j = ytr_frames(ytr_exponential("g", lo, hi, sc), 60, 1);
    klo = 30;
    khi = 120;
    nk = (int)(khi - klo + 1);
    memset(obs, 0, sizeof(obs));
    for (i = 0; i < 1000000; i++) {
        v = ytr_jitter_draw(&j, ytr_splitmix, &st);
        CHECK(v.frames >= klo && v.frames <= khi);
        if (v.frames < klo || v.frames > khi) break;
        obs[v.frames - klo]++;
    }
    {
        double q = exp(-1.0 / (60.0 * sc)), norm = 1.0 - pow(q, nk), h0 = 1.0 - q, surv, dev3 = 0.0, devg = 0.0;
        double end = 0.0;
        for (k = 0; k < nk; k++) ex[k] = 1000000.0 * pow(q, k) * (1.0 - q) / norm;
        chi = jt_chi(obs, ex, nk, &df);
        CHECK(chi < chi_crit(df));
        if (chi / chi_crit(df) > worst) worst = chi / chi_crit(df);
        /* Measured hazard per frame, count(k) / count(>= k): against flat,
         * the non-aging 1 - e^-(T/s) per frame of T, up to 3 scales before
         * hi, where the truncation's own factor 1 / (1 - e^-(hi - t)/s)
         * is still below e^-3 = 5 %; and against the truncated geometric's
         * exact hazard (1 - q) / (1 - q^(frames left)) wherever at least
         * 20000 draws survive, which is sampling noise only. */
        surv = 1000000.0;
        for (k = 0; k < nk && surv >= 20000.0; k++) {
            double h = obs[k] / surv, t_k = (double)(klo + k) / 60.0;
            double hg = (1.0 - q) / (1.0 - pow(q, nk - k));
            if (t_k <= hi - 3.0 * sc && fabs(h / h0 - 1.0) > dev3) dev3 = fabs(h / h0 - 1.0);
            if (fabs(h / hg - 1.0) > devg) devg = fabs(h / hg - 1.0);
            end = t_k;
            surv -= obs[k];
        }
        CHECK(dev3 < 0.08);
        CHECK(devg < 0.05);
        printf("  jitter hazard, exponential 0.5 to 2.0 s, scale 0.4, 60 Hz, 1e6 draws: per-frame hazard "
               "within %.1f%% of flat up to 0.8 s (hi - 3 scale); within %.1f%% of the truncated "
               "geometric's up to %.2f s; flat per-frame hazard x 60 = %.4f/s against 1/scale = %.4f/s\n",
               100.0 * dev3, 100.0 * devg, end, h0 * 60.0, 1.0 / sc);
    }

    /* Snapping never leaves [lo, hi]: random intervals and rates. */
    for (i = 0; i < 20000; i++) {
        int num = 1 + (int)(ytr_splitmix(&st) * 240000.0), den = 1 + (int)(ytr_splitmix(&st) * 1001.0);
        double a = ytr_splitmix(&st) * 3.0, b = a + ytr_splitmix(&st) * 2.0;
        char e2[160];
        j = ytr_frames(ytr_uniform("r", a, b), num, den);
        if (i % 2) {
            j.dist = YTR_JITTER_EXPONENTIAL;
            j.scale = 0.05 + ytr_splitmix(&st);
        }
        if (!ytr_jitter_check(&j, e2, sizeof(e2))) {
            CHECK_HAS(e2, "no whole frame");
            continue;
        }
        v = ytr_jitter_draw(&j, ytr_splitmix, &st);
        /* Within a nanosecond of the interval, the tolerance of 1e-9 frames. */
        CHECK(v.s >= a - 1e-9 && v.s <= b + 1e-9);
        CHECK(v.ns >= (int64_t)floor(a * 1e9) - 1 && v.ns <= (int64_t)ceil(b * 1e9) + 1);
        v = ytr_jitter_map(&j, 0.0);
        CHECK(v.s >= a - 1e-9);
        v = ytr_jitter_map(&j, 0.99999999);
        CHECK(v.s <= b + 1e-9);
    }
    printf("  jitter distributions: uniform, truncated exponential, choice, snapped uniform and "
           "exponential against their CDFs (worst chi2 at %.2f of its bound); 20000 random snaps "
           "inside their intervals\n", worst);
    /* Golden digests: the draws are integer arithmetic, so the same on
     * every C library, compiler and flag set. */
    CHECK_I(jg_check(0), 0);
}

/* The session tests use three jitters; a build with fewer runs the call
 * level only. */
#if YTR_MAX_JITTERS >= 4

/* ------------------------------------------------------- session level */

static const char jt_csv[] =
    "target,fp_lo,fp_hi\na,0.5,0.7\nb,1.0,1.5\nc,2.0,2.0\n";

static void jt_desc(ytr_desc* d, bool with) {
    static const double vals[3] = { 0.1, 0.2, 0.4 };
    zero_desc(d);
    d->n_conditions = 4;
    d->reps = 5;
    d->order = YTR_ORDER_FULL_RANDOM;
    d->n_practice = 2;
    d->requeue_gap = 2;
    d->rng = jt_rng;
    d->rng_ctx = &g_jl;
    if (with) {
        d->jitters[0] = ytr_frames(ytr_uniform("iti", 0.8, 1.2), 60000, 1001);
        d->jitters[1] = ytr_exponential("fp", 0.5, 2.0, 0.4);
        d->jitters[2] = ytr_choice("soa", vals, 3);
        d->n_jitters = 3;
    }
}

static void test_jitter_session(void) {
    static int outc[200];
    static double raw[200][3];
    static char line[512], line2[512];
    ytr_desc d, d0;
    ytr_trial_info ti = ti_zero;
    ytr_jitter_value v;
    int i, k, n, rc, n0, before, nrun;
    static int consumed[200], consumed0[200];
    static unsigned char snap[1 << 16];

    /* Open draws nothing for jitters: the schedule is the one without. */
    jt_desc(&d0, false);
    g_jl.state = 4242;
    g_jl.n = 0;
    CHECK(ytr_open(&g_u, &d0));
    n0 = g_jl.n;
    jt_desc(&d, true);
    g_jl.state = 4242;
    g_jl.n = 0;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(g_jl.n, n0);
    CHECK_I(ytr_n_scheduled(&g_t), ytr_n_scheduled(&g_u));
    for (i = 0; i < ytr_n_scheduled(&g_t); i++) CHECK_I(ytr_condition_at(&g_t, i), ytr_condition_at(&g_u, i));

    /* Draw order: each new trial takes its own draws, then one variate per
     * jitter in jitter order; the pending trial takes none. */
    for (i = 0; i < 200; i++) {
        before = g_jl.n;
        rc = ytr_next(&g_t, &ti);
        if (rc < 0) break;
        consumed[i] = g_jl.n - before;
        CHECK(consumed[i] >= 3);
        for (k = 0; k < 3; k++) {
            ytr_jitter_value w = ytr_jitter_map(&d.jitters[k], g_jl.u[g_jl.n - 3 + k]);
            v = ytr_jitter(&g_t, ti.index, k);
            CHECK(v.ns == w.ns && v.frames == w.frames);
            raw[i][k] = v.s;
        }
        before = g_jl.n;
        CHECK_I(ytr_next(&g_t, &ti), rc);
        CHECK_I(g_jl.n, before);
        if (i % 7 == 3 && !ti.practice) {
            outc[i] = YTR_REQUEUE;
            CHECK_I(ytr_requeue(&g_t), 0);
        } else {
            outc[i] = i % 3;
            CHECK_I(ytr_update(&g_t, outc[i], NULL), 0);
        }
    }
    nrun = i;
    /* The same session without jitters: the trial draws match one for one,
     * so jitters only append. */
    for (i = 0; i < nrun; i++) {
        before = g_jl.n;
        rc = ytr_next(&g_u, &ti);
        if (rc < 0) break;
        consumed0[i] = g_jl.n - before;
        if (outc[i] == YTR_REQUEUE) ytr_requeue(&g_u);
        else ytr_update(&g_u, outc[i], NULL);
    }
    CHECK_I(i, nrun);
    for (i = 0; i < nrun; i++) CHECK_I(consumed[i], consumed0[i] + 3);
    /* Bounds and snapping in the session. */
    for (i = 0; i < nrun; i++) {
        v = ytr_jitter(&g_t, i, 0);
        CHECK(v.frames >= 48 && v.frames <= 71);
        v = ytr_jitter(&g_t, i, 1);
        CHECK(v.s >= 0.5 && v.s <= 2.0 && v.frames == -1);
        v = ytr_jitter(&g_t, i, 2);
        CHECK(v.s == 0.1 || v.s == 0.2 || v.s == 0.4);
    }
    CHECK(ytr_jitter(&g_t, nrun, 0).ns == -1);
    CHECK(ytr_jitter(&g_t, 0, 3).ns == -1);
    CHECK(ytr_jitter(&g_t, -1, 0).ns == -1);
    CHECK_I(ytr_jitter_index(&g_t, "fp"), 1);
    CHECK_I(ytr_jitter_index(&g_t, "nope"), -1);

    /* The data line: one column per jitter, seconds to the nanosecond. */
    ytr_format_header(&g_t, line, sizeof(line));
    CHECK_HAS(line, ",iti,fp,soa\n");
    ytr_format_row(&g_t, 5, line, sizeof(line));
    {
        char want[96];
        v = ytr_jitter(&g_t, 5, 0);
        snprintf(want, sizeof(want), ",%lld.%09lld,", (long long)(v.ns / 1000000000), (long long)(v.ns % 1000000000));
        CHECK_HAS(line, want);
    }
    ytr_format_meta(&g_t, line, sizeof(line));
    CHECK_HAS(line, " jitters=iti:uniform:0.8:1.2:rate=60000/1001;fp:exponential:0.5:2:0.4;soa:choice:0.1:0.2:0.4\n");
    ytr_format_rules(&g_t, line, sizeof(line));
    CHECK_HAS(line, "\njitter iti uniform 0.8 1.2 rate=60000/1001\n"
                    "jitter fp exponential 0.5 2 0.4\n"
                    "jitter soa choice 0.1 0.2 0.4\n");

    /* Replay: a fresh open and restore() give the same durations. */
    jt_desc(&d, true);
    g_jl.state = 4242;
    g_jl.n = 0;
    CHECK(ytr_open(&g_u, &d));
    CHECK_I(ytr_restore(&g_u, outc, NULL, nrun), 0);
    for (i = 0; i < nrun; i++)
        for (k = 0; k < 3; k++) CHECK(ytr_jitter(&g_u, i, k).s == raw[i][k]);
    for (i = 0; i < nrun; i++) {
        ytr_format_row(&g_t, i, line, sizeof(line));
        ytr_format_row(&g_u, i, line2, sizeof(line2));
        CHECK_S(line2, line);
    }

    /* Save and load at cut points: format 3, and the loaded session goes
     * on drawing what the uninterrupted one draws. */
    for (k = 3; k < nrun; k += 9) {
        uint64_t st_at;
        jt_desc(&d, true);
        g_jl.state = 4242;
        g_jl.n = 0;
        CHECK(ytr_open(&g_u, &d));
        CHECK_I(ytr_restore(&g_u, outc, NULL, k), 0);
        n = ytr_save(&g_u, snap, sizeof(snap));
        CHECK(n > 8);
        if (n <= 8) break;
        CHECK_I(snap[4], 3);
        st_at = g_jl.state;
        jt_desc(&d, true);
        memset(&g_u, 0x5A, sizeof(g_u));
        g_jl.state = st_at;
        CHECK(ytr_open_saved(&g_u, &d, snap, (size_t)n));
        for (i = k; i < nrun; i++) {
            rc = ytr_next(&g_u, NULL);
            if (rc < 0) { CHECK(false); break; }
            if (outc[i] == YTR_REQUEUE) ytr_requeue(&g_u);
            else ytr_update(&g_u, outc[i], NULL);
        }
        for (i = 0; i < nrun; i++) {
            int jj;
            for (jj = 0; jj < 3; jj++) CHECK(ytr_jitter(&g_u, i, jj).s == raw[i][jj]);
        }
    }
    /* A changed jitter refuses the snapshot by name. */
    jt_desc(&d, true);
    g_jl.state = 4242;
    CHECK(ytr_open(&g_u, &d));
    n = ytr_save(&g_u, snap, sizeof(snap));
    d.jitters[1].scale = 0.5;
    CHECK(!ytr_open_saved(&g_u, &d, snap, (size_t)n));
    CHECK_HAS(ytr_error(&g_u), "desc.jitters[].scale does not match");
    d.jitters[1].scale = 0.4;
    d.n_jitters = 2;
    CHECK(!ytr_open_saved(&g_u, &d, snap, (size_t)n));
    d.n_jitters = 0;
    CHECK(!ytr_open_saved(&g_u, &d, snap, (size_t)n));
    CHECK_HAS(ytr_error(&g_u), "snapshot format is not 1");
    printf("  jitter session: open draws unchanged, %d trials each +3 variates after their own, "
           "restore and %d save/load cut points identical, format 3\n", nrun, (nrun - 3 + 8) / 9);
}

static void test_jitter_columns(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    ytr_jitter_value v;
    int i, rc;
    if (!csv_table(&g_tab, jt_csv)) { CHECK(false); return; }
    zero_desc(&d);
    d.table = &g_tab;
    d.reps = 10;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.rng = ytr_splitmix;
    g_rng_state = 5;
    d.rng_ctx = &g_rng_state;
    d.jitters[0] = ytr_uniform("fp", 0.0, 0.0);
    d.jitters[0].lo_column = "fp_lo";
    d.jitters[0].hi_column = "fp_hi";
    d.n_jitters = 1;
    CHECK(ytr_open(&g_t, &d));
    for (i = 0; (rc = ytr_next(&g_t, &ti)) >= 0; i++) {
        v = ytr_jitter(&g_t, ti.index, 0);
        CHECK(v.s >= ytb_num(&g_tab, ti.condition, 1) && v.s <= ytb_num(&g_tab, ti.condition, 2));
        if (ti.condition == 2) {
            static char ln[256];
            CHECK(v.s == 2.0);
            /* The nanoseconds zero-padded: 2 s is 2.000000000, not 2.0. */
            CHECK(ytr_format_row(&g_t, ti.index, ln, sizeof(ln)) > 0);
            CHECK_HAS(ln, ",2.000000000\n");
        }
        ytr_update(&g_t, 1, NULL);
    }
    CHECK_I(i, 30);
    /* Refusals: a missing or string column, a bad row, columns on CHOICE,
     * scale_column on UNIFORM, no table. */
    d.jitters[0].hi_column = "nope";
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.jitters[0] (fp): the table has no column 'nope'");
    d.jitters[0].hi_column = "target";
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "column 'target' is not numeric");
    d.jitters[0].hi_column = "fp_lo";
    d.jitters[0].lo_column = "fp_hi";
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.jitters[0] (fp), row 0: needs 0 <= lo <= hi");
    d.jitters[0].lo_column = "fp_lo";
    d.jitters[0].hi_column = "fp_hi";
    d.jitters[0].scale_column = "fp_lo";
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "scale_column needs an EXPONENTIAL jitter");
    d.jitters[0].scale_column = NULL;
    d.jitters[0].dist = YTR_JITTER_CHOICE;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "a CHOICE jitter takes no columns");
    d.jitters[0].dist = YTR_JITTER_UNIFORM;
    d.jitters[0] = ytr_frames(d.jitters[0], 60, 1);
    CHECK(ytr_open(&g_t, &d));
    d.table = NULL;
    d.n_conditions = 3;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "a column needs desc.table");
    /* Desc-level refusals. */
    zero_desc(&d);
    d.n_conditions = 2;
    d.reps = 2;
    d.jitters[0] = ytr_uniform("a b", 0.1, 0.2);
    d.n_jitters = 1;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.jitters[0].name must be");
    d.jitters[0] = ytr_uniform("ab", 0.1, 0.2);
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.rng is required for jitter draws");
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    d.jitters[1] = ytr_uniform("ab", 0.1, 0.2);
    d.n_jitters = 2;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.jitters[0] and [1] are both named 'ab'");
    d.n_jitters = YTR_MAX_JITTERS + 1;
    CHECK(!ytr_open(&g_t, &d));
    d.n_jitters = -1;
    CHECK(!ytr_open(&g_t, &d));
    d.n_jitters = 1;
    d.jitters[0] = ytr_frames(ytr_uniform("ab", 0.801, 0.81), 60, 1);
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.jitters[0] (ab): no whole frame of 60/1 Hz");
    printf("  jitter columns: per-row intervals from the table, 9 refusals by name\n");
}

static void test_jitter_rules(void) {
    static uint64_t arena[(1u << 16) / 8];
    static char out[2048], out2[2048];
    static const char* const bad[][2] = {
        { "jitter iti\n", "expected: jitter NAME" },
        { "jitter iti gaussian 1 2\n", "expected uniform, choice or exponential" },
        { "jitter iti uniform 1\n", "expected: jitter NAME uniform LO HI" },
        { "jitter iti exponential 1 2\n", "expected: jitter NAME exponential LO HI SCALE" },
        { "jitter iti uniform 1 2 rate=0\n", "expected rate=NUM or rate=NUM/DEN" },
        { "jitter iti uniform 1 2 rate=60/x\n", "expected rate=NUM or rate=NUM/DEN" },
        { "jitter iti choice 0.1 fp_lo\n", "'fp_lo' is not a number of seconds" },
        { "jitter iti uniform nope 2\n", "unknown column 'nope'" },
        { "jitter \"a b\" uniform 1 2\n", "a jitter's name is a word" },
        { "jiter iti uniform 1 2\n", "did you mean 'jitter'" },
        { "iti 1 2\n", "the statement here is 'jitter iti ...'" },
    };
    ytr_desc d, d2;
    ytr_rules_desc rd;
    char err[256];
    int i, k, n;
    if (!csv_table(&g_tab, jt_csv)) { CHECK(false); return; }
    zero_desc(&d);
    memset(&rd, 0, sizeof(rd));
    rd.text = "order full_random\nreps 4\njitter iti uniform 0.8 1.2 rate=60000/1001\n"
              "jitter fp exponential fp_lo fp_hi 0.4 rate=60\njitter soa choice 0.1 0.2 0.25\n";
    rd.len = strlen(rd.text);
    rd.table = &g_tab;
    rd.arena = arena;
    rd.arena_size = sizeof(arena);
    CHECK_I(ytr_rules(&d, &rd, err, sizeof(err)), 0);
    CHECK_I(d.n_jitters, 3);
    CHECK_S(d.jitters[0].name, "iti");
    CHECK(d.jitters[0].lo == 0.8 && d.jitters[0].hi == 1.2 && d.jitters[0].rate_num == 60000 &&
          d.jitters[0].rate_den == 1001);
    CHECK_S(d.jitters[1].lo_column, "fp_lo");
    CHECK(d.jitters[1].dist == YTR_JITTER_EXPONENTIAL && d.jitters[1].scale == 0.4 &&
          d.jitters[1].rate_num == 60 && d.jitters[1].rate_den == 1);
    CHECK(d.jitters[2].n_values == 3 && d.jitters[2].values[2] == 0.25);
    d.rng = ytr_splitmix;
    g_rng_state = 77;
    d.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &d));
    /* The rules text written back gives the same session. */
    n = ytr_format_rules(&g_t, out, sizeof(out));
    CHECK(n > 0);
    CHECK_HAS(out, "jitter fp exponential fp_lo fp_hi 0.4 rate=60/1\n");
    zero_desc(&d2);
    rd.text = out;
    rd.len = strlen(out);
    CHECK_I(ytr_rules(&d2, &rd, err, sizeof(err)), 0);
    d2.rng = ytr_splitmix;
    g_rng_state = 77;
    d2.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_u, &d2));
    ytr_format_rules(&g_u, out2, sizeof(out2));
    CHECK_S(out2, out);
    g_rng_state = 77;
    CHECK(ytr_open(&g_t, &d));
    {
        uint64_t s1 = g_rng_state, s2;
        g_rng_state = 77;
        CHECK(ytr_open(&g_u, &d2));
        s2 = g_rng_state;
        CHECK(s1 == s2);
        for (i = 0; i < 12; i++) {
            g_rng_state = s1;
            ytr_next(&g_t, NULL);
            s1 = g_rng_state;
            g_rng_state = s2;
            ytr_next(&g_u, NULL);
            s2 = g_rng_state;
            for (k = 0; k < 3; k++) CHECK(ytr_jitter(&g_t, i, k).ns == ytr_jitter(&g_u, i, k).ns);
            ytr_update(&g_t, 1, NULL);
            ytr_update(&g_u, 1, NULL);
        }
    }
    /* Five jitters is one too many at the default YTR_MAX_JITTERS. */
#if YTR_MAX_JITTERS == 4
    zero_desc(&d);
    rd.text = "jitter a uniform 1 2\njitter b uniform 1 2\njitter c uniform 1 2\njitter d uniform 1 2\n"
              "jitter e uniform 1 2\n";
    rd.len = strlen(rd.text);
    CHECK_I(ytr_rules(&d, &rd, err, sizeof(err)), 5);
    CHECK_HAS(err, "more than 4 jitters");
#endif
    for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        zero_desc(&d);
        rd.text = bad[i][0];
        rd.len = strlen(rd.text);
        CHECK(ytr_rules(&d, &rd, err, sizeof(err)) != 0);
        CHECK_HAS(err, bad[i][1]);
    }
    printf("  jitter rules: parsed, written back and re-read to the same session, %d error messages\n",
           (int)(sizeof(bad) / sizeof(bad[0])) + 1);
}

#endif

static void test_jitter(void) {
    test_jitter_fixed();
    test_jitter_call();
    test_jitter_dist();
#if YTR_MAX_JITTERS >= 4
    test_jitter_session();
    test_jitter_columns();
    test_jitter_rules();
#endif
}
