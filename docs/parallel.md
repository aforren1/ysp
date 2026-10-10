# ysp/parallel.h design

Status: **v0.4.0, implemented.** Linux (ppdev and direct port I/O) is
tested; the Windows InpOut backend compiles but has not run on a port. The
header's top comment is the manual. This page records why the API looks the
way it does. Until v0.4.0 the design notes lived only in the header.

## Return codes, nanoseconds and version macros (v0.4.0)

v0.4.0 applies the repository conventions (README.md, Conventions). It breaks
the API.

- **Widths are `int64_t` nanoseconds.** `ypar_pulse()` and
  `ypar_pulse_async()` took `uint32_t usec`. They take `width_ns` now, on the
  `yrt_now_ns()` clock that times the trailing edge already. A negative width
  returns `YPAR_ERR_ARG` before any write.
- **Every call but `ypar_open()` returns `int`.** The writes
  (`ypar_write_data()`, `ypar_set_data_dir()`, `ypar_write_control()`,
  `ypar_set_control_bit()`) and the pulses returned `bool`. They return 0 or a
  negative `YPAR_ERR_*` code now, which `ypar_strerror()` names:
  `YPAR_ERR_IO` (a ppdev ioctl failed), `YPAR_ERR_CLOSED`, `YPAR_ERR_ARG` and
  `YPAR_ERR_ASYNC` (no async worker).
- **The reads return the byte or a code.** `ypar_read_data()`,
  `ypar_read_status()` and `ypar_read_control()` returned a `uint8_t`, and a
  failure showed only as a message. A caller had to clear and test
  `ypar_error()` around each read, which the bindings did. They return 0..255
  or a `YPAR_ERR_*` code now, so `rc < 0` is the test, as for every other
  call.
- **`ypar_get_status_bit()` is not a predicate.** It returns 1, 0 or a
  `YPAR_ERR_*` code. As a `bool`, a failed read was "line low". A status line
  is often a response input (a button wired to the port), so a failure must
  not read as "not pressed".
- **The message belongs to the last failure.** Each data and register call
  cleared `ypar_error()` on entry. Now only `ypar_open()` clears it, and every
  failing call writes it before it returns. The async worker still never
  writes it; it sets `async_write_failed`.

`YPAR_VERSION_MAJOR`, `_MINOR`, `_PATCH` and `_STRING` are new, as in
`ysp/rt.h`. The CHANGELOG moved to the end of the top comment. The USAGE block
zeroes the handle and the desc with `memset()` and tests each return code.
The old block left the handle on the stack, which `ypar_open()` documents as
a leak risk, and its `ypar_desc desc = {0};` did not build as C++: both
structs start with the `ypar_backend` enum, and C++ does not convert 0 to an
enum. `= { 0 }` works for these structs in C only. The USAGE blocks of the
device headers were each compiled as C99 (gcc -Wall -Wextra -Werror) and
C++17 (g++) on 2026-10-10.

The Python and MATLAB bindings keep their API: widths in microseconds and an
exception (or a MATLAB error) on failure. Their C glue converts the units and
tests `rc < 0` instead of the message.

Checked on 2026-10-10 on Windows 11: `tests/compile/parallel.c` (C11, C++17,
`YPAR_NO_THREADS`) names every entry point by type and calls them on a zeroed
handle (codes, message, close twice). MSVC 19.44 (/W4 /WX) and MinGW-w64 gcc
16.1 (-Werror) build the tree, `examples/parallel/trigger.c` included. No port
was opened: this machine has no parallel port.
