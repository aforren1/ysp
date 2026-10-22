/* trials_test_vars.h - the v0.6 part of trials_test.c (included after
 * trials_test_data.h, whose helpers it uses): variables drawn per trial
 * from the trial's own stream and their seed (gap G3), the weight column
 * times reps (G4), and jitters snapped to the display's rate (G11). The
 * stream is checked against a second implementation written from the
 * manual and pinned by value; restore, save and load replay every draw;
 * every refusal is checked by its message. */

/* The stream as VARIABLES states it, written apart from the header. */
static uint64_t vt_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t vt_out(uint64_t seed, int i, int k) {
    const uint64_t g = 0x9E3779B97F4A7C15ULL;
    uint64_t s = vt_mix((seed ^ 0x6A09E667F3BCC909ULL) + (uint64_t)(i + 1) * g);
    return vt_mix(s + (uint64_t)(k + 1) * g);
}

static int vt_index(uint64_t seed, int i, int j, int n) {
    return (int)(((vt_out(seed, i, 1 + j) >> 11) * (uint64_t)n) >> 53);
}

static const char* const vt_sides[2] = { "left", "right" };
static const char* const vt_odd[3] = { "a b", "c,d", "\xC3\xA9t\xC3\xA9" };   /* été */

static void vt_desc(ytr_desc* d, uint64_t* state, bool with) {
    zero_desc(d);
    d->n_conditions = 3;
    d->reps = 6;
    d->order = YTR_ORDER_FULL_RANDOM;
    d->requeue_gap = 2;
    d->n_practice = 2;
    d->rng = ytr_splitmix;
    d->rng_ctx = state;
    d->jitters[0] = ytr_uniform("iti_s", YRT_MS(500), YRT_MS(900));
    d->n_jitters = 1;
    if (with) {
        d->seed = 20261021u;
        d->vars[0] = ytr_var_choice("side", vt_sides, 2);
        d->vars[1] = ytr_var_choice("odd", vt_odd, 3);
        d->n_vars = 2;
    }
}

/* Run a session to its end: outcome 1, a re-queue every seventh trial. */
static int vt_run(ytr_trials* t, int* outc, int cap) {
    ytr_trial_info ti = ti_zero;
    int n = 0;
    while (n < cap && ytr_next(t, &ti) >= 0) {
        if (ti.index % 7 == 3 && !ti.practice && ti.track < 0 && ytr_requeue(t) == 0) {
            outc[n++] = YTR_REQUEUE;
            continue;
        }
        ytr_update(t, 1, NULL);
        outc[n++] = 1;
    }
    return n;
}

static void test_vars_stream(void) {
    static int outc[64];
    ytr_desc d, d0;
    uint64_t s = 5, s0 = 5;
    int i, j, n, n0;
    static double cnt[3], pair[2][3];
    double expv[3], chi;
    int df;

    /* Pinned values: a change of the hash moves these. */
    CHECK(vt_out(0, 0, 0) == 0x96DCB1D7126A6EBAULL);
    CHECK(vt_out(20261021u, 0, 0) == 0x7136345FFEBD72F2ULL);
    CHECK(vt_out(20261021u, 999, 2) == 0xB1F2FA923152EA67ULL);

    /* Vars draw nothing from the session's generator: the schedule, the
     * jitters and the generator's position are the session's without. */
    vt_desc(&d0, &s0, false);
    vt_desc(&d, &s, true);
    CHECK(ytr_open(&g_u, &d0));
    CHECK(ytr_open(&g_t, &d));
    n0 = vt_run(&g_u, outc, 64);
    n = vt_run(&g_t, outc, 64);
    CHECK_I(n, n0);
    CHECK(s == s0);
    for (i = 0; i < n; i++) {
        CHECK_I(hist(&g_t, i).condition, hist(&g_u, i).condition);
        CHECK(ytr_jitter(&g_t, i, 0).ns == ytr_jitter(&g_u, i, 0).ns);
        /* each draw is the manual's formula, and the text is the value's */
        CHECK_I(ytr_var(&g_t, i, 0), vt_index(d.seed, i, 0, 2));
        CHECK_I(ytr_var(&g_t, i, 1), vt_index(d.seed, i, 1, 3));
        CHECK_S(ytr_var_text(&g_t, i, 0), vt_sides[vt_index(d.seed, i, 0, 2)]);
        CHECK_S(ytr_var_text(&g_t, i, 1), vt_odd[vt_index(d.seed, i, 1, 3)]);
        CHECK(ytr_trial_seed(&g_t, i) == vt_out(d.seed, i, 0));
    }
    CHECK_I(ytr_var_index(&g_t, "odd"), 1);
    CHECK_I(ytr_var_index(&g_t, "iti_s"), -1);
    CHECK_I(ytr_var(&g_t, n, 0), YTR_ERR_ARG);
    CHECK_I(ytr_var(&g_t, 0, 2), YTR_ERR_ARG);
    CHECK_I(ytr_var(&g_t, -1, 0), YTR_ERR_ARG);
    CHECK(ytr_var_text(&g_t, n, 0) == NULL);
    CHECK(ytr_trial_seed(&g_t, n) == 0);
    CHECK_I(ytr_var(NULL, 0, 0), YTR_ERR_ARG);

    /* Each value equally likely, the variables independent: 300,000
     * trials of the stream by the header's own draw. */
    g_t.desc.seed = 77;
    for (i = 0; i < 300000; i++) {
        int a = ytr__var_draw(&g_t, i, 0), b = ytr__var_draw(&g_t, i, 1);
        cnt[b] += 1.0;
        pair[a][b] += 1.0;
    }
    for (j = 0; j < 3; j++) expv[j] = 100000.0;
    chi = jt_chi(cnt, expv, 3, &df);
    CHECK(chi < chi_crit(df));
    {
        double o[6], e[6];
        for (j = 0; j < 6; j++) {
            o[j] = pair[j / 3][j % 3];
            e[j] = 50000.0;
        }
        chi = jt_chi(o, e, 6, &df);
        CHECK(chi < chi_crit(df));
        printf("  vars: 300000 trials, values chi-square %.2f on 2 df, pairs %.2f on %d df\n",
               jt_chi(cnt, expv, 3, &j), chi, df);
    }
    ytr_close(&g_t);
    ytr_close(&g_u);
}

static void test_vars_replay(void) {
    static int outc[64];
    static unsigned char snap[1 << 16];
    static char a[512], b[512];
    ytr_desc d;
    uint64_t s = 9, s2;
    int i, n, len, cut;

    /* restore() after a fresh open gives every value and data line again. */
    vt_desc(&d, &s, true);
    CHECK(ytr_open(&g_t, &d));
    n = vt_run(&g_t, outc, 64);
    s = 9;
    CHECK(ytr_open(&g_u, &d));
    CHECK_I(ytr_restore(&g_u, outc, NULL, n), 0);
    for (i = 0; i < n; i++) {
        ytr_format_row(&g_t, i, a, sizeof(a));
        ytr_format_row(&g_u, i, b, sizeof(b));
        CHECK_S(b, a);
        CHECK_I(ytr_var(&g_u, i, 1), ytr_var(&g_t, i, 1));
    }
    /* The data line ends with the jitter, then the variables, quoted where
     * CSV needs it. */
    ytr_format_header(&g_t, a, sizeof(a));
    CHECK_HAS(a, ",outcome,iti_s,side,odd\n");
    for (i = 0; i < n; i++) {
        ytr_format_row(&g_t, i, a, sizeof(a));
        if (ytr_var(&g_t, i, 1) == 1) { CHECK_HAS(a, ",\"c,d\"\n"); break; }
    }

    /* save at cut points, load into a fresh handle, finish: the same. */
    for (cut = 0; cut < n; cut += 5) {
        s = 9;
        CHECK(ytr_open(&g_u, &d));
        for (i = 0; i < cut; i++) {
            CHECK(ytr_next(&g_u, NULL) >= 0);
            if (outc[i] == YTR_REQUEUE) ytr_requeue(&g_u);
            else ytr_update(&g_u, outc[i], NULL);
        }
        len = ytr_save(&g_u, snap, sizeof(snap));
        CHECK(len > 8);
        CHECK_I(snap[4], 5);       /* format 5: a desc with variables */
        s2 = s;
        d.rng_ctx = &s2;
        CHECK(ytr_open_saved(&g_u, &d, snap, (size_t)len));
        for (i = cut; i < n; i++) {
            CHECK(ytr_next(&g_u, NULL) >= 0);
            if (outc[i] == YTR_REQUEUE) ytr_requeue(&g_u);
            else ytr_update(&g_u, outc[i], NULL);
        }
        for (i = 0; i < n; i++) {
            ytr_format_row(&g_t, i, a, sizeof(a));
            ytr_format_row(&g_u, i, b, sizeof(b));
            CHECK_S(b, a);
        }
        d.rng_ctx = &s;
    }

    /* A desc that differs in the seed or a value is refused by name. */
    s = 9;
    CHECK(ytr_open(&g_u, &d));
    CHECK(ytr_next(&g_u, NULL) >= 0);
    len = ytr_save(&g_u, snap, sizeof(snap));
    d.seed++;
    CHECK(!ytr_open_saved(&g_u, &d, snap, (size_t)len));
    CHECK_HAS(ytr_error(&g_u), "desc.seed does not match");
    d.seed--;
    d.vars[0] = ytr_var_choice("side", vt_odd, 2);
    CHECK(!ytr_open_saved(&g_u, &d, snap, (size_t)len));
    CHECK_HAS(ytr_error(&g_u), "desc.vars[].values does not match");
    d.vars[0] = ytr_var_choice("side", vt_sides, 2);
    d.n_vars = 1;
    CHECK(!ytr_open_saved(&g_u, &d, snap, (size_t)len));
    CHECK_HAS(ytr_error(&g_u), "desc.n_vars does not match");
    d.n_vars = 2;
    CHECK(ytr_open_saved(&g_u, &d, snap, (size_t)len));

    /* A seed alone (no variables) is format 5 too: the trial seeds depend
     * on it. No seed and no variables keep format 4. */
    vt_desc(&d, &s, false);
    d.seed = 3;
    CHECK(ytr_open(&g_u, &d));
    len = ytr_save(&g_u, snap, sizeof(snap));
    CHECK_I(snap[4], 5);
    ytr_format_meta(&g_u, a, sizeof(a));
    CHECK_HAS(a, " seed=3\n");
    vt_desc(&d, &s, false);
    CHECK(ytr_open(&g_u, &d));
    len = ytr_save(&g_u, snap, sizeof(snap));
    CHECK_I(snap[4], 4);
    ytr_format_meta(&g_u, a, sizeof(a));
    CHECK(strstr(a, "seed=") == NULL);
    ytr_close(&g_t);
    ytr_close(&g_u);
}

static void test_vars_refusals(void) {
    static const char* const bad_u8[1] = { "\xC3" };
    static const char* const empty[1] = { "" };
    static const char* const many[YTR_MAX_VAR_VALUES + 1] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16",
        "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32" };
    static char long_text[YTR_MAX_VAR_TEXT + 2];
    const char* long_vals[1];
    ytr_desc d;
    uint64_t s = 1;
    memset(long_text, 'x', YTR_MAX_VAR_TEXT + 1);
    long_vals[0] = long_text;

#define VREJ(sub) do { CHECK(!ytr_open(&g_t, &d)); CHECK_HAS(ytr_error(&g_t), sub); } while (0)
    vt_desc(&d, &s, true);
    d.vars[1].name = "2x";             VREJ("desc.vars[1].name must be");
    d.vars[1].name = "side";           VREJ("desc.vars[0] and [1] are both named 'side'");
    d.vars[1].name = "iti_s";          VREJ("desc.vars[1] and desc.jitters[0] are both named 'iti_s'");
    d.vars[1].name = "odd";
    d.vars[1].n_values = 0;            VREJ("desc.vars[1] (odd) needs 1 to 32 values");
    d.vars[1] = ytr_var_choice("odd", many, YTR_MAX_VAR_VALUES + 1);
    VREJ("needs 1 to 32 values");
    d.vars[1] = ytr_var_choice("odd", many, YTR_MAX_VAR_VALUES);
    CHECK(ytr_open(&g_t, &d));
    d.vars[1] = ytr_var_choice("odd", NULL, 2);  VREJ("needs 1 to 32 values");
    d.vars[1] = ytr_var_choice("odd", empty, 1); VREJ("value 0 must be 1 to 63 bytes");
    d.vars[1] = ytr_var_choice("odd", long_vals, 1); VREJ("value 0 must be 1 to 63 bytes");
    long_text[YTR_MAX_VAR_TEXT] = '\0';
    d.vars[1] = ytr_var_choice("odd", long_vals, 1);
    CHECK(ytr_open(&g_t, &d));
    d.vars[1] = ytr_var_choice("odd", bad_u8, 1); VREJ("value 0 is not valid UTF-8");
    d.n_vars = YTR_MAX_VARS + 1;       VREJ("desc.n_vars must be in [0, YTR_MAX_VARS");
    d.n_vars = -1;                     VREJ("desc.n_vars must be in");
    /* a table column's name */
    vt_desc(&d, &s, true);
    d.n_conditions = 0;
    CHECK(csv_table(&g_tab, g_cond_csv));
    d.table = &g_tab;
    d.vars[1].name = "word";           VREJ("desc.vars[1] is named 'word', as a table column is");
#undef VREJ
    ytr_close(&g_t);
}

static void test_vars_rules(void) {
    static char out[2048], err[256];
    ytr_desc r, r2;
    uint64_t s1 = 4, s2 = 4;
    int i;

    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order random\nreps 2\nvar side choice left right\n"
                            "var odd choice \"a b\" \"c,d\" \"q\"\"t\" x=y\n", 0, err, sizeof(err)), 4);
    CHECK_HAS(err, "a value with '=' is quoted");
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order random\nreps 2\nvar side choice left right\n"
                            "var odd choice \"a b\" \"c,d\" \"q\"\"t\" \"x=y\"\n", 0, err, sizeof(err)), 0);
    CHECK_I(r.n_vars, 2);
    CHECK_S(r.vars[0].name, "side");
    CHECK_I(r.vars[1].n_values, 4);
    CHECK_S(r.vars[1].values[2], "q\"t");
    CHECK_S(r.vars[1].values[3], "x=y");
    r.seed = 11;
    r.rng = ytr_splitmix;
    r.rng_ctx = &s1;
    CHECK(ytr_open(&g_t, &r));
    ytr_format_rules(&g_t, out, sizeof(out));
    CHECK_HAS(out, "\nvar side choice left right\nvar odd choice \"a b\" c,d \"q\"\"t\" \"x=y\"\n");
    ytr_format_meta(&g_t, out + 1024, 1024);
    CHECK_HAS(out + 1024, " seed=11 vars=side:choice:left:right;odd:choice:\"a b\":c,d:\"q\"\"t\":\"x=y\"\n");
    /* The logged rules paste back: the same values on every trial. */
    zero_desc(&r2);
    CHECK_I(apply_rules(&r2, out, 0, err, sizeof(err)), 0);
    r2.seed = 11;
    r2.rng = ytr_splitmix;
    r2.rng_ctx = &s2;
    CHECK(ytr_open(&g_u, &r2));
    for (i = 0; i < 20 && ytr_next(&g_t, NULL) >= 0 && ytr_next(&g_u, NULL) >= 0; i++) {
        CHECK_S(ytr_var_text(&g_u, i, 1), ytr_var_text(&g_t, i, 1));
        CHECK_I(ytr_var(&g_u, i, 0), ytr_var(&g_t, i, 0));
        ytr_update(&g_t, 1, NULL);
        ytr_update(&g_u, 1, NULL);
    }
    /* Errors, each at its line and by its message. */
    {
        static const struct { const char* text; const char* sub; } bad[] = {
            { "var side left right\n", "expected: var NAME choice V V ..." },
            { "var side\n", "expected: var NAME choice" },
            { "var \"side\" choice a\n", "a variable's name is a word" },
            { "var abcdefghijklmnopqrstuvwxyz0123456 choice a\n", "a variable's name is a word" },
            { "var v choice \"\"\n", "a value is 1 to 63 bytes" },
            { "var v choice 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32\n",
              "more than 32 values" },
            { "var a choice x\nvar b choice x\nvar c choice x\nvar d choice x\nvar e choice x\n",
              "more than 4 variables" },
            { "vars side choice a\n", "did you mean 'var'" },
        };
        for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
            zero_desc(&r);
            CHECK(apply_rules(&r, bad[i].text, 0, err, sizeof(err)) > 0);
            CHECK_HAS(err, bad[i].sub);
        }
    }
    ytr_close(&g_t);
    ytr_close(&g_u);
}

/* G4: `weight COLUMN` is each row's count per repetition, and reps
 * multiplies it, as OpenSesame's repeat does a cycle. */
static void test_weight_reps(void) {
    static const char* const csv =
        "word,ink,congruent,w\n"
        "red,red,1,2\nred,green,0,1\ngreen,green,1,2\ngreen,red,0,1\n";
    static const int base[4] = { 2, 1, 2, 1 };
    static int want[4];
    static char err[256], out[1024];
    ytr_desc r, c;
    uint64_t s = 8;
    int i;
    CHECK(csv_table(&g_tab, csv));
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order constrained\nweight w\nreps 3\nmax_run congruent=1 3\n", 0, err, sizeof(err)), 0);
    CHECK(r.cond_reps != NULL);
    for (i = 0; i < 4 && r.cond_reps; i++) CHECK_I(r.cond_reps[i], 3 * base[i]);
    /* the C program's own product gives the same session */
    zero_desc(&c);
    c.table = &g_tab;
    c.order = YTR_ORDER_CONSTRAINED;
    c.constraints[0] = ytr_max_run(2, 0, 3);
    c.constraints[0].level = ytb_find(&g_tab, 2, "1");
    c.n_constraints = 1;
    for (i = 0; i < 4; i++) want[i] = 3 * base[i];
    c.cond_reps = want;
    same_schedule(__LINE__, &c, &r, 31);
    /* either order of the two statements; weight alone is reps 1 */
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "reps 3\nweight w\n", 0, err, sizeof(err)), 0);
    for (i = 0; i < 4 && r.cond_reps; i++) CHECK_I(r.cond_reps[i], 3 * base[i]);
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "weight w\n", 0, err, sizeof(err)), 0);
    for (i = 0; i < 4 && r.cond_reps; i++) CHECK_I(r.cond_reps[i], base[i]);
    /* reps set in C before the rules multiplies too */
    zero_desc(&r);
    r.reps = 2;
    CHECK_I(apply_rules(&r, "weight w\n", 0, err, sizeof(err)), 0);
    for (i = 0; i < 4 && r.cond_reps; i++) CHECK_I(r.cond_reps[i], 2 * base[i]);
    /* where keeps its meaning: rows that do not match get none */
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "weight w\nreps 2\nwhere congruent=1\n", 0, err, sizeof(err)), 0);
    for (i = 0; i < 4 && r.cond_reps; i++) CHECK_I(r.cond_reps[i], (i % 2 == 0) ? 4 : 0);
    /* the logged rules carry the products, and paste back */
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order random\nweight w\nreps 3\n", 0, err, sizeof(err)), 0);
    r.rng = ytr_splitmix;
    r.rng_ctx = &s;
    CHECK(ytr_open(&g_t, &r));
    CHECK_I(ytr_n_scheduled(&g_t), 18);
    ytr_format_rules(&g_t, out, sizeof(out));
    CHECK_HAS(out, "\ncond_reps 6 3 6 3\n");
    /* cond_reps is a whole count: reps beside it would be ignored */
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "cond_reps 1 2 1 2\nreps 3\n", 0, err, sizeof(err)), 2);
    CHECK_HAS(err, "reps with cond_reps (line 1)");
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "reps 3\ncond_reps 1 2 1 2\n", 0, err, sizeof(err)), 2);
    CHECK_HAS(err, "cond_reps with reps (line 1)");
    /* a product above the history's size */
    zero_desc(&r);
    snprintf(out, sizeof(out), "weight w\nreps %d\n", YTR_MAX_TRIALS);
    CHECK(apply_rules(&r, out, 0, err, sizeof(err)) != 0);
    CHECK_HAS(err, "weight x reps for row 0 is");
    ytr_close(&g_t);
}

/* G11: rate=display snaps a jitter to the rate the caller sets from the
 * screen, after ytr_rules(). */
static void test_display_rate(void) {
    static unsigned char snap_a[1 << 15], snap_b[1 << 15];
    static char out[1024], err[256];
    ytr_desc r, f;
    ytr_jitter_desc j;
    uint64_t s1 = 6, s2 = 6;
    int i, la, lb;

    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order random\nreps 10\njitter soa_s uniform 0.8 1.2 rate=display\n", 0, err,
                        sizeof(err)), 0);
    CHECK_I(r.jitters[0].rate_num, YTR_RATE_DISPLAY);
    r.rng = ytr_splitmix;
    r.rng_ctx = &s1;
    /* without the display's rate, open() says what to set */
    CHECK(!ytr_open(&g_t, &r));
    CHECK_HAS(ytr_error(&g_t), "desc.jitters[0] (soa_s): snaps to the display's rate (rate=display)");
    r.display_rate_num = -60;
    CHECK(!ytr_open(&g_t, &r));
    CHECK_HAS(ytr_error(&g_t), "display_rate_den must not be negative");
    r.display_rate_num = 60000;
    r.display_rate_den = 1001;
    s1 = 6;
    CHECK(ytr_open(&g_t, &r));
    /* the same session as rate=60000/1001 in the text */
    zero_desc(&f);
    CHECK_I(apply_rules(&f, "order random\nreps 10\njitter soa_s uniform 0.8 1.2 rate=60000/1001\n", 0, err,
                        sizeof(err)), 0);
    f.rng = ytr_splitmix;
    f.rng_ctx = &s2;
    CHECK(ytr_open(&g_u, &f));
    for (i = 0; i < 20 && ytr_next(&g_t, NULL) >= 0 && ytr_next(&g_u, NULL) >= 0; i++) {
        CHECK(ytr_jitter(&g_t, i, 0).frames == ytr_jitter(&g_u, i, 0).frames);
        CHECK(ytr_jitter(&g_t, i, 0).ns == ytr_jitter(&g_u, i, 0).ns);
        CHECK(ytr_jitter(&g_t, i, 0).frames >= 48 && ytr_jitter(&g_t, i, 0).frames <= 71);
        ytr_update(&g_t, 1, NULL);
        ytr_update(&g_u, 1, NULL);
    }
    /* the snapshot holds the rate drawn on, so either desc resumes it */
    la = ytr_save(&g_t, snap_a, sizeof(snap_a));
    lb = ytr_save(&g_u, snap_b, sizeof(snap_b));
    CHECK_I(la, lb);
    CHECK(la > 0 && memcmp(snap_a, snap_b, (size_t)la) == 0);
    CHECK(ytr_open_saved(&g_u, &r, snap_b, (size_t)lb));
    r.display_rate_num = 60;
    r.display_rate_den = 0;
    CHECK(!ytr_open_saved(&g_u, &r, snap_a, (size_t)la));
    CHECK_HAS(ytr_error(&g_u), "jitters[].rate_num does not match");
    /* the rules text keeps "display"; the meta line gives the rate */
    ytr_format_rules(&g_t, out, sizeof(out));
    CHECK_HAS(out, "\njitter soa_s uniform 0.8 1.2 rate=display\n");
    ytr_format_meta(&g_t, out, sizeof(out));
    CHECK_HAS(out, " jitters=soa_s:uniform:0.8:1.2:rate=60000/1001 display_rate=60000/1001\n");
    /* rate_den beside YTR_RATE_DISPLAY is a mistake, and the call level has
     * no session to give the rate */
    r.display_rate_num = 60;
    r.jitters[0].rate_den = 1;
    CHECK(!ytr_open(&g_t, &r));
    CHECK_HAS(ytr_error(&g_t), "YTR_RATE_DISPLAY takes rate_den 0");
    j = ytr_frames(ytr_uniform("soa_s", YRT_MS(800), YRT_MS(1200)), YTR_RATE_DISPLAY, 0);
    CHECK_I(ytr_jitter_check(&j, err, sizeof(err)), YTR_ERR_ARG);
    CHECK_HAS(err, "YTR_RATE_DISPLAY needs a session's desc.display_rate_num");
    CHECK(ytr_jitter_map(&j, 0.5).ns == -1);
    /* rate=display with no frame inside is refused with the rate */
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "reps 1\njitter x_s uniform 0.001 0.002 rate=display\n", 0, err, sizeof(err)), 0);
    r.display_rate_num = 60;
    CHECK(!ytr_open(&g_t, &r));
    CHECK_HAS(ytr_error(&g_t), "no whole frame of 60/1 Hz");
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "jitter x_s uniform 1 2 rate=displays\n", 0, err, sizeof(err)), 1);
    CHECK_HAS(err, "expected rate=NUM or rate=NUM/DEN");
    ytr_close(&g_t);
    ytr_close(&g_u);
}

/* The data writer takes the variables as session columns. */
static void test_vars_data(void) {
    ytr_desc d;
    ytr_data_desc dd;
    ytr_col c = YTR_COL_SET("x", YTR_COL_INT);
    uint64_t s = 2;
    int i, col_side, col_odd;
    vt_desc(&d, &s, true);
    CHECK(ytr_open(&g_t, &d));
    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.trials = &g_t;
    dd.cols = &c;
    dd.n_cols = 1;
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    for (i = 0; i < 10 && ytr_next(&g_t, NULL) >= 0; i++) {
        ytr_update(&g_t, 1, NULL);
        ytr_data_set_int(&g_w, 0, i);
        CHECK_I(ytr_data_write(&g_w, NULL), 0);
    }
    CHECK_I(ytr_data_finish(&g_w), 0);
    if (read_back(g_sink, g_sink_len, false)) {
        col_side = ytb_col(&g_back, "side");
        col_odd = ytb_col(&g_back, "odd");
        CHECK(col_side >= 0 && col_odd == col_side + 1);
        CHECK_I(g_back.n_rows, 10);
        for (i = 0; i < 10 && col_side >= 0; i++) {
            CHECK_S(ytb_text(&g_back, i, col_side), ytr_var_text(&g_t, i, 0));
            CHECK_S(ytb_text(&g_back, i, col_odd), ytr_var_text(&g_t, i, 1));
        }
    } else {
        CHECK(0);
    }
    ytr_close(&g_t);
}

static void test_vars(void) {
    test_vars_stream();
    test_vars_replay();
    test_vars_refusals();
    test_vars_rules();
    test_weight_reps();
    test_display_rate();
    test_vars_data();
}
