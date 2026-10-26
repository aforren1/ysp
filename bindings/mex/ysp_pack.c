/* ysp_pack.c - MATLAB/Octave MEX binding for ysp/pack.h (builds to ysp_pack.<mexext>)
 *
 * Command dispatch in the style of the other ysp MEX files: the first
 * argument is a command string, and 'open' returns an opaque uint64 handle
 * (a counter, never reused) to an open pack.
 *
 *     h = ysp_pack('open', 'study.ysppak');            % mapped read-only
 *     L = ysp_pack('list', h);                          % struct array, one per entry
 *     b = ysp_pack('read', h, 'conditions.pstb');       % uint8 column, verified
 *     d = jsondecode(ysp_pack('definition', h));        % the EXPERIMENT entry
 *     ysp_pack('verify', h);                            % every entry; raises on a mismatch
 *     [ok, msg] = ysp_pack('verify', h);                % or returns the answer
 *     ysp_pack('close', h);
 *
 * COPIES: 'read' copies the entry once, from the mapping into a new uint8
 * column, so the result stays valid after 'close'. 'open' of a uint8 array
 * copies the array once into memory the handle owns: a MEX function cannot
 * keep the caller's array.
 *
 * Errors raise ysp_pack:<what> (arg, io, format, corrupt, version,
 * not_found, nomem, handle, usage) with the header's message.
 *
 * Build with build.m. The command reference is in README.md.
 */
#define PM_MOD "ysp_pack"
#include "ysp_mex_util.h"

#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"

/* This MEX function's own version (docs/bindings.md, Versions). */
#define PK_MEX_VERSION "1.0.0"

typedef struct pk_handle {
    ypak_pack pk;
    void*     own;      /* the pack's bytes for an 'open' of a uint8 array */
    int       policy;   /* the ypak_verify it was opened with */
} pk_handle;

static const char* const pk_policy_names[] = { "use", "open", "none" };

static void pk_destroy(void* obj) {
    pk_handle* h = (pk_handle*)obj;
    ypak_close(&h->pk);
    free(h->own);
    free(h);
}

static const char* pk_code_id(int code) {
    switch (code) {
    case YPAK_ERR_ARG:       return "arg";
    case YPAK_ERR_IO:        return "io";
    case YPAK_ERR_FORMAT:    return "format";
    case YPAK_ERR_CORRUPT:   return "corrupt";
    case YPAK_ERR_VERSION:   return "version";
    case YPAK_ERR_NOT_FOUND: return "not_found";
    case YPAK_ERR_NOMEM:     return "nomem";
    default:                 return "error";
    }
}

/* The header's message when it is this failure's (the claim of THREADS AND
 * COST cannot be lost here, with one thread, but the rule costs nothing). */
static const char* pk_msg(const ypak_pack* pk, int code) {
    return pk->error[0] && pk->error_code == code ? ypak_error(pk) : ypak_strerror(code);
}

static void pk_raise(const ypak_pack* pk, int code) {
    pm_err(pk_code_id(code), "%s", pk_msg(pk, code));
}

/* ---- text: UTF-8 in the pack, char in MATLAB and Octave ---------------------- */

/* A char argument as UTF-8 (mxMalloc'd). Octave's char already holds UTF-8. */
static char* pk_utf8_arg(const mxArray* a, const char* what) {
    char* s;
    if (!mxIsChar(a)) pm_err("arg", "%s must be a string", what);
#ifdef HAVE_OCTAVE
    s = mxArrayToString(a);
#else
    s = mxArrayToUTF8String(a);
#endif
    if (!s) pm_err("arg", "%s cannot be converted to UTF-8", what);
    return s;
}

#ifndef HAVE_OCTAVE
/* One code point from UTF-8 at s[0..n): its length, or 1 with U+FFFD for a
 * byte that starts no valid sequence (a hostile pack's manifest included). */
static size_t pk_utf8_next(const unsigned char* s, size_t n, uint32_t* cp) {
    uint32_t c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF && n >= 2 && (s[1] & 0xC0) == 0x80) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) { *cp = v; return 3; }
    }
    if (c >= 0xF0 && c <= 0xF4 && n >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (v >= 0x10000 && v <= 0x10FFFF) { *cp = v; return 4; }
    }
    *cp = 0xFFFD;
    return 1;
}
#endif

/* A 1 x N char row from UTF-8 bytes. MATLAB's char is UTF-16, so the
 * bytes are decoded; Octave's char is UTF-8, so they are copied. */
static mxArray* pk_utf8_out(const char* s, size_t n) {
    mwSize dims[2];
    mxArray* a;
#ifdef HAVE_OCTAVE
    dims[0] = 1; dims[1] = (mwSize)n;
    a = mxCreateCharArray(2, dims);
    if (n) memcpy(mxGetChars(a), s, n);
#else
    const unsigned char* u = (const unsigned char*)s;
    size_t i = 0, m = 0;
    uint32_t cp;
    mxChar* o;
    while (i < n) { i += pk_utf8_next(u + i, n - i, &cp); m += cp >= 0x10000 ? 2 : 1; }
    dims[0] = 1; dims[1] = (mwSize)m;
    a = mxCreateCharArray(2, dims);
    o = mxGetChars(a);
    for (i = 0; i < n;) {
        i += pk_utf8_next(u + i, n - i, &cp);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            *o++ = (mxChar)(0xD800 + (cp >> 10));
            *o++ = (mxChar)(0xDC00 + (cp & 0x3FF));
        } else {
            *o++ = (mxChar)cp;
        }
    }
#endif
    return a;
}

/* ---- arguments --------------------------------------------------------------- */

static int pk_policy(const mxArray* a) {
    if (!a) return YPAK_VERIFY_USE;
    if (!mxIsChar(a)) pm_err("arg", "verify must be 'use', 'open' or 'none'");
    return pm_enum(a, "verify", pk_policy_names, 3);
}

/* A byte count or offset: a nonnegative integer below 2^53. Inf, where
 * allowed, means "to the end" and comes back as -1. */
static int64_t pk_count(const mxArray* a, const char* what, int inf_ok) {
    double d = pm_scalar(a, what);
    if (inf_ok && isinf(d) && d > 0) return -1;
    if (!(d >= 0) || d != floor(d) || d > 9007199254740992.0)
        pm_err("arg", "%s must be a nonnegative integer number of bytes%s", what, inf_ok ? " (or Inf)" : "");
    return (int64_t)d;
}

static void pk_find(pk_handle* h, const mxArray* a, ypak_entry* e) {
    char* name = pk_utf8_arg(a, "name");
    int rc = ypak_find_n(&h->pk, name, strlen(name), e);
    mxFree(name);
    if (rc < 0) pk_raise(&h->pk, rc);
}

/* ---- results ------------------------------------------------------------------ */

static const char* pk_entry_fields[] = { "name", "index", "kind", "size", "stream", "flags",
                                               "header_off", "data_off", "xxh64", "crc32" };

static void pk_entry_set(mxArray* s, mwIndex i, const ypak_entry* e) {
    const char* kind = ypak_kind_name(e->kind);
    mxArray* crc = mxCreateNumericMatrix(1, 1, mxUINT32_CLASS, mxREAL);
    *(uint32_t*)mxGetData(crc) = e->crc32;
    mxSetField(s, i, "name", pk_utf8_out(e->name, e->name_len));
    mxSetField(s, i, "index", mxCreateDoubleScalar((double)e->index + 1));   /* 1-based */
    mxSetField(s, i, "kind", mxCreateString(kind ? kind : "unknown"));
    mxSetField(s, i, "size", mxCreateDoubleScalar((double)e->size));
    mxSetField(s, i, "stream", mxCreateLogicalScalar((e->flags & YPAK_F_STREAM) != 0));
    mxSetField(s, i, "flags", mxCreateDoubleScalar((double)e->flags));
    mxSetField(s, i, "header_off", mxCreateDoubleScalar((double)e->header_off));
    mxSetField(s, i, "data_off", mxCreateDoubleScalar((double)e->data_off));
    mxSetField(s, i, "xxh64", pm_u64_out(e->hash));
    mxSetField(s, i, "crc32", crc);
}

static mxArray* pk_u8_column(size_t n) {
#ifdef HAVE_OCTAVE
    return mxCreateNumericMatrix(n, 1, mxUINT8_CLASS, mxREAL);
#else
    return mxCreateUninitNumericMatrix(n, 1, mxUINT8_CLASS, mxREAL);   /* every byte is written next */
#endif
}

/* [offset, offset + n) of the entry as a uint8 column: one copy. The whole
 * entry goes through ypak_data() (its hash, by the policy); a range through
 * a cursor, which hashes only the 1 MiB chunks it touches. Raises. */
static mxArray* pk_read(pk_handle* h, const ypak_entry* e, int64_t offset, int64_t n) {
    mxArray* out;
    int64_t len = offset >= e->size ? 0 : e->size - offset;
    if (n >= 0 && n < len) len = n;
    if ((uint64_t)len > (uint64_t)SIZE_MAX) pm_err("arg", "the entry is larger than this process can hold");
    if (offset == 0 && len == e->size) {
        const void* p = ypak_data(&h->pk, e);
        if (!p) pk_raise(&h->pk, h->pk.error_code ? h->pk.error_code : YPAK_ERR_ARG);
        out = pk_u8_column((size_t)len);
        if (len) memcpy(mxGetData(out), p, (size_t)len);
        return out;
    }
    out = pk_u8_column((size_t)len);
    if (len > 0) {
        ypak_cursor c;
        int64_t got;
        int rc = ypak_cursor_init(&c, &h->pk, e, NULL, 0);
        got = rc < 0 ? rc : ypak_cursor_read(&c, offset, mxGetData(out), len);
        if (got != len) {
            mxDestroyArray(out);
            pk_raise(&h->pk, got < 0 ? (int)got : YPAK_ERR_IO);
        }
    }
    return out;
}

static pk_handle* pk_get(int nrhs, const mxArray* prhs[], const char* usage) {
    pm_nargs(nrhs, 2, usage);
    return (pk_handle*)pm_lookup(prhs[1]);
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    pk_handle* h;
    pm_destroy = pk_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- commands without a handle ---- */
    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(PK_MEX_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(YPAK_VERSION_STRING);
        return;
    }
    if (strcmp(cmd, "xxh64") == 0) {
        size_t n;
        const uint8_t* b;
        pm_nargs(nrhs, 2, "v = ysp_pack('xxh64', bytes [, seed])");
        b = pm_bytes_in(prhs[1], "bytes", &n);
        plhs[0] = pm_u64_out(ypak_xxh64(b, n, nrhs > 2 ? pm_u64(prhs[2], "seed") : 0));
        return;
    }
    if (strcmp(cmd, "entry_hash") == 0) {
        size_t n;
        const uint8_t* b;
        pm_nargs(nrhs, 2, "v = ysp_pack('entry_hash', bytes)");
        b = pm_bytes_in(prhs[1], "bytes", &n);
        plhs[0] = pm_u64_out(ypak_entry_hash(b, (int64_t)n, NULL));
        return;
    }
    if (strcmp(cmd, "open") == 0) {
        ypak_desc d;
        char* path = NULL;
        pm_nargs(nrhs, 2, "h = ysp_pack('open', path_or_uint8 [, verify])");
        memset(&d, 0, sizeof d);
        d.verify = (ypak_verify)pk_policy(nrhs > 2 ? prhs[2] : NULL);
        if (mxIsChar(prhs[1])) path = pk_utf8_arg(prhs[1], "path");
        else if (!mxIsUint8(prhs[1])) pm_err("arg", "the pack must be a path (char) or its bytes (uint8)");
        h = (pk_handle*)calloc(1, sizeof *h);
        if (!h) pm_err("nomem", "out of memory");
        h->policy = (int)d.verify;
        if (path) {
            d.path = path;
        } else {
            size_t n = mxGetNumberOfElements(prhs[1]);
            h->own = malloc(n ? n : 1);   /* malloc's alignment covers the header's 8 bytes */
            if (!h->own) { free(h); pm_err("nomem", "out of memory for a %llu-byte pack", (unsigned long long)n); }
            if (n) memcpy(h->own, mxGetData(prhs[1]), n);
            d.data = h->own;
            d.size = n;
        }
        if (!ypak_open(&h->pk, &d)) {
            char msg[300];
            int code = h->pk.error_code ? h->pk.error_code : YPAK_ERR_ARG;
            snprintf(msg, sizeof msg, "%s", pk_msg(&h->pk, code));
            pk_destroy(h);
            pm_err(pk_code_id(code), "%s", msg);
        }
        plhs[0] = pm_handle_out(pm_register(h));
        return;
    }

    /* ---- commands on a handle ---- */
    if (strcmp(cmd, "close") == 0) {
        h = pk_get(nrhs, prhs, "ysp_pack('close', h)");
        pm_unregister(h);
        pk_destroy(h);
        return;
    }
    if (strcmp(cmd, "info") == 0) {
        static const char* f[] = { "id", "format", "entries", "chunks", "file_size", "zip_start", "experiment",
                                   "policy", "hashed_bytes", "describe" };
        char id[72], line[512];
        ypak_entry e;
        mxArray* s;
        h = pk_get(nrhs, prhs, "info = ysp_pack('info', h)");
        ypak_id(&h->pk, id);
        ypak_describe(&h->pk, line, sizeof line);
        s = mxCreateStructMatrix(1, 1, 10, f);
        mxSetField(s, 0, "id", mxCreateString(id));
        mxSetField(s, 0, "format", mxCreateDoubleScalar(h->pk.format));
        mxSetField(s, 0, "entries", mxCreateDoubleScalar(h->pk.n));
        mxSetField(s, 0, "chunks", mxCreateDoubleScalar(h->pk.n_chunks));
        mxSetField(s, 0, "file_size", mxCreateDoubleScalar((double)h->pk.file_size));
        mxSetField(s, 0, "zip_start", mxCreateDoubleScalar((double)h->pk.zip_start));
        if (h->pk.experiment >= 0 && ypak_at(&h->pk, (uint32_t)h->pk.experiment, &e) == 0)
            mxSetField(s, 0, "experiment", pk_utf8_out(e.name, e.name_len));
        else
            mxSetField(s, 0, "experiment", mxCreateString(""));
        mxSetField(s, 0, "policy", mxCreateString(pk_policy_names[h->policy]));
        mxSetField(s, 0, "hashed_bytes", mxCreateDoubleScalar((double)h->pk.hashed_bytes));
        mxSetField(s, 0, "describe", mxCreateString(line));
        plhs[0] = s;
        return;
    }
    if (strcmp(cmd, "list") == 0) {
        mxArray* s;
        uint32_t i;
        h = pk_get(nrhs, prhs, "L = ysp_pack('list', h)");
        s = mxCreateStructMatrix(h->pk.n, 1, 10, pk_entry_fields);
        for (i = 0; i < h->pk.n; i++) {
            ypak_entry e;
            ypak_at(&h->pk, i, &e);
            pk_entry_set(s, i, &e);
        }
        plhs[0] = s;
        return;
    }
    if (strcmp(cmd, "entry") == 0) {
        ypak_entry e;
        h = pk_get(nrhs, prhs, "e = ysp_pack('entry', h, name)");
        pm_nargs(nrhs, 3, "e = ysp_pack('entry', h, name)");
        pk_find(h, prhs[2], &e);
        plhs[0] = mxCreateStructMatrix(1, 1, 10, pk_entry_fields);
        pk_entry_set(plhs[0], 0, &e);
        return;
    }
    if (strcmp(cmd, "has") == 0) {
        ypak_entry e;
        char* name;
        h = pk_get(nrhs, prhs, "tf = ysp_pack('has', h, name)");
        pm_nargs(nrhs, 3, "tf = ysp_pack('has', h, name)");
        name = pk_utf8_arg(prhs[2], "name");
        plhs[0] = mxCreateLogicalScalar(ypak_find_n(&h->pk, name, strlen(name), &e) == 0);
        mxFree(name);
        return;
    }
    if (strcmp(cmd, "read") == 0) {
        ypak_entry e;
        int64_t off = 0, n = -1;
        h = pk_get(nrhs, prhs, "b = ysp_pack('read', h, name [, offset, n])");
        pm_nargs(nrhs, 3, "b = ysp_pack('read', h, name [, offset, n])");
        if (nrhs > 3) off = pk_count(prhs[3], "offset", 0);
        if (nrhs > 4) n = pk_count(prhs[4], "n", 1);
        pk_find(h, prhs[2], &e);
        plhs[0] = pk_read(h, &e, off, n);
        return;
    }
    if (strcmp(cmd, "verify") == 0) {
        ypak_entry e;
        int rc = 0;
        h = pk_get(nrhs, prhs, "[ok, msg] = ysp_pack('verify', h [, name])");
        if (nrhs > 2) {
            pk_find(h, prhs[2], &e);
            /* ypak_data() hashes one entry under 'use' and 'open'; under
             * 'none' no header call hashes one entry, so refuse rather than
             * report a check that did not run. */
            if (h->policy == YPAK_VERIFY_NONE)
                pm_err("arg", "the pack was opened with verify 'none': verify of the whole pack hashes every entry; one entry needs 'use' or 'open'");
            if (!ypak_data(&h->pk, &e)) rc = h->pk.error_code ? h->pk.error_code : YPAK_ERR_ARG;
        } else {
            rc = ypak_verify_all(&h->pk);
        }
        if (nlhs == 0) {
            if (rc < 0) pk_raise(&h->pk, rc);
            return;
        }
        plhs[0] = mxCreateLogicalScalar(rc == 0);
        if (nlhs > 1) plhs[1] = mxCreateString(rc == 0 ? "" : pk_msg(&h->pk, rc));
        return;
    }
    if (strcmp(cmd, "manifest") == 0 || strcmp(cmd, "definition") == 0) {
        ypak_entry e;
        const void* p;
        int def = cmd[0] == 'd';
        h = pk_get(nrhs, prhs, def ? "txt = ysp_pack('definition', h)" : "txt = ysp_pack('manifest', h)");
        if (def && (h->pk.experiment < 0 || ypak_at(&h->pk, (uint32_t)h->pk.experiment, &e) < 0)) {
            plhs[0] = mxCreateString("");
            return;
        }
        if (!def) ypak_at(&h->pk, 0, &e);   /* entry 0, hashed against the pack ID at open */
        /* decoded straight from the mapping: one copy, into the char array */
        p = ypak_data(&h->pk, &e);
        if (!p) pk_raise(&h->pk, h->pk.error_code ? h->pk.error_code : YPAK_ERR_ARG);
        plhs[0] = pk_utf8_out((const char*)p, (size_t)e.size);
        return;
    }
    pm_err("usage", "unknown command '%s'", cmd);
}
