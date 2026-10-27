# ysp/net.h

Status: v0.3.0, 2026-10-10 (v0.1.0, 2026-10-09, for the measurements). Lab Streaming Layer (LSL) outlets and inlets on
the ysp_rt clock, with liblsl loaded at run time. This is step 4 of
`docs/devices_spec.md` section 14.2 and the LSL part of `docs/rig_spec.md`
4.7. The header's manual is the reference; this page has the decisions,
the LabStreamer notes, the how-to guides and the measurements.

The UDP triggers and the clock offset between two ysp machines of
rig_spec 4.7 are not in this version.

## How-to: get liblsl

`ysp/net.h` does not link liblsl. It loads it when `ynet_lsl_open()` runs:

| Platform | Names it tries, in order |
|---|---|
| Windows | the `path` argument, `%YSP_LSL_PATH%`, `lsl.dll`, `liblsl.dll` |
| Linux | the `path` argument, `$YSP_LSL_PATH`, `liblsl.so`, `liblsl.so.2`, `liblsl.so.1` |
| macOS | the `path` argument, `$YSP_LSL_PATH`, `liblsl.dylib`, `liblsl.2.dylib` |

1. Get a liblsl release (MIT license) from
   <https://github.com/sccn/liblsl/releases>: on Windows
   `liblsl-<version>-Win_amd64.zip`, which holds `bin/lsl.dll`; on Ubuntu
   the `.deb` or `.tar.gz` for your release (`lib/liblsl.so.<version>`).
2. Put the library beside the program, or set `YSP_LSL_PATH` to its full
   path.
3. Check it: `net_stream_view --list`. It prints the library's version
   and the streams on the network, or the reason the load failed.

For this repository's tests, `tools/vendor_lsl.py` does steps 1 and 2:

```sh
uv run --no-project python tools/vendor_lsl.py --write
```

It downloads liblsl v1.17.7 (the newest release GitHub does not mark as a
pre-release; the v1.18.0 tags are betas) for this platform (`win_amd64`,
or `noble_amd64` or `jammy_amd64` by `/etc/os-release`), checks the
archive's SHA-256 (the value GitHub's release API states) and the SHA-256
of each file it takes out, and writes only the library and the C headers
to `third_party/liblsl/<platform>/`, which git ignores. CMake then builds
`compile_lsl_abi` (the header's 28 declarations checked against that
`lsl_c.h`) and points `net_loopback` at the library. Without it, configure
says how to fetch it, and only the fake-liblsl test runs. `YSP_LSL_DIR`
names another folder. The macOS build is a `.pkg` installer, which the
script does not open.

On Windows `lsl.dll` and on WSL2 `liblsl.so` loaded by the default names.
The macOS names were not checked against a release.

## How-to: send markers

```c
static ynet_lsl lib;
static ynet_outlet out;
ynet_outlet_desc d = { 0 };
if (!ynet_lsl_open(&lib, NULL)) die(ynet_lsl_error(&lib));
d.lib = &lib;
d.role = "marks";
d.key = "lsl:ysp-markers:Markers:booth2-ysp";   /* name:type:source_id */
d.device = 3;                                   /* the role's index */
d.format = YNET_STRING;
d.ring = &log_ring;
if (!ynet_outlet_open(&out, &d)) die(ynet_outlet_error(&out));
int rc = ynet_out_mark(&out, 12, "target_on");  /* now */
if (rc < 0) fprintf(stderr, "marker: %s\n", ynet_strerror(rc));
rc = ynet_out_mark_at(&out, 12, "white", flip_onset_ns);   /* a past or future ysp_rt time */
```

The output calls return a code and never write `ynet_outlet_error()`:
several threads may call them at once.

`examples/net/marker_send.c` does this and prints the cost of each call.

## How-to: read a stream

```c
static ynet_inlet in;
static unsigned char mem[YNET_STREAM_BYTES(65536, 6)];
static ynet_stream ring;
if (ynet_stream_init(&ring, mem, sizeof mem, 6) < 0) die("stream ring");
ynet_inlet_desc d = { 0 };
d.lib = &lib;
d.role = "ref";
d.key = "lsl:Data::";              /* any stream named Data */
d.device = 4;
d.format = YNET_FLOAT32;
d.channels = 6;
d.stream = &ring;                  /* every sample */
d.edges[0].channel = 0;            /* channel A: a photodiode */
d.edges[0].level = 1.0f;
d.edges[0].hysteresis = 0.2f;
d.n_edges = 1;                     /* edges as SYNC events to the sink */
d.sink = to_bridge;                /* yscr_push_input() */
d.ring = &log_ring;
if (!ynet_inlet_open(&in, &d)) die(ynet_inlet_error(&in));
/* open returns while the inlet resolves its key */
if (ynet_inlet_wait_running(&in, YRT_S(5)) < 0) die(ynet_inlet_error(&in));
```

The data writer drains `ynet_stream_read()`; a gaze-contingent display reads
`ynet_stream_newest()`. `examples/net/stream_view.c` prints the rate, gaps,
the clock offset and the fit of any stream once a second.

## How-to: check display onsets with the LabStreamer

This is the hand test of the Neurobehavioral Systems LabStreamer as the
rig's reference instrument. The commands are in "LabStreamer hand test"
below.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Prefix | `ynet_`, `YNET_` | rig_spec 4.7 named it; the README has no other `ynet_` |
| Dependencies | `ysp/rt.h` and `ysp/input.h`. Glue for `ysp/screen.h` (a trigger channel that sends a marker at the flip) and `ysp/device.h` (a role index from `ydev_roles`) when those come first | An LSL rig need not build `ysp/serial.h` and `ysp/box.h`; the glue pattern is `ysp/device.h`'s |
| liblsl | Loaded at run time (`LoadLibraryExA` without the current folder, as `ysp/parallel.h` loads InpOut; `dlopen`), 28 functions declared in the header from liblsl's `lsl_c.h` (read at `sccn/liblsl` `main` on 2026-10-09; the same names resolved in liblsl 1.17.5); the table is a struct the caller can fill (tests fill it with a fake) | devices_spec principle 4 and decision 7. A fake table runs every path of the header in CI without liblsl and without a network |
| Instance type | Two new instance types, `ynet_outlet` and `ynet_inlet`, not a family of `ysp/device.h` | `ysp/device.h`'s instance is a byte transport with a `ysp/box.h` decoder, timer queries and encoders. LSL moves typed samples with the sender's stamps, and liblsl owns the connection, so none of that applies. What the device model needs is kept: the lifecycle and its state numbers, roles, the record layouts, the sink, the source entry and a stream ring |
| Records | Source `YRT_SRC_NET` (10, reserved in `ysp/rt.h`); kinds 1, 2, 3 and 5 have the layouts of `YDEV_REC_STATE`, `_GAP`, `_TEXT` and `_OUT`; kinds 32 and up are this header's; `aux` is the role index | One reader for device and LSL records; the role index joins them, as for devices. `ysp/device.h` is not changed |
| Roles in the rig profile | `ysp/rigfile.h` v0.2.0 takes family `lsl` with a key `lsl:<name>:<type>:<source_id>:<hostname>`; `yrig_open_device()` (v0.2.0: `yrig_start()`) refuses it with a message that names `ysp/net.h`; `yrig_source_role()` gives tier 1 to a checked `lsl` input as to the other device-clock families | The profile names a stream as it names a port; the player starts the instance with this header |
| Match key | `lsl:<name>:<type>:<source_id>:<hostname>`; an empty field matches anything; a value with `'` is refused (XPath 1.0 has no escape for it) | The form of the serial keys. More than one match: the inlet stays OPENING and logs "ambiguous", as two FTDI cables with blank EEPROMs would. Add the source_id or the hostname |
| Identify | A stream whose format or channel count is not `desc.format` / `desc.channels` is FAILED | A key that finds the wrong stream must fail before the session (devices_spec 4.6) |
| Reconnect | liblsl's own recovery is off (`recover` 0). Lost error or silence: LOST, the inlet destroyed, resolved again every `retry_ns`; a GAP record when it runs again; both fits restart | The lifecycle and the gap must be in the log (devices_spec 4.7). liblsl's silent recovery hides both. A restarted outlet can be on another clock |
| Silence | LOST after `silent_ns` (1 s) without a sample for a regular stream; never for an irregular one (markers), which goes LOST only on liblsl's lost error | A marker stream is silent by nature |
| Clock: local | A BRACKET fit (`ysp/rt.h`) of `lsl_local_clock()` against the ysp_rt clock: the tightest of 8 brackets every second | liblsl's clock is `std::chrono::steady_clock`. On Windows that is QPC as ysp_rt is, on Linux `CLOCK_MONOTONIC`, but neither is a documented promise. A fit costs 0.1 us a pair and states its width |
| Clock: remote | `lsl_time_correction_ex()` asked every second (liblsl answers from its cache and probes again on its own schedule); each new estimate is a pair of an UNBIASED (least squares) fit: the remote time, and the ysp_rt time of remote time plus offset; round trips over `max_unc_ns` (5 ms) refused | liblsl's offset is the midpoint of its best of several probes (`time_receiver.cpp`, "as in NTP"), off either way by the path's asymmetry. Measured ("Remote fit" below): least squares beat a lower envelope of bracket ends by 1.7 to 3.0 times and the newest offset alone by 1.2 to 3.6 times (max error after 10 s); BRACKET was the first build and is deleted. This is the XDF practice (raw stamps, offsets beside them, refit later) done online, with drift. No liblsl post-processing (`proc_clocksync`, `proc_dejitter`): the raw stamps and the offsets go in the log, so the analysis can refit |
| Event times | The remote fit's map of the sample's stamp; before it has a pair, the local map of stamp plus the newest offset; clamped to the read time (counted) | As `ysp/device.h`: an event cannot happen after it arrived |
| `unc_us` | Half the newest round trip or the remote fit's spread, whichever is larger, plus the local fit's widest bracket; 65535 before any correction | Half the round trip bounds an asymmetric path's error. A bound from the fits' own evidence, not a measurement |
| `ticks` | The remote stamp in microseconds, low 32 bits, with `YIN_DEVTICKS` | The OFFSET records carry the whole stamps for an offline refit |
| Stamp tier | `YIN_TIER_UNKNOWN`, stamp source SDK | No loopback has checked an LSL source. Kothe et al. 2025 measured 6 to 20 ms per-device offsets that LSL cannot see |
| Markers in | One event per sample: kind `desc.kind` (SYNC by default), PRESS, `code` the integer marker or the decimal value of a string marker (0 when it is not one); the text to `desc.text_sink` and to TEXT records | `yin_event` has no room for text |
| Numbers in | Every sample to `desc.stream` (a ring in the caller's memory); threshold crossings on up to 8 channels as PRESS (up) and RELEASE (down) events, hysteresis per edge | devices_spec 4.8: samples to the stream, events to the bridge. The edge detector is the analog edge detector of devices_spec 10.1, in its simplest form |
| Stream ring | Single producer (the reader), single consumer; fixed records (ysp_rt ns, the LSL stamp, the channels); full means the new sample is dropped and counted, with a DROP record at most once a second; a newest-sample slot behind a try-lock: the reader skips the update when a display holds it, and the next chunk updates it | A drop must never overwrite what the writer has not read; a newest-sample reader must never block the reader. A sequence lock was the draft; its copy of racing bytes is a data race in C11, which ThreadSanitizer would report |
| Gaps in a regular stream | A step between stamps above 1.5 periods is a STREAM_GAP record with the samples missing | The data file must say where samples are missing |
| Outlet timestamps | `lsl_local_clock()` read inside a ysp_rt bracket at each push; the record holds the bracket and the LSL stamp; `ynet_out_mark_at()` stamps a ysp_rt time mapped back through the local fit | The push and its LSL time are both in the log; a marker stamped with the flip's onset lets a receiver measure light minus onset directly |
| Markers at the flip | `ynet_trigger_channel()` for `ysp/screen.h`: the marker goes at the planned vblank, stamped with the deadline | No second scheduler, as `ysp/device.h` |
| Threads | One reader thread per inlet, elevated; `desc.manual` and `ynet_inlet_poll()` for none; outlets have no thread (liblsl sends). At open the reader waits in the first time correction (liblsl probes for it); samples queue in liblsl meanwhile and are mapped correctly after | devices_spec 4.8. Not waiting would give the first events the read time and unc_us 65535. The loopback's first run showed the wait as a 90 ms tail in marker latency when markers came during it |
| Same-host offset | Not assumed 0: liblsl's estimate is used for a stream of this machine too | Measured: liblsl's offset for a same-host stream was -7 to -16 us, not 0, and the mapping error followed it. A rule "hostname equal, offset 0" would remove it; not done, because the hostname is the sender's claim |
| Allocation | None after start, except what liblsl does inside | devices_spec principle 5. liblsl allocates each string sample; the header frees it with `lsl_destroy_string()` |

### Open, close, nanoseconds and LSL's own values (v0.2.0)

v0.2.0 applies the repository conventions (README.md, Conventions). It
breaks the API.

| Question | Decision | Why |
|---|---|---|
| The library | `bool ynet_lsl_open(lib, path)`, `void ynet_lsl_close(lib)`, `ynet_lsl_error(lib)`; were `ynet_lsl_load()` returning `YNET_ERR_NOLIB` or `YNET_ERR_SYMBOL`, and `ynet_lsl_unload()` | Loading the library fills a handle: an open. The two codes told apart only what the message already says (where it looked, or which function is missing), and no caller acted on the difference. The codes stay defined for `ynet_strerror()` |
| Outlets and inlets | `ynet_outlet_open/close()`, `ynet_inlet_open/close()` | They take a desc and make liblsl objects and a thread: an open |
| The first sample | `ynet_inlet_wait_running(in, timeout_ns)` and `YNET_ERR_TIMEOUT`, as `ydev_wait_running()` | `ynet_inlet_open()` returns while the key resolves. The caller had to poll the state itself |
| Key and role | Copied into the handle: `YNET_KEY_MAX` 240 (the size of `ynet_found.key`), `YNET_ROLE_MAX` 32; longer fails the open | The reader resolved `desc.key` again after every loss through the caller's pointer |
| Times | `ynet_resolve_all(..., wait_ns)`, `ynet_inlet_poll(in, timeout_ns)`, `desc.read_timeout_ns` | Every C time is `int64_t` ns. liblsl takes seconds; the header converts at the call |
| LSL's values | `desc.lsl_max_buflen` (was `buffer_s`), `ynet_stats.lsl_offset_s` and `lsl_rtt_s` (were `offset_s`, `rtt_s`) | They are liblsl's numbers in liblsl's units: `max_buflen` is seconds, or hundreds of samples for an irregular stream, and the time correction is in seconds on the LSL clocks. The `lsl_` prefix says so, as on the stamps (`lsl_s`), which stay double seconds |
| `ynet_stream_init()` | Returns 0 or `YNET_ERR_ARG` | It is an init that checks its arguments |

Checked on 2026-10-10 on Windows 11, MSVC 19.44 (/W4 /WX) and MinGW-w64
gcc 16.1 (-Werror): `tests/adapt/net_test.c` with new checks (the loader's
messages and a close on a zeroed lib, after a failed open and twice; the
copied key and role; the role limit; the wait reaching RUNNING, on a closed
inlet, on a wrong stream at once with its reason, and a timeout with nobody
sending). `tests/compile/net.c` names every entry point by type. Mutants
`net-18` (the inlet keeps the caller's key) and `net-19` (a FAILED wait
waits out its timeout) are new; both were caught.

### liblsl's time correction in nanoseconds (v0.3.0)

The user decided on 2026-10-10 that `ynet_stats.lsl_offset_s` and
`lsl_rtt_s` become `int64_t` ns, as every other time in a C API
(conventions.md, Units). It breaks the API; the struct keeps its size and
layout (two 8-byte fields).

| Question | Decision | Why |
|---|---|---|
| Names | `lsl_offset_ns`, `lsl_rtt_ns` | `lsl_` still says whose numbers they are: a difference between two LSL clocks and liblsl's round trip, not ysp_rt times. `_ns` says the unit |
| Rounding | `llround(s * 1e9)` at the `lsl_time_correction_ex()` call, once | To nearest, and symmetric for a negative offset. The old record payload truncated (`(int64_t)(off * 1e9)`), so a negative offset was off by up to 1 ns toward 0; the YNET_REC_OFFSET record now carries the same rounded value as the stats |
| The maps | The stamp plus `lsl_offset_ns * 1e-9` | The local map read the double before; the rounded value moves a mapped time by at most 0.5 ns, below the fit's own step (1 ns ticks) |
| The stamps and `lsl_max_buflen` | Unchanged: double seconds, liblsl's units | They are values on liblsl's clock and liblsl's own parameter (conventions.md keeps them as the exception) |

Checked on 2026-10-10: `tests/adapt/net_test.c` compares both fields with
`llround()` of the fake liblsl's last estimate (362 checks); the compile
check takes their addresses as `const int64_t*`, an error for a `double`.
The readers (`examples/net/stream_view.c`, `labstreamer_flip.c`,
`tests/loopback/net_loopback.c`) print them as before, from ns.
`tests/loopback/net_loopback.c --two --seconds 10` against liblsl 1.17.7
(Windows 11, MSVC 19.44 Release, on the guard) passed: offset -23.0 and
-16.4 us, round trip 100.0 and 92.9 us (one and two processes), mapping
error -35.1 to +6.9 us (v0.1.0: -39 to +68 us). Mutant `net-20`
(truncation, not rounding) is caught, and `net-16`, whose anchor moved,
still is.

### Times without end (v0.3.1)

The sum that ysp/audio.h 0.7.2 fixed (a clock reading plus a duration,
which overflows for `INT64_MAX`) was here too. `ynet__after()` saturates
it at `INT64_MAX`, as in ysp/device.h 0.5.1:

| Sum | What `INT64_MAX` did | Now |
|---|---|---|
| `ynet_inlet_wait_running()`: now + timeout | Timed out at the first look | Waits |
| A resolve: now + `desc.retry_ns` (after a failed resolve, and after a loss) | A resolve at every poll | Never |
| A correction: now + `desc.correct_ns` (at the open, and on the reader) | A correction at every poll | The open's correction, then none |

Checked on 2026-10-27: `tests/adapt/net_test.c` (386 checks) waits with
`INT64_MAX` and `INT64_MAX - 1 ms` on a stream the fake liblsl hides for
300 ms, runs `retry_ns` of `INT64_MAX` after a failed resolve and after a
loss (the outlet back before the next poll), `correct_ns` of `INT64_MAX`,
and a caller's clock moved to 10 s before its end, where the reader's
correction is due and the next one must not be (the fake gives a new
estimate at every call for these). MSVC 19.44 Release, MinGW-w64 gcc
16.1, WSL2 gcc 11.4 at -O2 and with ASan and UBSan. Mutants `ovf-01` to
`ovf-06`: 6 of 6 killed (`ovf-05` and `ovf-06` survived until the fake
gave a new estimate at every call: liblsl's cached estimate is not
counted).

## The LabStreamer

Read on 2026-10-09 from the vendor's manual,
<https://www.neurobs.com/manager/content/docs/labstreamer/index.html>
(sections Overview, Setup, Hardware Ports, Web Client, Timing Validation,
Stream Specifications, Troubleshooting). Quotes are the manual's.

### What the manual says

- It is a networked box (wired, or 2.4 GHz wireless), with its own clock:
  it "timestamps those hardware events in LabStreamer computer time", and
  "LSL provides tools to convert stimulus computer time to LabStreamer
  computer time". The web client shows a "Network Induced Uncertainty"
  plot, "the uncertainty in the conversion between the stimulus computer
  clock and the LabStreamer clock". The manual recommends a wired network.
- Inputs: four channels A to D, each one of "Phototransistor, Audio Left,
  Audio Right, Touch generator, or general analog inputs (-5 to 5V)", and
  an External channel that takes a float32 LSL stream of no more than
  10 kHz.
- It listens to one trigger stream, chosen in the web client ("The
  displayed name follows the format StreamName;SourceComputerID;StreamGUID").
  For each channel, an event filter (a substring, or a regular expression
  in `<ecode>...</ecode>`) selects the trigger events. Around each
  selected event it searches from Pre-time before to Post-time after "the
  LSL event" for a threshold crossing with a set level and direction.
- Streams it sends:
  - Data: "Sent at 10 kHz. 6 channels of timestamped float32s. Channels
    0, 1, 2, and 3 correspond to A, B, C, and D in the interface. Channel
    4 corresponds to the specified external channel, resampled to 10 kHz.
    Channel 5 corresponds to the digital output (with a range of 0 - 255)".
  - Latencies: "Sent on each event. JSON string containing event_time (s),
    trigger_string, channel (number from 0-3 corresponds to A-D, 4
    corresponds to digital), latency (s), and uncertainty (network
    uncertainty, s)".
  - Messages: "Sent on event or error, string".
- Digital outputs: "8 general digital outputs (0 or 5 V)". Threshold
  Crossing mode: "Only D0-D3 are used. These put out a hardware voltage
  transition (from 0 to 5 V) when channels A, B, C, or D are triggered",
  with "less than one sample (< 0.1 ms) latency"; Min Active and Min
  Inactive debounce them. On-Demand mode: "arbitrary digital values (from
  0-255) are output, controlled by an LSL stream", with a Desired Latency
  "between the LSL timestamp on the received event and when the digital
  voltages are output". Pulse mode: a value and 0 with set active and
  inactive times. The timing validation selects "Digital;LabStreamer" as a
  trigger stream, so its own digital stream is named "Digital" (unverified
  beyond that one line).
- It claims "0.1 ms measurement accuracy".

### What the manual does not say (not read, or not there)

- The LSL names, types and source IDs of the Data, Latencies and Messages
  streams. The section names are used as the names here; unverified.
- The units of the Data channels (volts, or raw counts), and how the
  Data stream's stamps relate to the sampling clock (per-sample stamps, or
  deduced from the rate).
- Whether latency is "hardware event minus LSL stamp" (positive when the
  light is late). The timing validation implies it; the manual gives no
  formula.
- Whether a trigger event that arrives after the hardware event is still
  matched (Pre-time suggests the device keeps a buffer; not stated).
- The device's clock source and its drift.

### How it binds

The LabStreamer is the reference, not a response device, so it gets three
roles, each an inlet or outlet of this header:

| Role | Stream | ysp side | Gives |
|---|---|---|---|
| `marks` (outlet) | a string marker stream, ysp's | `ynet_trigger_channel()`: "white" or "black" (or a code) at the planned vblank, stamped with the deadline | the LabStreamer's trigger stream; select it in the web client |
| `ref` (inlet) | Data, float32, 6 channels, 10 kHz | stream ring; edges on channel 0 (A, the phototransistor) as SYNC events | ysp's own light times, through LSL's clock mapping: an independent check of the LabStreamer's |
| `lat` (inlet) | Latencies, string | `text_sink` gives the JSON; the example parses it with `ysp/json.h` | the LabStreamer's light minus marker stamp, per event, with its network uncertainty |

Because the marker's stamp is the planned vblank, the LabStreamer's
latency is the display's onset offset as the LabStreamer sees it. The
example compares it per flip with ysp's flip record (onset minus planned
vblank) and with ysp's edge on the Data stream minus the onset. Two
estimates of one quantity that use different clock paths: their
difference is the cross-check.

## LabStreamer hand test

On the stimulus computer (this laptop), with the LabStreamer on the same
wired subnet (the manual: disconnect Wi-Fi when the cable is in) and its
phototransistor taped over the top-left corner of the display.

1. `uv run --no-project python tools/vendor_lsl.py --write`, then
   `set YSP_LSL_PATH=%CD%\third_party\liblsl\win_amd64\lsl.dll` (or put
   `lsl.dll` beside the programs; How-to: get liblsl).
2. Build: `cmake --preset msvc-full` (SDL3), then
   `cmake --build build/msvc-full --config Release --target net_stream_view net_labstreamer_flip`.
   The programs are in `build/msvc-full/Release/`.
3. `net_stream_view --list`. The LabStreamer's streams must be listed.
   Note their keys: the manual does not give the stream names, so
   "Data" and "Latencies" below are guesses. Give the real keys with
   `--data` and `--lat`.
4. `net_stream_view --key "lsl:Data::" --seconds 20 --edge 0:1.0:0.2`.
   Expect about 10000 samples a second, no gaps, an offset and a round
   trip. Cover and uncover the phototransistor: channel 0 changes and the
   event count goes up. Read the channel's dark and light values; the
   level is between them.
5. In the LabStreamer web client: Trigger stream `ysp-flips;...` (it
   appears when step 6 runs; restart the program after you select it),
   Trigger oscilloscope checked, Tab A: Channel Phototransistor, Event
   Filter `white`, Threshold at the level of step 4, Direction Up,
   Pre-time 10 ms, Post-time 500 ms.
6. `net_labstreamer_flip --frames 1200 --level <level> --csv ls_flip.csv`
   (WARNING: up to 5 Hz flicker). It waits up to 10 s for both inlets and
   for the LabStreamer to subscribe, runs 20 s of black and white runs of
   6 to 20 frames, and prints, per change and as distributions: onset
   minus stamp, edge minus onset, edge minus stamp, the LabStreamer's
   latency, and edge-minus-stamp minus latency (the cross-check). Exit 0
   when every change got an edge and a latency.
7. Run step 6 again with `--mark record` (the marker sent after the flip
   record, stamped with the measured onset). If the LabStreamer still
   matches every change, it buffers its input over Pre-time, which the
   manual does not say.

The cross-check is the last line. Both numbers measure light minus the
marker's stamp: the LabStreamer through its own clock and LSL's
conversion of the stamp, ysp through LSL's conversion of the Data
stream's stamps. Their difference is the error of the two clock paths
together; its median is a bias, its spread the two paths' jitter.

## Results

On 2026-10-09. Windows: the development laptop (Windows 11, Iris Xe,
on AC), MSVC 19.44 (Release) and MinGW-w64 gcc 16.1 (-O2). Linux: WSL2
(kernel 6.18, 8 CPUs) on the same laptop, gcc 11.4 (-O2), with the
liblsl 1.17.5 installed there (it came with the LabRecorder 1.17.0 `.deb`,
dpkg package `labrecorder`, installed 2026-06-11, which ships
`/usr/lib/liblsl.so.1.17.5`). On Windows, liblsl 1.17.7 from
`tools/vendor_lsl.py`. The vendored 1.17.7 `jammy_amd64` build passed the
quick loopback on WSL2 too. Timings ran under the measurement lock
(`psy-guard.sh time`, load 10 to 17 %).

### Tests

| Check | Result |
|---|---|
| `tests/adapt/net_test.c` | 340 checks pass on MSVC (Release) and MinGW (-O1, -O2); 339 on WSL2 gcc as C11 and C++17, where liblsl is installed, so the loader's no-library branch is not taken. Also under ASan with UBSan and under ThreadSanitizer (WSL2): clean once the test's own stop flag used the header's atomics |
| Compile checks | `tests/compile/net.c` as C99, C11, C++17 and with `YRT_NO_THREADS`, warnings as errors, on MSVC, MinGW and WSL2 gcc |
| The loader | A path that does not exist: NOLIB, the message names the paths tried and `YSP_LSL_PATH`. A library that is not liblsl (`kernel32.dll`, `libm.so.6`): SYMBOL, naming `lsl_library_version`, the library closed |
| Keys | 14 checks: fields, a fifth field, a quote, a tab, 63 and 64 bytes, the predicate, exact matching |
| Markers, one process, virtual clock | 20 string markers with text up to 43 bytes: each event within its `unc_us` of the push; a number as text gives its code; text to the sink and to TEXT records; `mark_at` 12.3 ms back lands within `unc_us`; a mark stamped 50 ms ahead is clamped to its read time and counted; int32 markers; OUT records with both clocks; nothing left allocated (stream infos, inlets, strings) |
| A remote 10 kHz, 6-channel stream, virtual clock | Another clock 987.654321 s away and 50 ppm fast; corrections every 2 s with round trips of 0.2 to 0.6 ms and an offset error uniform within half the round trip (a pessimistic model of liblsl's): 200000 samples, none dropped, no gaps; the map within 150 us after 10 s (all samples -108 to +184 us); the fit at -53.6 ppm against -50.0; 1999 edges, each at its sample |
| Lifecycle, virtual clock | No correction yet: read times, `unc_us` 65535. 5 missing samples: one STREAM_GAP of 5. A 256-sample ring given 300: 44 dropped, one DROP record. 1 s of silence: LOST (SILENT). Back: RUNNING, a GAP record. The outlet gone: LOST (DISCONNECTED) at once. Made again on a clock 5 s away: RUNNING, and the map is the new clock's at the first sample |
| Resolve | No stream: OPENING, one resolve a second, the resolve's wait as set. Two streams for one key: OPENING, "ambiguous 2 streams match" logged once; the source_id tells them apart. Wrong format, wrong channel count, an edge on a missing channel: FAILED after one resolve |
| Hysteresis, refusals | A noisy ramp through the level: one PRESS. Corrections with 8 to 24 ms round trips: all refused, the remote fit empty, `unc_us` at least 4 ms |
| A LabStreamer-like stream, one process | An outlet named Data, 6 float32 channels at 10 kHz, a `ysp-flips` marker outlet as the trigger channel, a Latencies-style string inlet: 10 markers at their deadlines with their names; 10 light edges 5.3 to 5.4 ms after their deadlines; no gaps; the trigger channel's FLUSHED call sends nothing |
| Mutants (`tests/mutate/net.toml`) | 16 of 16 caught, and the control `net-17` (BRACKET for the remote fit) survives as expected: the test's 400 us bound holds for both on its fake. The first run missed `net-08` (the remote fit kept across a reconnect): the fit's own restart rule recovered within 2.5 s, so the test now checks the map at the first sample after the reopen |
| `examples/net/*` without liblsl | Exit 2 with the paths tried (Windows) |
| `tests/loopback/net_loopback.c` without liblsl | Exit 77, a skip (Windows) |
| `tests/compile/lsl_abi.cpp` | The 28 declarations equal `lsl_c.h`'s 1.17.7 (Windows and Linux headers), allowing only pointer as `void*` and enum as `int`; a `float` put in place of a `double` parameter fails the build |
| `net_loopback --two --quick --seconds 3` (CI's run) | Passes on Windows (MSVC, liblsl 1.17.7) and WSL2 (liblsl 1.17.7 jammy); 29 to 30 s |

### Real liblsl: Windows and WSL2

`net_loopback --two --seconds 10`, three runs on each, under the
measurement lock. "Push" is the ysp_rt time just before `ynet_out_mark()`;
the sender wrote it into the marker. Ranges are over the three runs.

| Measure | Windows, liblsl 1.17.7, one process | Windows, two processes | WSL2, liblsl 1.17.5, one process | WSL2, two processes |
|---|---|---|---|---|
| `lsl_local_clock()` minus ysp_rt | 0 at the first bracket (under 0.5 us); 10000 brackets in 2 s within -8 to +18 us of it, p1 to p99 within 0.8 us; bracket p50 0.1 us, p99 0.6 to 1.3 us | the same clock | -1 us at the first bracket; within -28 to +10 us, p1 to p99 within 1.8 us; bracket p50 0.1 us, p99 1.3 us | the same clock |
| Marker latency: the sink's call minus the push | p50 89 to 121 us, p99 269 to 299 us, max 319 to 727 us | p50 96 to 119 us, p99 282 to 411 us, max 340 to 440 us | p50 158 to 194 us, p99 396 to 532 us, max 574 to 596 us | p50 140 to 153 us, p99 472 to 809 us, max 661 to 1173 us |
| Mapping error: the event's t minus the push | p50 -15 to -23 us; all within -39 to +68 us | p50 -12 to -15 us; all within -24 to +10 us | p50 -5.5 to -15 us; all within -31 to +52 us | p50 -4.5 to -13 us; all within -18 to +12 us |
| liblsl's offset for a same-machine stream (truly 0) | -14 to -25 us, round trip 89 to 107 us | -16 to -26 us, round trip 87 to 116 us | -7 to -16 us, round trip 82 to 90 us | -10 to -14 us, round trip 77 to 89 us |
| 10 kHz x 4 channels, float32, 10 s | 100020 to 100030 samples, 0 dropped, 0 gaps, 0 clamped; reader 46 to 51 ns a sample (about 104 a chunk); ring lag up to 22 ms | 100000 samples, 0 dropped, 0 gaps; reader 16 to 24 ns a sample | 100020 to 100060 samples, 0 dropped, 0 gaps, 0 clamped; reader 71 to 114 ns a sample (about 100 a chunk); ring lag up to 21 ms | 100000 to 100010 samples, 0 dropped, 0 gaps; reader 44 to 62 ns a sample |
| Fits after 10 s | local 0.000 ppm, bracket 0.00 us; remote +0.35 to +0.88 ppm (truly 0), spread 5 to 11 us | remote +0.11 to +0.50 ppm, spread 4 to 6 us | local 0.000 ppm, bracket 0.04 us; remote -0.13 to +1.10 ppm, spread 4 to 14 us | as one process |
| Resolve for a missing key | OPENING, 3 resolves in 3 s | | the same | |
| Disappear and return | LOST within 3 s; RUNNING 2.1 s after the outlet was made again; one GAP record (from 0.22 s before the outlet went to 5.14 s after) | | the same, to 10 ms | |

The ring lag is the 10 ms pull plus the test's 10 ms drain. The mapping
error follows liblsl's same-host offset error (see the decision
"Same-host offset"). The reader's cost at 10 kHz is under 0.12 % of one
CPU on both.

**Under load, not diagnosed.** Two Windows runs outside the measurement
lock, while other workers' builds ran (the guard saw 43 to 75 % load
minutes later), delivered the 10 kHz stream at 218 and 1050 Hz with up
to 9.2 s of lag, and markers at a p50 of 42 and 685 ms. liblsl alone, with
none of this header's code (a scratch program: one outlet, one inlet, the
same pushes), did the same then: 0.97 to 1.41 kHz with 4.2 to 4.5 s of
lag. Minutes later, quiet, it delivered 9990 Hz with 2.9 ms of lag. The
machine runs Cisco AnyConnect, Tailscale and four Hyper-V adapters; which
of them, or the load itself, slows liblsl's local TCP was not found. A rig
should run nothing else, and the check is this loopback before a
session.

### Windows, the fake liblsl

`test_net` (MSVC, Release), the reader thread at 10 kHz x 4 channels for
10 s, three runs under the measurement lock: 100010 to 100040 samples, 0
dropped, 0 gaps; reader 28 to 37 ns a sample with 10 samples a chunk
(the fake's 1 ms pushes); ring lag up to 12 to 20 ms. This is the
header's own cost; liblsl's is not in it.

### Remote fit: least squares or a lower envelope

A simulation (a scratch program on `ysp/rt.h`'s fit, not in the
repository) of liblsl-like corrections: a remote clock at a rate error,
corrections every 1 to 5 s with round trips drawn uniformly from 0.5 to
1.5 times a nominal value and the offset off by a uniform error within
half the round trip; 20 seeds, 60 s, the map's largest error after 10 s:

| Rate, round trip, interval | UNBIASED (least squares) | BRACKET (lower envelope of mid + rtt/2) | Newest offset |
|---|---|---|---|
| 50 ppm, 400 us, 2 s | 185 us | 546 us | 375 us |
| 50 ppm, 400 us, 1 s | 158 us | 300 us | 335 us |
| 0 ppm, 400 us, 2 s | 185 us | 546 us | 286 us |
| 50 ppm, 100 us, 2 s | 46 us | 137 us | 165 us |
| 10 ppm, 1 ms, 2 s | 462 us | 1365 us | 719 us |
| 50 ppm, 400 us, 5 s | 406 us | 701 us | 499 us |

liblsl keeps the best of several probes, so its offsets scatter less than
this model's; the order of the three did not change across the rows.

## Not done

- **CI's loopback** runs on Windows and Linux after `tools/vendor_lsl.py`
  (cached by `actions/cache`): about 30 s each, plus a configure and one
  target on Windows. It has not run on GitHub's runners yet; its checks
  (no drops, no gaps at 10 kHz) may need slack there if a runner is as
  slow as the loaded laptop above. macOS has no fetch and runs only the
  fake-liblsl test.
- **The LabStreamer.** Not attached. Its stream names, the Data channels'
  units and the meaning of `event_time` are not in its manual; the
  example's defaults are guesses that `net_stream_view --list` corrects.
  Whether it matches a trigger event that arrives after the light
  (`--mark record`) is unknown.
- **`net_labstreamer_flip`** was built (MSVC with SDL3) but not run: it
  needs liblsl and the device.
- **macOS**: compiled nowhere here; CI builds it.
- **rig_spec 4.7's UDP triggers and the clock offset between two ysp
  machines**: not in v0.1.0.
- **Outlets as ysp/device.h outputs**: `ydev_out_mark()` on a box does not
  also send an LSL marker; a player that wants both calls both.
- **The same-host offset**: liblsl's -7 to -16 us error for a stream of
  this machine is kept (see the decision).
- **A latency table for the rig profile**: no tool stores an LSL role's
  `bounds` yet; `net_labstreamer_flip` prints the numbers a future
  `--store` would write.
