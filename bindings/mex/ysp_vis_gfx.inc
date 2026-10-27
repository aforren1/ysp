/* ysp_vis_gfx.inc - the ysp_gfx commands of the ysp-vis core (MATLAB and Octave)
 *
 * Not compiled alone: ysp_vis.c includes it where the gfx commands go. The
 * work is bindings/share/ysp_vis_gfx.h's (the same for Python); this file
 * turns MATLAB values into its yv_kv lists and its results into arrays.
 *
 *     g  = ysp_gfx('open', h, 'background', 0.5);
 *     ga = ysp_gfx('gabor', g, 'sf', 1/32, 'sigma', 24, 'contrast', 0.5);
 *     fx = ysp_gfx('shape', g, 'circle', 'w', 8, 'color', 1);
 *     L  = ysp_gfx('list', g, [ga fx]);
 *     ysp_gfx('bind', g, ga, 'contrast', tl, 1);   % ysp_timeline channel 1, written in 'begin'
 *     f = ysp_screen('begin', h);
 *     ysp_gfx('frame', g, L);                      % begin, bindings, draws, end
 *     ysp_screen('flip', h);
 *
 * Every object is a uint64 handle in the core's one table. Closing a gfx
 * (or its screen) frees its objects and their handles, so a handle kept
 * after that is refused (ysp_gfx:handle). 'free' refuses an object that a
 * list, a stimulus or a binding still uses (ysp_gfx:busy). Channels,
 * elements and list items are 1-based, as in ysp_timeline.
 */

enum { VX_STIM = 20, VX_LIST = 21, VX_GROUP = 22, VX_TEX = 23 };

static const char* vx_kind_name(int k) {
    switch (k) {
    case VX_SCREEN: return "screen";
    case VX_GFX: return "gfx";
    case VX_STIM: return "stimulus";
    case VX_LIST: return "list";
    case VX_GROUP: return "group";
    case VX_TEX: return "texture";
    default: return "object";
    }
}

/* Any handle of the table: its node, or an error. */
static vx_node* vx_node_of(const mxArray* a, const char* what) {
    uint64_t id = 0;
    vx_node* n;
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) id = *(const uint64_t*)mxGetData(a);
    else vx_err("handle", "%s: a handle (uint64)", what);
    for (n = vx_list; n; n = n->next)
        if (n->id == id) return n;
    vx_err("handle", "%s: invalid or closed handle (closing a gfx or its screen frees its objects)", what);
    return NULL;
}

static yv_obj* vx_gobj(const mxArray* a, int kind, const char* what) {
    vx_node* n = vx_node_of(a, what);
    if (kind && n->kind != kind) vx_err("handle", "%s: a %s handle is needed here, not a %s", what, vx_kind_name(kind), vx_kind_name(n->kind));
    if (n->kind < VX_STIM) vx_err("handle", "%s: a stimulus, list, group or texture handle", what);
    return (yv_obj*)n->obj;
}

static uint64_t vx_id_of(const void* obj) {
    vx_node* n;
    for (n = vx_list; n; n = n->next) if (n->obj == obj) return n->id;
    return 0;
}

/* Frees every object of g and its handle, then g itself. */
static void vx_gfx_drop(yv_gfx* g) {
    vx_node** p = &vx_list;
    if (!g) return;
    /* lists first: they point at stimuli; stimuli before groups and textures */
    {
        int pass;
        for (pass = 0; pass < 3; pass++) {
            p = &vx_list;
            while (*p) {
                vx_node* n = *p;
                int k = n->kind;
                if (k >= VX_STIM && ((yv_obj*)n->obj)->g == g &&
                    ((pass == 0 && k == VX_LIST) || (pass == 1 && k == VX_STIM) || (pass == 2 && (k == VX_GROUP || k == VX_TEX)))) {
                    *p = n->next;
                    if (k == VX_LIST) yv_list_free((yv_list*)n->obj);
                    else if (k == VX_STIM) yv_stim_free((yv_stim*)n->obj);
                    else if (k == VX_GROUP) yv_group_free((yv_group*)n->obj);
                    else yv_tex_free((yv_tex*)n->obj);
                    free(n);
                } else {
                    p = &n->next;
                }
            }
        }
    }
    vx_unregister(g);
    yv_gfx_close(g);
    free(g);
}

static yv_gfx* vx_gfx(const mxArray* a) {
    yv_gfx* g = (yv_gfx*)vx_lookup(a, VX_GFX, "gfx");
    if (!g->open) vx_err("closed", "the gfx (or its screen) is closed");
    return g;
}

static void vx_gfx_fail_msg(int rc, const char* err) {
    if (rc == YV_ERR_HOST) vx_err("timeline", "%s", err[0] ? err : "a timeline callback failed");
    vx_err(vx_gfx_code(rc), "%s", err && err[0] ? err : ygfx_strerror(rc));
}

static void vx_gfx_fail(yv_gfx* g, int rc) {
    const char* m = ygfx_error(&g->g);
    vx_err(vx_gfx_code(rc), "%s", m && m[0] ? m : ygfx_strerror(rc));
}

static void vx_rgb(const mxArray* a, float out[3], const char* what) {
    size_t n = mxGetNumberOfElements(a);
    if (!mxIsDouble(a) || mxIsComplex(a) || (n != 1 && n != 3)) vx_err("arg", "%s: a gray level or [r g b], linear 0..1", what);
    out[0] = (float)mxGetPr(a)[0];
    out[1] = (float)mxGetPr(a)[n == 3 ? 1 : 0];
    out[2] = (float)mxGetPr(a)[n == 3 ? 2 : 0];
}

/* ---- arguments: MATLAB values to the core's yv_kv (mxMalloc: freed on error) ---- */

#define VX_MAX_KV 64

/* Numbers in row-major order: an m x k matrix row by row (n x 2 positions
 * are x1 y1 x2 y2 ...); a vector as it is. */
static const double* vx_doubles(const mxArray* a, int* n, const char* what) {
    size_t m = mxGetM(a), k = mxGetN(a), i, j, cnt = mxGetNumberOfElements(a);
    double* d;
    if ((!mxIsNumeric(a) && !mxIsLogical(a)) || mxIsComplex(a) || mxIsSparse(a))
        vx_err("arg", "%s: real numbers", what);
    if (mxGetNumberOfDimensions(a) > 2) vx_err("arg", "%s: a vector or a matrix", what);
    if (cnt > (size_t)INT32_MAX / 2) vx_err("arg", "%s: too many values", what);
    d = (double*)mxMalloc(sizeof(double) * (cnt ? cnt : 1));
    for (i = 0; i < m; i++)
        for (j = 0; j < k; j++) {
            size_t src = i + j * m, dst = (m == 1 || k == 1) ? src : i * k + j;
            double v;
            switch (mxGetClassID(a)) {
            case mxDOUBLE_CLASS: v = ((const double*)mxGetData(a))[src]; break;
            case mxSINGLE_CLASS: v = ((const float*)mxGetData(a))[src]; break;
            case mxLOGICAL_CLASS: v = ((const mxLogical*)mxGetData(a))[src] ? 1.0 : 0.0; break;
            case mxINT8_CLASS: v = ((const int8_t*)mxGetData(a))[src]; break;
            case mxUINT8_CLASS: v = ((const uint8_t*)mxGetData(a))[src]; break;
            case mxINT16_CLASS: v = ((const int16_t*)mxGetData(a))[src]; break;
            case mxUINT16_CLASS: v = ((const uint16_t*)mxGetData(a))[src]; break;
            case mxINT32_CLASS: v = ((const int32_t*)mxGetData(a))[src]; break;
            case mxUINT32_CLASS: v = ((const uint32_t*)mxGetData(a))[src]; break;
            case mxINT64_CLASS: v = (double)((const int64_t*)mxGetData(a))[src]; break;
            default: v = (double)((const uint64_t*)mxGetData(a))[src]; break;
            }
            d[dst] = v;
        }
    *n = (int)cnt;
    return d;
}

static char* vx_mstring(const mxArray* a, const char* what) {
    size_t n = mxGetNumberOfElements(a) + 1;
    char* s = (char*)mxMalloc(n);
    if (!mxIsChar(a) || mxGetString(a, s, (mwSize)n) != 0) vx_err("arg", "%s: a string", what);
    return s;
}

/* A cell array of names, or a char row: "a,b,c". */
static char* vx_names_joined(const mxArray* a, const char* what) {
    size_t i, n, len = 0;
    char* out;
    if (mxIsChar(a)) return vx_mstring(a, what);
    if (!mxIsCell(a)) vx_err("arg", "%s: a name or a cell array of names", what);
    n = mxGetNumberOfElements(a);
    out = (char*)mxMalloc(32 * n + 1);
    out[0] = 0;
    for (i = 0; i < n; i++) {
        const mxArray* c = mxGetCell(a, i);
        char buf[32];
        if (!c || !mxIsChar(c) || mxGetString(c, buf, sizeof buf) != 0) vx_err("arg", "%s: names", what);
        len += (size_t)snprintf(out + len, 32 * n + 1 - len, "%s%s", i ? "," : "", buf);
    }
    return out;
}

/* One value; 1 for [] (the default: skipped). */
static int vx_kv(const char* name, const mxArray* v, yv_kv* k) {
    memset(k, 0, sizeof *k);
    k->name = name;
    if (mxIsEmpty(v) && !mxIsChar(v)) return 1;
    if (mxIsChar(v)) { k->s = vx_mstring(v, name); return 0; }
    if (mxIsUint64(v) && mxGetNumberOfElements(v) == 1 && (!strcmp(name, "group") || !strcmp(name, "tex"))) {
        k->obj = vx_gobj(v, !strcmp(name, "group") ? VX_GROUP : VX_TEX, name);
        return 0;
    }
    if (mxIsStruct(v) || mxIsCell(v)) vx_err("arg", "%s: numbers or a name", name);
    k->v = vx_doubles(v, &k->n, name);
    return 0;
}

/* Name-value pairs (or one struct) from prhs[first]: the count. */
static int vx_kvs(int nrhs, const mxArray* prhs[], int first, yv_kv* kv) {
    int i, n = 0;
    if (nrhs > first && mxIsStruct(prhs[first])) {
        int f;
        if (mxGetNumberOfElements(prhs[first]) != 1) vx_err("arg", "a 1x1 struct of parameters");
        if (nrhs > first + 1) vx_err("usage", "a struct, or name-value pairs, not both");
        for (f = 0; f < mxGetNumberOfFields(prhs[first]) && n < VX_MAX_KV; f++)
            if (vx_kv(mxGetFieldNameByNumber(prhs[first], f), mxGetFieldByNumber(prhs[first], 0, f), &kv[n]) == 0) n++;
        return n;
    }
    if ((nrhs - first) % 2) vx_err("usage", "name-value pairs");
    if ((nrhs - first) / 2 > VX_MAX_KV) vx_err("arg", "too many parameters");
    for (i = first; i < nrhs; i += 2) {
        char* name = vx_mstring(prhs[i], "a parameter name");
        if (vx_kv(name, prhs[i + 1], &kv[n]) == 0) n++;
    }
    return n;
}

/* ysp_screen('open', ..., 'gfx', opts): true or a struct of gfx options; the
 * gfx the screen's on_context starts, which ysp_gfx('open', h) finishes. */
static struct yv_gfx* vx_gstart(const mxArray* opts) {
    yv_gfx* g;
    yv_kv kv[VX_MAX_KV];
    int n = 0, rc;
    char err[512];
    if (mxIsStruct(opts)) {
        const mxArray* pr[1];
        pr[0] = opts;
        n = vx_kvs(1, pr, 0, kv);
    } else if ((!mxIsLogical(opts) && !mxIsNumeric(opts)) || !vx_bool(opts, "gfx")) {
        vx_err("arg", "gfx: true or a struct of gfx options");
    }
    g = (yv_gfx*)calloc(1, sizeof *g);
    if (!g) vx_err("memory", "out of memory");
    yv_gfx_defaults(g);
    rc = yv_gfx_options(g, kv, n, err, sizeof err);
    if (rc < 0) { free(g); vx_gfx_fail_msg(rc, err); }
    return g;
}

/* Bindings that write o: the users that are not lists or stimuli. */
static int vx_binds_of(const yv_gfx* g, const yv_obj* o) {
    int i, j, n = 0;
    for (i = 0; i < g->n_tl; i++)
        for (j = 0; j < g->tl[i].n; j++) if (g->tl[i].who[j] == o) n++;
    return n;
}

/* ---- results ------------------------------------------------------------------------- */

static mxArray* vx_stim_value(const yv_stim* st, const yv_attr* a) {
    double v[32];
    int n, i;
    char err[256];
    mxArray* r;
    if (yv_attr_get(st, a, v, &n, err, sizeof err) < 0) vx_err("arg", "%s", err);
    if (a->type == YV_A_ENUM) {
        const ygfx_param* t = ygfx_enum_params(NULL);
        char buf[48];
        if (!strcmp(t[a->row].type, "bool")) return mxCreateLogicalScalar(v[0] != 0);
        if (!t[a->row].values) return vx_d(v[0]);
        return mxCreateString(yv_enum_name(t[a->row].values, (int)v[0], buf, sizeof buf));
    }
    r = mxCreateDoubleMatrix(1, (size_t)n, mxREAL);
    for (i = 0; i < n; i++) mxGetPr(r)[i] = v[i];
    return r;
}

/* Every parameter of st's kind, as a struct. */
static mxArray* vx_stim_all(const yv_stim* st) {
    const char* names[160];
    const ygfx_param* t;
    int n, i, k = 0, pass;
    mxArray* s;
    static const char* vecs[] = { "color", "dir", "tint", "src", "shape_p", "trim" };
    for (i = 0; i < 6; i++) {
        yv_attr a;
        char err[64];
        if (yv_attr_find(vecs[i], st->s.kind, st->s.shape, &a, err, sizeof err) == 0) names[k++] = vecs[i];
    }
    for (pass = 0; pass < 2; pass++) {
        t = pass ? ygfx_enum_params(&n) : ygfx_params(&n);
        for (i = 0; i < n && k < 158; i++) {
            const char* nm = t[i].name;
            int j, dup = 0;
            if (!(t[i].kinds & YV_KB(st->s.kind))) continue;
            if (!pass && (nm[0] == 'p' && nm[1] >= '0' && nm[1] <= '9')) continue;   /* p0..p31: in shape_p's place, not listed */
            if (!pass && (strstr(nm, "color_") == nm || strstr(nm, "dir_") == nm || strstr(nm, "tint_") == nm ||
                          strstr(nm, "src_") == nm || strstr(nm, "shape_p") == nm || strstr(nm, "trim_") == nm))
                continue;
            for (j = 0; j < k; j++) if (!strcmp(names[j], nm)) dup = 1;
            if (!dup) names[k++] = nm;
        }
    }
    s = mxCreateStructMatrix(1, 1, k, names);
    for (i = 0; i < k; i++) {
        yv_attr a;
        char err[256];
        if (yv_attr_find(names[i], st->s.kind, st->s.shape, &a, err, sizeof err) == 0)
            mxSetFieldByNumber(s, 0, i, vx_stim_value(st, &a));
    }
    return s;
}

static mxArray* vx_capture_stats(const ygfx_capture_stats* c) {
    static const char* f[] = { "chosen", "dropped", "read", "written", "failed", "no_record", "busy", "busy_max", "slots", "manual" };
    mxArray* s = mxCreateStructMatrix(1, 1, 10, f);
    mxSetFieldByNumber(s, 0, 0, vx_d((double)c->chosen));
    mxSetFieldByNumber(s, 0, 1, vx_d((double)c->dropped));
    mxSetFieldByNumber(s, 0, 2, vx_d((double)c->read));
    mxSetFieldByNumber(s, 0, 3, vx_d((double)c->written));
    mxSetFieldByNumber(s, 0, 4, vx_d((double)c->failed));
    mxSetFieldByNumber(s, 0, 5, vx_d((double)c->no_record));
    mxSetFieldByNumber(s, 0, 6, vx_d(c->busy));
    mxSetFieldByNumber(s, 0, 7, vx_d(c->busy_max));
    mxSetFieldByNumber(s, 0, 8, vx_d(c->slots));
    mxSetFieldByNumber(s, 0, 9, mxCreateLogicalScalar(c->manual != 0));
    return s;
}

/* ---- textures: MATLAB matrices to texels (rows top first, interleaved) ---------------- */

static void* vx_pixels(const mxArray* a, int* w, int* h, int* format) {
    const mwSize* dims = mxGetDimensions(a);
    size_t nd = mxGetNumberOfDimensions(a), H = dims[0], W = dims[1], C = nd >= 3 ? dims[2] : 1, r, c, ch;
    int f = *format;
    if (nd > 3 || H < 1 || W < 1 || H > 65536 || W > 65536) vx_err("arg", "texture: an H x W or H x W x C matrix");
    if (mxIsUint8(a)) {
        unsigned char* out;
        const unsigned char* in = (const unsigned char*)mxGetData(a);
        int oc;
        if (C < 1 || C > 4) vx_err("arg", "texture: uint8 with 1 to 4 channels");
        if (!f) f = C == 1 ? YGFX_R8 : C == 2 ? YGFX_RG8 : YGFX_RGBA8;
        if ((f == YGFX_R8 && C != 1) || (f == YGFX_RG8 && C != 2) || (f == YGFX_RGBA8 && C < 3) ||
            (f != YGFX_R8 && f != YGFX_RG8 && f != YGFX_RGBA8))
            vx_err("arg", "texture: the format does not fit the uint8 matrix's channels");
        oc = yv_format_bytes(f);
        out = (unsigned char*)mxMalloc(H * W * (size_t)oc);
        for (r = 0; r < H; r++)
            for (c = 0; c < W; c++)
                for (ch = 0; ch < (size_t)oc; ch++)
                    out[(r * W + c) * (size_t)oc + ch] = ch < C ? in[r + c * H + ch * H * W] : 255;
        *w = (int)W; *h = (int)H; *format = f;
        return out;
    }
    if (mxIsSingle(a) || mxIsDouble(a)) {
        float* out;
        int oc;
        if (C != 1 && C != 3 && C != 4) vx_err("arg", "texture: single or double with 1, 3 or 4 channels");
        if (!f) f = C == 1 ? YGFX_R32F : YGFX_RGBA32F;
        oc = (f == YGFX_R32F || f == YGFX_R16F) ? 1 : 4;
        if ((oc == 1 && C != 1) || (oc == 4 && C == 1) || (f != YGFX_R32F && f != YGFX_R16F && f != YGFX_RGBA32F && f != YGFX_RGBA16F))
            vx_err("arg", "texture: the format does not fit the float matrix's channels");
        out = (float*)mxMalloc(sizeof(float) * H * W * (size_t)oc);
        for (r = 0; r < H; r++)
            for (c = 0; c < W; c++)
                for (ch = 0; ch < (size_t)oc; ch++) {
                    size_t src = r + c * H + ch * H * W;
                    double v = ch == 3 && C == 3 ? 1.0 : mxIsSingle(a) ? ((const float*)mxGetData(a))[src] : ((const double*)mxGetData(a))[src];
                    out[(r * W + c) * (size_t)oc + ch] = (float)v;
                }
        *w = (int)W; *h = (int)H; *format = f;
        return out;
    }
    if (mxIsUint16(a)) {
        uint16_t* out;
        const uint16_t* in = (const uint16_t*)mxGetData(a);
        if (C != 1 || (f && f != YGFX_R16UI)) vx_err("arg", "texture: uint16 is one channel (r16ui)");
        out = (uint16_t*)mxMalloc(2 * H * W);
        for (r = 0; r < H; r++) for (c = 0; c < W; c++) out[r * W + c] = in[r + c * H];
        *w = (int)W; *h = (int)H; *format = YGFX_R16UI;
        return out;
    }
    vx_err("arg", "texture: uint8, uint16, single or double pixels");
    return NULL;
}

static int vx_format(const mxArray* a) {
    static const struct { const char* n; int f; } fs[] = {
        { "r8", YGFX_R8 }, { "rg8", YGFX_RG8 }, { "rgba8", YGFX_RGBA8 }, { "r16f", YGFX_R16F }, { "rgba16f", YGFX_RGBA16F },
        { "r32f", YGFX_R32F }, { "rgba32f", YGFX_RGBA32F }, { "r16ui", YGFX_R16UI } };
    char s[16];
    int i;
    vx_string(a, s, sizeof s, "format");
    for (i = 0; i < 8; i++) if (yv_lower_eq(s, fs[i].n)) return fs[i].f;
    vx_err("arg", "format: r8, rg8, rgba8, r16f, rgba16f, r32f, rgba32f or r16ui");
    return 0;
}

/* ---- timelines ------------------------------------------------------------------------- */

static ytl_timeline* vx_acquire(const mxArray* a) {
    char err[512];
    uint64_t id;
    ytl_timeline* p;
    if (ysp_timeline_api_import_mex(err, sizeof err) < 0) vx_err("shared", "%s", err);
    if (!mxIsUint64(a) || mxGetNumberOfElements(a) != 1) vx_err("arg", "a ysp_timeline handle (uint64)");
    id = *(const uint64_t*)mxGetData(a);
    p = (ytl_timeline*)ysp_timeline_api_ptr->head.acquire(&id, err, sizeof err);
    if (!p) vx_err("arg", "%s", err);
    return p;
}

/* ---- the commands ------------------------------------------------------------------------- */

static yv_obj* vx_new_handle(yv_obj* o, int kind, mxArray** out) {
    uint64_t id = vx_register(kind, o);
    if (!id) {
        if (kind == VX_STIM) yv_stim_free((yv_stim*)o);
        else if (kind == VX_LIST) yv_list_free((yv_list*)o);
        else if (kind == VX_GROUP) yv_group_free((yv_group*)o);
        else yv_tex_free((yv_tex*)o);
        vx_err("memory", "out of memory");
    }
    *out = vx_handle_out(id);
    return o;
}

static void vx_make(int vkind, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], int first, const char* lead) {
    yv_gfx* g;
    yv_kv kv[VX_MAX_KV + 1];
    int n = 0, code;
    char err[512];
    yv_stim* st;
    (void)nlhs;
    vx_nargs(nrhs, 2, "s = ysp_gfx(kind, g, ...)");
    g = vx_gfx(prhs[1]);
    if (lead) {
        if (nrhs <= first - 1 || !mxIsChar(prhs[first - 1])) vx_err("usage", "s = ysp_gfx('%s', g, %s, ...)", lead, lead);
        if (vx_kv(lead, prhs[first - 1], &kv[0]) == 0) n = 1;
    }
    n += vx_kvs(nrhs, prhs, first, kv + n);
    st = yv_stim_make(g, vkind, kv, n, &code, err, sizeof err);
    if (!st) vx_gfx_fail_msg(code, err);
    vx_new_handle(&st->o, VX_STIM, &plhs[0]);
}

static void vx_gfx_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char err[512];
    int rc;
    if (!strcmp(cmd, "version")) {
        plhs[0] = mxCreateString(YV_MEX_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(YGFX_VERSION_STRING);
    } else if (!strcmp(cmd, "open")) {
        yv_screen* v;
        yv_gfx* g;
        yv_kv kv[VX_MAX_KV];
        int n = 0;
        uint64_t id;
        vx_nargs(nrhs, 2, "g = ysp_gfx('open', screen, name, value, ...)");
        v = vx_scr(prhs[1]);
        if (v->gfx) vx_err("refused", "the screen has a gfx already (one per screen: its output stage writes the whole frame)");
        if (nrhs == 3 && mxIsNumeric(prhs[2])) {   /* phase A: ysp_gfx('open', h, background) */
            n = vx_kv("background", prhs[2], &kv[0]) == 0;
        } else {
            n = vx_kvs(nrhs, prhs, 2, kv);
        }
        if (v->gstart) {
            if (n) vx_err("arg", "the screen started its gfx at open ('gfx' option of ysp_screen('open')): give the options there");
            g = v->gstart;
        } else {
            g = (yv_gfx*)calloc(1, sizeof *g);
            if (!g) vx_err("memory", "out of memory");
            yv_gfx_defaults(g);
            rc = yv_gfx_options(g, kv, n, err, sizeof err);
            if (rc < 0) { free(g); vx_gfx_fail_msg(rc, err); }
        }
        if (!yv_gfx_open(g, v)) {
            char msg[1024];
            snprintf(msg, sizeof msg, "%s", ygfx_error(&g->g));
            if (v->gstart == g) v->gstart = NULL;
            yv_gfx_close(g);
            free(g);
            vx_err("open", "%s", msg);
        }
        id = vx_register(VX_GFX, g);
        if (!id) { yv_gfx_close(g); free(g); vx_err("memory", "out of memory"); }
        plhs[0] = vx_handle_out(id);
    } else if (!strcmp(cmd, "close")) {
        vx_nargs(nrhs, 2, "ysp_gfx('close', g)");
        vx_gfx_drop((yv_gfx*)vx_lookup(prhs[1], VX_GFX, "gfx"));
    } else if (!strcmp(cmd, "begin")) {
        yv_gfx* g;
        vx_nargs(nrhs, 2, "ysp_gfx('begin', g)");
        g = vx_gfx(prhs[1]);
        rc = yv_gfx_begin(g, err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "end")) {
        yv_gfx* g;
        vx_nargs(nrhs, 2, "ysp_gfx('end', g)");
        g = vx_gfx(prhs[1]);
        rc = yv_gfx_end(g, err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "draw") || !strcmp(cmd, "frame")) {
        /* ysp_gfx('draw', g, h1, h2, ...) or with a vector of handles */
        yv_gfx* g;
        yv_obj* objs[256];
        int n = 0, i, bo, bi;
        size_t j;
        vx_nargs(nrhs, 2, "ysp_gfx('frame', g, stimuli_or_lists, ...)");
        g = vx_gfx(prhs[1]);
        for (i = 2; i < nrhs; i++) {
            const mxArray* a = prhs[i];
            if (!mxIsUint64(a)) vx_err("handle", "%s: handles of stimuli and lists", cmd);
            for (j = 0; j < mxGetNumberOfElements(a); j++) {
                uint64_t id = ((const uint64_t*)mxGetData(a))[j];
                vx_node* nd;
                for (nd = vx_list; nd && nd->id != id; nd = nd->next) {}
                if (!nd || (nd->kind != VX_STIM && nd->kind != VX_LIST)) vx_err("handle", "%s: handle %d is not a live stimulus or list", cmd, n + 1);
                if (n == 256) vx_err("arg", "%s: at most 256 handles (put more in a list)", cmd);
                objs[n++] = (yv_obj*)nd->obj;
            }
        }
        if (cmd[0] == 'f') {
            rc = yv_gfx_frame(g, objs, n, &bo, &bi, err, sizeof err);
            if (rc < 0) {
                if (bi >= 0) vx_err(vx_gfx_code(rc), "handle %d, item %d: %s", bo + 1, bi + 1, err);
                if (bo >= 0) vx_err(vx_gfx_code(rc), "handle %d: %s", bo + 1, err);
                vx_gfx_fail_msg(rc, err);
            }
        } else {
            for (i = 0; i < n; i++) {
                rc = yv_gfx_draw(g, objs[i], &bi, err, sizeof err);
                if (rc < 0) {
                    if (bi >= 0) vx_err(vx_gfx_code(rc), "handle %d, item %d: %s", i + 1, bi + 1, err);
                    vx_err(vx_gfx_code(rc), "handle %d: %s", i + 1, err);
                }
            }
        }
    } else if (!strcmp(cmd, "rect")) {
        yv_gfx* g;
        float c[3] = { 1, 1, 1 };
        const double* r;
        ygfx_shape_desc sd;
        ygfx_stim st;
        vx_nargs(nrhs, 3, "ysp_gfx('rect', g, [x y w h], color)");
        g = vx_gfx(prhs[1]);
        if (!mxIsDouble(prhs[2]) || mxIsComplex(prhs[2]) || mxGetNumberOfElements(prhs[2]) != 4)
            vx_err("arg", "rect: [x y w h] in units, x, y the center from the screen's center");
        r = mxGetPr(prhs[2]);
        if (nrhs > 3) vx_rgb(prhs[3], c, "color");
        if (!g->in_frame) vx_err("order", "rect: between 'begin' and 'end'");
        memset(&sd, 0, sizeof sd);
        sd.shape = YGFX_RECT;
        sd.x = (float)r[0]; sd.y = (float)r[1]; sd.w = (float)r[2]; sd.h = (float)r[3];
        sd.color[0] = c[0]; sd.color[1] = c[1]; sd.color[2] = c[2];
        st = ygfx_shape(&sd);
        rc = ygfx_draw(&g->g, &st);
        if (rc < 0) vx_gfx_fail(g, rc);
    } else if (!strcmp(cmd, "shape")) {
        vx_make(YV_K_SHAPE, nlhs, plhs, nrhs, prhs, 3, "shape");
    } else if (!strcmp(cmd, "grating")) {
        vx_make(YV_K_GRATING, nlhs, plhs, nrhs, prhs, 2, NULL);
    } else if (!strcmp(cmd, "gabor")) {
        vx_make(YV_K_GABOR, nlhs, plhs, nrhs, prhs, 2, NULL);
    } else if (!strcmp(cmd, "noise")) {
        vx_make(YV_K_NOISE, nlhs, plhs, nrhs, prhs, 2, NULL);
    } else if (!strcmp(cmd, "dots")) {
        vx_make(YV_K_DOTS, nlhs, plhs, nrhs, prhs, 2, NULL);
    } else if (!strcmp(cmd, "image")) {
        /* ysp_gfx('image', g, tex, ...) */
        yv_gfx* g;
        yv_kv kv[VX_MAX_KV + 1];
        int n, code;
        yv_stim* st;
        vx_nargs(nrhs, 3, "s = ysp_gfx('image', g, tex, ...)");
        g = vx_gfx(prhs[1]);
        memset(&kv[0], 0, sizeof kv[0]);
        kv[0].name = "tex";
        kv[0].obj = vx_gobj(prhs[2], VX_TEX, "image: tex");
        n = 1 + vx_kvs(nrhs, prhs, 3, kv + 1);
        st = yv_stim_make(g, YV_K_IMAGE, kv, n, &code, err, sizeof err);
        if (!st) vx_gfx_fail_msg(code, err);
        vx_new_handle(&st->o, VX_STIM, &plhs[0]);
    } else if (!strcmp(cmd, "instances")) {
        /* ysp_gfx('instances', g, template, n, 'fields', {'xy','ori'}, 'palette', P) */
        yv_gfx* g;
        yv_stim* t;
        yv_stim* st;
        const char* fields = "xy";
        const double* pal = NULL;
        int npal = 0, i, code;
        double n;
        vx_nargs(nrhs, 4, "s = ysp_gfx('instances', g, template, n, 'fields', {...}, 'palette', P)");
        g = vx_gfx(prhs[1]);
        t = (yv_stim*)vx_gobj(prhs[2], VX_STIM, "instances: template");
        n = vx_scalar(prhs[3], "instances: n");
        if (n != floor(n) || n < 1 || n > (1 << 22)) vx_err("arg", "instances: n, a whole number, 1 or more");
        if ((nrhs - 4) % 2) vx_err("usage", "instances: name-value pairs after n");
        for (i = 4; i < nrhs; i += 2) {
            char name[16];
            vx_string(prhs[i], name, sizeof name, "instances: a name");
            if (!strcmp(name, "fields")) fields = vx_names_joined(prhs[i + 1], "fields");
            else if (!strcmp(name, "palette")) {
                pal = vx_doubles(prhs[i + 1], &npal, "palette");
                if (npal % 3) vx_err("arg", "palette: k x 3 [r g b] rows");
                npal /= 3;
            } else vx_err("arg", "instances: 'fields' or 'palette'");
        }
        st = yv_inst_make(t, (int)n, fields, pal, npal, NULL, &code, err, sizeof err);
        if (!st) vx_gfx_fail_msg(code, err);
        (void)g;
        vx_new_handle(&st->o, VX_STIM, &plhs[0]);
    } else if (!strcmp(cmd, "elements")) {
        /* set: ysp_gfx('elements', s, name, values, first); get: E = ysp_gfx('elements', s) (8 x n single) */
        yv_stim* st;
        vx_nargs(nrhs, 2, "ysp_gfx('elements', s, name, values, first) or E = ysp_gfx('elements', s)");
        st = (yv_stim*)vx_gobj(prhs[1], VX_STIM, "elements");
        if (yv_live(&st->o, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
        if (st->vkind != YV_K_INSTANCES) vx_err("arg", "elements: instances only");
        if (nrhs == 2) {
            plhs[0] = mxCreateNumericMatrix(8, st->s.n_inst, mxSINGLE_CLASS, mxREAL);
            memcpy(mxGetData(plhs[0]), st->inst, sizeof(ygfx_inst) * st->s.n_inst);
        } else {
            char name[16];
            int nv;
            double first = 1;
            const double* v;
            vx_nargs(nrhs, 4, "ysp_gfx('elements', s, name, values, first)");
            vx_string(prhs[2], name, sizeof name, "elements: a field name");
            v = vx_doubles(prhs[3], &nv, name);
            if (nrhs > 4) first = vx_scalar(prhs[4], "elements: first");
            if (first != floor(first) || first < 1) vx_err("arg", "elements: first, 1 or more");
            rc = yv_inst_set(st, name, (int)first - 1, v, nv, err, sizeof err);
            if (rc < 0) vx_gfx_fail_msg(rc, err);
        }
    } else if (!strcmp(cmd, "list")) {
        yv_gfx* g;
        yv_list* l;
        int code;
        size_t j;
        vx_nargs(nrhs, 2, "L = ysp_gfx('list', g, [s1 s2 ...])");
        g = vx_gfx(prhs[1]);
        l = yv_list_make(g, &code, err, sizeof err);
        if (!l) vx_gfx_fail_msg(code, err);
        vx_new_handle(&l->o, VX_LIST, &plhs[0]);
        if (nrhs > 2) {
            if (!mxIsUint64(prhs[2])) vx_err("handle", "list: stimulus handles");
            for (j = 0; j < mxGetNumberOfElements(prhs[2]); j++) {
                mxArray* one = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
                *(uint64_t*)mxGetData(one) = ((const uint64_t*)mxGetData(prhs[2]))[j];
                rc = yv_list_add(l, (yv_stim*)vx_gobj(one, VX_STIM, "list"), err, sizeof err);
                mxDestroyArray(one);
                if (rc < 0) vx_gfx_fail_msg(rc, err);
            }
        }
    } else if (!strcmp(cmd, "add") || !strcmp(cmd, "remove") || !strcmp(cmd, "clear") || !strcmp(cmd, "count")) {
        yv_list* l;
        size_t j;
        vx_nargs(nrhs, 2, "ysp_gfx('add', L, [s ...]) / ('remove', L, i) / ('clear', L) / ('count', L)");
        l = (yv_list*)vx_gobj(prhs[1], VX_LIST, cmd);
        if (yv_live(&l->o, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
        if (cmd[0] == 'a') {
            vx_nargs(nrhs, 3, "ysp_gfx('add', L, [s ...])");
            if (!mxIsUint64(prhs[2])) vx_err("handle", "add: stimulus handles");
            for (j = 0; j < mxGetNumberOfElements(prhs[2]); j++) {
                mxArray* one = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
                *(uint64_t*)mxGetData(one) = ((const uint64_t*)mxGetData(prhs[2]))[j];
                rc = yv_list_add(l, (yv_stim*)vx_gobj(one, VX_STIM, "add"), err, sizeof err);
                mxDestroyArray(one);
                if (rc < 0) vx_gfx_fail_msg(rc, err);
            }
        } else if (cmd[0] == 'r') {
            double i;
            vx_nargs(nrhs, 3, "ysp_gfx('remove', L, i)");
            i = vx_scalar(prhs[2], "remove: i");
            if (i != floor(i)) vx_err("arg", "remove: an index, 1-based");
            rc = yv_list_remove(l, (int)i - 1, err, sizeof err);
            if (rc < 0) vx_gfx_fail_msg(rc, err);
        } else if (cmd[1] == 'l') {
            yv_list_clear(l);
        } else {
            plhs[0] = vx_d(l->n);
        }
    } else if (!strcmp(cmd, "group")) {
        yv_gfx* g;
        yv_kv kv[VX_MAX_KV];
        int n, code;
        yv_group* gr;
        vx_nargs(nrhs, 2, "G = ysp_gfx('group', g, name, value, ...)");
        g = vx_gfx(prhs[1]);
        n = vx_kvs(nrhs, prhs, 2, kv);
        gr = yv_group_make(g, kv, n, &code, err, sizeof err);
        if (!gr) vx_gfx_fail_msg(code, err);
        vx_new_handle(&gr->o, VX_GROUP, &plhs[0]);
    } else if (!strcmp(cmd, "texture")) {
        /* ysp_gfx('texture', g, pixels, 'format', f, 'linear', true, 'srgb', true) */
        yv_gfx* g;
        int w = 0, h = 0, f = 0, linear = 0, srgb = 0, code, i;
        void* data;
        yv_tex* t;
        vx_nargs(nrhs, 3, "T = ysp_gfx('texture', g, pixels, 'format', f, 'linear', l, 'srgb', s)");
        g = vx_gfx(prhs[1]);
        if ((nrhs - 3) % 2) vx_err("usage", "texture: name-value pairs after the pixels");
        for (i = 3; i < nrhs; i += 2) {
            char name[16];
            vx_string(prhs[i], name, sizeof name, "texture: a name");
            if (!strcmp(name, "format")) f = vx_format(prhs[i + 1]);
            else if (!strcmp(name, "linear")) linear = vx_bool(prhs[i + 1], "linear");
            else if (!strcmp(name, "srgb")) srgb = vx_bool(prhs[i + 1], "srgb");
            else vx_err("arg", "texture: 'format', 'linear' or 'srgb'");
        }
        data = vx_pixels(prhs[2], &w, &h, &f);
        t = yv_tex_make(g, w, h, f, data, linear, srgb, &code, err, sizeof err);
        if (!t) vx_gfx_fail_msg(code, err);
        vx_new_handle(&t->o, VX_TEX, &plhs[0]);
    } else if (!strcmp(cmd, "update")) {
        /* ysp_gfx('update', T, pixels, [x y]) */
        yv_tex* t;
        int w = 0, h = 0, f, x = 0, y = 0;
        void* data;
        vx_nargs(nrhs, 3, "ysp_gfx('update', T, pixels, [x y])");
        t = (yv_tex*)vx_gobj(prhs[1], VX_TEX, "update");
        f = t->format;
        data = vx_pixels(prhs[2], &w, &h, &f);
        if (f != t->format) vx_err("arg", "update: the pixels' format is not the texture's");
        if (nrhs > 3) {
            if (!mxIsDouble(prhs[3]) || mxGetNumberOfElements(prhs[3]) != 2) vx_err("arg", "update: [x y], 0-based texels from the top-left");
            x = (int)mxGetPr(prhs[3])[0];
            y = (int)mxGetPr(prhs[3])[1];
        }
        rc = yv_tex_update(t, x, y, w, h, data, err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "set")) {
        /* ysp_gfx('set', obj, name, value, ...): a stimulus, a group, or a
         * list (values for every item: n, n x k, or one for all) */
        vx_node* nd;
        yv_kv kv[VX_MAX_KV];
        int n, i;
        vx_nargs(nrhs, 4, "ysp_gfx('set', obj, name, value, ...)");
        nd = vx_node_of(prhs[1], "set");
        if (nd->kind < VX_STIM || nd->kind == VX_TEX) vx_err("handle", "set: a stimulus, list or group handle");
        if (yv_live((yv_obj*)nd->obj, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
        n = vx_kvs(nrhs, prhs, 2, kv);
        if (nd->kind == VX_STIM) {
            rc = yv_set_rest((yv_stim*)nd->obj, kv, n, err, sizeof err);
            if (rc < 0) vx_gfx_fail_msg(rc, err);
        } else if (nd->kind == VX_GROUP) {
            for (i = 0; i < n; i++) {
                rc = yv_group_set((yv_group*)nd->obj, &kv[i], err, sizeof err);
                if (rc < 0) vx_gfx_fail_msg(rc, err);
            }
        } else {
            int space = 0;
            for (i = 0; i < n; i++)
                if (!strcmp(kv[i].name, "space")) {
                    if (!kv[i].s || yv_space_id(kv[i].s, &space, err, sizeof err) < 0) vx_err("arg", "space: a ysp/color.h space name");
                    kv[i].used = 1;
                }
            for (i = 0; i < n; i++) {
                if (kv[i].used) continue;
                if (kv[i].s || kv[i].obj) vx_err("arg", "set: a list takes numbers (%s)", kv[i].name);
                rc = yv_list_set((yv_list*)nd->obj, kv[i].name, kv[i].v, kv[i].n, space, err, sizeof err);
                if (rc < 0) vx_gfx_fail_msg(rc, err);
            }
        }
    } else if (!strcmp(cmd, "get")) {
        /* v = ysp_gfx('get', obj, name); s = ysp_gfx('get', stim) (every parameter) */
        vx_node* nd;
        char name[64];
        vx_nargs(nrhs, 2, "v = ysp_gfx('get', obj, name)");
        nd = vx_node_of(prhs[1], "get");
        if (nd->kind < VX_STIM || nd->kind == VX_TEX) vx_err("handle", "get: a stimulus, list or group handle");
        if (yv_live((yv_obj*)nd->obj, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
        if (nrhs < 3) {
            if (nd->kind != VX_STIM) vx_err("usage", "v = ysp_gfx('get', obj, name)");
            plhs[0] = vx_stim_all((yv_stim*)nd->obj);
            return;
        }
        vx_string(prhs[2], name, sizeof name, "get: a name");
        if (nd->kind == VX_STIM) {
            yv_stim* st = (yv_stim*)nd->obj;
            yv_attr a;
            if (!strcmp(name, "kind")) {
                static const char* kn[] = { "?", "shape", "grating", "gabor", "noise", "image", "dots", "instances" };
                plhs[0] = mxCreateString(kn[st->vkind]);
            } else if (!strcmp(name, "shape") && st->s.kind == YGFX_SHAPE) {
                char buf[32];
                plhs[0] = mxCreateString(yv_enum_name(ygfx_enum_params(NULL)[YGFX_E_SHAPE].values, st->s.shape, buf, sizeof buf));
            } else if (!strcmp(name, "anchor")) {
                plhs[0] = mxCreateDoubleMatrix(1, 2, mxREAL);
                mxGetPr(plhs[0])[0] = st->s.ax;
                mxGetPr(plhs[0])[1] = st->s.ay;
            } else if (!strcmp(name, "n")) {
                plhs[0] = vx_d(st->vkind == YV_K_DOTS ? st->dots_cap : st->s.n_inst);
            } else if (!strcmp(name, "count") && st->vkind == YV_K_DOTS) {
                plhs[0] = vx_d(st->s.count);
            } else if (!strcmp(name, "group")) {
                plhs[0] = st->group ? vx_handle_out(vx_id_of(st->group)) : mxCreateDoubleMatrix(0, 0, mxREAL);
            } else if (!strcmp(name, "tex")) {
                plhs[0] = st->tex ? vx_handle_out(vx_id_of(st->tex)) : mxCreateDoubleMatrix(0, 0, mxREAL);
            } else {
                if (yv_attr_find(name, st->s.kind, st->s.shape, &a, err, sizeof err) < 0 || a.type == YV_A_SPECIAL)
                    vx_err("arg", "%s", a.type == YV_A_SPECIAL ? "get: not read as a value" : err);
                plhs[0] = vx_stim_value(st, &a);
            }
        } else if (nd->kind == VX_GROUP) {
            double v;
            yv_group* gr = (yv_group*)nd->obj;
            if (!strcmp(name, "anchor")) {
                plhs[0] = mxCreateDoubleMatrix(1, 2, mxREAL);
                mxGetPr(plhs[0])[0] = gr->grp.ax;
                mxGetPr(plhs[0])[1] = gr->grp.ay;
            } else {
                if (yv_group_get(gr, name, &v, err, sizeof err) < 0) vx_err("arg", "%s", err);
                plhs[0] = vx_d(v);
            }
        } else {
            yv_list* l = (yv_list*)nd->obj;
            double* buf = (double*)mxMalloc(sizeof(double) * 32 * (size_t)(l->n > 0 ? l->n : 1));
            int k, i, j;
            rc = yv_list_get(l, name, buf, &k, err, sizeof err);
            if (rc < 0) vx_gfx_fail_msg(rc, err);
            plhs[0] = mxCreateDoubleMatrix((size_t)l->n, (size_t)k, mxREAL);
            for (i = 0; i < l->n; i++)
                for (j = 0; j < k; j++) mxGetPr(plhs[0])[i + (size_t)j * (size_t)l->n] = buf[(size_t)i * (size_t)k + (size_t)j];
        }
    } else if (!strcmp(cmd, "copy")) {
        yv_stim* st;
        int code;
        vx_nargs(nrhs, 2, "s2 = ysp_gfx('copy', s)");
        st = yv_stim_copy((yv_stim*)vx_gobj(prhs[1], VX_STIM, "copy"), &code, err, sizeof err);
        if (!st) vx_gfx_fail_msg(code, err);
        vx_new_handle(&st->o, VX_STIM, &plhs[0]);
    } else if (!strcmp(cmd, "free")) {
        vx_node* nd;
        yv_obj* o;
        vx_nargs(nrhs, 2, "ysp_gfx('free', obj)");
        nd = vx_node_of(prhs[1], "free");
        if (nd->kind < VX_STIM) vx_err("handle", "free: a stimulus, list, group or texture handle (ysp_gfx('close', g) for a gfx)");
        o = (yv_obj*)nd->obj;
        /* a list's own users are none; a stimulus in a list, a group or texture a stimulus uses: refused */
        if (nd->kind != VX_LIST) {
            int bound = o->g ? vx_binds_of(o->g, o) : 0;   /* bindings go with it */
            if (o->users - bound > 0)
                vx_err("busy", "free: %d list(s) or stimuli still use this %s; remove it from them first", o->users - bound, vx_kind_name(nd->kind));
        }
        vx_unregister(o);
        if (nd->kind == VX_STIM) yv_stim_free((yv_stim*)o);
        else if (nd->kind == VX_LIST) yv_list_free((yv_list*)o);
        else if (nd->kind == VX_GROUP) yv_group_free((yv_group*)o);
        else yv_tex_free((yv_tex*)o);
    } else if (!strcmp(cmd, "bind")) {
        /* ysp_gfx('bind', g, target, name, tl, channels, first_element) */
        yv_gfx* g;
        yv_obj* t;
        char name[64];
        const double* cv;
        int nc, i, *ch, first = -1;
        ytl_timeline* p;
        vx_nargs(nrhs, 6, "ysp_gfx('bind', g, target, name, timeline, channels, element)");
        g = vx_gfx(prhs[1]);
        t = vx_gobj(prhs[2], 0, "bind: target");
        if (t->type == YV_O_TEX) vx_err("handle", "bind: a stimulus, list or group");
        vx_string(prhs[3], name, sizeof name, "bind: a parameter name");
        cv = vx_doubles(prhs[5], &nc, "bind: channels");
        if (nc < 1) vx_err("arg", "bind: one channel or more");
        ch = (int*)mxMalloc(sizeof(int) * (size_t)nc);
        for (i = 0; i < nc; i++) {
            if (cv[i] != floor(cv[i]) || cv[i] < 1 || cv[i] > YTL_MAX_CHANNELS) vx_err("arg", "bind: channels 1 to %d", YTL_MAX_CHANNELS);
            ch[i] = (int)cv[i] - 1;
        }
        if (nrhs > 6 && !mxIsEmpty(prhs[6])) {
            double e = vx_scalar(prhs[6], "bind: element");
            if (e != floor(e) || e < 1) vx_err("arg", "bind: element, 1 or more");
            first = (int)e - 1;
        }
        p = vx_acquire(prhs[4]);
        rc = yv_bind(g, p, t, name, first, ch, nc, err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "unbind")) {
        /* n = ysp_gfx('unbind', g, target, name, tl): [] or '' match all */
        yv_gfx* g;
        yv_obj* t = NULL;
        char name[64];
        const char* nm = NULL;
        ytl_timeline* p = NULL;
        vx_nargs(nrhs, 2, "n = ysp_gfx('unbind', g, target, name, timeline)");
        g = (yv_gfx*)vx_lookup(prhs[1], VX_GFX, "gfx");
        if (nrhs > 2 && !mxIsEmpty(prhs[2])) t = vx_gobj(prhs[2], 0, "unbind: target");
        if (nrhs > 3 && !mxIsEmpty(prhs[3])) { vx_string(prhs[3], name, sizeof name, "unbind: name"); nm = name; }
        if (nrhs > 4 && !mxIsEmpty(prhs[4])) {
            p = vx_acquire(prhs[4]);
            ysp_timeline_api_ptr->head.release(p);   /* only its address is needed */
        }
        plhs[0] = vx_d(yv_unbind(g, t, nm, p));
    } else if (!strcmp(cmd, "timeline")) {
        /* ysp_gfx('timeline', g, tl, evaluate): 'begin' evaluates tl at the frame's onset */
        yv_gfx* g;
        int on = 1;
        vx_nargs(nrhs, 3, "ysp_gfx('timeline', g, tl, evaluate)");
        g = vx_gfx(prhs[1]);
        if (nrhs > 3) on = vx_bool(prhs[3], "evaluate");
        rc = yv_tl_evaluate(g, vx_acquire(prhs[2]), on, err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "hit")) {
        /* stimulus: true/false; instances: the element (1-based, 0 none); list: [item element] */
        vx_node* nd;
        double x, y;
        vx_nargs(nrhs, 4, "h = ysp_gfx('hit', obj, x, y)");
        nd = vx_node_of(prhs[1], "hit");
        if (nd->kind != VX_STIM && nd->kind != VX_LIST) vx_err("handle", "hit: a stimulus or list");
        if (yv_live((yv_obj*)nd->obj, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
        x = vx_scalar(prhs[2], "x");
        y = vx_scalar(prhs[3], "y");
        if (nd->kind == VX_STIM) {
            yv_stim* st = (yv_stim*)nd->obj;
            int e = yv_stim_hit(st, (float)x, (float)y);
            plhs[0] = st->vkind == YV_K_INSTANCES ? vx_d(e + 1) : mxCreateLogicalScalar(e >= 0);
        } else {
            int e, i = yv_list_hit((yv_list*)nd->obj, (float)x, (float)y, &e);
            plhs[0] = mxCreateDoubleMatrix(1, 2, mxREAL);
            mxGetPr(plhs[0])[0] = i + 1;
            mxGetPr(plhs[0])[1] = e + 1;
        }
    } else if (!strcmp(cmd, "bounds") || !strcmp(cmd, "resolve") || !strcmp(cmd, "length")) {
        yv_stim* st;
        float a, b, c, d;
        vx_nargs(nrhs, 2, "b = ysp_gfx('bounds', s) / ('resolve', s, element) / ('length', s)");
        st = (yv_stim*)vx_gobj(prhs[1], VX_STIM, cmd);
        if (yv_live(&st->o, err, sizeof err) < 0) vx_gfx_fail_msg(YGFX_ERR_CLOSED, err);
        if (cmd[0] == 'b') {
            ygfx_bounds(&st->o.g->g, &st->s, &a, &b, &c, &d);
            plhs[0] = mxCreateDoubleMatrix(1, 4, mxREAL);
            mxGetPr(plhs[0])[0] = a; mxGetPr(plhs[0])[1] = b; mxGetPr(plhs[0])[2] = c; mxGetPr(plhs[0])[3] = d;
        } else if (cmd[0] == 'r') {
            if (nrhs > 2) {
                double e = vx_scalar(prhs[2], "resolve: element");
                if (st->vkind != YV_K_INSTANCES || e != floor(e) || e < 1 || e > st->s.n_inst) vx_err("arg", "resolve: an element of instances, 1-based");
                ygfx_inst_resolve(&st->o.g->g, &st->s, (int)e - 1, &a, &b);
            } else {
                ygfx_resolve(&st->o.g->g, &st->s, &a, &b);
            }
            plhs[0] = mxCreateDoubleMatrix(1, 2, mxREAL);
            mxGetPr(plhs[0])[0] = a; mxGetPr(plhs[0])[1] = b;
        } else {
            double l = ygfx_length(&st->o.g->g, &st->s);
            if (l < 0) vx_err("arg", "length: this stimulus has no length");
            plhs[0] = vx_d(l);
        }
    } else if (!strcmp(cmd, "prime")) {
        yv_gfx* g;
        vx_nargs(nrhs, 2, "n = ysp_gfx('prime', g)");
        g = vx_gfx(prhs[1]);
        if (g->in_frame) vx_err("order", "prime: outside a frame");
        rc = ygfx_prime(&g->g);
        if (rc < 0) vx_gfx_fail(g, rc);
        if (nlhs > 0) plhs[0] = vx_d(rc);
    } else if (!strcmp(cmd, "read_scene") || !strcmp(cmd, "read_output")) {
        /* h x w x 4 single (scene, linear) or uint8 (output codes, after 'end' and before the flip) */
        yv_gfx* g;
        const double* r;
        int x, y, w, h, i, j, k, scene = cmd[5] == 's';
        mwSize dims[3];
        vx_nargs(nrhs, 3, "P = ysp_gfx('read_scene', g, [x y w h])");
        g = vx_gfx(prhs[1]);
        if (!mxIsDouble(prhs[2]) || mxGetNumberOfElements(prhs[2]) != 4) vx_err("arg", "read: [x y w h], px from the top-left");
        r = mxGetPr(prhs[2]);
        x = (int)r[0]; y = (int)r[1]; w = (int)r[2]; h = (int)r[3];
        if (w < 1 || h < 1 || w > 16384 || h > 16384) vx_err("arg", "read: w, h 1 or more");
        dims[0] = (mwSize)h; dims[1] = (mwSize)w; dims[2] = 4;
        if (scene) {
            float* buf = (float*)mxMalloc(sizeof(float) * 4 * (size_t)w * (size_t)h);
            float* out;
            rc = ygfx_read_scene(&g->g, x, y, w, h, buf);
            if (rc < 0) vx_gfx_fail(g, rc);
            plhs[0] = mxCreateNumericArray(3, dims, mxSINGLE_CLASS, mxREAL);
            out = (float*)mxGetData(plhs[0]);
            for (i = 0; i < h; i++) for (j = 0; j < w; j++) for (k = 0; k < 4; k++)
                out[i + (size_t)j * h + (size_t)k * h * w] = buf[((size_t)i * w + j) * 4 + k];
        } else {
            uint8_t* buf = (uint8_t*)mxMalloc(4 * (size_t)w * (size_t)h);
            uint8_t* out;
            rc = ygfx_read_output(&g->g, x, y, w, h, buf);
            if (rc < 0) vx_gfx_fail(g, rc);
            plhs[0] = mxCreateNumericArray(3, dims, mxUINT8_CLASS, mxREAL);
            out = (uint8_t*)mxGetData(plhs[0]);
            for (i = 0; i < h; i++) for (j = 0; j < w; j++) for (k = 0; k < 4; k++)
                out[i + (size_t)j * h + (size_t)k * h * w] = buf[((size_t)i * w + j) * 4 + k];
        }
    } else if (!strcmp(cmd, "_c_scene")) {
        yv_gfx* g;
        vx_nargs(nrhs, 3, "ysp_gfx('_c_scene', g, k)");
        g = vx_gfx(prhs[1]);
        if (!g->in_frame) vx_err("order", "_c_scene: between 'begin' and 'end'");
        rc = yv_gfx_c_scene(g, (int)vx_scalar(prhs[2], "k"), err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
    } else if (!strcmp(cmd, "color") || !strcmp(cmd, "dir")) {
        /* rgb = ysp_gfx('color', g, space, [a b c]) */
        yv_gfx* g;
        char sp[32];
        int space, n;
        const double* v;
        float in[3], out[3];
        vx_nargs(nrhs, 4, "rgb = ysp_gfx('color', g, space, [a b c])");
        g = vx_gfx(prhs[1]);
        vx_string(prhs[2], sp, sizeof sp, "space");
        if (yv_space_id(sp, &space, err, sizeof err) < 0) vx_err("arg", "%s", err);
        v = vx_doubles(prhs[3], &n, "color");
        if (n != 3) vx_err("arg", "%s: three values in the space", cmd);
        in[0] = (float)v[0]; in[1] = (float)v[1]; in[2] = (float)v[2];
        rc = yv_space_rgb(g, space, cmd[0] == 'd', in, out, err, sizeof err);
        if (rc < 0) vx_gfx_fail_msg(rc, err);
        plhs[0] = mxCreateDoubleMatrix(1, 3, mxREAL);
        mxGetPr(plhs[0])[0] = out[0]; mxGetPr(plhs[0])[1] = out[1]; mxGetPr(plhs[0])[2] = out[2];
    } else if (!strcmp(cmd, "capture_start")) {
        /* ysp_gfx('capture_start', g, 'path', p, 'every', n, 'rect', [x y w h], 'slots', k, 'manual', b) */
        yv_gfx* g;
        ygfx_capture_desc d;
        char* path = NULL;
        int i;
        vx_nargs(nrhs, 2, "ysp_gfx('capture_start', g, name, value, ...)");
        g = vx_gfx(prhs[1]);
        memset(&d, 0, sizeof d);
        if ((nrhs - 2) % 2) vx_err("usage", "capture_start: name-value pairs");
        for (i = 2; i < nrhs; i += 2) {
            char name[16];
            vx_string(prhs[i], name, sizeof name, "capture_start: a name");
            if (!strcmp(name, "path")) path = vx_mstring(prhs[i + 1], "path");
            else if (!strcmp(name, "every")) d.every = (int32_t)vx_scalar(prhs[i + 1], "every");
            else if (!strcmp(name, "slots")) d.slots = (int32_t)vx_scalar(prhs[i + 1], "slots");
            else if (!strcmp(name, "manual")) d.manual = vx_bool(prhs[i + 1], "manual") != 0;
            else if (!strcmp(name, "rect")) {
                if (!mxIsDouble(prhs[i + 1]) || mxGetNumberOfElements(prhs[i + 1]) != 4) vx_err("arg", "rect: [x y w h]");
                d.x = (int32_t)mxGetPr(prhs[i + 1])[0]; d.y = (int32_t)mxGetPr(prhs[i + 1])[1];
                d.w = (int32_t)mxGetPr(prhs[i + 1])[2]; d.h = (int32_t)mxGetPr(prhs[i + 1])[3];
            } else vx_err("arg", "capture_start: path, every, rect, slots or manual");
        }
        d.path = path;
        rc = ygfx_capture_start(&g->g, &d);
        if (rc < 0) vx_gfx_fail(g, rc);
    } else if (!strcmp(cmd, "capture_mark")) {
        yv_gfx* g;
        vx_nargs(nrhs, 2, "ysp_gfx('capture_mark', g)");
        g = vx_gfx(prhs[1]);
        rc = ygfx_capture_mark(&g->g);
        if (rc < 0) vx_gfx_fail(g, rc);
    } else if (!strcmp(cmd, "capture_pump")) {
        yv_gfx* g;
        vx_nargs(nrhs, 2, "n = ysp_gfx('capture_pump', g, max)");
        g = vx_gfx(prhs[1]);
        rc = ygfx_capture_pump(&g->g, nrhs > 2 ? (int)vx_scalar(prhs[2], "max") : 0);
        if (rc < 0) vx_gfx_fail(g, rc);
        if (nlhs > 0) plhs[0] = vx_d(rc);
    } else if (!strcmp(cmd, "capture_stop") || !strcmp(cmd, "capture_stats")) {
        yv_gfx* g;
        ygfx_capture_stats s;
        vx_nargs(nrhs, 2, "s = ysp_gfx('capture_stop', g)");
        g = vx_gfx(prhs[1]);
        memset(&s, 0, sizeof s);
        if (cmd[9] == 'o') {
            rc = ygfx_capture_stop(&g->g, &s);
            if (rc < 0) vx_gfx_fail(g, rc);
        } else {
            ygfx_capture_get_stats(&g->g, &s);
        }
        plhs[0] = vx_capture_stats(&s);
    } else if (!strcmp(cmd, "describe")) {
        char line[2048];
        vx_nargs(nrhs, 2, "s = ysp_gfx('describe', g)");
        ygfx_describe(&vx_gfx(prhs[1])->g, line, sizeof line);
        plhs[0] = mxCreateString(line);
    } else if (!strcmp(cmd, "info")) {
        static const char* f[] = { "open", "size", "center", "bindings", "timelines", "clipped", "calibrated", "frames", "in_frame", "cache" };
        yv_gfx* g;
        mxArray* s;
        float a, b;
        vx_nargs(nrhs, 2, "s = ysp_gfx('info', g)");
        g = (yv_gfx*)vx_lookup(prhs[1], VX_GFX, "gfx");
        s = mxCreateStructMatrix(1, 1, 10, f);
        mxSetFieldByNumber(s, 0, 0, mxCreateLogicalScalar(g->open != 0));
        if (g->open) {
            mxArray* m = mxCreateDoubleMatrix(1, 2, mxREAL);
            ygfx_size(&g->g, &a, &b);
            mxGetPr(m)[0] = a; mxGetPr(m)[1] = b;
            mxSetFieldByNumber(s, 0, 1, m);
            m = mxCreateDoubleMatrix(1, 2, mxREAL);
            ygfx_center(&g->g, &a, &b);
            mxGetPr(m)[0] = a; mxGetPr(m)[1] = b;
            mxSetFieldByNumber(s, 0, 2, m);
            mxSetFieldByNumber(s, 0, 5, vx_d((double)ygfx_clipped(&g->g)));
            mxSetFieldByNumber(s, 0, 6, mxCreateLogicalScalar(ygfx_calibrated(&g->g)));
        }
        mxSetFieldByNumber(s, 0, 3, vx_d(yv_bind_count(g)));
        mxSetFieldByNumber(s, 0, 4, vx_d(g->n_tl));
        mxSetFieldByNumber(s, 0, 7, vx_d((double)g->frames));
        mxSetFieldByNumber(s, 0, 8, mxCreateLogicalScalar(g->in_frame != 0));
        mxSetFieldByNumber(s, 0, 9, mxCreateString(g->cache_dir));
        plhs[0] = s;
    } else if (!strcmp(cmd, "params")) {
        /* p = ysp_gfx('params', kind): the parameter rows a kind reads */
        static const char* f[] = { "name", "type", "min", "max", "default", "unit", "doc", "values" };
        static const struct { const char* n; int k; } kinds[] = {
            { "shape", YGFX_SHAPE }, { "grating", YGFX_GRATING }, { "gabor", YGFX_GABOR }, { "dots", YGFX_DOTS },
            { "image", YGFX_IMAGE }, { "noise", YGFX_NOISE } };
        uint32_t mask = 0xFFFFFFFFu;
        const ygfx_param* t;
        int n, i, pass, k = 0, total = 0;
        mxArray* s;
        if (nrhs > 1) {
            char kn[16];
            vx_string(prhs[1], kn, sizeof kn, "params: a kind");
            for (i = 0; i < 6 && strcmp(kn, kinds[i].n); i++) {}
            if (i == 6) vx_err("arg", "params: shape, grating, gabor, dots, image or noise");
            mask = YV_KB(kinds[i].k);
        }
        for (pass = 0; pass < 2; pass++) {
            t = pass ? ygfx_enum_params(&n) : ygfx_params(&n);
            for (i = 0; i < n; i++) if (t[i].kinds & mask) total++;
        }
        s = mxCreateStructMatrix(1, (size_t)total, 8, f);
        for (pass = 0; pass < 2; pass++) {
            t = pass ? ygfx_enum_params(&n) : ygfx_params(&n);
            for (i = 0; i < n; i++) {
                if (!(t[i].kinds & mask)) continue;
                mxSetFieldByNumber(s, k, 0, mxCreateString(t[i].name));
                mxSetFieldByNumber(s, k, 1, mxCreateString(t[i].type));
                mxSetFieldByNumber(s, k, 2, vx_d(t[i].min));
                mxSetFieldByNumber(s, k, 3, vx_d(t[i].max));
                mxSetFieldByNumber(s, k, 4, vx_d(t[i].def));
                mxSetFieldByNumber(s, k, 5, mxCreateString(t[i].unit));
                mxSetFieldByNumber(s, k, 6, mxCreateString(t[i].doc));
                mxSetFieldByNumber(s, k, 7, mxCreateString(t[i].values ? t[i].values : ""));
                k++;
            }
        }
        plhs[0] = s;
    } else {
        vx_err("usage", "unknown command '%s'", cmd);
    }
}
