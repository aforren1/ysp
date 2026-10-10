/* ysp/parallel.h - v0.4.0 - public domain single-header parallel-port library
 *
 *   A small C library for reading and writing the PC parallel port (LPT).
 *   Built for low-latency 8-bit trigger output of the kind used in
 *   psychophysics / EEG / electrophysiology rigs, where a byte written to
 *   the data lines is latched onto a recording system.
 *
 *   Inspired by:
 *     - pyparallel    (https://github.com/pyparallel/pyparallel)
 *     - ppdev-mex     (https://github.com/widmann/ppdev-mex)
 *   and written in the single-header style of the stb / sokol libraries.
 *
 *   REQUIRES ysp/rt.h, its sibling in this repository, beside it. That
 *   header owns the clock, the waits, the scheduling ladder and the
 *   trailing-edge worker; this one owns the port. Copy BOTH files (the
 *   stb_truetype / stb_rect_pack arrangement). Nothing else is needed: the
 *   pair still depends on nothing but the OS.
 *
 *   Targets Windows and Linux. C99 is the floor: it builds as C99, C11 and
 *   C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Copy TWO files into your project, ysp/parallel.h and ysp/rt.h, and keep
 *   them in one ysp/ directory whose parent is on the include path (this
 *   header includes "ysp/rt.h"). Then do this:
 *
 *       #define YSP_PARALLEL_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *   That one file also creates ysp/rt.h's implementation, unless the
 *   translation unit already has it; you never need to define
 *   YSP_RT_IMPLEMENTATION for this header's sake, and defining it anyway
 *   (or implementing ysp/serial.h in the same file) still yields exactly
 *   one copy.
 *
 *       #define YSP_PARALLEL_IMPLEMENTATION
 *       #include "ysp/parallel.h"
 *
 *       ypar_port pp;                                 // zeroed, or closed
 *       ypar_desc desc;                               // zeroed: sensible defaults
 *       memset(&pp, 0, sizeof pp);     // not { 0 }: an enum comes first, and
 *       memset(&desc, 0, sizeof desc); // C++ does not convert 0 to it
 *       if (!ypar_open(&pp, &desc)) {
 *           fprintf(stderr, "open failed: %s\n", ypar_error(&pp));
 *           return 1;
 *       }
 *       if (ypar_write_data(&pp, 0x55) < 0)           // raise a trigger
 *           fprintf(stderr, "write: %s\n", ypar_error(&pp));
 *       if (ypar_pulse(&pp, 0x55, YRT_MS(2)) < 0)     // 0x55 for 2 ms, then 0
 *           fprintf(stderr, "pulse: %s\n", ypar_error(&pp));
 *       ypar_close(&pp);
 *
 *   ---------------------------------------------------------------------
 *   BACKENDS
 *   ---------------------------------------------------------------------
 *   Linux:
 *     YPAR_BACKEND_PPDEV   - the kernel ppdev character device
 *                            (/dev/parport0). The portable, recommended
 *                            path. Setup, once:
 *                              sudo modprobe ppdev        # creates /dev/parportN
 *                              sudo usermod -aG lp $USER  # device access; re-login
 *                            (add `ppdev` to /etc/modules-load.d/ to keep
 *                            it across reboots.) "No such file or directory"
 *                            on open almost always means ppdev is not
 *                            loaded. The `lp` printer driver only holds the
 *                            port while printing; unloading it is rarely
 *                            needed. NOTE: claiming the port (PPCLAIM)
 *                            BLOCKS until the port is free, it does not
 *                            fail. If another process holds it, ypar_open()
 *                            hangs; find the holder with
 *                            `lsof /dev/parport0` before you open.
 *     YPAR_BACKEND_DIRECT  - raw x86 port I/O via outb/inb. Lowest latency,
 *                            but requires root (CAP_SYS_RAWIO) and
 *                            x86/x86_64. You must know the I/O base address
 *                            (/proc/ioports). Legacy ISA bases below 0x400
 *                            use ioperm(), scoped to the 3 registers. PCI/
 *                            PCIe cards live above 0x400, where ioperm()
 *                            cannot reach, so the library uses iopl(3)
 *                            there, which grants the process EVERY I/O
 *                            port; a wrong base address then writes to
 *                            whatever device owns it.
 *
 *   DIRECT IS PER-THREAD. ioperm() and iopl() grant I/O permission to the
 *   CALLING THREAD only, not to the process. With YPAR_BACKEND_DIRECT a
 *   handle is therefore usable from two threads and no others: the thread
 *   that called ypar_open(), and the ysp/rt.h worker thread ypar_open()
 *   started. The worker grants itself permission on its own thread, from
 *   yrt_worker_desc.on_start, which runs there before yrt_worker_open()
 *   returns; so it is safe wherever it was started from, and a REFUSED grant
 *   fails ypar_open() rather than one trigger's trailing edge much later.
 *   outb/inb from any other thread raises
 *   SIGSEGV; there is no error return to check. Call ypar_open(),
 *   ypar_write_data(), ypar_pulse() and the register accessors from one
 *   thread, and use ypar_pulse_async() when you need a trailing edge issued
 *   from elsewhere. This matters most for the Python and MEX bindings, which
 *   are called from whatever thread the host runtime provides: prefer
 *   YPAR_BACKEND_PPDEV there, whose file descriptor is process-wide.
 *
 *   Closing a DIRECT handle revokes I/O permission for the whole thread, not
 *   just for that handle's 3 registers when iopl() was used, and ioperm()
 *   ranges of two handles may overlap. If you open several DIRECT handles on
 *   one thread, closing any of them can leave the others unable to write.
 *   Open one DIRECT handle per thread, or close them all together.
 *
 *   Windows (UNTESTED - compiled in CI but not run on real hardware; see
 *   parallel64 https://github.com/tekktrik/parallel64 for a maintained,
 *   Windows-focused alternative):
 *     YPAR_BACKEND_INPOUT  - loads inpout32.dll / inpoutx64.dll at runtime
 *                            (https://www.highrez.co.uk/downloads/inpout32/)
 *                            and calls its Out32/Inp32 entry points. Modern
 *                            Windows forbids user-mode port I/O, so a kernel
 *                            helper driver such as InpOut is required. The
 *                            DLL self-installs its driver on first use and
 *                            needs administrator rights to do so. Find the
 *                            port's I/O base address in Device Manager (the
 *                            LPT port, then Resources) and pass it as
 *                            desc.base_addr. NOTHING validates that address:
 *                            inpout writes to whatever port you name, and
 *                            0x64 (keyboard controller) or 0xCF8 (PCI
 *                            config) from a typo can hang the machine. The
 *                            DLL is loaded with the CWD excluded from the
 *                            search path, so put it next to the EXECUTABLE
 *                            or in System32, or pass its full path in
 *                            ypar_desc.dll_path. The executable is not
 *                            always yours: under the Python or MEX bindings
 *                            it is python.exe or MATLAB, so a DLL shipped
 *                            beside the binding module is found only via
 *                            dll_path.
 *
 *     Caveat: InpOut is an unmaintained, legacy-signed kernel driver that
 *     grants ring-0 port I/O. On hardened systems (Secure Boot with Memory
 *     Integrity enabled, or the Windows vulnerable-driver blocklist) it may
 *     be refused no matter how it is installed, and there is no maintained
 *     drop-in replacement.
 *
 *     What IS exercised on Windows is the timing underneath: since v0.3 the
 *     waits and the trailing-edge worker are ysp/rt.h's, and
 *     examples/rt/jitter.c measures those on Windows 11. The register writes
 *     on top of them are the part that has never reached hardware.
 *
 *   Trigger use needs a real PCIe/PCI parallel-port card or an onboard LPT.
 *   USB-to-parallel *printer* adapters do not expose a register-level I/O
 *   port on any platform.
 *
 *   YPAR_BACKEND_DEFAULT picks ppdev on Linux and inpout on Windows.
 *
 *   ---------------------------------------------------------------------
 *   REGISTER MODEL
 *   ---------------------------------------------------------------------
 *   A standard (SPP) parallel port exposes three byte-wide registers at
 *   consecutive I/O addresses, base+0/+1/+2:
 *
 *     Data    (base+0, R/W) 8 output bits -> pins 2..9. The trigger byte.
 *     Status  (base+1, R)   5 input bits  -> pins 15,13,12,10,11.
 *     Control (base+2, R/W) 4 output bits -> pins 1,14,16,17 + dir bit.
 *
 *   Some pins are inverted in hardware relative to their register bit
 *   (Busy, nStrobe, nAutoFeed, nSelectIn). This library returns/accepts the
 *   raw register bits, matching pyparallel; use the YPAR_STATUS_* and
 *   YPAR_CONTROL_* masks and mind the hardware inversion noted below.
 *
 *   ypar_open() forces the data direction to forward (output) on every
 *   backend: PPDATADIR 0 on ppdev, YPAR_CONTROL_DIR cleared on direct and
 *   inpout. A bidirectional port left in reverse mode by a previous client
 *   tri-states the data pins, so every trigger byte would float instead of
 *   driving the recorder. Call ypar_set_data_dir(p, true) afterwards if you
 *   really want to read the data lines.
 *
 *   ---------------------------------------------------------------------
 *   PULSES AND TIMING
 *   ---------------------------------------------------------------------
 *   Every wait, every clock read and every scheduling decision here belongs
 *   to ysp/rt.h. Its WAITS and SCHEDULING sections are the reference for
 *   what each one costs and what it is worth, and examples/rt/jitter.c
 *   measures the waits on YOUR rig rather than asserting a number. Pulse
 *   deadlines are yrt_now_ns() times, so a pulse timestamp and any other
 *   yrt_now_ns() timestamp taken in the same process subtract directly.
 *
 *   ypar_pulse(p, value, width_ns) writes value, waits until an absolute
 *   deadline taken right after that write, then writes 0. The wait is
 *   yrt_sleep_until() with YRT_DEFAULT_SPIN_NS: the OS gets the bulk of
 *   the width and the last spin window is spun on the clock (about 1.2 ms
 *   on Windows, where even the high-resolution waitable timer can fire that
 *   late, and 200 us on Linux; ysp/rt.h WAITS says why the two differ).
 *   It blocks the calling thread for the whole width, and that thread is
 *   whatever the caller has, at whatever priority the caller runs: no
 *   reservation, no elevated priority, preemptible by anything. Its
 *   trailing edge is therefore LESS precise than ypar_pulse_async's, which
 *   spins the same window on a thread that climbed the ladder. Same wait,
 *   different thread.
 *
 *   ypar_pulse_async(p, value, width_ns) returns at once. The leading edge is
 *   written on the calling thread, so the trigger ONSET is never delayed by
 *   thread scheduling. One ysp/rt.h deadline worker per port writes the
 *   trailing 0 at an absolute deadline taken right after the onset write.
 *   The worker is started by ypar_open(), so no trial pays for thread
 *   creation and the rung it obtained is final before the first pulse. It
 *   elevates ITSELF as far as this OS and this process's privileges allow:
 *   SCHED_DEADLINE with the reservation in ypar_desc.sched (default 1 ms of
 *   CPU per 10 ms), else SCHED_FIFO 80 (above the kernel's threaded IRQ
 *   handlers, below migration and the watchdog), else a normal thread with
 *   the 50 us timer slack dropped, on Linux; THREAD_PRIORITY_TIME_CRITICAL
 *   on Windows. Asking is not getting: SCHED_DEADLINE needs CAP_SYS_NICE
 *   and is refused as well when the thread's affinity is not the full root
 *   domain (taskset, cpusets, an isolcpus-pinned rig), and the fall back is
 *   silent.
 *
 *   ypar_port.async_policy is the rung the worker got, a yrt_policy.
 *   yrt_policy_name() prints it. Log it: the same jitter histogram means
 *   different things at different rungs.
 *
 *   The worker cannot report a failed trailing-edge write through
 *   ypar_error(), because the message buffer belongs to the calling thread.
 *   It sets ypar_port.async_write_failed instead. The flag is sticky: check
 *   it between trials or at the end of a block, and treat it as "the data
 *   lines may still be asserted, and some trigger offsets are missing". A
 *   YPAR_BACKEND_DIRECT worker that cannot get its own ioperm()/iopl() grant
 *   is NOT one of the cases it covers: that is settled at open time, where
 *   it fails ypar_open() outright.
 *
 *   Every data write on a handle is serialized under one lock this header
 *   owns. It is not ysp/rt.h's worker lock: that one guards the job slot and
 *   is released before the trailing edge runs, which is what lets the
 *   callback take this one. A plain ypar_write_data() or ypar_pulse() issued
 *   while an async pulse is pending CANCELS that pulse's trailing edge: a
 *   write means "hold these lines", and a stale trailing 0 must not undo it.
 *   The cancel is not a race the write can lose, not even against a worker
 *   already inside its final spin window, where yrt_worker_cancel() no
 *   longer bites: both take this header's lock, and the trailing edge writes
 *   only if the pending token it was submitted with is still the current
 *   one. A ypar_pulse_async() issued while one is pending moves the single
 *   trailing edge to the newer off-time, earlier or later. ypar_close()
 *   stops the worker, which runs a pending trailing edge IMMEDIATELY rather
 *   than waiting out the rest of the width, so the lines are never left
 *   asserted.
 *
 *   Timing is best-effort and bounded by OS scheduling. For the tightest
 *   triggers use YPAR_BACKEND_DIRECT on a real-time-tuned kernel with an
 *   isolated CPU. Measure on your rig with a scope or a second recorder,
 *   and record async_policy with the numbers; do not assume.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   ysp/rt.h must sit beside this header. It is included by source, so it
 *   adds nothing to the link line; point -I / /I at the parent of the ysp/
 *   directory holding the pair.
 *
 *   Link -pthread on Linux for the async worker; Windows needs no extra
 *   library. ysp/rt.h BUILDING carries the rest of the POSIX link line,
 *   since it is now the header that reads the clock and raises the thread.
 *
 *   YPAR_NO_THREADS drops threading and ypar_pulse_async entirely, and
 *   defines YRT_NO_THREADS for you so ysp/rt.h's worker goes with it. The
 *   two must agree within a translation unit. If you include ysp/rt.h
 *   BEFORE ysp/parallel.h, define YRT_NO_THREADS yourself alongside
 *   YPAR_NO_THREADS: by then ysp/rt.h has already read the macro. Define
 *   both or neither. YRT_NO_THREADS alone is a compile error here rather
 *   than a ypar_pulse_async() with no worker to call.
 *
 *   Define YPAR_API to override the default `extern` linkage. YRT_API is
 *   separate and belongs to ysp/rt.h.
 *
 *       cc -O2 -pthread -Iinclude -o parallel_trigger examples/parallel/trigger.c
 *       cl /O2 /Iinclude examples\parallel\trigger.c
 *
 *   ---------------------------------------------------------------------
 *   RETURN VALUES AND ERRORS
 *   ---------------------------------------------------------------------
 *   ypar_open() returns bool. Every other call that can fail returns an int:
 *   0 or more is success (a register byte, 0..255, for the reads; 1 or 0
 *   for ypar_get_status_bit()), and a negative value is one of
 *       YPAR_ERR_IO      the OS refused the register access (ppdev ioctl)
 *       YPAR_ERR_CLOSED  the port is not open
 *       YPAR_ERR_ARG     a negative width
 *       YPAR_ERR_ASYNC   no async worker: YPAR_NO_THREADS, or it did not
 *                        start (ypar_error() says why)
 *   which ypar_strerror() names. Test rc < 0. A failing call also writes
 *   ypar_error() before it returns; a call that succeeds leaves the message
 *   alone, so it belongs to the last failure. The async worker never writes
 *   the message: it sets async_write_failed (PULSES AND TIMING). The calls
 *   on one handle come from one thread at a time, as BACKENDS says for
 *   DIRECT; the message buffer is not shared with any other thread.
 *
 *   ---------------------------------------------------------------------
 *   BINDINGS
 *   ---------------------------------------------------------------------
 *   Python (CPython Limited API) and MATLAB/Octave (MEX) bindings live in
 *   the ysp repository under bindings/python/ysp_parallel and bindings/mex.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.4.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          ypar_pulse() and ypar_pulse_async() take int64_t width_ns
 *          instead of uint32_t usec. ypar_write_data(),
 *          ypar_set_data_dir(), ypar_pulse(), ypar_pulse_async(),
 *          ypar_write_control() and ypar_set_control_bit() return 0 or a
 *          negative YPAR_ERR_* code instead of bool; ypar_read_data(),
 *          ypar_read_status() and ypar_read_control() return the byte
 *          (0..255) or a YPAR_ERR_* code instead of a uint8_t with the
 *          failure only in the message; ypar_get_status_bit() returns 1, 0
 *          or a YPAR_ERR_* code, so a failed read is not "line low". New
 *          ypar_strerror(). A failing call writes ypar_error(); a call that
 *          succeeds no longer clears it, so the message belongs to the last
 *          failure. YPAR_VERSION_* macros.
 *   v0.3 - Depends on ysp/rt.h, and its own copies of the monotonic clock,
 *          the sleep-until, the SCHED_DEADLINE/FIFO ladder, the Windows
 *          waitable timers and the worker thread are gone. All three ysp
 *          headers now time against one implementation instead of three.
 *          ypar_sched_deadline is a typedef of yrt_sched_deadline and
 *          ypar_async_policy of yrt_policy. The YPAR_ASYNC_* and
 *          YPAR_DEFAULT_RT_* names survive as aliases, but the policy
 *          VALUES changed: yrt_policy carries a macOS rung
 *          (YRT_POLICY_TIME_CONSTRAINT = 2) that this header never
 *          returns, so FIFO, TIME_CRITICAL and NORMAL each moved up by one.
 *          Code comparing names is unaffected; code that stored, indexed or
 *          wire-formatted the integers must be rebuilt. YPAR_NO_THREADS now
 *          implies YRT_NO_THREADS. ypar_open() validates desc.sched on
 *          Windows too, where it used to be ignored.
 *          On YPAR_BACKEND_DIRECT the worker thread's own ioperm()/iopl()
 *          grant runs from yrt_worker_desc.on_start (ysp/rt.h v0.2), which
 *          executes on the worker thread before yrt_worker_start()
 *          returns. A refused grant therefore fails ypar_open() as it did in
 *          v0.2, instead of waiting to show up as async_write_failed on the
 *          first trailing edge. (An earlier v0.3 draft, before ysp/rt.h had
 *          that hook, did the latter.)
 *   v0.2 - ypar_open() forces the data direction to forward on every
 *          backend, so a port left in reverse mode by a previous client no
 *          longer makes every trigger float. New ypar_desc.dll_path lets a
 *          binding name the inpout DLL explicitly (the loader searches the
 *          EXECUTABLE's directory, which is python.exe or MATLAB for the
 *          bindings). New ypar_port.async_write_failed reports a failed
 *          trailing-edge write from the async worker. The DIRECT backend's
 *          per-thread I/O permission is now granted on the worker thread
 *          itself, and the per-thread rules are documented. Windows creates
 *          ypar_pulse()'s waitable timer in ypar_open(), not on first pulse.
 *   v0.1 - first release.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_PARALLEL_H_INCLUDED
#define YSP_PARALLEL_H_INCLUDED

/* The version of this header, for a binding's __version__ and for a log line.
 * The string always matches the three numbers. */
#define YPAR_VERSION_MAJOR  0
#define YPAR_VERSION_MINOR  4
#define YPAR_VERSION_PATCH  0
#define YPAR_VERSION_STRING "0.4.0"

/* Feature-test macro for the Linux implementation. This header's own needs
 * are modest, but the ysp/rt.h implementation it pulls in below wants
 * clock_nanosleep() and mlockall(), and ysp/rt.h's own feature-macro block
 * lives inside its include guard: by the time this file asks for that
 * implementation, the guard may already have been passed by an earlier
 * include, and the block can no longer fire. So the macro is set HERE too,
 * before the first system header. Both headers ask for the SAME macro on
 * Linux and both stand down once it is set, so whichever of them comes first
 * settles it and the include order does not matter.
 *
 * If you include other system headers before this one in your implementation
 * file, define _DEFAULT_SOURCE yourself. Prefer it to _POSIX_C_SOURCE: glibc
 * treats an explicit _POSIX_C_SOURCE (or _XOPEN_SOURCE, or -std=c11 without
 * -std=gnu11) as "strict POSIX", which switches _DEFAULT_SOURCE OFF for the
 * WHOLE translation unit, so strdup(), usleep(), M_PI and friends vanish from
 * the rest of your implementation file as well. If you must set
 * _POSIX_C_SOURCE, set _DEFAULT_SOURCE alongside it; glibc then keeps both. */
#if defined(YSP_PARALLEL_IMPLEMENTATION) && defined(__linux__) && \
    !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE) && !defined(_DEFAULT_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif

/* ysp/rt.h reads YRT_NO_THREADS when IT is first included, which may be
 * from another header entirely, so the coupling has to be declared before the
 * include below and cannot be repaired afterwards. */
#if defined(YPAR_NO_THREADS) && !defined(YRT_NO_THREADS)
    #define YRT_NO_THREADS
#endif
#if defined(YRT_NO_THREADS) && !defined(YPAR_NO_THREADS)
    #error "ysp_parallel: YRT_NO_THREADS without YPAR_NO_THREADS. \
ypar_pulse_async needs ysp/rt.h's worker. Define both or neither (defining \
YPAR_NO_THREADS first defines YRT_NO_THREADS for you, but only if this \
header is reached before ysp/rt.h)."
#endif

/* A hard dependency, not an option: yrt_policy and yrt_sched_deadline are
 * part of this header's public types, and ysp/rt.h owns every wait below. Keep
 * the two files together. */
#include "ysp/rt.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YPAR_API
#define YPAR_API extern
#endif

/* Negative results of every call but ypar_open(); see RETURN VALUES. */
#define YPAR_ERR_IO     (-1)
#define YPAR_ERR_CLOSED (-2)
#define YPAR_ERR_ARG    (-3)
#define YPAR_ERR_ASYNC  (-4)

/* Default I/O base addresses of the legacy ISA LPT ports. Add-in PCI/PCIe
 * cards usually live elsewhere; check your system (Linux: /proc/ioports;
 * Windows: Device Manager -> port -> Resources). */
#define YPAR_LPT1 0x378
#define YPAR_LPT2 0x278
#define YPAR_LPT3 0x3BC

/* Status register (base+1) bit masks. Read-only.
 * NOTE: Busy (bit 7) is inverted in hardware: the bit reads 0 while the
 * physical Busy line is asserted high. */
#define YPAR_STATUS_BUSY    0x80  /* pin 11, hardware-inverted */
#define YPAR_STATUS_ACK     0x40  /* pin 10, nAck             */
#define YPAR_STATUS_PAPER   0x20  /* pin 12, paper-out        */
#define YPAR_STATUS_SELECT  0x10  /* pin 13, select-in        */
#define YPAR_STATUS_ERROR   0x08  /* pin 15, nError/nFault    */

/* Control register (base+2) bit masks. Read/write.
 * NOTE: Strobe, AutoFeed and SelectIn are inverted in hardware relative to
 * their register bit; Init is not. YPAR_CONTROL_DIR=1 switches an enhanced
 * (PS/2/bidirectional) port's data lines to input. */
#define YPAR_CONTROL_STROBE   0x01  /* pin 1,  nStrobe,   hardware-inverted */
#define YPAR_CONTROL_AUTOFEED 0x02  /* pin 14, nAutoFeed, hardware-inverted */
#define YPAR_CONTROL_INIT     0x04  /* pin 16, nInit                        */
#define YPAR_CONTROL_SELECT   0x08  /* pin 17, nSelectIn, hardware-inverted */
#define YPAR_CONTROL_DIR      0x20  /* data direction: 1 = input/reverse    */

typedef enum ypar_backend {
    YPAR_BACKEND_DEFAULT = 0, /* ppdev on Linux, inpout on Windows */
    YPAR_BACKEND_PPDEV,       /* Linux kernel ppdev character device */
    YPAR_BACKEND_DIRECT,      /* Linux raw x86 port I/O (ioperm)     */
    YPAR_BACKEND_INPOUT       /* Windows inpout32/inpoutx64 DLL      */
} ypar_backend;

/* Real-time scheduling reservation for the async-pulse worker thread on Linux
 * (SCHED_DEADLINE). This is yrt_sched_deadline under an old name: the type
 * is shared with ysp/rt.h and ysp/serial.h, so one reservation struct can be
 * handed to all three. Times are in nanoseconds. They do NOT set the pulse
 * width (that is clock-driven); they govern how quickly and predictably the
 * worker gets the CPU when it wakes to write the trailing edge, which is edge
 * jitter.
 *
 *   runtime_ns  - CPU budget guaranteed (and capped) per period.
 *   deadline_ns - relative deadline; smaller = higher EDF priority = lower
 *                 wake latency, but harder to admit alongside other RT tasks.
 *   period_ns   - replenishment interval; reserved bandwidth = runtime/period.
 *
 * Leave all three zero for the library defaults below. If you set any field,
 * require 0 < runtime_ns <= deadline_ns <= period_ns (a zero period_ns is
 * taken as "same as deadline_ns"); ypar_open() rejects any other combination
 * with an error. A valid reservation that the kernel cannot admit (or any
 * setup on Windows) transparently falls back to SCHED_FIFO / normal. */
typedef yrt_sched_deadline ypar_sched_deadline;

#define YPAR_DEFAULT_RT_RUNTIME_NS  YRT_DEFAULT_RT_RUNTIME_NS  /*  1 ms */
#define YPAR_DEFAULT_RT_DEADLINE_NS YRT_DEFAULT_RT_DEADLINE_NS /* 10 ms */
#define YPAR_DEFAULT_RT_PERIOD_NS   YRT_DEFAULT_RT_PERIOD_NS   /* 10 ms */

/* Scheduling policy the async-pulse worker actually obtained: yrt_policy,
 * the one ladder every ysp header reports against. Requesting SCHED_DEADLINE
 * is not getting it: the kernel refuses it without CAP_SYS_NICE, and also when
 * the thread's CPU affinity is not the full root domain (taskset, cpusets, an
 * isolcpus-pinned experiment process), and the library then falls back. Read
 * ypar_port.async_policy after ypar_open(), print it with yrt_policy_name(),
 * and log it next to your timing data; jitter numbers without it are
 * unlabeled.
 *
 * The YPAR_ASYNC_* spellings below are kept so v0.2 code still compiles. They
 * name the same rungs, but their VALUES changed in v0.3: yrt_policy has a
 * macOS rung at 2 that this header, being Windows and Linux only, never
 * returns. Compare the names, not the integers, and rebuild anything that
 * stored the old ones. */
typedef yrt_policy ypar_async_policy;

#define YPAR_ASYNC_NONE          YRT_POLICY_NONE          /* no worker      */
#define YPAR_ASYNC_DEADLINE      YRT_POLICY_DEADLINE      /* Linux          */
#define YPAR_ASYNC_FIFO          YRT_POLICY_FIFO          /* Linux          */
#define YPAR_ASYNC_TIME_CRITICAL YRT_POLICY_TIME_CRITICAL /* Windows        */
#define YPAR_ASYNC_NORMAL        YRT_POLICY_NORMAL        /* nothing granted*/

/* Open description. Zero-initialize it and only set what you need; missing
 * fields fall back to sensible defaults inside ypar_open(). */
typedef struct ypar_desc {
    ypar_backend backend;   /* default: YPAR_BACKEND_DEFAULT          */
    const char*  device;    /* ppdev path, default "/dev/parport0"    */
    uint16_t     base_addr; /* direct/inpout base, default YPAR_LPT1   */
    bool         exclusive; /* ppdev: claim the port exclusively (PPEXCL) */
    ypar_sched_deadline sched; /* async-worker RT params; all-zero = defaults */
    /* Windows/inpout: full path of the inpout DLL to load, e.g.
     * "C:\\rig\\inpoutx64.dll". NULL (the default) searches the executable's
     * directory, System32 and AddDllDirectory() entries, never the current
     * directory. A binding must usually set this, because the executable is
     * python.exe or MATLAB, not the module the DLL ships beside. Ignored on
     * Linux. */
    const char*  dll_path;
} ypar_desc;

/* Port handle. Allocate it (stack/heap), pass to ypar_open(), and treat the
 * fields as opaque. */
typedef struct ypar_port {
    ypar_backend backend;
    uint16_t     base_addr;
    bool         is_open;
    char         error[256];
#if defined(_WIN32)
    void* dll;     /* HMODULE of the inpout DLL */
    void (*out_fn)(void); /* Out32 entry point, generic function pointer: ISO C
                           * has no object-to-function pointer conversion   */
    void (*inp_fn)(void); /* Inp32 entry point                              */
#else
    int  fd;       /* ppdev file descriptor (-1 if direct/closed) */
    bool claimed;  /* ppdev port claimed                          */
    bool direct;   /* using ioperm()+outb/inb                     */
#endif
    void* async;   /* async-pulse block (yrt_worker + data lock), started by
                    * ypar_open(), or NULL                                    */
    ypar_sched_deadline sched; /* effective async-worker RT params (Linux)    */
    ypar_async_policy async_policy; /* rung the worker obtained; final once
                                     * ypar_open() returns                     */
    /* Non-zero once an async trailing-edge write has failed on this handle.
     * The worker must not touch p->error (that buffer belongs to the calling
     * thread), so this flag is how a dropped trigger offset becomes visible.
     * Sticky: ypar_open() clears it and nothing else does, so poll it between
     * trials or at the end of a block. Read it, do not write it. */
    int async_write_failed;
} ypar_port;

/* One detected parallel port, as returned by ypar_list_ports(). */
typedef struct ypar_port_info {
    char         name[64];  /* openable identifier: ppdev path (Linux) or
                             * "LPTn" (Windows). Pass to ypar_desc.device on
                             * Linux; on Windows it is informational. */
    ypar_backend backend;   /* backend this entry was discovered for */
    uint16_t     base_addr; /* I/O base if known, else 0 */
} ypar_port_info;

/* --- discovery ---------------------------------------------------------- */

/* Enumerate parallel ports the system exposes, without opening any. Writes up
 * to `max` entries into `out` (pass out=NULL/max=0 to just count) and returns
 * the total number found, which may exceed `max`.
 *
 *   Linux:   scans /dev/parport0../dev/parport15 (the ppdev nodes); entries
 *            carry the device path and YPAR_BACKEND_PPDEV. For raw DIRECT
 *            access, find base addresses in /proc/ioports instead.
 *   Windows: lists LPT* DOS devices via QueryDosDevice; entries carry the name
 *            and YPAR_BACKEND_INPOUT with base_addr 0 (supply the real base
 *            address yourself via ypar_desc.base_addr).
 *
 * Typical use:
 *   int n = ypar_list_ports(NULL, 0);
 *   ypar_port_info* v = malloc(n * sizeof *v);
 *   ypar_list_ports(v, n);
 */
YPAR_API int ypar_list_ports(ypar_port_info* out, int max);

/* --- lifecycle ---------------------------------------------------------- */

/* Open a parallel port per `desc` (NULL = all defaults). Returns true on
 * success; on failure returns false and leaves a message in ypar_error().
 * `p` must be zeroed or closed: a fresh handle has no valid is_open flag, so
 * ypar_open() cannot detect and close a still-open port for you. Opening an
 * open handle leaks its descriptor and orphans its async worker.
 *
 * Forces the data direction to forward (output) so a port left in reverse
 * mode by a previous client does not tri-state every trigger byte; see
 * REGISTER MODEL.
 *
 * THREAD AFFINITY (YPAR_BACKEND_DIRECT only): ioperm()/iopl() grant I/O
 * permission to the CALLING THREAD, not to the process. The handle is usable
 * only from this thread and from the async worker started here, which grants
 * itself permission on its own thread while this call waits for it; if that
 * is refused, THIS CALL FAILS and the port is closed again. A write from any
 * other thread raises SIGSEGV rather than returning an error. Bindings called
 * from arbitrary host threads (Python, MEX) should use YPAR_BACKEND_PPDEV,
 * whose descriptor is process-wide. See BACKENDS. */
YPAR_API bool ypar_open(ypar_port* p, const ypar_desc* desc);

/* Release and close the port. Safe to call on a zeroed or already-closed
 * handle.
 *
 * YPAR_BACKEND_DIRECT: this drops the calling thread's I/O permission, which
 * is per-thread and not per-handle, so closing one DIRECT handle can leave
 * another open one on the same thread unable to write. See BACKENDS. */
YPAR_API void ypar_close(ypar_port* p);

/* The message of the last failure on this handle ("" if none). */
YPAR_API const char* ypar_error(const ypar_port* p);

/* Static name of a YPAR_ERR_* code ("ok" for values >= 0). */
YPAR_API const char* ypar_strerror(int code);

/* True if the handle currently owns an open port. */
YPAR_API bool ypar_is_open(const ypar_port* p);

/* --- data register (base+0) -------------------------------------------- */

/* Write the 8 data bits. This is the trigger-output workhorse.
 *
 * YPAR_BACKEND_DIRECT: call it from the thread that called ypar_open(). I/O
 * permission on Linux is per-thread, so outb() from another thread raises
 * SIGSEGV instead of failing; this function cannot detect that for you. The
 * ppdev and inpout backends have no such restriction. See BACKENDS.
 * Returns 0 or a YPAR_ERR_* code. */
YPAR_API int ypar_write_data(ypar_port* p, uint8_t value);

/* Read the data register back. On a forward (output) port this returns the
 * last value latched; on a bidirectional port in input mode it reads the
 * external lines. Returns the byte (0..255) or a YPAR_ERR_* code. */
YPAR_API int ypar_read_data(ypar_port* p);

/* Set data-line direction on a bidirectional port: input=true switches the
 * data pins to read mode, input=false (default) drives them as outputs.
 * Returns 0 or a YPAR_ERR_* code. */
YPAR_API int ypar_set_data_dir(ypar_port* p, bool input);

/* Write `value` to the data lines, hold for width_ns nanoseconds, then write
 * 0. The classic fixed-width trigger pulse. It blocks the caller for the
 * width, waiting with yrt_sleep_until() and YRT_DEFAULT_SPIN_NS on the
 * CALLER's thread at the caller's priority, so its trailing edge is less
 * precise than ypar_pulse_async's. Timing is best-effort and bounded by OS
 * scheduling; expect microsecond-to-millisecond jitter on a non-RT kernel,
 * and measure with examples/rt/jitter.c. Returns 0 or a YPAR_ERR_* code
 * (YPAR_ERR_ARG for a negative width, before any write). */
YPAR_API int ypar_pulse(ypar_port* p, uint8_t value, int64_t width_ns);

/* Non-blocking pulse. Writes `value` to the data lines immediately on the
 * calling thread (so the trigger ONSET is not delayed by thread wake-up),
 * then returns at once; a dedicated real-time worker thread writes the
 * trailing 0 after width_ns nanoseconds.
 *
 * The worker is a ysp/rt.h deadline worker, started by ypar_open() (if that
 * failed, the first call here retries and reports; on YPAR_BACKEND_DIRECT
 * there is nothing to retry, because a worker that failed to start would
 * have failed ypar_open() too). It elevates itself for
 * low timing jitter: SCHED_DEADLINE (falling back to SCHED_FIFO, then normal)
 * on Linux, THREAD_PRIORITY_TIME_CRITICAL on Windows, and waits with
 * yrt_sleep_until(). Acquiring a real-time policy needs privileges
 * (CAP_SYS_NICE / running elevated); ypar_pulse_async still works without
 * them, just with more jitter. p->async_policy reports the rung obtained.
 *
 * If a new async pulse is issued while one is still pending, the most
 * recently requested off-time governs the single trailing edge (the data
 * register is shared, so overlapping triggers cannot carry distinct values
 * simultaneously); the worker re-evaluates immediately, whether the new
 * deadline falls earlier or later than the old one. A ypar_write_data() or
 * ypar_pulse() while a pulse is pending cancels its trailing edge.
 *
 * A trailing edge that fails to reach the hardware cannot be reported here:
 * it happens after this call returned, on a thread that must not touch the
 * error buffer. It sets ypar_port.async_write_failed instead. On
 * YPAR_BACKEND_DIRECT that includes the worker thread being refused its own
 * I/O permission, which is per-thread and can only be asked for there.
 *
 * Returns 0, or YPAR_ERR_ASYNC if the worker could not be started (e.g.
 * built with YPAR_NO_THREADS), YPAR_ERR_CLOSED, YPAR_ERR_ARG for a negative
 * width or YPAR_ERR_IO; ypar_error() says more. With -DYPAR_NO_THREADS it
 * always returns YPAR_ERR_ASYNC and writes nothing. */
YPAR_API int ypar_pulse_async(ypar_port* p, uint8_t value, int64_t width_ns);

/* --- status register (base+1, read-only) ------------------------------- */

/* Read the raw status byte. Combine with YPAR_STATUS_* masks. Returns the
 * byte (0..255) or a YPAR_ERR_* code. */
YPAR_API int ypar_read_status(ypar_port* p);

/* Test a single status line (raw register bit): 1 set, 0 clear, or a
 * YPAR_ERR_* code. Not a bool: a status line is often a response input,
 * and a failed read must not look like "not pressed". */
YPAR_API int ypar_get_status_bit(ypar_port* p, uint8_t mask);

/* --- control register (base+2) ----------------------------------------- */

/* Read / write the raw control byte. Combine with YPAR_CONTROL_* masks. The
 * read returns the byte (0..255), the write 0; both a YPAR_ERR_* code on
 * failure. */
YPAR_API int ypar_read_control(ypar_port* p);
YPAR_API int ypar_write_control(ypar_port* p, uint8_t value);

/* Set or clear individual control bits without disturbing the others.
 * Returns 0 or a YPAR_ERR_* code; a failed read writes nothing back. */
YPAR_API int ypar_set_control_bit(ypar_port* p, uint8_t mask, bool on);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_PARALLEL_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_PARALLEL_IMPLEMENTATION
#ifndef YSP_PARALLEL_IMPLEMENTATION_GUARD
#define YSP_PARALLEL_IMPLEMENTATION_GUARD

/* ysp/rt.h's implementation block sits outside its include guard and carries
 * a guard of its own, so asking for it here is safe however this translation
 * unit arrived: a file that defines YSP_RT_IMPLEMENTATION itself, or that also
 * implements ysp/serial.h, still ends up with exactly one copy. */
#ifndef YSP_RT_IMPLEMENTATION_GUARD
    #define YSP_RT_IMPLEMENTATION
    #include "ysp/rt.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/* Pick the concrete platform once. */
#if defined(_WIN32)
    #define YPAR__WINDOWS 1
#elif defined(__linux__)
    #define YPAR__LINUX 1
    /* Raw port I/O (outb/inb) only exists on x86. */
    #if defined(__i386__) || defined(__x86_64__)
        #define YPAR__HAVE_IOPORT 1
    #endif
#else
    #error "ysp_parallel: unsupported platform (need Windows or Linux)"
#endif

/* --- shared helpers ----------------------------------------------------- */

static void ypar__set_error(ypar_port* p, const char* fmt, ...) {
    if (!p) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error, sizeof(p->error), fmt, ap);
    va_end(ap);
}

static void ypar__clear_error(ypar_port* p) {
    if (p) p->error[0] = '\0';
}

/* Every failing call writes the message before it returns its code. */
#define YPAR__FAIL(p, rc, ...) (ypar__set_error((p), __VA_ARGS__), (rc))
#define YPAR__CLOSED(p) YPAR__FAIL((p), YPAR_ERR_CLOSED, "port not open")

const char* ypar_error(const ypar_port* p) {
    return p ? p->error : "null port handle";
}

const char* ypar_strerror(int code) {
    switch (code) {
    case YPAR_ERR_IO:     return "the OS refused the register access";
    case YPAR_ERR_CLOSED: return "port not open";
    case YPAR_ERR_ARG:    return "bad argument";
    case YPAR_ERR_ASYNC:  return "no async worker";
    default:              return code >= 0 ? "ok" : "unknown error";
    }
}

bool ypar_is_open(const ypar_port* p) {
    return p && p->is_open;
}

/* Resolve desc.sched into p->sched. All-zero means the library defaults, a
 * zero period means "same as the deadline", and anything else must satisfy
 * 0 < runtime <= deadline <= period; yrt_sched_normalize() is the one place
 * that rule lives. Checking at open time is the point: a half-filled struct
 * the kernel would reject with EINVAL fails loudly here instead of silently
 * degrading to the next rung down. */
static bool ypar__set_sched(ypar_port* p, const ypar_desc* d) {
    p->sched = d->sched;
    if (yrt_sched_normalize(&p->sched) < 0) {
        ypar__set_error(p, "invalid desc.sched: require 0 < runtime_ns <= "
                           "deadline_ns <= period_ns (period_ns 0 means "
                           "same as deadline_ns)");
        return false;
    }
    return true;
}

/* Per-platform data write that sets p->error but takes no data lock. The
 * public ypar_write_data() wraps it with the lock and pending-pulse cancel. */
static int ypar__write_data_nolock(ypar_port* p, uint8_t value);

#ifndef YPAR_NO_THREADS
/* defined in the async-pulse section below */
static void ypar__async_stop(ypar_port* p);
static bool ypar__async_start(ypar_port* p);
static int  ypar__write_data_cancel(ypar_port* p, uint8_t value);
#endif

/* Shared tail of a successful ypar_open(): start the async worker now, so
 * its scheduling policy is known before the first trial and no pulse pays
 * for thread creation. A start failure is not fatal here; ypar_pulse_async()
 * retries and reports it. */
static bool ypar__open_done(ypar_port* p) {
    p->is_open = true;
#ifndef YPAR_NO_THREADS
    if (!ypar__async_start(p)) {
#if defined(YPAR__HAVE_IOPORT)
        /* On YPAR_BACKEND_DIRECT the worker's own per-thread ioperm()/iopl()
         * grant is part of what just failed, and no later call can repair it:
         * a port whose async trailing edges cannot write is not an open port.
         * ypar_close() leaves p->error alone, so the reason survives. */
        if (p->direct) { ypar_close(p); return false; }
#endif
        /* Every other backend writes through a process-wide descriptor or
         * DLL, so the blocking calls work and ypar_pulse_async() retries the
         * start; a failure here is not the caller's problem yet. */
        ypar__clear_error(p);
    }
#endif
    return true;
}

/* ======================================================================= *
 *  LINUX
 * ======================================================================= */
#if defined(YPAR__LINUX)

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/ppdev.h>
#include <linux/parport.h>
#if defined(YPAR__HAVE_IOPORT)
    #include <sys/io.h>
#endif

#if defined(YPAR__HAVE_IOPORT)
/* Grant the CALLING thread permission for the port's 3 registers. Needs
 * root/CAP_SYS_RAWIO either way. ioperm() covers only ports below 0x400,
 * which is where legacy ISA LPTs live; PCI/PCIe cards sit above that and need
 * iopl(3), which opens every port.
 *
 * Both are per-thread, which is why this is a helper: the async worker calls
 * it for itself before its first write, so a worker started from a thread
 * other than the one that opened the port still has permission. Re-granting
 * on a thread that already has it is a no-op. */
static int ypar__io_grant(uint16_t base) {
    if ((unsigned)base + 3u <= 0x400u) return ioperm(base, 3, 1);
    return iopl(3);
}
#endif

bool ypar_open(ypar_port* p, const ypar_desc* desc) {
    if (!p) return false;
    ypar_desc d;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    memset(p, 0, sizeof(*p));
    p->fd = -1;

    p->backend = d.backend;
    if (p->backend == YPAR_BACKEND_DEFAULT) p->backend = YPAR_BACKEND_PPDEV;
    if (p->backend == YPAR_BACKEND_INPOUT) {
        ypar__set_error(p, "inpout backend is Windows-only");
        return false;
    }
    p->base_addr = d.base_addr ? d.base_addr : YPAR_LPT1;

    /* A *valid* reservation the kernel merely cannot admit still falls back
     * to FIFO; only a malformed one fails the open. */
    if (!ypar__set_sched(p, &d)) return false;

    if (p->backend == YPAR_BACKEND_PPDEV) {
        const char* path = d.device ? d.device : "/dev/parport0";
        /* O_CLOEXEC: a claimed port must not leak into subprocesses
         * (Python subprocess, MATLAB system()), which would keep it claimed
         * after this handle closes. */
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            ypar__set_error(p, "open(%s): %s", path, strerror(errno));
            return false;
        }
        /* Claim the port from the kernel before any read/write. PPEXCL must
         * precede PPCLAIM; the caller asked for exclusivity, so its failure
         * is the caller's failure. */
        if (d.exclusive && ioctl(fd, PPEXCL) != 0) {
            ypar__set_error(p, "PPEXCL(%s): %s", path, strerror(errno));
            close(fd);
            return false;
        }
        /* PPCLAIM blocks while another client holds the port; it only fails
         * when the device has no parport behind it. */
        if (ioctl(fd, PPCLAIM) != 0) {
            ypar__set_error(p, "PPCLAIM(%s): %s", path, strerror(errno));
            close(fd);
            return false;
        }
        p->fd = fd;
        p->claimed = true;
        p->direct = false;
        /* A previous client may have left a bidirectional port in reverse
         * mode, where the data pins are tri-stated and every trigger byte
         * would float instead of driving the recorder. */
        int dir = 0; /* PPDATADIR: 0 = forward/out */
        if (ioctl(fd, PPDATADIR, &dir) != 0) {
            ypar__set_error(p, "PPDATADIR(%s): %s", path, strerror(errno));
            ioctl(fd, PPRELEASE);
            close(fd);
            p->fd = -1;
            p->claimed = false;
            return false;
        }
        return ypar__open_done(p);
    }

    if (p->backend == YPAR_BACKEND_DIRECT) {
#if defined(YPAR__HAVE_IOPORT)
        if (ypar__io_grant(p->base_addr) != 0) {
            ypar__set_error(p, "I/O permission for base 0x%X: %s "
                               "(need root/CAP_SYS_RAWIO)",
                            p->base_addr, strerror(errno));
            return false;
        }
        p->direct = true;
        /* Force forward mode; see the ppdev branch. */
        {
            uint8_t c = inb((uint16_t)(p->base_addr + 2));
            outb((uint8_t)(c & (uint8_t)~YPAR_CONTROL_DIR),
                 (uint16_t)(p->base_addr + 2));
        }
        return ypar__open_done(p);
#else
        ypar__set_error(p, "direct port I/O requires x86/x86_64");
        return false;
#endif
    }

    ypar__set_error(p, "unknown backend %d", (int)p->backend);
    return false;
}

void ypar_close(ypar_port* p) {
    if (!p || !p->is_open) return;
#ifndef YPAR_NO_THREADS
    ypar__async_stop(p);
#endif
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        /* Per-thread, not per-handle: this drops permission for every DIRECT
         * handle on this thread, and iopl(0) drops all ports, not just ours.
         * Documented in BACKENDS rather than reference-counted, because the
         * ioperm() ranges of two handles can overlap anyway. */
        if ((unsigned)p->base_addr + 3u <= 0x400u) ioperm(p->base_addr, 3, 0);
        else iopl(0);
#endif
    } else if (p->fd >= 0) {
        if (p->claimed) ioctl(p->fd, PPRELEASE);
        close(p->fd);
    }
    p->fd = -1;
    p->claimed = false;
    p->is_open = false;
}

static int ypar__write_data_nolock(ypar_port* p, uint8_t value) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        outb(value, p->base_addr);
        return 0;
#endif
    }
    if (ioctl(p->fd, PPWDATA, &value) != 0)
        return YPAR__FAIL(p, YPAR_ERR_IO, "PPWDATA: %s", strerror(errno));
    return 0;
}

#ifndef YPAR_NO_THREADS
/* Hardware data write that does NOT touch the shared p->error buffer, so the
 * async worker thread can drop the trailing edge without racing main-thread
 * API calls (which also read/write p->error). */
static bool ypar__write_data_raw(ypar_port* p, uint8_t value) {
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        outb(value, p->base_addr);
        return true;
#endif
    }
    return ioctl(p->fd, PPWDATA, &value) == 0;
}
#endif

int ypar_read_data(ypar_port* p) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        return inb(p->base_addr);
#endif
    }
    unsigned char v = 0;
    if (ioctl(p->fd, PPRDATA, &v) != 0)
        return YPAR__FAIL(p, YPAR_ERR_IO, "PPRDATA: %s", strerror(errno));
    return v;
}

int ypar_set_data_dir(ypar_port* p, bool input) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        uint8_t c = inb((uint16_t)(p->base_addr + 2));
        c = input ? (c | YPAR_CONTROL_DIR) : (c & (uint8_t)~YPAR_CONTROL_DIR);
        outb(c, (uint16_t)(p->base_addr + 2));
        return 0;
#endif
    }
    int dir = input ? 1 : 0; /* PPDATADIR: 0 = forward/out, 1 = reverse/in */
    if (ioctl(p->fd, PPDATADIR, &dir) != 0)
        return YPAR__FAIL(p, YPAR_ERR_IO, "PPDATADIR: %s", strerror(errno));
    return 0;
}

int ypar_read_status(ypar_port* p) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        return inb((uint16_t)(p->base_addr + 1));
#endif
    }
    unsigned char v = 0;
    if (ioctl(p->fd, PPRSTATUS, &v) != 0)
        return YPAR__FAIL(p, YPAR_ERR_IO, "PPRSTATUS: %s", strerror(errno));
    return v;
}

int ypar_read_control(ypar_port* p) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        return inb((uint16_t)(p->base_addr + 2));
#endif
    }
    unsigned char v = 0;
    if (ioctl(p->fd, PPRCONTROL, &v) != 0)
        return YPAR__FAIL(p, YPAR_ERR_IO, "PPRCONTROL: %s", strerror(errno));
    return v;
}

int ypar_write_control(ypar_port* p, uint8_t value) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (p->direct) {
#if defined(YPAR__HAVE_IOPORT)
        outb(value, (uint16_t)(p->base_addr + 2));
        return 0;
#endif
    }
    if (ioctl(p->fd, PPWCONTROL, &value) != 0)
        return YPAR__FAIL(p, YPAR_ERR_IO, "PPWCONTROL: %s", strerror(errno));
    return 0;
}

int ypar_list_ports(ypar_port_info* out, int max) {
    int count = 0;
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/parport%d", i);
        /* R_OK|W_OK: list what this user can open, not what merely exists. */
        if (access(path, R_OK | W_OK) != 0) continue;
        if (out && count < max) {
            ypar_port_info* e = &out[count];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), "%s", path);
            e->backend = YPAR_BACKEND_PPDEV;
            e->base_addr = 0;
        }
        count++;
    }
    return count;
}

#endif /* YPAR__LINUX */

/* ======================================================================= *
 *  WINDOWS  (inpout32 / inpoutx64)
 * ======================================================================= */
#if defined(YPAR__WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef void  (__stdcall *ypar__out32_t)(short, short);
typedef short (__stdcall *ypar__inp32_t)(short);

bool ypar_open(ypar_port* p, const ypar_desc* desc) {
    if (!p) return false;
    ypar_desc d;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    memset(p, 0, sizeof(*p));

    p->backend = d.backend;
    if (p->backend == YPAR_BACKEND_DEFAULT) p->backend = YPAR_BACKEND_INPOUT;
    if (p->backend != YPAR_BACKEND_INPOUT) {
        ypar__set_error(p, "only the inpout backend is available on Windows");
        return false;
    }
    p->base_addr = d.base_addr ? d.base_addr : YPAR_LPT1;
    /* Windows grants no reservation, but a malformed one is still a caller
     * error and fails here, so the same desc is accepted or rejected on both
     * platforms rather than only on Linux. */
    if (!ypar__set_sched(p, &d)) return false;

    /* LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: the EXECUTABLE's directory, System32
     * and AddDllDirectory() entries, but never the current directory, so a
     * DLL dropped into a data folder cannot be picked up as a ring-0 port
     * driver. The executable is python.exe or MATLAB under the bindings, so
     * desc.dll_path exists for callers that ship the DLL elsewhere; its
     * dependencies then resolve from its own folder too. */
    const DWORD search = d.dll_path
        ? (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
        : LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    const char* name = d.dll_path;
    HMODULE dll = NULL;
    DWORD err = 0;
    if (name) {
        dll = LoadLibraryExA(name, NULL, search);
        if (!dll) err = GetLastError();
    } else {
#if defined(_WIN64)
        /* Report the 64-bit DLL's error, not the 32-bit fallback's: in a
         * 64-bit process inpout32.dll almost always fails with
         * ERROR_BAD_EXE_FORMAT, which says nothing about why the right DLL
         * was missing. The fallback is kept because some InpOut installs ship
         * a 64-bit binary under the old name. */
        name = "inpoutx64.dll";
        dll = LoadLibraryExA(name, NULL, search);
        if (!dll) {
            err = GetLastError();
            dll = LoadLibraryExA("inpout32.dll", NULL, search);
        }
#else
        name = "inpout32.dll";
        dll = LoadLibraryExA(name, NULL, search);
        if (!dll) err = GetLastError();
#endif
    }
    if (!dll) {
        ypar__set_error(p, "LoadLibrary %s failed (err %lu); install InpOut "
                           "from highrez.co.uk and put the DLL next to the "
                           "executable, in System32, or name it in "
                           "desc.dll_path",
                        name, (unsigned long)err);
        return false;
    }

    /* FARPROC to the real signature by way of void (*)(void), the one
     * function type GCC exempts from -Wcast-function-type; a void* detour is
     * a pedantic error on MinGW. */
    ypar__out32_t out_fn = (ypar__out32_t)(void (*)(void))GetProcAddress(dll, "Out32");
    ypar__inp32_t inp_fn = (ypar__inp32_t)(void (*)(void))GetProcAddress(dll, "Inp32");
    if (!out_fn || !inp_fn) {
        ypar__set_error(p, "inpout DLL missing Out32/Inp32 entry points");
        FreeLibrary(dll);
        return false;
    }

    p->dll = (void*)dll;
    p->out_fn = (void (*)(void))out_fn;
    p->inp_fn = (void (*)(void))inp_fn;
    /* A previous client may have left a bidirectional port in reverse mode,
     * where the data pins are tri-stated and every trigger byte would float
     * instead of driving the recorder. */
    {
        short ctrl = (short)(uint16_t)(p->base_addr + 2);
        uint8_t c = (uint8_t)(inp_fn(ctrl) & 0xFF);
        out_fn(ctrl, (short)(uint8_t)(c & (uint8_t)~YPAR_CONTROL_DIR));
    }
    return ypar__open_done(p);
}

void ypar_close(ypar_port* p) {
    if (!p || !p->is_open) return;
#ifndef YPAR_NO_THREADS
    ypar__async_stop(p);
#endif
    if (p->dll) FreeLibrary((HMODULE)p->dll);
    p->dll = NULL;
    p->out_fn = NULL;
    p->inp_fn = NULL;
    p->is_open = false;
}

static void ypar__out(ypar_port* p, uint16_t addr, uint8_t val) {
    ((ypar__out32_t)p->out_fn)((short)addr, (short)val);
}
static uint8_t ypar__in(ypar_port* p, uint16_t addr) {
    return (uint8_t)(((ypar__inp32_t)p->inp_fn)((short)addr) & 0xFF);
}

static int ypar__write_data_nolock(ypar_port* p, uint8_t value) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    ypar__out(p, p->base_addr, value);
    return 0;
}

#ifndef YPAR_NO_THREADS
/* Hardware data write that does NOT touch the shared p->error buffer (see the
 * Linux note); used by the async worker for the trailing edge. */
static bool ypar__write_data_raw(ypar_port* p, uint8_t value) {
    ypar__out(p, p->base_addr, value);
    return true;
}
#endif

int ypar_read_data(ypar_port* p) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    return ypar__in(p, p->base_addr);
}

int ypar_set_data_dir(ypar_port* p, bool input) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    uint8_t c = ypar__in(p, (uint16_t)(p->base_addr + 2));
    c = input ? (c | YPAR_CONTROL_DIR) : (c & (uint8_t)~YPAR_CONTROL_DIR);
    ypar__out(p, (uint16_t)(p->base_addr + 2), c);
    return 0;
}

int ypar_read_status(ypar_port* p) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    return ypar__in(p, (uint16_t)(p->base_addr + 1));
}

int ypar_read_control(ypar_port* p) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    return ypar__in(p, (uint16_t)(p->base_addr + 2));
}

int ypar_write_control(ypar_port* p, uint8_t value) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    ypar__out(p, (uint16_t)(p->base_addr + 2), value);
    return 0;
}

int ypar_list_ports(ypar_port_info* out, int max) {
    int count = 0;
    /* Query LPT1..LPT9 by name. QueryDosDevice(NULL, ...) returns the whole
     * device namespace (tens of KB on a current system) and fails outright
     * with ERROR_INSUFFICIENT_BUFFER on any fixed buffer, which used to make
     * this function report zero ports on every machine. */
    for (int i = 1; i <= 9; i++) {
        char name[8], target[256];
        snprintf(name, sizeof(name), "LPT%d", i);
        if (QueryDosDeviceA(name, target, (DWORD)sizeof(target)) == 0) continue;
        if (out && count < max) {
            ypar_port_info* e = &out[count];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), "%s", name);
            e->backend = YPAR_BACKEND_INPOUT;
            e->base_addr = 0; /* unknown; supply via ypar_desc.base_addr */
        }
        count++;
    }
    return count;
}

#endif /* YPAR__WINDOWS */

/* ======================================================================= *
 *  ASYNC PULSE
 *
 *  The trailing edge of a non-blocking pulse is one job on one ysp/rt.h
 *  deadline worker per port. The leading edge is written by the caller, in
 *  ypar_pulse_async, so the onset never waits for a thread to wake; the
 *  worker only waits out an absolute deadline and then writes 0. There is
 *  one pending off-deadline per port, so a pulse issued while one is in
 *  flight moves that single trailing edge rather than queueing a second.
 *
 *  ysp/rt.h runs the job with ITS lock released, and cannot cancel a job
 *  that has already entered its final spin window. So this header keeps a
 *  lock of its own over the data register, plus a token: every data write
 *  bumps the token, and the trailing edge writes only while the token it was
 *  submitted with is still the current one. That is what keeps "a plain
 *  write cancels a pending trailing edge" a guarantee instead of a race, and
 *  it is what serializes the two writers on one register.
 *
 *  The lock order is one-way and therefore deadlock-free: a caller takes
 *  this lock and then calls into ysp/rt.h (submit or cancel, which take
 *  ysp/rt.h's own), while the job takes this one with ysp/rt.h's already
 *  released.
 * ======================================================================= */
#ifndef YPAR_NO_THREADS

#if defined(YPAR__LINUX)
#include <pthread.h>
typedef pthread_mutex_t ypar__lock;
#else
typedef CRITICAL_SECTION ypar__lock;
#endif

typedef struct ypar__async {
    yrt_worker w;
    ypar__lock   lock;
    ypar_port*   port;
    /* Bumped by every data write and by every submit. job_token is the value
     * the pending trailing edge carries; unequal means a later write has
     * claimed the lines and the trailing 0 must not undo it. */
    uint32_t     token;
    uint32_t     job_token;
} ypar__async;

#if defined(YPAR__LINUX)

static bool ypar__lock_init(ypar__lock* m) {
    /* Priority inheritance: the submitter (normal priority) holds this lock
     * across the onset ioctl, and without PI the real-time worker would wait
     * behind whatever preempted that submitter. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    int rc = pthread_mutex_init(m, &ma);
    pthread_mutexattr_destroy(&ma);
    return rc == 0;
}
static void ypar__lock_destroy(ypar__lock* m) { pthread_mutex_destroy(m); }
static void ypar__lock_acquire(ypar__lock* m) { pthread_mutex_lock(m); }
static void ypar__lock_release(ypar__lock* m) { pthread_mutex_unlock(m); }

#else

static bool ypar__lock_init(ypar__lock* m)    { InitializeCriticalSection(m); return true; }
static void ypar__lock_destroy(ypar__lock* m) { DeleteCriticalSection(m); }
static void ypar__lock_acquire(ypar__lock* m) { EnterCriticalSection(m); }
static void ypar__lock_release(ypar__lock* m) { LeaveCriticalSection(m); }

#endif

/* Give the worker thread port I/O permission. ioperm() and iopl() are
 * per-THREAD on Linux, so the thread that will run the trailing edges has to
 * ask for itself, and a missing grant is a SIGSEGV on the outb rather than an
 * error code. yrt_worker_desc.on_start runs on that thread before
 * yrt_worker_open() returns, which is what lets a refusal fail ypar_open()
 * instead of one trigger's offset. Only YPAR_BACKEND_DIRECT needs it: ppdev
 * has a process-wide descriptor and inpout a process-wide DLL. */
static bool ypar__async_on_start(void* ctx, char* err, size_t cap) {
#if defined(YPAR__HAVE_IOPORT)
    ypar_port* p = (ypar_port*)ctx;
    if (!p->direct) return true;
    if (ypar__io_grant(p->base_addr) != 0) {
        snprintf(err, cap, "worker thread I/O permission for base 0x%X: %s "
                           "(need root/CAP_SYS_RAWIO)",
                 (unsigned)p->base_addr, strerror(errno));
        return false;
    }
    return true;
#else
    (void)ctx; (void)err; (void)cap;
    return true;
#endif
}

/* The trailing edge. Runs on the ysp/rt.h worker thread, at the worker's
 * policy, with ysp/rt.h's lock released. p->error belongs to the calling
 * thread, so a failure is reported through the sticky flag instead. */
static void ypar__async_job(void* ctx, const yrt_job_info* info) {
    ypar__async* a = (ypar__async*)ctx;
    /* info->flushed is deliberately ignored: a ypar_close() with a pulse in
     * flight still has to drop the lines, just earlier than asked. The job's
     * info->seq is ignored too: job_token is the identity this header acts
     * on, because a plain write must be able to retire a trailing edge
     * without going anywhere near ysp/rt.h. */
    (void)info;
    ypar__lock_acquire(&a->lock);
    if (a->job_token == a->token) {
        if (!ypar__write_data_raw(a->port, 0))
            a->port->async_write_failed = 1;
    }
    ypar__lock_release(&a->lock);
}

static bool ypar__async_start(ypar_port* p) {
    if (p->async) return true;
    /* One allocation per handle, made by ypar_open(), never on a write or
     * pulse path. The yrt_worker is caller-allocated and lives in this
     * block rather than in ypar_port so the public struct neither grows by a
     * worker handle nor changes shape under YRT_NO_THREADS. */
    ypar__async* a = (ypar__async*)calloc(1, sizeof(*a));
    if (!a) { ypar__set_error(p, "async: out of memory"); return false; }
    a->port = p;
    if (!ypar__lock_init(&a->lock)) {
        free(a);
        ypar__set_error(p, "async: mutex init");
        return false;
    }
    yrt_worker_desc wd;
    memset(&wd, 0, sizeof(wd));
    wd.sched = p->sched; /* already normalized by ypar_open() */
    wd.on_start = ypar__async_on_start;
    wd.start_ctx = p;
    if (!yrt_worker_open(&a->w, &wd)) {
        ypar__set_error(p, "async: %s", yrt_worker_error(&a->w));
        ypar__lock_destroy(&a->lock);
        free(a);
        return false;
    }
    /* yrt_worker_open() does not return until the thread has published
     * its rung, so async_policy is final when ypar_open() returns. */
    p->async_policy = yrt_worker_policy(&a->w);
    p->async = a;
    return true;
}

static void ypar__async_stop(ypar_port* p) {
    ypar__async* a = (ypar__async*)p->async;
    if (!a) return;
    /* Clear p->async first: ypar_write_data() routes through the lock
     * whenever it sees a block, and a call racing this teardown (a contract
     * violation, but a cheap one to survive) must not reach for a lock that
     * is about to be destroyed. */
    p->async = NULL;
    /* Runs a pending trailing edge IMMEDIATELY through ypar__async_job
     * instead of waiting out the rest of the width, so the lines are never
     * left asserted. Called without the lock held; the job takes it. */
    yrt_worker_close(&a->w);
    ypar__lock_destroy(&a->lock);
    free(a);
    p->async_policy = YPAR_ASYNC_NONE;
}

static int ypar__write_data_cancel(ypar_port* p, uint8_t value) {
    ypar__async* a = (ypar__async*)p->async;
    ypar__lock_acquire(&a->lock);
    /* A plain write means "hold these lines"; a pending trailing edge would
     * silently undo it. The token bump is the authoritative cancel, because
     * yrt_worker_cancel() cannot reach a job already inside its final spin
     * window; the cancel call itself only saves the worker a pointless wake. */
    a->token++;
    (void)yrt_worker_cancel(&a->w);
    int rc = ypar__write_data_nolock(p, value);
    ypar__lock_release(&a->lock);
    return rc;
}

static int ypar__async_submit(ypar_port* p, uint8_t value, int64_t width_ns) {
    ypar__async* a = (ypar__async*)p->async;
    ypar__lock_acquire(&a->lock);
    /* Leading edge under the lock so it cannot be stomped by the trailing
     * edge of an in-flight pulse. */
    int wrc = ypar__write_data_nolock(p, value);
    /* The width is measured from the onset write, not from entry, so the
     * ioctl round trip is not subtracted from it. */
    int64_t off_at = yrt_now_ns() + width_ns;
    a->token++;
    a->job_token = a->token;
    /* Replaces any pending job, earlier deadline or later, and the worker
     * re-evaluates at once. */
    /* A positive return is this job's seq; only a negative one is an error. */
    int rc = yrt_worker_submit(&a->w, off_at, ypar__async_job, a);
    if (rc < 0) {
        /* Cannot happen while the worker is running, and if it somehow does,
         * the lines must not be left asserted with nothing to clear them. */
        ypar__write_data_raw(p, 0);
        ypar__lock_release(&a->lock);
        return YPAR__FAIL(p, YPAR_ERR_ASYNC, "async: submit: %s", yrt_strerror(rc));
    }
    ypar__lock_release(&a->lock);
    return wrc;
}

int ypar_pulse_async(ypar_port* p, uint8_t value, int64_t width_ns) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (width_ns < 0) return YPAR__FAIL(p, YPAR_ERR_ARG, "ypar_pulse_async: width_ns < 0");
    if (!p->async) {
        /* ypar_open() normally started the worker; this path only runs if
         * that failed. Write the onset NOW, before paying the worker-thread
         * creation cost, so this trigger's onset is not delayed by thread
         * spin-up (its width will include it). No worker exists yet, so
         * nothing can race this write or stomp it with a trailing edge. */
        int rc = ypar_write_data(p, value);
        if (rc < 0) return rc;
        if (!ypar__async_start(p)) {
            /* Don't leave the data lines asserted with no worker to clear
             * them; use the raw write so ypar__async_start's error survives. */
            ypar__write_data_raw(p, 0);
            return YPAR_ERR_ASYNC;
        }
    }
    /* Steady state: the leading edge is (re)written inside submit on the
     * caller's thread under the data lock, so onset is not delayed by the
     * worker waking up and the write cannot race the worker's trailing edge.
     * On the first-use path above this re-asserts the same value (no edge). */
    return ypar__async_submit(p, value, width_ns);
}

#else /* YPAR_NO_THREADS */

int ypar_pulse_async(ypar_port* p, uint8_t value, int64_t width_ns) {
    (void)value; (void)width_ns;
    return YPAR__FAIL(p, YPAR_ERR_ASYNC, "ypar_pulse_async unavailable: built with YPAR_NO_THREADS");
}

#endif /* YPAR_NO_THREADS */

/* ======================================================================= *
 *  PLATFORM-INDEPENDENT helpers (built on the primitives above)
 * ======================================================================= */

int ypar_get_status_bit(ypar_port* p, uint8_t mask) {
    int s = ypar_read_status(p);
    return s < 0 ? s : (s & mask) != 0;
}

int ypar_set_control_bit(ypar_port* p, uint8_t mask, bool on) {
    int c = ypar_read_control(p);
    /* A failed read must not be written back: 0 would clear every control
     * line. */
    if (c < 0) return c;
    c = on ? (c | mask) : (c & ~(int)mask);
    return ypar_write_control(p, (uint8_t)c);
}

int ypar_write_data(ypar_port* p, uint8_t value) {
#ifndef YPAR_NO_THREADS
    if (p && p->async) return ypar__write_data_cancel(p, value);
#endif
    return ypar__write_data_nolock(p, value);
}

int ypar_pulse(ypar_port* p, uint8_t value, int64_t width_ns) {
    if (!ypar_is_open(p)) return YPAR__CLOSED(p);
    if (width_ns < 0) return YPAR__FAIL(p, YPAR_ERR_ARG, "ypar_pulse: width_ns < 0");
    int rc = ypar_write_data(p, value);
    if (rc < 0) return rc;
    /* The deadline is absolute and taken just after the onset write, so the
     * write's own round trip is not added to the width and a signal restart
     * does not stretch it. ysp/rt.h hands the bulk of the wait to the OS and
     * spins the tail; this one runs on the CALLER's thread at the caller's
     * priority, which is why PULSES AND TIMING calls this edge the less
     * precise of the two. */
    if (width_ns)
        (void)yrt_sleep_until(yrt_now_ns() + width_ns, YRT_DEFAULT_SPIN_NS);
    return ypar_write_data(p, 0);
}

#endif /* YSP_PARALLEL_IMPLEMENTATION_GUARD */
#endif /* YSP_PARALLEL_IMPLEMENTATION */

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
