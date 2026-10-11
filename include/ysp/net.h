/* ysp/net.h - v0.3.0 - public domain single-header Lab Streaming Layer glue
 *
 *   Lab Streaming Layer (LSL) on the ysp_rt clock: marker outlets for a
 *   rig's event codes and text, numeric outlets, and inlets that resolve a
 *   stream by its match key, pull it on their own reader thread, map the
 *   sender's stamps to the ysp_rt clock (a fit of lsl_local_clock() and a
 *   fit of liblsl's time corrections), and deliver marker samples and
 *   threshold crossings as ysp/input.h events (the device layer's sink) and
 *   every numeric sample into a stream ring with a newest-sample slot.
 *   Lifecycle, gaps and every output are records in a ysp/rt.h ring.
 *   liblsl (MIT) is loaded at run time; nothing links it.
 *   docs/devices_spec.md (step 4 of 14.2) is the plan; docs/net.md has the
 *   decisions and the results.
 *
 *   REQUIRES ysp/rt.h and ysp/input.h beside it; this header includes them,
 *   and its implementation implements ysp/rt.h unless the translation unit
 *   already does. Windows, Linux and macOS. C99 is the floor: it builds as
 *   C99, C11 and C++17. On Linux, link -ldl for dlopen (glibc 2.34 and
 *   later have it in libc).
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_NET_IMPLEMENTATION
 *       #include "ysp/net.h"
 *
 *       static ynet_lsl lib;
 *       if (!ynet_lsl_open(&lib, NULL)) die(ynet_lsl_error(&lib));
 *
 *       static ynet_outlet marks;                       // an outlet
 *       ynet_outlet_desc o = { 0 };
 *       o.lib = &lib;
 *       o.role = "marks";
 *       o.key = "lsl:ysp-markers:Markers:booth2";       // name:type:source_id
 *       o.device = 3;
 *       o.ring = &log_ring;
 *       if (!ynet_outlet_open(&marks, &o)) die(ynet_outlet_error(&marks));
 *       int rc = ynet_out_mark(&marks, 12, "target_on");
 *       if (rc < 0) fprintf(stderr, "marker: %s\n", ynet_strerror(rc));
 *
 *       static ynet_inlet ref;                          // an inlet, ~190 KB
 *       ynet_inlet_desc d = { 0 };
 *       d.lib = &lib;
 *       d.role = "ref";
 *       d.key = "lsl:Data::";                           // any stream named Data
 *       d.device = 4;
 *       d.sink = to_bridge;                             // yscr_push_input()
 *       d.stream = &ring;                               // numeric samples
 *       d.ring = &log_ring;
 *       if (!ynet_inlet_open(&ref, &d)) die(ynet_inlet_error(&ref));
 *       // open returns while the inlet resolves its key (OPENING)
 *       if (ynet_inlet_wait_running(&ref, YRT_S(5)) < 0) die(ynet_inlet_error(&ref));
 *       ...
 *       ynet_inlet_close(&ref);
 *       ynet_outlet_close(&marks);
 *       ynet_lsl_close(&lib);
 *
 *   ---------------------------------------------------------------------
 *   THE LIBRARY (ynet_lsl)
 *   ---------------------------------------------------------------------
 *   ynet_lsl_open(lib, path) opens liblsl and finds the functions this
 *   header uses (their declarations follow liblsl's lsl_c.h; the struct
 *   names them without "lsl_"). path NULL tries $YSP_LSL_PATH, then
 *   lsl.dll and liblsl.dll (Windows), liblsl.so, .so.2, .so.1 (Linux),
 *   liblsl.dylib, liblsl.2.dylib (macOS). Returns true, or false with the
 *   reason in ynet_lsl_error(lib): no library (it says where it looked), or
 *   a function is missing (an old liblsl). lib->version is
 *   lsl_library_version() (116 is 1.16). A caller may also fill the table
 *   itself (a test's fake) and leave handle NULL. Open once, before any
 *   instance opens; ynet_lsl_close() after every instance closed. The
 *   close is safe on a zeroed table, after a failed open, and twice.
 *
 *   ---------------------------------------------------------------------
 *   MATCH KEYS (desc.key)
 *   ---------------------------------------------------------------------
 *     lsl:<name>:<type>:<source_id>:<hostname>
 *   LSL's stream properties. An inlet's key: empty or missing fields match
 *   anything, at least one field must be set, and no field may hold a
 *   quote ('), a control character or a colon. More than one stream that
 *   matches is no match: the inlet stays OPENING and logs "ambiguous"
 *   (add the source_id or the hostname). An outlet's key gives the name
 *   (required), the type and the source_id of the stream it makes.
 *   ynet_resolve_all() lists the streams on the network with their keys.
 *   An outlet and an inlet copy desc.key and desc.role into their handle
 *   (an inlet resolves its key again after every loss): a key has at most
 *   YNET_KEY_MAX - 1 (239) characters, a role YNET_ROLE_MAX - 1 (31); a
 *   longer one fails the open with a message. No allocation.
 *
 *   ---------------------------------------------------------------------
 *   OUTLETS
 *   ---------------------------------------------------------------------
 *   ynet_outlet_open() makes the stream (desc.format YNET_STRING, the
 *   default, YNET_INT32 or YNET_FLOAT32; desc.channels, 1 by default;
 *   desc.rate_hz, 0 = irregular). Calls from any thread:
 *     ynet_out_mark(o, code, text)          one marker, stamped now
 *     ynet_out_mark_at(o, code, text, t)    one marker stamped with the
 *                                           ysp_rt time t (past or future)
 *     ynet_out_push(o, x, n, lsl_s)         n numeric samples (float32), with
 *                                           their LSL stamps (NULL: now)
 *   A string outlet sends text, or the code in decimal when text is NULL or
 *   empty (or desc.code_names[code] when the caller gave names); an int32
 *   outlet sends the code, and the text goes to the log only. The LSL
 *   stamp is lsl_local_clock() read inside a ysp_rt bracket just before
 *   the push (each push's bracket is a pair of the outlet's local fit), or
 *   for _at the ysp_rt time mapped back through that fit. Each mark is one
 *   YNET_REC_OUT record: i64[0] the ysp_rt time before the push, i64[1]
 *   after, i64[2] the LSL stamp in ns (on the LSL clock), i64[3] the
 *   target time of _at or a trigger (else 0), u32[8] the code, u16[18]
 *   YNET_OUT_* flags, u16[19] the mark's number; with text, TEXT records
 *   "mark ..." follow (34 characters each). Numeric pushes are counted,
 *   not recorded. Each call returns YNET_OK or a YNET_ERR_* and never
 *   writes ynet_outlet_error(): several threads may call them at once, and
 *   the message would race (README.md, Conventions, Errors).
 *   ynet_strerror() names the code; ynet_out_last() gives the last call.
 *
 *   ---------------------------------------------------------------------
 *   INLETS AND THEIR LIFECYCLE
 *   ---------------------------------------------------------------------
 *   The states and their numbers are ysp/device.h's:
 *     OPENING  resolving the key every retry_ns (1 s), each resolve
 *              waiting resolve_ns (500 ms)
 *     RUNNING  subscribed and pulling
 *     LOST     liblsl said lost, or a regular stream sent nothing for
 *              silent_ns (1 s; an irregular stream never goes silent);
 *              the inlet is destroyed and the key resolved again
 *     FAILED   the stream at the key has another format or channel count
 *              than desc.format and desc.channels: a wrong stream. No retry
 *     CLOSED   closed
 *   ynet_inlet_open() returns while the inlet is OPENING.
 *   ynet_inlet_wait_running(in, timeout_ns) waits until it is RUNNING: it
 *   returns YNET_RUNNING, YNET_ERR_TIMEOUT, or YNET_ERR_STATE when the
 *   inlet ends FAILED (a wrong stream) or is CLOSED, with the reason in
 *   ynet_inlet_error(). In manual mode it calls ynet_inlet_poll() itself.
 *   liblsl's own recovery is off, so a loss is seen and logged. Each open
 *   restarts the remote fit (an outlet that comes back may run on another
 *   clock); a GAP record gives the last arrival before the loss and the
 *   time it ran again.
 *
 *   ---------------------------------------------------------------------
 *   TIME
 *   ---------------------------------------------------------------------
 *   Two fits of ysp/rt.h (DEVICE CLOCK FIT), 1 ns ticks:
 *     local   BRACKET: lsl_local_clock() read between two ysp_rt reads, the
 *             tightest of 8, every second: the LSL clock of this machine on
 *             the ysp_rt clock (clock_id 0x10000 + device)
 *     remote  UNBIASED: each new lsl_time_correction_ex() (asked every
 *             correct_ns, 1 s): the sender's time, paired with the local
 *             map of sender time plus offset; corrections whose round trip
 *             is over max_unc_ns (5 ms) refused (clock_id = device).
 *             liblsl's offset is the midpoint of its best probe, off either
 *             way by the path's asymmetry (at most half the round trip), so
 *             the pairs scatter both ways: least squares, not a lower
 *             envelope (docs/net.md has the comparison)
 *   A sample's t is the remote fit's map of its stamp; before the remote
 *   fit has a pair, the local map of stamp plus the newest offset; before
 *   any correction, the read time with unc_us 65535. t is never later than
 *   the read time (a map past it is clamped and counted). unc_us is half
 *   the newest round trip or the remote fit's spread, whichever is larger,
 *   plus the local fit's widest bracket: a bound from the fits' own
 *   evidence, not a measurement. ticks holds the stamp in us (low 32
 *   bits) with YIN_DEVTICKS. ynet_inlet_map() maps any stamp of the
 *   stream from any thread. Each correction is a YNET_REC_OFFSET record:
 *   i64[0] the sender's time in ns, i64[1] the offset in ns, i64[2] the
 *   round trip in ns, i64[3] the pair's ysp_rt time. ynet_inlet_source()
 *   is tier UNKNOWN, stamp source SDK: no loopback has checked an LSL
 *   source.
 *
 *   ---------------------------------------------------------------------
 *   EVENTS AND THE STREAM RING
 *   ---------------------------------------------------------------------
 *   String and int32 streams: one event per sample on desc.sink: kind
 *   desc.kind (0 = YIN_KIND_SYNC), type PRESS, value 1, code the int32
 *   value, or a string's decimal value (0 when it is not a number in
 *   0..4294967295); aux the sample's number. A string also goes to
 *   desc.text_sink with its event, and to TEXT records "in ..." (34
 *   characters each).
 *   Float32 streams: every sample to desc.stream (a ynet_stream in caller
 *   memory); desc.edges[] turn threshold crossings of a channel into
 *   PRESS (rising past level + hysteresis / 2) and RELEASE (falling past
 *   level - hysteresis / 2) events, control the edge's control (0 = the
 *   channel + 1), value the sample. A step between stamps above 1.5
 *   periods of a regular stream is a YNET_REC_STREAM_GAP record (i64[0]
 *   the stamp before, i64[1] the stamp after, in LSL ns; u64[2] the
 *   samples missing). Sinks run on the reader thread: quick and
 *   thread-safe, as yscr_push_input() is.
 *   The stream ring has one producer (the reader) and one consumer
 *   (ynet_stream_read()); a full ring drops the new sample, counts it, and
 *   writes a YNET_REC_DROP record at most once a second. Each record: the
 *   ysp_rt time, the LSL stamp, the channels. ynet_stream_newest() gives
 *   the newest sample to any thread (a gaze-contingent display), updated
 *   once per pulled chunk; the reader never waits for it.
 *
 *   ---------------------------------------------------------------------
 *   RECORDS (desc.ring)
 *   ---------------------------------------------------------------------
 *   Source YRT_SRC_NET, aux = desc.device. Kinds 1, 2, 3 and 5 have the
 *   layouts of ysp/device.h's YDEV_REC_STATE, _GAP, _TEXT and _OUT (OUT
 *   as above); 32 and up are this header's: OFFSET, STREAM_GAP, STREAM (at
 *   open: u32[0] format, u32[1] channels, f64[1] the nominal rate; TEXT
 *   records "name", "type", "source", "host", "uid" follow), DROP (u64[0]
 *   samples dropped in all). The fits' CLOCK and FIT records go to the
 *   same ring (YRT_SRC_RT).
 *
 *   ---------------------------------------------------------------------
 *   THREADS AND MANUAL MODE
 *   ---------------------------------------------------------------------
 *   ynet_inlet_open() starts one reader thread, elevated with
 *   yrt_thread_elevate() unless desc.no_elevate. Its pulls wait at most
 *   read_timeout_ns (10 ms). ynet_inlet_close() joins it; that can take a
 *   resolve's wait. desc.manual: no thread; call ynet_inlet_poll() from
 *   your loop. desc.now replaces yrt_now_ns() (tests). Nothing allocates
 *   after the open except inside liblsl; liblsl allocates each string
 *   sample and the header frees it with lsl_destroy_string().
 *   desc.lsl_max_buflen is liblsl's max_buflen, passed as given: seconds
 *   of data (or hundreds of samples for an irregular stream) that liblsl
 *   keeps for the inlet, 0 = 30. It is an LSL protocol value, like the
 *   lsl_ stamps (double seconds on liblsl's clock); every other time is
 *   int64_t ns. ynet_stats.lsl_offset_ns and lsl_rtt_ns are liblsl's newest
 *   time correction, rounded to the nearest ns: a difference between two
 *   LSL clocks and a round trip, not ysp_rt times.
 *
 *   ---------------------------------------------------------------------
 *   GLUE
 *   ---------------------------------------------------------------------
 *   With ysp/screen.h included first: ynet_trigger_channel(o, offset)
 *   gives a trigger channel whose callback sends a marker at the planned
 *   vblank, stamped with that deadline (flag FLIP). With ysp/device.h
 *   included first: ynet_role(roles, name) gives a role's index in a
 *   ydev_roles table (0 when it is full, the name is too long, or a
 *   running ysp/device.h instance has the role).
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - ynet_stats.lsl_offset_s and lsl_rtt_s (double seconds) are
 *          lsl_offset_ns and lsl_rtt_ns (int64_t, rounded to the nearest
 *          ns at the liblsl call): every time in the C API is int64_t ns
 *          (README.md, Conventions). The YNET_REC_OFFSET record's offset
 *          is the same rounded value (it was truncated). Breaks the API;
 *          the struct's size and layout do not change.
 *   v0.2.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          ynet_lsl_load() and ynet_lsl_unload() are bool ynet_lsl_open()
 *          and void ynet_lsl_close(), with ynet_lsl_error() (do not read
 *          lib.error). ynet_outlet_start/stop() and ynet_inlet_start/stop()
 *          are ynet_outlet_open/close() and ynet_inlet_open/close(). Times
 *          are int64_t ns: ynet_resolve_all(..., wait_ns),
 *          ynet_inlet_poll(in, timeout_ns), desc.read_timeout_ns (was
 *          read_timeout_ms). desc.buffer_s is desc.lsl_max_buflen, and
 *          ynet_stats.offset_s and rtt_s are lsl_offset_s and lsl_rtt_s:
 *          liblsl's own values, named so. ynet_stream_init() returns 0 or
 *          YNET_ERR_ARG. New ynet_inlet_wait_running() and
 *          YNET_ERR_TIMEOUT. The outlet and the inlet copy desc.key and
 *          desc.role into the handle (YNET_KEY_MAX, YNET_ROLE_MAX): the
 *          reader thread read the caller's key again at every reconnect.
 *   v0.1.0 - first version: the run-time loader, string, int32 and float32
 *          outlets, inlets with the lifecycle, two clock fits, marker and
 *          edge events, the stream ring; the ysp/screen.h trigger channel
 *          and the ysp/device.h role glue.
 *
 *   STATUS: v0.1.0, 2026-10-09. Built with MSVC 19.44 (/W4 /WX, C11 and
 *   C++17), MinGW-w64 gcc 16.1 and gcc 11.4 on WSL2 (C99, C11, C++17,
 *   -Werror). tests/adapt/net_test.c runs every path against a fake liblsl
 *   (a table of functions in the test): marker round trips, both fits
 *   against known offsets, drift and round trips, a 10 kHz stream on the
 *   reader thread, resolve timeouts, an ambiguous key, a wrong stream, a
 *   stream that disappears and returns, and the loader without liblsl.
 *   tests/loopback/net_loopback.c ran against the real liblsl, 1.17.7 on
 *   Windows (tools/vendor_lsl.py) and 1.17.5 and 1.17.7 on WSL2, in one
 *   process and in two; tests/compile/lsl_abi.cpp checks the declarations
 *   against lsl_c.h. No LabStreamer was attached: docs/net.md has the
 *   numbers and what that leaves open.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_NET_H_INCLUDED
#define YSP_NET_H_INCLUDED

#define YNET_VERSION_MAJOR 0
#define YNET_VERSION_MINOR 3
#define YNET_VERSION_PATCH 0
#define YNET_VERSION_STRING "0.3.0"

#include "ysp/rt.h"
#include "ysp/input.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YNET_API
#define YNET_API extern
#endif

/* Results */
#define YNET_OK          0
#define YNET_ERR_ARG    -1    /* a bad argument or desc                       */
#define YNET_ERR_NOLIB  -2    /* liblsl was not found                         */
#define YNET_ERR_SYMBOL -3    /* liblsl lacks a function this header uses     */
#define YNET_ERR_STATE  -4    /* not running                                  */
#define YNET_ERR_LSL    -5    /* liblsl returned an error                     */
#define YNET_ERR_FORMAT -6    /* the call does not fit the stream's format    */
#define YNET_ERR_TIMEOUT -7   /* not RUNNING within the wait                  */

/* The longest desc.key and desc.role, with the NUL: the handles keep a
 * copy (MATCH KEYS). A key fits ynet_found.key; a role, ydev_roles. */
#define YNET_KEY_MAX  240
#define YNET_ROLE_MAX 32

/* Lifecycle: ysp/device.h's numbers */
#define YNET_CLOSED  0
#define YNET_OPENING 1
#define YNET_RUNNING 2
#define YNET_LOST    3
#define YNET_FAILED  4

/* Why a state changed (STATE's u32[2]): ysp/device.h's numbers, and more */
#define YNET_WHY_START        1u
#define YNET_WHY_OPENED       2u
#define YNET_WHY_DISCONNECTED 3u
#define YNET_WHY_SILENT       4u
#define YNET_WHY_WRONG_STREAM 5u
#define YNET_WHY_STOP         6u

/* Channel formats: LSL's numbers */
#define YNET_FLOAT32 1
#define YNET_STRING  3
#define YNET_INT32   4

/* Record kinds under YRT_SRC_NET */
#define YNET_REC_STATE      1u
#define YNET_REC_GAP        2u
#define YNET_REC_TEXT       3u
#define YNET_REC_OUT        5u
#define YNET_REC_OFFSET    32u
#define YNET_REC_STREAM_GAP 33u
#define YNET_REC_STREAM    34u
#define YNET_REC_DROP      35u

/* YNET_REC_OUT flags (u16[18]): ysp/device.h's bits where they mean the same */
#define YNET_OUT_FAILED 0x0008u
#define YNET_OUT_MARK   0x0010u   /* TEXT records follow                     */
#define YNET_OUT_FLIP   0x0020u   /* from a trigger channel                  */
#define YNET_OUT_AT     0x0200u   /* stamped with a given ysp_rt time        */

#define YNET_MAX_CHANNELS 64
#define YNET_MAX_EDGES    8

/* --- the library ------------------------------------------------------------- */

/* liblsl's functions; LSL's handles (lsl_streaminfo, lsl_outlet, lsl_inlet)
 * are void*. A test may fill this struct with its own functions. */
typedef struct ynet_lsl {
    int32_t       (*library_version)(void);
    double        (*local_clock)(void);
    void          (*destroy_string)(char* s);
    void*         (*create_streaminfo)(const char* name, const char* type, int32_t channel_count,
                                       double nominal_srate, int channel_format, const char* source_id);
    void          (*destroy_streaminfo)(void* info);
    const char*   (*get_name)(void* info);
    const char*   (*get_type)(void* info);
    const char*   (*get_source_id)(void* info);
    const char*   (*get_hostname)(void* info);
    const char*   (*get_uid)(void* info);
    int32_t       (*get_channel_count)(void* info);
    double        (*get_nominal_srate)(void* info);
    int           (*get_channel_format)(void* info);
    int32_t       (*resolve_bypred)(void** buffer, uint32_t buffer_elements, const char* pred, int32_t minimum,
                                    double timeout);
    int32_t       (*resolve_all)(void** buffer, uint32_t buffer_elements, double wait_time);
    void*         (*create_outlet)(void* info, int32_t chunk_size, int32_t max_buffered);
    void          (*destroy_outlet)(void* out);
    int32_t       (*push_sample_strtp)(void* out, const char** data, double timestamp, int32_t pushthrough);
    int32_t       (*push_sample_itp)(void* out, const int32_t* data, double timestamp, int32_t pushthrough);
    int32_t       (*push_chunk_ftnp)(void* out, const float* data, unsigned long data_elements,
                                     const double* timestamps, int32_t pushthrough);
    int32_t       (*have_consumers)(void* out);
    void*         (*create_inlet)(void* info, int32_t max_buflen, int32_t max_chunklen, int32_t recover);
    void          (*destroy_inlet)(void* in);
    void          (*open_stream)(void* in, double timeout, int32_t* ec);
    double        (*time_correction_ex)(void* in, double* remote_time, double* uncertainty, double timeout,
                                        int32_t* ec);
    double        (*pull_sample_str)(void* in, char** buffer, int32_t buffer_elements, double timeout, int32_t* ec);
    double        (*pull_sample_i)(void* in, int32_t* buffer, int32_t buffer_elements, double timeout, int32_t* ec);
    unsigned long (*pull_chunk_f)(void* in, float* data_buffer, double* timestamp_buffer,
                                  unsigned long data_buffer_elements, unsigned long timestamp_buffer_elements,
                                  double timeout, int32_t* ec);
    void*         handle;          /* the loaded library; NULL for a filled table */
    int32_t       version;         /* lsl_library_version()                       */
    char          path[260];       /* what loaded                                 */
    char          error[300];      /* read it with ynet_lsl_error()               */
} ynet_lsl;

YNET_API const char* ynet_version(void);
/* THE LIBRARY: true, or false with the reason in ynet_lsl_error(). */
YNET_API bool ynet_lsl_open(ynet_lsl* lib, const char* path);
/* Unloads liblsl and zeroes lib. Safe on a zeroed lib, after a failed
 * open, and twice. */
YNET_API void ynet_lsl_close(ynet_lsl* lib);
YNET_API const char* ynet_lsl_error(const ynet_lsl* lib);
/* lsl_local_clock() in ns: the LSL clock of this machine, not ysp_rt. */
YNET_API int64_t ynet_lsl_now_ns(const ynet_lsl* lib);
YNET_API const char* ynet_strerror(int code);
YNET_API const char* ynet_state_name(int state);

/* --- keys --------------------------------------------------------------------- */

typedef struct ynet_key {
    char name[64], type[32], source_id[64], hostname[64];
} ynet_key;

/* Parse "lsl:<name>:<type>:<source_id>:<hostname>". false for another
 * prefix, a field too long, or a quote, colon or control character. */
YNET_API bool ynet_key_parse(const char* key, ynet_key* out);
/* liblsl's XPath predicate of the set fields ("name='X' and type='Y'"). */
YNET_API int  ynet_key_predicate(const ynet_key* k, char* out, size_t cap);
/* Whether the stream key matches the pattern: empty pattern fields match
 * anything. */
YNET_API bool ynet_key_match(const char* pattern, const char* key);

/* A stream on the network. */
typedef struct ynet_found {
    char   key[240];
    ynet_key k;
    int    format, channels;
    double rate_hz;
} ynet_found;
/* Every stream liblsl sees within wait_ns (ns); the count (at most cap), or
 * < 0. */
YNET_API int  ynet_resolve_all(const ynet_lsl* lib, ynet_found* out, int cap, int64_t wait_ns);

/* --- the stream ring ------------------------------------------------------------ */

/* Bytes of memory for n records (n a power of two) of ch channels. */
#define YNET_STREAM_STRIDE(ch) ((size_t)((16 + 4 * (size_t)(ch) + 7) / 8 * 8))
#define YNET_STREAM_BYTES(n, ch) ((size_t)(n) * YNET_STREAM_STRIDE(ch) + 8u)

typedef struct ynet_stream {
    unsigned char*    rec;           /* 8-aligned, inside the caller's memory  */
    uint32_t          mask, stride, channels, spare_;
    unsigned char     pad0_[48];     /* head, tail: a cache line each          */
    volatile uint32_t head;          /* the producer's                         */
    volatile uint32_t dropped;       /* the producer's: samples refused        */
    unsigned char     pad1_[56];
    volatile uint32_t tail;          /* the consumer's                         */
    unsigned char     pad2_[60];
    volatile int32_t  newest_lock;
    uint32_t          newest_set;
    int64_t           newest_t;
    double            newest_lsl;
    float             newest[YNET_MAX_CHANNELS];
} ynet_stream;

/* An init (no close): lay out a ring of the largest power of two of
 * records that fits. 0, or YNET_ERR_ARG for room for fewer than 2, or
 * channels outside 1..64. */
YNET_API int      ynet_stream_init(ynet_stream* s, void* mem, size_t bytes, int channels);
/* Up to max samples, oldest first: t (ysp_rt ns), lsl (the stamp, s), x
 * (max * channels floats); any of them NULL to skip. The count. */
YNET_API int      ynet_stream_read(ynet_stream* s, int64_t* t, double* lsl, float* x, int max);
YNET_API bool     ynet_stream_newest(ynet_stream* s, int64_t* t, double* lsl, float* x);
YNET_API uint32_t ynet_stream_capacity(const ynet_stream* s);
/* Samples refused because the ring was full (32 bits, wrapping). */
YNET_API uint32_t ynet_stream_dropped(ynet_stream* s);

/* --- outlets --------------------------------------------------------------------- */

typedef struct ynet_outlet_desc {
    const ynet_lsl*    lib;
    const char*        role;          /* logged; copied, < YNET_ROLE_MAX        */
    const char*        key;           /* lsl:<name>:<type>:<source_id>; copied,
                                       * < YNET_KEY_MAX characters              */
    uint32_t           device;        /* the role's index (aux), > 0            */
    int                format;        /* 0 = YNET_STRING                        */
    int                channels;      /* 0 = 1                                  */
    double             rate_hz;       /* 0 = irregular                          */
    yrt_ring*          ring;          /* records; NULL = none                   */
    const char* const* code_names;    /* a string outlet's text for a code with
                                       * no text: code_names[code]              */
    int                n_code_names;
    int64_t          (*now)(void* ctx);  /* the clock; NULL = yrt_now_ns()      */
    void*              now_ctx;
} ynet_outlet_desc;

typedef struct ynet_out_info {
    int64_t  t_before, t_after;      /* ysp_rt around the push                */
    double   lsl_s;                  /* the LSL stamp sent                    */
    int64_t  t_target;               /* _at and triggers: the time asked for  */
    uint32_t code, seq, flags;
    int      result;
} ynet_out_info;

typedef struct ynet_outlet {
    ynet_outlet_desc d;
    ynet_key       k;
    void*          info;
    void*          outlet;
    yrt_fit        fit;               /* local LSL clock -> ysp_rt             */
    double         origin_s;
    uint32_t       seq;
    uint64_t       marks, pushes, samples, errors;
    ynet_out_info  last;
    int            started;
    union { unsigned char b[64]; void* p; int64_t i; double f; } lock_mem;
    char           key_buf[YNET_KEY_MAX];    /* d.key points here              */
    char           role_buf[YNET_ROLE_MAX];  /* d.role points here             */
    char           err[160];
} ynet_outlet;

YNET_API bool ynet_outlet_open(ynet_outlet* o, const ynet_outlet_desc* desc);
YNET_API void ynet_outlet_close(ynet_outlet* o);
/* The output calls (OUTLETS): any thread, codes only. */
YNET_API int  ynet_out_mark(ynet_outlet* o, uint32_t code, const char* text);
YNET_API int  ynet_out_mark_at(ynet_outlet* o, uint32_t code, const char* text, int64_t t_ns);
/* desc.code_names or the code, stamped with deadline_ns, flagged FLIP: what
 * the trigger channel calls. */
YNET_API int  ynet_out_trigger(ynet_outlet* o, uint32_t code, int64_t deadline_ns);
YNET_API int  ynet_out_push(ynet_outlet* o, const float* x, int n, const double* lsl_s);
/* liblsl's lsl_have_consumers(): 1 when an inlet is connected. */
YNET_API int  ynet_outlet_consumers(ynet_outlet* o);
YNET_API void ynet_out_last(ynet_outlet* o, ynet_out_info* out);
/* The LSL time of a ysp_rt time through the outlet's local fit (s). */
YNET_API double ynet_outlet_to_lsl(ynet_outlet* o, int64_t t_ns);
YNET_API const char* ynet_outlet_error(const ynet_outlet* o);

/* --- inlets ------------------------------------------------------------------------ */

typedef void (*ynet_sink)(void* ctx, const yin_event* e);
typedef void (*ynet_text_sink)(void* ctx, const yin_event* e, const char* text);

typedef struct ynet_edge {
    int      channel;                /* 0-based                                */
    float    level;
    float    hysteresis;             /* the band around level: 0 = none        */
    uint32_t control;                /* the events' control; 0 = channel + 1   */
} ynet_edge;

typedef struct ynet_inlet_desc {
    const ynet_lsl* lib;
    const char*    role;             /* copied, < YNET_ROLE_MAX characters     */
    const char*    key;              /* MATCH KEYS; copied, < YNET_KEY_MAX     */
    uint32_t       device;           /* the role's index, > 0                  */
    int            kind;             /* events' yin kind; 0 = YIN_KIND_SYNC    */
    int            format;           /* required format; 0 = any of the three  */
    int            channels;         /* required count; 0 = any, at most 64    */
    ynet_sink      sink;             /* events                                 */
    void*          sink_ctx;
    ynet_text_sink text_sink;        /* string samples                         */
    void*          text_ctx;
    yrt_ring*      ring;             /* records; NULL = none                   */
    ynet_stream*   stream;           /* float32 samples; NULL = none           */
    ynet_edge      edges[YNET_MAX_EDGES];
    int            n_edges;
    int64_t        resolve_ns;       /* one resolve's wait: 0 = 500 ms         */
    int64_t        retry_ns;         /* 0 = 1 s                                */
    int64_t        silent_ns;        /* 0 = 1 s for a regular stream (never for
                                      * an irregular one); < 0 = never         */
    int64_t        correct_ns;       /* time corrections: 0 = every 1 s        */
    int64_t        max_unc_ns;       /* 0 = 5 ms                               */
    int32_t        lsl_max_buflen;   /* liblsl's max_buflen, an LSL value: 0 =
                                      * 30 (s, or x100 samples for an irregular
                                      * stream)                                */
    int64_t        read_timeout_ns;  /* the reader's longest pull: 0 = 10 ms   */
    bool           manual;           /* no thread: call ynet_inlet_poll()      */
    bool           no_elevate;
    int64_t      (*now)(void* ctx);  /* the clock; NULL = yrt_now_ns()         */
    void*          now_ctx;
} ynet_inlet_desc;

typedef struct ynet_stats {
    int          state;
    uint32_t     opens;
    uint64_t     samples, events, chunks, gaps, missing, dropped, clamped, backward;
    uint64_t     resolves, ambiguous, corrections, refused;
    int64_t      last_rx_ns;         /* ysp_rt time of the last arrival        */
    int64_t      busy_ns;            /* time the reader spent on samples       */
    int64_t      lsl_offset_ns;      /* liblsl's newest time correction: the
                                      * sender's LSL clock to this machine's,
                                      * rounded to the nearest ns             */
    int64_t      lsl_rtt_ns;         /* its round trip (liblsl's uncertainty) */
    double       rate_hz;            /* nominal                                */
    double       measured_hz;        /* samples in the last whole second       */
    int          format, channels;
    yrt_fit_info local, remote;
    ynet_key     found;
    char         uid[48];
} ynet_stats;

#define YNET__CHUNK_FLOATS 16384
#define YNET__CHUNK_SAMPLES 2048

typedef struct ynet_inlet {
    ynet_inlet_desc d;
    ynet_key       k;
    char           pred[400];
    void*          info;
    void*          inlet;
    yrt_fit        local, remote;
    double         origin_s, rorigin_s;
    double         snap_origin, snap_rorigin;  /* under the lock, for ynet_inlet_map() */
    double         period_s, last_ts, last_remote;
    int64_t        next_try, next_local, next_corr, lost_at, last_drop_rec, rate_t0, unc_ns;
    uint64_t       rate_n0, seq_no;
    int            have_rorigin, have_offset, have_last_ts, reported_ambiguous;
    int            edge_hi[YNET_MAX_EDGES];       /* -1 unknown, 0 low, 1 high */
    int            state, stop, started, have_thread;
    ynet_stats     st, snap;
    float          data[YNET__CHUNK_FLOATS];
    double         ts[YNET__CHUNK_SAMPLES];
    union { unsigned char b[64]; void* p; int64_t i; double f; } thread_mem, lock_mem;
    char           key_buf[YNET_KEY_MAX];    /* d.key points here              */
    char           role_buf[YNET_ROLE_MAX];  /* d.role points here             */
    char           err[160];
} ynet_inlet;

YNET_API bool ynet_inlet_open(ynet_inlet* in, const ynet_inlet_desc* desc);
YNET_API void ynet_inlet_close(ynet_inlet* in);
/* Manual mode: one step, pulling for at most timeout_ns (ns). The state. */
YNET_API int  ynet_inlet_poll(ynet_inlet* in, int64_t timeout_ns);
/* Wait at most timeout_ns (ns; 0 or less: look once) for RUNNING (INLETS AND
 * THEIR LIFECYCLE): YNET_RUNNING, YNET_ERR_TIMEOUT, or YNET_ERR_STATE when
 * the inlet ends FAILED or CLOSED; on failure ynet_inlet_error() says why.
 * In manual mode it calls ynet_inlet_poll(); with desc.now it gives up when
 * a poll does not advance that clock. From the thread that opened in. */
YNET_API int  ynet_inlet_wait_running(ynet_inlet* in, int64_t timeout_ns);
YNET_API int  ynet_inlet_state(ynet_inlet* in);
YNET_API void ynet_inlet_stats(ynet_inlet* in, ynet_stats* out);
/* The ysp_rt time of a stamp of this stream, from the newest fits; 0 before
 * any correction. Any thread. */
YNET_API int64_t ynet_inlet_map(ynet_inlet* in, double lsl_s);
YNET_API yin_source ynet_inlet_source(const ynet_inlet* in);
YNET_API const char* ynet_inlet_error(const ynet_inlet* in);

#ifdef __cplusplus
}
#endif

#endif /* YSP_NET_H_INCLUDED */

/* Outside the include guard: including this header again after ysp/screen.h
 * or ysp/device.h adds the glue. */
#if defined(YSP_SCREEN_H_INCLUDED) && defined(YSP_NET_H_INCLUDED) && !defined(YNET__SCREEN_GLUE)
#define YNET__SCREEN_GLUE
/* A ysp/screen.h trigger channel's callback; ctx is the outlet. */
static inline void ynet_trigger_fn(void* ctx, const yscr_trigger_info* info) {
    if (info->flags & YSCR_TRIG_FLUSHED) return;   /* the screen's close: no marker then */
    (void)ynet_out_trigger((ynet_outlet*)ctx, info->code, info->deadline_ns);
}
static inline yscr_trigger_desc ynet_trigger_channel(ynet_outlet* o, int64_t offset_ns) {
    yscr_trigger_desc t;
    t.fn = ynet_trigger_fn;
    t.ctx = o;
    t.offset_ns = offset_ns;
    t.name = o->d.role;
    return t;
}
#endif
#if defined(YSP_DEVICE_H_INCLUDED) && defined(YSP_NET_H_INCLUDED) && !defined(YNET__DEVICE_GLUE)
#define YNET__DEVICE_GLUE
#include <string.h>
#include <stdio.h>
/* The role's index in a ydev_roles table, added when new: desc.device of an
 * LSL instance. 0 when the table is full, the name is longer than 31, or a
 * running ysp/device.h instance has the role. ydev_role_device() gives
 * NULL for an LSL role: the table holds ysp/device.h instances only. */
static inline uint32_t ynet_role(ydev_roles* r, const char* name) {
    int i;
    if (!r || !name || !name[0] || strlen(name) >= sizeof r->name[0]) return 0;
    i = ydev_role_index(r, name);
    if (i) return r->dev[i - 1] ? 0u : (uint32_t)i;
    if (r->n >= YDEV_MAX_ROLES) return 0;
    snprintf(r->name[r->n], sizeof r->name[0], "%s", name);
    r->dev[r->n] = NULL;
    return (uint32_t)++r->n;
}
#endif

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_NET_IMPLEMENTATION
#ifndef YSP_NET_IMPLEMENTATION_GUARD
#define YSP_NET_IMPLEMENTATION_GUARD

#ifndef YSP_RT_IMPLEMENTATION_GUARD
    #define YSP_RT_IMPLEMENTATION
    #include "ysp/rt.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #if defined(_MSC_VER)
        #include <intrin.h>
    #endif
#else
    #include <pthread.h>
    #include <dlfcn.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- atomics, locks, threads ----------------------------------------------------- */

#if defined(_MSC_VER) && !defined(__clang__)
static uint32_t ynet__ld(volatile uint32_t* p) { return (uint32_t)_InterlockedOr((volatile long*)p, 0); }
static void     ynet__st(volatile uint32_t* p, uint32_t v) { (void)_InterlockedExchange((volatile long*)p, (long)v); }
static int32_t  ynet__xchg(volatile int32_t* p, int32_t v) { return (int32_t)_InterlockedExchange((volatile long*)p, (long)v); }
static void     ynet__rel(volatile int32_t* p) { (void)_InterlockedExchange((volatile long*)p, 0); }
#else
static uint32_t ynet__ld(volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void     ynet__st(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static int32_t  ynet__xchg(volatile int32_t* p, int32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQUIRE); }
static void     ynet__rel(volatile int32_t* p) { __atomic_store_n(p, 0, __ATOMIC_RELEASE); }
#endif

#if defined(_WIN32)
typedef CRITICAL_SECTION ynet__mutex;
static void ynet__lock_init(ynet__mutex* m) { InitializeCriticalSection(m); }
static void ynet__lock_free(ynet__mutex* m) { DeleteCriticalSection(m); }
static void ynet__lock(ynet__mutex* m)      { EnterCriticalSection(m); }
static void ynet__unlock(ynet__mutex* m)    { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t ynet__mutex;
static void ynet__lock_init(ynet__mutex* m) { pthread_mutex_init(m, NULL); }
static void ynet__lock_free(ynet__mutex* m) { pthread_mutex_destroy(m); }
static void ynet__lock(ynet__mutex* m)      { pthread_mutex_lock(m); }
static void ynet__unlock(ynet__mutex* m)    { pthread_mutex_unlock(m); }
#endif
/* The locks live in the instances (no allocation). */
typedef char ynet__lock_fits[sizeof(ynet__mutex) <= 64 ? 1 : -1];

YNET_API const char* ynet_version(void) { return YNET_VERSION_STRING; }

YNET_API const char* ynet_strerror(int code) {
    switch (code) {
    case YNET_OK:         return "ok";
    case YNET_ERR_ARG:    return "bad argument";
    case YNET_ERR_NOLIB:  return "liblsl not found";
    case YNET_ERR_SYMBOL: return "liblsl lacks a function";
    case YNET_ERR_STATE:  return "not running";
    case YNET_ERR_LSL:    return "liblsl error";
    case YNET_ERR_FORMAT: return "wrong format for the stream";
    case YNET_ERR_TIMEOUT: return "not running in time";
    default:              return "?";
    }
}

YNET_API const char* ynet_state_name(int state) {
    switch (state) {
    case YNET_CLOSED:  return "closed";
    case YNET_OPENING: return "opening";
    case YNET_RUNNING: return "running";
    case YNET_LOST:    return "lost";
    case YNET_FAILED:  return "failed";
    default:           return "?";
    }
}

/* --- the loader ------------------------------------------------------------------- */

typedef struct ynet__sym { const char* name; size_t off; } ynet__sym;
#define YNET__S(f) { "lsl_" #f, offsetof(ynet_lsl, f) }
static const ynet__sym ynet__syms[] = {
    YNET__S(library_version), YNET__S(local_clock), YNET__S(destroy_string), YNET__S(create_streaminfo),
    YNET__S(destroy_streaminfo), YNET__S(get_name), YNET__S(get_type), YNET__S(get_source_id),
    YNET__S(get_hostname), YNET__S(get_uid), YNET__S(get_channel_count), YNET__S(get_nominal_srate),
    YNET__S(get_channel_format), YNET__S(resolve_bypred), YNET__S(resolve_all), YNET__S(create_outlet),
    YNET__S(destroy_outlet), YNET__S(push_sample_strtp), YNET__S(push_sample_itp), YNET__S(push_chunk_ftnp),
    YNET__S(have_consumers), YNET__S(create_inlet), YNET__S(destroy_inlet), YNET__S(open_stream),
    YNET__S(time_correction_ex), YNET__S(pull_sample_str), YNET__S(pull_sample_i), YNET__S(pull_chunk_f)
};
#undef YNET__S

static void* ynet__open(const char* p) {
#if defined(_WIN32)
    /* never the current folder, where a data file's neighbor could be
     * planted: the program's folder, System32 and AddDllDirectory() for a
     * bare name; a path's own folder for its dependencies (as
     * ysp/parallel.h loads InpOut) */
    if (strchr(p, '\\') || strchr(p, '/')) {
        char full[MAX_PATH];
        DWORD n = GetFullPathNameA(p, (DWORD)sizeof full, full, NULL);
        if (n == 0 || n >= sizeof full) return NULL;
        return (void*)LoadLibraryExA(full, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    }
    return (void*)LoadLibraryExA(p, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return dlopen(p, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void ynet__close(void* h) {
#if defined(_WIN32)
    if (h) FreeLibrary((HMODULE)h);
#else
    if (h) dlclose(h);
#endif
}

YNET_API bool ynet_lsl_open(ynet_lsl* lib, const char* path) {
#if defined(_WIN32)
    static const char* const names[] = { "lsl.dll", "liblsl.dll", NULL };
#elif defined(__APPLE__)
    static const char* const names[] = { "liblsl.dylib", "liblsl.2.dylib", NULL };
#else
    static const char* const names[] = { "liblsl.so", "liblsl.so.2", "liblsl.so.1", NULL };
#endif
    const char* env;
    char envbuf[260];
    const char* tried[8];
    int nt = 0, i;
    size_t k;
    void* h = NULL;
    if (!lib) return false;
    memset(lib, 0, sizeof *lib);
#if defined(_WIN32)
    {   /* not getenv(): MSVC deprecates it */
        DWORD n = GetEnvironmentVariableA("YSP_LSL_PATH", envbuf, (DWORD)sizeof envbuf);
        env = n > 0 && n < sizeof envbuf ? envbuf : NULL;
    }
#else
    (void)envbuf;
    env = getenv("YSP_LSL_PATH");
#endif
    if (path && path[0]) tried[nt++] = path;
    else {
        if (env && env[0]) tried[nt++] = env;
        for (i = 0; names[i]; i++) tried[nt++] = names[i];
    }
    for (i = 0; i < nt && !h; i++) {
        h = ynet__open(tried[i]);
        if (h) snprintf(lib->path, sizeof lib->path, "%s", tried[i]);
    }
    if (!h) {
        size_t n = 0;
        n += (size_t)snprintf(lib->error, sizeof lib->error, "ysp_net: liblsl not found (tried");
        for (i = 0; i < nt && n < sizeof lib->error; i++)
            n += (size_t)snprintf(lib->error + n, sizeof lib->error - n, " %s", tried[i]);
        if (n < sizeof lib->error) snprintf(lib->error + n, sizeof lib->error - n, "); set YSP_LSL_PATH");
        return false;
    }
    for (k = 0; k < sizeof ynet__syms / sizeof ynet__syms[0]; k++) {
#if defined(_WIN32)
        FARPROC f = GetProcAddress((HMODULE)h, ynet__syms[k].name);
#else
        void* f = dlsym(h, ynet__syms[k].name);
#endif
        if (!f) {
            snprintf(lib->error, sizeof lib->error, "ysp_net: %.200s has no %.40s: liblsl 1.13 or later is needed",
                     lib->path, ynet__syms[k].name);
            ynet__close(h);
            lib->path[0] = '\0';
            return false;
        }
        /* a function pointer into its slot: memcpy, which no cast warning sees */
        memcpy((unsigned char*)lib + ynet__syms[k].off, &f, sizeof f);
    }
    lib->handle = h;
    lib->version = lib->library_version();
    return true;
}

YNET_API const char* ynet_lsl_error(const ynet_lsl* lib) { return lib ? lib->error : "ysp_net: NULL lib"; }

YNET_API void ynet_lsl_close(ynet_lsl* lib) {
    if (!lib) return;
    ynet__close(lib->handle);
    memset(lib, 0, sizeof *lib);
}

static bool ynet__lib_ok(const ynet_lsl* lib) {
    size_t k;
    if (!lib) return false;
    for (k = 0; k < sizeof ynet__syms / sizeof ynet__syms[0]; k++) {
        void* f;
        memcpy(&f, (const unsigned char*)lib + ynet__syms[k].off, sizeof f);
        if (!f) return false;
    }
    return true;
}

YNET_API int64_t ynet_lsl_now_ns(const ynet_lsl* lib) {
    if (!lib || !lib->local_clock) return 0;
    return (int64_t)(lib->local_clock() * 1e9);
}

/* --- keys ----------------------------------------------------------------------------- */

static bool ynet__field(const char* s, size_t* i, char* f, size_t cap) {
    size_t n = 0;
    while (s[*i] && s[*i] != ':') {
        unsigned char c = (unsigned char)s[*i];
        if (c < 0x20 || c == '\'' || c == 0x7f) return false;
        if (n + 1 >= cap) return false;
        f[n++] = (char)c;
        (*i)++;
    }
    if (s[*i] == ':') (*i)++;
    f[n] = '\0';
    return true;
}

YNET_API bool ynet_key_parse(const char* key, ynet_key* out) {
    size_t i = 4;
    if (!key || !out || strncmp(key, "lsl:", 4) != 0) return false;
    memset(out, 0, sizeof *out);
    if (!ynet__field(key, &i, out->name, sizeof out->name)) return false;
    if (!ynet__field(key, &i, out->type, sizeof out->type)) return false;
    if (!ynet__field(key, &i, out->source_id, sizeof out->source_id)) return false;
    if (!ynet__field(key, &i, out->hostname, sizeof out->hostname)) return false;
    return key[i] == '\0';
}

YNET_API int ynet_key_predicate(const ynet_key* k, char* out, size_t cap) {
    const char* names[4] = { "name", "type", "source_id", "hostname" };
    const char* vals[4];
    size_t n = 0;
    int i, m = 0;
    if (!k || !out || cap == 0) return -1;
    vals[0] = k->name; vals[1] = k->type; vals[2] = k->source_id; vals[3] = k->hostname;
    out[0] = '\0';
    for (i = 0; i < 4; i++) {
        int w;
        if (!vals[i][0]) continue;
        w = snprintf(out + n, cap - n, "%s%s='%s'", m ? " and " : "", names[i], vals[i]);
        if (w < 0 || (size_t)w >= cap - n) { out[0] = '\0'; return -1; }
        n += (size_t)w;
        m++;
    }
    return m ? (int)n : -1;
}

YNET_API bool ynet_key_match(const char* pattern, const char* key) {
    ynet_key p, k;
    if (!ynet_key_parse(pattern, &p) || !ynet_key_parse(key, &k)) return false;
    return (!p.name[0] || strcmp(p.name, k.name) == 0) && (!p.type[0] || strcmp(p.type, k.type) == 0) &&
           (!p.source_id[0] || strcmp(p.source_id, k.source_id) == 0) &&
           (!p.hostname[0] || strcmp(p.hostname, k.hostname) == 0);
}

/* A field of a found stream, cut and cleaned so it fits a key. */
static void ynet__clean(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    if (src)
        for (; *src && n + 1 < cap; src++) {
            unsigned char c = (unsigned char)*src;
            dst[n++] = (c < 0x20 || c == ':' || c == '\'' || c == 0x7f) ? '_' : (char)c;
        }
    dst[n] = '\0';
}

static void ynet__info_key(const ynet_lsl* lib, void* info, ynet_key* k) {
    ynet__clean(k->name, sizeof k->name, lib->get_name(info));
    ynet__clean(k->type, sizeof k->type, lib->get_type(info));
    ynet__clean(k->source_id, sizeof k->source_id, lib->get_source_id(info));
    ynet__clean(k->hostname, sizeof k->hostname, lib->get_hostname(info));
}

YNET_API int ynet_resolve_all(const ynet_lsl* lib, ynet_found* out, int cap, int64_t wait_ns) {
    void* buf[64];
    int n, i;
    if (!ynet__lib_ok(lib) || !out || cap <= 0) return YNET_ERR_ARG;
    /* liblsl takes seconds */
    n = lib->resolve_all(buf, 64, wait_ns > 0 ? (double)wait_ns / 1e9 : 0.0);
    if (n < 0) return YNET_ERR_LSL;
    for (i = 0; i < n; i++) {
        if (i < cap) {
            ynet_found* f = &out[i];
            memset(f, 0, sizeof *f);
            ynet__info_key(lib, buf[i], &f->k);
            f->format = lib->get_channel_format(buf[i]);
            f->channels = lib->get_channel_count(buf[i]);
            f->rate_hz = lib->get_nominal_srate(buf[i]);
            snprintf(f->key, sizeof f->key, "lsl:%s:%s:%s:%s", f->k.name, f->k.type, f->k.source_id, f->k.hostname);
        }
        lib->destroy_streaminfo(buf[i]);
    }
    return n < cap ? n : cap;
}

/* --- the stream ring ---------------------------------------------------------------------- */

YNET_API int ynet_stream_init(ynet_stream* s, void* mem, size_t bytes, int channels) {
    uintptr_t a;
    size_t stride, n = 1;
    if (!s) return YNET_ERR_ARG;
    memset(s, 0, sizeof *s);
    if (!mem || channels < 1 || channels > YNET_MAX_CHANNELS) return YNET_ERR_ARG;
    a = ((uintptr_t)mem + 7u) & ~(uintptr_t)7u;
    if (bytes < (size_t)(a - (uintptr_t)mem)) return YNET_ERR_ARG;
    bytes -= (size_t)(a - (uintptr_t)mem);
    stride = YNET_STREAM_STRIDE(channels);
    while (n * 2 * stride <= bytes && n < ((size_t)1 << 30)) n *= 2;
    if (n < 2 || n * stride > bytes) return YNET_ERR_ARG;
    s->rec = (unsigned char*)a;
    s->mask = (uint32_t)(n - 1);
    s->stride = (uint32_t)stride;
    s->channels = (uint32_t)channels;
    return 0;
}

YNET_API uint32_t ynet_stream_capacity(const ynet_stream* s) { return s && s->rec ? s->mask + 1u : 0u; }

YNET_API uint32_t ynet_stream_dropped(ynet_stream* s) { return s ? ynet__ld(&s->dropped) : 0u; }

/* The reader: n samples of ch floats each, no wait. Returns those kept. */
static int ynet__stream_put(ynet_stream* s, const int64_t* t, const double* lsl, const float* x, int n, int ch) {
    uint32_t head = s->head, tail = ynet__ld(&s->tail), cap = s->mask + 1u;
    int i, kept = 0, c = ch < (int)s->channels ? ch : (int)s->channels;
    for (i = 0; i < n; i++) {
        unsigned char* r;
        if (head - tail >= cap) {
            tail = ynet__ld(&s->tail);
            if (head - tail >= cap) { ynet__st(&s->dropped, s->dropped + (uint32_t)(n - i)); break; }
        }
        r = s->rec + (size_t)(head & s->mask) * s->stride;
        memcpy(r, &t[i], 8);
        memcpy(r + 8, &lsl[i], 8);
        memcpy(r + 16, x + (size_t)i * (size_t)ch, (size_t)c * 4u);
        if (c < (int)s->channels) memset(r + 16 + 4 * c, 0, (size_t)((int)s->channels - c) * 4u);
        head++;
        kept++;
    }
    ynet__st(&s->head, head);
    if (n > 0 && ynet__xchg(&s->newest_lock, 1) == 0) {
        /* never waits: when a reader holds the slot, the next chunk updates it */
        s->newest_t = t[n - 1];
        s->newest_lsl = lsl[n - 1];
        memcpy(s->newest, x + (size_t)(n - 1) * (size_t)ch, (size_t)c * 4u);
        s->newest_set = 1;
        ynet__rel(&s->newest_lock);
    }
    return kept;
}

YNET_API int ynet_stream_read(ynet_stream* s, int64_t* t, double* lsl, float* x, int max) {
    uint32_t head, tail;
    int n = 0;
    if (!s || !s->rec || max <= 0) return 0;
    head = ynet__ld(&s->head);
    tail = s->tail;
    while (tail != head && n < max) {
        const unsigned char* r = s->rec + (size_t)(tail & s->mask) * s->stride;
        if (t) memcpy(&t[n], r, 8);
        if (lsl) memcpy(&lsl[n], r + 8, 8);
        if (x) memcpy(x + (size_t)n * s->channels, r + 16, (size_t)s->channels * 4u);
        tail++;
        n++;
    }
    ynet__st(&s->tail, tail);
    return n;
}

YNET_API bool ynet_stream_newest(ynet_stream* s, int64_t* t, double* lsl, float* x) {
    bool ok;
    if (!s || !s->rec) return false;
    while (ynet__xchg(&s->newest_lock, 1) != 0) { }   /* the reader holds it for a copy */
    ok = s->newest_set != 0;
    if (ok) {
        if (t) *t = s->newest_t;
        if (lsl) *lsl = s->newest_lsl;
        if (x) memcpy(x, s->newest, (size_t)s->channels * 4u);
    }
    ynet__rel(&s->newest_lock);
    return ok;
}

/* --- clocks ------------------------------------------------------------------------------ */

/* Ticks are ns since an origin, plus a bias so stamps somewhat before the
 * origin stay positive: 10^15 ns is 11.6 days. */
#define YNET__BIAS 1000000000000000LL

/* liblsl's double seconds to int64_t ns, to nearest (llround, so a
 * negative offset rounds as a positive one does). */
static int64_t ynet__ns(double s) { return (int64_t)llround(s * 1e9); }

static uint64_t ynet__ticks(double s, double origin) {
    double v = (s - origin) * 1e9 + (double)YNET__BIAS;
    if (v < 0) v = 0;
    return (uint64_t)(v + 0.5);
}

static void ynet__fit_init(yrt_fit* f, int mode, uint32_t clock_id, int64_t max_width, yrt_ring* ring) {
    yrt_fit_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.mode = mode;
    fd.ns_per_tick = 1.0;
    fd.clock_id = clock_id;
    fd.max_width_ns = max_width;
    fd.ring = ring;
    (void)yrt_fit_open(f, &fd);
}

typedef int64_t (*ynet__now_fn)(void* ctx);

static int64_t ynet__clock(ynet__now_fn now, void* ctx) { return now ? now(ctx) : yrt_now_ns(); }

/* lsl_local_clock() between two ysp_rt reads, the tightest of tries. */
static void ynet__bracket(const ynet_lsl* lib, ynet__now_fn now, void* ctx, int tries,
                          double* lsl_s, int64_t* end, int64_t* width) {
    int i;
    *width = INT64_MAX;
    *end = 0;
    *lsl_s = 0.0;
    for (i = 0; i < tries; i++) {
        int64_t a = ynet__clock(now, ctx);
        double l = lib->local_clock();
        int64_t b = ynet__clock(now, ctx);
        if (b - a < *width) { *width = b - a; *end = b; *lsl_s = l; }
    }
}

/* The LSL time of t_ns through a local fit's map. */
static double ynet__inverse(const yrt_fit* f, double origin, int64_t t_ns) {
    yrt_fit_info fi;
    double ticks;
    yrt_fit_get(f, &fi);
    if (!fi.ready || fi.ns_per_tick <= 0) return 0.0;
    ticks = (double)((int64_t)fi.ticks0 - YNET__BIAS) + (double)(t_ns - fi.t0_ns) / fi.ns_per_tick;
    return origin + ticks / 1e9;
}

static void ynet__rec(yrt_ring* ring, uint32_t aux, uint16_t kind, int64_t t, const yrt_payload* u) {
    yrt_event ev;
    if (!ring) return;
    memset(&ev, 0, sizeof ev);
    ev.t_ns = (uint64_t)t;
    ev.source = (uint16_t)YRT_SRC_NET;
    ev.kind = kind;
    ev.aux = aux;
    ev.u = *u;
    (void)yrt_ring_push(ring, &ev);
}

/* TEXT records "<what> <text>", 34 characters of text each. */
static void ynet__text(yrt_ring* ring, uint32_t aux, int64_t t, const char* what, const char* text) {
    size_t n, i;
    if (!ring || !text) return;
    n = strlen(text);
    i = 0;
    do {
        yrt_payload u;
        memset(&u, 0, sizeof u);
        snprintf(u.text, sizeof u.text, "%s %.34s", what, text + i);
        ynet__rec(ring, aux, (uint16_t)YNET_REC_TEXT, t, &u);
        i += 34;
    } while (i < n);
}

/* --- outlets --------------------------------------------------------------------------------- */

static ynet__mutex* ynet__omx(ynet_outlet* o) { return (ynet__mutex*)(void*)o->lock_mem.b; }
static int64_t ynet__onow(const ynet_outlet* o) { return ynet__clock(o->d.now, o->d.now_ctx); }

YNET_API const char* ynet_outlet_error(const ynet_outlet* o) { return o ? o->err : "ysp_net: NULL outlet"; }

YNET_API bool ynet_outlet_open(ynet_outlet* o, const ynet_outlet_desc* desc) {
    double l = 0.0;
    int64_t end = 0, w = 0;
    int i;
    if (!o) return false;
    memset(o, 0, sizeof *o);
#define YNET__OFAIL(...) do { snprintf(o->err, sizeof o->err, "ysp_net: " __VA_ARGS__); return false; } while (0)
    if (!desc) YNET__OFAIL("NULL desc");
    if (!ynet__lib_ok(desc->lib)) YNET__OFAIL("desc.lib is not an open liblsl (ynet_lsl_open)");
    if (!desc->role || !desc->role[0]) YNET__OFAIL("desc.role is required");
    if (!desc->key || !ynet_key_parse(desc->key, &o->k) || !o->k.name[0])
        YNET__OFAIL("desc.key must be lsl:<name>:<type>:<source_id> with a name (MATCH KEYS)");
    if (desc->device == 0) YNET__OFAIL("desc.device must be > 0 (the role's index)");
    if (desc->format != 0 && desc->format != YNET_STRING && desc->format != YNET_INT32 && desc->format != YNET_FLOAT32)
        YNET__OFAIL("desc.format: YNET_STRING, YNET_INT32 or YNET_FLOAT32");
    if (desc->channels < 0 || desc->channels > YNET_MAX_CHANNELS) YNET__OFAIL("desc.channels: 0 to 64");
    if (desc->format != YNET_FLOAT32 && desc->channels > 1) YNET__OFAIL("a marker outlet has one channel");
    if (desc->rate_hz < 0) YNET__OFAIL("desc.rate_hz must be >= 0");
    if (strlen(desc->key) >= sizeof o->key_buf) YNET__OFAIL("desc.key: at most 239 characters (YNET_KEY_MAX)");
    if (strlen(desc->role) >= sizeof o->role_buf) YNET__OFAIL("desc.role: at most 31 characters (YNET_ROLE_MAX)");
    o->d = *desc;
    memcpy(o->key_buf, desc->key, strlen(desc->key) + 1);
    memcpy(o->role_buf, desc->role, strlen(desc->role) + 1);
    o->d.key = o->key_buf;
    o->d.role = o->role_buf;
    if (!o->d.format) o->d.format = YNET_STRING;
    if (!o->d.channels) o->d.channels = 1;
    o->info = o->d.lib->create_streaminfo(o->k.name, o->k.type, o->d.channels, o->d.rate_hz, o->d.format,
                                          o->k.source_id);
    if (!o->info) YNET__OFAIL("lsl_create_streaminfo failed");
    /* chunk 0: each push is one chunk; 360: liblsl's suggested buffer */
    o->outlet = o->d.lib->create_outlet(o->info, 0, 360);
    if (!o->outlet) {
        o->d.lib->destroy_streaminfo(o->info);
        o->info = NULL;
        YNET__OFAIL("lsl_create_outlet failed");
    }
    ynet__lock_init(ynet__omx(o));
    ynet__fit_init(&o->fit, YRT_FIT_BRACKET, 0x10000u + o->d.device, 0, o->d.ring);
    ynet__bracket(o->d.lib, o->d.now, o->d.now_ctx, 1, &l, &end, &w);
    o->origin_s = l;
    for (i = 0; i < 8; i++) {
        ynet__bracket(o->d.lib, o->d.now, o->d.now_ctx, 1, &l, &end, &w);
        (void)yrt_fit_add(&o->fit, ynet__ticks(l, o->origin_s), end, w);
    }
    o->started = 1;
    {
        int64_t t = ynet__onow(o);
        yrt_payload u;
        ynet__text(o->d.ring, o->d.device, t, "role", o->d.role);
        ynet__text(o->d.ring, o->d.device, t, "outlet", o->d.key);
        memset(&u, 0, sizeof u);
        u.u32[0] = YNET_CLOSED;
        u.u32[1] = YNET_RUNNING;
        u.u32[2] = YNET_WHY_START;
        u.u32[3] = 1;
        ynet__rec(o->d.ring, o->d.device, (uint16_t)YNET_REC_STATE, t, &u);
    }
#undef YNET__OFAIL
    return true;
}

YNET_API void ynet_outlet_close(ynet_outlet* o) {
    yrt_payload u;
    if (!o || !o->started) return;
    ynet__lock(ynet__omx(o));
    o->started = 0;
    if (o->outlet) o->d.lib->destroy_outlet(o->outlet);
    if (o->info) o->d.lib->destroy_streaminfo(o->info);
    o->outlet = o->info = NULL;
    ynet__unlock(ynet__omx(o));
    ynet__lock_free(ynet__omx(o));
    memset(&u, 0, sizeof u);
    u.u32[0] = YNET_RUNNING;
    u.u32[1] = YNET_CLOSED;
    u.u32[2] = YNET_WHY_STOP;
    u.u32[3] = 1;
    ynet__rec(o->d.ring, o->d.device, (uint16_t)YNET_REC_STATE, ynet__onow(o), &u);
}

YNET_API double ynet_outlet_to_lsl(ynet_outlet* o, int64_t t_ns) {
    double r;
    if (!o || !o->started) return 0.0;
    ynet__lock(ynet__omx(o));
    r = ynet__inverse(&o->fit, o->origin_s, t_ns);
    ynet__unlock(ynet__omx(o));
    return r;
}

/* Every mark. at: a ysp_rt time to stamp, or 0 for now. */
static int ynet__mark(ynet_outlet* o, uint32_t code, const char* text, int64_t at, uint32_t flags) {
    yrt_payload u;
    int64_t t0, tm, t1;
    double l, ts;
    int rc = YNET_OK;
    char num[16];
    const char* s;
    uint32_t seq;
    if (!o || !o->started) return YNET_ERR_STATE;
    if (o->d.format == YNET_FLOAT32) return YNET_ERR_FORMAT;
    if (text && text[0]) s = text;
    else if (o->d.code_names && (int64_t)code < (int64_t)o->d.n_code_names && o->d.code_names[code]) s = o->d.code_names[code];
    else { snprintf(num, sizeof num, "%u", (unsigned)code); s = num; }
    if (at) flags |= YNET_OUT_AT;
    if (text && text[0]) flags |= YNET_OUT_MARK;
    ynet__lock(ynet__omx(o));
    seq = ++o->seq;
    t0 = ynet__onow(o);
    l = o->d.lib->local_clock();
    tm = ynet__onow(o);
    ts = at ? ynet__inverse(&o->fit, o->origin_s, at) : l;
    if (o->d.format == YNET_STRING) {
        const char* one[1];
        one[0] = s;
        if (o->d.lib->push_sample_strtp(o->outlet, one, ts, 1) != 0) rc = YNET_ERR_LSL;
    } else {
        int32_t v = (int32_t)code;
        if (o->d.lib->push_sample_itp(o->outlet, &v, ts, 1) != 0) rc = YNET_ERR_LSL;
    }
    t1 = ynet__onow(o);
    /* the clock read is a pair of the local fit; its bracket excludes the push */
    (void)yrt_fit_add(&o->fit, ynet__ticks(l, o->origin_s), tm, tm - t0);
    if (rc != YNET_OK) { flags |= YNET_OUT_FAILED; o->errors++; }
    else o->marks++;
    o->last.t_before = t0;
    o->last.t_after = t1;
    o->last.lsl_s = ts;
    o->last.t_target = at;
    o->last.code = code;
    o->last.seq = seq;
    o->last.flags = flags;
    o->last.result = rc;
    memset(&u, 0, sizeof u);
    u.i64[0] = t0;
    u.i64[1] = t1;
    u.i64[2] = (int64_t)(ts * 1e9);
    u.i64[3] = at;
    u.u32[8] = code;
    u.u16[18] = (uint16_t)flags;
    u.u16[19] = (uint16_t)seq;
    ynet__rec(o->d.ring, o->d.device, (uint16_t)YNET_REC_OUT, t0, &u);
    if (text && text[0]) ynet__text(o->d.ring, o->d.device, t0, "mark", text);
    ynet__unlock(ynet__omx(o));
    return rc;
}

YNET_API int ynet_out_mark(ynet_outlet* o, uint32_t code, const char* text) { return ynet__mark(o, code, text, 0, 0u); }

YNET_API int ynet_out_mark_at(ynet_outlet* o, uint32_t code, const char* text, int64_t t_ns) {
    if (t_ns <= 0) return YNET_ERR_ARG;
    return ynet__mark(o, code, text, t_ns, 0u);
}

YNET_API int ynet_out_trigger(ynet_outlet* o, uint32_t code, int64_t deadline_ns) {
    if (deadline_ns <= 0) return YNET_ERR_ARG;
    return ynet__mark(o, code, NULL, deadline_ns, YNET_OUT_FLIP);
}

YNET_API int ynet_out_push(ynet_outlet* o, const float* x, int n, const double* lsl_s) {
    double ts[256];
    int rc = YNET_OK, done = 0;
    if (!o || !o->started) return YNET_ERR_STATE;
    if (o->d.format != YNET_FLOAT32) return YNET_ERR_FORMAT;
    if (!x || n < 0) return YNET_ERR_ARG;
    ynet__lock(ynet__omx(o));
    while (done < n && rc == YNET_OK) {
        int m = n - done < 256 ? n - done : 256, i;
        const double* stamps = lsl_s ? lsl_s + done : ts;
        if (!lsl_s) {
            /* now for the last sample; the rate spaces the others */
            double now = o->d.lib->local_clock();
            double per = o->d.rate_hz > 0 ? 1.0 / o->d.rate_hz : 0.0;
            for (i = 0; i < m; i++) ts[i] = now - (double)(n - done - 1 - i) * per;
        }
        if (o->d.lib->push_chunk_ftnp(o->outlet, x + (size_t)done * (size_t)o->d.channels,
                                      (unsigned long)m * (unsigned long)o->d.channels, stamps, 1) != 0)
            rc = YNET_ERR_LSL;
        done += m;
    }
    if (rc == YNET_OK) { o->pushes++; o->samples += (uint64_t)n; }
    else o->errors++;
    ynet__unlock(ynet__omx(o));
    return rc;
}

YNET_API int ynet_outlet_consumers(ynet_outlet* o) {
    int r;
    if (!o || !o->started) return 0;
    ynet__lock(ynet__omx(o));
    r = o->d.lib->have_consumers(o->outlet);
    ynet__unlock(ynet__omx(o));
    return r;
}

YNET_API void ynet_out_last(ynet_outlet* o, ynet_out_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!o || !o->started) return;
    ynet__lock(ynet__omx(o));
    *out = o->last;
    ynet__unlock(ynet__omx(o));
}

/* --- inlets ------------------------------------------------------------------------------------ */

static ynet__mutex* ynet__imx(ynet_inlet* in) { return (ynet__mutex*)(void*)in->lock_mem.b; }
static int64_t ynet__inow(const ynet_inlet* in) { return ynet__clock(in->d.now, in->d.now_ctx); }

YNET_API const char* ynet_inlet_error(const ynet_inlet* in) { return in ? in->err : "ysp_net: NULL inlet"; }

static void ynet__irec(ynet_inlet* in, uint16_t kind, int64_t t, const yrt_payload* u) {
    ynet__rec(in->d.ring, in->d.device, kind, t, u);
}

static void ynet__set_state(ynet_inlet* in, int s, uint32_t why) {
    yrt_payload u;
    if (in->state == s) return;
    memset(&u, 0, sizeof u);
    u.u32[0] = (uint32_t)in->state;
    u.u32[1] = (uint32_t)s;
    u.u32[2] = why;
    u.u32[3] = in->st.opens;
    ynet__irec(in, (uint16_t)YNET_REC_STATE, ynet__inow(in), &u);
    in->state = s;
    in->st.state = s;
}

/* The fits' infos and the uncertainty they give, after any pair. */
static void ynet__refresh(ynet_inlet* in) {
    yrt_fit_get(&in->local, &in->st.local);
    yrt_fit_get(&in->remote, &in->st.remote);
    {
        int64_t half = in->st.lsl_rtt_ns / 2;
        int64_t r = in->st.remote.ready && in->st.remote.spread_ns > half ? in->st.remote.spread_ns : half;
        in->unc_ns = r + in->st.local.width_ns;
    }
}

static void ynet__publish(ynet_inlet* in) {
    ynet__refresh(in);
    ynet__lock(ynet__imx(in));
    in->snap = in->st;
    in->snap_origin = in->origin_s;
    in->snap_rorigin = in->rorigin_s;
    ynet__unlock(ynet__imx(in));
}

static void ynet__drop_inlet(ynet_inlet* in) {
    if (in->inlet) in->d.lib->destroy_inlet(in->inlet);
    if (in->info) in->d.lib->destroy_streaminfo(in->info);
    in->inlet = in->info = NULL;
}

static void ynet__lose(ynet_inlet* in, uint32_t why) {
    ynet__drop_inlet(in);
    in->lost_at = in->st.last_rx_ns;
    in->next_try = ynet__inow(in) + in->d.retry_ns;
    ynet__set_state(in, YNET_LOST, why);
}

/* A local pair: the LSL clock of this machine against ysp_rt. */
static void ynet__local_pair(ynet_inlet* in) {
    double l = 0;
    int64_t end = 0, w = 0;
    ynet__bracket(in->d.lib, in->d.now, in->d.now_ctx, 8, &l, &end, &w);
    if (in->origin_s == 0.0) in->origin_s = l != 0.0 ? l : 1e-9;
    (void)yrt_fit_add(&in->local, ynet__ticks(l, in->origin_s), end, w);
    ynet__refresh(in);
}

static void ynet__correct(ynet_inlet* in, double timeout_s) {
    double remote = 0, unc = 0, off;
    int32_t ec = 0;
    int64_t t = ynet__inow(in), host, w;
    yrt_payload u;
    off = in->d.lib->time_correction_ex(in->inlet, &remote, &unc, timeout_s, &ec);
    if (ec != 0 || remote == 0.0) return;
    if (in->have_offset && remote == in->last_remote) return;   /* liblsl's cached estimate */
    in->last_remote = remote;
    in->st.lsl_offset_ns = ynet__ns(off);
    in->st.lsl_rtt_ns = ynet__ns(unc);
    in->have_offset = 1;
    in->st.corrections++;
    if (!in->have_rorigin) { in->rorigin_s = remote; in->have_rorigin = 1; }
    w = in->st.lsl_rtt_ns;
    host = yrt_fit_map(&in->local, ynet__ticks(remote + off, in->origin_s));
    if (w > in->d.max_unc_ns) in->st.refused++;
    else (void)yrt_fit_add(&in->remote, ynet__ticks(remote, in->rorigin_s), host, 0);
    memset(&u, 0, sizeof u);
    u.i64[0] = (int64_t)(remote * 1e9);
    u.i64[1] = in->st.lsl_offset_ns;
    u.i64[2] = w;
    u.i64[3] = host;
    ynet__irec(in, (uint16_t)YNET_REC_OFFSET, t, &u);
    ynet__refresh(in);
}

/* A stamp of the stream on ysp_rt: 0 when nothing maps it yet. */
static int64_t ynet__map(const ynet_inlet* in, double s) {
    if (in->st.remote.ready && in->have_rorigin) return yrt_fit_map(&in->remote, ynet__ticks(s, in->rorigin_s));
    if (in->have_offset) return yrt_fit_map(&in->local, ynet__ticks(s + (double)in->st.lsl_offset_ns * 1e-9, in->origin_s));
    return 0;
}

static void ynet__event(ynet_inlet* in, double s, int64_t t_read, int type, uint32_t control, uint32_t code,
                        float value, yin_event* e) {
    int64_t t = ynet__map(in, s);
    memset(e, 0, sizeof *e);
    if (t == 0) {
        t = t_read;
        e->unc_us = 65535;
    } else {
        int64_t us = (in->unc_ns + 999) / 1000;
        e->unc_us = (uint16_t)(us > 65535 ? 65535 : us < 1 ? 1 : us);
        if (t > t_read) { t = t_read; in->st.clamped++; }
    }
    e->t = t;
    e->device = in->d.device;
    e->kind = (uint8_t)in->d.kind;
    e->type = (uint8_t)type;
    e->control = control;
    e->code = code;
    e->value = value;
    e->flags = (uint8_t)YIN_DEVTICKS;
    e->ticks = (uint32_t)(uint64_t)(int64_t)(s * 1e6);
    e->aux = (float)in->seq_no;
}

static uint32_t ynet__number(const char* s) {
    uint64_t v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFull) return 0;
    }
    return (uint32_t)v;
}

/* Gaps between stamps of a regular stream. */
static void ynet__gap_check(ynet_inlet* in, double s, int64_t t) {
    if (in->have_last_ts && in->period_s > 0) {
        double d = s - in->last_ts;
        if (d > 1.5 * in->period_s) {
            yrt_payload u;
            uint64_t miss = (uint64_t)(d / in->period_s + 0.5) - 1u;
            in->st.gaps++;
            in->st.missing += miss;
            memset(&u, 0, sizeof u);
            u.i64[0] = (int64_t)(in->last_ts * 1e9);
            u.i64[1] = (int64_t)(s * 1e9);
            u.u64[2] = miss;
            ynet__irec(in, (uint16_t)YNET_REC_STREAM_GAP, t, &u);
        } else if (d < 0) {
            in->st.backward++;
        }
    }
    in->last_ts = s;
    in->have_last_ts = 1;
}

static void ynet__floats(ynet_inlet* in, int n, int64_t t_read) {
    int ch = in->st.channels, i, k;
    int64_t tt[256];
    for (i = 0; i < n; i++) {
        double s = in->ts[i];
        const float* x = in->data + (size_t)i * (size_t)ch;
        in->seq_no++;
        ynet__gap_check(in, s, t_read);
        for (k = 0; k < in->d.n_edges; k++) {
            const ynet_edge* e = &in->d.edges[k];
            float v = x[e->channel];
            int hi = in->edge_hi[k];
            int now_hi = hi;
            if (v > e->level + 0.5f * e->hysteresis) now_hi = 1;
            else if (v < e->level - 0.5f * e->hysteresis) now_hi = 0;
            else if (hi < 0) now_hi = v >= e->level;
            if (hi >= 0 && now_hi != hi && in->d.sink) {
                yin_event ev;
                ynet__event(in, s, t_read, now_hi ? YIN_PRESS : YIN_RELEASE,
                            e->control ? e->control : (uint32_t)e->channel + 1u, 0u, v, &ev);
                in->d.sink(in->d.sink_ctx, &ev);
                in->st.events++;
            }
            in->edge_hi[k] = now_hi;
        }
    }
    in->st.samples += (uint64_t)n;
    if (in->d.stream) {
        /* the map once per block of 256: the fits do not change inside a chunk */
        for (i = 0; i < n; i += 256) {
            int m = n - i < 256 ? n - i : 256, kept;
            for (k = 0; k < m; k++) {
                int64_t t = ynet__map(in, in->ts[i + k]);
                if (t == 0) t = t_read;
                else if (t > t_read) { t = t_read; in->st.clamped++; }
                tt[k] = t;
            }
            kept = ynet__stream_put(in->d.stream, tt, in->ts + i, in->data + (size_t)i * (size_t)ch, m, ch);
            if (kept < m) in->st.dropped += (uint64_t)(m - kept);
        }
        if (in->st.dropped && t_read - in->last_drop_rec >= 1000000000) {
            yrt_payload u;
            memset(&u, 0, sizeof u);
            u.u64[0] = in->st.dropped;
            ynet__irec(in, (uint16_t)YNET_REC_DROP, t_read, &u);
            in->last_drop_rec = t_read;
        }
    }
}

static void ynet__marker(ynet_inlet* in, double s, int64_t t_read, uint32_t code, const char* text) {
    yin_event ev;
    in->seq_no++;
    ynet__event(in, s, t_read, YIN_PRESS, 0u, code, 1.0f, &ev);
    in->st.samples++;
    if (in->d.sink) { in->d.sink(in->d.sink_ctx, &ev); in->st.events++; }
    if (text) {
        if (in->d.text_sink) in->d.text_sink(in->d.text_ctx, &ev, text);
        ynet__text(in->d.ring, in->d.device, t_read, "in", text);
    }
}

/* Resolve, identify, subscribe. */
static void ynet__try_open(ynet_inlet* in, int64_t t) {
    void* found[4];
    int n, i, fmt, ch;
    int32_t ec = 0;
    yrt_payload u;
    double rate;
    in->next_try = t + in->d.retry_ns;
    in->st.resolves++;
    /* minimum 2: wait the whole resolve so a second match shows up */
    n = in->d.lib->resolve_bypred(found, 4, in->pred, 2, (double)in->d.resolve_ns / 1e9);
    if (n <= 0) return;
    if (n > 1) {
        in->st.ambiguous++;
        if (!in->reported_ambiguous) {
            char msg[40];
            snprintf(msg, sizeof msg, "%d streams match", n);
            ynet__text(in->d.ring, in->d.device, ynet__inow(in), "ambiguous", msg);
            in->reported_ambiguous = 1;
        }
        for (i = 0; i < n && i < 4; i++) in->d.lib->destroy_streaminfo(found[i]);
        return;
    }
    in->info = found[0];
    fmt = in->d.lib->get_channel_format(in->info);
    ch = in->d.lib->get_channel_count(in->info);
    rate = in->d.lib->get_nominal_srate(in->info);
    ynet__info_key(in->d.lib, in->info, &in->st.found);
    ynet__clean(in->st.uid, sizeof in->st.uid, in->d.lib->get_uid(in->info));
    if ((in->d.format && fmt != in->d.format) || (in->d.channels && ch != in->d.channels) ||
        (fmt != YNET_FLOAT32 && fmt != YNET_STRING && fmt != YNET_INT32) || ch < 1 || ch > YNET_MAX_CHANNELS ||
        (fmt != YNET_FLOAT32 && ch != 1)) {
        char msg[64];
        snprintf(msg, sizeof msg, "format %d, %d channels", fmt, ch);
        ynet__text(in->d.ring, in->d.device, ynet__inow(in), "wrong", msg);
        /* under the lock: ynet_inlet_wait_running() writes the message too */
        ynet__lock(ynet__imx(in));
        snprintf(in->err, sizeof in->err, "ysp_net: the stream at %.100s has format %d and %d channels", in->d.key, fmt, ch);
        ynet__unlock(ynet__imx(in));
        ynet__drop_inlet(in);
        ynet__set_state(in, YNET_FAILED, YNET_WHY_WRONG_STREAM);
        return;
    }
    for (i = 0; i < in->d.n_edges; i++)
        if (in->d.edges[i].channel >= ch) {
            ynet__lock(ynet__imx(in));
            snprintf(in->err, sizeof in->err, "ysp_net: edge %d reads channel %d of %d", i, in->d.edges[i].channel, ch);
            ynet__unlock(ynet__imx(in));
            ynet__drop_inlet(in);
            ynet__set_state(in, YNET_FAILED, YNET_WHY_WRONG_STREAM);
            return;
        }
    in->inlet = in->d.lib->create_inlet(in->info, in->d.lsl_max_buflen, 0, 0);
    if (!in->inlet) { ynet__drop_inlet(in); return; }
    in->d.lib->open_stream(in->inlet, (double)in->d.resolve_ns / 1e9 + 1.0, &ec);
    if (ec != 0) { ynet__drop_inlet(in); return; }
    in->st.format = fmt;
    in->st.channels = ch;
    in->st.rate_hz = rate;
    in->period_s = rate > 0 ? 1.0 / rate : 0.0;
    in->have_last_ts = 0;
    for (i = 0; i < YNET_MAX_EDGES; i++) in->edge_hi[i] = -1;
    /* a new open: the sender may be another process on another clock */
    ynet__fit_init(&in->remote, YRT_FIT_UNBIASED, in->d.device, 0, in->d.ring);
    in->have_rorigin = 0;
    in->have_offset = 0;
    in->last_remote = 0;
    in->st.opens++;
    t = ynet__inow(in);
    memset(&u, 0, sizeof u);
    u.u32[0] = (uint32_t)fmt;
    u.u32[1] = (uint32_t)ch;
    u.f64[1] = rate;
    ynet__irec(in, (uint16_t)YNET_REC_STREAM, t, &u);
    ynet__text(in->d.ring, in->d.device, t, "name", in->st.found.name);
    ynet__text(in->d.ring, in->d.device, t, "type", in->st.found.type);
    ynet__text(in->d.ring, in->d.device, t, "source", in->st.found.source_id);
    ynet__text(in->d.ring, in->d.device, t, "host", in->st.found.hostname);
    ynet__text(in->d.ring, in->d.device, t, "uid", in->st.uid);
    /* the first correction blocks until liblsl has one (several ms) */
    ynet__correct(in, 2.0);
    in->next_corr = ynet__inow(in) + in->d.correct_ns;
    if (in->state == YNET_LOST) {
        memset(&u, 0, sizeof u);
        u.i64[0] = in->lost_at;
        u.i64[1] = ynet__inow(in);
        ynet__irec(in, (uint16_t)YNET_REC_GAP, u.i64[1], &u);
    }
    in->st.last_rx_ns = ynet__inow(in);
    in->rate_t0 = in->st.last_rx_ns;
    in->rate_n0 = in->st.samples;
    ynet__set_state(in, YNET_RUNNING, YNET_WHY_OPENED);
}

static void ynet__pull(ynet_inlet* in, int64_t timeout_ns) {
    int32_t ec = 0;
    int64_t t_read, t_done;
    double tmo = (double)timeout_ns / 1e9;     /* liblsl takes seconds */
    int n = 0;
    if (in->st.format == YNET_FLOAT32) {
        unsigned long cap = (unsigned long)(YNET__CHUNK_FLOATS / in->st.channels);
        unsigned long got;
        if (cap > YNET__CHUNK_SAMPLES) cap = YNET__CHUNK_SAMPLES;
        got = in->d.lib->pull_chunk_f(in->inlet, in->data, in->ts, cap * (unsigned long)in->st.channels, cap, tmo, &ec);
        t_read = ynet__inow(in);
        n = (int)(got / (unsigned long)in->st.channels);
        if (n > 0) ynet__floats(in, n, t_read);
    } else if (in->st.format == YNET_STRING) {
        char* s = NULL;
        double ts = in->d.lib->pull_sample_str(in->inlet, &s, 1, tmo, &ec);
        t_read = ynet__inow(in);
        if (ts != 0.0 && ec == 0) {
            n = 1;
            ynet__marker(in, ts, t_read, ynet__number(s), s ? s : "");
        }
        if (s) in->d.lib->destroy_string(s);   /* allocated even on a timeout */
    } else {
        int32_t v = 0;
        double ts = in->d.lib->pull_sample_i(in->inlet, &v, 1, tmo, &ec);
        t_read = ynet__inow(in);
        if (ts != 0.0 && ec == 0) {
            n = 1;
            ynet__marker(in, ts, t_read, (uint32_t)v, NULL);
        }
    }
    t_done = ynet__inow(in);
    if (n > 0) {
        in->st.chunks++;
        in->st.last_rx_ns = t_read;
        in->st.busy_ns += t_done - t_read;
    }
    if (ec == -2) { ynet__lose(in, YNET_WHY_DISCONNECTED); return; }   /* lsl_lost_error */
    if (in->d.silent_ns > 0 && in->period_s > 0 && t_done - in->st.last_rx_ns > in->d.silent_ns)
        ynet__lose(in, YNET_WHY_SILENT);
}

static void ynet__step(ynet_inlet* in, int64_t timeout_ns) {
    int64_t t = ynet__inow(in);
    if (t >= in->next_local) {
        ynet__local_pair(in);
        in->next_local = t + 1000000000;
    }
    if (in->state == YNET_OPENING || in->state == YNET_LOST) {
        if (t >= in->next_try) ynet__try_open(in, t);
        else if (!in->d.now) {
            int64_t w = in->next_try - t;
            (void)yrt_sleep_ns(w < timeout_ns ? w : timeout_ns);
        }
    } else if (in->state == YNET_RUNNING) {
        if (t >= in->next_corr) {
            ynet__correct(in, 0.1);
            in->next_corr = t + in->d.correct_ns;
        }
        ynet__pull(in, timeout_ns);
        t = ynet__inow(in);
        if (t - in->rate_t0 >= 1000000000) {
            in->st.measured_hz = (double)(in->st.samples - in->rate_n0) * 1e9 / (double)(t - in->rate_t0);
            in->rate_t0 = t;
            in->rate_n0 = in->st.samples;
        }
    } else if (!in->d.now) {
        (void)yrt_sleep_ns(timeout_ns);   /* FAILED: nothing to do */
    }
    ynet__publish(in);
}

static bool ynet__stopping(ynet_inlet* in) {
    int s;
    ynet__lock(ynet__imx(in));
    s = in->stop;
    ynet__unlock(ynet__imx(in));
    return s != 0;
}

#if defined(_WIN32)
static DWORD WINAPI ynet__thread(LPVOID arg)
#else
static void* ynet__thread(void* arg)
#endif
{
    ynet_inlet* in = (ynet_inlet*)arg;
    if (!in->d.no_elevate) (void)yrt_thread_elevate(NULL);
    while (!ynet__stopping(in)) ynet__step(in, in->d.read_timeout_ns);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

YNET_API bool ynet_inlet_open(ynet_inlet* in, const ynet_inlet_desc* desc) {
    int i;
    if (!in) return false;
    memset(in, 0, sizeof *in);
#define YNET__IFAIL(...) do { snprintf(in->err, sizeof in->err, "ysp_net: " __VA_ARGS__); return false; } while (0)
    if (!desc) YNET__IFAIL("NULL desc");
    if (!ynet__lib_ok(desc->lib)) YNET__IFAIL("desc.lib is not an open liblsl (ynet_lsl_open)");
    if (!desc->role || !desc->role[0]) YNET__IFAIL("desc.role is required");
    if (!desc->key || !ynet_key_parse(desc->key, &in->k) || ynet_key_predicate(&in->k, in->pred, sizeof in->pred) < 0)
        YNET__IFAIL("desc.key must be lsl:<name>:<type>:<source_id>:<hostname> with one field set, "
                    "no quote or control character (MATCH KEYS)");
    if (desc->device == 0) YNET__IFAIL("desc.device must be > 0 (the role's index)");
    if (desc->format != 0 && desc->format != YNET_STRING && desc->format != YNET_INT32 && desc->format != YNET_FLOAT32)
        YNET__IFAIL("desc.format: 0, YNET_STRING, YNET_INT32 or YNET_FLOAT32");
    if (desc->channels < 0 || desc->channels > YNET_MAX_CHANNELS) YNET__IFAIL("desc.channels: 0 to 64");
    if (desc->kind < 0 || desc->kind > 15) YNET__IFAIL("desc.kind is not a yin_kind");
    if (desc->n_edges < 0 || desc->n_edges > YNET_MAX_EDGES) YNET__IFAIL("desc.n_edges: 0 to 8");
    for (i = 0; i < desc->n_edges; i++)
        if (desc->edges[i].channel < 0 || desc->edges[i].channel >= YNET_MAX_CHANNELS || desc->edges[i].hysteresis < 0)
            YNET__IFAIL("desc.edges[%d]: channel 0 to 63, hysteresis >= 0", i);
    if (desc->stream && !desc->stream->rec) YNET__IFAIL("desc.stream is not initialized (ynet_stream_init)");
    if (desc->lsl_max_buflen < 0) YNET__IFAIL("desc.lsl_max_buflen must be >= 0");
    if (strlen(desc->key) >= sizeof in->key_buf) YNET__IFAIL("desc.key: at most 239 characters (YNET_KEY_MAX)");
    if (strlen(desc->role) >= sizeof in->role_buf) YNET__IFAIL("desc.role: at most 31 characters (YNET_ROLE_MAX)");
    in->d = *desc;
    /* the reader resolves the key again after every loss: the caller's
     * strings need not outlive this call */
    memcpy(in->key_buf, desc->key, strlen(desc->key) + 1);
    memcpy(in->role_buf, desc->role, strlen(desc->role) + 1);
    in->d.key = in->key_buf;
    in->d.role = in->role_buf;
    if (!in->d.kind) in->d.kind = YIN_KIND_SYNC;
    if (in->d.resolve_ns <= 0) in->d.resolve_ns = 500000000;
    if (in->d.retry_ns <= 0) in->d.retry_ns = 1000000000;
    if (in->d.silent_ns == 0) in->d.silent_ns = 1000000000;
    if (in->d.correct_ns <= 0) in->d.correct_ns = 1000000000;
    if (in->d.max_unc_ns <= 0) in->d.max_unc_ns = 5000000;
    if (in->d.lsl_max_buflen == 0) in->d.lsl_max_buflen = 30;
    if (in->d.read_timeout_ns <= 0) in->d.read_timeout_ns = 10000000;
    ynet__lock_init(ynet__imx(in));
    ynet__fit_init(&in->local, YRT_FIT_BRACKET, 0x10000u + in->d.device, 0, in->d.ring);
    ynet__fit_init(&in->remote, YRT_FIT_UNBIASED, in->d.device, 0, in->d.ring);
    for (i = 0; i < YNET_MAX_EDGES; i++) in->edge_hi[i] = -1;
    in->state = YNET_CLOSED;
    ynet__text(in->d.ring, in->d.device, ynet__inow(in), "role", in->d.role);
    ynet__text(in->d.ring, in->d.device, ynet__inow(in), "inlet", in->d.key);
    ynet__set_state(in, YNET_OPENING, YNET_WHY_START);
    in->next_try = ynet__inow(in);
    in->started = 1;
    ynet__local_pair(in);
    in->next_local = ynet__inow(in) + 1000000000;
    ynet__publish(in);
    if (in->d.manual) return true;
#if defined(_WIN32)
    {
        HANDLE h = CreateThread(NULL, 0, ynet__thread, in, 0, NULL);
        if (h) { memcpy(in->thread_mem.b, &h, sizeof h); in->have_thread = 1; }
    }
#else
    {
        pthread_t th;
        typedef char ynet__thread_fits[sizeof(pthread_t) <= sizeof in->thread_mem ? 1 : -1];
        (void)sizeof(ynet__thread_fits);
        if (pthread_create(&th, NULL, ynet__thread, in) == 0) {
            memcpy(in->thread_mem.b, &th, sizeof th);
            in->have_thread = 1;
        }
    }
#endif
    if (!in->have_thread) {
        ynet__lock_free(ynet__imx(in));
        in->started = 0;
        YNET__IFAIL("the reader thread did not start");
    }
#undef YNET__IFAIL
    return true;
}

YNET_API void ynet_inlet_close(ynet_inlet* in) {
    if (!in || !in->started) return;
    ynet__lock(ynet__imx(in));
    in->stop = 1;
    ynet__unlock(ynet__imx(in));
    if (in->have_thread) {
#if defined(_WIN32)
        HANDLE h;
        memcpy(&h, in->thread_mem.b, sizeof h);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
#else
        pthread_t th;
        memcpy(&th, in->thread_mem.b, sizeof th);
        pthread_join(th, NULL);
#endif
        in->have_thread = 0;
    }
    ynet__drop_inlet(in);
    ynet__set_state(in, YNET_CLOSED, YNET_WHY_STOP);
    ynet__publish(in);
    in->started = 0;
    ynet__lock_free(ynet__imx(in));
}

YNET_API int ynet_inlet_poll(ynet_inlet* in, int64_t timeout_ns) {
    if (!in || !in->started || !in->d.manual) return in ? in->state : YNET_CLOSED;
    ynet__step(in, timeout_ns < 0 ? 0 : timeout_ns);
    return in->state;
}

/* The wait's failure message, under the lock: the reader thread writes the
 * FAILED message there. */
static int ynet__wait_fail(ynet_inlet* in, int rc, int state) {
    ynet__lock(ynet__imx(in));
    if (state != YNET_FAILED || !in->err[0])
        snprintf(in->err, sizeof in->err, "ysp_net: %s: %s (state %s)", in->d.role, ynet_strerror(rc),
                 ynet_state_name(state));
    ynet__unlock(ynet__imx(in));
    return rc;
}

YNET_API int ynet_inlet_wait_running(ynet_inlet* in, int64_t timeout_ns) {
    int64_t end;
    if (!in) return YNET_ERR_STATE;
    if (!in->started) {
        snprintf(in->err, sizeof in->err, "ysp_net: ynet_inlet_wait_running: not open");
        return YNET_ERR_STATE;
    }
    end = ynet__inow(in) + (timeout_ns > 0 ? timeout_ns : 0);
    for (;;) {
        int64_t before = ynet__inow(in), now;
        int s = in->d.manual ? ynet_inlet_poll(in, 0) : ynet_inlet_state(in);
        if (s == YNET_RUNNING) return YNET_RUNNING;
        if (s == YNET_FAILED || s == YNET_CLOSED) return ynet__wait_fail(in, YNET_ERR_STATE, s);
        now = ynet__inow(in);
        if (now >= end) return ynet__wait_fail(in, YNET_ERR_TIMEOUT, s);
        if (in->d.now) {
            /* the caller's clock: a poll that does not move it never will */
            if (now == before) return ynet__wait_fail(in, YNET_ERR_TIMEOUT, s);
        } else {
            (void)yrt_sleep_ns(end - now < 1000000 ? end - now : 1000000);
        }
    }
}

YNET_API int ynet_inlet_state(ynet_inlet* in) {
    int s;
    if (!in || !in->started) return in ? in->state : YNET_CLOSED;
    ynet__lock(ynet__imx(in));
    s = in->snap.state;
    ynet__unlock(ynet__imx(in));
    return s;
}

YNET_API void ynet_inlet_stats(ynet_inlet* in, ynet_stats* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!in) return;
    if (!in->started) { *out = in->snap; return; }
    ynet__lock(ynet__imx(in));
    *out = in->snap;
    ynet__unlock(ynet__imx(in));
}

/* The published fit infos give the maps: host = t0 + (ticks - ticks0) * k. */
static int64_t ynet__info_map(const yrt_fit_info* f, uint64_t ticks) {
    return f->t0_ns + (int64_t)llround((double)((int64_t)ticks - (int64_t)f->ticks0) * f->ns_per_tick);
}

YNET_API int64_t ynet_inlet_map(ynet_inlet* in, double lsl_s) {
    ynet_stats s;
    double ro, lo;
    int64_t r = 0;
    if (!in || !in->started) return 0;
    ynet__lock(ynet__imx(in));
    s = in->snap;
    ro = in->snap_rorigin;
    lo = in->snap_origin;
    ynet__unlock(ynet__imx(in));
    if (s.remote.ready) r = ynet__info_map(&s.remote, ynet__ticks(lsl_s, ro));
    else if (s.corrections && s.local.ready) r = ynet__info_map(&s.local, ynet__ticks(lsl_s + (double)s.lsl_offset_ns * 1e-9, lo));
    return r;
}

YNET_API yin_source ynet_inlet_source(const ynet_inlet* in) {
    yin_source s;
    memset(&s, 0, sizeof s);
    if (!in) return s;
    s.kind = (uint8_t)in->d.kind;
    s.tier = (uint8_t)YIN_TIER_UNKNOWN;
    s.device = in->d.device;
    s.note = "ysp_net: LSL stamps (SDK), mapped by lsl_time_correction and a fit of lsl_local_clock; no loopback";
    return s;
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_NET_IMPLEMENTATION_GUARD */
#endif /* YSP_NET_IMPLEMENTATION */

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
