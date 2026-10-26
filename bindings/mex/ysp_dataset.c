/* ysp_dataset.c - MATLAB/Octave MEX binding for ysp/dataset.h (builds to ysp_dataset.<mexext>)
 *
 * A session's data set: the folder of session.json, trials.csv, frames.csv,
 * events.csv, clock.csv and streams/<name>.csv that the ysp player writes,
 * written from a Psychtoolbox script so the same analysis reads it. Command
 * dispatch in the style of the other ysp MEX files: the first argument is a
 * command string, and 'open' returns a uint64 handle.
 *
 *     d.dir = fullfile('data', 'P07_20261021T140312Z');   % must not exist
 *     d.trial_cols = {'participant', 'const', 'P07'; 'trial_index', 'int', [];
 *                     'response', 'string', []; 'correct', 'bool', []; 'rt_s', 'time', []};
 *     d.session = struct('experiment', 'gabor-2afc', 'seed', 20261021);
 *     h = ysp_dataset('open', d);
 *     ysp_dataset('write_frame', h, 'frame', k, 'trial_index', i, 'onset_s', vbl);
 *     ysp_dataset('write_event', h, 't_s', t, 'trial_index', i, 'kind', 'keyboard', 'text', 'left');
 *     ysp_dataset('write_trial', h, struct('trial_index', i, 'response', 'left', 'correct', true, 'rt_s', rt));
 *     rows = ysp_dataset('finish', h);
 *     z = ysp_dataset('zip', h);                         % uint8, or ('zip', h, path)
 *     ysp_dataset('close', h);
 *
 * TIMES are seconds as doubles, rounded to the nearest ns for the header,
 * which writes them back as seconds. A data value may be negative (a
 * residual, an SOA). [] and NaN are a missing value (an empty cell).
 *
 * ROWS. A row is a 1x1 struct whose fields are column names, or name-value
 * pairs. Every column of trials.csv and of a stream is a YTR_COL_SET column
 * (or YTR_COL_TEXT for 'const'), so a column the row does not name is
 * empty. Every value is checked before any is set, so a refused row leaves
 * nothing behind. Text goes into a scratch buffer the handle keeps, read
 * from the char array's own storage (UTF-16 in MATLAB, UTF-8 bytes in
 * Octave), and the command name into a stack buffer, so a row allocates
 * nothing in the binding once the buffer has grown to the longest row.
 *
 * Build with build.m. See README.md for the command reference.
 */
#define PM_MOD "ysp_dataset"
#include "ysp_mex_util.h"

#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"

/* MATLAB R2018a's interleaved-complex API does not change char storage;
 * mxChar is a UTF-16 code unit in MATLAB and a byte in Octave. */

#define DS_S_MAX 9.2e9   /* |s| below this keeps s * 1e9 inside int64 */

typedef struct ds_pend {
    int col, type, missing;
    union { int64_t i; double d; size_t off; } v;
} ds_pend;

typedef struct ds_col { char name[YTB_MAX_NAME + 1]; int type; size_t offset; } ds_col;

typedef struct ds_table {
    int     n;
    int     id;      /* -1: trials.csv; else the stream's id */
    ds_col* col;
} ds_table;

typedef struct ds_handle {
    yds_dataset ds;            /* about 770 KB: calloc, so the pages of
                                * unused streams are never touched         */
    ds_table    trials;
    ds_table    stream[YDS_MAX_STREAMS];
    int         have_rows;
    int64_t     rows[4 + YDS_MAX_STREAMS];   /* the counts at the finish   */
    char*       sbuf;
    size_t      scap, slen;
    ds_pend     pend[YTR_DATA_MAX_COLS];
} ds_handle;

static ds_col ds_frame_f[YDS_FRAME_COLS], ds_event_f[YDS_EVENT_COLS], ds_clock_f[YDS_CLOCK_COLS];
static int ds_fields_done = 0;

/* The fixed tables' fields from the header's own column lists (its
 * implementation is in this file): a field the header adds is bound
 * without an edit here. */
static void ds_fields_init(ds_col* f, const ytr_col* c, int n) {
    int i;
    for (i = 0; i < n; i++) {
        size_t len = strlen(c[i].name);
        memcpy(f[i].name, c[i].name, len + 1);
        if (c[i].type == YTR_COL_TIME_NS && len > 3 && strcmp(c[i].name + len - 3, "_ns") == 0)
            f[i].name[len - 2] = 's', f[i].name[len - 1] = '\0';
        f[i].type = (int)c[i].type;
        f[i].offset = c[i].offset;
    }
}

static void ds_table_free(ds_table* t) {
    free(t->col);
    memset(t, 0, sizeof *t);
}

static void ds_destroy(void* obj) {
    ds_handle* h = (ds_handle*)obj;
    int i;
    yds_close(&h->ds);   /* finishes an open data set: "closed": true */
    ds_table_free(&h->trials);
    for (i = 0; i < YDS_MAX_STREAMS; i++) ds_table_free(&h->stream[i]);
    free(h->sbuf);
    free(h);
}

static void ds_check(int rc, const yds_dataset* ds) {
    const char* m = yds_error(ds);
    if (rc >= 0) return;
    if (!m || !m[0]) m = yds_strerror(rc);
    switch (rc) {
    case YDS_ERR_ARG:    pm_err("arg", "%s", m); break;
    case YDS_ERR_IO:     pm_err("io", "%s", m); break;
    case YDS_ERR_EXISTS: pm_err("exists", "%s", m); break;
    case YDS_ERR_CLOSED: pm_err("closed", "%s", m); break;
    case YDS_ERR_ORDER:  pm_err("order", "%s", m); break;
    case YDS_ERR_FULL:   pm_err("full", "%s", m); break;
    case YDS_ERR_RANGE:  pm_err("range", "%s", m); break;
    case YDS_ERR_NOMEM:  pm_err("memory", "%s", m); break;
    default:             pm_err("error", "%s", m); break;
    }
}

/* A char array's text as UTF-8, mxMalloc'd for this call. Octave keeps
 * char as UTF-8 bytes and has no mxArrayToUTF8String(). */
static char* ds_utf8(const mxArray* a) {
#ifdef HAVE_OCTAVE
    return mxArrayToString(a);
#else
    return mxArrayToUTF8String(a);
#endif
}

static void ds_closed(void) { pm_err("closed", "the data set is not open"); }

/* Seconds to the nearest ns: fma() gives the product's rounding error, so
 * ns -> ns / 1e9 -> ns holds for every |s| below 2^23 s (ysp.rdk's method). */
static int ds_s_to_ns(double s, int64_t* out) {
    volatile double p;
    double e, n, r;
    if (!(fabs(s) < DS_S_MAX)) return -1;
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    *out = (int64_t)n;
    return 0;
}

/* --- text ------------------------------------------------------------------- */

static void ds_grow(ds_handle* h, size_t need) {
    size_t cap;
    char* q;
    if (need <= h->scap) return;
    cap = h->scap ? h->scap : 256;
    while (cap < need) cap *= 2;
    if (!(q = (char*)realloc(h->sbuf, cap))) pm_err("memory", "out of memory");
    h->sbuf = q, h->scap = cap;
}

/* A char row vector as UTF-8 into the scratch buffer, NUL-terminated; a
 * MATLAB string scalar is converted to char first. Returns the offset, or
 * (size_t)-1 when a is not text. */
static size_t ds_text(ds_handle* h, const mxArray* a, const char* col) {
    const mxChar* c;
    size_t n, i, off;
    char* p;
    mxArray* tmp = NULL;
    if (mxIsClass(a, "string")) {
        mxArray* in = (mxArray*)a;
        if (mxGetNumberOfElements(a) != 1) pm_err("arg", "column %s: one string, not %d", col, (int)mxGetNumberOfElements(a));
        mexCallMATLAB(1, &tmp, 1, &in, "char");
        a = tmp;
    }
    if (!mxIsChar(a)) return (size_t)-1;
    n = mxGetNumberOfElements(a);
    if (n && mxGetM(a) != 1) pm_err("arg", "column %s: a row of text (1 x N char)", col);
    c = mxGetChars(a);
    ds_grow(h, h->slen + 3 * n + 1);   /* a UTF-16 unit is at most 3 bytes; a pair 4 for 2 */
    off = h->slen;
    p = h->sbuf + off;
    for (i = 0; i < n; i++) {
        uint32_t u = (uint32_t)c[i];
        if (sizeof(mxChar) == 1) {   /* Octave: the bytes are UTF-8 already */
            u &= 0xFFu;
            if (!u) pm_err("arg", "column %s: the text holds a NUL", col);
            *p++ = (char)u;
            continue;
        }
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < n && (uint32_t)c[i + 1] >= 0xDC00 && (uint32_t)c[i + 1] < 0xE000) {
            u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)c[i + 1] - 0xDC00);
            i++;
        }
        /* A lone surrogate goes out as its 3 bytes, not UTF-8: the writer
         * turns them into U+FFFD. */
        if (u == 0) pm_err("arg", "column %s: the text holds a NUL", col);
        if (u < 0x80) *p++ = (char)u;
        else if (u < 0x800) { *p++ = (char)(0xC0 | (u >> 6)); *p++ = (char)(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) {
            *p++ = (char)(0xE0 | (u >> 12)); *p++ = (char)(0x80 | ((u >> 6) & 0x3F)); *p++ = (char)(0x80 | (u & 0x3F));
        } else {
            *p++ = (char)(0xF0 | (u >> 18)); *p++ = (char)(0x80 | ((u >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((u >> 6) & 0x3F)); *p++ = (char)(0x80 | (u & 0x3F));
        }
    }
    *p++ = '\0';
    h->slen = (size_t)(p - h->sbuf);
    if (tmp) mxDestroyArray(tmp);
    return off;
}

/* --- values ----------------------------------------------------------------- */

static int ds_is_num(const mxArray* a) {
    return (mxIsNumeric(a) || mxIsLogical(a)) && !mxIsComplex(a) && mxGetNumberOfElements(a) == 1;
}

/* One value of column type t into *pv. [] and NaN are missing. */
static void ds_value(ds_handle* h, const mxArray* a, int t, ds_pend* pv, const char* col) {
    double d;
    pv->missing = 0;
    if (mxIsEmpty(a) && !(t == YTR_COL_STRING && mxIsChar(a))) { pv->missing = 1; return; }
    switch (t) {
    case YTR_COL_INT:
    case YTR_COL_INT64:
        if (mxIsInt64(a) && mxGetNumberOfElements(a) == 1) {
            pv->v.i = *(const int64_t*)mxGetData(a);
        } else if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) {
            uint64_t u = *(const uint64_t*)mxGetData(a);
            if (u > (uint64_t)INT64_MAX) pm_err("arg", "column %s: out of range for int64", col);
            pv->v.i = (int64_t)u;
        } else {
            if (!ds_is_num(a)) pm_err("arg", "column %s: an integer scalar (or [] or NaN)", col);
            d = mxGetScalar(a);
            if (isnan(d)) { pv->missing = 1; return; }
            if (d != floor(d) || fabs(d) > 9007199254740992.0) pm_err("arg", "column %s: an integer (or [] or NaN)", col);
            pv->v.i = (int64_t)d;
        }
        if (t == YTR_COL_INT ? (pv->v.i <= INT_MIN || pv->v.i > INT_MAX) : pv->v.i == INT64_MIN)
            pm_err("arg", "column %s: out of range for %s", col, t == YTR_COL_INT ? "int" : "int64");
        return;
    case YTR_COL_DOUBLE:
    case YTR_COL_BOOL:
    case YTR_COL_TIME_NS:
        if (!ds_is_num(a)) pm_err("arg", "column %s: a numeric scalar (or [] or NaN)", col);
        d = mxGetScalar(a);
        if (isnan(d)) { pv->missing = 1; return; }
        if (t == YTR_COL_DOUBLE) { pv->v.d = d; return; }
        if (t == YTR_COL_BOOL) {
            if (d != 0.0 && d != 1.0) pm_err("arg", "column %s: true, false, 0 or 1 (or [] or NaN)", col);
            pv->v.i = d != 0.0;
            return;
        }
        if (ds_s_to_ns(d, &pv->v.i) < 0) pm_err("arg", "column %s: seconds, finite and below 9.2e9 in size", col);
        return;
    case YTR_COL_STRING:
        if ((pv->v.off = ds_text(h, a, col)) == (size_t)-1) pm_err("arg", "column %s: text (a char row or a string)", col);
        if (h->sbuf[pv->v.off] == '\0') pv->missing = 1;   /* '': the same empty field */
        return;
    default:
        pm_err("arg", "column %s is constant; its text is set at the open", col);
    }
}

/* The row's (name, value) pairs: a 1x1 struct, or name-value arguments. */
typedef struct ds_row {
    const mxArray*        s;      /* the struct, or NULL                       */
    const mxArray* const* nv;     /* the pairs, or NULL                        */
    int                   n;
} ds_row;

static void ds_row_parse(ds_row* r, int nrhs, const mxArray* prhs[], int at, const char* usage) {
    memset(r, 0, sizeof *r);
    if (nrhs == at) return;   /* an empty row */
    if (nrhs == at + 1) {
        if (!mxIsStruct(prhs[at]) || mxGetNumberOfElements(prhs[at]) != 1) pm_err("usage", "usage: %s", usage);
        r->s = prhs[at];
        r->n = mxGetNumberOfFields(prhs[at]);
        return;
    }
    if ((nrhs - at) % 2) pm_err("usage", "usage: %s (name-value pairs)", usage);
    r->nv = prhs + at;
    r->n = (nrhs - at) / 2;
}

static const mxArray* ds_row_get(const ds_row* r, int i, char* name, size_t cap) {
    if (r->s) {
        snprintf(name, cap, "%s", mxGetFieldNameByNumber(r->s, i));
        return mxGetFieldByNumber(r->s, 0, i);
    }
    if (!mxIsChar(r->nv[2 * i]) || mxGetString(r->nv[2 * i], name, cap) != 0)
        pm_err("arg", "a column name is a char row of at most %d bytes", (int)cap - 1);
    return r->nv[2 * i + 1];
}

static int ds_find(const ds_col* c, int n, const char* name) {
    int i;
    for (i = 0; i < n; i++) if (strcmp(c[i].name, name) == 0) return i;
    return -1;
}

/* A fixed table's row into its struct, after the struct's _init(). */
static void ds_fill_fixed(ds_handle* h, const ds_row* r, const ds_col* f, int nf, void* rec, const char* table) {
    int i, k_str = 0;
    size_t offs[8], soff[8];
    char name[YTB_MAX_NAME + 2];
    h->slen = 0;
    for (i = 0; i < r->n; i++) {
        ds_pend pv;
        const mxArray* v = ds_row_get(r, i, name, sizeof name);
        int c = ds_find(f, nf, name);
        char* at;
        if (c < 0) pm_err("arg", "%s has no column '%s'", table, name);
        if (!v) continue;
        ds_value(h, v, f[c].type, &pv, f[c].name);
        if (pv.missing) continue;
        at = (char*)rec + f[c].offset;
        switch (f[c].type) {
        case YTR_COL_INT: *(int*)at = (int)pv.v.i; break;
        case YTR_COL_INT64:
        case YTR_COL_TIME_NS: *(int64_t*)at = pv.v.i; break;
        case YTR_COL_DOUBLE: *(double*)at = pv.v.d; break;
        case YTR_COL_STRING:
            if (k_str < 8) offs[k_str] = f[c].offset, soff[k_str] = pv.v.off, k_str++;
            break;
        default: break;
        }
    }
    /* The buffer may move while later values come in: pointers last. */
    for (i = 0; i < k_str; i++) *(const char**)((char*)rec + offs[i]) = h->sbuf + soff[i];
}

static void ds_write_table(ds_handle* h, ds_table* t, const ds_row* r) {
    int i, n = 0, rc;
    ytr_data* w;
    char name[YTB_MAX_NAME + 2];
    h->slen = 0;
    for (i = 0; i < r->n; i++) {
        const mxArray* v = ds_row_get(r, i, name, sizeof name);
        int c = ds_find(t->col, t->n, name);
        if (c < 0) pm_err("arg", "%s has no column '%s'", t->id < 0 ? "trials.csv" : "the stream", name);
        if (!v) continue;
        h->pend[n].col = c;
        h->pend[n].type = t->col[c].type;
        ds_value(h, v, t->col[c].type, &h->pend[n], t->col[c].name);
        n++;
    }
    w = t->id < 0 ? yds_trials_writer(&h->ds) : yds_stream_writer(&h->ds, t->id);
    if (!w) ds_closed();
    for (i = 0; i < n; i++) {
        ds_pend* p = &h->pend[i];
        if (p->missing) continue;   /* not set: an empty cell */
        switch (p->type) {
        case YTR_COL_INT: rc = ytr_data_set_int(w, p->col, (int)p->v.i); break;
        case YTR_COL_INT64: rc = ytr_data_set_int64(w, p->col, p->v.i); break;
        case YTR_COL_DOUBLE: rc = ytr_data_set_double(w, p->col, p->v.d); break;
        case YTR_COL_BOOL: rc = ytr_data_set_bool(w, p->col, p->v.i != 0); break;
        case YTR_COL_STRING: rc = ytr_data_set_string(w, p->col, h->sbuf + p->v.off); break;
        default: rc = ytr_data_set_time_ns(w, p->col, p->v.i); break;
        }
        if (rc < 0) pm_err("arg", "%s", ytr_data_error(w));
    }
    rc = t->id < 0 ? yds_write_trial(&h->ds, NULL) : yds_write_stream(&h->ds, t->id, NULL);
    ds_check(rc, &h->ds);
}

/* --- columns ---------------------------------------------------------------- */

/* An N x 2 or N x 3 cell, {name, type[, decimals | text]} per row, into the
 * header's columns and a table. t->col and a const column's text are
 * mxMalloc'd for this call, so an error leaks nothing; the header copies
 * the columns at the open, and ds_keep() copies t->col for the handle. */
static int ds_parse_cols(const mxArray* spec, ytr_col* cols, ds_table* t, const char* what) {
    static const struct { const char* name; int type; } types[] = {
        { "int", YTR_COL_INT }, { "int64", YTR_COL_INT64 }, { "double", YTR_COL_DOUBLE }, { "float", YTR_COL_DOUBLE },
        { "bool", YTR_COL_BOOL }, { "logical", YTR_COL_BOOL }, { "string", YTR_COL_STRING }, { "char", YTR_COL_STRING },
        { "time", YTR_COL_TIME_NS }, { "const", YTR_COL_CONST },
    };
    size_t n, m, i, k;
    if (!mxIsCell(spec) || (mxGetN(spec) != 2 && mxGetN(spec) != 3) || mxGetM(spec) < 1)
        pm_err("arg", "%s: an N x 2 or N x 3 cell, {name, type[, decimals | text]} per row", what);
    n = mxGetM(spec);
    m = mxGetN(spec);
    if (n > YTR_DATA_MAX_COLS) pm_err("arg", "%s: at most %d columns", what, YTR_DATA_MAX_COLS);
    t->col = (ds_col*)mxCalloc(n, sizeof(ds_col));
    t->n = (int)n;
    for (i = 0; i < n; i++) {
        const mxArray* nm = mxGetCell(spec, i);
        const mxArray* ty = mxGetCell(spec, i + n);
        const mxArray* ex = m == 3 ? mxGetCell(spec, i + 2 * n) : NULL;
        char tname[16];
        int type = -1;
        if (ex && mxIsEmpty(ex)) ex = NULL;
        if (!nm || !mxIsChar(nm) || mxGetString(nm, t->col[i].name, sizeof t->col[i].name) != 0)
            pm_err("arg", "%s: row %d: the name is a char row of at most %d bytes", what, (int)i + 1, YTB_MAX_NAME);
        if (!ty || !mxIsChar(ty) || mxGetString(ty, tname, sizeof tname) != 0) tname[0] = '\0';
        for (k = 0; k < sizeof types / sizeof types[0]; k++)
            if (strcmp(tname, types[k].name) == 0) type = types[k].type;
        if (type < 0)
            pm_err("arg", "%s: column %s: the type is 'int', 'int64', 'double', 'bool', 'string', 'time' or 'const'", what,
                   t->col[i].name);
        t->col[i].type = type;
        memset(&cols[i], 0, sizeof cols[i]);
        cols[i].name = t->col[i].name;
        cols[i].type = (ytr_col_type)type;
        cols[i].offset = YTR_UNBOUND;
        if (type == YTR_COL_TIME_NS) {
            size_t len = strlen(t->col[i].name);
            /* The header also takes "_ns" and writes "_s"; here every value
             * is seconds, so the name says so as the file does. */
            if (len < 2 || strcmp(t->col[i].name + len - 2, "_s") != 0)
                pm_err("arg", "%s: column %s: a time column's name ends in _s (its values are seconds)", what, t->col[i].name);
        }
        if (type == YTR_COL_CONST) {
            if (!ex || !mxIsChar(ex)) pm_err("arg", "%s: column %s: a const column is {name, 'const', text}", what, t->col[i].name);
            cols[i].text = ds_utf8(ex);
        } else if (ex) {
            double d;
            if (type != YTR_COL_DOUBLE && type != YTR_COL_TIME_NS)
                pm_err("arg", "%s: column %s: only double and time take decimals, only const a text", what, t->col[i].name);
            d = pm_scalar(ex, "decimals");
            if (d != floor(d) || d < 0 || d > 17) pm_err("arg", "%s: column %s: decimals 0..17 (time 0..9)", what, t->col[i].name);
            cols[i].decimals = (int)d;
        }
    }
    return (int)n;
}

/* A table parsed for this call into the handle's own storage. */
static int ds_keep(ds_table* dst, const ds_table* src, int id) {
    ds_col* c = (ds_col*)malloc((size_t)src->n * sizeof(ds_col));
    if (!c) return -1;
    memcpy(c, src->col, (size_t)src->n * sizeof(ds_col));
    free(dst->col);
    dst->col = c;
    dst->n = src->n;
    dst->id = id;
    return 0;
}

/* --- JSON ------------------------------------------------------------------- */

static char ds_jerr[256];

static yjs_value* ds_jfail(const char* m) {
    snprintf(ds_jerr, sizeof ds_jerr, "%s", m);
    return NULL;
}

static yjs_value* ds_jnum(yjs_arena* a, const mxArray* v, size_t i) {
    if (mxIsInt64(v)) return yjs_new_int(a, ((const int64_t*)mxGetData(v))[i]);
    if (mxIsUint64(v)) {
        uint64_t u = ((const uint64_t*)mxGetData(v))[i];
        char t[24];
        if (u <= (uint64_t)INT64_MAX) return yjs_new_int(a, (int64_t)u);
        snprintf(t, sizeof t, "%llu", (unsigned long long)u);
        return yjs_new_number(a, t, strlen(t));
    }
    if (mxIsLogical(v)) return yjs_new(a, mxGetLogicals(v)[i] ? YJS_TRUE : YJS_FALSE);
    {
        double d;
        if (mxIsDouble(v)) d = mxGetPr(v)[i];
        else if (mxIsSingle(v)) d = ((const float*)mxGetData(v))[i];
        else if (mxIsInt32(v)) d = ((const int32_t*)mxGetData(v))[i];
        else if (mxIsUint32(v)) d = ((const uint32_t*)mxGetData(v))[i];
        else if (mxIsInt16(v)) d = ((const int16_t*)mxGetData(v))[i];
        else if (mxIsUint16(v)) d = ((const uint16_t*)mxGetData(v))[i];
        else if (mxIsInt8(v)) d = ((const int8_t*)mxGetData(v))[i];
        else d = ((const uint8_t*)mxGetData(v))[i];
        if (!isfinite(d)) return ds_jfail("a session value: JSON has no NaN or Inf");
        if (d == floor(d) && fabs(d) <= 9007199254740992.0) return yjs_new_int(a, (int64_t)d);
        return yjs_new_double(a, d);
    }
}

/* A MATLAB value as JSON: a 1x1 struct is an object, a struct array and a
 * cell an array, a char row a string, a scalar a number or a bool, a
 * vector an array, a matrix an array of its rows, [] null. NULL with
 * ds_jerr set on a refusal; never raises, so the caller frees its arena. */
static yjs_value* ds_json(yjs_arena* a, const mxArray* v, int depth) {
    yjs_value* o;
    size_t i, j, n;
    if (depth > YJS_DEFAULT_DEPTH) return ds_jfail("a session value nests deeper than 64");
    if (mxIsClass(v, "string")) return ds_jfail("a session string is a char row ('text'), not a string (\"text\")");
    if (mxIsChar(v)) {
        char* u;
        n = mxGetNumberOfElements(v);
        if (n && mxGetM(v) != 1) return ds_jfail("a session string is a char row");
        u = n ? ds_utf8(v) : NULL;
        if (n && !u) return ds_jfail("a session string is not text");
        o = yjs_new_string(a, u ? u : "", u ? strlen(u) : 0);
        if (u) mxFree(u);
        return o ? o : ds_jfail("a session string: not UTF-8");
    }
    if (mxIsStruct(v)) {
        int f, nf = mxGetNumberOfFields(v);
        n = mxGetNumberOfElements(v);
        if (n != 1) {
            if (!(o = yjs_new(a, YJS_ARRAY))) return ds_jfail("out of memory");
            for (i = 0; i < n; i++) {
                yjs_value* e = yjs_new(a, YJS_OBJECT);
                if (!e) return ds_jfail("out of memory");
                for (f = 0; f < nf; f++) {
                    const mxArray* x = mxGetFieldByNumber(v, i, f);
                    yjs_value* c = x ? ds_json(a, x, depth + 2) : yjs_new(a, YJS_NULL);
                    if (!c || yjs_set(a, e, mxGetFieldNameByNumber(v, f), c) != YJS_OK) return c ? ds_jfail("out of memory") : NULL;
                }
                if (yjs_push(a, o, e) != YJS_OK) return ds_jfail("out of memory");
            }
            return o;
        }
        if (!(o = yjs_new(a, YJS_OBJECT))) return ds_jfail("out of memory");
        for (f = 0; f < nf; f++) {
            const mxArray* x = mxGetFieldByNumber(v, 0, f);
            yjs_value* c = x ? ds_json(a, x, depth + 1) : yjs_new(a, YJS_NULL);
            if (!c || yjs_set(a, o, mxGetFieldNameByNumber(v, f), c) != YJS_OK) return c ? ds_jfail("out of memory") : NULL;
        }
        return o;
    }
    if (mxIsCell(v)) {
        n = mxGetNumberOfElements(v);
        if (!(o = yjs_new(a, YJS_ARRAY))) return ds_jfail("out of memory");
        for (i = 0; i < n; i++) {
            const mxArray* x = mxGetCell(v, i);
            yjs_value* c = x ? ds_json(a, x, depth + 1) : yjs_new(a, YJS_NULL);
            if (!c) return NULL;
            if (yjs_push(a, o, c) != YJS_OK) return ds_jfail("out of memory");
        }
        return o;
    }
    if ((mxIsNumeric(v) || mxIsLogical(v)) && !mxIsComplex(v)) {
        size_t rows = mxGetM(v), cols = mxGetN(v);
        n = mxGetNumberOfElements(v);
        if (n == 0) return mxIsLogical(v) ? yjs_new(a, YJS_ARRAY) : yjs_new(a, YJS_NULL);
        if (n == 1) return ds_jnum(a, v, 0);
        if (mxGetNumberOfDimensions(v) > 2) return ds_jfail("a session value: at most 2 dimensions");
        if (!(o = yjs_new(a, YJS_ARRAY))) return ds_jfail("out of memory");
        if (rows == 1 || cols == 1) {
            for (i = 0; i < n; i++) {
                yjs_value* c = ds_jnum(a, v, i);
                if (!c) return NULL;
                if (yjs_push(a, o, c) != YJS_OK) return ds_jfail("out of memory");
            }
            return o;
        }
        for (i = 0; i < rows; i++) {
            yjs_value* r = yjs_new(a, YJS_ARRAY);
            if (!r) return ds_jfail("out of memory");
            for (j = 0; j < cols; j++) {
                yjs_value* c = ds_jnum(a, v, i + j * rows);
                if (!c) return NULL;
                if (yjs_push(a, r, c) != YJS_OK) return ds_jfail("out of memory");
            }
            if (yjs_push(a, o, r) != YJS_OK) return ds_jfail("out of memory");
        }
        return o;
    }
    snprintf(ds_jerr, sizeof ds_jerr, "a session value is a struct, cell, char, numeric or logical, not %s", mxGetClassName(v));
    return NULL;
}

/* --- rows ------------------------------------------------------------------- */

static void ds_count(ds_handle* h) {
    int i;
    for (i = 0; i < 4 + h->ds.n_streams; i++) h->rows[i] = yds_rows(&h->ds, i);
    h->have_rows = 1;
}

static mxArray* ds_rows_out(ds_handle* h) {
    static const char* f[] = { "trials", "frames", "events", "clock", "streams" };
    mxArray* s = mxCreateStructMatrix(1, 1, 5, f);
    mxArray* st = mxCreateStructMatrix(1, 1, 0, NULL);
    int i;
    int64_t r[4 + YDS_MAX_STREAMS];
    if (h->have_rows) memcpy(r, h->rows, sizeof r);
    else for (i = 0; i < 4 + h->ds.n_streams; i++) r[i] = yds_rows(&h->ds, i);
    for (i = 0; i < 4; i++) mxSetField(s, 0, f[i], mxCreateDoubleScalar((double)r[i]));
    for (i = 0; i < h->ds.n_streams; i++) {
        mxAddField(st, h->ds.stream_name[i]);
        mxSetField(st, 0, h->ds.stream_name[i], mxCreateDoubleScalar((double)r[4 + i]));
    }
    mxSetField(s, 0, "streams", st);
    return s;
}

typedef struct ds_mem { char* p; size_t n, cap; } ds_mem;

static bool ds_mem_write(void* ctx, const void* bytes, size_t n) {
    ds_mem* m = (ds_mem*)ctx;
    if (m->n + n > m->cap) {
        size_t cap = m->cap ? m->cap : (size_t)1 << 16;
        char* q;
        while (cap < m->n + n) cap *= 2;
        if (!(q = (char*)realloc(m->p, cap))) return false;
        m->p = q, m->cap = cap;
    }
    memcpy(m->p + m->n, bytes, n);
    m->n += n;
    return true;
}

static ds_table* ds_stream_arg(ds_handle* h, const mxArray* a) {
    int i;
    if (mxIsChar(a)) {
        char name[YDS_MAX_NAME + 2];
        if (mxGetString(a, name, sizeof name) == 0)
            for (i = 0; i < h->ds.n_streams; i++)
                if (strcmp(h->ds.stream_name[i], name) == 0) return &h->stream[i];
        pm_err("arg", "no stream named '%s'", name);
    }
    i = pm_index(a, "stream", h->ds.n_streams > 0 ? h->ds.n_streams : 1);
    if (i >= h->ds.n_streams) pm_err("arg", "no stream %d", i + 1);
    return &h->stream[i];
}

static mxArray* ds_names_out(const ds_col* f, int n) {
    mxArray* c = mxCreateCellMatrix(1, (size_t)n);
    int i;
    for (i = 0; i < n; i++) mxSetCell(c, (size_t)i, mxCreateString(f[i].name));
    return c;
}

/* --- dispatch --------------------------------------------------------------- */

static const char* const ds_open_fields[] = { "dir", "trial_cols", "session", "sync" };

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char cmd[32];
    ds_handle* h;
    pm_destroy = ds_destroy;
    if (!ds_fields_done) {
        ds_fields_init(ds_frame_f, yds__frame_cols, YDS_FRAME_COLS);
        ds_fields_init(ds_event_f, yds__event_cols, YDS_EVENT_COLS);
        ds_fields_init(ds_clock_f, yds__clock_cols, YDS_CLOCK_COLS);
        ds_fields_done = 1;
    }
    /* A stack buffer, not mxArrayToString(): no allocation per call. */
    if (nrhs < 1 || !mxIsChar(prhs[0]) || mxGetString(prhs[0], cmd, sizeof cmd) != 0)
        pm_err("usage", "first argument must be a command string");
    (void)nlhs;

    /* ---- commands without a handle ---- */
    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(yds_version()); return; }
    if (strcmp(cmd, "seed_text") == 0) {
        char out[YDS_SEED_TEXT];
        pm_nargs(nrhs, 2, "s = ysp_dataset('seed_text', seed)");
        yds_seed_text(pm_u64(prhs[1], "seed"), out);
        plhs[0] = mxCreateString(out);
        return;
    }
    if (strcmp(cmd, "columns") == 0) {
        char t[16];
        pm_nargs(nrhs, 2, "c = ysp_dataset('columns', 'frames' | 'events' | 'clock')");
        if (!mxIsChar(prhs[1]) || mxGetString(prhs[1], t, sizeof t) != 0) t[0] = '\0';
        if (strcmp(t, "frames") == 0) plhs[0] = ds_names_out(ds_frame_f, YDS_FRAME_COLS);
        else if (strcmp(t, "events") == 0) plhs[0] = ds_names_out(ds_event_f, YDS_EVENT_COLS);
        else if (strcmp(t, "clock") == 0) plhs[0] = ds_names_out(ds_clock_f, YDS_CLOCK_COLS);
        else pm_err("arg", "the table is 'frames', 'events' or 'clock'");
        return;
    }
    if (strcmp(cmd, "open") == 0) {
        const mxArray *d, *f;
        ytr_col cols[YTR_DATA_MAX_COLS];
        ds_table t = { 0, -1, NULL };
        yds_desc desc;
        yjs_arena a;
        int n;
        pm_nargs(nrhs, 2, "h = ysp_dataset('open', struct('dir', dir, 'trial_cols', cols, ...))");
        d = prhs[1];
        pm_check_fields(d, ds_open_fields, 4, "open");
        if (!(f = pm_field(d, "dir")) || !mxIsChar(f)) pm_err("arg", "open: dir (a char row) is required");
        if (!pm_field(d, "trial_cols")) pm_err("arg", "open: trial_cols is required");
        memset(&desc, 0, sizeof desc);
        desc.dir = ds_utf8(f);
        /* Everything that can raise comes before the handle exists. */
        n = ds_parse_cols(pm_field(d, "trial_cols"), cols, &t, "trial_cols");
        desc.trial_cols = cols;
        desc.n_trial_cols = n;
        if ((f = pm_field(d, "sync")) != NULL) desc.flush = pm_bool(f, "sync") ? YTR_FLUSH_SYNC : YTR_FLUSH_ROW;
        yjs_arena_init_heap(&a, 0);
        if ((f = pm_field(d, "session")) != NULL) {
            if (!mxIsStruct(f) || mxGetNumberOfElements(f) != 1) {
                yjs_arena_free(&a);
                pm_err("arg", "open: session is a 1x1 struct of the caller's keys");
            }
            if (!(desc.session = ds_json(&a, f, 0))) {
                yjs_arena_free(&a);
                pm_err("arg", "open: session: %s", ds_jerr);
            }
        }
        h = (ds_handle*)calloc(1, sizeof *h);
        if (!h || ds_keep(&h->trials, &t, -1) < 0) {
            free(h);
            yjs_arena_free(&a);
            pm_err("memory", "out of memory (%d bytes for the data set)", (int)sizeof *h);
        }
        if (!yds_open(&h->ds, &desc)) {
            char msg[sizeof h->ds.error];
            yjs_arena_free(&a);
            snprintf(msg, sizeof msg, "%s", yds_error(&h->ds));
            ds_destroy(h);
            /* yds_open() returns bool; its message says which failure. */
            if (strstr(msg, "' exists;")) pm_err("exists", "%s", msg);
            if (strstr(msg, "cannot make") || strstr(msg, "cannot create") || strstr(msg, "cannot write") ||
                strstr(msg, "cannot rename"))
                pm_err("io", "%s", msg);
            pm_err("arg", "%s", msg);
        }
        yjs_arena_free(&a);
        plhs[0] = pm_handle_out(pm_register(h));
        return;
    }

    /* ---- commands on a handle ---- */
    pm_nargs(nrhs, 2, "ysp_dataset(cmd, h, ...)");
    h = (ds_handle*)pm_lookup(prhs[1]);
    if (strcmp(cmd, "write_frame") == 0) {
        yds_frame fr;
        ds_row r;
        ds_row_parse(&r, nrhs, prhs, 2, "ysp_dataset('write_frame', h, row | name, value, ...)");
        yds_frame_init(&fr);
        ds_fill_fixed(h, &r, ds_frame_f, YDS_FRAME_COLS, &fr, "frames.csv");
        ds_check(yds_write_frame(&h->ds, &fr), &h->ds);
    } else if (strcmp(cmd, "write_event") == 0) {
        yds_event e;
        ds_row r;
        ds_row_parse(&r, nrhs, prhs, 2, "ysp_dataset('write_event', h, row | name, value, ...)");
        yds_event_init(&e);
        ds_fill_fixed(h, &r, ds_event_f, YDS_EVENT_COLS, &e, "events.csv");
        ds_check(yds_write_event(&h->ds, &e), &h->ds);
    } else if (strcmp(cmd, "write_clock") == 0) {
        yds_clock c;
        ds_row r;
        ds_row_parse(&r, nrhs, prhs, 2, "ysp_dataset('write_clock', h, row | name, value, ...)");
        yds_clock_init(&c);
        ds_fill_fixed(h, &r, ds_clock_f, YDS_CLOCK_COLS, &c, "clock.csv");
        ds_check(yds_write_clock(&h->ds, &c), &h->ds);
    } else if (strcmp(cmd, "write_trial") == 0) {
        ds_row r;
        ds_row_parse(&r, nrhs, prhs, 2, "ysp_dataset('write_trial', h, row | name, value, ...)");
        if (!h->ds.open) ds_closed();
        ds_write_table(h, &h->trials, &r);
    } else if (strcmp(cmd, "write_stream") == 0) {
        ds_row r;
        ds_table* t;
        pm_nargs(nrhs, 3, "ysp_dataset('write_stream', h, stream, row | name, value, ...)");
        if (!h->ds.open) ds_closed();
        t = ds_stream_arg(h, prhs[2]);
        ds_row_parse(&r, nrhs, prhs, 3, "ysp_dataset('write_stream', h, stream, row | name, value, ...)");
        ds_write_table(h, t, &r);
    } else if (strcmp(cmd, "stream") == 0) {
        ytr_col cols[YTR_DATA_MAX_COLS];
        ds_table t = { 0, -1, NULL };
        char name[YDS_MAX_NAME + 2];
        int n, id;
        pm_nargs(nrhs, 4, "id = ysp_dataset('stream', h, name, cols)");
        if (!h->ds.open) ds_closed();
        if (!mxIsChar(prhs[2]) || mxGetString(prhs[2], name, sizeof name) != 0)
            pm_err("arg", "stream: the name is a char row of at most %d bytes", YDS_MAX_NAME);
        n = ds_parse_cols(prhs[3], cols, &t, name);
        ds_check(id = yds_stream(&h->ds, name, cols, n), &h->ds);
        if (ds_keep(&h->stream[id], &t, id) < 0) pm_err("memory", "out of memory");
        plhs[0] = mxCreateDoubleScalar(id + 1);
    } else if (strcmp(cmd, "session_set") == 0 || strcmp(cmd, "session_push") == 0) {
        char key[256];
        yjs_arena a;
        yjs_value* v;
        int rc;
        pm_nargs(nrhs, 4, "ysp_dataset('session_set' | 'session_push', h, key, value)");
        if (!mxIsChar(prhs[2]) || mxGetString(prhs[2], key, sizeof key) != 0)
            pm_err("arg", "the key is a char row of at most 255 bytes");
        yjs_arena_init_heap(&a, 0);
        if (!(v = ds_json(&a, prhs[3], 0))) {
            yjs_arena_free(&a);
            pm_err("arg", "%s: %s", key, ds_jerr);
        }
        rc = cmd[8] == 'p' ? yds_session_push(&h->ds, key, v) : yds_session_set(&h->ds, key, v);
        yjs_arena_free(&a);
        ds_check(rc, &h->ds);
    } else if (strcmp(cmd, "rows") == 0) {
        if (!h->ds.open && !h->have_rows) ds_closed();
        plhs[0] = ds_rows_out(h);
    } else if (strcmp(cmd, "finish") == 0) {
        if (!h->ds.open) ds_closed();
        ds_count(h);
        ds_check(yds_finish(&h->ds), &h->ds);
        plhs[0] = ds_rows_out(h);
    } else if (strcmp(cmd, "zip") == 0) {
        ds_mem m = { NULL, 0, 0 };
        int rc;
        if (nrhs > 2) {
            char* path;
            if (!mxIsChar(prhs[2])) pm_err("arg", "zip: the path is a char row");
            path = ds_utf8(prhs[2]);
            ds_check(yds_zip(&h->ds, path, NULL, NULL), &h->ds);
            return;
        }
        rc = yds_zip(&h->ds, NULL, ds_mem_write, &m);
        if (rc < 0) {
            free(m.p);
            ds_check(rc, &h->ds);
        }
        plhs[0] = pm_bytes_out(m.p, m.n);
        free(m.p);
    } else if (strcmp(cmd, "info") == 0) {
        static const char* f[] = { "dir", "is_open", "finished", "streams", "trial_cols" };
        mxArray* s = mxCreateStructMatrix(1, 1, 5, f);
        mxArray* st = mxCreateCellMatrix(1, (size_t)h->ds.n_streams);
        int i;
        mxSetField(s, 0, "dir", mxCreateString(h->ds.dir));
        mxSetField(s, 0, "is_open", mxCreateLogicalScalar(h->ds.open));
        mxSetField(s, 0, "finished", mxCreateLogicalScalar(h->ds.finished));
        for (i = 0; i < h->ds.n_streams; i++) mxSetCell(st, (size_t)i, mxCreateString(h->ds.stream_name[i]));
        mxSetField(s, 0, "streams", st);
        mxSetField(s, 0, "trial_cols", ds_names_out(h->trials.col, h->trials.n));
        plhs[0] = s;
    } else if (strcmp(cmd, "close") == 0) {
        /* yds_finish() first, so a write that failed reaches the caller;
         * the handle goes either way. */
        int rc = 0;
        char msg[sizeof h->ds.error];
        if (h->ds.open) rc = yds_finish(&h->ds);
        snprintf(msg, sizeof msg, "%s", yds_error(&h->ds));
        pm_unregister(h);
        ds_destroy(h);
        if (rc < 0) pm_err(rc == YDS_ERR_IO ? "io" : "error", "%s", msg);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
