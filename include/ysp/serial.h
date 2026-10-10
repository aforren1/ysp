/* ysp/serial.h - v0.4 - public domain single-header serial-port library
 *
 *   REQUIRES ysp/rt.h, its sibling in this collection, on the include path.
 *   This header includes it for the clock, the waits, the scheduling ladder
 *   and the deadline worker that writes the trailing edge of an async pulse.
 *   Copy TWO files, not one.
 *
 *   STATUS: IMPLEMENTED on Windows (Win32 COM API, overlapped I/O) and POSIX
 *   (Linux and macOS termios). Tested against a virtual port pair on Linux,
 *   including one reader thread plus one writer thread on a single handle
 *   under ThreadSanitizer. Windows and macOS compile clean in CI but have
 *   never run on a device: on Windows the open-time validation and the
 *   enumeration entry point run, but no byte has moved through the backend,
 *   and on macOS (IOSSIOSPEED, /dev/cu.* enumeration, the select() waits)
 *   nothing but the compiler has seen the code. No timing number in this
 *   header has been measured on a rig yet. The timing code is ysp/rt.h's, so
 *   its STATUS block applies too: the real-time rungs of the worker's ladder
 *   have never been granted on any machine here.
 *
 *   v0.4, the current version, deletes this header's private timing code and
 *   depends on ysp/rt.h for it:
 *     - The monotonic clock, the pulse-width wait, the break sleep, the
 *       SCHED_DEADLINE / SCHED_FIFO ladder, the sched_setattr declarations and
 *       the async-pulse worker thread are all gone from here. yser_now_us() is
 *       yrt_now_us(), so "your timestamps and the library's share one base"
 *       is true by construction instead of by two copies of the same code
 *       agreeing. There were three such copies in this repository; the Windows
 *       spin bug that only this one had is the reason there is now one.
 *     - yser_sched_deadline is a typedef of yrt_sched_deadline and
 *       yser_async_policy of yrt_policy. The YSER_ASYNC_*, YSER_DEFAULT_RT_*
 *       and type names all still work, but the NUMERIC VALUES of the policy
 *       enum changed, because yrt_policy has a macOS rung between DEADLINE
 *       and FIFO: recompile anything that stored those integers, do not just
 *       relink it. On macOS async_policy can now report
 *       YRT_POLICY_TIME_CONSTRAINT, a rung this header did not have.
 *     - Blocking yser_pulse() no longer busy-spins the whole width on Windows.
 *       It sleeps to the deadline and spins only the last
 *       YRT_DEFAULT_SPIN_NS, as the async worker always did.
 *     - The async worker is one yrt_worker plus a small writer mutex that
 *       stays here, because the mutex is about the byte stream, not about the
 *       deadline. THREADING says what each one owns.
 *
 *   v0.3 was a correctness pass over the two backends nobody has run yet,
 *   plus two documentation corrections:
 *     - Windows: a failed GetOverlappedResult on a write no longer reports a
 *       driver error as a short count (which a caller retries forever); an
 *       aborted read that was neither an interrupt nor a disconnect waits out
 *       the rest of its timeout instead of returning 0; a failed
 *       SetCommTimeouts maps through the disconnect test; the worker's
 *       sub-millisecond spin now yields to a replacement pulse and to quit.
 *     - macOS: the reader and the writer wait with select(), not poll(),
 *       because Apple's poll(2) still documents that it does not support
 *       devices. Linux keeps poll(). See POSIX WAITS.
 *     - Linux: the low_latency readback resolves the device name first, so a
 *       /dev/serial/by-id/... symlink no longer leaves low_latency false.
 *     - yser_close()'s flush of a pending trailing byte is bounded by 50 ms
 *       instead of by write_timeout_ms, so a wedged device cannot hold a
 *       close open for a second, or forever under YSER_TIMEOUT_INFINITE.
 *     - yser_open() refuses a handle that is still open instead of leaking
 *       its descriptor and its worker thread.
 *     - YSER_API is on the definitions too, so `static` works.
 *
 *   v0.2 changed five signatures from the v0.1 specification: yser_interrupt,
 *   yser_purge, yser_send_break, yser_set_dtr and yser_set_rts return int
 *   (0 or a YSER_ERR_* code), not bool, because they can be called from a
 *   thread that does not own the message buffer. See RETURN VALUES. It also
 *   added yser_pulse_async() and its worker thread; that worker has so far
 *   only been observed at YSER_ASYNC_NORMAL, because the test machine grants
 *   no CAP_SYS_NICE, so the real-time rungs of its ladder are unexercised.
 *   docs/serial.md holds the design rationale.
 *
 *   A small, dependency-free C library for byte I/O over serial ports:
 *   legacy RS-232, USB-CDC (Arduino, Teensy), and USB-serial bridges (FTDI,
 *   CP210x, CH340). Built for the devices found in psychophysics and
 *   neuroscience rigs:
 *
 *     - trigger boxes: Brain Products TriggerBox, BioSemi USB trigger
 *       interface, Cedrus c-pod / StimTracker, Black Box ToolKit, and DIY
 *       Arduino/Teensy boxes. Write a byte; it appears on the output lines.
 *     - response boxes: Cedrus RB-x40 (XID), PST Serial Response Box,
 *       Current Designs fORP in serial mode, DIY button boxes.
 *     - anything else that speaks bytes over a COM port.
 *
 *   Inspired by pyserial (https://github.com/pyserial/pyserial) and written
 *   in the single-header style of the stb / sokol libraries. It DEPENDS on
 *   ysp/rt.h for everything about time, the way stb_truetype.h depends on
 *   stb_rect_pack.h: two files to copy, one library to use. With
 *   ysp/parallel.h it shares conventions but not code.
 *
 *   Targets Windows (Win32 COM API), Linux and macOS (POSIX termios). C99 is
 *   the floor: it builds as C99, C11 and C++17, and in the C dialect MSVC
 *   compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Copy ysp/serial.h AND ysp/rt.h into one ysp/ directory of your project
 *   and put its parent on the include path, so that #include "ysp/rt.h"
 *   resolves. Then do this:
 *
 *       #define YSP_SERIAL_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *   ysp/rt.h's implementation comes along with it, in that same file. Defining
 *   YSP_RT_IMPLEMENTATION there as well is fine, and so is implementing
 *   another header that needs ysp/rt.h there, because ysp/rt.h's
 *   implementation block guards itself and lands once. What does not work is
 *   putting YSP_RT_IMPLEMENTATION in a SECOND file: that is a second copy of
 *   ysp/rt.h's code in the program, and the linker says so. One file
 *   implements everything.
 *
 *       #define YSP_SERIAL_IMPLEMENTATION
 *       #include "ysp/serial.h"
 *
 *       yser_port sp = { 0 };                  // zeroed, or closed; required
 *       yser_desc desc = {                     // designated init: C99 or C++20
 *           .device = "COM3",                  // or "/dev/ttyUSB0"
 *           .baud   = 115200,                  // 0 = default (115200)
 *       };
 *       if (!yser_open(&sp, &desc)) {
 *           fprintf(stderr, "open failed: %s\n", yser_error(&sp));
 *           return 1;
 *       }
 *       yser_write_byte(&sp, 0x55);            // raise a trigger
 *       yser_pulse(&sp, 0x55, 0x00, 2000);     // 0x55, hold 2 ms, then 0x00
 *       yser_pulse_async(&sp, 0x55, 0x00, 2000); // same, without blocking
 *
 *       uint8_t buf[64];
 *       int n = yser_read(&sp, buf, (int)sizeof buf, 100, YSER_READ_ANY);  // bytes within 100 ms
 *       int m = yser_read(&sp, buf, 6, 500, YSER_READ_ALL);           // a 6-byte frame within 500 ms
 *       if (n < 0) fprintf(stderr, "read: %s\n", yser_strerror(n));
 *       yser_close(&sp);
 *
 *   Find a box by USB id instead of hard-coding the port name:
 *
 *       yser_port_filter f = { .vid = 0x0403, .pid = 0x6001 };   // FTDI FT232R
 *       yser_port_info info;
 *       if (yser_find_ports(&f, &info, 1) == 1)                  // exactly one match
 *           desc.device = info.name;
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   A port is a full-duplex byte stream. The OS driver owns a receive buffer;
 *   bytes the device sends land there whether or not you are reading, and
 *   yser_read() drains it. Writes are handed to the driver and transmitted
 *   asynchronously; yser_drain() waits until the transmit buffer is empty.
 *
 *   RETURN VALUES AND ERRORS
 *     yser_open() is the only function that writes a message: it returns bool
 *     and fills yser_error(). Every other function returns an int, because
 *     every other function may run on a thread yser_open() knows nothing
 *     about, and two threads formatting into one 256-byte buffer is a data
 *     race under C11 whether or not anyone reads it. An int return is a count
 *     or a YSER_LINE_* mask when > 0, "done" when 0, or one of the YSER_ERR_*
 *     codes below when negative. The OS error number of the last failure goes
 *     to the slot that belongs to the calling role: yser_port.rd_oserr
 *     (reader), wr_oserr (writer), misc_oserr (the any-role calls).
 *     yser_strerror() names a code.
 *
 *       YSER_ERR_IO            OS error; see rd_oserr / wr_oserr / misc_oserr
 *       YSER_ERR_DISCONNECTED  the device went away (USB unplug); close it
 *       YSER_ERR_CLOSED        the port is not open
 *       YSER_ERR_INTERRUPTED   yser_interrupt() or yser_close() woke a wait
 *       YSER_ERR_ARG           bad argument
 *
 *   READ SEMANTICS
 *     yser_read(p, buf, cap, timeout_ms, flags)
 *       flags = YSER_READ_ANY (0): returns as soon as at least one byte is
 *       available, with everything buffered at that moment (up to `cap`).
 *       Returns 0 if nothing arrives within timeout_ms. timeout_ms = 0
 *       polls: it returns at once with whatever is buffered, possibly 0
 *       bytes. YSER_TIMEOUT_INFINITE blocks until data, interrupt or
 *       disconnect. This covers both "poll the response box once per frame"
 *       (timeout 0) and "wait for a button press" (timeout > 0).
 *       flags = YSER_READ_ALL: keeps reading until exactly `cap` bytes were
 *       read or timeout_ms elapsed in total; returns the count actually
 *       read. For framed protocols (a Cedrus XID key report is 6 bytes).
 *       A disconnect is an error (YSER_ERR_DISCONNECTED), never a 0, so a
 *       listener with a long timeout cannot spin on a dead port.
 *     yser_available(p)
 *       Bytes currently buffered by the driver, without reading them.
 *
 *   WRITE SEMANTICS
 *     yser_write(p, buf, len) blocks until every byte is accepted by the
 *     driver or desc.write_timeout_ms elapses; on timeout it returns the
 *     count accepted (< len), never an error code. Accepted is not
 *     delivered: the bytes are then queued for the UART or the USB pipe.
 *
 *   TIMING (read this before you trust a serial trigger)
 *     USB-serial adds latency that the parallel port does not have.
 *       - Host to device: a USB bulk write is scheduled into the next 1 ms
 *         (full-speed) or 125 us (high-speed) frame. Expect about 1 ms onset
 *         latency with sub-millisecond jitter for a single-byte write.
 *       - Device to host (responses): the bridge chip holds received bytes
 *         until its buffer fills or a latency timer expires. FTDI parts
 *         default to 16 ms. Set desc.low_latency = true to request 1 ms; the
 *         LOW LATENCY section says what each platform can actually do.
 *     To timestamp a trigger, bracket the call:
 *         uint64_t t0 = yser_now_us();
 *         yser_write_byte(&sp, code);
 *         uint64_t t1 = yser_now_us();
 *     and record both. The physical edge lies after t0 and, on USB, after
 *     the next frame boundary following the driver's acceptance; t1 - t0 is
 *     the syscall cost (10 to 100 us). Neither is the onset; they bound it.
 *     yser_now_us() IS yrt_now_us(), not a second clock that agrees with
 *     it, so these timestamps, ysp/rt.h's deadlines and anything ysp/rt.h
 *     reports are all differences of one counter.
 *     Measure the offset once with a scope and apply it.
 *     yser_pulse() widths below a few milliseconds are unreliable over USB.
 *     If the device can time the pulse itself (StimTracker, TriggerBox Plus,
 *     c-pod), prefer that and send one command instead of two.
 *
 *     PULSE WIDTH: WHO TIMES THE TRAILING EDGE
 *     yser_pulse() blocks the calling thread for the whole width and times the
 *     trailing edge on whatever thread the caller has, at whatever priority it
 *     has. It waits with yrt_sleep_until() and YRT_DEFAULT_SPIN_NS, so the
 *     OS gets the bulk of the width and only the tail is spun.
 *     yser_pulse_async() returns after the onset write and has a yrt_worker
 *     the library owns write the trailing byte at an absolute deadline taken
 *     right after that onset. That worker elevates itself up ysp/rt.h's ladder
 *     (SCHED_DEADLINE, else the macOS time-constraint policy, else SCHED_FIFO,
 *     else normal; THREAD_PRIORITY_TIME_CRITICAL on Windows) and waits the
 *     same way, so its trailing edge is MORE precise than yser_pulse's, not
 *     less, and the caller is free during the width. yser_port.async_policy
 *     says which rung it got; log it with your numbers, because they mean
 *     different things under different policies.
 *     What the wait itself is worth on your machine is ysp/rt.h's WAITS
 *     section, and examples/rt/jitter.c measures it there: it sweeps spin
 *     windows and prints the wake-latency distribution of each, which is the
 *     only honest number available for the trailing edge before the transport
 *     gets involved.
 *     What neither call can do is beat the transport. Over USB-serial the
 *     jitter of an edge is bounded below by the frame period (1 ms full-speed,
 *     125 us high-speed) and above by OS scheduling plus the driver's own
 *     batching, and on Windows that upper bound is reported to be far worse
 *     than a frame. Frame quantization is the floor, not the budget. Put a
 *     scope on the line, record the distribution together with async_policy,
 *     and do not assume a number from this header.
 *
 *   THREADING
 *     One reader thread and one writer thread may use a handle at the same
 *     time (the "listener thread + trigger sender" pattern). The roles:
 *       reader:  yser_read, yser_available, yser_purge(YSER_PURGE_RX)
 *       writer:  yser_write, yser_write_byte, yser_drain, yser_pulse,
 *                yser_send_break, yser_purge(YSER_PURGE_TX)
 *       any:     yser_interrupt, yser_get_lines, yser_set_dtr, yser_set_rts
 *                (one call at a time, from any thread; they share misc_oserr)
 *       neither: yser_open, yser_close (only after both roles have returned)
 *     The async-pulse worker is a SECOND WRITER on the handle, so every
 *     writer-role write (yser_write, yser_write_byte, yser_pulse,
 *     yser_pulse_async's onset, and the worker's own trailing byte) is
 *     serialized under one writer mutex this header owns, and the worker
 *     reports through wr_oserr like any writer. That mutex is here rather than
 *     in ysp/rt.h because yrt_worker runs its callback with its OWN lock
 *     released, which is what lets the callback block on a stream write;
 *     serializing the stream is this header's job. Next to the mutex sits the
 *     pending trailing byte, armed with its deadline: yser_pulse_async() takes
 *     the mutex, writes the onset and arms, every user write takes the mutex
 *     and DISARMS before it writes, and the worker's callback takes the mutex
 *     and writes only what is still armed and due. So a trailing byte the
 *     worker had already dispatched when a user write got in first finds
 *     nothing armed and writes nothing, and a stale `off` can never undo a
 *     write that means "hold this". The price, which the parallel port does not
 *     pay because outb cannot block: if the worker's trailing write is stuck
 *     in the driver (a wedged device, a peer that stopped draining), a user
 *     write waits behind it for up to write_timeout_ms, and the other way
 *     round. One byte stream, one writer at a time.
 *     yser_close() stops the worker before it closes anything and has it write
 *     any pending trailing byte immediately instead of waiting out the rest of
 *     the width, so a latching box is never left holding a trigger code
 *     (yrt_worker_close() runs a pending job at once, with a flush flag the
 *     callback reads). That one write happens with the writer mutex held, and
 *     yser_close() joins the worker thread behind it, so "at once" is about the
 *     deadline, not about the call returning: it is bounded by a fixed 50 ms
 *     and NOT by write_timeout_ms,
 *     so a wedged device delays a close by at most that, whatever
 *     write_timeout_ms says and even at YSER_TIMEOUT_INFINITE. A close with no
 *     pulse pending does not wait at all.
 *     yser_interrupt() is a reader-side wake and does not touch the worker.
 *     Nothing but yser_open and yser_close touches the message buffer, so the
 *     roles never race on it: yser_purge(YSER_PURGE_RX) on the reader and
 *     yser_send_break on the writer are safe at the same time, and the
 *     shutdown recipe below can call yser_interrupt while the writer is
 *     mid-pulse.
 *     Shutdown sequence for a blocked listener: yser_interrupt(p) from the
 *     main thread, join the listener (its yser_read returned
 *     YSER_ERR_INTERRUPTED), then yser_close(p). yser_close() also
 *     interrupts, but it cannot wait for the listener; closing a descriptor
 *     another thread is still using is undefined on POSIX.
 *
 *   ---------------------------------------------------------------------
 *   LOW LATENCY
 *   ---------------------------------------------------------------------
 *   desc.low_latency = true asks the driver for its minimum receive latency.
 *   It never fails yser_open(); yser_port.low_latency reports whether the
 *   request took effect.
 *
 *     Linux:   sets ASYNC_LOW_LATENCY via TIOCSSERIAL, then confirms the
 *              outcome by reading back the driver's latency_timer in sysfs,
 *              because cdc_acm accepts the ioctl and ignores the flag. Only
 *              ftdi_sio actually programs the timer (16 ms -> 1 ms). The same
 *              file can be set once per device by udev, which survives a
 *              re-plug and needs no privilege in the experiment:
 *                SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", \
 *                  ATTR{latency_timer}="1"
 *     Windows: the FTDI latency timer lives in the registry per device and
 *              needs a re-plug to apply, so it cannot be set at runtime here.
 *              Set it once in Device Manager (Port Settings > Advanced >
 *              Latency Timer = 1) or through the FTDI D2XX API.
 *     macOS:   the Apple FTDI driver has no user-settable latency timer; the
 *              FTDI vendor driver reads it from its plist.
 *
 *   ---------------------------------------------------------------------
 *   DTR AND DEVICE RESET
 *   ---------------------------------------------------------------------
 *   Opening a port asserts DTR and RTS on every platform (the pyserial
 *   default) and closing drops DTR. Arduino-class boards reset on a DTR
 *   edge, then spend about 2 s in the bootloader ignoring you. Options:
 *     desc.dtr_low_on_open      keep DTR low from the first moment the
 *                               library controls it, which is AFTER open().
 *                               On Linux this cannot stop a USB-serial board
 *                               from resetting: the kernel raises DTR and RTS
 *                               inside open() itself, O_NONBLOCK included, so
 *                               the edge has already happened and this option
 *                               only drops the line again afterwards. Use
 *                               keep_dtr_on_close in the session before, or
 *                               wait out the bootloader.
 *     desc.keep_dtr_on_close    leave DTR asserted after yser_close()
 *                               (clears HUPCL on POSIX), so the NEXT open
 *                               sees no edge. The reliable software fix for
 *                               "do not reset my box between sessions".
 *                               POSIX only: Windows drops DTR inside
 *                               CloseHandle and offers no way to stop it.
 *   Stale bytes the device sent before the open (its boot banner) stay in
 *   the driver buffer; call yser_purge(p, YSER_PURGE_RX) before you start.
 *
 *   ---------------------------------------------------------------------
 *   DEVICE NAMES
 *   ---------------------------------------------------------------------
 *     Windows: "COM3". Names above COM9 need the device-namespace prefix
 *              \\.\COM12 (spelled "\\\\.\\COM12" in C source). yser_open()
 *              adds the prefix for you when the name is a bare "COMn".
 *     Linux:   "/dev/ttyUSB0" (USB-serial bridge), "/dev/ttyACM0" (USB-CDC:
 *              Arduino, Teensy, most current trigger boxes), "/dev/ttyS0"
 *              (legacy UART). In experiment scripts prefer the stable
 *              "/dev/serial/by-id/usb-<vendor>_<product>_<serial>-if00-port0"
 *              symlinks, or yser_find_ports(); ttyUSB numbering changes
 *              between plug-ins.
 *     macOS:   "/dev/cu.usbserial-XXXX" or "/dev/cu.usbmodemXXXX". Use the
 *              cu.* (call-out) node, not tty.*, which blocks on carrier.
 *
 *   ---------------------------------------------------------------------
 *   POSIX WAITS
 *   ---------------------------------------------------------------------
 *   Every wait in this library is against a deadline on a non-blocking
 *   descriptor, but the call that does the waiting differs by platform.
 *     Linux:   poll(), on the port descriptor and the interrupt pipe.
 *     macOS:   select(). Apple's poll(2) still carries the note that it does
 *              not support devices, and a serial port is a device; pyserial
 *              uses select() on POSIX for the same reason. The cost is
 *              FD_SETSIZE: select() cannot name a descriptor at or above it,
 *              so yser_open() refuses the port with a message rather than
 *              corrupting the stack, in the unlikely case that a process has
 *              that many files open.
 *   Neither path has run on a Mac yet. If a macOS reader never wakes, or
 *   wakes without data, this is the first thing to check.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   ysp/rt.h must be on the include path; nothing else is added to the build.
 *   Link -pthread on POSIX for the async-pulse worker and for ysp/rt.h's
 *   scheduling calls; Windows needs no extra library for either.
 *   Define YSER_NO_THREADS to drop the worker, and with it yser_pulse_async(),
 *   which then reports YSER_ERR_IO. YSER_NO_THREADS also defines
 *   YRT_NO_THREADS before ysp/rt.h is included, so the two headers agree
 *   about whether yrt_worker exists. The coupling has one direction only: if
 *   YOU include ysp/rt.h before ysp/serial.h, define both macros or neither,
 *   because by then ysp/rt.h's declarations are already fixed.
 *   The Linux feature-test macro has no such ordering rule. Both headers ask
 *   for _DEFAULT_SOURCE on their first include and both stand down when any
 *   of _DEFAULT_SOURCE, _GNU_SOURCE, _POSIX_C_SOURCE or _XOPEN_SOURCE is
 *   already set, so either include order gives the same translation unit.
 *   The one rule that remains is the usual one: an implementation file that
 *   includes OTHER system headers before these defines _DEFAULT_SOURCE
 *   itself.
 *   No other libraries on POSIX. The Windows port enumeration links setupapi
 *   and advapi32: MSVC gets both from a #pragma comment in the
 *   implementation, and CMake consumers of ysp::ysp already get setupapi from
 *   the target, so only a hand-written MinGW command line needs -lsetupapi
 *   (advapi32 is already a MinGW default). The implementation raises
 *   _WIN32_WINNT to 0x0600 if it is lower, for CancelIoEx. Define YSER_API to
 *   override the default `extern` linkage. It sits on the definitions as well
 *   as the declarations, so -DYSER_API=static gives one translation unit a
 *   private copy of the library (expect -Wunused-function for whatever that
 *   unit does not call), and a decl-spec such as __declspec(dllexport)
 *   reaches both.
 *
 *       cc -O2 -Iinclude -o serial_trigger examples/serial/trigger.c
 *       cl /O2 /Iinclude examples\serial\trigger.c
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_SERIAL_H_INCLUDED
#define YSP_SERIAL_H_INCLUDED

/* CancelIoEx (Vista) is what lets yser_interrupt() end a pending overlapped
 * read. The MSVC SDK targets a recent Windows by default, but MinGW headers
 * still default to an older _WIN32_WINNT and would hide the declaration.
 * Raised here, before the first system header, on the same terms as the Linux
 * feature macro below. */
#if defined(YSP_SERIAL_IMPLEMENTATION) && defined(_WIN32) && \
    (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600)
    #undef  _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
#endif

/* Feature-test macro for the Linux implementation. glibc hides CRTSCTS,
 * CMSPAR and every B* rate above 38400 behind __USE_MISC, and realpath()
 * behind more than strict C. Defined here, before the first system header, so
 * it takes effect when this header is the translation unit's first include.
 * If you include other system headers first, define _DEFAULT_SOURCE (or
 * _GNU_SOURCE) yourself.
 *
 * All three ysp headers ask for the same macro on Linux and each stands down
 * once it is set, so the include order between them does not matter:
 * whichever comes first settles it. ysp/rt.h asks on its FIRST include,
 * implementation or not, which is what keeps __USE_MISC in place for a
 * translation unit that includes ysp/rt.h before this header. */
#if defined(YSP_SERIAL_IMPLEMENTATION) && defined(__linux__) && \
    !defined(_DEFAULT_SOURCE) && !defined(_GNU_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif

/* One build flag, two headers: ysp/rt.h owns the worker thread now, so its
 * declarations have to disappear on the same terms as yser_pulse_async's.
 * This only works while ysp/serial.h is the first of the two to be included;
 * see BUILDING. */
#if defined(YSER_NO_THREADS) && !defined(YRT_NO_THREADS)
    #define YRT_NO_THREADS
#endif

/* The dependency. yser_desc.sched, yser_port.async_policy and yser_now_us()
 * are ysp/rt.h's types and ysp/rt.h's clock, so it is included from the public
 * section, not just from the implementation. */
#include "ysp/rt.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linkage of every public function, on the declarations below and on their
 * definitions in the implementation. Both, so that -DYSER_API=static really
 * does make the library private to one translation unit; with the keyword on
 * the declarations alone the definitions would still be external and the
 * compiler would reject the pair. */
#ifndef YSER_API
#define YSER_API extern
#endif

#define YSER_DEFAULT_BAUD             115200u
#define YSER_DEFAULT_WRITE_TIMEOUT_MS 1000u
#define YSER_TIMEOUT_INFINITE         0xFFFFFFFFu

/* Negative return codes of the data-path functions. See RETURN VALUES. */
#define YSER_ERR_IO           (-1)
#define YSER_ERR_DISCONNECTED (-2)
#define YSER_ERR_CLOSED       (-3)
#define YSER_ERR_INTERRUPTED  (-4)
#define YSER_ERR_ARG          (-5)

typedef enum yser_parity {
    YSER_PARITY_NONE = 0,
    YSER_PARITY_ODD,
    YSER_PARITY_EVEN,
    YSER_PARITY_MARK,   /* Windows and Linux only; yser_open() rejects it on macOS */
    YSER_PARITY_SPACE   /* Windows and Linux only; yser_open() rejects it on macOS */
} yser_parity;

typedef enum yser_stop_bits {
    YSER_STOP_BITS_1 = 0,
    YSER_STOP_BITS_2
} yser_stop_bits;

typedef enum yser_flow {
    YSER_FLOW_NONE = 0,
    YSER_FLOW_RTSCTS,   /* hardware handshake on the RTS/CTS lines           */
    YSER_FLOW_XONXOFF   /* software handshake; 0x11/0x13 become control codes */
} yser_flow;

/* Modem input lines, as a bit mask from yser_get_lines(). */
#define YSER_LINE_CTS 0x01
#define YSER_LINE_DSR 0x02
#define YSER_LINE_RI  0x04
#define YSER_LINE_DCD 0x08

/* Buffer selectors for yser_purge(). */
#define YSER_PURGE_RX 0x01
#define YSER_PURGE_TX 0x02

/* Flags for yser_read(). */
#define YSER_READ_ANY 0x00  /* return on the first byte(s) available            */
#define YSER_READ_ALL 0x01  /* keep reading until `cap` bytes or the timeout    */

/* Real-time scheduling reservation for the async-pulse worker (Linux
 * SCHED_DEADLINE, macOS THREAD_TIME_CONSTRAINT_POLICY). ysp/rt.h's type and
 * ysp/rt.h's meaning, kept under the old name so existing call sites compile;
 * yrt_sched_deadline is the spelling to prefer in new code. Times are in
 * nanoseconds. They do NOT set the pulse width (that is clock-driven); they
 * govern how fast and how predictably the worker gets the CPU when it wakes to
 * write the trailing byte, which is edge jitter.
 *
 *   runtime_ns  - CPU budget guaranteed (and capped) per period.
 *   deadline_ns - relative deadline; smaller = higher EDF priority = lower
 *                 wake latency, but harder to admit next to other RT tasks.
 *   period_ns   - replenishment interval; reserved bandwidth = runtime/period.
 *
 * Leave all three zero for the defaults below. If you set any field, require
 * 0 < runtime_ns <= deadline_ns <= period_ns (a zero period_ns is taken as
 * "same as deadline_ns"); yser_open() rejects anything else, through
 * yrt_sched_normalize(). A valid reservation the kernel cannot admit, and
 * any value on Windows, falls back down the ladder without failing the open. */
typedef yrt_sched_deadline yser_sched_deadline;

#define YSER_DEFAULT_RT_RUNTIME_NS   YRT_DEFAULT_RT_RUNTIME_NS  /*  1 ms */
#define YSER_DEFAULT_RT_DEADLINE_NS  YRT_DEFAULT_RT_DEADLINE_NS /* 10 ms */
#define YSER_DEFAULT_RT_PERIOD_NS    YRT_DEFAULT_RT_PERIOD_NS   /* 10 ms */

/* Scheduling policy the async-pulse worker actually obtained: ysp/rt.h's
 * ladder, ysp/rt.h's enum. Asking for SCHED_DEADLINE is not getting it: the
 * kernel refuses it without CAP_SYS_NICE, and also when the thread's affinity
 * is not the full root domain (taskset, cpusets, an isolcpus-pinned experiment
 * process), and the library then falls back. Read yser_port.async_policy after
 * yser_open() and log it next to your timing data; jitter numbers without it
 * are unlabeled. yrt_policy_name() prints it.
 *
 * The YSER_ASYNC_* names below are aliases of the yrt_policy enumerators
 * they always described. Their NUMERIC values changed in v0.4, because
 * yrt_policy carries a macOS rung this header never had, so async_policy can
 * now also be YRT_POLICY_TIME_CONSTRAINT and nothing may store these
 * integers across a rebuild. */
typedef yrt_policy yser_async_policy;

#define YSER_ASYNC_NONE          YRT_POLICY_NONE          /* no worker      */
#define YSER_ASYNC_DEADLINE      YRT_POLICY_DEADLINE      /* SCHED_DEADLINE */
#define YSER_ASYNC_FIFO          YRT_POLICY_FIFO          /* SCHED_FIFO 80  */
#define YSER_ASYNC_TIME_CRITICAL YRT_POLICY_TIME_CRITICAL /* Windows        */
#define YSER_ASYNC_NORMAL        YRT_POLICY_NORMAL        /* nothing granted */

/* Open description. Use a designated initializer and set only what you need
 * (yser_desc d = { .device = "COM3" };); zero fields take the defaults noted
 * here. `device` is the one required field.
 *
 * Any baud rate is passed to the driver as given. A rate the driver rejects
 * fails yser_open() with the OS error; on POSIX that means rates without a
 * termios B* constant, except that macOS accepts any rate via IOSSIOSPEED.
 *
 * USB-CDC devices (ttyACM, usbmodem) ignore baud, data bits, parity and stop
 * bits: the USB pipe carries bytes, not a UART line. The values are still
 * applied so the driver does not reject the open. */
typedef struct yser_desc {
    const char*    device;            /* REQUIRED: "COM3", "/dev/ttyUSB0", "/dev/cu.usbmodem14101" */
    uint32_t       baud;              /* default YSER_DEFAULT_BAUD                        */
    uint8_t        data_bits;         /* 5..8, default 8                                  */
    yser_parity    parity;            /* default YSER_PARITY_NONE                         */
    yser_stop_bits stop_bits;         /* default YSER_STOP_BITS_1                         */
    yser_flow      flow;              /* default YSER_FLOW_NONE                           */
    uint32_t       write_timeout_ms;  /* default 1000 (0 = default; the minimum is 1);
                                       * YSER_TIMEOUT_INFINITE blocks                    */
    bool           exclusive;         /* POSIX: TIOCEXCL, refuse other opens. Windows
                                       * ports are always exclusive.                     */
    bool           low_latency;       /* request the driver's minimum RX latency;
                                       * see LOW LATENCY above                           */
    bool           dtr_low_on_open;   /* see DTR AND DEVICE RESET                         */
    bool           rts_low_on_open;
    bool           keep_dtr_on_close;
    yser_sched_deadline sched;        /* async-pulse worker RT params;
                                       * all-zero = defaults                   */
} yser_desc;

/* Port handle. Allocate it (stack/heap) zeroed or closed, pass to yser_open(),
 * and treat the platform block as opaque: it is public the way ysp/parallel.h
 * is public, for inspection, not for writing. The effective settings and the
 * three per-role OS error numbers are yours to read. Hot fields (touched by
 * every read and write) come first so they share a cache line; the two string
 * buffers come last.
 *
 * Every field is written once by yser_open() and then only by its own role,
 * which is what makes the threading promise hold. `async`, `sched` and
 * `async_policy` are set by yser_open() and cleared by yser_close(); on
 * Windows `pa_ovl` and `pa_event` belong to the worker thread alone. write_timeout_ms in
 * particular is immutable while the port is open: on Windows the reader
 * rewrites the whole COMMTIMEOUTS structure and reconstructs the writer's
 * constant from this field, so a caller that poked it would change the
 * writer's timeout behind its back. */
typedef struct yser_port {
    bool     is_open;
    void*    async;                 /* async-pulse worker, started by yser_open(),
                                     * NULL if none; tested by every write     */
#if defined(_WIN32)
    void*    handle;                /* HANDLE from CreateFile (FILE_FLAG_OVERLAPPED)         */
    void*    rd_event;              /* OVERLAPPED.hEvent, reader side                        */
    void*    wr_event;              /* OVERLAPPED.hEvent, writer side                        */
    void*    wake_event;            /* manual-reset event set by yser_interrupt()            */
    uint64_t rd_ovl[4];             /* OVERLAPPED storage, reader (kept in the handle so a   */
    uint64_t wr_ovl[4];             /* canceled I/O can never complete into a dead frame)   */
    uint64_t pa_ovl[4];             /* the same, for the async worker's trailing write: a
                                     * transfer is reaped by the thread that started it, so
                                     * the worker never borrows wr_ovl/wr_event             */
    void*    pa_event;              /* OVERLAPPED.hEvent, async worker                       */
    uint32_t rd_timeout_applied_ms; /* COMMTIMEOUTS currently set; skips redundant updates   */
#else
    int      fd;                    /* O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC, -1 if closed */
    int      wake_fd[2];            /* self-pipe: reader polls [0], yser_interrupt writes [1] */
#endif
    int      rd_oserr;              /* errno / GetLastError of the last reader-side failure   */
    int      wr_oserr;              /* same for the writer side                               */
    int      misc_oserr;            /* same for the any-role calls (interrupt, get_lines,
                                     * set_dtr, set_rts), which have no direction             */
    uint32_t write_timeout_ms;      /* immutable while open; see the comment above            */

    /* Effective settings, filled by yser_open() after defaults are applied. */
    uint32_t       baud;
    uint8_t        data_bits;
    yser_parity    parity;
    yser_stop_bits stop_bits;
    yser_flow      flow;
    bool           low_latency;     /* true if the driver accepted the request  */
    bool           keep_dtr_on_close;
    yser_sched_deadline sched;      /* effective worker reservation            */
    yser_async_policy   async_policy; /* rung the worker got; final once
                                       * yser_open() returns                   */

    char     device[128];           /* name as opened (with any \\.\ prefix)   */
    char     error[256];            /* yser_open() failure message; see RETURN VALUES */
} yser_port;

/* One detected serial port, as returned by yser_list_ports(). Fields the
 * platform cannot determine are "" or 0. */
typedef struct yser_port_info {
    char     name[128];         /* openable name: "COM3", "/dev/ttyUSB0", "/dev/cu.usbserial-A5XK3RJT" */
    char     description[128];  /* driver/product string, e.g. "USB Serial Port (COM3)"                */
    char     serial_number[64]; /* USB serial number, for matching a specific box                       */
    char     location[32];      /* bus position, e.g. "1-1.4:1.0" (Linux) or "Port_#0003.Hub_#0001"
                                 * (Windows); tells apart the ports of a multi-port bridge
                                 * (FT2232/FT4232 share one serial number). On Windows an FTDI port
                                 * letter wins the field ("A"), so two EEPROM-blank FT232R cables
                                 * report the same location and can only be told apart by unplugging
                                 * one. Linux has no such gap.                                          */
    uint16_t vid;               /* USB vendor id (0x0403 FTDI, 0x10C4 Silicon Labs, 0x2341 Arduino, ...) */
    uint16_t pid;               /* USB product id                                                       */
} yser_port_info;

/* Match criteria for yser_find_ports(). Zero/NULL fields match anything, so a
 * zeroed filter lists every port, and { .vid = 0x0403, .pid = 0x6001 } finds
 * every FT232R without knowing its serial number. Cedrus and many DIY boxes
 * ship the FTDI default vid/pid, so on a rig with two FTDI cables add
 * serial_number or location. */
typedef struct yser_port_filter {
    uint16_t    vid;            /* 0 = any                                    */
    uint16_t    pid;            /* 0 = any                                    */
    const char* serial_number;  /* NULL = any; case-insensitive exact match   */
    const char* location;       /* NULL = any; case-insensitive prefix match  */
    const char* description;    /* NULL = any; case-insensitive substring     */
    const char* name;           /* NULL = any; case-insensitive substring     */
} yser_port_filter;

/* --- discovery ---------------------------------------------------------- */

/* Enumerate serial ports the system exposes, without opening any. Writes up
 * to `max` entries into `out` (pass out=NULL/max=0 to just count) and returns
 * the total number found, which may exceed `max`, or YSER_ERR_IO if the
 * platform enumeration itself failed.
 *
 *   Windows: SetupAPI over GUID_DEVINTERFACE_COMPORT; vid/pid from the
 *            hardware id, serial and location from the instance id (FTDI
 *            instance ids carry the serial plus an interface letter; the
 *            letter goes to location).
 *   Linux:   /sys/class/tty entries that have a `device` link (real
 *            hardware, no virtual consoles); USB fields from the parent
 *            sysfs node (idVendor, idProduct, product, serial), location
 *            from the interface directory name.
 *   macOS:   /dev/cu.* nodes, names only. The USB fields stay empty: they
 *            would need IOKit, and linking two frameworks into every
 *            implementation translation unit is not worth a setup-time
 *            convenience. FTDI and Apple name the nodes after the serial
 *            number ("/dev/cu.usbserial-A5XK3RJT"), so filter on `name`.
 *
 * Typical use:
 *   int n = yser_list_ports(NULL, 0);
 *   yser_port_info* v = malloc(n * sizeof *v);
 *   yser_list_ports(v, n);
 */
YSER_API int yser_list_ports(yser_port_info* out, int max);

/* Enumerate only the ports that match `filter` (NULL = all, same as
 * yser_list_ports). Same out/max/return contract. Two identical boxes both
 * match a vid/pid filter: add serial_number or location to pick one, or take
 * the array and choose. Platform-independent, built on yser_list_ports().
 *
 *   yser_port_filter f = { .vid = 0x0403, .pid = 0x6001 };
 *   yser_port_info info;
 *   if (yser_find_ports(&f, &info, 1) == 1) desc.device = info.name;
 */
YSER_API int yser_find_ports(const yser_port_filter* filter, yser_port_info* out, int max);

/* --- lifecycle ---------------------------------------------------------- */

/* Open and configure a port per `desc`. Returns true on success; on failure
 * returns false and leaves a message in yser_error(). Validates every enum
 * and range before touching the OS. `p` must be zeroed or closed (see the
 * struct comment); a handle whose is_open flag is still set is refused with a
 * message rather than reopened, because reopening it would lose the old
 * descriptor and orphan the old worker thread. A handle that was never zeroed
 * has no valid flag, so that check cannot save a caller who ignores the
 * contract in the other direction.
 *
 * It also starts the async-pulse worker, so yser_port.async_policy is final
 * before the first trial and no pulse pays for thread creation. A worker that
 * cannot start is not a failed open: this returns true, async_policy stays
 * YSER_ASYNC_NONE, yser_error() describes why, and yser_pulse_async() reports
 * YSER_ERR_IO. Everything else on the handle works. */
YSER_API bool yser_open(yser_port* p, const yser_desc* desc);

/* Interrupt, release and close the port. Safe to call on a zeroed or
 * already-closed handle. Both the reader and the writer must have returned;
 * see THREADING. A pulse still pending has its trailing byte written first,
 * immediately rather than at its deadline, so this can block for up to 50 ms
 * on a wedged device; it never waits out write_timeout_ms. */
YSER_API void yser_close(yser_port* p);

/* Wake a yser_read() blocked on another thread; it returns
 * YSER_ERR_INTERRUPTED. Callable from any thread. The wake is latched, not
 * queued: it stays pending until one yser_read() consumes it (that read
 * returns YSER_ERR_INTERRUPTED at once, even if it started later), and
 * several interrupts collapse into one. Latching is what makes the shutdown
 * sequence in THREADING race-free; a wake dropped because the listener
 * happened to be between reads would hang the join. Returns 0, or a
 * YSER_ERR_* code with the OS error in misc_oserr. */
YSER_API int yser_interrupt(yser_port* p);

/* yser_open()'s failure message for this handle ("" if none). */
YSER_API const char* yser_error(const yser_port* p);

/* Static description of a YSER_ERR_* code ("ok" for values >= 0). */
YSER_API const char* yser_strerror(int code);

/* True if the handle currently owns an open port. */
YSER_API bool yser_is_open(const yser_port* p);

/* Monotonic microseconds: yrt_now_us(), which is CLOCK_MONOTONIC on POSIX
 * and QueryPerformanceCounter on Windows. Use it to bracket writes. It is the
 * same call, not a second clock that agrees with ysp/rt.h's, so the one base
 * this collection promises holds by construction. */
YSER_API uint64_t yser_now_us(void);

/* --- output (writer role) ---------------------------------------------- */

/* Write `len` bytes. Blocks until the driver accepted all of them or
 * write_timeout_ms elapsed. Returns the count accepted (== len on success,
 * fewer on timeout) or a YSER_ERR_* code. */
YSER_API int yser_write(yser_port* p, const void* buf, int len);

/* Write one byte. The trigger-output workhorse. Returns 1, 0 on timeout, or
 * a YSER_ERR_* code. */
YSER_API int yser_write_byte(yser_port* p, uint8_t value);

/* Block until the driver's transmit buffer is empty (tcdrain /
 * FlushFileBuffers). On USB-serial this means "handed to the bridge chip",
 * not "on the wire". Returns 0 or a YSER_ERR_* code. */
YSER_API int yser_drain(yser_port* p);

/* Discard buffered bytes: `which` is YSER_PURGE_RX (reader role),
 * YSER_PURGE_TX (writer role) or both (single-threaded use only). Returns 0
 * or a YSER_ERR_* code; the OS error goes to the slot of the role that owns
 * the direction, and to wr_oserr when both are purged. */
YSER_API int yser_purge(yser_port* p, int which);

/* Write `on`, hold for `usec` microseconds, then write `off`. The two-byte
 * trigger pulse for boxes that latch the last byte received (TriggerBox,
 * BioSemi, DIY): on = code, off = 0x00. Do not use it with a command
 * protocol such as XID, where 0x00 is a protocol byte. Blocking; the hold
 * starts when the first write is accepted by the driver (see TIMING). The
 * wait is yrt_sleep_until() with the platform's default spin window, so the
 * calling thread sleeps for most of the width and spins only the tail.
 * Returns 2 on success, the count of bytes accepted on timeout, or a
 * YSER_ERR_* code. */
YSER_API int yser_pulse(yser_port* p, uint8_t on, uint8_t off, uint32_t usec);

/* Non-blocking pulse. Writes `on` on the calling thread and returns at once;
 * the async-pulse worker writes `off` at an absolute deadline taken right
 * after the onset write, so the width does not include this call's own cost
 * and the caller does not wait it out. Use it when the experiment has
 * something better to do than hold still for a trigger width, and read
 * TIMING for what the trailing edge is worth on each platform.
 *
 * Returns 1 (the onset byte was accepted), 0 if the onset write timed out, or
 * a YSER_ERR_* code; YSER_ERR_IO with wr_oserr == 0 means there is no worker
 * (YSER_NO_THREADS, or yser_open() could not start one). A timed-out or failed
 * onset schedules no trailing byte.
 *
 * Writer role, like yser_write. The worker keeps ONE pending trailing byte per
 * handle: a second yser_pulse_async() while one is pending moves that single
 * edge to the new time and the new `off`, and any plain yser_write /
 * yser_write_byte / yser_pulse cancels it, because a write means "hold this"
 * and a stale `off` must not undo it. See THREADING for the shared-lock cost
 * and yser_close()'s flush. */
YSER_API int yser_pulse_async(yser_port* p, uint8_t on, uint8_t off, uint32_t usec);

/* --- input (reader role) ----------------------------------------------- */

/* Read up to `cap` bytes. With flags = YSER_READ_ANY (0): returns as soon as
 * at least one byte is available, with everything buffered at that moment;
 * 0 if nothing arrived within timeout_ms (0 = poll, YSER_TIMEOUT_INFINITE =
 * block). With YSER_READ_ALL: keeps reading until `cap` bytes were read or
 * timeout_ms elapsed in total, and returns the count actually read (== cap
 * on success, fewer on timeout). Negative: YSER_ERR_DISCONNECTED,
 * YSER_ERR_INTERRUPTED, YSER_ERR_CLOSED, YSER_ERR_IO.
 * A negative return discards the count: YSER_READ_ALL interrupted or
 * disconnected part way through a frame returns the code, and the bytes
 * already in `buf` are neither counted nor reported. That is deliberate,
 * because a half frame is not usable and yser_interrupt() means shutdown, not
 * "give me what you have". Use YSER_READ_ANY in a loop if you need to keep
 * partial input. */
YSER_API int yser_read(yser_port* p, void* buf, int cap, uint32_t timeout_ms, int flags);

/* Bytes waiting in the driver's receive buffer, or a YSER_ERR_* code. */
YSER_API int yser_available(yser_port* p);

/* --- modem lines (any role, one call at a time) -------------------------- */

/* Drive the DTR / RTS output lines. RTS is ignored while YSER_FLOW_RTSCTS is
 * active (the driver owns it). Return 0 or a YSER_ERR_* code, with the OS
 * error in misc_oserr. */
YSER_API int yser_set_dtr(yser_port* p, bool on);
YSER_API int yser_set_rts(yser_port* p, bool on);

/* Read the input lines as a YSER_LINE_* mask, or a YSER_ERR_* code with the
 * OS error in misc_oserr. */
YSER_API int yser_get_lines(yser_port* p);

/* Hold the TX line in the break (space) state for `ms` milliseconds (writer
 * role; sleeps, does not spin). Returns 0 or a YSER_ERR_* code with the OS
 * error in wr_oserr. */
YSER_API int yser_send_break(yser_port* p, uint32_t ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_SERIAL_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_SERIAL_IMPLEMENTATION
#ifndef YSP_SERIAL_IMPLEMENTATION_GUARD
#define YSP_SERIAL_IMPLEMENTATION_GUARD

/* ysp/rt.h's implementation, unless this translation unit already has it.
 * Its implementation block sits outside its header guard and carries a guard
 * of its own, so a unit that also defines YSP_RT_IMPLEMENTATION, or that also
 * implements ysp/parallel.h, still ends up with exactly one copy. */
#ifndef YSP_RT_IMPLEMENTATION_GUARD
    #define YSP_RT_IMPLEMENTATION
    #include "ysp/rt.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>   /* malloc: scratch list in yser_find_ports; strtoul */
#include <ctype.h>    /* tolower: case-insensitive filter matching */

/* Pick the concrete platform once. */
#if defined(_WIN32)
    #define YSER__WINDOWS 1
#elif defined(__linux__) || defined(__APPLE__)
    #define YSER__POSIX 1
#else
    #error "ysp_serial: unsupported platform (need Windows, Linux or macOS)"
#endif

#if defined(YSER__WINDOWS)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <setupapi.h>   /* port enumeration only */
    #if defined(_MSC_VER)
        #pragma comment(lib, "setupapi")  /* MinGW: link -lsetupapi yourself */
        #pragma comment(lib, "advapi32")  /* RegQueryValueEx, for PortName   */
    #endif
    /* OVERLAPPED lives in the public handle as opaque storage. The third form
     * covers MSVC's legacy C dialect, which is what setuptools and MATLAB's
     * mex compile with unless told otherwise; a negative array bound fails
     * the same way, with a worse message. */
    #if defined(__cplusplus)
        static_assert(sizeof(OVERLAPPED) <= 4 * sizeof(uint64_t),
                      "yser_port.rd_ovl is too small for OVERLAPPED");
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
        _Static_assert(sizeof(OVERLAPPED) <= 4 * sizeof(uint64_t),
                       "yser_port.rd_ovl is too small for OVERLAPPED");
    #else
        typedef char yser__ovl_fits[sizeof(OVERLAPPED) <= 4 * sizeof(uint64_t) ? 1 : -1];
    #endif
#else
    #include <errno.h>
    #include <time.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <termios.h>
    #include <poll.h>
    #include <dirent.h>
    #include <limits.h>
    #include <sys/ioctl.h>
    #if defined(__linux__)
        #include <linux/serial.h>   /* TIOCGSERIAL, ASYNC_LOW_LATENCY */
    #endif
    #if defined(__APPLE__)
        #include <sys/select.h>   /* Darwin waits with select(); see POSIX WAITS */
        /* IOSSIOSPEED is spelled out instead of including <IOKit/serial/ioss.h>:
         * the ioctl number is stable ABI, and the header would drag the IOKit
         * SDK into every translation unit that defines the implementation. */
        #ifndef IOSSIOSPEED
            #define IOSSIOSPEED _IOW('T', 2, speed_t)
        #endif
    #endif
#endif

/* --- shared helpers ----------------------------------------------------- */

static void yser__set_error(yser_port* p, const char* fmt, ...) {
    if (!p) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error, sizeof(p->error), fmt, ap);
    va_end(ap);
}

YSER_API const char* yser_error(const yser_port* p) {
    return p ? p->error : "null port handle";
}

YSER_API const char* yser_strerror(int code) {
    switch (code) {
        case YSER_ERR_IO:           return "I/O error (see rd_oserr / wr_oserr)";
        case YSER_ERR_DISCONNECTED: return "device disconnected";
        case YSER_ERR_CLOSED:       return "port not open";
        case YSER_ERR_INTERRUPTED:  return "wait interrupted";
        case YSER_ERR_ARG:          return "bad argument";
        default:                    return code >= 0 ? "ok" : "unknown error";
    }
}

YSER_API bool yser_is_open(const yser_port* p) {
    return p && p->is_open;
}

/* How long yser_close() lets the worker's flush of a pending trailing byte
 * block. The flush runs with the writer mutex held and yser_close() joins the
 * worker thread behind it, so write_timeout_ms would make a wedged device hold
 * a close for a second, and YSER_TIMEOUT_INFINITE would hold it forever. A healthy device
 * accepts one byte in microseconds, so this bound only ever fires on a device
 * that is already lost. */
#define YSER__CLOSE_FLUSH_MS 50u

/* Per-platform write that assumes an open port and a validated length, takes
 * no writer mutex, and gives up after timeout_ms. The public yser_write()
 * wraps it with the mutex and the pending-pulse cancel. */
static int yser__write_timed(yser_port* p, const void* buf, int len, uint32_t timeout_ms);

/* The ordinary writer-role write: the port's own timeout, as documented. */
static int yser__write_nolock(yser_port* p, const void* buf, int len) {
    return yser__write_timed(p, buf, len, p->write_timeout_ms);
}

#ifndef YSER_NO_THREADS
/* Defined in the ASYNC PULSE WORKER section below. */
static bool yser__async_start(yser_port* p);
static void yser__async_stop(yser_port* p);
static int  yser__write_cancel(yser_port* p, const void* buf, int len);
static int  yser__async_submit(yser_port* p, uint8_t on, uint8_t off, uint32_t usec);
#endif

/* One clock for the library, the caller and ysp/rt.h. A wrapper rather than a
 * second reader of the same OS counter: two copies of this conversion are two
 * things to keep in agreement, and the manual promises one base. */
YSER_API uint64_t yser_now_us(void) {
    return (uint64_t)yrt_now_ns() / 1000u;
}

/* Case-insensitive helpers; MSVC has no strcasestr/strcasecmp. */
static bool yser__ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static bool yser__iprefix(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    return true;
}

static bool yser__icontains(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (; *hay; hay++)
        if (yser__iprefix(hay, needle)) return true;
    return false;
}

/* Bounded copy and append. Truncation is the intended behavior for the fixed
 * yser_port_info fields, and snprintf("%s", ...) on a source the compiler can
 * see is longer trips -Wformat-truncation. */
static void yser__copy(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void yser__append(char* dst, size_t cap, const char* src) {
    size_t n = strlen(dst);
    if (n + 1 < cap) yser__copy(dst + n, cap - n, src);
}

/* Milliseconds left until an absolute deadline, rounded up so a
 * sub-millisecond remainder still waits once instead of degrading into a
 * poll. Clamped to INT32_MAX because poll() and select() take an int and a
 * struct timeval built from one. */
static uint32_t yser__remaining_ms(uint64_t deadline_us) {
    uint64_t now = yser_now_us();
    if (now >= deadline_us) return 0;
    uint64_t ms = (deadline_us - now + 999ull) / 1000ull;
    return ms > 0x7FFFFFFFull ? 0x7FFFFFFFu : (uint32_t)ms;
}

/* Compare device names with digit runs read as numbers, so COM3 sorts before
 * COM10 and ttyUSB2 before ttyUSB10. Plain strcmp orders them the other way,
 * and then "the first port" means something different on a machine with ten
 * of them. */
static int yser__natcmp(const char* a, const char* b) {
    for (;;) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (a[0] == '0' && isdigit((unsigned char)a[1])) a++;
            while (b[0] == '0' && isdigit((unsigned char)b[1])) b++;
            const char* ea = a;
            const char* eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (ea - a) < (eb - b) ? -1 : 1;
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c != 0) return c;
            a = ea;
            b = eb;
            continue;
        }
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == '\0') return 0;
        a++;
        b++;
    }
}

/* Growing scratch list for yser_list_ports(). The platform code appends every
 * port it finds and the shared tail sorts the whole set before copying the
 * first `max` out, so a caller with a buffer smaller than the machine's port
 * count gets the same entries every time instead of whichever ones the
 * directory or SetupAPI happened to yield first. Enumeration is a setup-time
 * operation, so one allocation per call is not worth designing around. */
typedef struct yser__list {
    yser_port_info* v;
    int             count;
    int             cap;
    bool            oom;
} yser__list;

static yser_port_info* yser__list_add(yser__list* l) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        yser_port_info* v = (yser_port_info*)realloc(l->v, (size_t)cap * sizeof(*v));
        if (!v) { l->oom = true; return NULL; }
        l->v = v;
        l->cap = cap;
    }
    yser_port_info* e = &l->v[l->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static int yser__list_finish(yser__list* l, yser_port_info* out, int max) {
    int n = l->count;
    if (l->oom) { free(l->v); return YSER_ERR_IO; }
    for (int i = 1; i < n; i++) {   /* insertion sort: n is a handful */
        yser_port_info tmp = l->v[i];
        int k = i;
        while (k > 0 && yser__natcmp(l->v[k - 1].name, tmp.name) > 0) {
            l->v[k] = l->v[k - 1];
            k--;
        }
        l->v[k] = tmp;
    }
    /* No ports means l->v was never allocated; memcpy's pointer arguments
     * must be non-null even for a zero-length copy. */
    if (out && max > 0 && n > 0) {
        int k = n < max ? n : max;
        memcpy(out, l->v, (size_t)k * sizeof(*out));
    }
    free(l->v);
    return n;
}

/* ======================================================================= *
 *  WINDOWS  (Win32 COM API, overlapped I/O)
 * ======================================================================= */
#if defined(YSER__WINDOWS)

#define YSER__OVL(field) ((OVERLAPPED*)(void*)(field))

/* Errors a COM handle reports once the device itself is gone. A USB unplug
 * surfaces as any of these depending on the driver and on how far the I/O
 * got, and every one of them must become YSER_ERR_DISCONNECTED rather than a
 * timeout, or a listener spins on a dead port. */
static bool yser__gone(DWORD e) {
    switch (e) {
        case ERROR_ACCESS_DENIED:
        case ERROR_BAD_COMMAND:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_GEN_FAILURE:
        case ERROR_INVALID_HANDLE:
        case ERROR_NOT_READY:
#ifdef ERROR_DEVICE_REMOVED
        case ERROR_DEVICE_REMOVED:
#endif
            return true;
        default:
            return false;
    }
}

/* System message for a Win32 error code, trimmed of the trailing newline the
 * FormatMessage catalog carries. */
static void yser__errmsg(DWORD e, char* buf, size_t cap) {
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             buf, (DWORD)cap, NULL);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == '.'  || buf[n - 1] == ' ')) buf[--n] = '\0';
    if (n == 0) snprintf(buf, cap, "error %lu", (unsigned long)e);
}

/* Is the handle still usable? Asked after an aborted I/O to tell a purge or a
 * cancel (recoverable) from a device that vanished. */
static bool yser__port_alive(yser_port* p) {
    DWORD errors = 0;
    COMSTAT st;
    return ClearCommError((HANDLE)p->handle, &errors, &st) != 0;
}

/* --- discovery ---------------------------------------------------------- */

/* GUID_DEVINTERFACE_COMPORT, spelled out so the header needs neither
 * <devguid.h> nor the INITGUID dance. Only real serial ports carry it, unlike
 * the Ports device class, which also holds LPT. */
static const GUID yser__guid_comport =
    { 0x86E0D1E0u, 0x8089u, 0x11D0u, { 0x9Cu, 0xE4u, 0x08u, 0x00u, 0x3Eu, 0x30u, 0x1Fu, 0x73u } };

static uint16_t yser__hex4_after(const char* s, const char* tag) {
    size_t tlen = strlen(tag);
    for (; *s; s++) {
        if (!yser__iprefix(s, tag)) continue;
        return (uint16_t)strtoul(s + tlen, NULL, 16);
    }
    return 0;
}

static bool yser__devprop(HDEVINFO set, SP_DEVINFO_DATA* dev, DWORD prop,
                          char* out, size_t cap) {
    DWORD type = 0, got = 0;
    out[0] = '\0';
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, prop, &type, (PBYTE)out,
                                           (DWORD)(cap - 1), &got))
        return false;
    if (got >= cap) got = (DWORD)cap - 1;
    out[got] = '\0';
    return out[0] != '\0';
}

/* The instance id is the only place a USB serial number appears without
 * opening the device. Three shapes matter:
 *   USB\VID_2341&PID_0043\85735313234351C0   tail is the USB serial number
 *   USB\VID_1A86&PID_7523\5&2c7b7f6&0&2      tail is path-derived; '&' says so
 *   FTDIBUS\VID_0403+PID_6001+A5XK3RJTA\0000 serial plus one port letter
 * FTDI multi-port bridges (FT2232/FT4232) share a serial number and differ
 * only in that letter, so it becomes the location. */
static void yser__parse_instance_id(const char* id, yser_port_info* e) {
    const char* tail = strrchr(id, '\\');
    tail = tail ? tail + 1 : id;
    if (yser__iprefix(id, "FTDIBUS\\")) {
        const char* plus = strrchr(id, '+');
        if (plus) {
            size_t n = 0;
            plus++;
            while (plus[n] && plus[n] != '\\' && n + 1 < sizeof(e->serial_number)) n++;
            memcpy(e->serial_number, plus, n);
            e->serial_number[n] = '\0';
            if (n > 1 && e->serial_number[n - 1] >= 'A' && e->serial_number[n - 1] <= 'Z') {
                snprintf(e->location, sizeof(e->location), "%c", e->serial_number[n - 1]);
                e->serial_number[n - 1] = '\0';
            }
        }
    } else if (!strchr(tail, '&')) {
        yser__copy(e->serial_number, sizeof(e->serial_number), tail);
    }
}

YSER_API int yser_list_ports(yser_port_info* out, int max) {
    HDEVINFO set = SetupDiGetClassDevsA(&yser__guid_comport, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return YSER_ERR_IO;

    yser__list list;
    memset(&list, 0, sizeof(list));
    SP_DEVINFO_DATA dev;
    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        /* The openable name lives in the device's own registry key, not in any
         * SPDRP_* property. */
        char portname[64] = "";
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == (HKEY)INVALID_HANDLE_VALUE || key == NULL) continue;
        DWORD type = 0, cb = (DWORD)sizeof(portname) - 1;
        LONG rc = RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)portname, &cb);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || type != REG_SZ) continue;
        portname[cb < sizeof(portname) ? cb : sizeof(portname) - 1] = '\0';
        if (!yser__iprefix(portname, "COM")) continue; /* LPT via a COM interface: not ours */

        yser_port_info* e = yser__list_add(&list);
        if (!e) break;
        yser__copy(e->name, sizeof(e->name), portname);

        char buf[512];
        if (yser__devprop(set, &dev, SPDRP_FRIENDLYNAME, buf, sizeof(buf)) ||
            yser__devprop(set, &dev, SPDRP_DEVICEDESC, buf, sizeof(buf)))
            yser__copy(e->description, sizeof(e->description), buf);
        if (yser__devprop(set, &dev, SPDRP_HARDWAREID, buf, sizeof(buf))) {
            e->vid = yser__hex4_after(buf, "VID_");
            e->pid = yser__hex4_after(buf, "PID_");
        }
        char id[512] = "";
        if (SetupDiGetDeviceInstanceIdA(set, &dev, id, (DWORD)sizeof(id) - 1, NULL))
            yser__parse_instance_id(id, e);
        /* Hub and port numbers, for boxes with no serial number at all. An
         * FTDI port letter already claimed the field when present, so two
         * EEPROM-blank FT232R cables share a location; see the
         * yser_port_info.location comment. */
        if (!e->location[0] &&
            yser__devprop(set, &dev, SPDRP_LOCATION_INFORMATION, buf, sizeof(buf)))
            yser__copy(e->location, sizeof(e->location), buf);
    }
    SetupDiDestroyDeviceInfoList(set);
    return yser__list_finish(&list, out, max);
}

/* --- lifecycle ---------------------------------------------------------- */

static bool yser__open_fail(yser_port* p) {
    if (p->handle) CloseHandle((HANDLE)p->handle);
    if (p->rd_event) CloseHandle((HANDLE)p->rd_event);
    if (p->wr_event) CloseHandle((HANDLE)p->wr_event);
    if (p->wake_event) CloseHandle((HANDLE)p->wake_event);
    if (p->pa_event) CloseHandle((HANDLE)p->pa_event);
    p->handle = p->rd_event = p->wr_event = p->wake_event = p->pa_event = NULL;
    return false;
}

/* Read timeouts, in the one combination that gives "whatever is buffered now,
 * else the first byte within ms". SetCommTimeouts is port-global, so the write
 * constant is rewritten with the value yser_open() chose instead of being left
 * to whatever the driver defaulted to. On failure rd_oserr holds the Win32
 * code and the caller decides what it means: after an unplug this call is
 * where a reader first notices, and a disconnect must not be reported as an
 * I/O error. */
static bool yser__set_read_timeout(yser_port* p, uint32_t ms) {
    if (p->rd_timeout_applied_ms == ms) return true;
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof(t));
    t.ReadIntervalTimeout = MAXDWORD;
    if (ms > 0) {
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant   = ms;
    }
    t.WriteTotalTimeoutConstant =
        (p->write_timeout_ms == YSER_TIMEOUT_INFINITE) ? 0 : p->write_timeout_ms;
    if (!SetCommTimeouts((HANDLE)p->handle, &t)) {
        p->rd_oserr = (int)GetLastError();
        return false;
    }
    p->rd_timeout_applied_ms = ms;
    return true;
}

static bool yser__open_platform(yser_port* p, const yser_desc* d) {
    HANDLE h = CreateFileA(p->device, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        char msg[256];
        yser__errmsg(GetLastError(), msg, sizeof(msg));
        yser__set_error(p, "CreateFile(%s): %s", p->device, msg);
        return false;
    }
    p->handle = (void*)h;

    /* Advisory: drivers may clamp or ignore it, and a failure here is not a
     * reason to refuse the port. */
    SetupComm(h, 4096, 4096);

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        char msg[256];
        yser__errmsg(GetLastError(), msg, sizeof(msg));
        yser__set_error(p, "GetCommState(%s): %s (not a serial port?)", p->device, msg);
        return yser__open_fail(p);
    }
    dcb.BaudRate = (DWORD)p->baud;
    dcb.ByteSize = p->data_bits;
    dcb.fBinary  = TRUE;   /* the only mode Windows supports */
    dcb.fNull    = FALSE;  /* keep incoming 0x00, it is a trigger code */
    dcb.fAbortOnError = FALSE;
    dcb.fErrorChar    = FALSE;
    switch (p->parity) {
        case YSER_PARITY_ODD:   dcb.Parity = ODDPARITY;   break;
        case YSER_PARITY_EVEN:  dcb.Parity = EVENPARITY;  break;
        case YSER_PARITY_MARK:  dcb.Parity = MARKPARITY;  break;
        case YSER_PARITY_SPACE: dcb.Parity = SPACEPARITY; break;
        default:                dcb.Parity = NOPARITY;    break;
    }
    dcb.fParity   = (p->parity != YSER_PARITY_NONE);
    dcb.StopBits  = (p->stop_bits == YSER_STOP_BITS_2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fDtrControl = d->dtr_low_on_open ? DTR_CONTROL_DISABLE : DTR_CONTROL_ENABLE;
    if (p->flow == YSER_FLOW_RTSCTS) {
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
        dcb.fOutxCtsFlow = TRUE;
    } else {
        dcb.fRtsControl = d->rts_low_on_open ? RTS_CONTROL_DISABLE : RTS_CONTROL_ENABLE;
        dcb.fOutxCtsFlow = FALSE;
    }
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = dcb.fInX = (p->flow == YSER_FLOW_XONXOFF);
    dcb.fTXContinueOnXoff = TRUE;
    dcb.XonChar  = 0x11;
    dcb.XoffChar = 0x13;
    dcb.XonLim   = 2048;
    dcb.XoffLim  = 512;
    if (!SetCommState(h, &dcb)) {
        char msg[256];
        yser__errmsg(GetLastError(), msg, sizeof(msg));
        yser__set_error(p, "SetCommState(%s): %s (baud %u, %u data bits?)",
                        p->device, msg, p->baud, (unsigned)p->data_bits);
        return yser__open_fail(p);
    }

    p->rd_timeout_applied_ms = 1; /* force the first apply */
    if (!yser__set_read_timeout(p, 0)) {
        char msg[256];
        yser__errmsg((DWORD)p->rd_oserr, msg, sizeof(msg));
        yser__set_error(p, "SetCommTimeouts(%s): %s", p->device, msg);
        return yser__open_fail(p);
    }

    /* Manual-reset events: an overlapped operation signals its event and only
     * the owner resets it, and the wake event must stay set until the reader
     * that it belongs to consumes it. */
    p->rd_event   = (void*)CreateEventA(NULL, TRUE, FALSE, NULL);
    p->wr_event   = (void*)CreateEventA(NULL, TRUE, FALSE, NULL);
    p->wake_event = (void*)CreateEventA(NULL, TRUE, FALSE, NULL);
    p->pa_event   = (void*)CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!p->rd_event || !p->wr_event || !p->wake_event || !p->pa_event) {
        yser__set_error(p, "CreateEvent: error %lu", (unsigned long)GetLastError());
        return yser__open_fail(p);
    }
    /* Windows has no per-handle receive-latency control: the FTDI latency timer
     * is a registry value applied at plug time. See LOW LATENCY. */
    p->low_latency = false;
    p->is_open = true;
    return true;
}

YSER_API void yser_close(yser_port* p) {
    if (!p || !p->is_open) return;
#ifndef YSER_NO_THREADS
    /* First, while the port is still open: this joins the worker and lets it
     * write a pending trailing byte at once, so a pulse in flight is never
     * left hanging, and nothing can start an I/O after the handle is gone. */
    yser__async_stop(p);
#endif
    if (p->wake_event) SetEvent((HANDLE)p->wake_event);
    /* Both roles have returned by contract, so nothing should be in flight;
     * cancel anyway and reap, so no completion can land in the handle after
     * the events are gone. */
    CancelIoEx((HANDLE)p->handle, NULL);
    DWORD done = 0;
    GetOverlappedResult((HANDLE)p->handle, YSER__OVL(p->rd_ovl), &done, TRUE);
    GetOverlappedResult((HANDLE)p->handle, YSER__OVL(p->wr_ovl), &done, TRUE);
    p->is_open = false;
    yser__open_fail(p); /* same teardown, ignores the return */
}

YSER_API int yser_interrupt(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!SetEvent((HANDLE)p->wake_event)) {
        p->misc_oserr = (int)GetLastError();
        return YSER_ERR_IO;
    }
    return 0;
}

/* --- output ------------------------------------------------------------- */

/* One bounded write on the caller's own OVERLAPPED. The user's writer thread
 * and the async worker each bring their own, because on Windows a transfer is
 * reaped by the thread that started it. */
static int yser__write_ovl(yser_port* p, const void* buf, int len, uint32_t timeout_ms,
                           OVERLAPPED* ovl, HANDLE ev) {
    memset(ovl, 0, sizeof(*ovl));
    ovl->hEvent = ev;
    ResetEvent(ev);

    DWORD done = 0;
    if (!WriteFile((HANDLE)p->handle, buf, (DWORD)len, &done, ovl)) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            p->wr_oserr = (int)e;
            return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
        }
        /* The COMMTIMEOUTS write constant already bounds the ordinary write,
         * so that case simply reaps. A caller asking for a different bound
         * (yser_close()'s flush) waits on the event itself and cancels the
         * rest, because rewriting the port-global COMMTIMEOUTS for one byte
         * would change the reader's timeouts too. */
        if (timeout_ms != p->write_timeout_ms &&
            WaitForSingleObject(ev, (DWORD)timeout_ms) != WAIT_OBJECT_0)
            CancelIoEx((HANDLE)p->handle, ovl);
        if (!GetOverlappedResult((HANDLE)p->handle, ovl, &done, TRUE)) {
            e = GetLastError();
            p->wr_oserr = (int)e;
            /* Only a purge, our own cancel or a vanished device aborts a
             * write. Every other failure is a driver error and must be said
             * so: reporting it as a short count makes it indistinguishable
             * from a write timeout, and the caller retries it forever. */
            if (e != ERROR_OPERATION_ABORTED)
                return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
            if (!yser__port_alive(p)) return YSER_ERR_DISCONNECTED;
            return (int)done;  /* a purge or our cancel: report what got through */
        }
    }
    return (int)done;
}

static int yser__write_timed(yser_port* p, const void* buf, int len, uint32_t timeout_ms) {
    return yser__write_ovl(p, buf, len, timeout_ms,
                           YSER__OVL(p->wr_ovl), (HANDLE)p->wr_event);
}

YSER_API int yser_drain(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!FlushFileBuffers((HANDLE)p->handle)) {
        DWORD e = GetLastError();
        p->wr_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

YSER_API int yser_purge(yser_port* p, int which) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!(which & (YSER_PURGE_RX | YSER_PURGE_TX))) return YSER_ERR_ARG;
    DWORD flags = 0;
    /* The ABORT bits also end an overlapped transfer in the same direction,
     * which is why RX purge belongs to the reader role. */
    if (which & YSER_PURGE_RX) flags |= PURGE_RXCLEAR | PURGE_RXABORT;
    if (which & YSER_PURGE_TX) flags |= PURGE_TXCLEAR | PURGE_TXABORT;
    if (!PurgeComm((HANDLE)p->handle, flags)) {
        DWORD e = GetLastError();
        /* The error lands in the slot of the role that owns the direction, so
         * a reader and a writer purging at once cannot overwrite each other. */
        if (which & YSER_PURGE_TX) p->wr_oserr = (int)e;
        else                       p->rd_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

/* --- input -------------------------------------------------------------- */

/* One read of whatever is buffered, waiting up to timeout_ms for the first
 * byte. The platform primitive under yser_read(); same return contract as
 * YSER_READ_ANY. */
static int yser__read_some(yser_port* p, void* buf, int cap, uint32_t timeout_ms) {
    HANDLE h = (HANDLE)p->handle;
    OVERLAPPED* ovl = YSER__OVL(p->rd_ovl);
    bool infinite = (timeout_ms == YSER_TIMEOUT_INFINITE);
    /* An abort that was neither an interrupt nor a disconnect resumes the
     * wait, so what is left of a finite timeout has to be measured against an
     * absolute deadline rather than restarted from the caller's duration. */
    uint64_t deadline = infinite ? 0 : yser_now_us() + (uint64_t)timeout_ms * 1000ull;
    uint32_t remaining = timeout_ms;

    for (;;) {
        /* A latched interrupt wins over buffered data, so a reader that is
         * being flooded can still be shut down. The POSIX side checks the
         * wake pipe before the port for the same reason. */
        if (WaitForSingleObject((HANDLE)p->wake_event, 0) == WAIT_OBJECT_0) {
            ResetEvent((HANDLE)p->wake_event);
            p->rd_oserr = 0;
            return YSER_ERR_INTERRUPTED;
        }
        /* YSER_TIMEOUT_INFINITE is MAXDWORD, which the timeout structure
         * reserves for its "return at once" case, so an infinite wait is a
         * loop over long finite ones. */
        uint32_t slice = infinite ? 0x7FFFFFFFu : remaining;
        if (!yser__set_read_timeout(p, slice))
            return yser__gone((DWORD)p->rd_oserr) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;

        memset(ovl, 0, sizeof(*ovl));
        ovl->hEvent = (HANDLE)p->rd_event;
        ResetEvent((HANDLE)p->rd_event);

        DWORD done = 0;
        if (!ReadFile(h, buf, (DWORD)cap, &done, ovl)) {
            DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                p->rd_oserr = (int)e;
                return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
            }
            HANDLE waits[2];
            waits[0] = (HANDLE)p->rd_event;
            waits[1] = (HANDLE)p->wake_event;
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                /* Interrupted, or the wait itself failed: either way the read
                 * must be canceled and reaped before the OVERLAPPED in the
                 * handle can be reused. */
                bool woke = (w == WAIT_OBJECT_0 + 1);
                p->rd_oserr = woke ? 0 : (int)GetLastError();
                CancelIoEx(h, ovl);
                if (!GetOverlappedResult(h, ovl, &done, TRUE) &&
                    GetLastError() != ERROR_OPERATION_ABORTED)
                    done = 0;  /* only a canceled IRP carries a count */
                if (!woke) return YSER_ERR_IO;
                /* Bytes that beat the cancel are not thrown away, whether the
                 * reap succeeded or reported the cancel: serial.sys fills in
                 * the transfer count either way, and this must agree with the
                 * completed-then-aborted case below, which keeps them. The
                 * wake stays latched and the next read reports it. */
                if (done > 0) return (int)done;
                ResetEvent((HANDLE)p->wake_event);
                return YSER_ERR_INTERRUPTED;
            }
            if (!GetOverlappedResult(h, ovl, &done, TRUE)) {
                e = GetLastError();
                p->rd_oserr = (int)e;
                if (yser__gone(e)) return YSER_ERR_DISCONNECTED;
                if (e == ERROR_OPERATION_ABORTED) {
                    /* serial.sys fills in the transfer count even on a
                     * canceled IRP, so bytes that made it into the buffer are
                     * returned rather than dropped. */
                    if (done > 0) return (int)done;
                    /* A purge or a cancel aborts the read, and some drivers
                     * report a vanished device the same way. Ask the port. */
                    if (!yser__port_alive(p)) return YSER_ERR_DISCONNECTED;
                    /* Alive, and an interrupt would have been seen at the top
                     * of this loop, so the abort was a yser_purge(YSER_PURGE_RX)
                     * on this same thread. The wait it cut short is not over:
                     * returning 0 would end a YSER_TIMEOUT_INFINITE read with
                     * no data, no interrupt and no disconnect, and would look
                     * like a deadline to the YSER_READ_ALL loop. Resume it. */
                    if (!infinite) {
                        remaining = yser__remaining_ms(deadline);
                        if (remaining == 0) return 0;
                    }
                    continue;
                }
                return YSER_ERR_IO;
            }
        }
        if (done > 0) return (int)done;
        if (!infinite) return 0;  /* the total read timeout expired */
    }
}

YSER_API int yser_available(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    DWORD errors = 0;
    COMSTAT st;
    memset(&st, 0, sizeof(st));
    if (!ClearCommError((HANDLE)p->handle, &errors, &st)) {
        DWORD e = GetLastError();
        p->rd_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return (int)st.cbInQue;
}

/* --- modem lines -------------------------------------------------------- */

static int yser__escape(yser_port* p, DWORD fn) {
    if (!EscapeCommFunction((HANDLE)p->handle, fn)) {
        DWORD e = GetLastError();
        p->misc_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

YSER_API int yser_set_dtr(yser_port* p, bool on) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    return yser__escape(p, on ? SETDTR : CLRDTR);
}

YSER_API int yser_set_rts(yser_port* p, bool on) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (p->flow == YSER_FLOW_RTSCTS) return 0; /* the driver owns RTS */
    return yser__escape(p, on ? SETRTS : CLRRTS);
}

YSER_API int yser_get_lines(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    DWORD st = 0;
    if (!GetCommModemStatus((HANDLE)p->handle, &st)) {
        DWORD e = GetLastError();
        /* misc_oserr, not rd_oserr: this call has no direction and may run on
         * the writer thread while the reader is failing. */
        p->misc_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    int mask = 0;
    if (st & MS_CTS_ON)  mask |= YSER_LINE_CTS;
    if (st & MS_DSR_ON)  mask |= YSER_LINE_DSR;
    if (st & MS_RING_ON) mask |= YSER_LINE_RI;
    if (st & MS_RLSD_ON) mask |= YSER_LINE_DCD;
    return mask;
}

YSER_API int yser_send_break(yser_port* p, uint32_t ms) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!SetCommBreak((HANDLE)p->handle)) {
        DWORD e = GetLastError();
        p->wr_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    /* yrt_sleep_ns() hands the bulk of the wait to the OS and spins only its
     * tail, so a break no longer has the 15.6 ms Windows tick as its floor.
     * The multiply is 64-bit: ms * 1000000 overflows 32 bits at about 4.3 s,
     * and this call takes the caller's number as given. */
    (void)yrt_sleep_ns((int64_t)ms * 1000000);
    if (!ClearCommBreak((HANDLE)p->handle)) {
        DWORD e = GetLastError();
        p->wr_oserr = (int)e;
        return yser__gone(e) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

#endif /* YSER__WINDOWS */

/* ======================================================================= *
 *  POSIX  (Linux and macOS, termios)
 * ======================================================================= */
#if defined(YSER__POSIX)

/* errno values that mean the device is gone rather than busy. */
static bool yser__gone(int e) {
    return e == EIO || e == ENXIO || e == ENODEV || e == EPIPE;
}

/* What yser__wait() saw. */
#define YSER__W_IO   0x01   /* the port is ready in the direction asked for   */
#define YSER__W_WAKE 0x02   /* yser_interrupt() put a byte in the self-pipe   */
#define YSER__W_GONE 0x04   /* hangup or error on the port descriptor         */

/* Wait until the port descriptor is ready, the interrupt pipe has a byte, or
 * timeout_ms (-1 = forever) runs out. Returns a YSER__W_* mask, 0 on timeout,
 * or -1 with errno set. wake_fd < 0 means the caller has no interrupt to watch
 * for, which is the writer.
 *
 * Linux polls; macOS selects. Apple's poll(2) still documents that it does not
 * support devices, and every descriptor here is one, so the Darwin build uses
 * the call pyserial uses on POSIX for the same reason. The price is that
 * select() cannot express a hangup: on Darwin a dead port instead surfaces as
 * a readable descriptor whose read() returns 0, or as EIO/ENXIO out of
 * read()/write(), both of which the callers already map. yser_open() refuses
 * descriptors at or above FD_SETSIZE, so FD_SET here is always in range. */
static int yser__wait(int fd, int wake_fd, bool for_write, int timeout_ms) {
#if defined(__APPLE__)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    int nfds = fd;
    if (wake_fd >= 0) {
        FD_SET(wake_fd, &set);
        if (wake_fd > nfds) nfds = wake_fd;
    }
    struct timeval tv;
    struct timeval* ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int r = select(nfds + 1, for_write ? NULL : &set, for_write ? &set : NULL,
                   NULL, ptv);
    if (r < 0) return -1;
    if (r == 0) return 0;
    int ev = 0;
    if (FD_ISSET(fd, &set)) ev |= YSER__W_IO;
    if (wake_fd >= 0 && FD_ISSET(wake_fd, &set)) ev |= YSER__W_WAKE;
    return ev;
#else
    struct pollfd fds[2];
    nfds_t n = 1;
    fds[0].fd      = fd;
    fds[0].events  = (short)(for_write ? POLLOUT : POLLIN);
    fds[0].revents = 0;
    if (wake_fd >= 0) {
        fds[1].fd      = wake_fd;
        fds[1].events  = POLLIN;
        fds[1].revents = 0;
        n = 2;
    }
    int r = poll(fds, n, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    int ev = 0;
    if (fds[0].revents & (for_write ? POLLOUT : POLLIN)) ev |= YSER__W_IO;
    if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) ev |= YSER__W_GONE;
    if (n == 2 && (fds[1].revents & POLLIN)) ev |= YSER__W_WAKE;
    return ev;
#endif
}

/* --- discovery ---------------------------------------------------------- */

#if defined(__linux__)
/* One sysfs attribute as a trimmed string. stdio is fine here: enumeration is
 * a setup-time operation, never on the read or write path. */
static bool yser__read_attr(const char* dir, const char* name, char* out, size_t cap) {
    char path[PATH_MAX];
    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return n > 0;
}

/* Last path component, for sysfs node names used as descriptions. */
static const char* yser__basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* "1-1.4:1.0": a USB interface directory, the finest-grained stable position
 * of a port on the bus. Distinguishes the channels of an FT4232 and two
 * identical boxes without serial numbers. */
static bool yser__is_usb_interface(const char* name) {
    return strchr(name, ':') != NULL && strchr(name, '-') != NULL;
}

YSER_API int yser_list_ports(yser_port_info* out, int max) {
    DIR* dir = opendir("/sys/class/tty");
    if (!dir) return YSER_ERR_IO;
    yser__list list;
    memset(&list, 0, sizeof(list));
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char link[PATH_MAX], devpath[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/tty/%s/device", ent->d_name);
        /* No device link: a virtual console or the ptmx/console aliases. */
        if (!realpath(link, devpath)) continue;
        /* The 8250 driver registers every port the chipset could have, present
         * or not, and the absent ones read type 0 (PORT_UNKNOWN). Listing them
         * would offer the user a dozen ports that cannot be opened. */
        char attr[64];
        snprintf(link, sizeof(link), "/sys/class/tty/%s", ent->d_name);
        if (yser__read_attr(link, "type", attr, sizeof(attr)) && strcmp(attr, "0") == 0)
            continue;

        yser_port_info* e = yser__list_add(&list);
        if (!e) break;
        yser__copy(e->name, sizeof(e->name), "/dev/");
        yser__append(e->name, sizeof(e->name), ent->d_name);
        char node[PATH_MAX];
        yser__copy(node, sizeof(node), devpath);
        char manufacturer[128] = "", product[128] = "";
        /* Walk toward the root: the tty hangs off a USB interface, whose
         * parent is the USB device with the identifying attributes. */
        for (int hop = 0; hop < 8; hop++) {
            if (!e->location[0] && yser__is_usb_interface(yser__basename(node)))
                yser__copy(e->location, sizeof(e->location), yser__basename(node));
            char vid[16], pid[16];
            if (yser__read_attr(node, "idVendor", vid, sizeof(vid)) &&
                yser__read_attr(node, "idProduct", pid, sizeof(pid))) {
                e->vid = (uint16_t)strtoul(vid, NULL, 16);
                e->pid = (uint16_t)strtoul(pid, NULL, 16);
                yser__read_attr(node, "serial", e->serial_number, sizeof(e->serial_number));
                yser__read_attr(node, "manufacturer", manufacturer, sizeof(manufacturer));
                yser__read_attr(node, "product", product, sizeof(product));
                break;
            }
            char* slash = strrchr(node, '/');
            if (!slash || slash == node) break;
            *slash = '\0';
        }
        if (product[0]) {
            yser__copy(e->description, sizeof(e->description), manufacturer);
            if (manufacturer[0]) yser__append(e->description, sizeof(e->description), " ");
            yser__append(e->description, sizeof(e->description), product);
        } else {
            /* Not USB: the driver name is all sysfs offers. */
            char drv[PATH_MAX];
            yser__copy(link, sizeof(link), devpath);
            yser__append(link, sizeof(link), "/driver");
            if (realpath(link, drv))
                yser__copy(e->description, sizeof(e->description), yser__basename(drv));
        }
    }
    closedir(dir);
    return yser__list_finish(&list, out, max);
}
#else /* __APPLE__ */
YSER_API int yser_list_ports(yser_port_info* out, int max) {
    /* Names only. The USB fields would need IOKit, which means linking
     * -framework IOKit -framework CoreFoundation into every translation unit
     * that defines the implementation; that is a dependency this collection
     * does not take for a setup-time convenience. Match on the name, which
     * carries the serial number for FTDI parts
     * ("/dev/cu.usbserial-A5XK3RJT"), or take the port name from the user. */
    DIR* dir = opendir("/dev");
    if (!dir) return YSER_ERR_IO;
    yser__list list;
    memset(&list, 0, sizeof(list));
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        /* cu.*, not tty.*: the call-out node does not block waiting for
         * carrier detect. */
        if (strncmp(ent->d_name, "cu.", 3) != 0) continue;
        yser_port_info* e = yser__list_add(&list);
        if (!e) break;
        yser__copy(e->name, sizeof(e->name), "/dev/");
        yser__append(e->name, sizeof(e->name), ent->d_name);
    }
    closedir(dir);
    return yser__list_finish(&list, out, max);
}
#endif

/* --- lifecycle ---------------------------------------------------------- */

static bool yser__open_fail(yser_port* p) {
    if (p->fd >= 0) close(p->fd);
    if (p->wake_fd[0] >= 0) close(p->wake_fd[0]);
    if (p->wake_fd[1] >= 0) close(p->wake_fd[1]);
    p->fd = -1;
    p->wake_fd[0] = p->wake_fd[1] = -1;
    return false;
}

/* Standard rates only. A rate with no B* constant fails on Linux (see the
 * Deferred section of docs/serial.md) and goes through IOSSIOSPEED on
 * macOS. */
static bool yser__baud_constant(uint32_t rate, speed_t* out) {
    static const struct { uint32_t rate; speed_t code; } table[] = {
        {     50, B50     }, {     75, B75     }, {    110, B110    },
        {    134, B134    }, {    150, B150    }, {    200, B200    },
        {    300, B300    }, {    600, B600    }, {   1200, B1200   },
        {   1800, B1800   }, {   2400, B2400   }, {   4800, B4800   },
        {   9600, B9600   }, {  19200, B19200  }, {  38400, B38400  },
#ifdef B57600
        {  57600, B57600  },
#endif
#ifdef B115200
        { 115200, B115200 },
#endif
#ifdef B230400
        { 230400, B230400 },
#endif
#ifdef B460800
        { 460800, B460800 },
#endif
#ifdef B500000
        { 500000, B500000 },
#endif
#ifdef B576000
        { 576000, B576000 },
#endif
#ifdef B921600
        { 921600, B921600 },
#endif
#ifdef B1000000
        {1000000, B1000000},
#endif
#ifdef B1152000
        {1152000, B1152000},
#endif
#ifdef B1500000
        {1500000, B1500000},
#endif
#ifdef B2000000
        {2000000, B2000000},
#endif
#ifdef B2500000
        {2500000, B2500000},
#endif
#ifdef B3000000
        {3000000, B3000000},
#endif
#ifdef B3500000
        {3500000, B3500000},
#endif
#ifdef B4000000
        {4000000, B4000000},
#endif
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].rate == rate) { *out = table[i].code; return true; }
    }
    return false;
}

/* select() indexes a fixed-size bitmap, so on Darwin a descriptor at or above
 * FD_SETSIZE would write past the fd_set. Refusing the port at open time is
 * the only honest answer. Linux polls, where the descriptor number is not
 * bounded, so there this costs nothing and checks nothing. See POSIX WAITS. */
static bool yser__fd_ok(yser_port* p, int fd, const char* what) {
#if defined(__APPLE__)
    if (fd >= FD_SETSIZE) {
        yser__set_error(p, "%s got descriptor %d, at or above FD_SETSIZE (%d); "
                           "macOS waits with select()", what, fd, (int)FD_SETSIZE);
        return false;
    }
#else
    (void)p; (void)fd; (void)what;
#endif
    return true;
}

static bool yser__open_platform(yser_port* p, const yser_desc* d) {
    /* O_NONBLOCK stays set for the life of the handle: every wait in this
     * library is against a deadline (poll on Linux, select on macOS), never a
     * blocking descriptor. O_NOCTTY keeps a serial console from becoming this
     * process's terminal. */
    p->fd = open(p->device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (p->fd < 0) {
        yser__set_error(p, "open(%s): %s", p->device, strerror(errno));
        return false;
    }
    if (!yser__fd_ok(p, p->fd, p->device)) return yser__open_fail(p);
    if (d->exclusive && ioctl(p->fd, TIOCEXCL) != 0) {
        yser__set_error(p, "TIOCEXCL(%s): %s", p->device, strerror(errno));
        return yser__open_fail(p);
    }

    /* Self-pipe for yser_interrupt(): the reader polls [0], any thread writes
     * [1]. pipe() plus fcntl() rather than pipe2(), whose declaration glibc
     * hides behind _GNU_SOURCE. Non-blocking on both ends, so neither an
     * interrupt nor its drain can ever stall. */
    if (pipe(p->wake_fd) != 0) {
        yser__set_error(p, "pipe: %s", strerror(errno));
        return yser__open_fail(p);
    }
    for (int i = 0; i < 2; i++) {
        if (!yser__fd_ok(p, p->wake_fd[i], "interrupt pipe"))
            return yser__open_fail(p);
        int fl = fcntl(p->wake_fd[i], F_GETFL, 0);
        fcntl(p->wake_fd[i], F_SETFD, FD_CLOEXEC);
        fcntl(p->wake_fd[i], F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    }

    struct termios t;
    if (tcgetattr(p->fd, &t) != 0) {
        yser__set_error(p, "tcgetattr(%s): %s (not a serial port?)", p->device, strerror(errno));
        return yser__open_fail(p);
    }
    /* Raw: no canonical mode, no echo, no signals, no CR/LF rewriting, no
     * output post-processing. A trigger byte is not text. */
    t.c_iflag &= (tcflag_t)~(BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                             ICRNL | IXON | IXOFF | IXANY | INPCK | IGNPAR);
    /* IGNBRK, not cleared: with BRKINT off and IGNBRK off a line break is
     * delivered as a single 0x00 byte, which on a response box reads as a
     * phantom key and on a latching trigger box as a code. Drop breaks. */
    t.c_iflag |= (tcflag_t)IGNBRK;
    t.c_oflag &= (tcflag_t)~OPOST;
    /* FLUSHO is not a mode the caller sets, but a stale one (a previous user
     * of the port hit Ctrl-O, or the driver left it set) silently discards
     * everything written, which looks exactly like a dead device. */
    t.c_lflag &= (tcflag_t)~(ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN
#if defined(FLUSHO)
                             | FLUSHO
#endif
                            );
    t.c_cflag &= (tcflag_t)~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS | HUPCL);
#if defined(CMSPAR)
    t.c_cflag &= (tcflag_t)~CMSPAR;
#endif
    /* CLOCAL: ignore modem control lines, so an open or a read never waits for
     * carrier detect. CREAD: actually receive. */
    t.c_cflag |= (tcflag_t)(CLOCAL | CREAD);
    switch (p->data_bits) {
        case 5:  t.c_cflag |= CS5; break;
        case 6:  t.c_cflag |= CS6; break;
        case 7:  t.c_cflag |= CS7; break;
        default: t.c_cflag |= CS8; break;
    }
    switch (p->parity) {
        case YSER_PARITY_ODD:  t.c_cflag |= (tcflag_t)(PARENB | PARODD); break;
        case YSER_PARITY_EVEN: t.c_cflag |= (tcflag_t)PARENB; break;
#if defined(CMSPAR)
        case YSER_PARITY_MARK:  t.c_cflag |= (tcflag_t)(PARENB | CMSPAR | PARODD); break;
        case YSER_PARITY_SPACE: t.c_cflag |= (tcflag_t)(PARENB | CMSPAR); break;
#endif
        default: break;
    }
    if (p->stop_bits == YSER_STOP_BITS_2) t.c_cflag |= (tcflag_t)CSTOPB;
    if (p->flow == YSER_FLOW_RTSCTS)      t.c_cflag |= (tcflag_t)CRTSCTS;
    if (p->flow == YSER_FLOW_XONXOFF)     t.c_iflag |= (tcflag_t)(IXON | IXOFF);
    /* HUPCL drops DTR when the last handle closes, which resets Arduino-class
     * boards. Clearing it is the reliable way to keep a box alive between
     * sessions; see DTR AND DEVICE RESET. */
    if (!p->keep_dtr_on_close) t.c_cflag |= (tcflag_t)HUPCL;
    /* Return from read() at once with whatever is there: the timeout lives in
     * the wait (see POSIX WAITS), not in the driver. */
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;

    speed_t code = 0;
    bool standard = yser__baud_constant(p->baud, &code);
#if defined(__APPLE__)
    /* Darwin termios stops at 230400; IOSSIOSPEED sets the real rate after
     * tcsetattr, so the termios speed only has to be something legal. */
    if (!standard) code = B9600;
#else
    if (!standard) {
        yser__set_error(p, "unsupported baud rate %u: no termios constant "
                           "(see docs/serial.md, Deferred)", p->baud);
        return yser__open_fail(p);
    }
#endif
    if (cfsetispeed(&t, code) != 0 || cfsetospeed(&t, code) != 0) {
        yser__set_error(p, "cfsetspeed(%u): %s", p->baud, strerror(errno));
        return yser__open_fail(p);
    }
    if (tcsetattr(p->fd, TCSANOW, &t) != 0) {
        yser__set_error(p, "tcsetattr(%s): %s", p->device, strerror(errno));
        return yser__open_fail(p);
    }
#if defined(__APPLE__)
    if (!standard) {
        speed_t want = (speed_t)p->baud;
        if (ioctl(p->fd, IOSSIOSPEED, &want) != 0) {
            yser__set_error(p, "IOSSIOSPEED(%u): %s", p->baud, strerror(errno));
            return yser__open_fail(p);
        }
    }
#endif

    /* DTR and RTS are asserted unless the caller asked otherwise (the pyserial
     * default). dtr_low_on_open cannot prevent an Arduino-class reset on
     * Linux USB-serial: the kernel raises both lines inside open() itself,
     * O_NONBLOCK included, so by the time this runs the edge is already on the
     * wire and all it does is drop the line again. See DTR AND DEVICE RESET
     * for what does work. RTS is left alone under a hardware handshake, where
     * it belongs to the driver. */
    int dtr = TIOCM_DTR, rts = TIOCM_RTS;
    ioctl(p->fd, d->dtr_low_on_open ? TIOCMBIC : TIOCMBIS, &dtr);
    if (p->flow != YSER_FLOW_RTSCTS)
        ioctl(p->fd, d->rts_low_on_open ? TIOCMBIC : TIOCMBIS, &rts);

#if defined(__linux__)
    if (d->low_latency) {
        /* The ioctl pair is the request. It is not the answer: cdc_acm
         * implements TIOCSSERIAL, accepts ASYNC_LOW_LATENCY and does nothing
         * with it, so a ttyACM device would report low_latency = true while
         * still buffering. What ftdi_sio actually programs is the
         * latency_timer attribute, in milliseconds, so read that back and
         * only claim success when the timer really is 1 ms. A device with no
         * such attribute leaves low_latency false. */
        struct serial_struct ss;
        if (ioctl(p->fd, TIOCGSERIAL, &ss) == 0) {
            ss.flags |= ASYNC_LOW_LATENCY;
            ioctl(p->fd, TIOCSSERIAL, &ss);
        }
        /* The sysfs directory is named after the real tty, so the device name
         * has to be resolved first. DEVICE NAMES tells experiments to open the
         * stable /dev/serial/by-id/... symlinks, and their basename matches
         * nothing under /sys/bus/usb-serial/devices: without this the attribute
         * never opens and low_latency reads false on exactly the devices whose
         * timer ftdi_sio just programmed. A fixed buffer, because yser_open()
         * allocates nothing the caller did not ask for. */
        char real[PATH_MAX], dir[PATH_MAX], val[32];
        const char* node = realpath(p->device, real) ? real : p->device;
        yser__copy(dir, sizeof(dir), "/sys/bus/usb-serial/devices/");
        yser__append(dir, sizeof(dir), yser__basename(node));
        /* No such attribute (cdc_acm and every non-USB port): low_latency
         * stays false, which is the honest answer. */
        if (yser__read_attr(dir, "latency_timer", val, sizeof(val)))
            p->low_latency = (strtol(val, NULL, 10) == 1);
    }
#else
    (void)d->low_latency; /* no runtime control on macOS; see LOW LATENCY */
#endif

    p->is_open = true;
    return true;
}

YSER_API void yser_close(yser_port* p) {
    if (!p || !p->is_open) return;
#ifndef YSER_NO_THREADS
    /* First, while the fd is still valid: this joins the worker and lets it
     * write a pending trailing byte at once, so a pulse in flight is never
     * left hanging. */
    yser__async_stop(p);
#endif
    /* Wake a reader that is about to notice the descriptor going away. The
     * caller is supposed to have joined it already; this only narrows the
     * window if it did not. */
    if (p->wake_fd[1] >= 0) {
        char x = 'x';
        ssize_t ignored = write(p->wake_fd[1], &x, 1);
        (void)ignored;
    }
    p->is_open = false;
    yser__open_fail(p); /* same teardown, ignores the return */
}

YSER_API int yser_interrupt(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    char x = 'x';
    ssize_t n = write(p->wake_fd[1], &x, 1);
    /* A full pipe means an earlier wake is still pending, which is the same
     * outcome. */
    if (n < 0 && errno != EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        && errno != EWOULDBLOCK
#endif
    ) {
        p->misc_oserr = errno;
        return YSER_ERR_IO;
    }
    return 0;
}

/* --- output ------------------------------------------------------------- */

static int yser__write_timed(yser_port* p, const void* buf, int len, uint32_t timeout_ms) {
    const uint8_t* src = (const uint8_t*)buf;
    int got = 0;
    bool infinite = (timeout_ms == YSER_TIMEOUT_INFINITE);
    uint64_t deadline = infinite ? 0
                                 : yser_now_us() + (uint64_t)timeout_ms * 1000ull;
    while (got < len) {
        ssize_t n = write(p->fd, src + got, (size_t)(len - got));
        if (n > 0) { got += (int)n; continue; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
                && errno != EWOULDBLOCK
#endif
            ) {
                p->wr_oserr = errno;
                return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
            }
        }
        /* The transmit buffer is full (or the peer is not draining it under
         * flow control): wait for room, bounded by the caller's timeout. */
        int tmo = infinite ? -1 : (int)yser__remaining_ms(deadline);
        if (tmo == 0) break;
        int ev = yser__wait(p->fd, -1, true, tmo);
        if (ev < 0) {
            if (errno == EINTR) continue;
            p->wr_oserr = errno;
            return YSER_ERR_IO;
        }
        if (ev == 0) break;  /* timeout: report the count accepted so far */
        /* A hangup outranks writability: the room the wait reported leads
         * nowhere on a port that is gone. */
        if (ev & YSER__W_GONE) {
            p->wr_oserr = 0;
            return YSER_ERR_DISCONNECTED;
        }
    }
    return got;
}

YSER_API int yser_drain(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    while (tcdrain(p->fd) != 0) {
        if (errno == EINTR) continue;
        p->wr_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

YSER_API int yser_purge(yser_port* p, int which) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!(which & (YSER_PURGE_RX | YSER_PURGE_TX))) return YSER_ERR_ARG;
    int sel = ((which & YSER_PURGE_RX) && (which & YSER_PURGE_TX)) ? TCIOFLUSH
              : (which & YSER_PURGE_RX) ? TCIFLUSH : TCOFLUSH;
    if (tcflush(p->fd, sel) != 0) {
        /* The error lands in the slot of the role that owns the direction, so
         * a reader and a writer purging at once cannot overwrite each other. */
        if (which & YSER_PURGE_TX) p->wr_oserr = errno;
        else                       p->rd_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

/* --- input -------------------------------------------------------------- */

/* One read of whatever is buffered, waiting up to timeout_ms for the first
 * byte. The platform primitive under yser_read(); same return contract as
 * YSER_READ_ANY. */
static int yser__read_some(yser_port* p, void* buf, int cap, uint32_t timeout_ms) {
    bool infinite = (timeout_ms == YSER_TIMEOUT_INFINITE);
    uint64_t deadline = infinite ? 0 : yser_now_us() + (uint64_t)timeout_ms * 1000ull;

    for (;;) {
        int tmo = infinite ? -1 : (int)yser__remaining_ms(deadline);
        int ev = yser__wait(p->fd, p->wake_fd[0], false, tmo);
        if (ev < 0) {
            if (errno == EINTR) continue;
            p->rd_oserr = errno;
            return YSER_ERR_IO;
        }
        if (ev == 0) return 0;
        if (ev & YSER__W_WAKE) {
            /* Drain the whole pipe: several interrupts collapse into one. */
            char sink[16];
            while (read(p->wake_fd[0], sink, sizeof(sink)) > 0) { }
            p->rd_oserr = 0;
            return YSER_ERR_INTERRUPTED;
        }
        if (ev & YSER__W_IO) {
            ssize_t n = read(p->fd, buf, (size_t)cap);
            if (n > 0) return (int)n;
            /* n == 0 is read as a hangup only because the wait just reported
             * the port readable. Ungated it would be ambiguous: with VMIN = 0
             * and VTIME = 0, which is how this library configures the line,
             * Linux n_tty also returns 0 for "nothing buffered". After a
             * readable report there is nothing to be had but end of file, and
             * that is how macOS surfaces an unplug. */
            if (n == 0) { p->rd_oserr = 0; return YSER_ERR_DISCONNECTED; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            if (errno == EWOULDBLOCK) continue;
#endif
            p->rd_oserr = errno;
            return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
        }
        /* Checked after readability so buffered bytes are delivered before the
         * hangup that follows them. */
        if (ev & YSER__W_GONE) {
            p->rd_oserr = 0;
            return YSER_ERR_DISCONNECTED;
        }
    }
}

YSER_API int yser_available(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    int n = 0;
    if (ioctl(p->fd, FIONREAD, &n) != 0) {
        p->rd_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return n;
}

/* --- modem lines -------------------------------------------------------- */

static int yser__modem_bit(yser_port* p, int bit, bool on) {
    int arg = bit;
    if (ioctl(p->fd, on ? TIOCMBIS : TIOCMBIC, &arg) != 0) {
        p->misc_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

YSER_API int yser_set_dtr(yser_port* p, bool on) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    return yser__modem_bit(p, TIOCM_DTR, on);
}

YSER_API int yser_set_rts(yser_port* p, bool on) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (p->flow == YSER_FLOW_RTSCTS) return 0; /* the driver owns RTS */
    return yser__modem_bit(p, TIOCM_RTS, on);
}

YSER_API int yser_get_lines(yser_port* p) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    int st = 0;
    if (ioctl(p->fd, TIOCMGET, &st) != 0) {
        /* misc_oserr, not rd_oserr: this call has no direction and may run on
         * the writer thread while the reader is failing. */
        p->misc_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    int mask = 0;
    if (st & TIOCM_CTS) mask |= YSER_LINE_CTS;
    if (st & TIOCM_DSR) mask |= YSER_LINE_DSR;
    if (st & TIOCM_RI)  mask |= YSER_LINE_RI;
    if (st & TIOCM_CD)  mask |= YSER_LINE_DCD;
    return mask;
}

YSER_API int yser_send_break(yser_port* p, uint32_t ms) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    /* TIOCSBRK rather than tcsendbreak(), whose duration is 0.25 to 0.5 s and
     * not under the caller's control. */
    if (ioctl(p->fd, TIOCSBRK) != 0) {
        p->wr_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    /* 64-bit multiply: ms * 1000000 overflows 32 bits at about 4.3 s. */
    (void)yrt_sleep_ns((int64_t)ms * 1000000);
    if (ioctl(p->fd, TIOCCBRK) != 0) {
        p->wr_oserr = errno;
        return yser__gone(errno) ? YSER_ERR_DISCONNECTED : YSER_ERR_IO;
    }
    return 0;
}

#endif /* YSER__POSIX */

/* ======================================================================= *
 *  ASYNC PULSE WORKER
 *
 *  ysp/rt.h owns the thread. One yrt_worker per handle, started by
 *  yser_open(), runs one callback at one absolute deadline and elevates
 *  itself up ysp/rt.h's scheduling ladder before yser_open() returns. What
 *  stays here is the part that is about the byte stream rather than about
 *  time.
 *
 *  yrt_worker runs its callback with its OWN lock released, which is what
 *  lets that callback block in a driver write without deadlocking a submit
 *  from another thread. The price is that ysp/rt.h serializes nothing for us,
 *  and this library's threading promise is that exactly one thread writes the
 *  port at a time. Hence the writer mutex below: every writer-role write, the
 *  worker's trailing byte included, holds it.
 *
 *  Next to the mutex sits the pending trailing byte, armed together with the
 *  deadline it is due at. That pair is the token the callback checks: it
 *  writes only what is still armed and due, so a byte the worker had already
 *  dispatched when a user write, a replacement pulse or a close got the mutex
 *  first is dropped instead of undoing that write. yrt_worker_cancel()
 *  handles the ordinary case, where the job has not been dispatched yet; the
 *  token covers the window between ysp/rt.h releasing its lock and the
 *  callback taking ours, which no cancel can reach.
 *
 *  Lock order is writer mutex, then ysp/rt.h's lock (yser__async_submit and
 *  yser__write_cancel take both). The callback runs the other way round, but
 *  it holds nothing of ysp/rt.h's when it takes ours, so the cycle does not
 *  close.
 * ======================================================================= */
#ifndef YSER_NO_THREADS

#if defined(YSER__POSIX)
#include <pthread.h>
typedef pthread_mutex_t yser__mutex;
#else
typedef CRITICAL_SECTION yser__mutex;
#endif

typedef struct yser__async {
    yrt_worker rt;         /* ysp/rt.h's thread; the handle is ours to hold */
    yser__mutex  mtx;        /* one writer at a time on the byte stream       */
    uint64_t     off_at_ns;  /* when the armed trailing byte is due           */
    uint8_t      off_byte;
    int          armed;      /* 0 = nothing pending; written under mtx only   */
    yser_port*   port;
} yser__async;

#if defined(YSER__POSIX)
/* Priority inheritance: a normal-priority submitter holds this mutex across
 * the onset write, and without PI the real-time worker would wait behind
 * whatever preempted that submitter. */
static bool yser__mutex_init(yser_port* p, yser__mutex* m) {
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    int rc = pthread_mutex_init(m, &ma);
    pthread_mutexattr_destroy(&ma);
    if (rc != 0) yser__set_error(p, "async pulse: mutex init: %s", strerror(rc));
    return rc == 0;
}
static void yser__mutex_destroy(yser__mutex* m) { (void)pthread_mutex_destroy(m); }
static void yser__lock(yser__async* a)   { (void)pthread_mutex_lock(&a->mtx); }
static void yser__unlock(yser__async* a) { (void)pthread_mutex_unlock(&a->mtx); }

/* The trailing-edge write. On POSIX a write touches nothing but the
 * descriptor, so this is the same code the user's writer thread runs; the
 * mutex is what keeps the two apart. Failures land in wr_oserr, which is safe
 * for the same reason. */
static int yser__write_worker(yser_port* p, uint8_t value, uint32_t timeout_ms) {
    return yser__write_timed(p, &value, 1, timeout_ms);
}
#else
static bool yser__mutex_init(yser_port* p, yser__mutex* m) {
    (void)p;
    /* Since Vista this cannot fail, and there is no priority-inheritance
     * option to ask for: Windows raises the owner of a critical section for
     * a waiter on its own. */
    InitializeCriticalSection(m);
    return true;
}
static void yser__mutex_destroy(yser__mutex* m) { DeleteCriticalSection(m); }
static void yser__lock(yser__async* a)   { EnterCriticalSection(&a->mtx); }
static void yser__unlock(yser__async* a) { LeaveCriticalSection(&a->mtx); }

/* The trailing-edge write, on the worker's own OVERLAPPED and event: a
 * transfer is reaped by the thread that started it, and the callback below
 * runs on ysp/rt.h's worker thread, so it must never borrow the user writer's
 * wr_ovl/wr_event. Failures land in wr_oserr, which is safe because this runs
 * under the same mutex as yser_write. */
static int yser__write_worker(yser_port* p, uint8_t value, uint32_t timeout_ms) {
    return yser__write_ovl(p, &value, 1, timeout_ms,
                           YSER__OVL(p->pa_ovl), (HANDLE)p->pa_event);
}
#endif

/* The trailing edge, on ysp/rt.h's worker thread at ysp/rt.h's policy.
 *
 * It asks what is armed NOW instead of carrying a decision from submit time,
 * because between the worker choosing this job and this callback taking the
 * mutex another thread may have disarmed it, or armed a different byte at a
 * later deadline. Writing "whatever is armed and due" settles both: a
 * canceled byte is not written at all, and a replacement is left to its own
 * deadline unless it is already due, in which case writing it here is what
 * its own callback would do a moment later anyway.
 *
 * A flush (yrt_worker_close, which is yser_close) writes whatever is armed
 * at once, bounded by YSER__CLOSE_FLUSH_MS rather than write_timeout_ms,
 * because yser_close() is joining this thread behind the write. */
static void yser__off_job(void* ctx, const yrt_job_info* info) {
    yser__async* a = (yser__async*)ctx;
    yser__lock(a);
    if (a->armed && (info->flushed || (uint64_t)yrt_now_ns() >= a->off_at_ns)) {
        a->armed = 0;
        (void)yser__write_worker(a->port, a->off_byte,
                                 info->flushed ? YSER__CLOSE_FLUSH_MS
                                               : a->port->write_timeout_ms);
    }
    yser__unlock(a);
}

static bool yser__async_start(yser_port* p) {
    if (p->async) return true;
    /* One allocation per open, and none on any write or pulse path. The
     * yrt_worker handle is caller-allocated and lives here rather than in
     * yser_port, so the public handle the bindings mirror does not grow half a
     * kilobyte of opaque OS storage, and so its size does not depend on
     * whether the build has threads. */
    yser__async* a = (yser__async*)calloc(1, sizeof(*a));
    if (!a) { yser__set_error(p, "async pulse: out of memory"); return false; }
    a->port = p;
    if (!yser__mutex_init(p, &a->mtx)) { free(a); return false; }

    yrt_worker_desc wd;
    memset(&wd, 0, sizeof(wd));
    wd.sched = p->sched;   /* already normalized by yser_open() */
    if (!yrt_worker_open(&a->rt, &wd)) {
        yser__set_error(p, "async pulse: %s", yrt_worker_error(&a->rt));
        yser__mutex_destroy(&a->mtx);
        free(a);
        return false;
    }
    /* yrt_worker_open() returns only once the thread has published its
     * rung, so async_policy is final when yser_open() returns. */
    p->async_policy = yrt_worker_policy(&a->rt);
    p->async = a;
    return true;
}

static void yser__async_stop(yser_port* p) {
    yser__async* a = (yser__async*)p->async;
    if (!a) return;
    /* Clearing p->async before the stop is bookkeeping, not synchronization:
     * a yser_write() racing this teardown may already have read p->async and
     * be inside yser__write_cancel, so the order protects nothing. What makes
     * the teardown safe is the role contract yser_close() states, that both
     * the reader and the writer have returned before it is called. */
    p->async = NULL;
    /* Runs a pending job immediately through yser__off_job with info->flushed
     * set, then joins. Called without the mutex, because that callback takes
     * it. */
    yrt_worker_close(&a->rt);
    yser__mutex_destroy(&a->mtx);
    free(a);
    p->async_policy = YSER_ASYNC_NONE;
}

static int yser__write_cancel(yser_port* p, const void* buf, int len) {
    yser__async* a = (yser__async*)p->async;
    yser__lock(a);
    /* A plain write means "hold this"; a pending trailing byte would silently
     * undo it. Disarming is what stops it. The cancel only saves the worker a
     * pointless wake, because a job it has already dispatched cannot be
     * canceled and the callback waits behind this mutex either way. */
    a->armed = 0;
    (void)yrt_worker_cancel(&a->rt);
    int r = yser__write_nolock(p, buf, len);
    yser__unlock(a);
    return r;
}

static int yser__async_submit(yser_port* p, uint8_t on, uint8_t off, uint32_t usec) {
    yser__async* a = (yser__async*)p->async;
    yser__lock(a);
    /* Onset under the mutex, so the trailing byte of an in-flight pulse cannot
     * land between this write and the new deadline. */
    int r = yser__write_nolock(p, &on, 1);
    if (r == 1) {
        /* The width runs from the onset write, not from entry, so the syscall
         * is not subtracted from it. */
        uint64_t at = yrt_now_ns() + (uint64_t)usec * 1000ull;
        a->off_byte  = off;
        a->off_at_ns = at;
        a->armed     = 1;
        /* A positive return is this job's seq; only a negative one is an
         * error. The armed/deadline pair is what this header identifies the
         * pending byte by, so the seq is not kept. */
        if (yrt_worker_submit(&a->rt, at, yser__off_job, a) < 0) {
            /* The worker refuses a submit only when it is not running, which
             * the role contract forbids while a write is in flight. The onset
             * is already on the wire, so the trailing byte goes out here
             * rather than never: a latching box holding a trigger code is the
             * worse failure. */
            a->armed = 0;
            (void)yser__write_nolock(p, &off, 1);
        }
    } else {
        /* No onset, no trailing edge. A stale `off` from an earlier pulse is
         * worse than none, because the caller will report this one as failed. */
        a->armed = 0;
        (void)yrt_worker_cancel(&a->rt);
    }
    yser__unlock(a);
    return r;
}

#endif /* YSER_NO_THREADS */

/* ======================================================================= *
 *  PLATFORM-INDEPENDENT ENTRY POINTS
 * ======================================================================= */

/* A location filter matches whole path elements: "1-1.4" is the fourth port
 * of a hub, and it must not also match "1-1.41:1.0" on a machine with more
 * than nine ports on that hub. '.', ':' and '-' are the element separators
 * sysfs and Windows use. */
static bool yser__location_matches(const char* location, const char* prefix) {
    if (!yser__iprefix(location, prefix)) return false;
    char next = location[strlen(prefix)];
    return next == '\0' || next == '.' || next == ':' || next == '-';
}

static bool yser__port_matches(const yser_port_filter* f, const yser_port_info* i) {
    if (f->vid && f->vid != i->vid) return false;
    if (f->pid && f->pid != i->pid) return false;
    if (f->serial_number && !yser__ieq(f->serial_number, i->serial_number)) return false;
    if (f->location && !yser__location_matches(i->location, f->location)) return false;
    if (f->description && !yser__icontains(i->description, f->description)) return false;
    if (f->name && !yser__icontains(i->name, f->name)) return false;
    return true;
}

YSER_API int yser_find_ports(const yser_port_filter* filter, yser_port_info* out, int max) {
    if (!filter) return yser_list_ports(out, max);
    int n = yser_list_ports(NULL, 0);
    if (n <= 0) return n;
    /* One scratch list per call. Enumeration is a setup-time operation, not
     * a per-trial one, so the allocation is not worth designing around. */
    yser_port_info* all = (yser_port_info*)malloc((size_t)n * sizeof(*all));
    if (!all) return YSER_ERR_IO;
    int got = yser_list_ports(all, n);
    if (got < 0) { free(all); return got; }
    if (got < n) n = got; /* a port vanished between the two calls */
    int count = 0;
    for (int k = 0; k < n; k++) {
        if (!yser__port_matches(filter, &all[k])) continue;
        if (out && count < max) out[count] = all[k];
        count++;
    }
    free(all);
    return count;
}

YSER_API bool yser_open(yser_port* p, const yser_desc* desc) {
    if (!p) return false;
    /* A handle that still owns a port would lose its descriptor (or HANDLE)
     * and its worker thread to the memset below, with no way to get either
     * back. One flag test is cheaper than the leak. It only catches a caller
     * who broke the "zeroed or closed" contract in this direction: an
     * uninitialized handle has no valid flag either way. */
    if (p->is_open) {
        yser__set_error(p, "yser_open: handle already open on %s; yser_close() it first",
                        p->device);
        return false;
    }
    yser_desc d;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    /* This memset is also what clears the message, so yser_error() only ever
     * describes the most recent yser_open(). Nothing else in the library
     * writes it, which is what lets a reader and a writer share a handle. */
    memset(p, 0, sizeof(*p));
#if defined(YSER__POSIX)
    p->fd = -1;
    p->wake_fd[0] = p->wake_fd[1] = -1;
#endif

    /* Validate and resolve the description first, so a bad desc fails loudly
     * and identically on every platform, before any OS call. Bindings pass
     * raw ints for the enums, so range-check them too. */
    if (!d.device || !d.device[0]) {
        yser__set_error(p, "desc.device is required (e.g. \"COM3\", \"/dev/ttyUSB0\")");
        return false;
    }
    /* Bare "COMn" only resolves for n <= 9 without the device-namespace
     * prefix; add it unconditionally, it is valid for every n. */
    size_t namelen = strlen(d.device);
#if defined(YSER__WINDOWS)
    bool prefixed = yser__iprefix(d.device, "COM") &&
                    d.device[3] >= '0' && d.device[3] <= '9';
    if (prefixed) namelen += 4;
#endif
    /* Refusing beats truncating: a truncated name opens a different port, or
     * fails with a message that does not look like the name the caller
     * passed. */
    if (namelen >= sizeof(p->device)) {
        yser__set_error(p, "device name too long: %u bytes, limit %u",
                        (unsigned)namelen, (unsigned)(sizeof(p->device) - 1));
        return false;
    }
#if defined(YSER__WINDOWS)
    if (prefixed) yser__copy(p->device, sizeof(p->device), "\\\\.\\");
    else          p->device[0] = '\0';
    yser__append(p->device, sizeof(p->device), d.device);
#else
    yser__copy(p->device, sizeof(p->device), d.device);
#endif
    p->baud      = d.baud ? d.baud : YSER_DEFAULT_BAUD;
    p->data_bits = d.data_bits ? d.data_bits : 8;
    if (p->data_bits < 5 || p->data_bits > 8) {
        yser__set_error(p, "invalid desc.data_bits %u: require 5..8", (unsigned)p->data_bits);
        return false;
    }
    if ((int)d.parity < 0 || d.parity > YSER_PARITY_SPACE) {
        yser__set_error(p, "invalid desc.parity %d", (int)d.parity);
        return false;
    }
#if defined(__APPLE__)
    if (d.parity == YSER_PARITY_MARK || d.parity == YSER_PARITY_SPACE) {
        yser__set_error(p, "mark/space parity is not available on macOS");
        return false;
    }
#endif
    if ((int)d.stop_bits < 0 || d.stop_bits > YSER_STOP_BITS_2) {
        yser__set_error(p, "invalid desc.stop_bits %d", (int)d.stop_bits);
        return false;
    }
    if ((int)d.flow < 0 || d.flow > YSER_FLOW_XONXOFF) {
        yser__set_error(p, "invalid desc.flow %d", (int)d.flow);
        return false;
    }
    p->parity    = d.parity;
    p->stop_bits = d.stop_bits;
    p->flow      = d.flow;
    p->write_timeout_ms = d.write_timeout_ms ? d.write_timeout_ms : YSER_DEFAULT_WRITE_TIMEOUT_MS;
    p->keep_dtr_on_close = d.keep_dtr_on_close;

    /* Resolve the worker's RT reservation with ysp/rt.h's own rules, so the
     * reservation this header accepts and the one yrt_worker_open() accepts
     * cannot drift apart. All-zero means defaults; a zero period becomes the
     * deadline (as the kernel does); anything else that breaks
     * 0 < runtime <= deadline <= period fails loudly here rather than silently
     * degrading one rung (the kernel rejects deadline_ns == 0 with EINVAL). A
     * valid reservation the kernel merely cannot admit still falls back. */
    p->sched = d.sched;
    if (yrt_sched_normalize(&p->sched) < 0) {
        yser__set_error(p, "invalid desc.sched: require 0 < runtime_ns <= "
                           "deadline_ns <= period_ns (period_ns 0 means "
                           "same as deadline_ns)");
        return false;
    }

    if (!yser__open_platform(p, &d)) return false;
#ifndef YSER_NO_THREADS
    /* Start the worker now, so async_policy is final before the first trial
     * and no pulse pays for thread creation. A failure leaves the message for
     * the caller to read and async_policy at YSER_ASYNC_NONE; the port itself
     * is open and usable, so this does not fail the open. */
    (void)yser__async_start(p);
#endif
    return true;
}

YSER_API int yser_write(yser_port* p, const void* buf, int len) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!buf || len < 0) return YSER_ERR_ARG;
    if (len == 0) return 0;
#ifndef YSER_NO_THREADS
    /* With a worker alive every writer-role write goes through its lock, and
     * cancels any pending trailing byte: see yser_pulse_async. */
    if (p->async) return yser__write_cancel(p, buf, len);
#endif
    return yser__write_nolock(p, buf, len);
}

YSER_API int yser_write_byte(yser_port* p, uint8_t value) {
    return yser_write(p, &value, 1);
}

YSER_API int yser_pulse_async(yser_port* p, uint8_t on, uint8_t off, uint32_t usec) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
#ifndef YSER_NO_THREADS
    if (p->async) return yser__async_submit(p, on, off, usec);
#else
    (void)on; (void)off; (void)usec;
#endif
    /* No worker: built with YSER_NO_THREADS, or yser_open() could not start
     * one (its message says why). wr_oserr = 0 tells this apart from a driver
     * error, and nothing is written, because a pulse whose trailing edge
     * nobody will write is worse than no pulse. */
    p->wr_oserr = 0;
    return YSER_ERR_IO;
}

YSER_API int yser_pulse(yser_port* p, uint8_t on, uint8_t off, uint32_t usec) {
    int r = yser_write_byte(p, on);
    if (r != 1) return r;
    /* The deadline is taken after the onset write, so the width does not
     * include that write's own cost. The spin window is ysp/rt.h's platform
     * default; a caller who has measured its rig with examples/rt/jitter.c and
     * wants a different one can call yrt_sleep_until() around two
     * yser_write_byte() calls, which is all this function is. */
    if (usec) (void)yrt_sleep_until(yrt_now_ns() + (int64_t)usec * 1000,
                                      YRT_DEFAULT_SPIN_NS);
    r = yser_write_byte(p, off);
    return r < 0 ? r : 1 + r;
}

YSER_API int yser_read(yser_port* p, void* buf, int cap, uint32_t timeout_ms, int flags) {
    if (!yser_is_open(p)) return YSER_ERR_CLOSED;
    if (!buf || cap < 0) return YSER_ERR_ARG;
    if (cap == 0) return 0;
    if (!(flags & YSER_READ_ALL)) return yser__read_some(p, buf, cap, timeout_ms);

    /* YSER_READ_ALL: repeat single reads against one overall deadline. The
     * first read always happens, so timeout 0 still collects what is
     * buffered. Note for Windows: the remaining time shrinks on every
     * iteration, so every iteration but the first calls SetCommTimeouts.
     * rd_timeout_applied_ms only elides repeated identical timeouts, which is
     * the YSER_READ_ANY case (a frame loop polling with 0, a listener with one
     * fixed timeout). A frame of a few bytes costs a few extra ioctls; if that
     * ever matters, read the frame with YSER_READ_ANY in a loop. */
    uint8_t* dst = (uint8_t*)buf;
    int got = 0;
    bool infinite = (timeout_ms == YSER_TIMEOUT_INFINITE);
    uint64_t deadline = infinite ? 0 : yser_now_us() + (uint64_t)timeout_ms * 1000ull;
    uint32_t remaining = timeout_ms;
    for (;;) {
        int n = yser__read_some(p, dst + got, cap - got, remaining);
        if (n < 0) return n;
        got += n;
        if (got >= cap || n == 0) break; /* done, or the wait timed out */
        if (!infinite) {
            remaining = yser__remaining_ms(deadline);
            if (remaining == 0) break;
        }
    }
    return got;
}

#endif /* YSP_SERIAL_IMPLEMENTATION_GUARD */
#endif /* YSP_SERIAL_IMPLEMENTATION */

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
