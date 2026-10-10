/* ysp/box.h - v0.3.0 - public domain single-header box protocol decoders
 *
 *   Bytes from a response box, a trigger box or a microcontroller board in,
 *   ysp/input.h events and clock pairs out; and the bytes a host sends to
 *   them: queries, and output codes for trigger lines. Families:
 *     YBOX_XID        Cedrus XID devices (RB-x40, Lumina, c-pod, m-pod,
 *                     StimTracker in XID mode) on USB-serial; in and out
 *     YBOX_LINE       the ysp line protocol (below): Teensy, RP2040 or any
 *                     board with a microsecond counter and a serial port;
 *                     in and out
 *     YBOX_PHOTO      the 6-byte photodiode frame of
 *                     tests/loopback/screen_loopback.c; in
 *     YBOX_TRIGGERBOX Brain Products TriggerBox: a byte holds 8 lines; out
 *     YBOX_BIOSEMI    BioSemi USB trigger interface: a byte is an 8 ms
 *                     pulse on 8 lines; out
 *     YBOX_MMBTS      Neurospec MMBT-S: a byte is an 8 ms pulse (switch P)
 *                     or holds (switch S); out
 *     YBOX_LINES      a serial port's DTR and RTS lines as two TTL outputs;
 *                     no bytes; out
 *     YBOX_PARALLEL   the data lines of a parallel port; out
 *   ysp/device.h runs a decoder on a reader thread, feeds the pairs to
 *   ysp/rt.h's device clock fit and maps each event's device time to the
 *   ysp_rt clock, and writes the encoders' bytes. This header does none of
 *   that: it does no I/O, starts no thread, reads no clock and allocates
 *   nothing, so a test or an analysis can decode captured bytes with it
 *   alone.
 *
 *   REQUIRES ysp/input.h (the event) beside it; this header includes it.
 *   C99 is the floor: it builds as C99, C11 and C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_BOX_IMPLEMENTATION
 *       #include "ysp/box.h"
 *
 *       ybox_decoder dec;
 *       yin_event ev[32];
 *       ybox_pair pair[32];
 *       ybox_out out = { 0 };
 *       out.ev = ev; out.ev_cap = 32; out.pair = pair; out.pair_cap = 32;
 *       if (ybox_init(&dec, YBOX_LINE, YIN_KIND_SYNC, 1) < 0) return 1;
 *       // for each read of n bytes at host time t_read:
 *       size_t used = 0;
 *       while (used < n) {
 *           ybox_out_clear(&out);
 *           used += ybox_decode(&dec, bytes + used, n - used, t_read, &out);
 *           ... out.n_ev events (t = t_read, ticks = device time),
 *               out.n_pair clock pairs (ticks, probe)
 *       }
 *
 *   ---------------------------------------------------------------------
 *   EVENTS AND PAIRS
 *   ---------------------------------------------------------------------
 *   Each event is a yin_event with t = the host read time given to
 *   ybox_decode(), kind and device as given to ybox_init(), YIN_DEVTICKS
 *   set and ticks = the low 32 bits of the device's own time. The caller
 *   (ysp/device.h) replaces t with the mapped device time.
 *   Each frame that carries the device's time also gives a pair: the full
 *   device time and `probe`, 0 for a frame the device sent on its own and
 *   the query's number for an answer to a timer query (ybox_probe()). A
 *   frame the device sent on its own is late, never early, so its pair
 *   suits ysp/rt.h's LATE (or BRACKET with width 0) fit; an answer suits
 *   BRACKET with the query's write time.
 *   Bytes that fit no frame count in out.garbage and dec.garbage; the
 *   decoder finds the next frame by itself. ybox_decode() stops when an
 *   output array is full and returns the bytes it used; call it again for
 *   the rest. Arrays of one event and one pair suffice.
 *
 *   ---------------------------------------------------------------------
 *   CEDRUS XID (YBOX_XID)
 *   ---------------------------------------------------------------------
 *   From the device: 6-byte key frames 'k', info, time (uint32,
 *   little-endian, ms since the timer's last reset); info bits 0 to 3 the
 *   port (bits 2 and 3 must be 0: pyxid2 flushes on them), bit 4 pressed,
 *   bits 5 to 7 the key (0 means 8). Event: type PRESS or RELEASE, control
 *   the key (1 to 8), code the port, value 1 or 0. Timer answers: '_', 'e',
 *   '5', time (uint32, LE), 7 bytes. A StimTracker 2 sends 9-byte frames
 *   instead of 'k' frames: 'o', port, key (0 means 8), '1' pressed or '0'
 *   released, time (uint32, LE), 0 (pyxid2 '<ccBcIB'; Cedrus: "a
 *   terminating null byte"); the same event. ybox_probe() gives "_e5" (XID2
 *   firmware; XID1 has no query); ybox_identify() gives "_c1", whose answer
 *   "_xid0" (5 bytes, XID mode) ysp/device.h reads before decoding.
 *   pyxid2 writes every command one byte at a time with 1 ms between
 *   bytes; ysp/device.h does the same for queries (YBOX_SLOW_WRITE). The
 *   time unit is 1 ms (ybox_ns_per_tick()).
 *
 *   ---------------------------------------------------------------------
 *   THE YSP LINE PROTOCOL v1 (YBOX_LINE)
 *   ---------------------------------------------------------------------
 *   ASCII lines ending in '\n' ('\r' is ignored), at most 94 characters.
 *   <t> is the board's microsecond counter in decimal (up to 64 bits; a
 *   32-bit micros() wraps every 71.6 minutes and the host unwraps it).
 *   Board to host:
 *     S <t>                  a sync: the counter now. Send one every 100 ms,
 *                            so the host has pairs and knows the board lives
 *     E <t> <ch> <0|1>       an edge on channel ch (1 to 255): 1 on (pressed,
 *                            light), 0 off. Event PRESS or RELEASE,
 *                            control ch, value 1 or 0
 *     A <t> <ch> <value>     a sample (decimal, sign and fraction allowed):
 *                            event SAMPLE, control ch, value
 *     Q <seq> <t>            the answer to "q <seq>": the counter when the
 *                            query was read
 *     I <text>               the answer to "i": "ysp-line 1 <board> <name>"
 *     O <t> <code>           the board set its outputs to code at t (after
 *                            "o" or "p", and when a "p" pulse ends): a pair,
 *                            and out.has_out with the code and the time
 *     # <text>               a comment, ignored
 *   Host to board: "q <seq>\n" (seq 1 to 4294967295), "i\n", and since
 *   v0.2.0 "o <code>\n" (hold code on the outputs, bit k = output k) and
 *   "p <code> <us>\n" (code for us microseconds, timed by the board, then
 *   0). A board without outputs ignores "o" and "p".
 *   Anything else is garbage. firmware/ysp_line/ is the reference board.
 *
 *   ---------------------------------------------------------------------
 *   THE PHOTODIODE FRAME (YBOX_PHOTO)
 *   ---------------------------------------------------------------------
 *   6 bytes: 0xA5, level (0 dark, 1 light), us (uint32, LE) at each edge:
 *   event PRESS (light) or RELEASE (dark), control 1; 0x5A, 0, us every
 *   100 ms: a pair. No queries.
 *
 *   ---------------------------------------------------------------------
 *   OUTPUTS (ybox_encode, ybox_caps)
 *   ---------------------------------------------------------------------
 *   ybox_encode(family, op, code, width_ns, buf, cap) gives the bytes that
 *   put code on a device's output lines; 0 bytes for a family whose outputs
 *   are not bytes (LINES, whose code is bit 0 DTR and bit 1 RTS) or that
 *   cannot do the op; YBOX_ERR_ARG for a code above ybox_code_max(), a short
 *   buf, or a width the protocol cannot carry. width_ns is used by PULSE and
 *   WIDTH only; the wire unit is the protocol's (XID whole ms, LINE us).
 *     YBOX_OP_SET    hold code (0 is the idle code of every family)
 *     YBOX_OP_PULSE  code for width_ns, timed by the device (XID: "mp"
 *                    then "mh", rounded to the nearest ms, at least 1;
 *                    LINE: "p", rounded to the nearest us, at least 1,
 *                    at most 2^32 - 1 us); 0 bytes when the device cannot
 *                    time a pulse
 *     YBOX_OP_WIDTH  XID only: the pulse width of later SETs, rounded to
 *                    the nearest ms ("mp"; 0 = SET holds)
 *     YBOX_OP_CLOSE  the bytes to write before closing (TriggerBox 0xFF)
 *   ybox_caps(family) says what a family does (YBOX_CAP_*): IN (events),
 *   OUT (output codes), LINES (modem lines, no bytes), PULSE (the device
 *   times a pulse of any width), FIXED (every code is a pulse of
 *   ybox_fixed_pulse_ns(); SET cannot hold), IDENT (an identity query).
 *   A family without IN has a decoder that counts every byte as garbage
 *   (a TriggerBox sends a byte when its input lines change; not decoded).
 *   Bytes per family and their source:
 *     XID         SET "mh", lines low byte, high byte (16 lines); WIDTH "mp",
 *                 uint32 LE ms; PULSE both. Cedrus: "If the duration is 0
 *                 [default], then an mh command will set the output lines
 *                 and hold them"; pyxid2 pack('<ccI') for mp and 'mh' +
 *                 chr(lo) + chr(hi). XID 1 devices use "ah" (pyxid2): not
 *                 encoded
 *     LINE        SET "o <code>\n", PULSE "p <code> <us>\n" (8 outputs)
 *     TRIGGERBOX  SET one byte; CLOSE 0xFF. Brain Products, "TriggerBox
 *                 Programming Examples" Rev03: "Each byte written to the
 *                 COM port is transmitted to the eight output lines";
 *                 "the output line should be reset to their default levels
 *                 by writing a 0xFF"; port settings are ignored. The box
 *                 holds a byte, so a code repeated without a 0 between
 *                 gives no edge (Brain Products: "send 0 after every
 *                 trigger")
 *     BIOSEMI     one byte, held 8 ms, then all lines 0 (BioSemi, "USB
 *                 Trigger interface cable": "will hold this value for 8mS";
 *                 115200 baud, 8N1). The USB-C model can be set to 1 to
 *                 99 ms by a command not encoded here
 *     MMBTS       one byte at 9600 baud; switch P (factory): the box ends
 *                 each code after 8 ms; switch S: a byte holds. The switch
 *                 cannot be read. Opening at 1200 baud resets the box (an
 *                 Arduino bootloader). Source: chrplr/goxpyriment
 *                 triggers/notes/mmbts.md (written from the vendor's manual
 *                 v2.3) and ebadier/NeurospecTriggerBox-Unity (9600 baud,
 *                 one byte); the vendor's manual itself was not read.
 *                 ybox_caps() reports switch P
 *     PARALLEL    one byte, the data lines (ysp/parallel.h writes it)
 *   No family here has been checked on its device.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          ybox_init() returns int: 0, or YBOX_ERR_ARG for a NULL decoder
 *          or an unknown family (was bool). ybox_encode() takes the
 *          width as int64_t width_ns (was uint32_t arg in microseconds)
 *          and returns YBOX_ERR_ARG (-1, as before) for a bad argument,
 *          a negative width or a width the protocol cannot carry. New
 *          ybox_strerror(). Decoding and the bytes of every encoder
 *          unchanged.
 *   v0.2.0 - Outputs: ybox_encode() (set, pulse, width, close) and
 *          ybox_caps() for every family; five output families
 *          (TRIGGERBOX, BIOSEMI, MMBTS, LINES, PARALLEL); the line
 *          protocol's "o", "p" and "O" lines; XID's StimTracker 2 frame
 *          ('o', 9 bytes); ybox_code_max(), ybox_fixed_pulse_ns(),
 *          ybox_default_baud(). Input decoding of v0.1.0 unchanged.
 *   v0.1.0 - first version: XID key frames and timer answers, the ysp line
 *          protocol v1, the photodiode frame; queries for each.
 *
 *   STATUS: v0.2.0, 2026-10-08. Built with MSVC 19.44 (/W4 /WX, C11, C++17),
 *   MinGW-w64 gcc 16.1 and gcc 11.4 on WSL2 (C99, C11, C++17, -Werror).
 *   tests/adapt/box_test.c decodes generated byte streams for each family,
 *   split at every byte boundary, with garbage between frames, a full
 *   output, and a fuzz pass of random bytes against a counting model, and
 *   checks the bytes of every encoder. The XID frames, the timer answer and
 *   the output commands follow Cedrus's XID command page and pyxid2
 *   (cedrus-opensource/pyxid at d5b0a73, struct '<cBI', '<cccI', '<ccBcIB'
 *   and '<ccI'); the trigger boxes follow the sources cited at
 *   ybox_encode(). NO BYTE FROM OR TO A VENDOR DEVICE has been exchanged:
 *   none was attached. The line protocol's firmware (firmware/ysp_line/)
 *   compiles with arduino-cli 1.5.1 for the Teensy 4.1 and 4.0 and the
 *   Raspberry Pi Pico; no board has run it here. tests/mutate/box.toml
 *   lists the mutants.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_BOX_H_INCLUDED
#define YSP_BOX_H_INCLUDED

#define YBOX_VERSION_MAJOR 0
#define YBOX_VERSION_MINOR 3
#define YBOX_VERSION_PATCH 0
#define YBOX_VERSION_STRING "0.3.0"

#include "ysp/input.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The unit macros of ysp/rt.h (README.md, Conventions: Units), for widths:
 * ybox_encode(..., YRT_MS(2), ...). This header does not include ysp/rt.h,
 * so it defines the same block under the same guard. */
#ifndef YRT_S
#define YRT_NS(x)    ((int64_t)(x))
#define YRT_US(x)    ((int64_t)((x) * 1e3 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_MS(x)    ((int64_t)((x) * 1e6 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_S(x)     ((int64_t)((x) * 1e9 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_TO_MS(ns) ((double)(ns) / 1e6)
#define YRT_TO_S(ns)  ((double)(ns) / 1e9)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YBOX_API
#define YBOX_API extern
#endif

typedef enum ybox_family {
    YBOX_NONE       = 0,
    YBOX_XID        = 1,
    YBOX_LINE       = 2,
    YBOX_PHOTO      = 3,
    YBOX_TRIGGERBOX = 4,
    YBOX_BIOSEMI    = 5,
    YBOX_MMBTS      = 6,
    YBOX_LINES      = 7,
    YBOX_PARALLEL   = 8
} ybox_family;

#define YBOX_FAMILY_LAST YBOX_PARALLEL

/* ybox_caps() (OUTPUTS) */
#define YBOX_CAP_IN    0x01u   /* the decoder gives events                   */
#define YBOX_CAP_OUT   0x02u   /* takes output codes                         */
#define YBOX_CAP_LINES 0x04u   /* outputs are modem lines, not bytes         */
#define YBOX_CAP_PULSE 0x08u   /* the device times a pulse of any width      */
#define YBOX_CAP_FIXED 0x10u   /* every code is a pulse of a fixed width     */
#define YBOX_CAP_IDENT 0x20u   /* answers an identity query                  */

/* The negative result of ybox_init() and ybox_encode(). */
#define YBOX_ERR_ARG (-1)

/* ybox_encode() ops (OUTPUTS) */
#define YBOX_OP_SET   1
#define YBOX_OP_PULSE 2
#define YBOX_OP_WIDTH 3
#define YBOX_OP_CLOSE 4

/* One clock pair from a frame: the device's time and, for an answer to a
 * timer query, the query's number (0 = sent by the device on its own). */
typedef struct ybox_pair {
    uint64_t ticks;
    uint32_t probe;
    uint32_t reserved_;
} ybox_pair;

/* The caller's arrays; ybox_decode() appends to them. */
typedef struct ybox_out {
    yin_event* ev;
    int        ev_cap;
    int        n_ev;
    ybox_pair* pair;
    int        pair_cap;
    int        n_pair;
    uint32_t   garbage;      /* bytes that fit no frame, this call          */
    int        has_text;     /* an identity line arrived (LINE "I ...")     */
    char       text[96];     /* that line without the "I ", NUL-terminated  */
    int        has_out;      /* LINE "O": the board changed its outputs;
                              * ybox_decode() returns after one            */
    uint32_t   out_code;     /* the code it set                             */
    uint64_t   out_ticks;    /* its time on the board's clock               */
} ybox_out;

#define YBOX_LINE_MAX 96

typedef struct ybox_decoder {
    int      family;
    uint32_t device;         /* yin_event.device of every event            */
    uint8_t  kind;           /* yin_event.kind                             */
    uint8_t  skip;           /* LINE: dropping an overlong line            */
    uint16_t n;              /* bytes in buf                               */
    uint8_t  buf[YBOX_LINE_MAX];
    uint64_t frames;         /* frames decoded since init                  */
    uint64_t garbage;        /* bytes that fit no frame since init         */
} ybox_decoder;

/* XID commands are written one byte at a time with 1 ms between bytes, as
 * pyxid2 does; ybox_slow_write() says which family needs it. */
#define YBOX_SLOW_WRITE_NS 1000000

YBOX_API const char* ybox_version(void);
YBOX_API const char* ybox_family_name(int family);

/* Static name of a YBOX_ERR_* code ("ok" for values >= 0). */
YBOX_API const char* ybox_strerror(int code);

/* An init (no desc, no close): 0, or YBOX_ERR_ARG for a NULL d or an
 * unknown family. */
YBOX_API int    ybox_init(ybox_decoder* d, int family, uint8_t kind, uint32_t device);

/* Decode up to n bytes read at host time t_read; returns the bytes used. */
YBOX_API size_t ybox_decode(ybox_decoder* d, const uint8_t* bytes, size_t n, int64_t t_read, ybox_out* out);

/* Empty the counts of out (not its arrays). */
YBOX_API void   ybox_out_clear(ybox_out* out);

/* The bytes of a timer query (seq numbers the answer for LINE; XID answers
 * carry no number), or 0 when the family has none or cap is too small. */
YBOX_API int    ybox_probe(int family, uint32_t seq, uint8_t* buf, int cap);

/* The bytes of an identity query, or 0. */
YBOX_API int    ybox_identify(int family, uint8_t* buf, int cap);

/* The device clock's nominal tick in ns (1e6 for XID, 1e3 otherwise), and
 * whether commands must be written one byte at a time. */
YBOX_API double ybox_ns_per_tick(int family);
YBOX_API bool   ybox_slow_write(int family);

/* What the family does: YBOX_CAP_* bits; 0 for an unknown family. */
YBOX_API uint32_t ybox_caps(int family);

/* The highest output code (65535 XID, 3 LINES, 255 the others; 0 without
 * outputs), the width of a FIXED family's pulses in ns (0 otherwise), and
 * the baud rate the device expects (9600 MMBT-S, 115200 otherwise). */
YBOX_API uint32_t ybox_code_max(int family);
YBOX_API int64_t  ybox_fixed_pulse_ns(int family);
YBOX_API uint32_t ybox_default_baud(int family);

/* The bytes of an output op (OUTPUTS): their count, 0 when the family has
 * no bytes for it, YBOX_ERR_ARG for a code above ybox_code_max(), a short
 * buf, or a width (ns; PULSE and WIDTH only) the protocol cannot carry. */
YBOX_API int    ybox_encode(int family, int op, uint32_t code, int64_t width_ns, uint8_t* buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* YSP_BOX_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_BOX_IMPLEMENTATION
#ifndef YSP_BOX_IMPLEMENTATION_GUARD
#define YSP_BOX_IMPLEMENTATION_GUARD

#include <string.h>
#include <stdlib.h>   /* strtod: LINE samples */
#include <stdio.h>    /* snprintf: queries */

YBOX_API const char* ybox_version(void) { return YBOX_VERSION_STRING; }

YBOX_API const char* ybox_family_name(int family) {
    switch (family) {
    case YBOX_XID:   return "xid";
    case YBOX_LINE:  return "line";
    case YBOX_PHOTO: return "photo";
    case YBOX_TRIGGERBOX: return "triggerbox";
    case YBOX_BIOSEMI:    return "biosemi";
    case YBOX_MMBTS:      return "mmbts";
    case YBOX_LINES:      return "lines";
    case YBOX_PARALLEL:   return "parallel";
    default:         return "none";
    }
}

YBOX_API const char* ybox_strerror(int code) {
    if (code >= 0) return "ok";
    return code == YBOX_ERR_ARG ? "bad argument" : "unknown error";
}

YBOX_API int ybox_init(ybox_decoder* d, int family, uint8_t kind, uint32_t device) {
    if (!d || family < YBOX_XID || family > YBOX_FAMILY_LAST) return YBOX_ERR_ARG;
    memset(d, 0, sizeof *d);
    d->family = family;
    d->kind = kind;
    d->device = device;
    return 0;
}

YBOX_API void ybox_out_clear(ybox_out* out) {
    if (!out) return;
    out->n_ev = 0;
    out->n_pair = 0;
    out->garbage = 0;
    out->has_text = 0;
    out->text[0] = '\0';
    out->has_out = 0;
    out->out_code = 0;
    out->out_ticks = 0;
}

YBOX_API double ybox_ns_per_tick(int family) { return family == YBOX_XID ? 1e6 : 1e3; }
YBOX_API bool   ybox_slow_write(int family) { return family == YBOX_XID; }

YBOX_API uint32_t ybox_caps(int family) {
    switch (family) {
    case YBOX_XID:        return YBOX_CAP_IN | YBOX_CAP_OUT | YBOX_CAP_PULSE | YBOX_CAP_IDENT;
    case YBOX_LINE:       return YBOX_CAP_IN | YBOX_CAP_OUT | YBOX_CAP_PULSE | YBOX_CAP_IDENT;
    case YBOX_PHOTO:      return YBOX_CAP_IN;
    case YBOX_TRIGGERBOX: return YBOX_CAP_OUT;
    case YBOX_BIOSEMI:    return YBOX_CAP_OUT | YBOX_CAP_FIXED;
    case YBOX_MMBTS:      return YBOX_CAP_OUT | YBOX_CAP_FIXED;
    case YBOX_LINES:      return YBOX_CAP_OUT | YBOX_CAP_LINES;
    case YBOX_PARALLEL:   return YBOX_CAP_OUT;
    default:              return 0;
    }
}

YBOX_API uint32_t ybox_code_max(int family) {
    if (!(ybox_caps(family) & YBOX_CAP_OUT)) return 0;
    if (family == YBOX_XID) return 0xFFFFu;
    if (family == YBOX_LINES) return 3u;
    return 0xFFu;
}

YBOX_API int64_t ybox_fixed_pulse_ns(int family) {
    return (ybox_caps(family) & YBOX_CAP_FIXED) ? 8000000 : 0;
}

YBOX_API uint32_t ybox_default_baud(int family) {
    /* MMBT-S: fixed in its firmware; 1200 would reset it into its bootloader */
    return family == YBOX_MMBTS ? 9600u : 115200u;
}

static void ybox__put_le32(uint8_t* b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

YBOX_API int ybox_encode(int family, int op, uint32_t code, int64_t width_ns, uint8_t* buf, int cap) {
    uint32_t caps = ybox_caps(family);
    if (!buf || cap < 0 || !(caps & YBOX_CAP_OUT)) return YBOX_ERR_ARG;
    if (op == YBOX_OP_CLOSE) {
        if (family != YBOX_TRIGGERBOX) return 0;
        if (cap < 1) return YBOX_ERR_ARG;
        buf[0] = 0xFF;            /* the box's default levels */
        return 1;
    }
    if (op != YBOX_OP_SET && op != YBOX_OP_PULSE && op != YBOX_OP_WIDTH) return YBOX_ERR_ARG;
    if (code > ybox_code_max(family)) return YBOX_ERR_ARG;
    /* a width the wire cannot carry (both protocols send 32 bits) is the
     * caller's error, not a silent clamp */
    if (op != YBOX_OP_SET && (width_ns < 0 || width_ns > (int64_t)0xFFFFFFFF * 1000)) return YBOX_ERR_ARG;
    if (caps & YBOX_CAP_LINES) return 0;
    if (family == YBOX_XID) {
        /* mp takes ms; a pulse rounds to the nearest ms, at least 1, since
         * 0 means hold */
        uint32_t ms = (uint32_t)((width_ns + 500000) / 1000000);
        int n = 0;
        if (op == YBOX_OP_PULSE && ms == 0) ms = 1;
        if (op != YBOX_OP_SET) {
            if (cap < 6) return YBOX_ERR_ARG;
            buf[0] = 'm'; buf[1] = 'p';
            ybox__put_le32(buf + 2, ms);
            n = 6;
            if (op == YBOX_OP_WIDTH) return n;
        }
        if (cap < n + 4) return YBOX_ERR_ARG;
        buf[n] = 'm'; buf[n + 1] = 'h';
        buf[n + 2] = (uint8_t)(code & 0xFFu);
        buf[n + 3] = (uint8_t)(code >> 8);
        return n + 4;
    }
    if (op == YBOX_OP_WIDTH) return 0;
    if (family == YBOX_LINE) {
        char tmp[40];
        int n;
        if (op == YBOX_OP_PULSE) {
            /* the board takes whole us (its micros() clock) */
            int64_t us = (width_ns + 500) / 1000;
            if (us == 0) return YBOX_ERR_ARG;
            n = snprintf(tmp, sizeof tmp, "p %lu %lu\n", (unsigned long)code, (unsigned long)us);
        } else {
            n = snprintf(tmp, sizeof tmp, "o %lu\n", (unsigned long)code);
        }
        if (n <= 0 || n > cap) return YBOX_ERR_ARG;
        memcpy(buf, tmp, (size_t)n);
        return n;
    }
    /* one byte: TriggerBox, BioSemi, MMBT-S, the parallel port. None of
     * them times a pulse of a chosen width. */
    if (op == YBOX_OP_PULSE) return 0;
    if (cap < 1) return YBOX_ERR_ARG;
    buf[0] = (uint8_t)code;
    return 1;
}

YBOX_API int ybox_probe(int family, uint32_t seq, uint8_t* buf, int cap) {
    if (!buf) return 0;
    if (family == YBOX_XID) {
        if (cap < 3) return 0;
        memcpy(buf, "_e5", 3);
        return 3;
    }
    if (family == YBOX_LINE) {
        char tmp[24];
        int n = snprintf(tmp, sizeof tmp, "q %lu\n", (unsigned long)seq);
        if (seq == 0 || n <= 0 || n > cap) return 0;
        memcpy(buf, tmp, (size_t)n);
        return n;
    }
    return 0;
}

YBOX_API int ybox_identify(int family, uint8_t* buf, int cap) {
    if (!buf) return 0;
    if (family == YBOX_XID && cap >= 3) { memcpy(buf, "_c1", 3); return 3; }
    if (family == YBOX_LINE && cap >= 2) { memcpy(buf, "i\n", 2); return 2; }
    return 0;
}

static uint32_t ybox__le32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static yin_event* ybox__event(ybox_decoder* d, ybox_out* out, int64_t t, uint64_t ticks, uint8_t type,
                              uint32_t control) {
    yin_event* e = &out->ev[out->n_ev++];
    memset(e, 0, sizeof *e);
    e->t = t;
    e->device = d->device;
    e->kind = d->kind;
    e->type = type;
    e->control = control;
    e->flags = (uint8_t)YIN_DEVTICKS;
    e->ticks = (uint32_t)ticks;
    return e;
}

static void ybox__pair(ybox_out* out, uint64_t ticks, uint32_t probe) {
    ybox_pair* p = &out->pair[out->n_pair++];
    p->ticks = ticks;
    p->probe = probe;
    p->reserved_ = 0;
}

/* --- XID and the photodiode frame: fixed frames, a byte at a time --------- */

/* A frame starts with one of these bytes; anything else is garbage. */
static bool ybox__fixed_start(int family, uint8_t b) {
    if (family == YBOX_XID) return b == 'k' || b == '_' || b == 'o';
    return b == 0xA5 || b == 0x5A;
}

/* Called with the frame's next byte appended. 1: a frame was decoded and
 * buf emptied; 0: incomplete; -1: buf[0] starts no valid frame. */
static int ybox__fixed_step(ybox_decoder* d, int64_t t, ybox_out* out) {
    const uint8_t* b = d->buf;
    if (d->family == YBOX_XID) {
        if (b[0] == 'k') {
            uint8_t info;
            uint32_t key;
            if (d->n < 6) return (d->n >= 2 && (b[1] & 0x0Cu) != 0) ? -1 : 0;
            info = b[1];
            key = (info >> 5) & 7u;
            {
                uint32_t ms = ybox__le32(b + 2);
                yin_event* e = ybox__event(d, out, t, ms, (info & 0x10u) ? (uint8_t)YIN_PRESS : (uint8_t)YIN_RELEASE,
                                           key == 0 ? 8u : key);
                e->code = info & 0x0Fu;
                e->value = (info & 0x10u) ? 1.0f : 0.0f;
                ybox__pair(out, ms, 0);
            }
            return 1;
        }
        if (b[0] == 'o') {
            /* StimTracker 2: 'o', port, key, '1' or '0', ms (LE), 0 */
            if (d->n >= 4 && b[3] != '1' && b[3] != '0') return -1;
            if (d->n < 9) return 0;
            if (b[8] != 0u) return -1;
            {
                uint32_t ms = ybox__le32(b + 4);
                uint32_t key = b[2];
                int pressed = b[3] == '1';
                yin_event* e = ybox__event(d, out, t, ms, pressed ? (uint8_t)YIN_PRESS : (uint8_t)YIN_RELEASE,
                                           key == 0 ? 8u : key);
                e->code = b[1];
                e->value = pressed ? 1.0f : 0.0f;
                ybox__pair(out, ms, 0);
            }
            return 1;
        }
        /* '_': only the timer answer "_e5" is decoded */
        if (d->n >= 2 && b[1] != 'e') return -1;
        if (d->n >= 3 && b[2] != '5') return -1;
        if (d->n < 7) return 0;
        ybox__pair(out, ybox__le32(b + 3), 1);
        return 1;
    }
    /* the photodiode frame */
    if (d->n >= 2 && b[1] > 1u) return -1;
    if (d->n >= 2 && b[0] == 0x5A && b[1] != 0u) return -1;
    if (d->n < 6) return 0;
    {
        uint32_t us = ybox__le32(b + 2);
        if (b[0] == 0xA5) {
            yin_event* e = ybox__event(d, out, t, us, b[1] ? (uint8_t)YIN_PRESS : (uint8_t)YIN_RELEASE, 1u);
            e->value = b[1] ? 1.0f : 0.0f;
        }
        ybox__pair(out, us, 0);
    }
    return 1;
}

/* Feeds one byte; on a bad start, drops buf[0] and feeds the rest again,
 * so a frame that starts inside a broken one is still found. */
static void ybox__fixed_byte(ybox_decoder* d, uint8_t byte, int64_t t, ybox_out* out) {
    uint8_t pend[16];   /* a broken frame (up to 9 bytes) and the rest */
    int np = 1, i = 0;
    pend[0] = byte;
    while (i < np) {
        uint8_t b = pend[i++];
        int r;
        if (d->n == 0) {
            if (!ybox__fixed_start(d->family, b)) { d->garbage++; out->garbage++; continue; }
        }
        d->buf[d->n++] = b;
        r = ybox__fixed_step(d, t, out);
        if (r == 1) { d->n = 0; d->frames++; }
        else if (r < 0) {
            /* buf[1..n) and the bytes still pending go round again */
            int k, m = d->n - 1;
            uint8_t rest[16];
            for (k = 0; k < m; k++) rest[k] = d->buf[1 + k];
            for (k = i; k < np; k++) rest[m++] = pend[k];
            d->n = 0;
            d->garbage++;
            out->garbage++;
            memcpy(pend, rest, (size_t)m);
            np = m;
            i = 0;
        }
    }
}

/* --- the line protocol ------------------------------------------------------ */

/* Parses an unsigned decimal at *p, skipping spaces before it. */
static bool ybox__u64(const char** p, uint64_t* out) {
    const char* s = *p;
    uint64_t v = 0;
    int digits = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') {
        uint64_t dgt = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - dgt) / 10u) return false;
        v = v * 10u + dgt;
        s++;
        digits++;
    }
    if (!digits || (*s != ' ' && *s != '\0')) return false;
    *p = s;
    *out = v;
    return true;
}

static bool ybox__end(const char* p) {
    while (*p == ' ') p++;
    return *p == '\0';
}

static bool ybox__line(ybox_decoder* d, char* s, int64_t t, ybox_out* out) {
    const char* p = s + 1;
    uint64_t a, b, c;
    if (s[0] == '#' || s[0] == '\0') return true;
    if (s[1] != ' ' && !(s[0] == 'I' && s[1] == '\0')) return false;
    switch (s[0]) {
    case 'S':
        if (!ybox__u64(&p, &a) || !ybox__end(p)) return false;
        ybox__pair(out, a, 0);
        return true;
    case 'Q':
        if (!ybox__u64(&p, &a) || !ybox__u64(&p, &b) || !ybox__end(p) || a == 0 || a > 0xFFFFFFFFu) return false;
        ybox__pair(out, b, (uint32_t)a);
        return true;
    case 'O':
        if (!ybox__u64(&p, &a) || !ybox__u64(&p, &b) || !ybox__end(p) || b > 0xFFFFFFFFu) return false;
        ybox__pair(out, a, 0);
        out->has_out = 1;
        out->out_code = (uint32_t)b;
        out->out_ticks = a;
        return true;
    case 'E': {
        yin_event* e;
        if (!ybox__u64(&p, &a) || !ybox__u64(&p, &b) || !ybox__u64(&p, &c) || !ybox__end(p)) return false;
        if (b < 1 || b > 255 || c > 1) return false;
        e = ybox__event(d, out, t, a, c ? (uint8_t)YIN_PRESS : (uint8_t)YIN_RELEASE, (uint32_t)b);
        e->value = (float)c;
        ybox__pair(out, a, 0);
        return true;
    }
    case 'A': {
        yin_event* e;
        char* end;
        double v;
        if (!ybox__u64(&p, &a) || !ybox__u64(&p, &b) || b < 1 || b > 255) return false;
        while (*p == ' ') p++;
        if (*p == '\0') return false;
        v = strtod(p, &end);
        if (end == p || !ybox__end(end)) return false;
        e = ybox__event(d, out, t, a, (uint8_t)YIN_SAMPLE, (uint32_t)b);
        e->value = (float)v;
        ybox__pair(out, a, 0);
        return true;
    }
    case 'I': {
        size_t n = strlen(s + (s[1] ? 2 : 1));
        if (n >= sizeof out->text) n = sizeof out->text - 1;
        memcpy(out->text, s + (s[1] ? 2 : 1), n);
        out->text[n] = '\0';
        out->has_text = 1;
        return true;
    }
    default:
        return false;
    }
}

YBOX_API size_t ybox_decode(ybox_decoder* d, const uint8_t* bytes, size_t n, int64_t t_read, ybox_out* out) {
    size_t i;
    if (!d || !out || (!bytes && n)) return 0;
    for (i = 0; i < n; i++) {
        /* A byte finishes at most one frame: a broken frame shows by its
         * ninth byte at the latest (a StimTracker 2 frame), and the eight
         * bytes fed again hold at most one frame (6 bytes or more). */
        if (out->n_ev >= out->ev_cap || out->n_pair >= out->pair_cap || out->has_out) break;
        if (!(ybox_caps(d->family) & YBOX_CAP_IN)) {
            /* an output-only family: nothing to decode */
            d->garbage++;
            out->garbage++;
            continue;
        }
        if (d->family != YBOX_LINE) {
            ybox__fixed_byte(d, bytes[i], t_read, out);
            continue;
        }
        if (bytes[i] == '\r') continue;
        if (bytes[i] == '\n') {
            if (d->skip) { d->skip = 0; d->n = 0; d->garbage++; out->garbage++; continue; }
            d->buf[d->n] = '\0';
            if (ybox__line(d, (char*)d->buf, t_read, out)) {
                if (d->n) d->frames++;
            } else {
                d->garbage += (uint64_t)d->n + 1u;
                out->garbage += (uint32_t)d->n + 1u;
            }
            d->n = 0;
            continue;
        }
        if (d->skip) { d->garbage++; out->garbage++; continue; }
        if (d->n + 1u >= sizeof d->buf || bytes[i] < 0x20u || bytes[i] > 0x7Eu) {
            /* too long, or not text: drop the line */
            d->garbage += (uint64_t)d->n + 1u;
            out->garbage += (uint32_t)d->n + 1u;
            d->n = 0;
            d->skip = 1;
            continue;
        }
        d->buf[d->n++] = bytes[i];
    }
    return i;
}

#endif /* YSP_BOX_IMPLEMENTATION_GUARD */
#endif /* YSP_BOX_IMPLEMENTATION */

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
