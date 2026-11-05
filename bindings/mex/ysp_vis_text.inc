/* ysp_vis_text.inc - the text commands of ysp_gfx in the ysp-vis core (MATLAB and Octave)
 *
 * Not compiled alone: ysp_vis.c includes it after ysp_vis_gfx.inc. The work
 * is bindings/share/ysp_vis_text.h's (the same for Python); this file turns
 * MATLAB values into its yv_kv lists and its results into arrays.
 *
 *     f = ysp_gfx('font', g, {'C:/Windows/Fonts/segoeui.ttf', 'C:/Windows/Fonts/msyh.ttc'});
 *     t = ysp_gfx('text', g, 'RED', 'font', f, 'size', 48, 'color', [0.6 0.1 0.1]);
 *     ysp_gfx('set', t, 'text', 'GREEN');      % a layout, between frames
 *     [W, words] = ysp_gfx('words', t);        % [first last line x0 y0 x1 y1], screen px
 *
 * STRINGS. A text is UTF-8 inside. MATLAB's char is UTF-16: a string goes
 * in and out through UTF-16 (non-BMP characters as surrogate pairs), and a
 * character index is a MATLAB char index (1-based; a non-BMP character
 * takes two). Octave's char holds UTF-8 bytes: an index is a byte index.
 * Paths and lang go through the same conversion.
 */

/* ---- UTF-16 (MATLAB) or UTF-8 bytes (Octave) and UTF-8 --------------------------------- */

#define VX_UTF16 (sizeof(mxChar) == 2)

/* A char row (or '') as UTF-8, mxMalloc'd, its length in *n. */
static char* vx_utf8(const mxArray* a, size_t* n, const char* what) {
    size_t k, i, o = 0;
    char* s;
    if (!mxIsChar(a) || (mxGetM(a) > 1 && mxGetNumberOfElements(a) > 0) || mxGetNumberOfDimensions(a) > 2)
        vx_err("arg", "%s: a char row", what);
    k = mxGetNumberOfElements(a);
    if (!VX_UTF16) {
        s = (char*)mxMalloc(k + 1);
        if (k) memcpy(s, mxGetChars(a), k);
        s[k] = 0;
        if (n) *n = k;
        return s;
    }
    {
        const uint16_t* u = (const uint16_t*)mxGetChars(a);
        s = (char*)mxMalloc(3 * k + 1);
        for (i = 0; i < k; i++) {
            uint32_t c = u[i];
            if (c >= 0xD800 && c <= 0xDBFF) {
                if (i + 1 >= k || u[i + 1] < 0xDC00 || u[i + 1] > 0xDFFF) vx_err("arg", "%s: a lone UTF-16 surrogate at %d", what, (int)i + 1);
                c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
                i++;
            } else if (c >= 0xDC00 && c <= 0xDFFF) {
                vx_err("arg", "%s: a lone UTF-16 surrogate at %d", what, (int)i + 1);
            }
            if (c < 0x80) s[o++] = (char)c;
            else if (c < 0x800) { s[o++] = (char)(0xC0 | (c >> 6)); s[o++] = (char)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) {
                s[o++] = (char)(0xE0 | (c >> 12)); s[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); s[o++] = (char)(0x80 | (c & 0x3F));
            } else {
                s[o++] = (char)(0xF0 | (c >> 18)); s[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
                s[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); s[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        s[o] = 0;
        if (n) *n = o;
        return s;
    }
}

/* Decodes one UTF-8 codepoint at s (valid UTF-8: the core checked it). */
static uint32_t vx_u8(const unsigned char* s, uint32_t* len) {
    if (s[0] < 0x80) { *len = 1; return s[0]; }
    if ((s[0] >> 5) == 6) { *len = 2; return (uint32_t)(s[0] & 0x1F) << 6 | (s[1] & 0x3F); }
    if ((s[0] >> 4) == 14) { *len = 3; return (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); }
    *len = 4;
    return (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
}

/* UTF-8 as a char row. */
static mxArray* vx_from_utf8(const char* s, size_t n) {
    mwSize dims[2];
    mxArray* a;
    size_t i = 0, units = 0;
    if (!VX_UTF16) {
        dims[0] = 1; dims[1] = (mwSize)n;
        a = mxCreateCharArray(2, dims);
        if (n) memcpy(mxGetChars(a), s, n);
        return a;
    }
    while (i < n) {
        uint32_t len, c = vx_u8((const unsigned char*)s + i, &len);
        units += c >= 0x10000 ? 2 : 1;
        i += len;
    }
    dims[0] = 1; dims[1] = (mwSize)units;
    a = mxCreateCharArray(2, dims);
    {
        uint16_t* u = (uint16_t*)mxGetChars(a);
        size_t o = 0;
        i = 0;
        while (i < n) {
            uint32_t len, c = vx_u8((const unsigned char*)s + i, &len);
            if (c >= 0x10000) { c -= 0x10000; u[o++] = (uint16_t)(0xD800 + (c >> 10)); u[o++] = (uint16_t)(0xDC00 + (c & 0x3FF)); }
            else u[o++] = (uint16_t)c;
            i += len;
        }
    }
    return a;
}

/* The host offset (0-based: MATLAB UTF-16 units, Octave bytes) of each
 * codepoint of s, and one past the last: n_cp + 1 entries, mxMalloc'd. */
static uint32_t* vx_host_offsets(const char* s, size_t n, uint32_t* n_cp) {
    uint32_t* h = (uint32_t*)mxMalloc(sizeof(uint32_t) * (n + 1));
    size_t i = 0;
    uint32_t k = 0, at = 0;
    while (i < n) {
        uint32_t len, c = vx_u8((const unsigned char*)s + i, &len);
        h[k++] = at;
        at += VX_UTF16 ? (c >= 0x10000 ? 2u : 1u) : len;
        i += len;
    }
    h[k] = at;
    *n_cp = k;
    return h;
}

/* A 1-based host index to a codepoint index (0-based); one that falls
 * inside a character's units is refused. */
static uint32_t vx_host_to_cp(const uint32_t* h, uint32_t n_cp, double idx, const char* what) {
    uint32_t lo = 0, hi = n_cp;
    double z = idx - 1;
    if (z != floor(z) || z < 0 || z > h[n_cp]) vx_err("arg", "%s: a char index, 1 to %u", what, h[n_cp]);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if ((double)h[mid] < z) lo = mid + 1; else hi = mid;
    }
    if ((double)h[lo] != z) vx_err("arg", "%s: char index %g is inside a character (a UTF-16 pair or a UTF-8 sequence)", what, idx);
    return lo;
}

/* ---- arguments of the text commands ----------------------------------------------------- */

/* spans [first last r g b size] (1-based, last included, host char
 * indices; NaN r g b: the text's color; size 0: the text's) to the core's
 * rows (codepoints, end excluded), against utf8, the text they apply to. */
static void vx_spans_fix(yv_kv* kv, int n, const char* utf8) {
    int i, r;
    for (i = 0; i < n; i++) {
        double* v;
        uint32_t* h;
        uint32_t ncp;
        if (strcmp(kv[i].name, "spans")) continue;
        if (kv[i].s || kv[i].obj || kv[i].n % 6) vx_err("arg", "spans: a k x 6 matrix [first last r g b size]");
        v = (double*)kv[i].v;
        h = vx_host_offsets(utf8, strlen(utf8), &ncp);
        for (r = 0; r < kv[i].n / 6; r++) {
            double* q = v + 6 * r;
            uint32_t a = vx_host_to_cp(h, ncp, q[0], "spans: first");
            uint32_t b = vx_host_to_cp(h, ncp, q[1] + 1, "spans: last");
            q[0] = a;
            q[1] = b;
        }
        mxFree(h);
    }
}

/* The text and the stimulus's name-value pairs from prhs[first]: text,
 * font and lang as UTF-8; font a handle, a path or a cell of paths. */
static int vx_text_kvs(int nrhs, const mxArray* prhs[], int first, yv_kv* kv, int start) {
    int i, n = start;
    if ((nrhs - first) % 2) vx_err("usage", "name-value pairs");
    if ((nrhs - first) / 2 + start > VX_MAX_KV) vx_err("arg", "too many parameters");
    for (i = first; i < nrhs; i += 2) {
        char* name = vx_mstring(prhs[i], "a parameter name");
        const mxArray* v = prhs[i + 1];
        if (!strcmp(name, "text") || !strcmp(name, "lang") || (!strcmp(name, "font") && mxIsChar(v))) {
            memset(&kv[n], 0, sizeof kv[n]);
            kv[n].name = name;
            kv[n].s = vx_utf8(v, NULL, name);
            n++;
        } else if (!strcmp(name, "font") && mxIsCell(v)) {
            size_t j, len = 0;
            char* joined = (char*)mxMalloc(1);
            joined[0] = 0;
            for (j = 0; j < mxGetNumberOfElements(v); j++) {
                const mxArray* c = mxGetCell(v, j);
                size_t m;
                char* p;
                char* nj;
                if (!c || !mxIsChar(c)) vx_err("arg", "font: a handle, a path, or a cell of paths (a fallback list)");
                p = vx_utf8(c, &m, "font");
                nj = (char*)mxMalloc(len + m + 2);
                memcpy(nj, joined, len);
                if (len) nj[len++] = '|';
                memcpy(nj + len, p, m + 1);
                len += m;
                joined = nj;
            }
            memset(&kv[n], 0, sizeof kv[n]);
            kv[n].name = name;
            kv[n].s = joined;
            n++;
        } else if (!strcmp(name, "font") && mxIsUint64(v)) {
            memset(&kv[n], 0, sizeof kv[n]);
            kv[n].name = name;
            kv[n].obj = vx_gobj(v, VX_FONT, "font");
            n++;
        } else if (vx_kv(name, v, &kv[n]) == 0) {
            n++;
        }
    }
    return n;
}

static yv_kv* vx_kv_find(yv_kv* kv, int n, const char* name) {
    int i;
    for (i = 0; i < n; i++) if (!strcmp(kv[i].name, name)) return &kv[i];
    return NULL;
}

/* A text stimulus handle. */
static yv_stim* vx_text_of(const mxArray* a, const char* what) {
    yv_stim* st = (yv_stim*)vx_gobj(a, VX_STIM, what);
    char err[256];
    if (st->vkind != YV_K_TEXT) vx_err("handle", "%s: a text handle (ysp_gfx('text', ...))", what);
    if (yv_live(&st->o, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
    return st;
}

/* 'set' on a text: its own parameters (one layout) and the stimulus's. */
static void vx_text_set(yv_stim* st, int nrhs, const mxArray* prhs[]) {
    yv_kv kv[VX_MAX_KV];
    yv_kv* tk;
    char err[512];
    int n, rc;
    n = vx_text_kvs(nrhs, prhs, 2, kv, 0);
    tk = vx_kv_find(kv, n, "text");
    vx_spans_fix(kv, n, tk && tk->s ? tk->s : ((yv_text*)st->text)->utf8);
    rc = yv_text_set(st, kv, n, err, sizeof err);
    if (rc < 0) vx_gfx_fail_msg(rc, err);
}

/* 'get' on a text: its own parameters; 0 when the name is not one. */
static int vx_text_get(yv_stim* st, const char* name, mxArray** out) {
    const yv_text* t = (const yv_text*)st->text;
    double v[2];
    int nv, rc;
    const char* s;
    char buf[4096];
    if (!strcmp(name, "kind")) { *out = mxCreateString("text"); return 1; }
    if (!strcmp(name, "font")) {
        uint64_t id = vx_id_of(t->font);
        *out = id ? vx_handle_out(id) : mxCreateDoubleMatrix(0, 0, mxREAL);
        return 1;
    }
    rc = yv_text_get(st, name, v, &nv, &s, buf, sizeof buf);
    if (rc < 0) return 0;
    if (rc == 1) {   /* spans, back in host indices */
        uint32_t ncp, *h = vx_host_offsets(t->utf8, t->n_bytes, &ncp);
        int i, k;
        *out = mxCreateDoubleMatrix((size_t)t->n_spans, 6, mxREAL);
        for (i = 0; i < t->n_spans; i++) {
            const double* q = t->span_v + 6 * i;
            for (k = 0; k < 6; k++) {
                double x = q[k];
                if (k == 0) x = h[(uint32_t)q[0] <= ncp ? (uint32_t)q[0] : ncp] + 1.0;
                if (k == 1) x = h[(uint32_t)q[1] <= ncp ? (uint32_t)q[1] : ncp];
                mxGetPr(*out)[i + (size_t)k * (size_t)t->n_spans] = x;
            }
        }
        mxFree(h);
        return 1;
    }
    if (!strcmp(name, "text")) { *out = vx_from_utf8(s, strlen(s)); return 1; }
    if (s) { *out = vx_from_utf8(s, strlen(s)); return 1; }
    *out = mxCreateDoubleMatrix(1, (size_t)nv, mxREAL);
    memcpy(mxGetPr(*out), v, sizeof(double) * (size_t)nv);
    return 1;
}

/* ---- the commands --------------------------------------------------------------------- */

/* 1 when cmd is a text command (done), 0 when it is not one. */
static int vx_text_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char err[512];
    if (!strcmp(cmd, "font")) {
        /* f = ysp_gfx('font', g, src, 'face', k, 'name', s): src a path, uint8 bytes, or a cell of them */
        yv_gfx* g;
        yv_font_src fs[YV_FONT_MAX];
        const double* faces = NULL;
        int nfaces = 0, nsrc, i, code;
        char* name = NULL;
        yv_font* f;
        vx_nargs(nrhs, 3, "f = ysp_gfx('font', g, path_or_bytes_or_cell, 'face', k, 'name', s)");
        g = vx_gfx(prhs[1]);
        if ((nrhs - 3) % 2) vx_err("usage", "font: name-value pairs after the source");
        for (i = 3; i < nrhs; i += 2) {
            char nm[16];
            vx_string(prhs[i], nm, sizeof nm, "font: a name");
            if (!strcmp(nm, "face")) faces = vx_doubles(prhs[i + 1], &nfaces, "face");
            else if (!strcmp(nm, "name")) name = vx_utf8(prhs[i + 1], NULL, "name");
            else vx_err("arg", "font: 'face' or 'name'");
        }
        nsrc = mxIsCell(prhs[2]) ? (int)mxGetNumberOfElements(prhs[2]) : 1;
        if (nsrc < 1 || nsrc > YV_FONT_MAX) vx_err("arg", "font: 1 to %d files", YV_FONT_MAX);
        if (nfaces != 0 && nfaces != 1 && nfaces != nsrc) vx_err("arg", "face: one, or one per file");
        memset(fs, 0, sizeof fs);
        for (i = 0; i < nsrc; i++) {
            const mxArray* s = mxIsCell(prhs[2]) ? mxGetCell(prhs[2], (mwIndex)i) : prhs[2];
            double fc = nfaces ? faces[nfaces == 1 ? 0 : i] : 0;
            if (fc != floor(fc) || fc < 0) vx_err("arg", "face: 0 or more (a .ttc's face)");
            fs[i].face = (int)fc;
            fs[i].name = name;
            if (s && mxIsChar(s)) fs[i].path = vx_utf8(s, NULL, "font");
            else if (s && mxIsUint8(s)) { fs[i].bytes = mxGetData(s); fs[i].n = mxGetNumberOfElements(s); }
            else vx_err("arg", "font: a path, uint8 bytes of a font file, or a cell of them");
        }
        f = yv_font_make(g, fs, nsrc, NULL, &code, err, sizeof err);
        if (!f) vx_gfx_fail_msg(code, err);
        vx_new_handle(&f->o, VX_FONT, &plhs[0]);
    } else if (!strcmp(cmd, "font_info")) {
        /* s = ysp_gfx('font_info', f): per file name, face, sha256, units_per_em, glyphs, ascender, descender, line_gap, built */
        static const char* fn[] = { "name", "face", "sha256", "units_per_em", "glyphs", "ascender", "descender", "line_gap",
                                    "built", "build_time" };
        yv_font* f;
        int i;
        mxArray* s;
        vx_nargs(nrhs, 2, "s = ysp_gfx('font_info', f)");
        f = (yv_font*)vx_gobj(prhs[1], VX_FONT, "font_info");
        s = mxCreateStructMatrix(1, (size_t)f->n, 10, fn);
        for (i = 0; i < f->n; i++) {
            ylay_font_info fi;
            if (yv_font_info(f, i, &fi) < 0) continue;
            mxSetFieldByNumber(s, (mwIndex)i, 0, vx_from_utf8(fi.name, strlen(fi.name)));
            mxSetFieldByNumber(s, (mwIndex)i, 1, vx_d(fi.face));
            mxSetFieldByNumber(s, (mwIndex)i, 2, mxCreateString(fi.sha256));
            mxSetFieldByNumber(s, (mwIndex)i, 3, vx_d(fi.units_per_em));
            mxSetFieldByNumber(s, (mwIndex)i, 4, vx_d(fi.n_glyphs));
            mxSetFieldByNumber(s, (mwIndex)i, 5, vx_d(fi.ascender));
            mxSetFieldByNumber(s, (mwIndex)i, 6, vx_d(fi.descender));
            mxSetFieldByNumber(s, (mwIndex)i, 7, vx_d(fi.line_gap));
            mxSetFieldByNumber(s, (mwIndex)i, 8, vx_d(fi.n_built));
            mxSetFieldByNumber(s, (mwIndex)i, 9, vx_d(fi.build_us / 1e6));
        }
        plhs[0] = s;
    } else if (!strcmp(cmd, "text")) {
        /* t = ysp_gfx('text', g, str, name, value, ...) */
        yv_gfx* g;
        yv_kv kv[VX_MAX_KV + 1];
        yv_kv* tk;
        int n, code;
        yv_stim* st;
        vx_nargs(nrhs, 3, "t = ysp_gfx('text', g, str, name, value, ...)");
        g = vx_gfx(prhs[1]);
        memset(&kv[0], 0, sizeof kv[0]);
        kv[0].name = "text";
        kv[0].s = vx_utf8(prhs[2], NULL, "text");
        n = vx_text_kvs(nrhs, prhs, 3, kv, 1);
        tk = vx_kv_find(kv + 1, n - 1, "text");
        if (tk) vx_err("arg", "text given twice");
        vx_spans_fix(kv, n, kv[0].s);
        st = yv_text_make(g, kv, n, &code, err, sizeof err);
        if (!st) vx_gfx_fail_msg(code, err);
        vx_new_handle(&st->o, VX_STIM, &plhs[0]);
    } else if (!strcmp(cmd, "glyphs") || !strcmp(cmd, "lines")) {
        /* G = ysp_gfx('glyphs', t): n x 6 [char line x0 y0 x1 y1]; L = ysp_gfx('lines', t): [first last x0 y0 x1 y1] */
        yv_stim* st;
        const yv_text* t;
        double* d;
        uint32_t ncp, *h;
        int n, i, k, glyphs = cmd[0] == 'g';
        vx_nargs(nrhs, 2, "G = ysp_gfx('glyphs', t)");
        st = vx_text_of(prhs[1], cmd);
        t = (const yv_text*)st->text;
        d = (double*)mxMalloc(sizeof(double) * 6 * (size_t)((glyphs ? t->b.n_items : t->b.n_lines) + 1));
        n = glyphs ? yv_text_glyphs(st, d) : yv_text_lines(st, d);
        h = vx_host_offsets(t->utf8, t->n_bytes, &ncp);
        plhs[0] = mxCreateDoubleMatrix((size_t)n, 6, mxREAL);
        for (i = 0; i < n; i++) {
            const double* q = d + 6 * i;
            for (k = 0; k < 6; k++) {
                double x = q[k];
                if (glyphs && k == 0) x = h[(uint32_t)q[0] <= ncp ? (uint32_t)q[0] : ncp] + 1.0;
                else if (glyphs && k == 1) x = q[1] + 1;
                else if (!glyphs && k == 0) x = h[(uint32_t)q[0] <= ncp ? (uint32_t)q[0] : ncp] + 1.0;
                else if (!glyphs && k == 1) x = h[(uint32_t)q[1] <= ncp ? (uint32_t)q[1] : ncp];
                mxGetPr(plhs[0])[i + (size_t)k * (size_t)n] = x;
            }
        }
    } else if (!strcmp(cmd, "words")) {
        /* [W, words] = ysp_gfx('words', t): W n x 7 [first last line x0 y0 x1 y1]; words a cell of the strings */
        yv_stim* st;
        const yv_text* t;
        double* d;
        uint32_t ncp, *h;
        int n, i, k, cap;
        vx_nargs(nrhs, 2, "[W, words] = ysp_gfx('words', t)");
        st = vx_text_of(prhs[1], cmd);
        t = (const yv_text*)st->text;
        cap = yv_text_word_count(t);
        d = (double*)mxMalloc(sizeof(double) * 7 * (size_t)(cap + 1));
        n = yv_text_words(st, d, cap);
        if (n < 0) vx_err("memory", "out of memory");
        h = vx_host_offsets(t->utf8, t->n_bytes, &ncp);
        plhs[0] = mxCreateDoubleMatrix((size_t)n, 7, mxREAL);
        if (nlhs > 1) plhs[1] = mxCreateCellMatrix((size_t)n, 1);
        for (i = 0; i < n; i++) {
            const double* q = d + 7 * i;
            uint32_t a = (uint32_t)q[0], b = (uint32_t)q[1];
            for (k = 0; k < 7; k++) {
                double x = q[k];
                if (k == 0) x = h[a] + 1.0;
                else if (k == 1) x = h[b];
                else if (k == 2) x = q[2] + 1;
                mxGetPr(plhs[0])[i + (size_t)k * (size_t)n] = x;
            }
            if (nlhs > 1) mxSetCell(plhs[1], (mwIndex)i, vx_from_utf8(t->utf8 + t->cp_off[a], t->cp_off[b] - t->cp_off[a]));
        }
    } else if (!strcmp(cmd, "_c_text_setup")) {
        int rc;
        vx_nargs(nrhs, 2, "ysp_gfx('_c_text_setup', g)");
        rc = yv_ctext_setup(vx_gfx(prhs[1]), err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "_c_text")) {
        yv_gfx* g;
        int rc;
        vx_nargs(nrhs, 3, "ysp_gfx('_c_text', g, k)");
        g = vx_gfx(prhs[1]);
        if (!g->in_frame) vx_err("order", "_c_text: between 'begin' and 'end'");
        rc = yv_ctext_draw(g, (int)vx_scalar(prhs[2], "k"), err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else {
        return 0;
    }
    (void)nlhs;
    return 1;
}
