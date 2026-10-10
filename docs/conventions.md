<!-- source: conventions branch README, merge its later edits here -->
<!-- docs branch (readme-slim) text not in the conventions branch's
version; fold in at merge:
- Dependencies: "[Files to copy](status.md#files-to-copy) lists what
  each header includes." (replaces "A transport header includes it, so a
  user copies `ysp/rt.h` and the transport header.")
- Dependencies: "The device headers build on each other too (`ysp/box.h`
  on `ysp/input.h`, `ysp/device.h` on `ysp/serial.h` and `ysp/box.h`,
  `ysp/rigfile.h` on `ysp/device.h` and `ysp/json.h`), and
  `ysp/screen.h`, `ysp/net.h` and `ysp/response.h` include
  `ysp/input.h`." (contradicts "There are no other dependencies between
  headers.")
- Threads: "`ysp/device.h` and `ysp/net.h` have no such macro: their
  manual mode (`ydev_poll()`, `ynet_inlet_poll()`) is the form without a
  thread."
The docs branch's Handles and Errors bullets (per-header split of
`ysp/parallel.h` and `ysp/serial.h`) are superseded by this branch's
Handles, Lifecycle verbs, Return values and Errors bullets.
-->

# Conventions

This page is explanation and reference: the rules that every header
follows.

- **Dependencies.** `ysp/rt.h` is the one header the others use. It holds
  the clock, the deadline waits, the thread elevation, the deadline worker
  and the pump. A transport header includes it, so a user copies `ysp/rt.h`
  and the transport header. The adaptive-method headers (`ysp/stair.h`,
  `ysp/quest.h`, `ysp/aep.h`), `ysp/trials.h`, `ysp/table.h`, `ysp/timeline.h`, `ysp/color.h` and `ysp/outline.h` are pure computation and
  include nothing but the C standard library. The exceptions are the opt-in
  async layer of `ysp/quest.h` and `ysp/aep.h` (`YQST_ASYNC`, `YAEP_ASYNC`),
  which includes `ysp/rt.h` for the pump, and `ysp/trials.h`, which requires
  `ysp/table.h` (v0.2) and compiles its implementation with its own. The rig headers build on each
  other: `ysp/screen.h` on `ysp/rt.h`, `ysp/gfx.h` on `ysp/screen.h` and
  `ysp/color.h`, `ysp/video.h` on `ysp/gfx.h` and `ysp/timeline.h`, and
  `ysp/audio.h` on `ysp/rt.h`. There are no other dependencies
  between headers. Two mechanics
  follow from this. First, the transport's implementation block also
  compiles the implementation of `ysp/rt.h`, unless the same translation unit
  already defined `YSP_RT_IMPLEMENTATION` and included `ysp/rt.h` before it.
  Either order gives exactly one copy of the implementation. Second,
  `Y<X>_NO_THREADS` also sets `YRT_NO_THREADS`. If you include `ysp/rt.h`
  first, it reads its own macro before the transport can set it, so define
  both macros or neither.
- **Names.** `ysp/<name>.h` uses a short, unique function prefix `y<x>_`
  and macro prefix `Y<X>_`, listed in the Prefix column of the [README](../README.md#headers) table. Keep a
  prefix short while it stays unique. The
  implementation macro is `YSP_<NAME>_IMPLEMENTATION`. Private symbols use a
  double underscore (`ypar__set_error`).
- **Handles.** The caller allocates the handle struct and zeroes it. Large
  handles (a timeline, a trial list, a device) belong in static storage. A
  zero field in a desc means "default"; some headers have a required field
  (`yser_desc.device`), and the header says so. Each USAGE block builds as
  C99 and as C++17, so it zeroes a desc and then sets fields
  (`yser_desc d = { 0 }; d.device = "COM3";`). In C and C++20,
  designated initializers do the same.
- **Lifecycle verbs.** One set of verbs, with one meaning each:

  | Verb | Meaning | Returns |
  |---|---|---|
  | `y<x>_open(h, desc)` | Fills a handle from a desc: acquires memory, a thread, a device or a window, or checks the desc and keeps the result. A variant adds a word (`ygfx_open_start()`, `yqst_open_saved()`, `ytb_open_csv()`). A handle that belongs to another handle takes the owner first (`yau_wav_open(&au, &wav, &d)`). | `bool`, true on success |
  | `y<x>_close(h)` | Releases what open acquired. Safe on a zeroed handle, after a failed open, and a second time. Every open has a close, also when the close has nothing to release. | `void` |
  | `start`, `stop` | Begin and end an activity on an open handle (`yrdk_start()` starts a field's motion, `ytl_stop()` stops a timeline base). They never acquire the handle. | `int` |
  | `init` | Fills a plain struct from arguments, without a desc: a value, a decoder's state, an arena on the caller's memory (`ybox_init()`, `yrig_init()`, `yjs_arena_init()`). No close. A struct that grows into heap memory after init has a `_free` (`yjs_arena_free()`, `yol_path_free()`). | `void`, or `int` when it checks its arguments |
  | `load`, `save`, `view` | Read or write a plain struct as bytes or a file (`yrig_load()`, `ycol_cal_load()`). A view keeps pointers into the caller's bytes (`ypak_cset_view()`, `yol_font_view()`). | `int` |

  An object inside an open handle (a texture, a buffer, a sound) is an id.
  The call that makes one has the object's name, and `_free` releases it.
- **Return values.** `open` returns `bool`. A predicate returns `bool`
  (`y<x>_is_open()`, `ytr_done()`, `yscr_poll()` when an event arrived).
  Every other call that can fail returns `int`: 0 or more is success (a
  count, an id, a sequence number, or 0), and a negative value is a
  `Y<X>_ERR_*` code that `y<x>_strerror(code)` names. Test `rc < 0`; do not
  test `!rc`, because 0 is success. A call that cannot fail returns `void`
  or its value.
- **Errors.** The handle holds the message, and `y<x>_error(h)` reads it.
  `open` clears it on entry. Every other call on the handle that fails
  writes it before it returns, in the frame loop too, so the message
  belongs to the last failure. Three exceptions, each stated in its header:
  - A call that the header lets a second thread make on the same handle
    (`yrt_worker_submit()`, `yser_write()`, `ydev_out_pulse()`) returns the
    code and does not write the message, because the write would race.
    `y<x>_strerror(code)` names the code.
  - A call without a handle (`init`, `load`, `view`, a parser of the
    caller's bytes) writes its message into a caller buffer (`char* err,
    size_t cap`). The buffer may be NULL.
  - `ysp/pack.h`: lookups and reads may run on several threads at once.
    Each failure writes the pack's message under an atomic claim, so two
    failures do not mix, and the message is the last failure of any
    thread.
- **Units.** Every time in a C API is an `int64_t` count of nanoseconds on
  the `yrt_now_ns()` clock, unless the name says another clock (`_ticks`
  for a device's own counter, `lsl_` for an LSL stamp). A field or a
  parameter whose name does not already say its unit ends in `_ns`. Write a
  literal with `YRT_S()`, `YRT_MS()` or `YRT_US()` (`.duration_ns =
  YRT_MS(500)`); `YRT_TO_S()` gives seconds back for a person to read.
  `ysp/rt.h` defines these macros, and a header that does not include it
  defines the same ones under the same guard. A bare `0.5` where
  nanoseconds go converts to 0: MSVC warns at its default level (C4244 at
  /W3), and gcc warns with `-Wfloat-conversion`, which the CMake build
  turns on. Seconds stay in files, experiment definitions, scripts, data
  and the Python, MATLAB and Octave bindings. Text for a person (describe
  lines, CSV columns) keeps its units.
- **Threads.** Headers that spawn a worker thread honor `Y<X>_NO_THREADS`
  to drop it, and link `-pthread` on POSIX otherwise.
- **Platforms.** A header `#error`s on platforms it does not support, so an
  unsupported build fails at compile time, not at run time.
- **Documentation.** sokol-style. The header's top comment holds the manual
  in labeled sections (USAGE, platform setup, TIMING, BUILDING, ...), and
  every declaration carries its reference comment. `docs/` holds design notes
  and comparisons: why a header looks the way it does and how it measured
  against other toolboxes, never what the header already says. Code comments
  say why, not how.
- **Timing.** Anything that claims a latency number measures it. Each header
  says how.
