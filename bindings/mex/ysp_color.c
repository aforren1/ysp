/* ysp_color.c - MATLAB/Octave MEX binding for ysp/color.h (builds to ysp_color.<mexext>)
 *
 * Command dispatch in the style of the other ysp MEX files: the first
 * argument is a command string, and 'open' returns an opaque uint64 handle
 * to a conversion context. Calibrations and luminance records travel as
 * their canonical bytes (uint8 rows), the same bytes as the .yspcal and
 * .ysplum files.
 *
 *     cal = ysp_color('cal_nominal', [.64 .33; .30 .60; .15 .06; .3127 .3290], 80, 2.2);
 *     h   = ysp_color('open', struct('cal', cal, 'background', [.5 .5 .5]));
 *     [rgb, g] = ysp_color('to_rgb', h, 'dkl', [0 90 0.1]);    % rows of colors
 *     X   = ysp_color('convert', h, 'rgb', 'cielab', RGB);       % N x 3
 *     k   = ysp_color('max_scale', h, 'dkl', [0 90 1], 'symmetric');
 *     ysp_color('close', h);
 *
 * Colors are rows: an N x 3 matrix in the space named by a string ("dkl",
 * "cielab", ...). A gamut comes back as a struct of columns (in, below,
 * above, margin, distance, scale, kept). A refusal raises ysp_color:refused
 * with the header's reason.
 *
 * Build with build.m. See README.md for the command reference.
 */
#define PM_MOD "ysp_color"
#include "ysp_mex_util.h"

#define YSP_COLOR_IMPLEMENTATION
#include "ysp/color.h"

/* A handle: the calibration and the lum record live beside the context that
 * points at them, so they cannot go away under it. */
typedef struct col_handle {
    ycol_cal cal;
    ycol_lum lum;
    ycol_ctx cx;
} col_handle;

static void col_destroy(void* obj) { free(obj); }

static const char* const col_cones_names[] = { "ss2", "ss10" };
static const char* const col_adapt_names[] = { "none", "bradford" };
static const char* const col_method_names[] = { "", "hfp", "min_motion", "min_border", "other" };
static const char* const col_map_names[] = { "", "scale", "chroma_oklch", "chroma_cielch", "chroma_dkl", "clip" };
static const char* const col_plane_names[] = { "", "dkl", "cone", "oklch", "cielch" };
static const char* const col_sides_names[] = { "one", "symmetric" };

static void col_check(int rc, const char* why) {
    if (rc >= 0) return;
    switch (rc) {
    case YCOL_ERR_ARG:     pm_err("arg", "%s", why ? why : ycol_strerror(rc)); break;
    case YCOL_ERR_REFUSED: pm_err("refused", "%s", why ? why : ycol_strerror(rc)); break;
    case YCOL_ERR_FORMAT:  pm_err("format", "%s", why ? why : ycol_strerror(rc)); break;
    case YCOL_ERR_RANGE:   pm_err("range", "%s", why ? why : ycol_strerror(rc)); break;
    default:                 pm_err("error", "%s", why ? why : ycol_strerror(rc)); break;
    }
}

static int col_space(const mxArray* a) {
    char* s = pm_string(a, "space");
    int sp = ycol_space_from_name(s);
    if (!sp) pm_err("arg", "unknown space '%s' (\"rgb\", \"dkl\", \"cielab\", ...)", s);
    return sp;
}

/* An N x 3 real double matrix (or one row): its row count and data. */
static const double* col_rows(const mxArray* a, size_t* n, const char* what) {
    if (!mxIsDouble(a) || mxIsComplex(a) || mxGetN(a) != 3) pm_err("arg", "%s must be a real N x 3 double matrix", what);
    *n = mxGetM(a);
    return mxGetPr(a);
}

static void col_load_cal(const mxArray* a, ycol_cal* c) {
    size_t n;
    char err[256];
    const uint8_t* b = pm_bytes_in(a, "cal", &n);
    col_check(ycol_cal_load(c, b, n, err, sizeof err), err);
}

static void col_load_lum(const mxArray* a, ycol_lum* l) {
    size_t n;
    char err[256];
    const uint8_t* b = pm_bytes_in(a, "lum", &n);
    col_check(ycol_lum_load(l, b, n, err, sizeof err), err);
}

static mxArray* col_mat3(const double* m) {   /* a row-major 3 x 3 as a MATLAB 3 x 3 */
    mxArray* a = mxCreateDoubleMatrix(3, 3, mxREAL);
    double* p = mxGetPr(a);
    int i, j;
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) p[i + 3 * j] = m[3 * i + j];
    return a;
}

/* The gamut of n results as a struct of columns. */
static mxArray* col_gamut_out(const ycol_gamut* g, size_t n) {
    static const char* f[] = { "in", "below", "above", "margin", "distance", "scale", "kept" };
    mxArray* s = mxCreateStructMatrix(1, 1, 7, f);
    mxArray *in = mxCreateLogicalMatrix(n, 1), *below = mxCreateDoubleMatrix(n, 1, mxREAL),
            *above = mxCreateDoubleMatrix(n, 1, mxREAL), *margin = mxCreateDoubleMatrix(n, 3, mxREAL),
            *dist = mxCreateDoubleMatrix(n, 1, mxREAL), *scale = mxCreateDoubleMatrix(n, 1, mxREAL),
            *kept = mxCreateDoubleMatrix(n, 1, mxREAL);
    mxLogical* pin = mxGetLogicals(in);
    size_t i;
    for (i = 0; i < n; i++) {
        pin[i] = g[i].in != 0;
        mxGetPr(below)[i] = g[i].below; mxGetPr(above)[i] = g[i].above;
        mxGetPr(margin)[i] = g[i].margin[0]; mxGetPr(margin)[i + n] = g[i].margin[1]; mxGetPr(margin)[i + 2 * n] = g[i].margin[2];
        mxGetPr(dist)[i] = g[i].distance; mxGetPr(scale)[i] = g[i].scale; mxGetPr(kept)[i] = g[i].kept;
    }
    mxSetField(s, 0, "in", in); mxSetField(s, 0, "below", below); mxSetField(s, 0, "above", above);
    mxSetField(s, 0, "margin", margin); mxSetField(s, 0, "distance", dist); mxSetField(s, 0, "scale", scale);
    mxSetField(s, 0, "kept", kept);
    return s;
}

static const char* const col_open_fields[] = { "cal", "background", "cones", "lum", "white", "src_white_Y", "adapt" };

/* to_rgb, to_dir, map: rows in a space to linear RGB, with their gamut. */
static void col_rows_to_rgb(col_handle* h, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], int what) {
    size_t n, i;
    const double* v;
    int sp, method = 0;
    ycol_gamut* g;
    double* out;
    pm_nargs(nrhs, what == 2 ? 5 : 4, what == 2 ? "[rgb, g] = ysp_color('map', h, space, X, method)" : "[rgb, g] = ysp_color(cmd, h, space, X)");
    sp = col_space(prhs[2]);
    v = col_rows(prhs[3], &n, "X");
    if (what == 2) method = pm_enum(prhs[4], "method", col_map_names, 6);
    if (what == 2 && method == 0) pm_err("arg", "method: 'scale', 'chroma_oklch', 'chroma_cielch', 'chroma_dkl' or 'clip'");
    plhs[0] = mxCreateDoubleMatrix(n, 3, mxREAL);
    out = mxGetPr(plhs[0]);
    g = (ycol_gamut*)mxMalloc((n ? n : 1) * sizeof *g);
    for (i = 0; i < n; i++) {
        ycol_color c = ycol_make(sp, v[i], v[i + n], v[i + 2 * n]);
        ycol_rgb r = what == 0 ? ycol_to_rgb(&h->cx, c, &g[i]) : what == 1 ? ycol_to_dir(&h->cx, c, &g[i])
                                                                              : ycol_map(&h->cx, c, method, &g[i]);
        if (g[i].status < 0) col_check(g[i].status, g[i].why);
        out[i] = r.r; out[i + n] = r.g; out[i + 2 * n] = r.b;
    }
    if (nlhs > 1) plhs[1] = col_gamut_out(g, n);
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    col_handle* h;
    pm_destroy = col_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- commands without a handle ---- */
    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(ycol_version()); return; }
    if (strcmp(cmd, "cal_nominal") == 0) {
        static ycol_cal c;
        double xy[8];
        float f[4][2];
        int i;
        pm_nargs(nrhs, 4, "cal = ysp_color('cal_nominal', xy4x2, white_Y, gamma)");
        if (!mxIsDouble(prhs[1]) || mxGetM(prhs[1]) != 4 || mxGetN(prhs[1]) != 2) pm_err("arg", "xy must be 4 x 2 (R, G, B, white)");
        memcpy(xy, mxGetPr(prhs[1]), sizeof xy);
        for (i = 0; i < 4; i++) { f[i][0] = (float)xy[i]; f[i][1] = (float)xy[i + 4]; }
        col_check(ycol_cal_nominal(&c, (const float(*)[2])f, (float)pm_finite(prhs[2], "white_Y"), pm_finite(prhs[3], "gamma")),
                  "the stated primaries and gamma give no calibration");
        plhs[0] = pm_bytes_out(&c, sizeof c);
        return;
    }
    if (strcmp(cmd, "cal_derive") == 0) {
        /* readings: N x 5 [gun level Y x y]; spectra: M x 4 or 5 [nm r g b (black)],
         * evenly spaced; nominal_spectra: true for a datasheet's */
        static ycol_cal c;
        char err[256];
        size_t n, i;
        const double* r;
        pm_nargs(nrhs, 2, "cal = ysp_color('cal_derive', readings, [spectra], [nominal_spectra])");
        if (!mxIsDouble(prhs[1]) || mxGetN(prhs[1]) != 5) pm_err("arg", "readings must be N x 5: gun, level, Y, x, y");
        n = mxGetM(prhs[1]);
        r = mxGetPr(prhs[1]);
        ycol_cal_init(&c);
        for (i = 0; i < n; i++)
            col_check(ycol_cal_add(&c, (int)r[i], (float)r[i + n], (float)r[i + 2 * n], (float)r[i + 3 * n], (float)r[i + 4 * n]),
                      "a reading: gun -1 (black), 0, 1, 2 or 3 (white); level 0..1; Y >= 0");
        if (nrhs > 2 && !mxIsEmpty(prhs[2])) {
            size_t m = mxGetM(prhs[2]), k = mxGetN(prhs[2]), j;
            const double* s = mxGetPr(prhs[2]);
            float *fr, *fg, *fb, *fk = NULL;
            int nominal = nrhs > 3 && pm_bool(prhs[3], "nominal_spectra"), rc;
            if (!mxIsDouble(prhs[2]) || (k != 4 && k != 5) || m < 2) pm_err("arg", "spectra must be M x 4 or M x 5: nm, r, g, b (, black)");
            fr = (float*)mxMalloc(m * sizeof(float)); fg = (float*)mxMalloc(m * sizeof(float)); fb = (float*)mxMalloc(m * sizeof(float));
            if (k == 5) fk = (float*)mxMalloc(m * sizeof(float));
            for (j = 0; j < m; j++) {
                fr[j] = (float)s[j + m]; fg[j] = (float)s[j + 2 * m]; fb[j] = (float)s[j + 3 * m];
                if (fk) fk[j] = (float)s[j + 4 * m];
            }
            rc = nominal ? ycol_cal_set_spectra_nominal(&c, (float)s[0], (float)(s[1] - s[0]), (int)m, fr, fg, fb, fk)
                         : ycol_cal_set_spectra(&c, (float)s[0], (float)(s[1] - s[0]), (int)m, fr, fg, fb, fk);
            col_check(rc, "spectra: 2 to 471 samples at an even step");
        }
        col_check(ycol_cal_derive(&c, err, sizeof err), err);
        plhs[0] = pm_bytes_out(&c, sizeof c);
        return;
    }
    if (strcmp(cmd, "cal_info") == 0) {
        static ycol_cal c;
        static const char* f[] = { "flags", "crc", "n_readings", "rgb_to_xyz", "rgb_to_lms", "black_xyz", "black_lms", "white_err",
                                   "lut", "describe" };
        mxArray* s;
        char line[512];
        int n, k, j;
        pm_nargs(nrhs, 2, "info = ysp_color('cal_info', cal)");
        col_load_cal(prhs[1], &c);
        s = mxCreateStructMatrix(1, 1, 10, f);
        mxSetField(s, 0, "flags", mxCreateDoubleScalar(c.flags));
        mxSetField(s, 0, "crc", mxCreateDoubleScalar(c.crc));
        mxSetField(s, 0, "n_readings", mxCreateDoubleScalar(c.n_readings));
        mxSetField(s, 0, "rgb_to_xyz", col_mat3(c.rgb_to_xyz));
        mxSetField(s, 0, "rgb_to_lms", col_mat3(c.rgb_to_lms));
        mxSetField(s, 0, "black_xyz", pm_row(c.black_xyz, 3));
        mxSetField(s, 0, "black_lms", pm_row(c.black_lms, 3));
        mxSetField(s, 0, "white_err", mxCreateDoubleScalar(c.white_err));
        n = c.lut_n ? c.lut_n : YCOL_CAL_MAX_LUT;
        {
            mxArray* lut = mxCreateDoubleMatrix((size_t)n, 3, mxREAL);
            for (k = 0; k < 3; k++) for (j = 0; j < n; j++) mxGetPr(lut)[j + (size_t)k * n] = c.lut[k][j];
            mxSetField(s, 0, "lut", lut);
        }
        ycol_cal_describe(&c, line, sizeof line);
        mxSetField(s, 0, "describe", mxCreateString(line));
        plhs[0] = s;
        return;
    }
    if (strcmp(cmd, "lum_derive") == 0) {
        /* settings: N x 6 [a b] in linear device RGB; opts: struct with cones,
         * method, participant, fit_s, field_deg, ecc_deg, freq_hz */
        static ycol_lum l;
        static ycol_cal c;
        static const char* const allowed[] = { "cones", "method", "participant", "fit_s", "field_deg", "ecc_deg", "freq_hz" };
        const mxArray* f;
        char err[256];
        size_t n, i;
        const double* s;
        int cones = 0, method = YCOL_LUM_HFP;
        pm_nargs(nrhs, 3, "lum = ysp_color('lum_derive', settings, cal, [opts])");
        if (!mxIsDouble(prhs[1]) || mxGetN(prhs[1]) != 6) pm_err("arg", "settings must be N x 6: the two lights a and b");
        n = mxGetM(prhs[1]);
        s = mxGetPr(prhs[1]);
        col_load_cal(prhs[2], &c);
        if (nrhs > 3) {
            pm_check_fields(prhs[3], allowed, 7, "opts");
            if ((f = pm_field(prhs[3], "cones"))) cones = pm_enum(f, "cones", col_cones_names, 2);
            if ((f = pm_field(prhs[3], "method"))) method = pm_enum(f, "method", col_method_names, 5);
        }
        ycol_lum_init(&l, cones, method);
        if (nrhs > 3) {
            if ((f = pm_field(prhs[3], "participant"))) snprintf(l.participant, sizeof l.participant, "%s", pm_string(f, "participant"));
            if ((f = pm_field(prhs[3], "fit_s")) && pm_bool(f, "fit_s")) l.flags |= YCOL_LUM_FIT_S;
            if ((f = pm_field(prhs[3], "field_deg"))) l.field_deg = (float)pm_finite(f, "field_deg");
            if ((f = pm_field(prhs[3], "ecc_deg"))) l.ecc_deg = (float)pm_finite(f, "ecc_deg");
            if ((f = pm_field(prhs[3], "freq_hz"))) l.freq_hz = (float)pm_finite(f, "freq_hz");
        }
        for (i = 0; i < n; i++) {
            double a[3], b[3];
            int k;
            for (k = 0; k < 3; k++) { a[k] = s[i + k * n]; b[k] = s[i + (k + 3) * n]; }
            col_check(ycol_lum_add(&l, a, b, NULL, 0, 1), "16 settings at most, finite lights");
        }
        col_check(ycol_lum_derive(&l, &c, err, sizeof err), err);
        plhs[0] = pm_bytes_out(&l, sizeof l);
        return;
    }
    if (strcmp(cmd, "lum_stated") == 0) {
        static ycol_lum l;
        double w[3];
        int cones = 0;
        pm_nargs(nrhs, 2, "lum = ysp_color('lum_stated', w, [cones])");
        pm_vector_n(prhs[1], "w", w, 3);
        if (nrhs > 2) cones = pm_enum(prhs[2], "cones", col_cones_names, 2);
        memset(&l, 0, sizeof l);
        col_check(ycol_lum_stated(&l, cones, w), "stated weights: L and M above 0");
        plhs[0] = pm_bytes_out(&l, sizeof l);
        return;
    }
    if (strcmp(cmd, "lum_info") == 0) {
        static ycol_lum l;
        static const char* f[] = { "w", "resid", "crc", "cal_crc", "flags", "describe" };
        char line[512];
        mxArray* s;
        pm_nargs(nrhs, 2, "info = ysp_color('lum_info', lum)");
        col_load_lum(prhs[1], &l);
        s = mxCreateStructMatrix(1, 1, 6, f);
        mxSetField(s, 0, "w", pm_row(l.w, 3));
        mxSetField(s, 0, "resid", pm_row(l.resid, l.n_settings));
        mxSetField(s, 0, "crc", mxCreateDoubleScalar(l.crc));
        mxSetField(s, 0, "cal_crc", mxCreateDoubleScalar(l.cal_crc));
        mxSetField(s, 0, "flags", mxCreateDoubleScalar(l.flags));
        ycol_lum_describe(&l, line, sizeof line);
        mxSetField(s, 0, "describe", mxCreateString(line));
        plhs[0] = s;
        return;
    }
    if (strcmp(cmd, "output_code") == 0) {
        static ycol_cal c;
        size_t n, i;
        const double* v;
        double* out;
        int bits = 8;
        pm_nargs(nrhs, 3, "codes = ysp_color('output_code', cal, RGB, [bits])");
        col_load_cal(prhs[1], &c);
        v = col_rows(prhs[2], &n, "RGB");
        if (nrhs > 3) bits = pm_int(prhs[3], "bits");
        plhs[0] = mxCreateDoubleMatrix(n, 3, mxREAL);
        out = mxGetPr(plhs[0]);
        for (i = 0; i < n; i++) {
            ycol_rgb r;
            uint32_t code[3];
            r.r = v[i]; r.g = v[i + n]; r.b = v[i + 2 * n];
            col_check(ycol_output_code(&c, r, bits, code), "bits: 8 to 16");
            out[i] = code[0]; out[i + n] = code[1]; out[i + 2 * n] = code[2];
        }
        return;
    }
    if (strcmp(cmd, "cone_fundamentals") == 0) {
        size_t n, i;
        const double* nm;
        int cones = 0;
        pm_nargs(nrhs, 2, "lms = ysp_color('cone_fundamentals', nm, [cones])");
        if (!mxIsDouble(prhs[1])) pm_err("arg", "nm must be double");
        n = mxGetNumberOfElements(prhs[1]);
        nm = mxGetPr(prhs[1]);
        if (nrhs > 2) cones = pm_enum(prhs[2], "cones", col_cones_names, 2);
        plhs[0] = mxCreateDoubleMatrix(n, 3, mxREAL);
        for (i = 0; i < n; i++) {
            double l[3];
            ycol_cone_fundamentals(cones, nm[i], l);
            mxGetPr(plhs[0])[i] = l[0]; mxGetPr(plhs[0])[i + n] = l[1]; mxGetPr(plhs[0])[i + 2 * n] = l[2];
        }
        return;
    }
    if (strcmp(cmd, "open") == 0) {
        ycol_ctx_desc d;
        const mxArray* f;
        char err[256];
        pm_nargs(nrhs, 2, "h = ysp_color('open', struct('cal', calbytes, 'background', [r g b], ...))");
        pm_check_fields(prhs[1], col_open_fields, 7, "desc");
        if (!(f = pm_field(prhs[1], "cal"))) pm_err("arg", "desc.cal is required (the bytes from cal_derive, cal_nominal or a .yspcal file)");
        h = (col_handle*)calloc(1, sizeof *h);
        if (!h) pm_err("memory", "out of memory");
        {
            size_t n;
            const uint8_t* b = pm_bytes_in(f, "cal", &n);
            int rc = ycol_cal_load(&h->cal, b, n, err, sizeof err);
            if (rc < 0) { free(h); col_check(rc, err); }
        }
        memset(&d, 0, sizeof d);
        d.cal = &h->cal;
        if ((f = pm_field(prhs[1], "background"))) pm_vector_n(f, "background", d.background, 3);
        if ((f = pm_field(prhs[1], "cones"))) d.cones = pm_enum(f, "cones", col_cones_names, 2);
        if ((f = pm_field(prhs[1], "adapt"))) d.adapt = pm_enum(f, "adapt", col_adapt_names, 2);
        if ((f = pm_field(prhs[1], "white"))) pm_vector_n(f, "white", d.white, 3);
        if ((f = pm_field(prhs[1], "src_white_Y"))) d.src_white_Y = pm_finite(f, "src_white_Y");
        if ((f = pm_field(prhs[1], "lum"))) {
            size_t n;
            const uint8_t* b = pm_bytes_in(f, "lum", &n);
            int rc = ycol_lum_load(&h->lum, b, n, err, sizeof err);
            if (rc < 0) { free(h); col_check(rc, err); }
            d.lum = &h->lum;
        }
        {
            if (!ycol_ctx_open(&h->cx, &d)) {
                int rc = h->cx.status;
                snprintf(err, sizeof err, "%s", ycol_ctx_error(&h->cx));
                free(h);
                col_check(rc, err);
            }
        }
        plhs[0] = pm_handle_out(pm_register(h));
        return;
    }

    /* ---- commands on a handle ---- */
    if (nrhs < 2) pm_err("usage", "'%s' needs a handle", cmd);
    h = (col_handle*)pm_lookup(prhs[1]);
    if (strcmp(cmd, "close") == 0) {
        pm_unregister(h);
        col_destroy(h);
    } else if (strcmp(cmd, "to_rgb") == 0) {
        col_rows_to_rgb(h, nlhs, plhs, nrhs, prhs, 0);
    } else if (strcmp(cmd, "to_dir") == 0) {
        col_rows_to_rgb(h, nlhs, plhs, nrhs, prhs, 1);
    } else if (strcmp(cmd, "map") == 0) {
        col_rows_to_rgb(h, nlhs, plhs, nrhs, prhs, 2);
    } else if (strcmp(cmd, "convert") == 0) {
        size_t n, i;
        const double* v;
        int from, to;
        double *out, *ing;
        pm_nargs(nrhs, 5, "[Y, in, flags] = ysp_color('convert', h, src, dst, X)");
        from = col_space(prhs[2]);
        to = col_space(prhs[3]);
        v = col_rows(prhs[4], &n, "X");
        plhs[0] = mxCreateDoubleMatrix(n, 3, mxREAL);
        out = mxGetPr(plhs[0]);
        if (nlhs > 1) plhs[1] = mxCreateDoubleMatrix(n, 1, mxREAL);
        if (nlhs > 2) plhs[2] = mxCreateDoubleMatrix(n, 1, mxREAL);
        ing = nlhs > 1 ? mxGetPr(plhs[1]) : NULL;
        for (i = 0; i < n; i++) {
            ycol_color o;
            ycol_gamut g;
            int rc = ycol_convert(&h->cx, ycol_make(from, v[i], v[i + n], v[i + 2 * n]), to, &o, &g);
            if (rc < 0 && (rc != YCOL_ERR_RANGE || n == 1)) col_check(rc, g.why);
            out[i] = rc < 0 ? mxGetNaN() : o.u.v[0];
            out[i + n] = rc < 0 ? mxGetNaN() : o.u.v[1];
            out[i + 2 * n] = rc < 0 ? mxGetNaN() : o.u.v[2];
            if (ing) ing[i] = rc < 0 ? 0 : g.in;
            if (nlhs > 2) mxGetPr(plhs[2])[i] = rc < 0 ? -1 : (double)o.flags;
        }
    } else if (strcmp(cmd, "max_scale") == 0) {
        size_t n, i;
        const double* v;
        int sp, sides = 0;
        pm_nargs(nrhs, 4, "k = ysp_color('max_scale', h, space, X, ['one' | 'symmetric'])");
        sp = col_space(prhs[2]);
        v = col_rows(prhs[3], &n, "X");
        if (nrhs > 4) sides = pm_enum(prhs[4], "sides", col_sides_names, 2);
        plhs[0] = mxCreateDoubleMatrix(n, 1, mxREAL);
        for (i = 0; i < n; i++) {
            double k = ycol_max_scale(&h->cx, ycol_make(sp, v[i], v[i + n], v[i + 2 * n]), sides);
            if (k < 0) {
                ycol_gamut g;
                ycol_to_rgb(&h->cx, ycol_make(sp, v[i], v[i + n], v[i + 2 * n]), &g);
                col_check((int)k, g.why);
            }
            mxGetPr(plhs[0])[i] = k;
        }
    } else if (strcmp(cmd, "ring") == 0) {
        int plane, n, sides = 0;
        pm_nargs(nrhs, 5, "r = ysp_color('ring', h, plane, fixed, n, ['one' | 'symmetric'])");
        plane = pm_enum(prhs[2], "plane", col_plane_names, 5);
        n = pm_int(prhs[4], "n");
        if (plane == 0 || n < 1 || n > 1000000) pm_err("arg", "plane 'dkl', 'cone', 'oklch' or 'cielch'; n 1 to 1000000");
        if (nrhs > 5) sides = pm_enum(prhs[5], "sides", col_sides_names, 2);
        plhs[0] = mxCreateDoubleMatrix(1, (size_t)n, mxREAL);
        col_check(ycol_max_ring(&h->cx, plane, pm_finite(prhs[3], "fixed"), sides, n, mxGetPr(plhs[0])),
                  h->cx.why_no_bg ? h->cx.why_no_bg : NULL);
    } else if (strcmp(cmd, "describe") == 0) {
        char line[1024];
        ycol_ctx_describe(&h->cx, line, sizeof line);
        plhs[0] = mxCreateString(line);
    } else if (strcmp(cmd, "info") == 0) {
        static const char* f[] = { "id", "can", "background", "bg_lms", "w", "white", "dkl", "rgb_to_lms", "rgb_to_xyz", "mb_k" };
        mxArray* s = mxCreateStructMatrix(1, 1, 10, f);
        mxSetField(s, 0, "id", mxCreateDoubleScalar(h->cx.id));
        mxSetField(s, 0, "can", mxCreateDoubleScalar(h->cx.can));
        mxSetField(s, 0, "background", pm_row(h->cx.bg, 3));
        mxSetField(s, 0, "bg_lms", pm_row(h->cx.bg_lms, 3));
        mxSetField(s, 0, "w", pm_row(h->cx.w, 3));
        mxSetField(s, 0, "white", pm_row(h->cx.white_xyz, 3));
        mxSetField(s, 0, "dkl", col_mat3(h->cx.dkl));
        mxSetField(s, 0, "rgb_to_lms", col_mat3(h->cx.rgb_to_lms));
        mxSetField(s, 0, "rgb_to_xyz", col_mat3(h->cx.rgb_to_xyz));
        mxSetField(s, 0, "mb_k", mxCreateDoubleScalar(h->cx.mb_k));
        plhs[0] = s;
    } else if (strcmp(cmd, "set_background") == 0) {
        double bg[3];
        pm_nargs(nrhs, 3, "ysp_color('set_background', h, [r g b])");
        pm_vector_n(prhs[2], "background", bg, 3);
        col_check(ycol_ctx_set_background(&h->cx, bg), "background: three numbers in 0..1");
    } else if (strcmp(cmd, "lum_pair") == 0) {
        double a[3], b[3];
        pm_nargs(nrhs, 5, "[a, b] = ysp_color('lum_pair', h, azim, elev, contrast)");
        col_check(ycol_lum_pair(&h->cx, pm_finite(prhs[2], "azim"), pm_finite(prhs[3], "elev"), pm_finite(prhs[4], "contrast"), a, b),
                  h->cx.why_no_bg);
        plhs[0] = pm_row(a, 3);
        if (nlhs > 1) plhs[1] = pm_row(b, 3);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
