/* ysp_vis_video.inc - the ysp_video commands of the ysp-vis MEX core (ysp_vis.c includes it)
 *
 * Not a translation unit: ysp_vis.c includes it after the gfx commands, so a
 * movie handle sits in the one handle table beside screens and gfx. The part
 * both hosts share is bindings/share/ysp_vis_video.h.
 *
 *     m = ysp_video('open', g, 'clip.mp4');          % the .yspvi index beside it
 *     ysp_video('play', m);                          % as soon as the frames are ready
 *     while true
 *         f = ysp_screen('begin', h);
 *         playing = ysp_video('update', m);          % before ysp_gfx('begin')
 *         ysp_gfx('begin', g); ysp_video('draw', m); ysp_gfx('end', g);
 *         ysp_screen('flip', h);
 *         if ~playing, break; end
 *     end
 *     ysp_screen('wait_flip', h);
 *     r = ysp_video('records', m);                   % one record per display frame
 *     ysp_video('close', m);
 *
 * TIMES: seconds; records carry the exact ns beside each time (int64).
 * PINS: a ysp_audio handle given to 'soundtrack' or 'follow_audio' and a
 * ysp_timeline handle given to 'open' are pinned through their owners'
 * tables until the movie closes: ysp_audio('close', a) is ysp_audio:busy
 * meanwhile. PATHS: char arrays as mxGetString gives them; ysp/video.h opens
 * them with fopen().
 */

enum { VX_MOVIE = 3 };

/* ysp_vis_video.h: a pin goes back at once (no lock in MEX). */
static void yv_video_release(void (*release)(void*), void* obj) { release(obj); }

static const char* vx_video_code(int code) {
    switch (code) {
    case YVID_ERR_ARG: return "arg";
    case YVID_ERR_CLOSED: return "closed";
    case YVID_ERR_FORMAT: return "format";
    case YVID_ERR_ORDER: return "order";
    case YVID_ERR_FULL: return "full";
    case YVID_ERR_DECODER: return "decoder";
    case YVID_ERR_REFUSED: return "refused";
    case YVID_ERR_LOST: return "lost";
    case YVID_ERR_IO: return "io";
    case YVID_ERR_NOT_FOUND: return "not_found";
    case YVID_ERR_NOT_IMPLEMENTED: return "not_implemented";
    default: return "error";
    }
}

/* Any movie, open or closed (with its gfx or screen). */
static yv_movie* vx_movie_any(const mxArray* a) {
    uint64_t id = 0;
    vx_node* n;
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) id = *(const uint64_t*)mxGetData(a);
    else vx_err("handle", "a movie handle (uint64) from ysp_video('open')");
    for (n = vx_list; n; n = n->next)
        if (n->id == id) {
            if (n->kind != VX_MOVIE) vx_err("handle", "a movie handle is needed here");
            return (yv_movie*)n->obj;
        }
    vx_err("handle", "invalid or closed movie handle");
    return NULL;
}

static yv_movie* vx_movie(const mxArray* a) {
    yv_movie* mv = vx_movie_any(a);
    if (!mv->open) {
        yv_movie_unpin(mv);   /* closed with its gfx or screen */
        vx_err("closed", "the movie is closed (or its gfx or screen)");
    }
    return mv;
}

static void vx_video_fail(yv_movie* mv, int rc, const char* why) {
    const char* m = why ? why : yvid_error(&mv->m);
    vx_err(vx_video_code(rc), "%s", m && m[0] ? m : yvid_strerror(rc));
}

/* Closes and frees every movie: before the gfx and screens (vx_close_all). */
static void vx_video_close_all(void) {
    vx_node** p = &vx_list;
    while (*p) {
        vx_node* n = *p;
        if (n->kind == VX_MOVIE) {
            *p = n->next;
            yv_movie_free((yv_movie*)n->obj);
            free(n);
        } else {
            p = &n->next;
        }
    }
}

static mxArray* vx_fields(const yv_fields* f) {
    const char* names[YVF_MAX];
    mxArray* s;
    int i;
    for (i = 0; i < f->n; i++) names[i] = f->f[i].name;
    s = mxCreateStructMatrix(1, 1, f->n, names);
    for (i = 0; i < f->n; i++) {
        const yv_field* x = &f->f[i];
        mxArray* v;
        switch (x->type) {
        case YVF_S: v = vx_d(yv_s(x->i)); break;
        case YVF_NS: v = vx_i64(x->i); break;
        case YVF_INT: v = vx_d((double)x->i); break;
        case YVF_NUM: v = vx_d(x->d); break;
        case YVF_BOOL: v = mxCreateLogicalScalar(x->i != 0); break;
        default: v = mxCreateString(x->s ? x->s : ""); break;
        }
        mxSetFieldByNumber(s, 0, i, v);
    }
    return s;
}

static const char* vx_vrec_f[] = { "display", "frame", "onset", "onset_ns", "due", "due_ns", "residual", "residual_ns",
                                   "movie_t", "movie_t_ns", "decision", "why", "tier", "path", "flags", "ahead", "shows" };

static void vx_vrec_set(mxArray* s, size_t k, int64_t display, int64_t frame, int64_t onset, int64_t due, int64_t movie_t,
                        unsigned decision, unsigned why, unsigned tier, unsigned path, unsigned flags, unsigned ahead,
                        unsigned shows) {
    mxSetFieldByNumber(s, k, 0, vx_d((double)display));
    mxSetFieldByNumber(s, k, 1, vx_d((double)frame));
    mxSetFieldByNumber(s, k, 2, vx_d(yv_s(onset)));
    mxSetFieldByNumber(s, k, 3, vx_i64(onset));
    mxSetFieldByNumber(s, k, 4, vx_d(yv_s(due)));
    mxSetFieldByNumber(s, k, 5, vx_i64(due));
    mxSetFieldByNumber(s, k, 6, vx_d(yv_s(onset - due)));
    mxSetFieldByNumber(s, k, 7, vx_i64(onset - due));
    mxSetFieldByNumber(s, k, 8, vx_d(yv_s(movie_t)));
    mxSetFieldByNumber(s, k, 9, vx_i64(movie_t));
    mxSetFieldByNumber(s, k, 10, mxCreateString(decision < 4 ? yvv_decisions[decision] : "?"));
    mxSetFieldByNumber(s, k, 11, mxCreateString(yvv_why(why)));
    mxSetFieldByNumber(s, k, 12, vx_d(tier));
    mxSetFieldByNumber(s, k, 13, vx_d(path));
    mxSetFieldByNumber(s, k, 14, vx_d(flags));
    mxSetFieldByNumber(s, k, 15, vx_d(ahead));
    mxSetFieldByNumber(s, k, 16, vx_d(shows));
}

static void vx_vrec_ev(mxArray* s, size_t k, const yrt_event* e) {
    const yrt_payload* u = &e->u;
    unsigned w = u->u16[14];
    vx_vrec_set(s, k, u->u32[9], u->i64[0], e->t_ns, u->i64[1], u->i64[2], YVID_EV_DECISION_OF(w), YVID_EV_WHY_OF(w),
                YVID_EV_TIER_OF(w), YVID_EV_PATH_OF(w), u->u32[6], u->u16[15], u->u32[8]);
}

static void vx_import(int audio) {
    char err[512];
    if (audio ? ysp_audio_api_import_mex(err, sizeof err) < 0 : ysp_timeline_api_import_mex(err, sizeof err) < 0)
        vx_err("shared", "%s", err);
}

/* A ysp_audio handle, pinned. */
static yau_audio* vx_audio_pin(const mxArray* a) {
    char err[256];
    void* p;
    uint64_t id;
    if (!mxIsUint64(a) || mxGetNumberOfElements(a) != 1) vx_err("arg", "a ysp_audio handle (uint64) from ysp_audio('open')");
    vx_import(1);
    id = *(const uint64_t*)mxGetData(a);
    p = ysp_audio_api_ptr->head.acquire(&id, err, sizeof err);
    if (!p) vx_err("arg", "%s", err);
    return (yau_audio*)p;
}

/* What 'open' takes besides the desc's fields: kept as arrays until every
 * option has parsed, so an error (which leaves through mexErrMsgIdAndTxt)
 * leaks no memory and no pin. */
typedef struct vx_open_args {
    const mxArray* data;
    const mxArray* index;
    const mxArray* timeline;
    int64_t        records;
} vx_open_args;

static void vx_movie_option(yvid_desc* d, vx_open_args* a, const char* name, const mxArray* v) {
    char err[600];
    if (!strcmp(name, "data")) {
        if (!mxIsUint8(v) || mxIsComplex(v)) vx_err("arg", "data: a uint8 array (the file's bytes)");
        a->data = v;
    } else if (!strcmp(name, "index")) {
        if (!mxIsEmpty(v) && !mxIsChar(v) && (!mxIsUint8(v) || mxIsComplex(v))) vx_err("arg", "index: a path, or uint8 bytes");
        a->index = mxIsEmpty(v) ? NULL : v;
    } else if (!strcmp(name, "timeline")) {
        if (!mxIsEmpty(v) && (!mxIsUint64(v) || mxGetNumberOfElements(v) != 1)) vx_err("arg", "timeline: a ysp_timeline handle (uint64)");
        a->timeline = mxIsEmpty(v) ? NULL : v;
    } else if (!strcmp(name, "records")) {
        double r = vx_scalar(v, "records");
        if (r < 0 || r != floor(r) || r > 16777216.0) vx_err("arg", "records: a whole number, 0 to 16777216 (0 = 36000)");
        a->records = (int64_t)r;
    } else {
        const yvv_opt* o = yvv_find_opt(name);
        if (!o) vx_err("arg", "open: unknown option '%s' (ysp_video('options') lists them)", name);
        if (mxIsEmpty(v)) return;
        if (o->type == YVV_ENUM) {
            char s[64];
            vx_string(v, s, sizeof s, name);
            if (yvv_opt_enum(d, o, s, err, sizeof err) < 0) vx_err("arg", "%s", err);
        } else if (yvv_opt_num(d, o, vx_scalar(v, name), err, sizeof err) < 0) {
            vx_err("arg", "%s", err);
        }
    }
}

/* A copy of a uint8 array in the movie's slot k (malloc: it outlives the call). */
static int vx_owned_copy(yv_movie* mv, int k, const mxArray* a, const void** p, size_t* n) {
    *n = mxGetNumberOfElements(a);
    mv->owned[k] = malloc(*n ? *n : 1);
    if (!mv->owned[k]) return -1;
    if (*n) memcpy(mv->owned[k], mxGetData(a), *n);
    *p = mv->owned[k];
    return 0;
}

/* A time argument: absent or [] = ASAP (0), else seconds > 0. */
static int64_t vx_at(int nrhs, const mxArray* prhs[], int k, const char* what) {
    int64_t t;
    if (nrhs <= k || mxIsEmpty(prhs[k])) return YVID_ASAP;
    t = vx_seconds(prhs[k], what, 0);
    if (t == 0) vx_err("arg", "%s: a time > 0 on the ysp_rt clock, or [] for as soon as possible", what);
    return t;
}

/* resume: absent or [] = ASAP, 'paused', or a time. */
static int64_t vx_resume(int nrhs, const mxArray* prhs[], int k) {
    if (nrhs > k && mxIsChar(prhs[k])) {
        char s[16];
        vx_string(prhs[k], s, sizeof s, "resume");
        if (strcmp(s, "paused") != 0) vx_err("arg", "resume: [] (as soon as possible), 'paused' or a time");
        return YVID_STAY_PAUSED;
    }
    return vx_at(nrhs, prhs, k, "resume");
}

static mxArray* vx_id(yv_movie* mv, int64_t id) {
    if (id < 0) vx_video_fail(mv, (int)id, NULL);
    return vx_d((double)id);
}

static uint8_t vx_color_opt(const mxArray* v, const yv_name* t, const char* what) {
    char s[32], err[256];
    int x;
    vx_string(v, s, sizeof s, what);
    if (yvv_lookup(t, what, s, &x, err, sizeof err) < 0) vx_err("arg", "%s", err);
    return (uint8_t)x;
}

/* 'write_seq': a whole movie from a MATLAB array, column-major to the rows
 * of a frame. [h w n] or [h w 1 n] one channel, [h w 4 n] RGBA; uint8 (R8,
 * RGBA8), uint16 (R16), single (R32F, RGBA32F). */
static int64_t vx_write_seq(const char* path, const mxArray* a, int32_t num, int32_t den, int qoi, int64_t* frames_out) {
    const mwSize* dim = mxGetDimensions(a);
    mwSize nd = mxGetNumberOfDimensions(a);
    size_t h = dim[0], w = nd > 1 ? dim[1] : 1, c = 1, n = 1, es, x, y, k, i;
    yvid_seq sq;
    yvid_seq_desc d;
    uint8_t* row;
    int rc = 0;
    if (nd == 3) n = dim[2];
    if (nd == 4) { c = dim[2]; n = dim[3]; }
    if (nd > 4 || (c != 1 && c != 4)) vx_err("arg", "write_seq: frames [h w n] (one channel) or [h w 4 n] (RGBA)");
    memset(&d, 0, sizeof d);
    if (mxIsUint8(a)) { d.format = c == 4 ? YVID_FMT_RGBA8 : YVID_FMT_R8; es = 1; }
    else if (mxIsUint16(a) && c == 1) { d.format = YVID_FMT_R16; es = 2; }
    else if (mxIsSingle(a)) { d.format = c == 4 ? YVID_FMT_RGBA32F : YVID_FMT_R32F; es = 4; }
    else { vx_err("arg", "write_seq: uint8 (R8, RGBA8), uint16 (R16, one channel) or single (R32F, RGBA32F)"); return 0; }
    if (mxIsComplex(a) || h == 0 || w == 0 || n == 0 || h > 65535 || w > 65535) vx_err("arg", "write_seq: a real, non-empty array");
    if (qoi && d.format != YVID_FMT_RGBA8) vx_err("arg", "write_seq: 'qoi' is for uint8 RGBA [h w 4 n]");
    d.path = path;
    d.w = (int32_t)w;
    d.h = (int32_t)h;
    d.fps_num = num;
    d.fps_den = den;
    d.compression = qoi ? YVID_SEQ_QOI : YVID_SEQ_RAW;
    d.max_frames = (int64_t)n;
    row = (uint8_t*)mxMalloc(w * h * c * es);
    if (!yvid_seq_open(&sq, &d)) { mxFree(row); vx_err("arg", "%s", yvid_seq_error(&sq)); }
    for (i = 0; i < n && rc == 0; i++) {
        const uint8_t* src = (const uint8_t*)mxGetData(a) + i * h * w * c * es;
        const void* planes[3] = { row, NULL, NULL };
        int32_t strides[3] = { (int32_t)(w * c * es), 0, 0 };
        /* MATLAB's element (y, x, ch) of frame i to row y, texel x, channel ch */
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                for (k = 0; k < c; k++)
                    memcpy(row + ((y * w + x) * c + k) * es, src + ((k * w + x) * h + y) * es, es);
        rc = yvid_seq_write(&sq, planes, strides, NULL);
    }
    mxFree(row);
    if (rc == 0) rc = yvid_seq_finish(&sq);
    if (rc < 0) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s", yvid_seq_error(&sq));
        yvid_seq_close(&sq);
        vx_err(vx_video_code(rc), "%s", msg);
    }
    yvid_seq_close(&sq);
    *frames_out = (int64_t)n;
    return 0;
}

static void vx_video_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char err[640];
    if (!strcmp(cmd, "version")) {
        plhs[0] = mxCreateString(YV_MEX_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(YVID_VERSION_STRING);
    } else if (!strcmp(cmd, "shared")) {
        plhs[0] = vx_shared();
    } else if (!strcmp(cmd, "open")) {
        yv_gfx* g;
        yv_movie* mv;
        yvid_desc d;
        vx_open_args oa;
        char path[1024];
        int first = 3, k, i;
        uint64_t id;
        vx_nargs(nrhs, 3, "m = ysp_video('open', g, path, name, value, ...)  (path '' with 'data')");
        g = vx_gfx(prhs[1]);
        memset(&d, 0, sizeof d);
        memset(&oa, 0, sizeof oa);
        path[0] = 0;
        if (mxIsChar(prhs[2])) vx_string(prhs[2], path, sizeof path, "path");
        else if (mxIsStruct(prhs[2])) first = 2;
        else vx_err("arg", "open: a path, or '' with 'data', then name-value pairs or a struct");
        if (nrhs > first && mxIsStruct(prhs[first])) {
            if (mxGetNumberOfElements(prhs[first]) != 1 || nrhs > first + 1) vx_err("usage", "open: a 1x1 struct of options, or name-value pairs");
            for (k = 0; k < mxGetNumberOfFields(prhs[first]); k++)
                vx_movie_option(&d, &oa, mxGetFieldNameByNumber(prhs[first], k), mxGetFieldByNumber(prhs[first], 0, k));
        } else {
            if ((nrhs - first) % 2) vx_err("usage", "open: name-value pairs");
            for (i = first; i < nrhs; i += 2) {
                char name[64];
                vx_string(prhs[i], name, sizeof name, "an option name");
                vx_movie_option(&d, &oa, name, prhs[i + 1]);
            }
        }
        if ((path[0] != 0) == (oa.data != NULL)) vx_err("arg", "open: a path, or '' with 'data' (uint8), not both");
        if (path[0] && !yvv_exists(path)) vx_err("not_found", "cannot open %s", path);
        if (oa.timeline) vx_import(0);
        /* from here every failure frees mv (and its pins) before it raises */
        mv = (yv_movie*)calloc(1, sizeof *mv);
        if (!mv) vx_err("memory", "out of memory");
        err[0] = 0;
        if (path[0]) d.path = path;
        if (oa.data && vx_owned_copy(mv, 0, oa.data, &d.data, &d.size) < 0) snprintf(err, sizeof err, "out of memory");
        if (!err[0] && oa.index) {
            if (mxIsChar(oa.index)) {
                char ip[1024];
                void* b = NULL;
                if (mxGetString(oa.index, ip, sizeof ip) != 0) snprintf(err, sizeof err, "index: a path of at most 1023 characters");
                else if (yvv_read_file(ip, &b, &d.index_size, err, sizeof err) == 0) { mv->owned[1] = b; d.index = b; }
            } else if (vx_owned_copy(mv, 1, oa.index, &d.index, &d.index_size) < 0) {
                snprintf(err, sizeof err, "out of memory");
            }
        }
        if (!err[0] && oa.timeline) {
            uint64_t tid = *(const uint64_t*)mxGetData(oa.timeline);
            void* p = ysp_timeline_api_ptr->head.acquire(&tid, err, sizeof err);
            if (p) { mv->tl = (ytl_timeline*)p; d.timeline = mv->tl; err[0] = 0; }
            else if (!err[0]) snprintf(err, sizeof err, "timeline: not an open ysp_timeline handle");
        }
        if (err[0]) { yv_movie_free(mv); vx_err("arg", "%s", err); }
        if (!yv_movie_open(mv, g, &d, oa.records, err, sizeof err)) {
            char msg[640];
            snprintf(msg, sizeof msg, "%s", err[0] ? err : "open failed");
            yv_movie_free(mv);
            vx_err("open", "%s", msg);
        }
        id = vx_register(VX_MOVIE, mv);
        if (!id) { yv_movie_free(mv); vx_err("memory", "out of memory"); }
        plhs[0] = vx_handle_out(id);
    } else if (!strcmp(cmd, "close")) {
        yv_movie* mv;
        vx_nargs(nrhs, 2, "ysp_video('close', m)");
        mv = vx_movie_any(prhs[1]);
        vx_unregister(mv);
        yv_movie_free(mv);
    } else if (!strcmp(cmd, "update")) {
        yv_movie* mv;
        const char* why;
        int rc;
        vx_nargs(nrhs, 2, "playing = ysp_video('update', m)");
        mv = vx_movie(prhs[1]);
        rc = yv_movie_update(mv, &why);
        if (rc < 0) vx_video_fail(mv, rc, why);
        plhs[0] = mxCreateLogicalScalar(rc != YVID_ENDED);
    } else if (!strcmp(cmd, "draw")) {
        yv_movie* mv;
        yvid_stim_desc sd;
        const char* why;
        int rc;
        vx_nargs(nrhs, 2, "ysp_video('draw', m, [x y w h], ori, opacity, linear)");
        mv = vx_movie(prhs[1]);
        memset(&sd, 0, sizeof sd);
        sd.opacity = 1.0f;
        if (nrhs > 2 && !mxIsEmpty(prhs[2])) {
            const double* r;
            if (!mxIsDouble(prhs[2]) || mxIsComplex(prhs[2]) || (mxGetNumberOfElements(prhs[2]) != 2 && mxGetNumberOfElements(prhs[2]) != 4))
                vx_err("arg", "draw: [x y] or [x y w h] in pixels (center from the screen's center; w, h 0 = the frame's pixels)");
            r = mxGetPr(prhs[2]);
            sd.x = (float)r[0];
            sd.y = (float)r[1];
            if (mxGetNumberOfElements(prhs[2]) == 4) { sd.w = (float)r[2]; sd.h = (float)r[3]; }
        }
        if (nrhs > 3 && !mxIsEmpty(prhs[3])) sd.ori = (float)vx_scalar(prhs[3], "draw: ori");
        if (nrhs > 4 && !mxIsEmpty(prhs[4])) sd.opacity = (float)vx_scalar(prhs[4], "draw: opacity");
        if (nrhs > 5 && !mxIsEmpty(prhs[5])) sd.linear = vx_bool(prhs[5], "draw: linear");
        rc = yv_movie_draw(mv, &sd, &why);
        if (rc < 0) {
            if (why) vx_video_fail(mv, rc, why);
            vx_err(vx_gfx_code(rc), "%s", ygfx_error(&mv->gfx->g));
        }
    } else if (!strcmp(cmd, "play") || !strcmp(cmd, "pause")) {
        yv_movie* mv;
        int64_t t;
        vx_nargs(nrhs, 2, "id = ysp_video('play' | 'pause', m, t)");
        mv = vx_movie(prhs[1]);
        t = vx_at(nrhs, prhs, 2, cmd);
        plhs[0] = vx_id(mv, cmd[1] == 'l' ? yvid_play_at(&mv->m, t) : yvid_pause_at(&mv->m, t));
    } else if (!strcmp(cmd, "seek")) {
        yv_movie* mv;
        int64_t t, r;
        vx_nargs(nrhs, 3, "id = ysp_video('seek', m, t, resume)");
        mv = vx_movie(prhs[1]);
        t = vx_seconds(prhs[2], "seek: the movie time", 0);
        r = vx_resume(nrhs, prhs, 3);
        plhs[0] = vx_id(mv, yvid_seek(&mv->m, t, r));
    } else if (!strcmp(cmd, "seek_frame")) {
        yv_movie* mv;
        double i;
        int64_t r;
        vx_nargs(nrhs, 3, "id = ysp_video('seek_frame', m, frame, resume)");
        mv = vx_movie(prhs[1]);
        i = vx_scalar(prhs[2], "seek_frame: frame");
        if (i != floor(i) || i < 0) vx_err("arg", "seek_frame: a frame index >= 0");
        r = vx_resume(nrhs, prhs, 3);
        plhs[0] = vx_id(mv, yvid_seek_frame(&mv->m, (int64_t)i, r));
    } else if (!strcmp(cmd, "show")) {
        yv_movie* mv;
        double i;
        int rc;
        vx_nargs(nrhs, 3, "ysp_video('show', m, frame)");
        mv = vx_movie(prhs[1]);
        i = vx_scalar(prhs[2], "show: frame");
        if (i != floor(i)) vx_err("arg", "show: a frame index");
        rc = yvid_show(&mv->m, (int64_t)i);
        if (rc < 0) vx_video_fail(mv, rc, NULL);
    } else if (!strcmp(cmd, "result")) {
        yv_movie* mv;
        yrt_event e;
        yv_fields f;
        double id;
        int rc;
        vx_nargs(nrhs, 3, "r = ysp_video('result', m, id)");
        mv = vx_movie(prhs[1]);
        id = vx_scalar(prhs[2], "result: id");
        rc = yvid_result(&mv->m, (int64_t)id, &e);
        if (rc == YVID_PENDING) { plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL); return; }
        if (rc == YVID_ERR_NOT_FOUND) vx_err("not_found", "no control %.0f (an id from play, pause or seek; the last 8 are kept)", id);
        if (rc < 0) vx_video_fail(mv, rc, NULL);
        yvv_event_fields(&e, &f);
        plhs[0] = vx_fields(&f);
    } else if (!strcmp(cmd, "last")) {
        yv_movie* mv;
        yvid_record r;
        vx_nargs(nrhs, 2, "r = ysp_video('last', m)");
        mv = vx_movie(prhs[1]);
        if (yvid_last(&mv->m, &r) != YVID_OK) { plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL); return; }
        plhs[0] = mxCreateStructMatrix(1, 1, 17, vx_vrec_f);
        vx_vrec_set(plhs[0], 0, r.display, r.frame, r.onset ? r.onset : r.predicted, r.due, r.movie_t, r.decision, r.why,
                    r.tier, r.path, r.flags, r.ahead, r.shows);
    } else if (!strcmp(cmd, "records")) {
        yv_movie* mv;
        yrt_event e;
        size_t k = 0;
        vx_nargs(nrhs, 2, "r = ysp_video('records', m)");
        mv = vx_movie_any(prhs[1]);
        yv_movie_drain(mv);
        plhs[0] = mxCreateStructMatrix(1, mv->fr_n, 17, vx_vrec_f);
        while (yv_movie_take(mv->fr, mv->fr_cap, &mv->fr_head, &mv->fr_n, &e)) vx_vrec_ev(plhs[0], k++, &e);
        if (nlhs > 1) plhs[1] = vx_d((double)(mv->fr_lost + mv->ring_lost));
    } else if (!strcmp(cmd, "events")) {
        yv_movie* mv;
        yrt_event e;
        yv_fields f;
        size_t k = 0;
        vx_nargs(nrhs, 2, "e = ysp_video('events', m)");
        mv = vx_movie_any(prhs[1]);
        yv_movie_drain(mv);
        plhs[0] = mxCreateCellMatrix(1, mv->ev_n);
        while (yv_movie_take(mv->ev, mv->ev_cap, &mv->ev_head, &mv->ev_n, &e)) {
            yvv_event_fields(&e, &f);
            mxSetCell(plhs[0], k++, vx_fields(&f));
        }
        if (nlhs > 1) plhs[1] = vx_d((double)mv->ev_lost);
    } else if (!strcmp(cmd, "movie_time")) {
        yv_movie* mv;
        int64_t t;
        vx_nargs(nrhs, 3, "s = ysp_video('movie_time', m, t)");
        mv = vx_movie(prhs[1]);
        t = vx_seconds(prhs[2], "movie_time: t", 0);
        plhs[0] = vx_d(yv_s(yvid_movie_time(&mv->m, t)));
    } else if (!strcmp(cmd, "soundtrack")) {
        yv_movie* mv;
        yvid_soundtrack_desc sd;
        yau_audio* au;
        char wav[1024];
        int i, follow = 1, rc;
        double ring = -1;
        vx_nargs(nrhs, 4, "ysp_video('soundtrack', m, audio, wav, name, value, ...)  (wav a path or uint8 bytes)");
        mv = vx_movie(prhs[1]);
        if (mv->snd_au) vx_err("order", "the movie has a soundtrack already");
        memset(&sd, 0, sizeof sd);
        if ((nrhs - 4) % 2) vx_err("usage", "soundtrack: name-value pairs after the WAV: db, channels, ramp, ring, follow");
        for (i = 4; i < nrhs; i += 2) {
            char name[32];
            vx_string(prhs[i], name, sizeof name, "an option name");
            if (!strcmp(name, "db")) sd.db = (float)vx_scalar(prhs[i + 1], "db");
            else if (!strcmp(name, "channels")) sd.channels = (uint64_t)vx_scalar(prhs[i + 1], "channels");
            else if (!strcmp(name, "ramp")) sd.ramp_ns = vx_seconds(prhs[i + 1], "ramp", 0);
            else if (!strcmp(name, "ring")) { (void)vx_seconds(prhs[i + 1], "ring", 0); ring = vx_scalar(prhs[i + 1], "ring"); }
            else if (!strcmp(name, "follow")) follow = vx_bool(prhs[i + 1], "follow");
            else vx_err("arg", "soundtrack: unknown option '%s' (db, channels, ramp, ring, follow)", name);
        }
        if (mxIsChar(prhs[3])) {
            vx_string(prhs[3], wav, sizeof wav, "wav");
            sd.path = wav;
        } else if (!mxIsUint8(prhs[3]) || mxIsComplex(prhs[3])) {
            vx_err("arg", "soundtrack: the WAV, a path or uint8 bytes");
        }
        au = vx_audio_pin(prhs[2]);
        if (!sd.path) {
            free(mv->owned[2]);
            mv->owned[2] = NULL;
            if (vx_owned_copy(mv, 2, prhs[3], &sd.data, &sd.size) < 0) { ysp_audio_api_ptr->head.release(au); vx_err("memory", "out of memory"); }
        }
        if (ring >= 0) {
            yau_caps c;
            yau_get_caps(au, &c);
            sd.ring = (int64_t)(ring * c.rate + 0.5);
        }
        rc = yvid_soundtrack(&mv->m, au, &sd);
        if (rc < 0) {
            ysp_audio_api_ptr->head.release(au);
            vx_video_fail(mv, rc, NULL);
        }
        mv->snd_au = au;
        if (follow && !mv->follow_au) mv->follow_au = vx_audio_pin(prhs[2]);
    } else if (!strcmp(cmd, "follow_audio")) {
        yv_movie* mv;
        vx_nargs(nrhs, 3, "ysp_video('follow_audio', m, audio)  ([] stops it)");
        mv = vx_movie(prhs[1]);
        if (mv->follow_au) {
            yau_audio* p = mv->follow_au;
            mv->follow_au = NULL;
            ysp_audio_api_ptr->head.release(p);
        }
        if (!mxIsEmpty(prhs[2])) mv->follow_au = vx_audio_pin(prhs[2]);
    } else if (!strcmp(cmd, "describe")) {
        char line[2048];
        vx_nargs(nrhs, 2, "s = ysp_video('describe', m)");
        yvid_describe(&vx_movie(prhs[1])->m, line, sizeof line);
        plhs[0] = mxCreateString(line);
    } else if (!strcmp(cmd, "info")) {
        yvid_info i;
        yv_fields f;
        mxArray *rb, *db;
        int k, nf;
        const char* wn[YVID_WHY_COUNT];
        vx_nargs(nrhs, 2, "i = ysp_video('info', m)");
        yvid_get_info(&vx_movie(prhs[1])->m, &i);
        yvv_info_fields(&i, &f);
        plhs[0] = vx_fields(&f);
        for (k = 0; k < YVID_WHY_COUNT; k++) wn[k] = yvv_whys[k];
        rb = mxCreateStructMatrix(1, 1, YVID_WHY_COUNT, wn);
        db = mxCreateStructMatrix(1, 1, YVID_WHY_COUNT, wn);
        for (k = 0; k < YVID_WHY_COUNT; k++) {
            mxSetFieldByNumber(rb, 0, k, vx_d((double)i.repeats_by[k]));
            mxSetFieldByNumber(db, 0, k, vx_d((double)i.drops_by[k]));
        }
        nf = mxAddField(plhs[0], "repeats_by");
        mxSetFieldByNumber(plhs[0], 0, nf, rb);
        nf = mxAddField(plhs[0], "drops_by");
        mxSetFieldByNumber(plhs[0], 0, nf, db);
    } else if (!strcmp(cmd, "probe")) {
        yvid_desc d;
        yvid_info i;
        yv_fields f;
        char path[1024];
        void* idx = NULL;
        size_t n = 0;
        int rc;
        vx_nargs(nrhs, 2, "i = ysp_video('probe', path, index)");
        vx_string(prhs[1], path, sizeof path, "probe: path");
        /* yvid_probe() answers a missing file with REFUSED ("reads frame
         * sequences and indexed ... files only"): found here first */
        if (!yvv_exists(path)) vx_err("not_found", "cannot open %s", path);
        memset(&d, 0, sizeof d);
        d.path = path;
        if (nrhs > 2 && mxIsChar(prhs[2])) {
            char ip[1024];
            vx_string(prhs[2], ip, sizeof ip, "probe: index");
            if (yvv_read_file(ip, &idx, &n, err, sizeof err) < 0) vx_err("not_found", "%s", err);
            d.index = idx;
            d.index_size = n;
        }
        err[0] = 0;
        rc = yvid_probe(&d, &i, err, sizeof err);
        free(idx);
        if (rc < 0) vx_err(vx_video_code(rc), "%s", err[0] ? err : yvid_strerror(rc));
        yvv_info_fields(&i, &f);
        plhs[0] = vx_fields(&f);
    } else if (!strcmp(cmd, "make_index")) {
        yvid_index_desc d;
        char media[1024], index[1024];
        int i, v;
        int64_t k;
        vx_nargs(nrhs, 2, "n = ysp_video('make_index', media, index, name, value, ...)  (index '' = media + '.yspvi')");
        vx_string(prhs[1], media, sizeof media, "make_index: media");
        index[0] = 0;
        if (nrhs > 2) vx_string(prhs[2], index, sizeof index, "make_index: index ('' = beside the media)");
        memset(&d, 0, sizeof d);
        if (nrhs > 3 && (nrhs - 3) % 2) vx_err("usage", "make_index: name-value pairs after the index");
        for (i = 3; i < nrhs; i += 2) {
            char name[32], s[32];
            vx_string(prhs[i], name, sizeof name, "an option name");
            if (!strcmp(name, "matrix")) d.matrix = vx_color_opt(prhs[i + 1], yvv_matrices, "matrix");
            else if (!strcmp(name, "range")) d.range = vx_color_opt(prhs[i + 1], yvv_ranges, "range");
            else if (!strcmp(name, "transfer")) d.transfer = vx_color_opt(prhs[i + 1], yvv_transfers, "transfer");
            else if (!strcmp(name, "primaries")) d.primaries = vx_color_opt(prhs[i + 1], yvv_primaries, "primaries");
            else if (!strcmp(name, "siting")) d.siting = vx_color_opt(prhs[i + 1], yvv_sitings, "siting");
            else if (!strcmp(name, "backend")) {
                vx_string(prhs[i + 1], s, sizeof s, "backend");
                if (yvv_lookup(yvv_backends, "backend", s, &v, err, sizeof err) < 0) vx_err("arg", "%s", err);
                d.backend = (yvid_backend)v;
            } else if (!strcmp(name, "hw")) {
                vx_string(prhs[i + 1], s, sizeof s, "hw");
                if (yvv_lookup(yvv_hws, "hw", s, &v, err, sizeof err) < 0) vx_err("arg", "%s", err);
                d.hw = (yvid_hw)v;
            } else if (!strcmp(name, "no_seek_check")) d.no_seek_check = vx_bool(prhs[i + 1], "no_seek_check");
            else vx_err("arg", "make_index: unknown option '%s' (matrix, range, transfer, primaries, siting, backend, hw, no_seek_check)", name);
        }
        err[0] = 0;
        k = yvid_index_make(media, index[0] ? index : NULL, &d, err, sizeof err);
        if (k < 0) vx_err(vx_video_code((int)k), "%s", err[0] ? err : yvid_strerror((int)k));
        plhs[0] = vx_d((double)k);
    } else if (!strcmp(cmd, "write_seq")) {
        char path[1024];
        double fps[2] = { 0, 1 };
        int qoi = 0;
        int64_t n = 0;
        vx_nargs(nrhs, 4, "n = ysp_video('write_seq', path, frames, fps, 'qoi')  (fps a number or [num den])");
        vx_string(prhs[1], path, sizeof path, "write_seq: path");
        if (!mxIsDouble(prhs[3]) || (mxGetNumberOfElements(prhs[3]) != 1 && mxGetNumberOfElements(prhs[3]) != 2))
            vx_err("arg", "write_seq: fps, a whole number or [num den]");
        fps[0] = mxGetPr(prhs[3])[0];
        if (mxGetNumberOfElements(prhs[3]) == 2) fps[1] = mxGetPr(prhs[3])[1];
        if (fps[0] < 1 || fps[1] < 1 || fps[0] != floor(fps[0]) || fps[1] != floor(fps[1]) || fps[0] > 2147483647.0 || fps[1] > 2147483647.0)
            vx_err("arg", "write_seq: fps, whole numbers > 0");
        if (nrhs > 4) {
            char s[8];
            vx_string(prhs[4], s, sizeof s, "write_seq: compression");
            if (!strcmp(s, "qoi")) qoi = 1;
            else if (strcmp(s, "raw") != 0) vx_err("arg", "write_seq: 'raw' or 'qoi'");
        }
        vx_write_seq(path, prhs[2], (int32_t)fps[0], (int32_t)fps[1], qoi, &n);
        plhs[0] = vx_d((double)n);
    } else if (!strcmp(cmd, "options")) {
        int n = 0, i;
        const char* names[40];
        static const char* extra[][2] = {
            { "data", "the file in memory (uint8), with path ''" },
            { "index", "the .yspvi index: a path or uint8 bytes; none = path + '.yspvi'" },
            { "timeline", "a ysp_timeline handle whose base ('base') is the movie clock" },
            { "records", "FRAME records kept between 'records' calls; 0 = 36000" } };
        mxArray* s;
        for (i = 0; yvv_opts[i].name && n < 36; i++) names[n++] = yvv_opts[i].name;
        for (i = 0; i < 4; i++) names[n++] = extra[i][0];
        s = mxCreateStructMatrix(1, 1, n, names);
        for (i = 0; yvv_opts[i].name; i++) mxSetFieldByNumber(s, 0, i, mxCreateString(yvv_opts[i].doc));
        for (i = 0; i < 4; i++) mxSetFieldByNumber(s, 0, n - 4 + i, mxCreateString(extra[i][1]));
        plhs[0] = s;
    } else if (!strcmp(cmd, "constants")) {
        static const struct { const char* name; double v; } c[] = {
            { "F_PENDING", YVID_F_PENDING }, { "F_LATE", YVID_F_LATE }, { "F_ESTIMATED", YVID_F_ESTIMATED },
            { "F_BELOW_TIER", YVID_F_BELOW_TIER }, { "F_AUDIO_CLOCK", YVID_F_AUDIO_CLOCK },
            { "F_TS_MISMATCH", YVID_F_TS_MISMATCH }, { "F_HASH_MISMATCH", YVID_F_HASH_MISMATCH },
            { "F_NOT_SHOWN", YVID_F_NOT_SHOWN } };
        const char* names[8];
        int i, n = (int)(sizeof c / sizeof c[0]);
        for (i = 0; i < n; i++) names[i] = c[i].name;
        plhs[0] = mxCreateStructMatrix(1, 1, n, names);
        for (i = 0; i < n; i++) mxSetFieldByNumber(plhs[0], 0, i, vx_d(c[i].v));
    } else {
        vx_err("usage", "unknown command '%s'", cmd);
    }
}
