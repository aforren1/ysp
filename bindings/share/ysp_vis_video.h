/* ysp_vis_video.h - movies (ysp/video.h) in ysp-vis: the part both hosts share
 *
 * Not a library header. ysp_vis_core.h includes it at its end, so the Python
 * module (bindings/python/ysp_vis/vis_video.c) and the MEX core
 * (bindings/mex/ysp_vis_video.inc) bind the same thing the same way:
 *
 *   - yv_movie: a yvid_movie on a gfx, its record ring, and the objects of
 *     other modules it holds (a ysp.audio Audio for the soundtrack or the
 *     follow, a ysp.timeline Timeline for the movie base), each pinned
 *     through the owner's table (docs/bindings.md, "Lifetime");
 *   - the open options, by name, for a Python keyword and a MATLAB field;
 *   - the records as named fields (seconds, with the exact ns beside each
 *     absolute time), the info, and the frame sequence layouts.
 *
 * THE FRAME. yvid_update() takes the screen's current frame (yv_screen.f,
 * the last begin()'s), after the screen's begin() and before the gfx's
 * begin(). It never waits. The FRAME record of a display frame is pushed
 * when its flip record arrives: through the next update(), or through the
 * screen's on_flip hook (yv_movies_flip), so wait_flip() completes them too.
 *
 * LIFETIME. A movie lives on one gfx (gfx->movies). Closing the gfx (or its
 * screen) closes its movies first, while the GL objects still exist. A
 * closed movie gives back its pins through yv_video_release(), which each
 * host defines: Python takes the GIL there (a screen closes without it).
 *
 * MEMORY. At open: the movie, its ring and the record buffers (the
 * `records` option, 36000 FRAME records by default: 10 min at 60 Hz). No
 * allocation per frame: update() drains the ring into the buffers.
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_VIS_VIDEO_H
#define YSP_VIS_VIDEO_H

#define YV_MOVIE_RING     1024    /* records between two drains (one per update) */
#define YV_MOVIE_FRAMES  36000    /* FRAME records kept: the `records` option's default */
#define YV_MOVIE_EVENTS   1024    /* the other records kept */

/* Each host: drop one pin of an owner's table (Python takes the GIL). */
static void yv_video_release(void (*release)(void*), void* obj);

typedef struct yv_movie {
    yvid_movie       m;
    struct yv_gfx*   gfx;
    struct yv_movie* next;        /* gfx->movies                                  */
    int              open;
    yrt_ring         ring;
    void*            ring_mem;
    yrt_event*       fr;          /* FRAME records, oldest at fr_head          */
    uint32_t         fr_cap, fr_head, fr_n;
    yrt_event*       ev;          /* the other records                         */
    uint32_t         ev_cap, ev_head, ev_n;
    uint64_t         fr_lost, ev_lost, ring_lost;
    yau_audio*       snd_au;      /* pinned: the soundtrack's device           */
    yau_audio*       follow_au;   /* pinned: follow_audio()'s device           */
    ytl_timeline*    tl;          /* pinned: the movie base's timeline         */
    void*            owned[3];    /* malloc'd bytes the movie reads: data, index, WAV */
} yv_movie;

static void yv_movie_put(yrt_event* buf, uint32_t cap, uint32_t* head, uint32_t* n, uint64_t* lost,
                         const yrt_event* e) {
    buf[(*head + *n) % cap] = *e;
    if (*n < cap) (*n)++;
    else { *head = (*head + 1) % cap; (*lost)++; }
}

/* The ring to the two buffers. Called by every update() and before each read. */
static void yv_movie_drain(yv_movie* mv) {
    yrt_event b[64];
    int n, i;
    if (!mv->ring_mem) return;
    while ((n = yrt_ring_drain(&mv->ring, b, 64)) > 0) {
        for (i = 0; i < n; i++) {
            if (b[i].source == YRT_SRC_RT && b[i].kind == YRT_KIND_LOSS) mv->ring_lost += b[i].u.u64[0];
            else if (b[i].kind == YVID_EV_FRAME) yv_movie_put(mv->fr, mv->fr_cap, &mv->fr_head, &mv->fr_n, &mv->fr_lost, &b[i]);
            else yv_movie_put(mv->ev, mv->ev_cap, &mv->ev_head, &mv->ev_n, &mv->ev_lost, &b[i]);
        }
        if (n < 64) break;
    }
}

static int yv_movie_take(yrt_event* buf, uint32_t cap, uint32_t* head, uint32_t* n, yrt_event* out) {
    if (*n == 0) return 0;
    *out = buf[*head];
    *head = (*head + 1) % cap;
    (*n)--;
    return 1;
}

/* Opens mv (zeroed) on g. 0, or a negative YVID_ERR_* with the reason in
 * err: the header's own (mv->m.error_code, ysp/video.h 0.6.0) when
 * yvid_open() refused. The caller frees what mv holds with yv_movie_free()
 * either way. */
static int yv_movie_open(yv_movie* mv, struct yv_gfx* g, yvid_desc* d, int64_t records, char* err, size_t cap) {
    yrt_ring_desc rd;
    if (!g || !g->open) { snprintf(err, cap, "ysp_video: the gfx is closed"); return YVID_ERR_CLOSED; }
    if (records <= 0) records = YV_MOVIE_FRAMES;
    if (records > 16777216) { snprintf(err, cap, "ysp_video: records: at most 16777216"); return YVID_ERR_ARG; }
    mv->ring_mem = calloc(1, YRT_RING_BYTES(YV_MOVIE_RING));
    mv->fr = (yrt_event*)calloc((size_t)records, sizeof(yrt_event));
    mv->ev = (yrt_event*)calloc(YV_MOVIE_EVENTS, sizeof(yrt_event));
    if (!mv->ring_mem || !mv->fr || !mv->ev) { snprintf(err, cap, "ysp_video: out of memory for the records"); return YVID_ERR_FULL; }
    mv->fr_cap = (uint32_t)records;
    mv->ev_cap = YV_MOVIE_EVENTS;
    rd.memory = mv->ring_mem;
    rd.bytes = YRT_RING_BYTES(YV_MOVIE_RING);
    if (!yrt_ring_open(&mv->ring, &rd)) { snprintf(err, cap, "ysp_video: %s", yrt_ring_error(&mv->ring)); return YVID_ERR_ARG; }
    d->ring = &mv->ring;
    if (!yvid_open(&mv->m, &g->g, d)) {
        snprintf(err, cap, "%s", yvid_error(&mv->m));
        return mv->m.error_code < 0 ? mv->m.error_code : YVID_ERR_ARG;
    }
    mv->open = 1;
    mv->gfx = g;
    mv->next = g->movies;
    g->movies = mv;
    return 0;
}

static void yv_movie_unlink(yv_movie* mv) {
    struct yv_movie** p;
    if (!mv->gfx) return;
    for (p = &mv->gfx->movies; *p; p = &(*p)->next)
        if (*p == mv) { *p = mv->next; break; }
    mv->next = NULL;
    mv->gfx = NULL;
}

/* The C part of a close: the decode thread and the soundtrack's stream
 * stop, the texture goes; the records stay readable. No host call. */
static void yv_movie_close_c(yv_movie* mv) {
    if (mv->open) {
        yvid_close(&mv->m);
        yv_movie_drain(mv);
        yrt_ring_close(&mv->ring);
    }
    mv->open = 0;
    yv_movie_unlink(mv);
}

/* The pins back to their owners (yv_video_release: the host's rule). */
static void yv_movie_unpin(yv_movie* mv) {
    if (mv->snd_au) { yau_audio* p = mv->snd_au; mv->snd_au = NULL; yv_video_release(ysp_audio_api_ptr->head.release, p); }
    if (mv->follow_au) { yau_audio* p = mv->follow_au; mv->follow_au = NULL; yv_video_release(ysp_audio_api_ptr->head.release, p); }
    if (mv->tl) { ytl_timeline* p = mv->tl; mv->tl = NULL; yv_video_release(ysp_timeline_api_ptr->head.release, p); }
}

static void yv_movie_free(yv_movie* mv) {
    int k;
    if (!mv) return;
    yv_movie_close_c(mv);
    yv_movie_unpin(mv);
    free(mv->ring_mem);
    free(mv->fr);
    free(mv->ev);
    for (k = 0; k < 3; k++) free(mv->owned[k]);
    free(mv);
}

/* The core's hooks (ysp_vis_core.h): a gfx that closes, a flip record. */
static void yv_movies_close(struct yv_gfx* g) {
    while (g->movies) {
        yv_movie* mv = g->movies;
        yv_movie_close_c(mv);   /* unlinks it */
        yv_movie_unpin(mv);
    }
}

static void yv_movies_flip(struct yv_gfx* g, const yscr_record* r) {
    yv_movie* mv;
    for (mv = g->movies; mv; mv = mv->next)
        if (mv->open) yvid_flip_done(&mv->m, r);
}

/* update(): the order checks, the follow, the update and the drain.
 * YVID_OK, YVID_ENDED or a negative code (yvid_error(), or `why` for the
 * binding's own order errors). */
static int yv_movie_update(yv_movie* mv, const char** why) {
    int rc;
    yv_screen* v;
    *why = NULL;
    if (!mv->open || !mv->gfx || !mv->gfx->scr) { *why = "the movie is closed"; return YVID_ERR_CLOSED; }
    v = mv->gfx->scr;
    if (!v->in_frame) { *why = "update() comes after the screen's begin()"; return YVID_ERR_ORDER; }
    if (mv->gfx->in_frame) { *why = "update() comes before the gfx's begin() (a texture changes only between frames)"; return YVID_ERR_ORDER; }
    if (mv->follow_au) {
        rc = yvid_follow_audio(&mv->m, mv->follow_au, &v->f);
        if (rc < 0) return rc;
    }
    rc = yvid_update(&mv->m, &v->f);
    yv_movie_drain(mv);
    return rc;
}

/* draw(): the movie's IMAGE stimulus into the gfx's current frame. */
static int yv_movie_draw(yv_movie* mv, const yvid_stim_desc* sd, const char** why) {
    ygfx_stim st;
    *why = NULL;
    if (!mv->open || !mv->gfx) { *why = "the movie is closed"; return YVID_ERR_CLOSED; }
    if (!mv->gfx->in_frame) { *why = "draw() comes between the gfx's begin() and end()"; return YVID_ERR_ORDER; }
    st = yvid_stim(&mv->m, sd);
    return ygfx_draw(&mv->gfx->g, &st);
}

/* ---- names --------------------------------------------------------------------- */

static const yv_name yvv_lights[] = { { "auto", YVID_LIGHT_AUTO }, { "codes", YVID_LIGHT_CODES }, { "eotf", YVID_LIGHT_EOTF }, { NULL, 0 } };
static const yv_name yvv_chromas[] = { { "sited", YVID_CHROMA_SITED }, { "nearest", YVID_CHROMA_NEAREST }, { NULL, 0 } };
static const yv_name yvv_backends[] = {
    { "auto", YVID_BACKEND_AUTO }, { "seq", YVID_BACKEND_SEQ }, { "plmpeg", YVID_BACKEND_PLMPEG }, { "mf", YVID_BACKEND_MF },
    { NULL, 0 } };
static const yv_name yvv_hws[] = { { "auto", YVID_HW_AUTO }, { "off", YVID_HW_OFF }, { "dxva", YVID_HW_DXVA }, { NULL, 0 } };
static const yv_name yvv_paths[] = {
    { "auto", YVID_PATH_AUTO }, { "upload", YVID_PATH_UPLOAD }, { "shared", YVID_PATH_SHARED }, { "gpu", YVID_PATH_GPU }, { NULL, 0 } };
static const yv_name yvv_aligns[] = {
    { "center", YGFX_CENTER }, { "top_left", YGFX_TOP_LEFT }, { "top", YGFX_TOP }, { "top_right", YGFX_TOP_RIGHT },
    { "left", YGFX_LEFT }, { "right", YGFX_RIGHT }, { "bottom_left", YGFX_BOTTOM_LEFT }, { "bottom", YGFX_BOTTOM },
    { "bottom_right", YGFX_BOTTOM_RIGHT }, { NULL, 0 } };
static const yv_name yvv_formats[] = {
    { "r8", YVID_FMT_R8 }, { "rg8", YVID_FMT_RG8 }, { "rgba8", YVID_FMT_RGBA8 }, { "r16f", YVID_FMT_R16F },
    { "rgba16f", YVID_FMT_RGBA16F }, { "r32f", YVID_FMT_R32F }, { "rgba32f", YVID_FMT_RGBA32F }, { "r16", YVID_FMT_R16 },
    { "i420", YVID_FMT_I420 }, { "nv12", YVID_FMT_NV12 }, { "p010", YVID_FMT_P010 }, { NULL, 0 } };
static const yv_name yvv_codecs[] = {
    { "seq", YVID_CODEC_SEQ }, { "mpeg1", YVID_CODEC_MPEG1 }, { "h264", YVID_CODEC_H264 }, { "hevc", YVID_CODEC_HEVC },
    { "custom", YVID_CODEC_CUSTOM }, { NULL, 0 } };
static const yv_name yvv_matrices[] = {
    { "rgb", YVID_MATRIX_RGB }, { "bt601", YVID_MATRIX_BT601 }, { "bt709", YVID_MATRIX_BT709 }, { "bt2020", YVID_MATRIX_BT2020 },
    { NULL, 0 } };
static const yv_name yvv_ranges[] = { { "limited", YVID_RANGE_LIMITED }, { "full", YVID_RANGE_FULL }, { NULL, 0 } };
static const yv_name yvv_transfers[] = {
    { "device", YVID_TRC_DEVICE }, { "bt1886", YVID_TRC_BT1886 }, { "srgb", YVID_TRC_SRGB }, { "linear", YVID_TRC_LINEAR },
    { "gamma22", YVID_TRC_GAMMA22 }, { NULL, 0 } };
static const yv_name yvv_primaries[] = {
    { "device", YVID_PRIM_DEVICE }, { "bt709", YVID_PRIM_BT709 }, { "bt601_525", YVID_PRIM_BT601_525 },
    { "bt601_625", YVID_PRIM_BT601_625 }, { "bt2020", YVID_PRIM_BT2020 }, { NULL, 0 } };
static const yv_name yvv_sitings[] = { { "none", YVID_SITING_NONE }, { "left", YVID_SITING_LEFT }, { "center", YVID_SITING_CENTER }, { NULL, 0 } };
static const char* const yvv_whys[YVID_WHY_COUNT] = { "due", "cadence", "display_late", "decode_late", "drift", "seek", "loop", "manual" };
static const char* const yvv_decisions[4] = { "none", "shown", "repeated", "dropped" };

static const char* yvv_name(const yv_name* t, int v) {
    int i;
    for (i = 0; t[i].name; i++) if (t[i].value == v) return t[i].name;
    return v == 0 ? "unspecified" : "?";
}

/* ---- open options ---------------------------------------------------------------- */
/* yvid_desc fields by name. The host handles path, data, index, timeline
 * (an object) and records itself. */

enum { YVV_BOOL, YVV_I32, YVV_U32, YVV_F64, YVV_ENUM };

typedef struct yvv_opt {
    const char*    name;
    int            type;
    size_t         off;      /* in yvid_desc */
    const yv_name* names;    /* YVV_ENUM */
    const char*    doc;
} yvv_opt;

#define YVV_OFF(f) offsetof(yvid_desc, f)
static const yvv_opt yvv_opts[] = {
    { "loop", YVV_BOOL, YVV_OFF(loop), NULL, "after the last frame comes frame 0" },
    { "ahead", YVV_I32, YVV_OFF(ahead), NULL, "frames decoded ahead; 0 = 6" },
    { "preroll", YVV_I32, YVV_OFF(preroll), NULL, "frames ready before an ASAP start; 0 = ahead" },
    { "lead", YVV_F64, YVV_OFF(lead), NULL, "the due rule's lead, a fraction of the period; 0 = 0.5, -1 = never early" },
    { "light", YVV_ENUM, YVV_OFF(light), yvv_lights, "auto, codes or eotf" },
    { "chroma", YVV_ENUM, YVV_OFF(chroma), yvv_chromas, "sited or nearest" },
    { "backend", YVV_ENUM, YVV_OFF(backend), yvv_backends, "auto (from the file), seq, plmpeg or mf" },
    { "hw_decode", YVV_ENUM, YVV_OFF(hw_decode), yvv_hws, "Media Foundation's decoder: auto, off or dxva" },
    { "strict_cadence", YVV_BOOL, YVV_OFF(strict_cadence), NULL, "refuse a refresh that is not a multiple of the rate" },
    { "inline_decode", YVV_BOOL, YVV_OFF(inline_decode), NULL, "decode inside update() (tests)" },
    { "inline_budget", YVV_I32, YVV_OFF(inline_budget), NULL, "decodes per update inline; 0 = no limit" },
    { "base", YVV_I32, YVV_OFF(base), NULL, "the timeline base of the movie clock, 1..7 (with timeline)" },
    { "refresh_num", YVV_I32, YVV_OFF(refresh_num), NULL, "the refresh as num/den Hz for the cadence; 0 = the screen's" },
    { "refresh_den", YVV_I32, YVV_OFF(refresh_den), NULL, "" },
    { "movie_index", YVV_U32, YVV_OFF(movie_index), NULL, "aux of every record" },
    { "min_tier", YVV_I32, YVV_OFF(min_tier), NULL, "flag FRAME records whose tier is worse; 0 = off" },
    { "pin_cpu", YVV_I32, YVV_OFF(pin_cpu), NULL, "the decode thread's CPU; 0 = no pin" },
    { NULL, 0, 0, NULL, NULL },
};
#undef YVV_OFF

static const yvv_opt* yvv_find_opt(const char* name) {
    int i;
    for (i = 0; yvv_opts[i].name; i++) if (strcmp(yvv_opts[i].name, name) == 0) return &yvv_opts[i];
    return NULL;
}

static int yvv_opt_num(yvid_desc* d, const yvv_opt* o, double v, char* err, size_t cap) {
    char* p = (char*)d + o->off;
    switch (o->type) {
    case YVV_BOOL: *(bool*)p = v != 0.0; return 0;
    case YVV_I32:
    case YVV_U32:
        if (!(v == floor(v)) || v < (o->type == YVV_U32 ? 0.0 : -2147483648.0) ||
            v > (o->type == YVV_U32 ? 4294967295.0 : 2147483647.0)) {
            snprintf(err, cap, "%s: an integer", o->name);
            return -1;
        }
        if (o->type == YVV_I32) *(int32_t*)p = (int32_t)v;
        else *(uint32_t*)p = (uint32_t)v;
        return 0;
    case YVV_F64:
        if (!(v == v)) { snprintf(err, cap, "%s: a number, not NaN", o->name); return -1; }
        *(double*)p = v;
        return 0;
    default:
        snprintf(err, cap, "%s: a name, not a number", o->name);
        return -1;
    }
}

static int yvv_opt_enum(yvid_desc* d, const yvv_opt* o, const char* s, char* err, size_t cap) {
    int v, i;
    size_t len = 0;
    char list[160];
    if (o->type != YVV_ENUM) { snprintf(err, cap, "%s: a number, not a name", o->name); return -1; }
    if (yv_name_value(o->names, s, &v) == 0) { *(int*)((char*)d + o->off) = v; return 0; }
    list[0] = 0;
    for (i = 0; o->names[i].name && len + 1 < sizeof list; i++)
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", i ? ", " : "", o->names[i].name);
    snprintf(err, cap, "%s: '%s' is not one of %s", o->name, s, list);
    return -1;
}

/* A name from a table, or -1 with err. */
static int yvv_lookup(const yv_name* t, const char* what, const char* s, int* out, char* err, size_t cap) {
    int i;
    size_t len = 0;
    char list[200];
    if (yv_name_value(t, s, out) == 0) return 0;
    list[0] = 0;
    for (i = 0; t[i].name && len + 1 < sizeof list; i++)
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", i ? ", " : "", t[i].name);
    snprintf(err, cap, "%s: '%s' is not one of %s", what, s, list);
    return -1;
}

/* A whole file into malloc'd memory: an index named by path when the movie
 * is bytes (desc.index takes bytes only). The path is UTF-8, as
 * ysp/video.h takes paths, so on Windows it goes to _wfopen_s as UTF-16.
 * 0, or a negative YVID_ERR_* with err. */
static int yvv_read_file(const char* path, void** out, size_t* n, char* err, size_t cap) {
    FILE* f = NULL;
    long len;
    void* b;
    int e;
#if defined(_WIN32)
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1024) <= 0) {
        snprintf(err, cap, "ysp_video: the index path is not valid UTF-8 or longer than 1023 characters");
        return YVID_ERR_ARG;
    }
    e = _wfopen_s(&f, w, L"rb");
    if (e != 0) f = NULL;
#else
    f = fopen(path, "rb");
    e = f ? 0 : errno;
#endif
    if (!f) {
        snprintf(err, cap, "ysp_video: cannot open %s", path);
        return e == ENOENT || e == ENOTDIR ? YVID_ERR_NOT_FOUND : YVID_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, cap, "ysp_video: cannot read %s", path);
        return YVID_ERR_IO;
    }
    b = malloc(len > 0 ? (size_t)len : 1);
    if (!b) { fclose(f); snprintf(err, cap, "ysp_video: out of memory for %s", path); return YVID_ERR_FULL; }
    if (len > 0 && fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(b);
        snprintf(err, cap, "ysp_video: cannot read %s", path);
        return YVID_ERR_IO;
    }
    fclose(f);
    *out = b;
    *n = (size_t)len;
    return 0;
}

/* ---- records as fields --------------------------------------------------------------- */
/* A record, an info or a result as a list of named values, which each host
 * turns into a dict or a struct. A time is two fields: name in seconds and
 * name_ns exact. */

enum { YVF_S, YVF_NS, YVF_INT, YVF_NUM, YVF_STR, YVF_BOOL };

typedef struct yv_field {
    const char* name;
    int         type;
    int64_t     i;
    double      d;
    const char* s;
} yv_field;

#define YVF_MAX 48

typedef struct yv_fields { yv_field f[YVF_MAX]; int n; } yv_fields;

static void yvf_int(yv_fields* o, const char* name, int64_t v) {
    if (o->n < YVF_MAX) { yv_field* f = &o->f[o->n++]; f->name = name; f->type = YVF_INT; f->i = v; }
}
static void yvf_num(yv_fields* o, const char* name, double v) {
    if (o->n < YVF_MAX) { yv_field* f = &o->f[o->n++]; f->name = name; f->type = YVF_NUM; f->d = v; }
}
static void yvf_bool(yv_fields* o, const char* name, int v) {
    if (o->n < YVF_MAX) { yv_field* f = &o->f[o->n++]; f->name = name; f->type = YVF_BOOL; f->i = v != 0; }
}
static void yvf_str(yv_fields* o, const char* name, const char* v) {
    if (o->n < YVF_MAX) { yv_field* f = &o->f[o->n++]; f->name = name; f->type = YVF_STR; f->s = v; }
}
/* name (seconds) and name_ns (exact) */
static void yvf_time(yv_fields* o, const char* name, const char* name_ns, int64_t ns) {
    if (o->n + 1 < YVF_MAX) {
        yv_field* f = &o->f[o->n++];
        f->name = name; f->type = YVF_S; f->i = ns;
        f = &o->f[o->n++];
        f->name = name_ns; f->type = YVF_NS; f->i = ns;
    }
}

static const char* yvv_kind_name(unsigned k) {
    static const char* const n[] = { "?", "frame", "drop", "open", "play", "pause", "seek", "loop", "end", "clock",
                                     "decode", "sound", "rate" };
    return k < sizeof n / sizeof n[0] ? n[k] : "?";
}

static const char* yvv_why(unsigned w) { return w < YVID_WHY_COUNT ? yvv_whys[w] : "?"; }

/* A record other than FRAME (or a control's result) as fields. */
static void yvv_event_fields(const yrt_event* e, yv_fields* o) {
    const yrt_payload* u = &e->u;
    o->n = 0;
    yvf_str(o, "kind", yvv_kind_name(e->kind));
    yvf_time(o, "t", "t_ns", e->t_ns);
    switch (e->kind) {
    case YVID_EV_FRAME:
        yvf_int(o, "frame", u->i64[0]);
        yvf_time(o, "due", "due_ns", u->i64[1]);
        yvf_time(o, "movie_t", "movie_t_ns", u->i64[2]);
        yvf_str(o, "decision", yvv_decisions[YVID_EV_DECISION_OF(u->u16[14])]);
        yvf_str(o, "why", yvv_why(YVID_EV_WHY_OF(u->u16[14])));
        yvf_int(o, "flags", u->u32[6]);
        break;
    case YVID_EV_DROP:
        yvf_int(o, "frame", u->i64[0]);
        yvf_int(o, "count", u->i64[1]);
        yvf_str(o, "why", yvv_why(u->u16[14]));
        yvf_int(o, "display", u->u32[9]);
        break;
    case YVID_EV_OPEN:
        yvf_int(o, "w", u->i32[0]);
        yvf_int(o, "h", u->i32[1]);
        yvf_str(o, "format", yvv_name(yvv_formats, u->i32[2]));
        yvf_int(o, "fps_num", u->i32[3]);
        yvf_int(o, "fps_den", u->i32[4]);
        yvf_str(o, "codec", yvv_name(yvv_codecs, u->i32[5]));
        yvf_str(o, "backend", yvv_name(yvv_backends, u->i32[6]));
        yvf_int(o, "multiple", u->i32[7]);
        yvf_str(o, "matrix", yvv_name(yvv_matrices, (int)(u->u32[8] & 0xFF)));
        yvf_str(o, "range", yvv_name(yvv_ranges, (int)(u->u32[8] >> 8 & 0xFF)));
        yvf_str(o, "transfer", yvv_name(yvv_transfers, (int)(u->u32[8] >> 16 & 0xFF)));
        yvf_str(o, "primaries", yvv_name(yvv_primaries, (int)(u->u32[8] >> 24 & 0xFF)));
        yvf_int(o, "ahead", u->i32[9]);
        break;
    case YVID_EV_PLAY:
    case YVID_EV_PAUSE:
        yvf_time(o, "movie_t", "movie_t_ns", u->i64[0]);
        yvf_int(o, "id", u->i64[1]);
        yvf_int(o, "frame", u->i64[2]);
        yvf_time(o, "requested", "requested_ns", u->i64[3]);   /* 0: ASAP */
        break;
    case YVID_EV_SEEK:
        yvf_int(o, "target", u->i64[0]);
        yvf_int(o, "keyframe", u->i64[1]);
        yvf_int(o, "discarded", u->i64[2]);
        yvf_time(o, "requested", "requested_ns", u->i64[3]);   /* the onset of the request */
        yvf_int(o, "id", u->u32[8]);
        yvf_int(o, "skipped", u->u32[9]);
        break;
    case YVID_EV_LOOP: yvf_int(o, "cycle", u->i64[0]); break;
    case YVID_EV_END: yvf_int(o, "frame", u->i64[0]); break;
    case YVID_EV_CLOCK:
        yvf_int(o, "unit0", u->i64[0]);
        yvf_int(o, "rate", u->i64[1]);
        break;
    case YVID_EV_DECODE:
        yvf_time(o, "length", "length_ns", u->i64[0]);
        yvf_int(o, "frame", u->i64[1]);
        break;
    case YVID_EV_SOUND:
        yvf_str(o, "action", u->u32[8] == 1 ? "start" : u->u32[8] == 2 ? "stop" : "?");
        yvf_int(o, "play_id", u->i64[0]);
        yvf_int(o, "sample", u->i64[1]);   /* -1 for a stop */
        yvf_time(o, "target", "target_ns", u->i64[2]);
        yvf_int(o, "id", u->u32[9]);
        break;
    case YVID_EV_RATE:
        yvf_int(o, "num", u->i32[0]);
        yvf_int(o, "den", u->i32[1]);
        yvf_int(o, "multiple", u->i32[2]);
        yvf_num(o, "per_frame", u->f64[2]);
        yvf_bool(o, "refused", u->u32[8] == 1);
        break;
    default:
        break;
    }
}

static void yvv_info_fields(const yvid_info* i, yv_fields* o) {
    o->n = 0;
    yvf_int(o, "w", i->w);
    yvf_int(o, "h", i->h);
    yvf_int(o, "fps_num", i->fps_num);
    yvf_int(o, "fps_den", i->fps_den);
    yvf_num(o, "fps", i->fps_den ? (double)i->fps_num / (double)i->fps_den : 0.0);
    yvf_int(o, "frames", i->frames);
    yvf_time(o, "duration", "duration_ns", i->duration);
    yvf_int(o, "gop", i->gop);
    yvf_str(o, "codec", yvv_name(yvv_codecs, i->codec));
    yvf_str(o, "format", yvv_name(yvv_formats, i->format));
    yvf_str(o, "matrix", yvv_name(yvv_matrices, i->matrix));
    yvf_str(o, "range", yvv_name(yvv_ranges, i->range));
    yvf_str(o, "transfer", yvv_name(yvv_transfers, i->transfer));
    yvf_str(o, "primaries", yvv_name(yvv_primaries, i->primaries));
    yvf_str(o, "siting", yvv_name(yvv_sitings, i->siting));
    yvf_int(o, "refresh_num", i->refresh_num);
    yvf_int(o, "refresh_den", i->refresh_den);
    yvf_num(o, "per_frame", i->per_frame);
    yvf_int(o, "multiple", i->multiple);
    yvf_num(o, "err_ppm", i->err_ppm);
    yvf_num(o, "lead", i->lead);
    yvf_int(o, "ahead", i->ahead);
    yvf_int(o, "slots", i->slots);
    yvf_int(o, "slot_bytes", (int64_t)i->slot_bytes);
    yvf_str(o, "backend", yvv_name(yvv_backends, i->backend));
    yvf_str(o, "path", yvv_name(yvv_paths, i->path));
    yvf_str(o, "light", yvv_name(yvv_lights, i->light));
    yvf_str(o, "hw", yvv_name(yvv_hws, i->hw));
    yvf_int(o, "worst_tier", i->worst_tier);
    yvf_int(o, "shown", (int64_t)i->shown);
    yvf_int(o, "repeated", (int64_t)i->repeated);
    yvf_int(o, "dropped", (int64_t)i->dropped);
    yvf_int(o, "ts_mismatch", (int64_t)i->ts_mismatch);
    yvf_int(o, "hash_mismatch", (int64_t)i->hash_mismatch);
    yvf_int(o, "skipped", (int64_t)i->skipped);
}

/* ---- the frame sequence writer ----------------------------------------------------- */

static int32_t yvv_bpt(int32_t f) {
    switch (f) {
    case YVID_FMT_R8: return 1;
    case YVID_FMT_RG8: return 2;
    case YVID_FMT_RGBA8: return 4;
    case YVID_FMT_R16F: return 4;     /* float in the file */
    case YVID_FMT_RGBA16F: return 16;
    case YVID_FMT_R32F: return 4;
    case YVID_FMT_RGBA32F: return 16;
    case YVID_FMT_R16: return 2;
    default: return 0;
    }
}

/* A frame's planes in one tight buffer: the data pointers and strides into
 * base, and the frame's bytes; 0 for a format the writer does not take. */
static size_t yvv_frame_layout(int32_t f, int32_t w, int32_t h, const uint8_t* base, const void* planes[3],
                               int32_t strides[3]) {
    int32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    planes[0] = planes[1] = planes[2] = NULL;
    strides[0] = strides[1] = strides[2] = 0;
    if (yvv_bpt(f) > 0) {
        planes[0] = base;
        strides[0] = w * yvv_bpt(f);
        return (size_t)strides[0] * (size_t)h;
    }
    if (f == YVID_FMT_I420) {
        planes[0] = base; strides[0] = w;
        planes[1] = base + (size_t)w * h; strides[1] = cw;
        planes[2] = base + (size_t)w * h + (size_t)cw * ch; strides[2] = cw;
        return (size_t)w * h + 2 * (size_t)cw * ch;
    }
    if (f == YVID_FMT_NV12) {
        planes[0] = base; strides[0] = w;
        planes[1] = base + (size_t)w * h; strides[1] = 2 * cw;
        return (size_t)w * h + 2 * (size_t)cw * ch;
    }
    return 0;
}

#endif /* YSP_VIS_VIDEO_H */
