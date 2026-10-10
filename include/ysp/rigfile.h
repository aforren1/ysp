/* ysp/rigfile.h - v0.3.0 - public domain single-header rig profile
 *
 *   The file on a rig that says which device plays each role and what the
 *   rig's loopbacks measured, kept outside every pack (docs/devices_spec.md
 *   11.2, decided 2026-10-09): "ysp-rig 1", JSON in one canonical form,
 *   in the user's config folder. Load it with errors that name the field,
 *   check every loopback file it names by SHA-256, give a role's desc for
 *   ysp/device.h and open the instance, change a binding, store a
 *   loopback result, write it back canonically, and give its hash for the
 *   data file header.
 *
 *   REQUIRES ysp/device.h (and so ysp/rt.h, ysp/input.h, ysp/box.h,
 *   ysp/serial.h) and ysp/json.h beside it; this header includes them, and
 *   its implementation implements ysp/json.h and ysp/device.h unless the
 *   translation unit already does. Windows, Linux and macOS. C99 is the
 *   floor: it builds as C99, C11 and C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_RIGFILE_IMPLEMENTATION
 *       #include "ysp/rigfile.h"
 *
 *       static yrig_profile prof;                      // about 50 KB
 *       static ydev_device resp;
 *       char err[256], hash[65];
 *       int i;
 *       if (yrig_load(&prof, NULL, err, sizeof err) != YRIG_OK) die(err);
 *       for (i = 0; i < prof.n_notes; i++) log_line(prof.notes[i]);   // checks
 *       if (yrig_hash(&prof, hash) != YRIG_OK) die("hash"); // for the data file
 *       ydev_desc base = { 0 };                        // sink, ring, roles
 *       base.sink = to_bridge;
 *       base.roles = &roles;
 *       if (!yrig_open_device(&resp, &prof, "resp", &base)) die(ydev_error(&resp));
 *       if (ydev_wait_running(&resp, YRT_S(3)) < 0) die(ydev_error(&resp));
 *       yin_source src = yrig_source(&prof, &resp);    // tier 1 only when checked
 *       ...
 *       ydev_close(&resp);
 *
 *   ---------------------------------------------------------------------
 *   THE FILE
 *   ---------------------------------------------------------------------
 *   One JSON object (ysp/json.h's strict reader; at most 1 MiB). Unknown
 *   keys are refused at every level, so a misspelled field is an error,
 *   not a default. Durations are seconds, read exactly to the nanosecond
 *   (ysp/json.h's yjs_fixed()); dates are UTC, "2026-10-09T14:03:00Z".
 *     format    "ysp-rig 1"                                    required
 *     rig       the rig's name, 1 to 63 bytes                  required
 *     written   when it was last written                       required
 *     roles     { NAME: role, ... }, at most 32; NAME is 1 to 31 of
 *               A-Z a-z 0-9 _ . - (the ydev_roles name)         required
 *     display   { onset_offset_s, calibration: { file, sha256 } }
 *   A role:
 *     family    a ysp/box.h family name: xid, line, photo, triggerbox,
 *               biosemi, mmbts, lines, parallel; or lsl, a Lab
 *               Streaming Layer stream (ysp/net.h)             required
 *     key       the match key (ysp/device.h, MATCH KEYS; for lsl,
 *               ysp/net.h's "lsl:<name>:<type>:<source_id>:<hostname>",
 *               no quote)                                      required
 *     options   { baud, latched, ftdi_latency_s, buttons }: baud 1 to
 *               10,000,000; latched (MMBT-S at switch S) for mmbts only;
 *               ftdi_latency_s 0.001 to 0.255 as measured; buttons
 *               { CODE: NAME }, at most 16, CODE 1 to 15 bytes, NAME 1
 *               to 31; an lsl role takes buttons only
 *     pulse_s   the rig's fixed pulse width, above 0, at most 10; not
 *               for lsl
 *     latency   an output's write to edge: { n, p5_s, median_s, p95_s,
 *               max_s, date, sha256 }, all required, p5 <= median <= p95
 *               <= max
 *     bounds    an input's stamp minus event: { n, lo_s, hi_s, date,
 *               sha256 }, all required, lo <= hi
 *   sha256 names the loopback's own result file, kept beside the profile
 *   in loopback/<sha256>.txt (yrig_store_loopback() puts it there), 64
 *   lowercase hex digits. display.calibration.file is a plain file name in
 *   the rig folder (a .yspcal), no separator, not starting with '.'.
 *
 *   CANONICAL FORM. yrig_write() gives the bytes: ysp/json.h's
 *   YJS_WRITE_PRETTY (keys in bytewise order, two spaces, LF) and one
 *   final LF; seconds as exact decimals of the nanoseconds with no
 *   trailing zeros (0.000577, 2); a default is left out (options with no
 *   option, latched false, an unset pulse, an empty button map). The
 *   profile's hash, yrig_hash(), is the SHA-256 of these bytes, so it does
 *   not change with the whitespace or number spelling of a hand edit.
 *
 *   ---------------------------------------------------------------------
 *   WHERE IT LIVES
 *   ---------------------------------------------------------------------
 *   yrig_dir(): yrt_user_dir(YRT_DIR_CONFIG, "rig"), so %APPDATA%\ysp\rig
 *   on Windows and $XDG_CONFIG_HOME/ysp/rig or ~/.config/ysp/rig on Linux.
 *   On POSIX that function refuses a folder that all users can write or
 *   another user owns, so a planted profile cannot bind a role to another
 *   device or state a false latency: yrig_load() and yrig_save() then fail
 *   with YRIG_ERR_FOLDER and use no other folder. profile.json is the
 *   default profile; a machine with two rigs keeps <NAME>.json beside it
 *   (NAME is 1 to 63 of A-Z a-z 0-9 _ -), the player's --rig NAME.
 *   yrig_save() makes the folder (0700 on POSIX) and writes through a
 *   temporary file that it renames, so a reader never sees half a file.
 *
 *   ---------------------------------------------------------------------
 *   CHECKS AT LOAD
 *   ---------------------------------------------------------------------
 *   With a folder (yrig_load(), yrig_load_file(), or yrig_parse() with a
 *   dir), each latency and bounds file and the calibration file is read
 *   and hashed: check is YRIG_CHECK_OK, MISSING, CHANGED (another hash), or
 *   DIFFERS (the hash is right, but the profile's summary is not the
 *   file's; the output latency files of examples/device/out_latency.c are
 *   compared field by field). Every check that is not OK adds a line to
 *   notes, and yrig_source() gives that role YIN_TIER_UNKNOWN: the numbers
 *   stay in the struct for the log, never for a tier. Without a folder
 *   every check is YRIG_CHECK_NOT_RUN. A file that fails its check is not
 *   a load error: the rig can still run, with the tier it can prove.
 *   TIER. yrig_source() gives YIN_TIER_1 and the bounds (lo_ns, hi_ns)
 *   only for a role whose bounds' check is OK and whose family maps a
 *   device clock (an input family: xid, line, photo; and lsl, whose
 *   sender's clock ysp/net.h maps), as ysp/input.h defines tier 1; else
 *   the device's own source with YIN_TIER_UNKNOWN. yrig_source_role() is
 *   the same rule by role name, for an instance of another header (an
 *   LSL inlet's ynet_inlet_source()).
 *
 *   ---------------------------------------------------------------------
 *   BINDING
 *   ---------------------------------------------------------------------
 *   yrig_desc() fills role, family, key, baud, latched and (when the rig
 *   fixes it) pulse_ns of a ydev_desc; desc.role and desc.key point into
 *   the profile. yrig_open_device() copies a base desc (sink, ring, roles,
 *   transport, ...), fills it the same way and opens the instance with
 *   ydev_open(), which copies the role and the key: the profile need not
 *   outlive the instance. It returns false with the message in
 *   ydev_error(dev), also when the profile refuses the role before the
 *   device opens; ydev_close(dev) is safe after it either way. Both
 *   refuse an lsl role (YRIG_ERR_FORMAT, the message names ysp/net.h):
 *   give its key to a ysp/net.h inlet or outlet instead (yrig_find(p,
 *   role)->key). yrig_init() is an init (no file, no close); yrig_load*()
 *   and yrig_save*() write their message into the caller's err buffer,
 *   which may be NULL. yrig_bind() sets a role's
 *   family and key
 *   (a binder by activity, the designer's Run view) and clears its
 *   latency and bounds, which belonged to the old device; yrig_save()
 *   writes it.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          yrig_start(dev, p, role, base, err, cap) is
 *          bool yrig_open_device(dev, p, role, base): it opens a device,
 *          so its message is the device's, in ydev_error(dev), also for
 *          a role the profile refuses before the open. On ysp/device.h
 *          v0.3.0, which copies the key and the role: the profile need
 *          not outlive the instance any more. Nothing else changed.
 *   v0.2.0 - Family "lsl": a role bound to a Lab Streaming Layer stream
 *          (ysp/net.h), keyed "lsl:<name>:<type>:<source_id>:<hostname>",
 *          with no serial options and no pulse_s; YRIG_FAMILY_LSL;
 *          yrig_desc() and yrig_start() refuse it, naming ysp/net.h;
 *          yrig_source_role(), the tier rule by role name, for instances
 *          that are not ysp/device.h's. The file format is unchanged.
 *   v0.1.0 - first version: the "ysp-rig 1" format, load, check, write,
 *          hash, bind, store a loopback result.
 *
 *   STATUS: v0.2.0, 2026-10-09. docs/device.md ("The rig profile") has the
 *   how-to, the reference and the test counts.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_RIGFILE_H_INCLUDED
#define YSP_RIGFILE_H_INCLUDED

#define YRIG_VERSION_MAJOR 0
#define YRIG_VERSION_MINOR 3
#define YRIG_VERSION_PATCH 0
#define YRIG_VERSION_STRING "0.3.0"

#include "ysp/device.h"
#include "ysp/json.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YRIG_API
#define YRIG_API extern
#endif

#define YRIG_FORMAT      "ysp-rig 1"
#define YRIG_MAX_ROLES   YDEV_MAX_ROLES
#define YRIG_MAX_BUTTONS 16
#define YRIG_MAX_NOTES   16
#define YRIG_MAX_BYTES   ((size_t)1 << 20)

/* The family id of "lsl": a Lab Streaming Layer stream, started with
 * ysp/net.h. Not a YBOX_ family, so no ysp/box.h number can take it. */
#define YRIG_FAMILY_LSL  64

/* Results */
#define YRIG_OK          0
#define YRIG_ERR_ARG    -1   /* a NULL or a bad name                        */
#define YRIG_ERR_IO     -2   /* a file could not be read or written         */
#define YRIG_ERR_FOLDER -3   /* no user folder, or one that others can write */
#define YRIG_ERR_JSON   -4   /* not strict JSON                             */
#define YRIG_ERR_FORMAT -5   /* JSON, but a field is wrong (the message names it) */
#define YRIG_ERR_MISSING -6  /* no profile file there                       */
#define YRIG_ERR_FULL   -7   /* YRIG_MAX_ROLES roles already                */

/* The check of a file the profile names (CHECKS AT LOAD) */
#define YRIG_CHECK_NONE    0   /* the profile names none                    */
#define YRIG_CHECK_OK      1
#define YRIG_CHECK_MISSING 2
#define YRIG_CHECK_CHANGED 3   /* another SHA-256                           */
#define YRIG_CHECK_DIFFERS 4   /* the summary is not the file's             */
#define YRIG_CHECK_NOT_RUN 5   /* parsed without a folder                   */

/* A loopback's summary: latency (an output) uses n, p5, median, p95, max;
 * bounds (an input) uses n, lo, hi. */
typedef struct yrig_loopback {
    bool     set;
    int32_t  n;
    int64_t  p5_ns, median_ns, p95_ns, max_ns;
    int64_t  lo_ns, hi_ns;
    char     date[24];
    char     sha256[65];
    int      check;              /* YRIG_CHECK_*                              */
} yrig_loopback;

typedef struct yrig_button {
    char code[16];
    char name[32];
} yrig_button;

typedef struct yrig_role {
    char          name[32];
    char          family[16];    /* ybox_family_name(), or "lsl"             */
    int           family_id;     /* YBOX_*, or YRIG_FAMILY_LSL               */
    char          key[160];
    uint32_t      baud;          /* 0 = the family's                         */
    bool          latched;
    int64_t       ftdi_latency_ns;   /* -1 = not stated                       */
    int64_t       pulse_ns;      /* -1 = not fixed by the rig                */
    int           n_buttons;
    yrig_button   buttons[YRIG_MAX_BUTTONS];
    yrig_loopback latency;       /* an output: write to edge                 */
    yrig_loopback bounds;        /* an input: stamp minus event              */
} yrig_role;

typedef struct yrig_display {
    bool     set;
    int64_t  onset_offset_ns;
    char     calibration[64];    /* a file in the rig folder; "" = none      */
    char     calibration_sha256[65];
    int      check;              /* the calibration file's                   */
} yrig_display;

typedef struct yrig_profile {
    char         rig[64];
    char         written[24];
    int          n_roles;
    yrig_role    role[YRIG_MAX_ROLES];
    yrig_display display;
    char         dir[512];       /* the folder of loopback/; "" = none       */
    int          n_notes;
    char         notes[YRIG_MAX_NOTES][160];   /* what the checks found      */
} yrig_profile;

YRIG_API const char* yrig_version(void);
YRIG_API const char* yrig_check_name(int check);

/* --- files ------------------------------------------------------------------- */
/* The user's rig folder (WHERE IT LIVES); YRIG_ERR_FOLDER when refused. */
YRIG_API int  yrig_dir(char* out, size_t cap);
/* dir/profile.json (name NULL, "" or "profile") or dir/<name>.json. */
YRIG_API int  yrig_path(const char* dir, const char* name, char* out, size_t cap);
YRIG_API int  yrig_load(yrig_profile* p, const char* name, char* err, size_t cap);
/* A profile file anywhere; its loopback/ is beside it. */
YRIG_API int  yrig_load_file(yrig_profile* p, const char* path, char* err, size_t cap);
/* From text; dir is the folder for the checks, NULL for none. */
YRIG_API int  yrig_parse(yrig_profile* p, const char* text, size_t n, const char* dir, char* err, size_t cap);
/* Sets written to now, checks the profile, writes it (WHERE IT LIVES). */
YRIG_API int  yrig_save(yrig_profile* p, const char* name, char* err, size_t cap);
YRIG_API int  yrig_save_file(yrig_profile* p, const char* path, char* err, size_t cap);

/* --- the profile in memory ----------------------------------------------------- */
YRIG_API void yrig_init(yrig_profile* p, const char* rig);
/* The canonical bytes: their length, and the bytes with a NUL when they
 * fit in cap. 0 when the profile has a value the format cannot hold. */
YRIG_API size_t yrig_write(const yrig_profile* p, char* buf, size_t cap);
/* The SHA-256 of yrig_write()'s bytes, in hex. */
YRIG_API int  yrig_hash(const yrig_profile* p, char hex[65]);
YRIG_API yrig_role* yrig_find(const yrig_profile* p, const char* role);
/* The role, added with family and key empty when it is new. */
YRIG_API yrig_role* yrig_add(yrig_profile* p, const char* role);
YRIG_API bool yrig_remove(yrig_profile* p, const char* role);
/* Binds role to a device: family, key; clears its latency and bounds. */
YRIG_API int  yrig_bind(yrig_profile* p, const char* role, int family, const char* key);

/* --- loopback files ------------------------------------------------------------ */
/* Writes data to dir/loopback/<sha256>.txt; sha_hex gets the name. */
YRIG_API int  yrig_store_loopback(const char* dir, const void* data, size_t n, char sha_hex[65], char* err, size_t cap);
/* The summary of an output latency file ("format ysp-out-latency 1"). */
YRIG_API int  yrig_read_latency(const char* text, size_t n, yrig_loopback* out, char* err, size_t cap);
YRIG_API void yrig_sha256(const void* data, size_t n, uint8_t out[32]);

/* --- binding (ysp/device.h) ---------------------------------------------------- */
/* Fills role, family, key, baud, latched and pulse_ns of d (BINDING); the
 * strings point into p. */
YRIG_API int  yrig_desc(const yrig_profile* p, const char* role, ydev_desc* d, char* err, size_t cap);
/* base (NULL = zeroed) filled by yrig_desc(), then ydev_open(). false with
 * the message in ydev_error(dev): the profile's ("roles.trig: no device is
 * bound") or the device layer's, after "roles.<role>: ". */
YRIG_API bool yrig_open_device(ydev_device* dev, const yrig_profile* p, const char* role, const ydev_desc* base);
YRIG_API yin_source yrig_source(const yrig_profile* p, const ydev_device* dev);
/* The same rule by role name: src with tier 1 and the role's bounds when
 * they checked and the family maps a device clock, else src with tier
 * UNKNOWN and no bounds. */
YRIG_API yin_source yrig_source_role(const yrig_profile* p, const char* role, yin_source src);

#ifdef __cplusplus
}
#endif

#endif /* YSP_RIGFILE_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_RIGFILE_IMPLEMENTATION
#ifndef YSP_RIGFILE_IMPLEMENTATION_GUARD
#define YSP_RIGFILE_IMPLEMENTATION_GUARD

#ifndef YSP_JSON_IMPLEMENTATION_GUARD
    #define YSP_JSON_IMPLEMENTATION
    #include "ysp/json.h"
#endif
#ifndef YSP_DEVICE_IMPLEMENTATION_GUARD
    #define YSP_DEVICE_IMPLEMENTATION
    #include "ysp/device.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

YRIG_API const char* yrig_version(void) { return YRIG_VERSION_STRING; }

YRIG_API const char* yrig_check_name(int check) {
    switch (check) {
    case YRIG_CHECK_NONE:    return "none";
    case YRIG_CHECK_OK:      return "ok";
    case YRIG_CHECK_MISSING: return "missing";
    case YRIG_CHECK_CHANGED: return "changed";
    case YRIG_CHECK_DIFFERS: return "differs";
    case YRIG_CHECK_NOT_RUN: return "not run";
    default:                 return "?";
    }
}

static int yrig__err(char* err, size_t cap, int code, const char* fmt, ...) {
    va_list ap;
    if (!err || !cap) return code;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return code;
}

static void yrig__copy(char* dst, size_t cap, const char* s, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* ======================================================================= *
 *  SHA-256 (FIPS 180-4)
 * ======================================================================= */

static const uint32_t yrig__k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static uint32_t yrig__ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void yrig__sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = yrig__ror(w[i - 15], 7) ^ yrig__ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = yrig__ror(w[i - 2], 17) ^ yrig__ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        t1 = k + (yrig__ror(e, 6) ^ yrig__ror(e, 11) ^ yrig__ror(e, 25)) + ((e & f) ^ (~e & g)) + yrig__k[i] + w[i];
        t2 = (yrig__ror(a, 2) ^ yrig__ror(a, 13) ^ yrig__ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

YRIG_API void yrig_sha256(const void* data, size_t n, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t* msg = (const uint8_t*)data;
    uint8_t tail[128];
    size_t i, full = n / 64 * 64, rest = n - full, tn;
    uint64_t bits = (uint64_t)n * 8u;
    for (i = 0; i < full; i += 64) yrig__sha_block(h, msg + i);
    memset(tail, 0, sizeof tail);
    if (rest) memcpy(tail, msg + full, rest);
    tail[rest] = 0x80;
    tn = rest + 9 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[tn - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (i = 0; i < tn; i += 64) yrig__sha_block(h, tail + i);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

static void yrig__sha_hex(const void* data, size_t n, char hex[65]) {
    static const char hx[] = "0123456789abcdef";
    uint8_t d[32];
    int i;
    yrig_sha256(data, n, d);
    for (i = 0; i < 32; i++) { hex[2 * i] = hx[d[i] >> 4]; hex[2 * i + 1] = hx[d[i] & 15u]; }
    hex[64] = '\0';
}

/* ======================================================================= *
 *  FILES
 * ======================================================================= */

#if defined(_WIN32)
static bool yrig__wide(const char* s, wchar_t* w, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) > 0;
}
/* 0, or the errno of the failure. */
static int yrig__wopen(const wchar_t* w, const wchar_t* mode, FILE** f) {
#if defined(_MSC_VER)
    errno_t e = _wfopen_s(f, w, mode);
    if (e) *f = NULL;
    return (int)e;
#else
    *f = _wfopen(w, mode);
    return *f ? 0 : errno;
#endif
}
#endif

/* The whole file into a malloc'd buffer (with a NUL after it).
 * YRIG_ERR_MISSING when it is not there. */
static int yrig__read(const char* path, size_t max, char** out, size_t* n) {
    FILE* f;
    char* b;
    long sz;
    int e;
    *out = NULL;
    *n = 0;
#if defined(_WIN32)
    {
        wchar_t w[1024];
        if (!yrig__wide(path, w, 1024)) return YRIG_ERR_ARG;
        e = yrig__wopen(w, L"rb", &f);
    }
#else
    f = fopen(path, "rb");
    e = f ? 0 : errno;
#endif
    if (!f) return e == ENOENT ? YRIG_ERR_MISSING : YRIG_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return YRIG_ERR_IO; }
    if ((size_t)sz > max) { fclose(f); return YRIG_ERR_FORMAT; }
    b = (char*)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return YRIG_ERR_IO; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return YRIG_ERR_IO; }
    fclose(f);
    b[sz] = '\0';
    *out = b;
    *n = (size_t)sz;
    return YRIG_OK;
}

/* Makes path and each missing folder above it (0700 on POSIX). */
static bool yrig__mkdirs(const char* path) {
    char t[1024];
    size_t n = strlen(path), i, start = 1;
    if (n == 0 || n >= sizeof t) return false;
    memcpy(t, path, n + 1);
#if defined(_WIN32)
    if (n >= 3 && t[1] == ':') start = 3;
    else if (t[0] == '\\' && t[1] == '\\') {      /* \\server\share\ */
        int seps = 0;
        for (start = 2; start < n && seps < 2; start++) if (t[start] == '\\' || t[start] == '/') seps++;
    }
#endif
    for (i = start; i <= n; i++) {
        if (i == n || t[i] == '/' || t[i] == '\\') {
            char c = t[i];
            t[i] = '\0';
#if defined(_WIN32)
            {
                wchar_t w[1024];
                if (!yrig__wide(t, w, 1024)) return false;
                if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
            }
#else
            if (mkdir(t, 0700) != 0 && errno != EEXIST) return false;
#endif
            t[i] = c;
        }
    }
    return true;
}

/* path through path.tmp and a rename, so a reader sees the old file or the
 * new one, never a part. */
static int yrig__write_file(const char* path, const void* data, size_t n) {
    char tmp[1040];
    FILE* f;
    bool ok;
    if (strlen(path) + 5 >= sizeof tmp) return YRIG_ERR_ARG;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
#if defined(_WIN32)
    {
        wchar_t w[1040], wt[1040];
        if (!yrig__wide(path, w, 1040) || !yrig__wide(tmp, wt, 1040)) return YRIG_ERR_ARG;
        if (yrig__wopen(wt, L"wb", &f) != 0 || !f) return YRIG_ERR_IO;
        ok = fwrite(data, 1, n, f) == n;
        ok = fclose(f) == 0 && ok;
        if (ok) ok = MoveFileExW(wt, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileW(wt);
    }
#else
    f = fopen(tmp, "wb");
    if (!f) return YRIG_ERR_IO;
    ok = fwrite(data, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
#endif
    return ok ? YRIG_OK : YRIG_ERR_IO;
}

#if defined(_WIN32)
#define YRIG__SEP "\\"
#else
#define YRIG__SEP "/"
#endif

YRIG_API int yrig_dir(char* out, size_t cap) {
    if (!out || !cap) return YRIG_ERR_ARG;
    return yrt_user_dir(YRT_DIR_CONFIG, "rig", out, cap) == YRT_OK ? YRIG_OK : YRIG_ERR_FOLDER;
}

static bool yrig__name_ok(const char* s, size_t max, bool dot) {
    size_t i, n = s ? strlen(s) : 0;
    if (n == 0 || n > max) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (dot && c == '.')))
            return false;
    }
    return true;
}

YRIG_API int yrig_path(const char* dir, const char* name, char* out, size_t cap) {
    int w;
    if (!dir || !dir[0] || !out || !cap) return YRIG_ERR_ARG;
    out[0] = '\0';
    if (!name || !name[0]) name = "profile";
    if (!yrig__name_ok(name, 63, false)) return YRIG_ERR_ARG;
    w = snprintf(out, cap, "%s" YRIG__SEP "%s.json", dir, name);
    if (w < 0 || (size_t)w >= cap) { out[0] = '\0'; return YRIG_ERR_ARG; }
    return YRIG_OK;
}

/* ======================================================================= *
 *  READING
 * ======================================================================= */

typedef struct yrig__rd {
    const char* what;     /* the file, or "the rig profile"               */
    const char* text;
    size_t      n;
    char*       err;
    size_t      cap;
} yrig__rd;

/* "what: field: msg (line L, column C)". */
static int yrig__bad(yrig__rd* r, const yjs_value* at, const char* field, const char* fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (at) {
        uint32_t line, col;
        yjs_line_col(r->text, r->n, at->pos, &line, &col);
        return yrig__err(r->err, r->cap, YRIG_ERR_FORMAT, "%s: %s: %s (line %u, column %u)", r->what, field, msg, (unsigned)line,
                         (unsigned)col);
    }
    return yrig__err(r->err, r->cap, YRIG_ERR_FORMAT, "%s: %s: %s", r->what, field, msg);
}

static int yrig__keys(yrig__rd* r, const yjs_value* o, const char* field, const char* const* allowed) {
    uint32_t i;
    if (!o || o->type != YJS_OBJECT) return yrig__bad(r, o, field, "must be an object");
    for (i = 0; i < o->n; i++) {
        const yjs_member* m = &o->u.members[i];
        int k;
        bool ok = false;
        for (k = 0; allowed[k]; k++)
            if (strlen(allowed[k]) == m->key_len && memcmp(allowed[k], m->key, m->key_len) == 0) ok = true;
        if (!ok) {
            uint32_t line, col;
            yjs_line_col(r->text, r->n, m->key_pos, &line, &col);
            return yrig__err(r->err, r->cap, YRIG_ERR_FORMAT, "%s: %s: unknown key \"%.40s\" (line %u, column %u)", r->what, field, m->key,
                             (unsigned)line, (unsigned)col);
        }
    }
    return YRIG_OK;
}

/* A string of 1 to max bytes with no control character, into dst. */
static int yrig__str(yrig__rd* r, const yjs_value* o, const char* key, const char* field, char* dst, size_t max, bool required) {
    const yjs_value* v = yjs_get(o, key);
    size_t i;
    if (!v) return required ? yrig__bad(r, o, field, "is missing") : YRIG_OK;
    if (v->type != YJS_STRING) return yrig__bad(r, v, field, "must be a string");
    if (v->n == 0 || v->n > max) return yrig__bad(r, v, field, "must be 1 to %u bytes", (unsigned)max);
    for (i = 0; i < v->n; i++)
        if ((unsigned char)v->u.s[i] < 0x20 || v->u.s[i] == 0x7F) return yrig__bad(r, v, field, "has a control character");
    yrig__copy(dst, max + 1, v->u.s, v->n);
    return YRIG_OK;
}

/* Seconds into nanoseconds, exactly, within [lo, hi]. */
static int yrig__secs(yrig__rd* r, const yjs_value* o, const char* key, const char* field, int64_t* out, int64_t lo, int64_t hi,
                      bool required) {
    const yjs_value* v = yjs_get(o, key);
    int rc;
    if (!v) return required ? yrig__bad(r, o, field, "is missing") : YRIG_OK;
    if (v->type != YJS_NUMBER) return yrig__bad(r, v, field, "must be a number (seconds)");
    rc = yjs_fixed(v, 9, out);
    if (rc != YJS_OK || *out < lo || *out > hi) return yrig__bad(r, v, field, "%s is out of range", v->u.s);
    return YRIG_OK;
}

static int yrig__int(yrig__rd* r, const yjs_value* o, const char* key, const char* field, int64_t* out, int64_t lo, int64_t hi,
                     bool required) {
    const yjs_value* v = yjs_get(o, key);
    if (!v) return required ? yrig__bad(r, o, field, "is missing") : YRIG_OK;
    if (v->type != YJS_NUMBER || yjs_int64(v, out) != YJS_OK) return yrig__bad(r, v, field, "must be an integer");
    if (*out < lo || *out > hi) return yrig__bad(r, v, field, "%s is out of range", v->u.s);
    return YRIG_OK;
}

static bool yrig__date_ok(const char* s) {
    static const char shape[] = "dddd-dd-ddTdd:dd:ddZ";
    int i, mo, d, h, mi, se;
    if (strlen(s) != 20) return false;
    for (i = 0; i < 20; i++) {
        if (shape[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != shape[i]) return false;
    }
    mo = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    h = (s[11] - '0') * 10 + (s[12] - '0');
    mi = (s[14] - '0') * 10 + (s[15] - '0');
    se = (s[17] - '0') * 10 + (s[18] - '0');
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 23 && mi <= 59 && se <= 60;
}

static int yrig__date(yrig__rd* r, const yjs_value* o, const char* key, const char* field, char* dst) {
    int rc = yrig__str(r, o, key, field, dst, 23, true);
    if (rc) return rc;
    if (!yrig__date_ok(dst)) return yrig__bad(r, yjs_get(o, key), field, "must be a UTC date like 2026-10-09T14:03:00Z");
    return YRIG_OK;
}

static int yrig__sha(yrig__rd* r, const yjs_value* o, const char* field, char* dst) {
    const yjs_value* v = yjs_get(o, "sha256");
    int i;
    char f[96];
    snprintf(f, sizeof f, "%s.sha256", field);
    if (!v) return yrig__bad(r, o, f, "is missing");
    if (v->type != YJS_STRING || v->n != 64) return yrig__bad(r, v, f, "must be 64 lowercase hex digits");
    for (i = 0; i < 64; i++) {
        char c = v->u.s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return yrig__bad(r, v, f, "must be 64 lowercase hex digits");
    }
    memcpy(dst, v->u.s, 65);
    return YRIG_OK;
}

#define YRIG__S_MAX ((int64_t)1000000000 * 3600)   /* an hour: above any loopback */

static int yrig__loopback(yrig__rd* r, const yjs_value* o, const char* field, yrig_loopback* lb, bool latency) {
    static const char* const lkeys[] = { "n", "p5_s", "median_s", "p95_s", "max_s", "date", "sha256", NULL };
    static const char* const bkeys[] = { "n", "lo_s", "hi_s", "date", "sha256", NULL };
    char f[96];
    int64_t n = 0;
    int rc;
    if ((rc = yrig__keys(r, o, field, latency ? lkeys : bkeys)) != YRIG_OK) return rc;
    snprintf(f, sizeof f, "%s.n", field);
    if ((rc = yrig__int(r, o, "n", f, &n, 1, 100000000, true)) != YRIG_OK) return rc;
    lb->n = (int32_t)n;
    if (latency) {
        snprintf(f, sizeof f, "%s.p5_s", field);
        if ((rc = yrig__secs(r, o, "p5_s", f, &lb->p5_ns, -YRIG__S_MAX, YRIG__S_MAX, true)) != YRIG_OK) return rc;
        snprintf(f, sizeof f, "%s.median_s", field);
        if ((rc = yrig__secs(r, o, "median_s", f, &lb->median_ns, -YRIG__S_MAX, YRIG__S_MAX, true)) != YRIG_OK) return rc;
        snprintf(f, sizeof f, "%s.p95_s", field);
        if ((rc = yrig__secs(r, o, "p95_s", f, &lb->p95_ns, -YRIG__S_MAX, YRIG__S_MAX, true)) != YRIG_OK) return rc;
        snprintf(f, sizeof f, "%s.max_s", field);
        if ((rc = yrig__secs(r, o, "max_s", f, &lb->max_ns, -YRIG__S_MAX, YRIG__S_MAX, true)) != YRIG_OK) return rc;
        if (!(lb->p5_ns <= lb->median_ns && lb->median_ns <= lb->p95_ns && lb->p95_ns <= lb->max_ns))
            return yrig__bad(r, o, field, "p5_s <= median_s <= p95_s <= max_s does not hold");
    } else {
        snprintf(f, sizeof f, "%s.lo_s", field);
        if ((rc = yrig__secs(r, o, "lo_s", f, &lb->lo_ns, -YRIG__S_MAX, YRIG__S_MAX, true)) != YRIG_OK) return rc;
        snprintf(f, sizeof f, "%s.hi_s", field);
        if ((rc = yrig__secs(r, o, "hi_s", f, &lb->hi_ns, -YRIG__S_MAX, YRIG__S_MAX, true)) != YRIG_OK) return rc;
        if (lb->lo_ns > lb->hi_ns) return yrig__bad(r, o, field, "lo_s <= hi_s does not hold");
    }
    snprintf(f, sizeof f, "%s.date", field);
    if ((rc = yrig__date(r, o, "date", f, lb->date)) != YRIG_OK) return rc;
    if ((rc = yrig__sha(r, o, field, lb->sha256)) != YRIG_OK) return rc;
    lb->set = true;
    return YRIG_OK;
}

static int yrig__family_id(const char* name) {
    int k;
    for (k = 1; k <= YBOX_FAMILY_LAST; k++)
        if (strcmp(ybox_family_name(k), name) == 0) return k;
    if (strcmp(name, "lsl") == 0) return YRIG_FAMILY_LSL;
    return 0;
}

static int yrig__role(yrig__rd* r, const yjs_member* m, yrig_role* ro) {
    static const char* const keys[] = { "family", "key", "options", "pulse_s", "latency", "bounds", NULL };
    static const char* const okeys[] = { "baud", "latched", "ftdi_latency_s", "buttons", NULL };
    const yjs_value* o = m->value;
    const yjs_value* opt;
    char f[96];
    int rc;
    if (!yrig__name_ok(m->key, 31, true)) {
        uint32_t line, col;
        yjs_line_col(r->text, r->n, m->key_pos, &line, &col);
        return yrig__err(r->err, r->cap, YRIG_ERR_FORMAT,
                         "%s: roles: \"%.40s\" is not a role name (1 to 31 of A-Z a-z 0-9 _ . -) (line %u, column %u)", r->what, m->key,
                         (unsigned)line, (unsigned)col);
    }
    memset(ro, 0, sizeof *ro);
    ro->ftdi_latency_ns = -1;
    ro->pulse_ns = -1;
    yrig__copy(ro->name, sizeof ro->name, m->key, m->key_len);
    snprintf(f, sizeof f, "roles.%s", ro->name);
    if ((rc = yrig__keys(r, o, f, keys)) != YRIG_OK) return rc;
    snprintf(f, sizeof f, "roles.%s.family", ro->name);
    if ((rc = yrig__str(r, o, "family", f, ro->family, 15, true)) != YRIG_OK) return rc;
    ro->family_id = yrig__family_id(ro->family);
    if (!ro->family_id) return yrig__bad(r, yjs_get(o, "family"), f, "\"%s\" is not a ysp/box.h family or lsl", ro->family);
    snprintf(f, sizeof f, "roles.%s.key", ro->name);
    if ((rc = yrig__str(r, o, "key", f, ro->key, sizeof ro->key - 1, true)) != YRIG_OK) return rc;
    if (ro->family_id == YRIG_FAMILY_LSL && (strncmp(ro->key, "lsl:", 4) != 0 || strchr(ro->key, '\'')))
        return yrig__bad(r, yjs_get(o, "key"), f, "an lsl key is lsl:<name>:<type>:<source_id>:<hostname>, no quote");
    if (ro->family_id != YRIG_FAMILY_LSL && strncmp(ro->key, "lsl:", 4) == 0)
        return yrig__bad(r, yjs_get(o, "key"), f, "an lsl key needs the family lsl");
    snprintf(f, sizeof f, "roles.%s.pulse_s", ro->name);
    if (ro->family_id == YRIG_FAMILY_LSL && yjs_get(o, "pulse_s"))
        return yrig__bad(r, yjs_get(o, "pulse_s"), f, "an lsl role has no pulse");
    if ((rc = yrig__secs(r, o, "pulse_s", f, &ro->pulse_ns, 1, (int64_t)10000000000LL, false)) != YRIG_OK) return rc;
    opt = yjs_get(o, "options");
    if (opt) {
        const yjs_value* v;
        int64_t baud = 0;
        snprintf(f, sizeof f, "roles.%s.options", ro->name);
        if ((rc = yrig__keys(r, opt, f, okeys)) != YRIG_OK) return rc;
        snprintf(f, sizeof f, "roles.%s.options", ro->name);
        if (ro->family_id == YRIG_FAMILY_LSL && (yjs_get(opt, "baud") || yjs_get(opt, "ftdi_latency_s") || yjs_get(opt, "latched")))
            return yrig__bad(r, opt, f, "an lsl role has no serial options (baud, ftdi_latency_s, latched)");
        snprintf(f, sizeof f, "roles.%s.options.baud", ro->name);
        if ((rc = yrig__int(r, opt, "baud", f, &baud, 1, 10000000, false)) != YRIG_OK) return rc;
        ro->baud = (uint32_t)baud;
        snprintf(f, sizeof f, "roles.%s.options.ftdi_latency_s", ro->name);
        if ((rc = yrig__secs(r, opt, "ftdi_latency_s", f, &ro->ftdi_latency_ns, 1000000, 255000000, false)) != YRIG_OK) return rc;
        if ((v = yjs_get(opt, "latched")) != NULL) {
            snprintf(f, sizeof f, "roles.%s.options.latched", ro->name);
            if (yjs_bool(v, &ro->latched) != YJS_OK) return yrig__bad(r, v, f, "must be true or false");
            if (ro->latched && ro->family_id != YBOX_MMBTS) return yrig__bad(r, v, f, "only an mmbts can be latched");
        }
        if ((v = yjs_get(opt, "buttons")) != NULL) {
            uint32_t i;
            snprintf(f, sizeof f, "roles.%s.options.buttons", ro->name);
            if (v->type != YJS_OBJECT) return yrig__bad(r, v, f, "must be an object");
            if (v->n > YRIG_MAX_BUTTONS) return yrig__bad(r, v, f, "more than %d buttons", YRIG_MAX_BUTTONS);
            for (i = 0; i < v->n; i++) {
                const yjs_member* b = &v->u.members[i];
                char bf[96];
                size_t k;
                snprintf(bf, sizeof bf, "roles.%s.options.buttons.%.15s", ro->name, b->key);
                if (b->key_len == 0 || b->key_len > 15) return yrig__bad(r, b->value, bf, "a button code is 1 to 15 bytes");
                for (k = 0; k < b->key_len; k++)
                    if ((unsigned char)b->key[k] < 0x20) return yrig__bad(r, b->value, bf, "a button code has a control character");
                if ((rc = yrig__str(r, v, b->key, bf, ro->buttons[i].name, 31, true)) != YRIG_OK) return rc;
                yrig__copy(ro->buttons[i].code, sizeof ro->buttons[i].code, b->key, b->key_len);
            }
            ro->n_buttons = (int)v->n;
        }
    }
    if (yjs_get(o, "latency")) {
        snprintf(f, sizeof f, "roles.%s.latency", ro->name);
        if ((rc = yrig__loopback(r, yjs_get(o, "latency"), f, &ro->latency, true)) != YRIG_OK) return rc;
    }
    if (yjs_get(o, "bounds")) {
        snprintf(f, sizeof f, "roles.%s.bounds", ro->name);
        if ((rc = yrig__loopback(r, yjs_get(o, "bounds"), f, &ro->bounds, false)) != YRIG_OK) return rc;
    }
    return YRIG_OK;
}

static bool yrig__file_name_ok(const char* s) {
    size_t i, n = strlen(s);
    if (n == 0 || n > 63 || s[0] == '.') return false;
    for (i = 0; i < n; i++)
        if (s[i] == '/' || s[i] == '\\' || s[i] == ':' || (unsigned char)s[i] < 0x20) return false;
    return true;
}

static int yrig__display(yrig__rd* r, const yjs_value* o, yrig_display* d) {
    static const char* const keys[] = { "onset_offset_s", "calibration", NULL };
    static const char* const ckeys[] = { "file", "sha256", NULL };
    const yjs_value* c;
    int rc;
    if ((rc = yrig__keys(r, o, "display", keys)) != YRIG_OK) return rc;
    if ((rc = yrig__secs(r, o, "onset_offset_s", "display.onset_offset_s", &d->onset_offset_ns, -(int64_t)1000000000, (int64_t)1000000000,
                         true)) != YRIG_OK)
        return rc;
    if ((c = yjs_get(o, "calibration")) != NULL) {
        if ((rc = yrig__keys(r, c, "display.calibration", ckeys)) != YRIG_OK) return rc;
        if ((rc = yrig__str(r, c, "file", "display.calibration.file", d->calibration, 63, true)) != YRIG_OK) return rc;
        if (!yrig__file_name_ok(d->calibration))
            return yrig__bad(r, yjs_get(c, "file"), "display.calibration.file", "must be a file name in the rig folder, with no separator");
        if ((rc = yrig__sha(r, c, "display.calibration", d->calibration_sha256)) != YRIG_OK) return rc;
    }
    d->set = true;
    return YRIG_OK;
}

static void yrig__note(yrig_profile* p, const char* fmt, ...) {
    va_list ap;
    if (p->n_notes >= YRIG_MAX_NOTES) return;
    va_start(ap, fmt);
    vsnprintf(p->notes[p->n_notes++], sizeof p->notes[0], fmt, ap);
    va_end(ap);
}

YRIG_API int yrig_read_latency(const char* text, size_t n, yrig_loopback* out, char* err, size_t cap) {
    size_t i = 0;
    int have = 0;
    yrig_loopback lb;
    if (!text || !out) return YRIG_ERR_ARG;
    memset(&lb, 0, sizeof lb);
    while (i < n) {
        size_t e = i, sp;
        const char* name = text + i;
        const char* val;
        size_t nl, vl;
        while (e < n && text[e] != '\n') e++;
        for (sp = i; sp < e && text[sp] != ' '; sp++) { }
        nl = sp - i;
        val = text + (sp < e ? sp + 1 : e);
        vl = sp < e ? e - sp - 1 : 0;
        if (vl && val[vl - 1] == '\r') vl--;
#define YRIG__IS(lit) (nl == sizeof(lit) - 1 && memcmp(name, lit, nl) == 0)
        if (YRIG__IS("format")) {
            if (!(vl == 17 && memcmp(val, "ysp-out-latency 1", 17) == 0))
                return yrig__err(err, cap, YRIG_ERR_FORMAT, "not an output latency file (format %.*s)", (int)(vl < 40 ? vl : 40), val);
            have |= 1;
        } else if (YRIG__IS("n")) {
            int64_t v;
            if (yjs_parse_fixed(val, vl, 0, &v) != YJS_OK || v < 0 || v > 100000000) return yrig__err(err, cap, YRIG_ERR_FORMAT, "bad n");
            lb.n = (int32_t)v;
            have |= 2;
        } else if (YRIG__IS("p5_s") || YRIG__IS("median_s") || YRIG__IS("p95_s") || YRIG__IS("max_s")) {
            int64_t v;
            if (yjs_parse_fixed(val, vl, 9, &v) != YJS_OK) return yrig__err(err, cap, YRIG_ERR_FORMAT, "bad %.*s", (int)nl, name);
            if (YRIG__IS("p5_s")) { lb.p5_ns = v; have |= 4; }
            else if (YRIG__IS("median_s")) { lb.median_ns = v; have |= 8; }
            else if (YRIG__IS("p95_s")) { lb.p95_ns = v; have |= 16; }
            else { lb.max_ns = v; have |= 32; }
        } else if (YRIG__IS("date")) {
            if (vl >= sizeof lb.date) return yrig__err(err, cap, YRIG_ERR_FORMAT, "bad date");
            yrig__copy(lb.date, sizeof lb.date, val, vl);
            have |= 64;
        }
#undef YRIG__IS
        i = e + 1;
    }
    if (!(have & 1)) return yrig__err(err, cap, YRIG_ERR_FORMAT, "not an output latency file (no format line)");
    if (have != 127) return yrig__err(err, cap, YRIG_ERR_FORMAT, "an output latency file without its summary (no edges?)");
    yrig__sha_hex(text, n, lb.sha256);
    lb.set = true;
    lb.check = YRIG_CHECK_OK;
    *out = lb;
    return YRIG_OK;
}

/* The check of one file the profile names. */
static int yrig__check_file(const char* dir, const char* sub, const char* name, const char* sha, const yrig_loopback* lb) {
    char path[1100], hex[65];
    char* data;
    size_t n;
    int rc, w;
    if (sub) w = snprintf(path, sizeof path, "%s" YRIG__SEP "%s" YRIG__SEP "%s", dir, sub, name);
    else w = snprintf(path, sizeof path, "%s" YRIG__SEP "%s", dir, name);
    if (w < 0 || (size_t)w >= sizeof path) return YRIG_CHECK_MISSING;
    rc = yrig__read(path, (size_t)64 << 20, &data, &n);
    if (rc != YRIG_OK) return rc == YRIG_ERR_MISSING ? YRIG_CHECK_MISSING : YRIG_CHECK_CHANGED;
    yrig__sha_hex(data, n, hex);
    rc = strcmp(hex, sha) == 0 ? YRIG_CHECK_OK : YRIG_CHECK_CHANGED;
    if (rc == YRIG_CHECK_OK && lb && n >= 24 && strstr(data, "\nformat ysp-out-latency 1") != NULL) {
        yrig_loopback f;
        if (yrig_read_latency(data, n, &f, NULL, 0) != YRIG_OK || f.n != lb->n || f.p5_ns != lb->p5_ns || f.median_ns != lb->median_ns ||
            f.p95_ns != lb->p95_ns || f.max_ns != lb->max_ns || strcmp(f.date, lb->date) != 0)
            rc = YRIG_CHECK_DIFFERS;
    }
    free(data);
    return rc;
}

static void yrig__checks(yrig_profile* p, const char* dir) {
    int i;
    for (i = 0; i < p->n_roles; i++) {
        yrig_role* ro = &p->role[i];
        int k;
        for (k = 0; k < 2; k++) {
            yrig_loopback* lb = k ? &ro->bounds : &ro->latency;
            char name[80];
            if (!lb->set) { lb->check = YRIG_CHECK_NONE; continue; }
            if (!dir) { lb->check = YRIG_CHECK_NOT_RUN; continue; }
            snprintf(name, sizeof name, "%s.txt", lb->sha256);
            lb->check = yrig__check_file(dir, "loopback", name, lb->sha256, k ? NULL : lb);
            if (lb->check != YRIG_CHECK_OK)
                yrig__note(p, "roles.%s.%s: loopback/%.12s....txt %s: the role's tier is UNKNOWN", ro->name, k ? "bounds" : "latency",
                           lb->sha256, lb->check == YRIG_CHECK_MISSING ? "is missing" : lb->check == YRIG_CHECK_CHANGED ? "has changed"
                                                                                         : "does not match the profile's summary");
        }
    }
    if (p->display.set && p->display.calibration[0]) {
        if (!dir) p->display.check = YRIG_CHECK_NOT_RUN;
        else {
            p->display.check = yrig__check_file(dir, NULL, p->display.calibration, p->display.calibration_sha256, NULL);
            if (p->display.check != YRIG_CHECK_OK)
                yrig__note(p, "display.calibration: %.40s %s", p->display.calibration,
                           p->display.check == YRIG_CHECK_MISSING ? "is missing" : "has changed");
        }
    }
}

static int yrig__parse(yrig_profile* p, const char* text, size_t n, const char* dir, const char* what, char* err, size_t cap) {
    static const char* const keys[] = { "format", "rig", "written", "roles", "display", NULL };
    yjs_arena a;
    yjs_value* root;
    yjs_error e;
    yjs_opts o;
    yrig__rd r;
    const yjs_value* v;
    int rc;
    uint32_t i;
    if (!p || (!text && n)) return yrig__err(err, cap, YRIG_ERR_ARG, "yrig_parse: NULL");
    memset(p, 0, sizeof *p);
    if (err && cap) err[0] = '\0';
    r.what = what;
    r.text = text;
    r.n = n;
    r.err = err;
    r.cap = cap;
    if (n > YRIG_MAX_BYTES) return yrig__err(err, cap, YRIG_ERR_FORMAT, "%s: larger than %u bytes", what, (unsigned)YRIG_MAX_BYTES);
    memset(&o, 0, sizeof o);
    o.max_depth = 8;
    yjs_arena_init_heap(&a, (size_t)16 << 20);
    if (yjs_parse(&a, text, n, &o, &root, &e) != YJS_OK) {
        yjs_arena_free(&a);
        return yrig__err(err, cap, e.code == YJS_ERR_NOMEM ? YRIG_ERR_IO : YRIG_ERR_JSON, "%s: line %u, column %u: %s", what,
                         (unsigned)e.line, (unsigned)e.column, e.msg);
    }
    rc = yrig__keys(&r, root, "the profile", keys);
    if (rc == YRIG_OK) {
        v = yjs_get(root, "format");
        if (!v) rc = yrig__bad(&r, root, "format", "is missing");
        else if (v->type != YJS_STRING) rc = yrig__bad(&r, v, "format", "must be a string");
        else if (strcmp(v->u.s, YRIG_FORMAT) != 0)
            rc = yrig__bad(&r, v, "format", "\"%.40s\" is not \"" YRIG_FORMAT "\" (a newer ysp wrote it?)", v->u.s);
    }
    if (rc == YRIG_OK) rc = yrig__str(&r, root, "rig", "rig", p->rig, sizeof p->rig - 1, true);
    if (rc == YRIG_OK) rc = yrig__date(&r, root, "written", "written", p->written);
    if (rc == YRIG_OK) {
        v = yjs_get(root, "roles");
        if (!v) rc = yrig__bad(&r, root, "roles", "is missing");
        else if (v->type != YJS_OBJECT) rc = yrig__bad(&r, v, "roles", "must be an object");
        else if (v->n > YRIG_MAX_ROLES) rc = yrig__bad(&r, v, "roles", "more than %d roles", YRIG_MAX_ROLES);
        for (i = 0; rc == YRIG_OK && v && i < v->n; i++) rc = yrig__role(&r, &v->u.members[i], &p->role[i]);
        if (rc == YRIG_OK) p->n_roles = (int)v->n;
    }
    if (rc == YRIG_OK && (v = yjs_get(root, "display")) != NULL) rc = yrig__display(&r, v, &p->display);
    yjs_arena_free(&a);
    if (rc != YRIG_OK) {
        memset(p, 0, sizeof *p);
        return rc;
    }
    if (dir) yrig__copy(p->dir, sizeof p->dir, dir, strlen(dir));
    yrig__checks(p, dir);
    return YRIG_OK;
}

YRIG_API int yrig_parse(yrig_profile* p, const char* text, size_t n, const char* dir, char* err, size_t cap) {
    return yrig__parse(p, text, n, dir, "the rig profile", err, cap);
}

YRIG_API int yrig_load_file(yrig_profile* p, const char* path, char* err, size_t cap) {
    char dir[1024];
    char* text;
    size_t n, k;
    int rc;
    if (!p || !path || !path[0]) return yrig__err(err, cap, YRIG_ERR_ARG, "yrig_load_file: no path");
    rc = yrig__read(path, YRIG_MAX_BYTES, &text, &n);
    if (rc == YRIG_ERR_MISSING) return yrig__err(err, cap, rc, "no rig profile at %s", path);
    if (rc == YRIG_ERR_FORMAT) return yrig__err(err, cap, rc, "%s: larger than %u bytes", path, (unsigned)YRIG_MAX_BYTES);
    if (rc != YRIG_OK) return yrig__err(err, cap, rc, "%s: cannot read it", path);
    k = strlen(path);
    if (k >= sizeof dir) k = sizeof dir - 1;
    memcpy(dir, path, k);
    dir[k] = '\0';
    while (k > 0 && dir[k - 1] != '/' && dir[k - 1] != '\\') k--;
    if (k > 1) dir[k - 1] = '\0';
    else if (k == 1) dir[1] = '\0';        /* "/profile.json" */
    else { dir[0] = '.'; dir[1] = '\0'; }
    rc = yrig__parse(p, text, n, dir, path, err, cap);
    free(text);
    return rc;
}

YRIG_API int yrig_load(yrig_profile* p, const char* name, char* err, size_t cap) {
    char dir[512], path[600];
    if (!p) return yrig__err(err, cap, YRIG_ERR_ARG, "yrig_load: NULL");
    if (yrig_dir(dir, sizeof dir) != YRIG_OK)
        return yrig__err(err, cap, YRIG_ERR_FOLDER,
                         "no rig folder: the config folder is missing, or another user can write it (ysp/rt.h yrt_user_dir)");
    if (yrig_path(dir, name, path, sizeof path) != YRIG_OK)
        return yrig__err(err, cap, YRIG_ERR_ARG, "\"%.40s\" is not a profile name (1 to 63 of A-Z a-z 0-9 _ -)", name ? name : "");
    return yrig_load_file(p, path, err, cap);
}

/* ======================================================================= *
 *  WRITING
 * ======================================================================= */

YRIG_API void yrig_init(yrig_profile* p, const char* rig) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    yrig__copy(p->rig, sizeof p->rig, rig ? rig : "rig", strlen(rig ? rig : "rig"));
    memcpy(p->written, "1970-01-01T00:00:00Z", 21);
}

static bool yrig__set_secs(yjs_arena* a, yjs_value* o, const char* key, int64_t ns) {
    return yjs_set(a, o, key, yjs_new_fixed(a, ns, 9)) == YJS_OK;
}
static bool yrig__set_str(yjs_arena* a, yjs_value* o, const char* key, const char* s) {
    return yjs_set(a, o, key, yjs_new_stringz(a, s)) == YJS_OK;
}

static yjs_value* yrig__lb_json(yjs_arena* a, const yrig_loopback* lb, bool latency) {
    yjs_value* o = yjs_new(a, YJS_OBJECT);
    bool ok = o && yjs_set(a, o, "n", yjs_new_int(a, lb->n)) == YJS_OK && yrig__set_str(a, o, "date", lb->date) &&
              yrig__set_str(a, o, "sha256", lb->sha256);
    if (latency)
        ok = ok && yrig__set_secs(a, o, "p5_s", lb->p5_ns) && yrig__set_secs(a, o, "median_s", lb->median_ns) &&
             yrig__set_secs(a, o, "p95_s", lb->p95_ns) && yrig__set_secs(a, o, "max_s", lb->max_ns);
    else
        ok = ok && yrig__set_secs(a, o, "lo_s", lb->lo_ns) && yrig__set_secs(a, o, "hi_s", lb->hi_ns);
    return ok ? o : NULL;
}

static yjs_value* yrig__json(yjs_arena* a, const yrig_profile* p) {
    yjs_value* root = yjs_new(a, YJS_OBJECT);
    yjs_value* roles = yjs_new(a, YJS_OBJECT);
    int i, k;
    bool ok = root && roles && yrig__set_str(a, root, "format", YRIG_FORMAT) && yrig__set_str(a, root, "rig", p->rig) &&
              yrig__set_str(a, root, "written", p->written) && yjs_set(a, root, "roles", roles) == YJS_OK;
    if (p->n_roles < 0 || p->n_roles > YRIG_MAX_ROLES) return NULL;
    for (i = 0; ok && i < p->n_roles; i++) {
        const yrig_role* ro = &p->role[i];
        yjs_value* o = yjs_new(a, YJS_OBJECT);
        yjs_value* opt = yjs_new(a, YJS_OBJECT);
        ok = o && opt && yrig__set_str(a, o, "family", ro->family) && yrig__set_str(a, o, "key", ro->key);
        if (ok && ro->baud) ok = yjs_set(a, opt, "baud", yjs_new_int(a, ro->baud)) == YJS_OK;
        if (ok && ro->latched) ok = yjs_set(a, opt, "latched", yjs_new(a, YJS_TRUE)) == YJS_OK;
        if (ok && ro->ftdi_latency_ns >= 0) ok = yrig__set_secs(a, opt, "ftdi_latency_s", ro->ftdi_latency_ns);
        if (ok && ro->n_buttons > 0) {
            yjs_value* b = yjs_new(a, YJS_OBJECT);
            ok = b != NULL && ro->n_buttons <= YRIG_MAX_BUTTONS;
            for (k = 0; ok && k < ro->n_buttons; k++) ok = yrig__set_str(a, b, ro->buttons[k].code, ro->buttons[k].name);
            if (ok) ok = yjs_set(a, opt, "buttons", b) == YJS_OK;
        }
        if (ok && opt->n) ok = yjs_set(a, o, "options", opt) == YJS_OK;
        if (ok && ro->pulse_ns >= 0) ok = yrig__set_secs(a, o, "pulse_s", ro->pulse_ns);
        if (ok && ro->latency.set) ok = yjs_set(a, o, "latency", yrig__lb_json(a, &ro->latency, true)) == YJS_OK;
        if (ok && ro->bounds.set) ok = yjs_set(a, o, "bounds", yrig__lb_json(a, &ro->bounds, false)) == YJS_OK;
        if (ok) ok = yjs_set(a, roles, ro->name, o) == YJS_OK;
    }
    if (ok && p->display.set) {
        yjs_value* d = yjs_new(a, YJS_OBJECT);
        ok = d && yrig__set_secs(a, d, "onset_offset_s", p->display.onset_offset_ns);
        if (ok && p->display.calibration[0]) {
            yjs_value* c = yjs_new(a, YJS_OBJECT);
            ok = c && yrig__set_str(a, c, "file", p->display.calibration) && yrig__set_str(a, c, "sha256", p->display.calibration_sha256) &&
                 yjs_set(a, d, "calibration", c) == YJS_OK;
        }
        if (ok) ok = yjs_set(a, root, "display", d) == YJS_OK;
    }
    return ok ? root : NULL;
}

YRIG_API size_t yrig_write(const yrig_profile* p, char* buf, size_t cap) {
    yjs_arena a;
    yjs_value* root;
    size_t n = 0;
    if (!p) return 0;
    yjs_arena_init_heap(&a, (size_t)16 << 20);
    root = yrig__json(&a, p);
    if (root) {
        n = yjs_write_mem(root, YJS_WRITE_PRETTY, buf, cap);
        if (n) {
            /* one LF after the value: a text file */
            if (buf && n + 1 < cap) { buf[n] = '\n'; buf[n + 1] = '\0'; }
            else if (buf && cap) buf[cap - 1] = '\0';
            n++;
        }
    }
    yjs_arena_free(&a);
    return n;
}

/* The canonical bytes in a malloc'd buffer, checked by reading them back:
 * a profile that would not load is never written or hashed. */
static char* yrig__bytes(const yrig_profile* p, size_t* n, char* err, size_t cap) {
    size_t need = yrig_write(p, NULL, 0);
    char* b;
    yrig_profile* back;
    int rc;
    if (!need) { yrig__err(err, cap, YRIG_ERR_FORMAT, "the profile has a value its format cannot hold"); return NULL; }
    b = (char*)malloc(need + 1);
    back = (yrig_profile*)malloc(sizeof *back);
    if (!b || !back || yrig_write(p, b, need + 1) != need) {
        free(b);
        free(back);
        yrig__err(err, cap, YRIG_ERR_IO, "out of memory");
        return NULL;
    }
    rc = yrig__parse(back, b, need, NULL, "the profile in memory", err, cap);
    free(back);
    if (rc != YRIG_OK) { free(b); return NULL; }
    *n = need;
    return b;
}

YRIG_API int yrig_hash(const yrig_profile* p, char hex[65]) {
    size_t n;
    char* b;
    if (!hex) return YRIG_ERR_ARG;
    hex[0] = '\0';
    b = yrig__bytes(p, &n, NULL, 0);
    if (!b) return YRIG_ERR_FORMAT;
    yrig__sha_hex(b, n, hex);
    free(b);
    return YRIG_OK;
}

/* Now as "YYYY-MM-DDTHH:MM:SSZ" (days to the civil date, so no gmtime and
 * its shared buffer). */
static void yrig__now_utc(char out[24]) {
    int64_t t = (int64_t)time(NULL), days = t / 86400, sec = t % 86400, z, era, doe, yoe, y, doy, mp, d, m;
    if (sec < 0) { sec += 86400; days--; }
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, 24, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)y, (int)m, (int)d, (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
}

YRIG_API int yrig_save_file(yrig_profile* p, const char* path, char* err, size_t cap) {
    char* b;
    size_t n, k;
    char dir[1024];
    int rc;
    if (!p || !path || !path[0]) return yrig__err(err, cap, YRIG_ERR_ARG, "yrig_save_file: no path");
    yrig__now_utc(p->written);
    b = yrig__bytes(p, &n, err, cap);
    if (!b) return YRIG_ERR_FORMAT;
    k = strlen(path);
    if (k >= sizeof dir) { free(b); return yrig__err(err, cap, YRIG_ERR_ARG, "%s: path too long", path); }
    memcpy(dir, path, k + 1);
    while (k > 0 && dir[k - 1] != '/' && dir[k - 1] != '\\') k--;
    if (k > 1) {
        dir[k - 1] = '\0';
        if (!yrig__mkdirs(dir)) { free(b); return yrig__err(err, cap, YRIG_ERR_IO, "%s: cannot make the folder", dir); }
    }
    rc = yrig__write_file(path, b, n);
    free(b);
    if (rc != YRIG_OK) return yrig__err(err, cap, rc, "%s: cannot write it", path);
    return YRIG_OK;
}

YRIG_API int yrig_save(yrig_profile* p, const char* name, char* err, size_t cap) {
    char dir[512], path[600];
    if (yrig_dir(dir, sizeof dir) != YRIG_OK)
        return yrig__err(err, cap, YRIG_ERR_FOLDER,
                         "no rig folder: the config folder is missing, or another user can write it (ysp/rt.h yrt_user_dir)");
    if (yrig_path(dir, name, path, sizeof path) != YRIG_OK)
        return yrig__err(err, cap, YRIG_ERR_ARG, "\"%.40s\" is not a profile name (1 to 63 of A-Z a-z 0-9 _ -)", name ? name : "");
    return yrig_save_file(p, path, err, cap);
}

YRIG_API yrig_role* yrig_find(const yrig_profile* p, const char* role) {
    int i;
    if (!p || !role) return NULL;
    for (i = 0; i < p->n_roles && i < YRIG_MAX_ROLES; i++)
        if (strcmp(p->role[i].name, role) == 0) return (yrig_role*)&p->role[i];
    return NULL;
}

YRIG_API yrig_role* yrig_add(yrig_profile* p, const char* role) {
    yrig_role* r = yrig_find(p, role);
    if (r) return r;
    if (!p || !yrig__name_ok(role, 31, true) || p->n_roles >= YRIG_MAX_ROLES) return NULL;
    r = &p->role[p->n_roles++];
    memset(r, 0, sizeof *r);
    yrig__copy(r->name, sizeof r->name, role, strlen(role));
    r->ftdi_latency_ns = -1;
    r->pulse_ns = -1;
    return r;
}

YRIG_API bool yrig_remove(yrig_profile* p, const char* role) {
    yrig_role* r = yrig_find(p, role);
    int i;
    if (!r) return false;
    i = (int)(r - p->role);
    memmove(&p->role[i], &p->role[i + 1], (size_t)(p->n_roles - i - 1) * sizeof(yrig_role));
    p->n_roles--;
    return true;
}

YRIG_API int yrig_bind(yrig_profile* p, const char* role, int family, const char* key) {
    yrig_role* r;
    const char* fname;
    if (!p || !role || !key || !key[0] || strlen(key) >= sizeof r->key ||
        ((family < 1 || family > YBOX_FAMILY_LAST) && family != YRIG_FAMILY_LSL))
        return YRIG_ERR_ARG;
    if ((family == YRIG_FAMILY_LSL) != (strncmp(key, "lsl:", 4) == 0) || strchr(key, '\'')) return YRIG_ERR_ARG;
    r = yrig_find(p, role);
    if (!r && p->n_roles >= YRIG_MAX_ROLES) return YRIG_ERR_FULL;
    if (!r && !(r = yrig_add(p, role))) return YRIG_ERR_ARG;
    fname = family == YRIG_FAMILY_LSL ? "lsl" : ybox_family_name(family);
    yrig__copy(r->family, sizeof r->family, fname, strlen(fname));
    r->family_id = family;
    yrig__copy(r->key, sizeof r->key, key, strlen(key));
    if (family != YBOX_MMBTS) r->latched = false;
    if (family == YRIG_FAMILY_LSL) {   /* no serial options, no pulse */
        r->baud = 0;
        r->ftdi_latency_ns = -1;
        r->pulse_ns = -1;
    }
    memset(&r->latency, 0, sizeof r->latency);   /* measured on the old device */
    memset(&r->bounds, 0, sizeof r->bounds);
    return YRIG_OK;
}

YRIG_API int yrig_store_loopback(const char* dir, const void* data, size_t n, char sha_hex[65], char* err, size_t cap) {
    char sub[600], path[700];
    int rc;
    if (!dir || !dir[0] || (!data && n) || !sha_hex) return yrig__err(err, cap, YRIG_ERR_ARG, "yrig_store_loopback: NULL");
    yrig__sha_hex(data, n, sha_hex);
    if (snprintf(sub, sizeof sub, "%s" YRIG__SEP "loopback", dir) >= (int)sizeof sub) return yrig__err(err, cap, YRIG_ERR_ARG, "path too long");
    if (!yrig__mkdirs(sub)) return yrig__err(err, cap, YRIG_ERR_IO, "%s: cannot make the folder", sub);
    snprintf(path, sizeof path, "%s" YRIG__SEP "%s.txt", sub, sha_hex);
    rc = yrig__write_file(path, data, n);
    if (rc != YRIG_OK) return yrig__err(err, cap, rc, "%s: cannot write it", path);
    return YRIG_OK;
}

/* ======================================================================= *
 *  BINDING
 * ======================================================================= */

YRIG_API int yrig_desc(const yrig_profile* p, const char* role, ydev_desc* d, char* err, size_t cap) {
    const yrig_role* r;
    int fam;
    if (!p || !role || !d) return yrig__err(err, cap, YRIG_ERR_ARG, "yrig_desc: NULL");
    r = yrig_find(p, role);
    if (!r) return yrig__err(err, cap, YRIG_ERR_MISSING, "the rig profile \"%s\" has no role \"%.31s\"", p->rig, role);
    /* by its name, which is what the file holds */
    fam = yrig__family_id(r->family);
    if (!fam || !r->key[0]) return yrig__err(err, cap, YRIG_ERR_FORMAT, "roles.%s: no device is bound", r->name);
    if (fam == YRIG_FAMILY_LSL)
        return yrig__err(err, cap, YRIG_ERR_FORMAT, "roles.%s is an LSL stream: start it with ysp/net.h and the role's key",
                         r->name);
    d->role = r->name;
    d->family = fam;
    d->key = r->key;
    d->baud = r->baud;
    d->latched = r->latched;
    if (r->pulse_ns >= 0) d->pulse_ns = r->pulse_ns;
    return YRIG_OK;
}

YRIG_API bool yrig_open_device(ydev_device* dev, const yrig_profile* p, const char* role, const ydev_desc* base) {
    ydev_desc d;
    char msg[sizeof dev->err];
    if (!dev) return false;
    if (base) d = *base;
    else memset(&d, 0, sizeof d);
    /* a refusal before the open goes where an open's message goes, so the
     * caller reads one place */
    if (yrig_desc(p, role, &d, dev->err, sizeof dev->err) != YRIG_OK) return false;
    if (!ydev_open(dev, &d)) {
        snprintf(msg, sizeof msg, "%s", ydev_error(dev));
        yrig__err(dev->err, sizeof dev->err, YRIG_ERR_ARG, "roles.%.31s: %s", role, msg);
        return false;
    }
    return true;
}

/* Tier 1 with the role's bounds when they checked and fam maps a device
 * clock; else tier UNKNOWN. */
static yin_source yrig__tier(const yrig_role* r, int fam, yin_source s) {
    bool clock = fam == YRIG_FAMILY_LSL || (fam >= 1 && fam <= YBOX_FAMILY_LAST && (ybox_caps(fam) & YBOX_CAP_IN));
    s.tier = (uint8_t)YIN_TIER_UNKNOWN;
    s.lo_ns = s.hi_ns = 0;
    if (r && r->bounds.set && r->bounds.check == YRIG_CHECK_OK && clock && yrig__family_id(r->family) == fam) {
        /* ysp/input.h's tier 1: a device clock whose fit a loopback checked */
        s.tier = (uint8_t)YIN_TIER_1;
        s.lo_ns = r->bounds.lo_ns;
        s.hi_ns = r->bounds.hi_ns;
        s.note = "ysp_rig: bounds from a loopback the rig profile names, checked by SHA-256";
    } else {
        s.note = "ysp_rig: no checked loopback for this role: tier UNKNOWN";
    }
    return s;
}

YRIG_API yin_source yrig_source(const yrig_profile* p, const ydev_device* dev) {
    yin_source s;
    memset(&s, 0, sizeof s);
    if (!dev) return s;
    return yrig__tier(dev->d.role && p ? yrig_find(p, dev->d.role) : NULL, dev->d.family, ydev_source(dev));
}

YRIG_API yin_source yrig_source_role(const yrig_profile* p, const char* role, yin_source src) {
    const yrig_role* r = p && role ? yrig_find(p, role) : NULL;
    return yrig__tier(r, r ? yrig__family_id(r->family) : 0, src);
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_RIGFILE_IMPLEMENTATION_GUARD */
#endif /* YSP_RIGFILE_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
