# Status of each header

This page is reference. It lists, for each header, what is built, what
was checked and measured, and what is not built or not measured. It also
lists the files to copy for each header and the hardware that ran each
header. The [README](../README.md) has the short table.

A header needs the OS, the C standard library and the ysp headers in its
row of [Files to copy](#files-to-copy); a few also need an outside
library (SDL3, miniaudio, liblsl).

## Verified on hardware

Only one machine has run these headers on real devices: a Windows 11
laptop (i7-1360P, Intel Iris Xe driving its internal 1920x1200 60 Hz
panel, Realtek speakers). No photodiode, photometer, loopback cable,
external monitor, parallel port, response box, trigger box or
LabStreamer was attached (docs/screen.md, docs/audio.md,
docs/device.md). The one serial device was a Teensy 4.0 echo board on
native USB (the serial and device rows). "Sim" below means simulated displays,
fake transports, virtual serial pairs, null audio devices or software
renderers, in tests and CI.

| Header | Ran on real hardware | Only in sim or CI | Source |
|---|---|---|---|
| screen | Windows 11, Iris Xe, internal panel, 60 Hz: flip records from software timestamps (both swap paths); the web backend's callback times in Chrome, Edge and Firefox | Light (no photodiode); another GPU, rate or display; Linux and macOS (simulated display only); light in a browser | screen.h STATUS; docs/screen.md; docs/web.md |
| gfx | Windows 11, Iris Xe through ANGLE (D3D11) | Light (no photometer or photodiode); WARP, SwiftShader and Mesa llvmpipe are software | gfx.h STATUS; docs/gfx.md |
| video | Windows 11, Iris Xe, Media Foundation with DXVA, a 640x360 window; soundtrack on the real WASAPI device | Light and sound; fullscreen | video.h STATUS |
| audio | Windows 11, WASAPI on the Realtek speakers (shared 48 kHz, exclusive), placement checked by WASAPI loopback capture; capture from the SST microphone array (shared, exclusive, duplex with the speakers); Web Audio in Chrome 154 on the same speakers (the browser's estimates) | Sound at the speaker and capture times against a reference (no loopback cable: no delay to sound or from it is measured); capture on any other backend, and in a browser (compiled, never run); CoreAudio, ALSA, PulseAudio; Web Audio on a phone | audio.h STATUS; docs/audio.md |
| rt | Windows 11: clock, waits, worker, event ring | The device clock fit: no real device has fed it; macOS | rt.h STATUS; docs/rt.md |
| input, response | Windows 11: keys sent with SendInput through SDL 3.4 | A key pressed by hand; mouse, touch, pen, gamepad timing; response boxes | input.h; docs/response.md |
| net | Windows 11 and WSL2: the real liblsl (1.17.5, 1.17.7) over local loopback | No LabStreamer or other LSL device; macOS | net.h STATUS; docs/net.md |
| serial | Windows 11, i7-1360P, 2026-10-11: the Windows backend on a Teensy 4.0 echo board over native USB CDC (usbser), through the MATLAB MEX binding. Round trip 172 to 288 us median, 279 to 424 us p99, against a raw Win32 floor of 167 to 229 us; IOPort with BlockingBackgroundRead=1 PollLatency=0 matches it | Linux virtual port pairs only; macOS, USB-serial bridges (FTDI) and RS-232 never ran | docs/serial.md, "Results with the Teensy 4.0 echo board" |
| parallel | none on Windows; the "Linux tested" note does not say whether a real port was used | The Windows backend never reached hardware | parallel.h STATUS |
| box | none | Pure protocol code; no byte from or to a vendor device; the firmware never ran on a board | box.h STATUS; docs/box.md |
| device | Windows 11, i7-1360P, 2026-10-12: host-timed outputs (the TriggerBox byte path, DTR, RTS) into a Teensy 4.0 echo board over native USB CDC (usbser); width error -65 to -16 us median | Inputs from a real board; every vendor box, the line board, the parallel port; FTDI bridges; Linux and macOS | docs/device.md, "Host-timed pulses on a Teensy 4.0" |
| rigfile | none | Binds through a fake transport | docs/device.md |
| color | not applicable (pure computation) | No number is a measurement of light | color.h; docs/color.md |
| stair, quest, aep, trials, table, json, pack, timeline, rdk, outline | not applicable (pure computation) | | |
| dataset | not applicable (files only): NTFS on Windows 11, ext4 on WSL2, Emscripten's in-memory file system | A power cut; macOS | docs/dataset.md |

## Files to copy

The `#include` lines of the headers (2026-10-10) give these sets. Copy
every file in the row into one `ysp/` folder on your include path. Where a
row has more than one header, define each implementation macro once
([Quick start](../README.md#quick-start), step 2); a header compiles the implementations it includes
unless your file already did.

| Header | Includes | Copy these files | Outside dependencies |
|---|---|---|---|
| rt | | rt.h | Tracy (opt-in) |
| stair, table, json, pack, timeline, input, color, rdk, outline | | the header | none |
| quest, aep | rt.h with `YQST_ASYNC`, `YAEP_ASYNC` only | the header (+ rt.h for the async layer) | none |
| trials | table.h | trials.h, table.h | none |
| dataset | trials.h, json.h | dataset.h, trials.h, table.h, json.h | none |
| parallel | rt.h | parallel.h, rt.h | InpOut driver on Windows |
| serial | rt.h | serial.h, rt.h | none |
| response | input.h | response.h, input.h | none |
| box | input.h | box.h, input.h | none |
| device | rt.h, input.h, box.h, serial.h | device.h, rt.h, input.h, box.h, serial.h (+ parallel.h for `YBOX_PARALLEL`) | none |
| rigfile | device.h, json.h | rigfile.h, json.h, device.h, rt.h, input.h, box.h, serial.h | none |
| net | rt.h, input.h | net.h, rt.h, input.h | liblsl, loaded at run time |
| screen | rt.h, input.h | screen.h, rt.h, input.h | SDL3; ANGLE's DLLs at run time on Windows |
| audio | rt.h | audio.h, rt.h | miniaudio 0.11.25 (`YAU_NO_MINIAUDIO`: none) |
| gfx | screen.h, color.h | gfx.h, screen.h, color.h, rt.h, input.h (+ outline.h for `ygfx_text_make()`) | as screen |
| video | gfx.h, timeline.h | video.h, gfx.h, timeline.h, screen.h, color.h, rt.h, input.h | as screen; pl_mpeg for MPEG-1 (optional) |

ysp/response.h also has adapters for ysp/screen.h, ysp/audio.h,
ysp/timeline.h and ysp/trials.h; they exist when you include those headers
before it. ysp/gfx.h has `ygfx_text_make()` when you include ysp/outline.h
before it.

## Headers

Each entry gives the prefix, the platforms, what the header needs, its
purpose, and its status by version, newest first.

### ysp/rt.h

- Header: [ysp/rt.h](../include/ysp/rt.h)
- Prefix: `yrt_`, `YRT_`
- Platforms: Windows, Linux, macOS, WebAssembly (Emscripten, no scheduling ladder)
- Needs: OS only; Tracy, opt-in

Purpose: The shared base: monotonic clock, deadline waits, thread elevation, a one-shot deadline worker, a background pump for inference between trials, a wait-free event ring for the rig's log, clock correlation, a device clock fit (a box's or a board's own clock mapped to the rig's: lower envelope, bracketed reads or least squares, with unwrap and restart), per-user cache and config folders with ownership checks, and instrumentation macros (nothing, Tracy, or the ring). The transport headers and the async layers of the adaptive headers use it.

Status:

- Bindings: `ysp-rt` 0.9.0 (Python) and the MEX `ysp_rt` 1.0.0: the clock (the one `ysp.serial.now()` and `ysp.parallel.now()` read, checked), deadline waits (the GIL released), clock correlation, the device clock fit, the virtual clock and the per-user folders, in seconds; worker, pump, ring and the scheduling ladder are not bound. On Windows a binding's wait returns the header's lateness (median 0.1 us with the default spin); the host's return adds 8 us in Python, 37 to 40 us in MATLAB and 65 us in Octave at the median. Tests pass on Windows and WSL2 (Python), MATLAB R2023a and Octave 10.1. [Bindings](rt.md#bindings).
- v0.9.0: a virtual clock for simulated runs (`yrt_virtual_begin()`, virtual timers), and `yrt_real_ns()`, the real clock for costs on either clock: the driver thread's waits move it, other threads wait for it, and ysp/screen.h's SIM backend, ysp/audio.h's null device and ysp/video.h follow it. The CI list of 49 `--sim` and `--null` runs gave the same data files and output in three idle runs and two beside 24 CPU-bound processes (MSVC), and the same data with MinGW-w64; the lines that differ print real CPU time. Real-clock cost inside the run-to-run spread: `yrt_now_ns()` 24.3 to 26.3 ns against 23.5 to 26.1 ns pinned, the screen's per-frame header cost unchanged. 13 of 13 new mutants caught ([Virtual clock](rt.md#virtual-clock-v090)).
- v0.8.0 (the [Conventions](conventions.md): `yrt_now_ns()` and every time `int64_t` ns, `YRT_S()`/`YRT_MS()`/`YRT_US()`, worker and pump open/close, `yrt_fit_open()`; 2 mutants added, both caught).
- v0.7.0 (per-user folders: `yrt_user_dir()`, the rules of gfx's program cache folder, tested on Windows, WSL2 and emcc).
- v0.6.0 (the device clock fit: on synthetic devices within tens of us of the truth for dense pairs, ms for sparse pairs behind a 16 ms timer; 17 of 17 mutants caught; no real device yet).
- v0.5.0: event ring stress-tested with 2 to 8 producers (no torn or lost record unaccounted for), ThreadSanitizer clean; a push costs 11 to 37 ns natively and a TIME_CRITICAL producer's p99.9 stays near 2 us while 7 others push, against 0.1 to 0.5 ms behind a lock; Tracy mode built against Tracy v0.14.1 ([design notes and tables](rt.md)). Pump built and run on Windows, Linux and under node (Emscripten); on MinGW the worker and pump threads inherit the x87 control word so a thread computes the same bits as the caller, ThreadSanitizer clean including a wait-versus-stop hammer; the deadline worker as before: Windows measured with [rt_jitter](../examples/rt/jitter.c); Linux built and run under WSL2, worker clean under ThreadSanitizer; CI compiles the macOS backend, which has not run on a Mac. No test host grants CAP_SYS_NICE, so the SCHED_FIFO and Mach time-constraint rungs are unexercised.

### ysp/parallel.h

- Header: [ysp/parallel.h](../include/ysp/parallel.h)
- Prefix: `ypar_`, `YPAR_`
- Platforms: Windows, Linux
- Needs: ysp/rt.h; InpOut driver on Windows

Purpose: Parallel port (LPT) trigger output and line input, blocking and async pulses.

Status:

- Bindings: `ysp-parallel` 0.4.0.1 (Python) and the MEX `ysp_parallel` take and return every time in seconds. [Bindings](bindings.md#versions).
- v0.4.0 (the [Conventions](conventions.md): pulse widths in `int64_t` ns, every call but open returns 0, a byte or a `YPAR_ERR_*` code, `ypar_strerror()`, version macros).
- v0.3, on ysp/rt.h. Linux tested; Windows backend compiled but not run on hardware. [Design notes](parallel.md).

### ysp/serial.h

- Header: [ysp/serial.h](../include/ysp/serial.h)
- Prefix: `yser_`, `YSER_`
- Platforms: Windows, Linux, macOS
- Needs: ysp/rt.h

Purpose: Serial port (RS-232, USB-serial, USB-CDC) byte I/O for trigger and response boxes, blocking and async pulses.

Status:

- Hardware: the Windows backend ran on a Teensy 4.0 over native USB CDC (usbser), 2026-10-11, through the MATLAB MEX: round trip 172 to 288 us median, 279 to 424 us p99, against a raw Win32 floor of 167 to 229 us; IOPort with BlockingBackgroundRead=1 PollLatency=0 matches it. macOS and USB-serial bridges (FTDI) and RS-232 never ran. [Results](serial.md#results-with-the-teensy-40-echo-board).
- Bindings: `ysp-serial` 0.6.1 (Python) and the MEX `ysp_serial` 2.2.0 take and return every time in seconds; both have an async line reader (the Python one since 0.5.0.2). [Bindings](bindings.md#versions).
- v0.6.1: `yser_open()` reads the port's driver on Windows (SetupAPI `SPDRP_SERVICE`; new `yser_port.driver`, `yser_port_info.driver`), and `yser_set_rts()` sends the extra DTR request only on `usbser.sys` (new `yser_port.rts_via_dtr`). New `yser_pulse_off_at_ns()`, the trailing-edge rule that ysp/device.h 0.3.1 uses. On the Teensy 4.0: 0 of 1200 RTS pulses missed, the open 3.9 to 4.0 ms median against 4.8 to 4.9 ms with 0.6.0 (the walk is inside the open's spread). The path without the extra request (FTDI and other drivers) did not run: no such device here. A DTR and RTS call tail (3 to 7 % of calls over 1 ms, also in raw `EscapeCommFunction`) came with a load of orphaned `find /` processes on the machine. [Driver detection](serial.md#driver-detection-v061), [The DTR call tail](serial.md#the-dtr-call-tail).
- v0.6.0: `yser_pulse_async()` only queues (20 us median, was 462 us) and the worker writes both bytes, in call order with every other write; both pulse calls time the width from the onset write's start, at least `desc.min_hold_ns` (0.25 ms) after its return (width error -57 to -22 us median, was +0.34 to +0.42 ms); `yser_set_rts()` reaches a USB CDC device under `usbser.sys` (0 of 1600 missed, was 160 of 160). Measured on the Teensy 4.0; the async onset is 49 us later (median) than the synchronous one. [Results with v0.6.0](serial.md#results-with-v060), [RTS on usbser.sys](serial.md#rts-on-usbsersys).
- v0.5.1: the TIMING comment gives the pulse onset and width measured on a Teensy 4.0 over `usbser.sys` (onset 176 to 185 us median, DTR 83 us; width +0.34 to +0.42 ms median, p99 up to +0.69 ms; 0 of 19200 missed; RTS alone never sent) and names what is not measured (FTDI, RS-232, Linux, macOS). No code change. [Pulses](serial.md#pulses-onset-and-width).
- v0.5.0 (the [Conventions](conventions.md): widths, timeouts and the break in `int64_t` ns, `YSER_TIMEOUT_INFINITE` is -1, `yser_now_us()` removed, version macros).
- v0.4, on ysp/rt.h. Linux tested against a virtual port pair; the Windows and macOS backends compiled in CI but had not run on a device. [Design notes](serial.md).

### ysp/trials.h

- Header: [ysp/trials.h](../include/ysp/trials.h)
- Prefix: `ytr_`, `YTR_`
- Platforms: any
- Needs: ysp/table.h

Purpose: Trial sequencing above the adaptive methods: conditions and repetitions (constant stimuli), sequential, random and constrained-random orders, interleaved adaptive tracks, blocks, practice and catch trials, re-queues, tallies, a replayable history. From v0.2: conditions files and trial lists as tables, order lists, draws with replacement, subsets, blocked and alternating groups in Latin-square order, units ("b always follows a"), exact first-order transition balance, Latin and Williams squares by participant, and a rules text with one statement per C call. From v0.2.1: jitter, a duration per trial (uniform, a list, or a non-aging truncated exponential, optionally whole frames) drawn in a fixed order, logged and replayed. From v0.4.0: a data writer, one CSV row per trial with the session's columns and the caller's (a struct or values set per row; times in int64 ns written as seconds), flushed to the OS per row, read back as written by ysp/table.h. No heap; no I/O but the data writer's file.

Status:

- v0.6.0: four gaps of the [experiment definition](experiment_format.md) closed. G3: variables drawn per trial (`desc.vars[]`, `ytr_var_choice()`, rules `var NAME choice ...`) from the trial's own stream of `desc.seed` and the trial index, logged in the data line, replayed by restore and snapshot (format 5), no draw from the session's generator; `ytr_trial_seed()`. G4: `weight COLUMN` times `reps`. G11: `rate=display` (`YTR_RATE_DISPLAY`, `desc.display_rate_num`). G10: 256 columns in a data file. The data line has a `trial_seed` column (`x` and 16 hex digits, text in pandas, R and ysp/table.h). Pins unchanged; 21 of 21 new mutations caught. The Python binding and, from 2026-10-22, the MEX binding take all of it (`seed`, `vars`, `display_rate`, `rate` `'display'`; `var`, `var_index`, `trial_seed`). [Design notes](trials.md#v06-variables-weight-x-reps-the-displays-rate-256-columns).
- v0.5.1: the snapshot magic is `YTRL` (was the psy-era `PSTR`, now refused; no saved data existed).
- v0.5.0: every column of seconds ends in `_s` (the user's decision, 2026-10-20: jsPsych writes `rt` in ms, PsychoPy in s). `ytr_data_open()` refuses, by name, a `TIME_NS` column without `_s` or `_ns` and a session jitter without `_s`; 6 new refusals tested, 4 of 4 new mutations caught. [Design notes](trials.md#v05-columns-of-seconds-end-in-_s).
- v0.4.1: builds under -std=c11 when a system header comes before it (fileno() for YTR_FLUSH_SYNC on glibc).
- v0.4.0 (the data writer, on the [Conventions](conventions.md): `ytr_data_finish()` and a void `ytr_data_close()`, a `TIME_NS` column `x_ns` written `x_s`; every type, missing value and 400 random strings with commas, quotes, line breaks and bad UTF-8 read back by ysp/table.h, 2000 random doubles bit for bit; 25 of 25 mutations caught; a 26-column row costs 4.2 / 1.9 us to format, 15 / 14 us with the write to the OS, 1.7 ms with a sync to disk, gcc / MSVC).
- v0.3.0 (the [Conventions](conventions.md): jitters in `int64_t` ns with `_ns` desc fields, seconds kept in rules, tables and bindings with every pinned draw unchanged; `ytr_open_saved()`, `ytr_close()`, messages from the loop calls).
- v0.2.1. The hashes of 600 v0.1.1 and 320 v0.2.0 sessions match on gcc, MSVC and Linux; jitter distributions checked against their CDFs, the draws integer-only with golden digests that match under every compiler and flag set tried (-Ofast and /fp:fast included), and the snapped exponential's hazard measured; each v0.2 order checked by chi-square against its stated distribution, units and balance uniform over every valid order of small designs; 22.9 million fuzz inputs and 47 of 47 mutations caught; gcc and MSVC, sanitizer clean. Sequential order identical to PsychoPy's TrialHandler and the random orders' block properties matched over 100 seeds ([tests/compare/](../tests/compare/)). open() of a 10,000-trial design 0.02 to 7 ms. [Design notes](trials.md).

### ysp/dataset.h

- Header: [ysp/dataset.h](../include/ysp/dataset.h)
- Prefix: `yds_`, `YDS_`
- Platforms: Windows, Linux, macOS (not run), Emscripten (its in-memory file system)
- Needs: ysp/trials.h (and so ysp/table.h), ysp/json.h

Purpose: A session's data set (experiment_format.md D9 and 2.10, gap G13): a folder with `session.json` (canonical JSON, format "ysp-session" 1, `"closed": false` until the finish, every rewrite atomic: a synced temporary file renamed over), `trials.csv` (the caller's columns), `frames.csv` (flip records), `events.csv` (landings, inputs, responses, outputs), `clock.csv` (clock pairs and fits) and `streams/<name>.csv`, each row in the OS when its write returns, through ysp/trials.h's writer. Inline adapters from `yscr_record`, `yin_event`, `ytl_event` and `yrt_fit_info`. A stored zip of the finished folder for the web. A folder that exists is refused.

Status:

- v0.2.0 (2026-10-25): `yds_abandon()`, an abnormal end that closes the files and leaves `"closed": false` (the bindings' teardown of a data set never closed and their exception paths used to finish it), and `ds.error_code`, the code of the last failure, set by `yds_open()` too. 662 checks on MSVC and MinGW, 656 on WSL2 gcc with ASan and UBSan; 38 of 39 mutants killed (the 6 new ones among them; ds-09 survives as before). emcc not run.
- v0.1.0 (2026-10-21): the first version (gap G13). A synthetic session against golden bytes (session.json at the open and at the finish, four tables, a stream, the zip's digest), a crash in a child process (every file reads, `"closed": false`), the atomic rewrite against a blocked temporary file and a held session.json, refusals before the folder exists: on ysp/trials.h v0.6.0 (`trial_seed` in trials' spelling through `yds_seed_text()`): 561 checks on MSVC and MinGW, 555 on WSL2 gcc with ASan and UBSan, 536 under emcc; the zip opened by Python's zipfile, unzip and Expand-Archive, every CSV by pandas. 32 of 33 mutants killed (one survivor expected: the sync before the rename). On trials v0.6 the handle is 767 KB; the open leaves the pages of unused streams untouched, so its working set grows 340 to 352 KB (was 936 to 948). On the Iris Xe laptop's SSD: 12 us a frame or event row, 8.2 ms an atomic rewrite of session.json. [Design notes](dataset.md).
- Bindings: `ysp-dataset` 0.2.0 (Python) and the MEX `ysp_dataset` write a session's data set from PsychoPy or Psychtoolbox; times in seconds; generic rows (the header takes no `ytr_trials` for trials.csv), checked against `ysp.trials`' and `ysp_trials`' data line; a killed writer, an exception, `abandon` and a teardown of a data set never closed leave `"closed": false` and every row. [Design notes](dataset.md#bindings).

### ysp/table.h

- Header: [ysp/table.h](../include/ysp/table.h)
- Prefix: `ytb_`, `YTB_`
- Platforms: any
- Needs: nothing

Purpose: Typed tables as one position-independent, hashed block: an RFC 4180 CSV parser with hard bounds (32767 rows, 256 columns, 4096-byte fields), errors that name the line, row and column, int32, number and string columns with levels, and a correctly rounded number syntax no locale can change. The pack stores the same block, and a view checks it in place. No heap; file I/O only with `YTB_STDIO`.

Status:

- v0.3.0: 256 columns (was 64), for a trial row with many recorded stimuli; the block format and hash are unchanged; 2 of 2 new mutations caught. [Design notes](table.md#256-columns-v030).
- v0.2.0 (the [Conventions](conventions.md): `ytb_open_csv()`, `ytb_open_csv_file()`, `ytb_open_view()`, `ytb_close()`, negative number reasons).
- v0.1.0. Numbers equal to strtod bit for bit on 10 million strings per compiler; every single-bit flip of a block refused; 5.9 million fuzz inputs; 24 of 25 mutations caught, the other equivalent. Parse 97 to 218 MB/s. [Design notes](table.md).

### ysp/pack.h

- Header: [ysp/pack.h](../include/ysp/pack.h)
- Prefix: `ypak_`, `YPAK_`
- Platforms: Windows, Linux, WebAssembly; macOS not run
- Needs: OS file mapping (`YPAK_NO_FILE`: nothing)

Purpose: Reads a ysp pack (a store-only zip, entries aligned to 4096, a JSON manifest, XXH64 over 1 MiB chunks, the pack ID in the comment): from a mapped file, memory, a range reader, or bytes a browser fetched; lookups by name, zero-copy entries verified on first use, cursors that verify each chunk as it streams; views of the pack's own forms (curve sets, artwork, glyph runs, shaders, textures with QOI). Hostile input is checked before any pointer leaves it. The pack tool (`pack/ysp/pack_tool.h`, the `ypak` CLI; `YSP_BUILD_PACK`) builds, verifies, rebuilds and appends packs; it needs lodepng, stb_image, pl_mpeg and libsamplerate (all fetched by `tools/vendor_pack.py`), and glyph runs also need the layout library (`YSP_BUILD_LAYOUT`, HarfBuzz and Skribidi)

Status:

- Bindings (2026-10-25): `ysp-pack` 0.3.0 (Python) and the MEX `ysp_pack` 1.0.0 open, list, read (a copy), verify, and read the manifest and the definition; every truncation and flipped byte of two test packs refused. Python 16 tests (Windows, WSL2), MEX 60 checks (MATLAB R2023a, Octave 10.1). [Design notes](pack.md#17-bindings-2026-10-25).
- Tool v0.7.0 (2026-10-22; the header unchanged): a definition is stored as its canonical bytes (`"op": "canonical"`, experiment_format.md D2), and its source's SHA-256 in the manifest is the canonical bytes', so a definition respelled (whitespace, key order, number spelling, escapes) gives the same pack; `ypak rebuild` checks the source so. A lookup whose map lacks a level its `by` can take in a leaf that reaches it (table levels, vars, the booleans as far as the leaf's rules make them true, outcome, response, method) is refused by stimulus, key and levels; a key no level reaches is a warning. Corpus pin unchanged. Tests: 346 checks (MSVC), 341 (Linux gcc, Release and ASan with UBSan), 277 (MinGW core tool), 284 in the derivation's test; MSVC and Linux gcc give the same packs; 82 of 82 mutants of `pack/derive.c` (47 new) and 10 of 10 hand mutants of the tool killed; 1,095,940 fuzz inputs, no finding. [Design notes](pack.md#16-tool-v070-the-definitions-canonical-bytes-and-its-lookups-2026-10-22).
- Tool v0.6.0 (2026-10-21; with the header's v0.3.0): the experiment's glyph runs (gap G9). For a version 1 definition the tool lays out every text its text stimuli can show (literals, string-column levels, vars, every lookup value) into `experiment/<name>.ysprun`, keyed by text and style through `ypak_runs_key()` (ysp/pack.h 0.3.0, which the player calls too), so stimuli of one style share a block; a glyph runs resource of that name adds blocks and fallback fonts; a text known only at run time is refused by name. The Stroop example's pack source (`examples/pack/stroop/`) lists no glyph runs. Every corpus entry byte-identical to v0.5.1's. Tests 331 checks (MSVC), 326 (Linux gcc, Release and ASan with UBSan), 168 in the derivation's own test; MSVC and Linux gcc give the same Stroop, union, two-font and shared-style packs; 35 of 35 mutants of `pack/derive.c` and 15 of 15 hand mutants of the tool killed; 362,488 fuzz inputs, no finding. [Design notes](pack.md#15-tool-v060-the-experiments-glyph-runs-gap-g9-2026-10-21).
- Tool v0.5.1 (2026-10-20; the header unchanged): an ffmpeg step with mpeg2video is refused before the encode, because the video index reads MPEG-1 only and misread its output.
- Tool v0.5.0 (2026-10-20; the header unchanged): the ffmpeg step runs on N threads, N from the resource's `threads` or the machine's logical processors, recorded in the manifest and used again by `ypak rebuild`. With N fixed, 936 encodes (libx264, libx265, mpeg1video, mpeg2video; N = 1, 4, 8, 16; 2 and 4 cores of affinity, Windows and WSL2 builds of one ffmpeg) gave one set of bytes per N. 16 threads encode 1080p 3.9 times (x264) and 3.1 times (x265) faster than one. The tool adds `info=0` for libx265 and `-flags +bitexact -dct int -idct simple` for mpeg1video and mpeg2video, which make their bytes the same at every SIMD level and on Windows and Linux (up to 1.7 times the MPEG encode time); libx264 has no such flag and rebuilds the same only on a CPU with the same instruction-set path (AVX2 against AVX measured). Tests 265 checks (MSVC), 260 (Linux gcc, Release and ASan with UBSan); 17 of 17 hand mutants killed; the hand test for the Linux rig passed with WSL2 as the second machine. [Design notes](pack.md#14-tool-v050-the-encodes-thread-count-2026-10-20).
- Tool v0.4.0 (2026-10-13; the header unchanged): phase 3. Video with its index (MPEG-1 everywhere, MP4 on Windows) as given or made by a recorded ffmpeg step that the tool runs on one thread (any machine rebuilds the same bytes; 1.8 times slower on a 640 x 480 clip with x264, 10 times at 1080p), which a rebuild runs again only with the same ffmpeg and otherwise takes from the pack; frame sequences with every frame checked; tables from `.xlsx` (all 23 PsychoPy demo workbooks, 2,305 cells equal to openpyxl's); textures from JPEG (stb_image; 14 of 15 demo photographs, the 15th has a display's ICC profile); sRGB ICC profiles accepted; opt-in resampling (libsamplerate, 140 dB below a test sine). Tests 255 checks (MSVC), 242 under ASan and UBSan on Linux (the same corpus pack ID as MSVC), 89 in the readers' own test; 38 of 38 mutations caught; 922,080 fuzz inputs after the one finding (a JPEG too short for its frame, now refused), none since. Throughput: a workbook 2.2 ms, a photograph 19 ms, a video frame indexed 3.7 ms. [Design notes](pack.md#13-phase-3-what-was-built-tool-v040-2026-10-13).
- v0.3.0 (2026-10-21): `ypak_runs_key()`, the key of a glyph-run block derived from a text stimulus (font, face, size, width, line height, alignment, direction, language and the text; numbers by `yjs_format_double()`), so the pack tool and the player build the same bytes; the header now requires ysp/json.h. 468 checks; 8 of 8 new mutations caught. [The block key](pack.md#the-block-key).
- v0.2.0 (the [Conventions](conventions.md): `ypak_open()` returns `bool`, true with `pk.need_len > 0` for an incremental open; `pk.error_code` and `ypak_strerror()`; concurrent failures write the message under an atomic claim).
- v0.1.0. Every bit flip and truncation of a test pack refused; 29 rule faults refused for their own reason; a virtual 4.3 GB entry and 65,537 entries (zip64); ASan and UBSan clean on Linux; 36 of 36 mutations caught; 28.7 million fuzz inputs, no finding. Open 0.2 ms at 100 entries, 15 ms at 10,000; lookup 0.06 to 0.14 us; verification 3.4 to 3.6 GB/s on a 1 GiB pack; 0 allocations after open. The tool gives the same bytes from MSVC, MinGW, emcc and Linux gcc builds; six zip readers read its packs. Tool v0.2.0 (2026-10-09): a TEXTURE entry stores the encoding `ygfx_texture()` takes, and sRGB codes in RGB need `"primaries"` in the description. Tool v0.3.0 (2026-10-10): the core tool, without the layout library, packs tables, WAV, textures and every other kind but glyph runs to the same bytes as the full tool (MSVC `ypak.exe` 0.45 MB against 1.85 MB). [Design notes](pack.md).

### ysp/json.h

- Header: [ysp/json.h](../include/ysp/json.h)
- Prefix: `yjs_`, `YJS_`
- Platforms: any
- Needs: nothing

Purpose: The strict JSON reader and canonical writer of the repository (RFC 8259 and nothing more: no comments, trailing commas, duplicate keys, invalid UTF-8 or lone surrogates), with a depth and a size limit and errors that give the line and column. Numbers keep their text, with exact int64 and fixed-point accessors (seconds read as nanoseconds) and a correctly rounded, locale-free double; the writer gives each value one spelling, and a double's text is its shortest round-trip decimal, the same bytes on every platform. Objects are sorted once, so lookups are binary searches and the writer needs no sort; values live in the caller's memory or one heap arena. The pack tool and ysp/rigfile.h read through it.

Status:

- v0.2.1 (2026-10-21): builds under gcc -O3 -Werror (MinGW gcc 16.1, CMake's Release); no byte changed.
- v0.2.0 (2026-10-21): canonical numbers, gap G7 of the [experiment format](experiment_format.md). The writer writes each number's exact value in one spelling (`1.50`, `1.5e0` and `15E-1` give `1.5`); `yjs_format_double()` gives a double's shortest round-trip decimal by Ryu with 780-byte tables, integer arithmetic only. 10 million doubles give Python's text, byte for byte, on MSVC, MinGW, WSL2 gcc (also with fused multiply-adds) and emcc under node; 2 million agree with V8. `yjs_new_int()` and `yjs_new_fixed()` bytes unchanged. 3.4 million fuzz inputs with no finding after the one fixed (a respelled exponent past 9 digits; the reader now limits it); 39 of 40 mutants killed, 1 equivalent. 69 ns a double (snprintf `%.17g`: 813 to 1615 ns). [Design notes](json.md#canonical-numbers-v020).
- v0.1.1 (checked against the [Conventions](conventions.md); no code changed).
- v0.1.0. 470 checks pass on MSVC, MinGW and WSL2 gcc (and ASan with UBSan); doubles equal to strtod bit for bit on 200,000 strings; the pack tool on it gives byte-identical packs; 14.2 million fuzz inputs, no finding; 28 of 28 mutations caught. Parse 351 to 604 MB/s, a typical profile in 4 to 5 us. [Design notes](json.md).

### ysp/stair.h

- Header: [ysp/stair.h](../include/ysp/stair.h)
- Prefix: `yst_`, `YST_`
- Platforms: any
- Needs: nothing

Purpose: Adaptive staircases: transformed and weighted up/down, accelerated stochastic approximation. No heap, no threads.

Status:

- v0.3.0 (2026-10-21): `yst_save()` and `yst_open_saved()` ([experiment_format.md](experiment_format.md) gap G12) and `yst_params()` (G6). Sessions cut at up to nine points in four configurations resume to the uninterrupted runs byte for byte; every truncation and every crafted bad field refused; 16 of 16 mutations caught; 747,740 fuzz inputs over the three methods' readers, no finding in stair's. [Design notes](adapt.md#parameter-tables-and-the-staircase-snapshot-stair-v030-quest-v070-aep-v0160).
- v0.2.0 (the [Conventions](conventions.md): `yst_close()`, a failed `yst_update()` leaves a message, C++17-clean USAGE).
- v0.1.2. Hand-derived tracks and a simulated observer in [tests/adapt/](../tests/adapt/); gcc and MSVC, sanitizer clean; replays PsychoPy's StairHandler and Palamedes' PAL_AMUD trial for trial ([tests/compare/](../tests/compare/)). [Design notes](adapt.md), [comparison of the methods](adapt_comparison.md).

### ysp/quest.h

- Header: [ysp/quest.h](../include/ysp/quest.h)
- Prefix: `yqst_`, `YQST_`
- Platforms: any
- Needs: nothing; ysp/rt.h for the async layer

Purpose: QUEST+ on a grid, with QUEST, Psi and Psi-marginal as configurations; built-in psychometric functions, per-cell and batch callbacks for custom models (quick CSF); optional async layer on ysp/rt.h.

Status:

- v0.7.0 (2026-10-21): `yqst_params()` (gap G6), every row checked against the desc field it names and every zero against its default; 7 of 7 mutations caught; the snapshot reader fuzzed, no finding. The snapshot magic is `YQST` (was the psy-era `PSYQ`, now refused). [Design notes](adapt.md#parameter-tables-and-the-staircase-snapshot-stair-v030-quest-v070-aep-v0160).
- v0.6.0 (the [Conventions](conventions.md): `yqst_async_open()`/`_close()`, `yqst_open_saved()`, an `int64_t` wait timeout, messages from the handle-changing calls).
- v0.5.2. Snapshots the size of the posterior, byte-identical between gcc and MSVC; 14770 checks against an independent reference; a Psi-marginal selection in about 1 ms; gcc and MSVC, sanitizer and ThreadSanitizer clean; identical selections to questplus on 180 of 180 trials, to Watson's own QUEST+ notebook on all 17 of its saved runs, to mQUESTPlus on 424 of 424 (ties included) and to Palamedes' PAL_AMPM ([tests/compare/](../tests/compare/)). [Design notes](adapt.md), [comparison of the methods](adapt_comparison.md).

### ysp/aep.h

- Header: [ysp/aep.h](../include/ysp/aep.h)
- Prefix: `yaep_`, `YAEP_`
- Platforms: any
- Needs: nothing; ysp/rt.h for the async layer

Purpose: Gaussian-process adaptive estimation (the AEPsych feature set and Keeley et al.'s psychometric model): Laplace GP with binary, ordinal, categorical, pairwise or continuous outcomes, RBF and semiparametric kernels, a two-latent threshold-and-slope model, integer and categorical dimensions, look-ahead level-set and global acquisitions on a candidate set, priors on the hyperparameters; optional async layer on ysp/rt.h.

Status:

- v0.16.0 (2026-10-21): `yaep_params()` (gap G6), 363 zero-against-default session comparisons over 83 rows; `yaep_open_saved()` refuses a recorded outcome outside the likelihood's, which the new fuzz target found reading outside the cutpoints (747,740 inputs since, no finding); 9 of 9 mutations caught. [Design notes](adapt.md#parameter-tables-and-the-staircase-snapshot-stair-v030-quest-v070-aep-v0160). The snapshot magic is `YAEP` (was the psy-era `PSGP`, now refused).
- v0.15.0 (the [Conventions](conventions.md): `yaep_async_open()`/`_close()`, `yaep_open_saved()`, an `int64_t` wait timeout, messages from the handle-changing calls).
- v0.14.1. Two-latent psychometric model, optimization and pairwise acquisitions, mixed parameters, monotonic projection, conjugate-gradient updates past 512 trials and, opt-in, in the fits; runtime priors; snapshots. Numerics checked against dense references; the Owen et al. 2021 audiometric benchmark reproduced by [aep_audiometric](../examples/aep/audiometric.c) and run beside AEPsych on one response stream, where it matches or beats AEPsych's threshold error with EAVC and BALV and halves its field error ([tests/compare/](../tests/compare/)); gcc and MSVC, sanitizer clean. [Design notes](adapt.md), [comparison of the methods](adapt_comparison.md).

### ysp/timeline.h

- Header: [ysp/timeline.h](../include/ysp/timeline.h)
- Prefix: `ytl_`, `YTL_`
- Platforms: any
- Needs: nothing

Purpose: Events and tracks on the clock: onsets, offsets, value changes, triggers and annotations on a trial, movie or RT time base; float channels driven by keyframes with easing (Bezier included), by sampled trajectories (a 10-minute sum-of-sines target in 288 KB), by repeating tracks (flicker) or by tweens; tweens that write the caller's own fields (a stimulus's x, contrast or color) as GSAP's to(), from() and set() do, with stagger, repeats, yoyo and callbacks; and one call per frame that takes the frame's predicted onset, fills the channel values and reports which events landed on the frame and their residuals. Rewind, late events, a seek that skips what it passes over instead of firing it late, a pure look-ahead of the events due in an RT window (for sound), exact rational base rates (slow motion for a movie base), the frame window as a query, a record of where every event landed. A sequence builder that reads like a GSAP timeline, and the same trial as a table of ops, checked all or none, with a parameter table per op kind for a loader and a designer. No heap, no I/O, no clock.

Status:

- v0.8.1: `ytl_add()` and `ytl_add_n()` refuse a TRIGGER with a negative channel (`YTL_ERR_ARG`, nothing added), as the sequence calls and op tables do, so a fired trigger's channel is 0 to `INT32_MAX` on every path; no caller in the repository added one. 4 of 4 new mutants caught; the whole list (109) run again: 106 caught, the 3 known equivalents survive, also alone on a quiet machine. [Design notes](timeline.md#ytl_add-refuses-a-negative-trigger-channel-v081).
- v0.8.0: a TRIGGER names its trigger channel (`ytl_trigger_ch()`, `YTL_TRIGGER_CH()`; the op's `ch`, reported as the event's `target`), so a player routes each fired trigger to its output role; `ytl_trigger()` and `YTL_TRIGGER()` keep channel 0. Parameter tables in the shared row shape: `ytl_op_params(kind, &n)` per op kind (`YTL_OP_NONE` lists the kinds) and `ytl_desc_params()`. The test checks each row against the field it names, its default against a zero field, and its bounds against `ytl_check_ops()` and `ytl_open()`; 14 of 14 new mutants caught. [Design notes](timeline.md#trigger-channels-and-parameter-tables-v080).
- v0.7.0: a yoyo's return leg is the forward leg played backward (GSAP), by the reverse ease, on channel, sequence and target tweens; a set leaves a `YTL_TARGET_SET` record of the frame that showed it; `ytl_prune()` frees overridden target tweens and sets the horizon (`ytl_horizon()`), inside which a rewind or seek replays bit for bit. The mirror within 1 float ulp of the rule; a 100,000-frame retarget script in a pool of 16 tweens, the same bits as `YTL_OVERWRITE_ALL`; 26 of 26 new mutants caught. The per-field cost is unchanged within run-to-run spread (gcc 6.75 against 6.98 ns, MSVC 9.70 against 8.30 ns, medians of 5 runs); the handle grows to 68 KB.
- v0.6.0: targets, tweens that write the caller's fields (`ytl_tween_to()`, `ytl_animate()`, stagger, overwrite, kill, callbacks), from pools the caller sizes at open; four more eases. A target tween gives the same bits as `ytl_tween()` on a channel (400 random descs), a rewind and a seek replay a trial bit for bit, a random model of the rules agrees; 20 of 20 new mutants caught. About 8 ns per animated field per frame on gcc and MSVC, the same as a channel tween plus a binding copy; 0.5 ms holds about 60,000 fields.
- v0.5.0: time fields the caller writes end in `_ns`, the `YRT_*` unit macros replace `YTL_S`/`YTL_MS`, `ytl_close()`, and every failing call writes `ytl_error()`; 3 of 3 new mutants caught.
- v0.4.0. A test written from the manual alone (skip, peek, rates and sequences by the implementer), with its own model and 32 random model runs; rates against an exact 128-bit reference, sequences against hand-built tracks over 600 random tables; 21 of 24 rate and 19 of 19 sequence mutations of the header caught (3 cannot change a result); gcc, MinGW, MSVC, sanitizer clean; evaluate under 1.6 us p99 on a two-hour movie with 10,000 annotations. [Design notes](timeline.md).
- Bindings: `ysp-timeline` 0.8.1 (Python) and the MEX `ysp_timeline` 1.0.0: sequences, op tables, tracks, channel and target tweens on binding-owned fields, times in seconds with the exact ns in each landing record. A rewind and seeks replay a trial bit for bit; 25 Python tests (Windows, WSL2, ASan) and 44 MEX checks (MATLAB R2023a, Octave 10.1) pass. 1000 tweened values cost 9.5 to 34 us a frame through the binding. [Design notes](timeline.md#bindings).

### ysp/input.h

- Header: [ysp/input.h](../include/ysp/input.h)
- Prefix: `yin_`, `YIN_`
- Platforms: any
- Needs: nothing

Purpose: The input event every device shares: one 48-byte record (time on the ysp_rt clock, kind, device, control, type, value) for keyboards, mice, touch, pens, gamepads, response boxes, eye trackers and sync inputs (scanner pulses, photodiode edges), with a per-event uncertainty and the device clock's raw ticks, the source's timing tier and bounds, and inline adapters from SDL3 events and ysp/screen.h's raw mice. Producers and consumers depend on it alone. No implementation, nothing to link.

Status:

- v0.4.0: `yin_source` bounds are `int64_t` ns (`lo_ns`, `hi_ns`); `unc_us` stays a 16-bit us field, the one exception (an `int64_t` would make the event 56 bytes and the screen's input slot 72, past a cache line).
- v0.3.0 (the SYNC kind, `unc_us`, `ticks`, eye controls; size and offsets unchanged).
- v0.2.0: the raw mouse report moved in. Split out of ysp/response.h; layout and constants pinned by its test; gcc, MinGW, MSVC. [Design notes](input.md).

### ysp/response.h

- Header: [ysp/response.h](../include/ysp/response.h)
- Prefix: `yrsp_`, `YRSP_`
- Platforms: any
- Needs: ysp/input.h

Purpose: The response of a trial from any input device: a device-neutral event (keyboard, mouse, touch, pen, gamepad, response box, eye tracker) with its timing quality; choices by physical key (key names resolve to US-layout positions), keycode, ALL or NONE, and threshold crossings on analog channels (a trigger, pen pressure, movement onset); a window bound to the stimulus's flip onset or a sound's onset, refined when the flip record arrives; minimum RT (anticipations logged, not dropped), deadline, first-response or several responses, key release and hold duration, jsPsych's allow_held_key; a trace buffer; a result with RT in seconds beside the onset's tier and residual and the response stamp's tier. An SDL3 adapter; SDL 3.4's second key reports are dropped by ysp/screen.h's input bridge. No heap, no I/O, no clock. Keyboards and, with ysp/screen.h's raw mice, mice are told apart by device (two participants); a per-participant cursor with a stated gain.

Status:

- v0.7.0: `yrsp_rule_choices()`, `yrsp_rule_arm()` and `yrsp_rule_window()`, inline where ysp/device.h came first: ysp/device.h's response rule takes its choices and its window (`[onset + min_rt, onset + duration)`, plan then final onset, closed after a response) from the collector (gap G8). Codes sent: `yrsp_set_sent()` and `yrsp_rule_sent()`; the result flags `YRSP_R_SENT_MISMATCH` (in `rsp_flags`) when a code sent at the press and the final responses disagree. No rule of the collector changed; 15 of 15 new mutants caught (`sent-01` to `sent-10`; `rule-12` to `rule-15` and `rule-18` in `tests/mutate/device.toml`). [Response triggers](response.md#response-triggers-v070).
- v0.6.0: `yrsp_params()`, the parameter table of the desc and choice fields a definition sets, with enum values (gap G6); a zero `trace_kinds` reads 65535 in the handle. Every row names a field, its default is what the opened collector holds for a zero field, and each enum row lists every value of its C enum; 5 of 5 new mutants caught. [The parameter table](response.md#the-parameter-table-v060).
- v0.5.0: `YRSP_DATA_COLS` names its columns of seconds with `_s` (`rt_s`, `rt_key_duration_s`, `onset_residual_s`, `landing_residual_s`), as ysp/trials.h v0.5.0 requires; 2 of 2 new mutations caught.
- v0.4.0: helpers for the frame loop (`yrsp_update_frame()`: the stimulus frame's flip record as the final onset, then the deadline; `yrsp_feed_screen()`; `yrsp_set_landing()`) and the data file (`YRSP_DATA_COLS`, `yrsp_data_set()`: the onset's source, tier, residual and the landing residual in every row), inline where ysp/screen.h, ysp/timeline.h and ysp/trials.h came first; `result.plan_residual`. `trial_keyboard.c` went from 256 to 184 lines of code without `--sim` and moved no flip in a window or fullscreen; 8 of 8 new mutants caught. [The first trial](response.md#the-first-trial-v040).
- v0.3.0: times in `int64_t` ns (`duration_ns`, `min_rt_ns`, `max_hold_ns`, `rt_ns`, `rt_key_duration_ns` with `YRSP_NO_TIME` for none, `stamp_lo_ns`), `yrsp_open()` and `yrsp_close()`, every failing call writes `yrsp_error()`; 2 of 2 new mutants caught.
- v0.2.0 (a trim: the second-report rule moved to ysp/screen.h v0.4.1's input bridge, so every consumer gets it; the CSV row and `settle` removed).
- v0.1.4: ALL mode excludes SYNC events.
- v0.1.3: the event moved to ysp/input.h. Every rule checked on synthetic streams (20,576 checks, 20,000 random streams); 43 of 43 mutations caught; gcc, MinGW, MSVC. `examples/response/` has ten trials as small experiments (keyboard, same-different, serial RT, adjustment, mouse tracking, audio-keyboard, stop-signal, two keyboards, a 2AFC staircase and QUEST+, Stroop), each with a self-checking `--sim` run; what they show about the header is in the design notes. Not measured: a key pressed by hand, mouse, touch, pen and gamepad timing. [Tutorial, how-to guides and design notes](response.md).
- Bindings: `ysp-response` 0.7.0 (Python) and the MEX `ysp_response`: the collector, the shared input event, an offline rescore and the data columns, every time in seconds with the exact ns beside the absolute times; feeding an event allocates nothing. [Bindings](response.md#bindings).

### ysp/box.h

- Header: [ysp/box.h](../include/ysp/box.h)
- Prefix: `ybox_`, `YBOX_`
- Platforms: Windows, Linux, macOS
- Needs: ysp/input.h

Purpose: Box protocols, pure: bytes in, ysp/input.h events and device-clock pairs out, and the bytes of timer and identity queries and of output codes. Cedrus XID (key frames, StimTracker 2 frames, the `_e5` timer answer; `mp` and `mh` outputs), the ysp line protocol v1 (text lines from a microcontroller board: syncs, edges, samples, query answers, identity, output reports; `o` and `p` outputs), the 6-byte photodiode frame, and the trigger interfaces that take one byte (Brain Products TriggerBox, BioSemi USB trigger interface, Neurospec MMBT-S, the parallel port) and a serial port's DTR and RTS lines. No I/O, no clock, no heap.

Status:

- v0.3.0 (the [Conventions](conventions.md): `ybox_init()` returns 0 or `YBOX_ERR_ARG`, `ybox_encode()` takes `int64_t` ns; no byte changed; 2 mutants added, both caught).
- v0.2.0 (outputs: encoders and capabilities for eight families, each byte pinned to the vendor's documentation, pyxid2 or an open-source driver; StimTracker 2 frames). Decoders checked on generated streams split at every byte, with garbage, broken frames and full outputs, and on random bytes; encoders checked byte for byte; 24 of 24 mutants caught. No byte from or to a vendor device yet. [Design notes](box.md).
- v0.3.1 (2026-10-26): `ybox_decode()` returns after a line board's identity line (`I ...`), as after an `O` report; two identity lines in one call kept only the second before. Identity lines in every generated line stream; 1 mutant added, 27 of 27 caught. [One identity line per call](box.md#one-identity-line-per-call-v031).
- Bindings: `ysp-box` 0.3.1 (Python) and the MEX `ysp_box` decode captured bytes into events (the shared `Event` fields, times in seconds with the exact ns beside them) and clock pairs, and give the query and output bytes; random bytes and bad arguments refused cleanly. The header limit they found (two identity lines in one decode call kept the second only) is fixed in v0.3.1. [Design notes](box.md#bindings).

### ysp/device.h

- Header: [ysp/device.h](../include/ysp/device.h)
- Prefix: `ydev_`, `YDEV_`
- Platforms: Windows, Linux, macOS
- Needs: ysp/rt.h, ysp/input.h, ysp/box.h, ysp/serial.h; ysp/parallel.h for the parallel port

Purpose: A response box, a trigger interface or a microcontroller board on the ysp_rt clock: one instance per device with its own reader thread (or manual polling), a ysp/box.h decoder and encoders, a ysp/rt.h clock fit fed by timer queries (bracketed pairs), match keys that find the device again after a replug, identify, LOST and reconnect with the fit restarted and the gap logged, records in a ysp/rt.h ring. Outputs: `ydev_out_set`, `_pulse`, `_mark`, each write a record with the times before and after it; pulses timed by the device (XID, line boards, the fixed 8 ms of BioSemi and MMBT-S) or by the instance's own worker; the callback for a ysp/screen.h trigger channel; response rules, which write a response's code on the input's reader thread; roles by index. The sink is the caller's: ysp/screen.h's input bridge in the example.

Status:

- Bindings: `ysp-device` 0.5.0 (Python) and the MEX `ysp_device` 1.0.0: devices, events on the ysp_rt clock (a C ring the reader threads fill without the GIL), outputs, response rules, and simulated boards for tests; every time in seconds. 21 Python tests pass on Windows and WSL2, `test_mex_device.m` (59 checks, with `ysp_rigfile`) in MATLAB R2023a and Octave 10.1; no hardware. [Bindings](device.md#bindings).
- v0.5.0: response rules (`ydev_rule`, `desc.rule`, `YDEV_OUT_RULE`): a response's code written on the input's reader thread, not a frame later from the frame loop (gap G8). On an in-process box and output, Windows 11, i7-1360P, under the timing guard: press stamp to write start p50 5.2 us, max 38.7 us through the rule against p50 8.7 ms, max 16.7 ms through a 60 Hz frame loop (600 presses). `ydev_rule_fires()` gives each fire of an arm, for ysp/response.h's codes-sent check. TSan clean on WSL2 with 4 producer threads against arms, window moves and the frame loop's own pulses; 17 of 17 new mutants caught. [Response rules](device.md#response-rules-v050).
- v0.4.0: `ydev_params(family, &n)`, a parameter table per family of the desc fields it reads, with that family's defaults, for a role's keys and the rig profile (gap G6); `ydev_open()` writes the family's baud into the handle for a zero `desc.baud`. Each family's rows follow its `ybox_caps()`, and each default is the opened handle's; 7 of 7 new mutants caught. [Parameter tables](device.md#parameter-tables-v040).
- v0.3.1: a host-timed pulse's width runs from the start of the leading write, at least the new `desc.min_hold_ns` (0.25 ms; none on the parallel port) after its return (`yser_pulse_off_at_ns()`, ysp/serial.h 0.6.1). On a Teensy 4.0 echo board over `usbser.sys` (TriggerBox byte path, DTR, RTS; 1 and 5 ms; 0 of 2400 missed): width error -65 to -16 us median, against +350 to +401 us with v0.3.0. 7 mutants added (6 new, 1 moved), all caught. No vendor box, parallel port or FTDI bridge. [Host-timed width](device.md#host-timed-width-v031), [results](device.md#host-timed-pulses-on-a-teensy-40-v031).
- v0.3.0 (the [Conventions](conventions.md): `ydev_open()`/`ydev_close()`, `ydev_wait_running()` before the first output, key and role copied into the handle, `int64_t` ns timeouts; 3 mutants added, all caught).
- v0.2.1 (builds with YRT_NO_THREADS again; host-timed pulses end in ydev_poll() there).
- v0.2.0 (outputs, the trigger channel, roles). On a fake transport, Windows, MSVC, under the timing guard: a 2 ms host-timed pulse +0.7 to +1.3 us wide at p50, +3.3 to +13.4 us at p99, max +16 to +454 us (3 runs of 300); a pulse call 5.3 to 8.7 us at p50. The serial transport's outputs over a socat pty pair (20 of 20 reported back). The loopback tool on a simulated adapter and board agrees with the model's truth within the input's own uncertainty.
- v0.1.0: simulated inputs within +5 to +15 us of truth (line board) and -1.0 to +0.9 ms (XID behind a 16 ms timer). TSan, ASan and UBSan clean; 24 of 25 mutants caught, 1 equivalent. No real device opened yet. [Design notes](device.md).

### ysp/rigfile.h

- Header: [ysp/rigfile.h](../include/ysp/rigfile.h)
- Prefix: `yrig_`, `YRIG_`
- Platforms: Windows, Linux; macOS not run
- Needs: ysp/device.h, ysp/json.h

Purpose: The rig profile ("ysp-rig 1", docs/devices_spec.md 11.2): which device plays each role and what the rig's loopbacks measured, in the user's config folder (refused when other users can write it). Load with errors that name the field, a SHA-256 check of each loopback and calibration file it names (a missing or changed file gives that role tier UNKNOWN, with a note), canonical bytes and their hash for the data file header, a role's desc and start on ysp/device.h, a binding (also of a Lab Streaming Layer stream, family `lsl`, for ysp/net.h), a stored loopback result, and the rig's viewing geometry for sizes in degrees.

Status:

- Bindings: `ysp-rigfile` 0.5.0 (Python) and the MEX `ysp_rigfile` 1.0.0: load, checks, notes, hash, roles, the tier rule, view, bind and save; `Profile.desc(role)` opens the role with `ysp.device.Device`. 10 Python tests (the planted-folder refusal on WSL2 only). [Bindings](device.md#bindings).
- v0.5.0 (2026-10-21): `view` (`distance_mm`, `width_mm`, `width_px`) and `yrig_ppd()`, the pixels per degree by ysp/gfx.h's YGFX_DEG expression (gap G2 of experiment_format.md); a profile without `view` keeps its bytes and hash; `rigfile_profile --view` writes it; trial_2afc_adaptive reads it. 552 checks (51 new); 8 of 8 new mutants killed. Arithmetic and files only. [Decisions](device.md#the-viewing-geometry-ysprigfileh-v050-2026-10-21).
- v0.4.0: `display.latency`, the photodiode run behind the onset offset, checked at load; `yrig_read_loopback()` reads the new input bounds and display onset files, and a bounds file is now compared with the profile (DIFFERS). Two tools: `rigfile_bounds` (an input's bounds against a reference line board, each difference widened by its bracket; in 160 of 160 simulated runs over seeds and USB tails the bounds contained the truth) and `rigfile_onset` (the offset from a photodiode run's CSV). 501 checks; 37 of 37 mutations caught (8 new). Run on simulated boards and a generated CSV only. [The tools](device.md#the-rig-profile-tools-ysprigfileh-v040-2026-10-13).
- v0.3.0 (the [Conventions](conventions.md): `yrig_open_device()` replaces `yrig_start()`, its message in `ydev_error()`).
- v0.2.0. 403 checks pass on MSVC and MinGW, 410 on WSL2 gcc with the planted-folder checks; canonical bytes pinned; 66 refusals by field, 7 more for `lsl` roles; binding through a fake transport; 28 of 28 mutations caught. A 4-role profile read and checked in 18 to 23 us. [Design notes](device.md#how-to-keep-a-rigs-devices-and-latencies-in-its-rig-profile).

### ysp/net.h

- Header: [ysp/net.h](../include/ysp/net.h)
- Prefix: `ynet_`, `YNET_`
- Platforms: Windows, Linux; macOS not run
- Needs: ysp/rt.h, ysp/input.h; liblsl at run time

Purpose: Lab Streaming Layer on the ysp_rt clock, with liblsl (MIT) loaded at run time: marker outlets (string or int32, stamped now or with any ysp_rt time, and a ysp/screen.h trigger channel that sends one at the flip), numeric outlets, and inlets that find a stream by its match key, pull it on their own thread, map the sender's stamps to ysp_rt (a fit of the LSL clock and a fit of liblsl's time corrections), and give markers and threshold crossings as ysp/input.h events and every numeric sample to a stream ring with a newest-sample slot; LOST, reconnect and gaps in the log, as ysp/device.h. The LabStreamer binds as three roles (docs/net.md).

Status:

- v0.3.0: `ynet_stats.lsl_offset_ns` and `lsl_rtt_ns` (`int64_t`, rounded to the nearest ns at the liblsl call) replace `lsl_offset_s` and `lsl_rtt_s`; the conventions' open exception for them is closed.
- v0.2.0 (the [Conventions](conventions.md): `ynet_lsl_open()`/`_close()`/`_error()`, outlet and inlet open/close, `ynet_inlet_wait_running()`, key and role copied, `int64_t` ns timeouts, liblsl's own values named `lsl_`; 2 mutants added, both caught).
- v0.1.0. 340 checks on MSVC and MinGW against a fake liblsl, also under ASan, UBSan and ThreadSanitizer on WSL2; 16 of 16 mutations caught (one control). Against the real liblsl 1.17.5 on WSL2: marker call to sink p50 140 to 194 us, p99 0.4 to 0.8 ms; mapped time within -31 to +52 us of the push; 10 kHz x 4 channels with no drops and no gaps at 44 to 94 ns of reader time a sample. The same on Windows with liblsl 1.17.7 (`tools/vendor_lsl.py`, hash-checked; CI runs the loopback on Windows and Linux): marker p50 89 to 121 us, mapping within -39 to +68 us, 16 to 51 ns a sample. No LabStreamer was attached. [Design notes](net.md).

### ysp/color.h

- Header: [ysp/color.h](../include/ysp/color.h)
- Prefix: `ycol_`, `YCOL_`
- Platforms: any
- Needs: nothing

Purpose: Color on a calibrated display: the display calibration (photometer readings and spectra in, a lookup table and the matrices into CIE 1931 XYZ and Stockman-Sharpe 2 or 10 degree cone space out, the canonical .yspcal file), a conversion context, values tagged with their space, and conversions both ways between linear device RGB, device codes, XYZ, xyY, CIELAB, CIELUV, Oklab, OkLCh, sRGB, Display P3, Rec. 2020, LMS, cone contrast, DKL (spherical and Cartesian) and MacLeod-Boynton. A conversion the calibration cannot support is refused with the reason; a color outside the gamut comes back unclipped with its distance. Gamut questions (the largest contrast along a direction, the boundary in a plane), gamut mapping by a stated method, a Michelson contrast in light as ysp/gfx.h's contrast and as the output codes show it, and a participant's luminance from flicker photometry for the DKL isoluminant plane. No heap, no I/O.

Status:

- v0.4.0 (2026-10-21): `ycol_michelson()`: a Michelson contrast in light as ysp/gfx.h's contrast along a direction, the largest one in gamut, the peak's and trough's output codes and the contrast their light shows; past the gamut, reported, not clipped; luminance from the readings alone (gap G5 of experiment_format.md). Against the 2AFC example's own arithmetic, 0 of 2406 codes differ and the values agree within 1.1e-16; 10 of 10 new mutants killed. trial_2afc_adaptive, the Python binding (`Context.michelson()`) and the MEX binding (`ysp_color('michelson', ...)`, 2026-10-22) use it. [Design notes](color.md#michelson-contrast-in-light-v040).
- v0.3.0 (2026-10-21): `ycol_param.values`, after `offset` (the shared row shape of gap G6): `cones` and `adapt` list their C enums' names and values. The test checks each row against its desc field and its enum, and each default against a zero desc; 2 of 2 new mutants killed. No conversion changed. [Design notes](color.md#enum-values-in-the-parameter-table-v030).
- v0.2.0 (the [Conventions](conventions.md): `ycol_ctx_open()`/`_close()`, the message in `ycol_ctx_error()`; 2 mutants added).
- v0.1.0. The calibration is ysp/gfx.h v0.4's, bit for bit (52 calibrations, 2100 raw calls, PAINT's and VIDEO's matrices); conversions within 1e-12 of colour-science 0.4.7 (OkLCh hue of near-grays 3.3e-10); round trips through every space within 1.4e-13; Psychtoolbox's ComputeDKL_M within 6.2e-16, LMSToMacBoyn 1.2e-7, SensorToPrimary 1.5e-8 with the same out-of-gamut flags, MaximizeGamutContrast 7.3e-16 (MATLAB R2023a); 32 of 32 mutations caught; MSVC, MinGW, gcc with ASan and UBSan. A DKL direction 22 to 27 ns a call (MSVC), an OkLCh gamut ring of 360 hues 0.8 to 1.0 ms. No number is a measurement of light. [Design notes](color.md).

### ysp/rdk.h

- Header: [ysp/rdk.h](../include/ysp/rdk.h)
- Prefix: `yrdk_`, `YRDK_`
- Platforms: any
- Needs: nothing

Purpose: Random dot kinematograms: coherence, direction, speed, density, dot lifetime, the noise rule (random position, random walk, random direction: Scase, Braddick and Raymond 1996) and the signal rule (same, different, least recent), the presets of Pilly and Seitz 2009 (white noise, Movshon/Newsome with 3 interleaved sets, limited lifetime, Brownian); circular or rectangular apertures with a wrap that keeps the density uniform (a dot moves modulo its own chord); motion by the predicted onset of each frame or by a fixed step per frame; transparent motion as two fields. The state is integers and every draw is Philox4x32-10 of (seed, stream, dot, update, slot), so a trial regenerates bit for bit on any platform and the noise is frozen across coherence levels. Output into ysp/gfx.h's dot buffers or straight into its instance records (gabor arrays). No I/O; one allocation at open, or none.

Status:

- v0.3.0 (2026-10-21): `yrdk_param.values`, after `offset`, NULL in each row (the shared row shape of gap G6); 1 of 1 new mutant killed. The rules and the digests do not change; the Python binding is 0.3.0. [Design notes](rdk.md#the-shared-row-shape-v030).
- Bindings: `ysp-rdk` 0.2.0.1 (Python) takes and returns every time in seconds, with the exact ns beside the seconds in `Field.last`; exact to the ns below 2^23 s (about 97 days); the golden digests and the C-logged replay unchanged. The MEX `ysp_rdk` (2026-10-25) has the same surface and replays the golden digests and the C-logged trial bit for bit in MATLAB R2023a and Octave 10.1. [Design notes](rdk.md#bindings).
- v0.2.0: `yrdk_open()` returns bool, `desc.lifetime_ns` (was float seconds), `yrdk_out.age_s` (float seconds, a GPU attribute), every failing call writes `yrdk_error()`; the golden digests and the Python binding unchanged; 2 of 2 new mutants caught.
- v0.1.0. Statistical tests: the realized coherence from the dots' displacements, density by chi-square in equal-area cells and along the edge (36 configurations), lifetimes, displacement against speed x elapsed time with dropped frames, the noise rules' distributions, frozen noise; 8 golden digests reproduced by an independent Python model and by MSVC, MinGW, gcc (-Ofast too, ASan, UBSan); 26 of 26 non-equivalent mutations caught. 10,000 dots: 102 to 443 us of CPU a frame by rule; with ysp/gfx.h 0.3 ms of CPU and 0.32 to 0.38 ms of GPU. The Python binding `ysp.rdk` replays a C-logged trial into equal NumPy arrays bit for bit. [Design notes](rdk.md).

### ysp/outline.h

- Header: [ysp/outline.h](../include/ysp/outline.h)
- Prefix: `yol_`, `YOL_`
- Platforms: any
- Needs: nothing

Purpose: The outline builder, the CPU side of text and artwork: paths of quadratic Bezier curves from C calls (move, line, quadratic, cubic, SVG arcs, rectangles, ellipses, transforms, nonzero and even-odd), glyph outlines from a bounds-checked TrueType and CFF (CID-keyed included) reader for fonts from untrusted packs, an SVG subset reader (paths, basic shapes, transforms, solid fills and strokes; everything else refused by name), overlap removal (winding 0 or 1, every vertex shared), strokes expanded to fills with Nehab's evolute elements so curves tighter than the half width do not fold (round, miter, bevel; butt, round, square; faux bold and inset), exact box-filter alpha per pixel in double, and curve sets for ysp/gfx.h's Slug runs (format v1, the RESOLVED flag). No heap after warm-up, or a fixed arena; no I/O.

Status:

- v0.4.1 (2026-10-22): builds under gcc 11.4 -O1 `-Werror` (Ubuntu 22.04): a `-Wmaybe-uninitialized` false positive on a CFF DICT operand; the number reader now writes it on every path. gcc 16.1 and clang 11 did not warn. No output changed. [Design notes](outline.md#v041-gcc-11-at--o1).
- v0.4.0 (2026-10-21): `yol_param.values`, after `offset` (the shared row shape of gap G6): `join`, `cap`, `mode` and `format` list their C enums. Every default that is not 0 gives the same stroke or glyph outline, byte for byte, as a zero field; 3 of 3 new mutants killed. No output changed. [Design notes](outline.md#v040-enum-values-in-the-parameter-tables).
- v0.3.2: fixed on Linux: the header sets `_DEFAULT_SOURCE` (unless a feature macro is set) before its system headers, so ysp/gfx.h's TEXT, which wants it first, does not leave ysp/rt.h without `clock_gettime()` under `-std=c11` (gcc 11.4 and clang 11 did not compile `test_gfx`).
- v0.3.1: fixed `yol_resolve()` refusing ("the boundary does not close") strokes that cross an edge a few float ulps from horizontal; found by `test_gfx` under Ubuntu 22.04's gcc 11.4 -O3, which drops the caller's `(float)` casts before the inlined `yol_line()`. The resolve now resolves 100 of 100 such grids (v0.3.0: 8) within 2.8e-16 of the reference area, on every compiler; 3 mutants added, caught.
- v0.3.0 (the [Conventions](conventions.md): `yol_open()`/`yol_close()`, `yol_close_path()`, `yol_font_view()`, `yol_cset_open()`/`_close()`; every failure on a context leaves a message; 2 mutants added).
- v0.2.1. TrueType glyphs placed as HarfBuzz places them: every glyph of Arial and Times New Roman equal to HarfBuzz 14.6.0's outline, and the curve sets equal word for word. Curve sets equal the format's test vector and pass ysp/gfx.h's own check and winding; resolve within 2e-13 and exact alpha within 1.8e-13 of an adaptive Gauss-Kronrod reference; strokes 0 wrong of 1254027 pixel centers on 400 random curves (half bands alone: 186 wrong on 30); 30000 mutated fonts and 20000 mutated SVG files without a crash, also under ASan and UBSan; every glyph of seven Windows fonts builds; 64 of 67 mutants killed, 3 equivalent. A whole Microsoft YaHei into a set 1.7 to 1.9 s; exact alpha for 3000 CJK glyphs at 32 px 0.37 to 0.40 s; a CJK stroke 0.73 to 0.80 ms. Missed: a Latin stroke 289 to 303 us against a 200 us bar, and a CFF CJK set 94 to 103 us a glyph against 66. [Design notes](outline.md).

### ysp/screen.h

- Header: [ysp/screen.h](../include/ysp/screen.h)
- Prefix: `yscr_`, `YSCR_`
- Platforms: Windows 10 and 11 (DXGI flip through ANGLE), Windows 11 x64 (the composition swapchain, present at a target time); browsers through Emscripten (WebGL2, requestAnimationFrame; the lowest tier); Linux and macOS: the simulated display only
- Needs: ysp/rt.h, ysp/input.h, SDL3; ANGLE at run time on Windows; SDL3's Emscripten port and ASYNCIFY or JSPI in a browser

Purpose: The stimulus display: window, GL ES 3.0 context, display modes and the swap path, with flip at a time. Each frame gets the predicted onset of its flip for the timeline, presents for a target time on the ysp/rt.h clock, and gets a record of the vblank it was shown on, the drops, the composition path and where its time went, in ysp/rt.h's event ring. Also the mode whose refresh is a multiple of a video's rate, a photodiode patch, input restamping and several displays.

Status:

- v0.9.0 (2026-10-21): `yscr_param.values`, the last field (gap G6): `backend` lists every `yscr_backend` (the stubs too; its range could not show that 3 to 6 exist), `patch.corner` and `settle` their `YSCR_` macros. `key_dedup_ns`'s default is 5e7, what a zero field means (was 0). Every default checked against a SIM screen opened with a zero desc, or against open()'s constant; 6 of 6 new mutants killed (2 of them by tests/adapt/param_tables_test.c, the cross-header check). [Design notes](screen.md#enum-values-in-the-parameter-table-v090).
- v0.8.0: BACKEND_SIM on ysp/rt.h's virtual clock: no frame drops and the same records every run; `clock=virtual` in the describe line; a real backend refused there; a record's phases are costs on the real clock. The `--sim` checks widened for loaded CI runners are exact again, `trial_mouse_tracking --raw-mice` included. 2 of 2 new mutants caught ([SIM on a virtual clock](screen.md#sim-on-a-virtual-clock-v080)).
- v0.7.2: on the web backend `yscr_open()` and `yscr_close()` no longer leave a fullscreen the page entered, and a page shown again keeps it: SDL's port called `document.exitFullscreen()` when it made, destroyed or showed the window. `av.html` lost fullscreen 2.2 to 3.7 s into each run before (Chrome 154 and Firefox 157, isolated and not); after, every demo kept it until the page left it at the end. The pages now ask again on the next key or tap after a browser's own exit ([web.md](web.md#fullscreen)). JavaScript only: no native test or mutant.
- v0.7.1: on the web backend `yscr_event_input()` stamps key, mouse, touch and pen presses and releases with the DOM event's `timeStamp` where it finds it (`dom_stamps=` in the describe line), and the describe line adds `dpr=`. Checked with scripted keys and taps in Chrome 154 (headless, and Pixel 7 emulation); no physical key or touch measured. The matching is JavaScript: no native test or mutant.
- v0.7.0: `YSCR_BACKEND_WEB` ([web.md](web.md)): under Emscripten, SDL3's canvas and the header's WebGL2 context, frames paced by awaited `requestAnimationFrame` callbacks (ASYNCIFY or JSPI), records from the callback times as estimates (`YSCR_TIER_WEB`, `YSCR_PATH_BROWSER`), the period measured at open, settling that warns and never fails, `YSCR_EV_VISIBILITY` records. Run in Chrome 154, Edge 155 and Firefox 154 to 157; no light measured. 4 mutants added.
- v0.6.0 (the [Conventions](conventions.md): every failing call on a screen writes `yscr_error()`; `desc.panic_window_ns`, `panic_grace_ns` and `trigger_spin_ns` int64 ns; 2 mutants added).
- v0.5.2: `desc.on_context`, a hook that `yscr_open()` calls once when the GL context exists, before the warm-up and settling, so work on other threads overlaps them (ysp/gfx.h's `ygfx_open_start()`); its time is added to the settle cap and shown in the describe line. On the Iris Xe with a cold program cache, gfx was ready 2.57 s after the `yscr_open()` call in a window (4.79 to 4.83 s before) and 2.46 to 2.47 s fullscreen (2.87 to 2.91 s) on AC, 3.41 s (5.37 to 5.40 s) and 3.46 to 3.47 s (3.85 to 3.89 s) on battery; settling took the same time and presents with and without the compiles, and all 260 opens settled. 7 of 7 new mutations caught.
- v0.5.1: `desc.foreground`: AUTO forces the raise fullscreen (SDL's force hint) and honors the OS policy in a window, or HONOR or FORCE; the describe line, the settle record and `yscr_settle_check()` say granted, forced or refused. Started from the background, the OS refused 20 of 21 opens: settling passed, but the keys went to another program; forced, 25 of 25 got it, and close() gave it back to the window that had it in 15 of 15 (without that, a third program got it in 9 of 9). Fixed: on the composition swapchain the first frame after 67 ms or more without a present showed a vblank late (72 of 72; DXGI flip 0 of 54); a thread now waits on the display's vblank event: 0 of 36. With it and the next fix, the first frame after a 2.2 s program compile dropped in 0 of 8 runs (21 of 22 before). Fixed: `yscr_open()` returned with the just-presented back buffer bound, so `ygfx_prime()` before the first frame hung (gfx_hello, gfx_trial). 7 of 7 new mutations caught.
- v0.5.0: `yscr_open()` settles the display, as Psychtoolbox's sync tests do: black frames until an OS vblank time, 6 clean flips in a row on one path and depth, the last 20 vblank intervals within 50 us SD and 10 us of the mode's period, and 2.5 s in a window; capped at 3 s (4 s in a window). At the cap a fullscreen open fails and names the condition, a window opens with a warning; `desc.settle` and the `YSP_SETTLE` variable change it, and every outcome is in the describe line, a ring record and `yscr_settle_check()`. On the Iris Xe, 100 opens with the window in front: open took 0.56 to 0.67 s fullscreen (median; at most 0.85 s) and 2.5 s in a window, and no early, late, estimated or off-grid record and no path or depth change came after it (v0.4.4: 96 bad records in 20 DXGI flip window opens, 8 to 12 path and depth changes per composition-swapchain window open). A fullscreen window covered at open (a program started from the background) now fails the open. Fixed: a stale second statistic flapped the path and depth of composition-swapchain windows. 20 of 20 new mutations caught.
- v0.4.4: the depth (vblanks from a present to its flip) comes from the path, never from misses: measured at open, the new path's at each path change, lowered on DXGI flip by a flip that showed early; `desc.depth` pins it, and `desc.depth_learn` keeps the old miss-driven rule as an opt-in (with steady GPU work near a period it dropped 1 to 6 frames in 600 where the default dropped 3 to 57). Each change is a ring record and counted in the describe line. On the v0.4.3 stress runs no frame showed early after heavy frames (3 per run before). AUTO now picks the composition swapchain where it opens with independent flip, else DXGI flip: in 10-minute runs it matched DXGI flip at p99 (1.92 against 1.88 us fullscreen, 1.82 against 1.78 us in a window) and in drops, with no early flip; its present call costs twice as much. Fixed: a window that got no OS time at open kept a guessed grid (every record tier 3, a false sync-guard alarm). 20 of 20 new mutations caught.
- v0.4.3: fixed held frames shown a vblank early on DXGI flip (26 to 72 of 1200 held frames in fullscreen, idle, in every version since v0.2.0; about 145 more flagged EARLY that were on time). A scanout probe (D3DKMTGetScanLine) showed the vblanks steady while DXGI's count lost about 1 vblank in 25 and 15% of its times were up to half a period off; the grid followed those times and woke the next hold early. Now a flip's vblank is the grid's nearest to the OS's time and only times on the grid move it: 0 early in 1200 held frames, 3 of 3600 under load, misses and holds (from the depth rule). 9 of 9 new mutations caught.
- v0.4.2: a sync guard for a driver setting that forces vsync off: when 32 of 64 flips came two in one refresh, flipped with no vblank wait, or tore, or presents outran the refreshes, the screen is untimed (tier 3, `YSCR_FLIP_UNSYNCED`, a ring record, and `yscr_sync_check()` with a message that names the AMD, NVIDIA, Intel and Mesa settings). A scripted forced-off driver fires it four ways; legitimate paths give no evidence in the core test, and on the Iris Xe at most 3 of 64 flips under load, misses and held frames. Whether a driver panel overrides `Present(1)` is not measured (a hand test). `screen_sync_check`, PTB's PerceptualVBLSyncTest. Fixed: a second key report's key-up was taken by another keyboard's held key. 16 of 16 new mutations caught.
- v0.4.1: `yscr_event_input()` drops SDL 3.4's second key reports (a key-down within 50 ms of the last of that key, and its key-up), counted and logged, for every consumer of the bridge (was ysp/response.h's rule only); `desc.key_dedup_ns` sets the window or turns it off; 15 of 15 new mutations caught.
- v0.4.0: an input bridge: any thread pushes ysp/input.h events, each announced by an SDL event, so one `yscr_poll()` loop sees keys, mice, raw mice, boxes and eye trackers in arrival order, and `yscr_event_input()` decodes any input event; requires ysp/input.h.
- v0.3.5: `desc.raw_mice` (opt-in, Windows) reads every mouse's Raw Input on a thread: per-device clicks, wheel and unaccelerated movement, stamped when read; a guard renews the registration after SDL's relative mode (per-device separation verified with injected input only).
- v0.3.4: input devices (keyboards, mice, touch, gamepads, pens) logged to the ring with id and name at open and on hot-plug.
- v0.3.3: `desc.gamepads` (opt-in) starts SDL's gamepad subsystem and opens each gamepad; `caps.raw_keyboard` says whether keys come on the raw path. Measured: SDL 3.4's second key report comes only for keys without a scan code (virtual-key injection); scan-code injection gave one report per press, so reports of one abort press now merge within 50 ms (was 30).
- v0.3.2: `yscr_text_input()` turns SDL text input on for a text box and off again, with a ring record per change and a flag on every flip planned meanwhile, because keys then leave the raw timed path; off at open, as before.
- v0.3.1: Shift+Esc (not Esc alone), a close request, Alt+F4 or a request from any thread makes begin() report an abort once; the caller decides how to stop. An opt-in panic watchdog: Shift+Esc 3 times while the frame loop reports nothing puts back the gamma ramp and ends a hung program (28 to 85 ms after the third press here). A window icon (Escher's impossible cube). A D3D11 device with video support and multithread protection for ysp/video.h, at no measured cost per frame.
- v0.3.0. Flip hooks: exact device codes in the frame (VPixx Pixel Mode and pixel sync presets, self-tested and read back), triggers at the planned onset on a deadline worker that move when a flip will be late, and an after-flip callback. Software timestamps only, on one Windows 11 laptop at 60 Hz (Iris Xe, Electron's ANGLE 2.1.23876). Fullscreen on a quiet machine, 1 minute each: onset within 2.33 us of the prediction at p99 on DXGI flip and 2.36 us on the composition swapchain, no drop, every frame on the first vblank after the present call (15.96 and 15.63 ms p50 to scanout start). In a window kept on top, the composition swapchain stayed on independent flip while DXGI flip moved between the overlay and the composed path: 2.1 to 2.5 us p99 against 4.5 to 5.4 us, and 0 to 24 drops per minute against 27 to 67 (runs made before the v0.2 depth rule and early-flip fix). Every injected overrun flagged. Each flip carries a timing tier; a composed onset on the composition swapchain is the compositor's plan (tier 3). No photodiode has run the loopback test. A core test with a scripted swap path on a virtual clock (no SDL, no display) on MSVC, MinGW, gcc and clang, sanitizer clean, 43 mutations of the header caught. [Design notes](screen.md).

### ysp/audio.h

- Header: [ysp/audio.h](../include/ysp/audio.h)
- Prefix: `yau_`, `YAU_`
- Platforms: Windows (WASAPI shared; exclusive runs but this laptop's driver buffers about 100 ms); Web Audio in Chrome and Firefox (AudioWorklet and ScriptProcessorNode, the browser's estimates only); CoreAudio, ALSA and PulseAudio compiled through miniaudio, never run
- Needs: ysp/rt.h, miniaudio 0.11.25

Purpose: Sound at a time, and its capture, on the ysp/rt.h clock: `yau_play_at(buf, t)` plans the first sample on the device frame t falls on, through a fit of the device clock against the ysp_rt clock, and records where it landed (stream frame, its time from that fit, residual, tier) in the handle and in the event ring. The time is the fit's, confirmed by position reports, never observed: tier 2 at best. A strict open that refuses any rate, channel or format conversion; cancel, stop and gain at a time; loops, routing, clipping flags; tones, noise and clicks at the project rate; the OS's latency claim, kept out of every onset; the Audio device and Audio source extension interfaces. Never resamples, never inserts frames. v0.5.0: `yau_capture`, an input whose every frame has a capture frame number and a time from a fit of the input's own clock, alone or as one duplex device with the output; a lock-free ring to one or two readers; drops and device losses as gaps that keep the frame numbers; a voice key and a click finder; a WAV writer.

Status:

- v0.7.0 (2026-10-21): `yau_param.values`, the last field (gap G6): `format.sample` and `backend` list their C enums. `format.channels` is `u16` (the field's type; was `u32`) and `stream.preroll`'s default is 12000 frames, a quarter of the default ring (was 0). Every default checked against a device opened with a zero desc; 4 of 4 new mutants killed. [Design notes](audio.md#enum-values-in-the-parameter-table-v070).
- v0.6.1: one AudioContext a page on the AudioWorklet path too: the context made in the Start tap, with one worklet scope and one `AudioWorkletNode`, is resumed by every later open at the same rate and channel count. The 3 contexts an open before were the device's (Emscripten's `emscripten_create_audio_context()`) and 2 of miniaudio's sample-rate queries; the queries now get the device's context on both paths. `av.html` with a reopen after setup on both builds, Chrome 155 and Firefox 158 headless on this laptop, 30 opens a cell: 0 silent, contexts made a page 6 to 1 (AudioWorklet) and 1 to 1 (ScriptProcessorNode), sound after the reopen in 120 of 120, the describe line's `context=reused` in all; Firefox's reopen had no transient user activation in 60 of 60. Sound minus plan unchanged (headed Firefox AudioWorklet, 30 opens each: p5 to p95 -0.79 to +0.54 ms before, -0.75 to +0.54 after). Not run in Safari or on a phone: docs/web.md has the iPhone and iPad hand test. Browser-only code: no mutant. [One context on the AudioWorklet path](audio.md#one-context-on-the-audioworklet-path-v061).
- v0.7.1 (2026-10-26): the implementation includes `<objbase.h>` before miniaudio.h on Windows, so ysp/audio.h before ysp/screen.h in one translation unit builds under MSVC `/W4 /WX` (C4005 on `CLSCTX_ALL` before). A compile check builds the audio, screen and video implementations in one unit in both orders. [Windows headers in one translation unit](audio.md#windows-headers-in-one-translation-unit-v071).
- v0.6.0: on ysp/rt.h's virtual clock `BACKEND_NULL` is the header's own device, its blocks on a virtual timer: the fit exact, every onset of the `--sim` examples on its plan within half a sample (was 1 to 10 ms allowed); a real backend refused there. 8 of 8 new mutants caught ([The null device on a virtual clock](audio.md#the-null-device-on-a-virtual-clock-v060)).
- v0.5.2: the web reader keeps the lower envelope of `getOutputTimestamp()` over every read, not over the pairs the blocks saw. On the ScriptProcessorNode path a block sees one pair, read at a phase locked to the browser's audio callback; in Firefox 157 those stayed above the envelope for up to a second, the fit did not warm up in 3 s, and `yau_open()` failed. `av.html` opens with no sound, v0.5.1 against v0.5.2, on this laptop: Firefox ScriptProcessorNode 8 of 30 against 0 of 30 headless, 3 of 15 against 0 of 15 headed; Firefox AudioWorklet and Chrome on both paths 0 in each. A browser probe counts the opens (`tests/probe/web_audio`). On the ScriptProcessorNode path the page keeps one AudioContext: a reopen after the tap resumes it (a new context made after the tap may stay suspended on iOS Safari). `yau_open()` gives the fit 3 s from the first callback, at most 5 s from the start. With both: 0 silent in 30, 31, 31 and 30 opens (Firefox, Chrome; both paths), one context per page, the reopen played in 61 of 61; 2 mutants killed. Not run on a phone or in Safari. [Web envelope](audio.md#web-envelope-v052).
- v0.5.1: WASAPI exclusive mode writes one silent buffer before Start. Without it, 19 of 35 exclusive opens on this laptop (Realtek behind Intel SST) underran for the whole run: 20 to 26 XRUN records a second, LATE at every lead, in every version tested (v0.3.1 to v0.5.0). With it, 0 of 22; `audio_schedule --exclusive` gives a 125 ms shortest safe lead again, residuals -12.8 to +10.0 us. A device with position reports now fits reports only, which removed a false halt record at each exclusive open. Shared mode unchanged. 1 new mutant killed. [Exclusive start](audio.md#exclusive-start-v051).
- v0.5.0: capture. On the laptop's SST microphone array through WASAPI: 0 losses and 0 drops in 3 minutes idle and 3 under load (shared) and 30 s at 480 and 144 frames (exclusive); the input fit's spread 14 to 70 us; duplex with the speakers, both fits within 0.2 ppm, 0 underruns. Every injected stall of 20 to 300 ms lost the stall less about 12 ms and was found, one record each; 10 ms lost nothing. Through the air, speakers to microphones: 36.6 ms p50, sd 0.18 ms (shared output, exclusive input; the shared input does not pass the speakers' sound). Capture times are tier 3: no input checked against a reference. 22 capture mutants killed. In a browser: compiled with emcc, tier WEB, never run. [Design notes and how-to guides](audio.md#capture-v050).
- v0.4.1: `YAU_BACKEND_WEB` ([web.md](web.md), "Audio"): position reports from the AudioContext's `getOutputTimestamp()` on miniaudio's AudioWorklet path (threads, an isolated page) and ScriptProcessorNode path (no threads), `YAU_TIER_WEB` (5) for every web onset, main-thread waits that yield. Chrome 154 headed on this laptop, `av.html`, 3 runs a path: sound minus light (both the browser's estimates) within -0.26 to +0.25 ms (AudioWorklet) and -0.25 to +0.28 ms (ScriptProcessorNode, 2048 frames); the browser's render-to-onset figure 52 to 55 ms and 111 to 127 ms. Firefox 157: one guarded AudioWorklet run, -1.27 to +0.03 ms; its pairs need a lower envelope and an `outputLatency` correction. Sound at the speaker not measured. 2 of 2 new mutants killed.
- v0.4.0: `yau_stream_open()`/`yau_stream_close()` (close never fails: it cancels a play and waits for the device), `yau_wav_open()` returns bool and `yau_wav_close()` never fails, desc times end in `_ns`, `YRT_S`/`YRT_MS` replace `YAU_S`/`YAU_MS`, every failing frame-thread call writes `yau_error()`; 3 of 3 new mutants caught.
- v0.3.1: an underrun of about a period, which halts the WASAPI position short of the frames written, is now found from reports that stay late against the fit: an XRUN record with the halt's length, the fit restarted, the sounds it moved flagged XRUN, tier 3. 126 of 126 injected halts found (stalls of 5 to 30 ms at queues 1 and 2), none for the 84 stalls ridden out, 0 false records in 26 minutes of the long runs. 20 of 20 mutations caught.
- v0.3.0: the WASAPI shared queue (`desc.queue`, default 2 periods): shortest safe lead 50 ms before, 40 ms at the default, 30 ms at 1, with 0 underruns in 10 minutes idle and 3 minutes under load at each and residuals within tens of microseconds, tier 2; the endpoint offers 10 ms periods only, so not under 10 ms here. Each period saved is a period less margin against a stalled callback; queue 1 stays opt-in for that margin. Exclusive mode with one period per buffer: no underruns, onsets on the plan, 125 ms lead. 11 of 11 mutations caught.
- v0.2.1: `yau_wav_probe()`, the WAV parse without a device, for the pack tool (4 of 4 mutations caught).
- v0.1.0. Software timestamps only, on one Windows 11 laptop, Realtek speakers, WASAPI shared at 48 kHz: 0 underruns in 10 minutes idle and 3 minutes under load; every burst at its planned frame in WASAPI's own loopback capture (700 of 700 idle, 500 of 500 under load); onset time against the capture 67 us p99 from its median idle (one frame, 20.8 us, was the bar: missed) and 20 us under load in one run, so tier 2 with those conditions; 18.7 us per callback with 32 voices; no heap call after open. No loopback cable: the delay to sound is unmeasured. A core test with a scripted device on a virtual clock on MSVC, MinGW, gcc and emcc, sanitizer and ThreadSanitizer clean, fifteen mutations caught. [Design notes](audio.md).

### ysp/gfx.h

- Header: [ysp/gfx.h](../include/ysp/gfx.h)
- Prefix: `ygfx_`, `YGFX_`
- Platforms: Windows 11 through ysp/screen.h (DXGI flip, composition swapchain); headless on any GL ES 3.0 context (tested: ANGLE, Mesa); Linux and macOS: the null backend on the simulated display
- Needs: ysp/screen.h, ysp/color.h (SDL3; ANGLE at run time on Windows)

Purpose: Stimuli on GL ES 3.0 over ysp/screen.h: signed-distance shapes with a stated edge profile (hard, raised cosine, Gaussian), gratings, gabors, dot fields, image textures and sprites with tint, and hash noise, drawn in linear light into a float scene, then one output stage (a lookup table per channel, ordered or noise dithering, 8-bit codes). Strokes on every shape (inside, centered or outside; round or mitered joins), shapes from signed-distance textures, flat groups and render targets. Vector shapes from closed-form distance functions: rounded rects, arcs, pies, capsules, regular polygons, stars, ellipses, Bezier and polyline paths, compounds of up to 56 primitives with smooth merges, drop shadows, glows and outlines in one pass, dashes and trim, gradients in RGB, Oklab or DKL, add and multiply blends, MSDF atlases and instanced glyph runs; text and artwork from Slug curve sets (exact area per pixel on resolved glyphs), a separable Gaussian blur pass for glow, soft shadows and blurred letters; MIT notices for the SDF functions (Inigo Quilez, 0xfaded). Screen coordinates from the top-left corner, a place and an anchor per stimulus, hit tests by the shader's own SDF. The calibration, its file and the color math are ysp/color.h's (v0.5); `ygfx_color()` gives the context PAINT uses; a user shader contract; timeline bindings; capture of chosen frames to files during a trial; a 24-call backend interface.

Status:

- v0.17.1 (2026-10-22): fixed: `ygfx_text_make()` cut a font path of 512 bytes or more to 511 and opened that, another file; now YGFX_ERR_IO. The error's list of paths tried marks a cut path with "...". Found by gcc's `-Wformat-truncation` at -O1, -Os and -Og (gcc 11.4 and 16.1), which broke `-Werror` builds. 2 of 2 new mutants killed. [Design notes](gfx.md#v0171-font-paths-that-do-not-fit).
- v0.17.0 (2026-10-21): `ygfx_desc_params()` fills `yscr_param.values` (ysp/screen.h v0.9.0): `units`, `dither`, `output`, `stereo` and `cones`; `output` and `stereo` list every value of their enums, with `max` the last (was 0), and `open()` still takes `OUT_8` and `MONO` only. Every row checked against its field and enum, and `max_draws` against an opened gfx; 2 of 2 new mutants killed. [Design notes](gfx.md#v0170-values-in-the-desc-table).
- v0.16.0: `ygfx_enum_params()`, the stimulus fields that are not floats (place, anchor, shape, aperture, edge, dist, stroke_align, join, cap, blend, square, octaves, dash_snap and the IMAGE and TEXT flags), with kinds and enum values (gap G6); `ygfx_enum_get()` and `ygfx_enum_set()` apply a row by index; `ygfx_param` has a last field, `values`; 8 of 8 new mutants caught. [Every stimulus field in a table](gfx.md#v0160-every-stimulus-field-in-a-table).
- v0.15.1: the program cache's open and store times and a text's build time read the real clock (`yrt_real_ns()`), so they are costs also on ysp/rt.h's virtual clock, where they read 0 before. No other change.
- v0.15.0: `ygfx_open_screen()` (the screen and the gfx with OPEN START's overlap, then primed, in one call), `ygfx_show_until_key()` (an instruction or break screen), and with ysp/outline.h first `ygfx_text_make()` (text by advances, wrapped and centered, its program primed: the text's first frame 0.8 to 1.3 ms of CPU against 10.1 to 11.0 ms unprimed on the Iris Xe); 5 of 5 new mutants caught.
- v0.14.0: capture writes PPM only (the user, 2026-10-10): `YGFX_IMAGE_PNG`, `YGFX_IMAGE_PPM`, `ygfx_capture_desc.format` and the `format` argument of `ygfx_image_write()` removed, with the stored-PNG writer and its CRC-32 and Adler-32; the PPM writer's two-line comment tested; PNG's 3 mutants removed, 1 added.
- v0.13.1: fixed under WebGL: a pass into a render target unbinds its texture from every unit first; WebGL refused every draw with it bound to a sampled unit (133 on the gallery's first page in Chrome 154). No change natively.
- v0.13.0: stimulus capture during a trial, opt-in: every Nth frame or marked frames copied from the output stage into pixel buffers with fences, mapped a frame later without a wait, tagged with the frame index and flip record, and written by a writer thread at below normal priority (PPM or stored PNG, or a callback); a full ring drops and counts the frame; manual mode (and `YRT_NO_THREADS`) leaves maps and writes to `ygfx_capture_pump()` between trials; each capture call writes `ygfx_error()` when it fails, and `ygfx_image_write()` (which replaces the examples' four PPM writers) writes its message into the caller's buffer. At 1920 x 1200 on the Iris Xe a captured frame costs 0.56 to 0.77 ms of GPU and 2.6 ms of frame-thread CPU in ANGLE's map; capture of every frame moved no flip fullscreen or in a window (no late target, no early flip, no interval over 16.69 ms); the writer keeps up with every frame in PPM (8 to 11 ms a file) and every second in PNG (15 to 18 ms). Copies equal `ygfx_read_output()` bit for bit on three renderers; 16 of 16 capture mutants caught.
- v0.12.0 (the [Conventions](conventions.md): every failing call on a gfx writes `ygfx_error()`; 2 mutants added).
- v0.11.1: `ygfx_open_start()`, the open's first half, for ysp/screen.h's `desc.on_context`: it submits every program and returns, and `ygfx_open()` waits for the rest, so the compiles run while the screen settles: gfx ready 2.2 s earlier in a window and 0.4 s fullscreen with a cold cache (AC; 2.0 and 0.4 s on battery), 24 to 51 ms with a warm one. Without `GL_KHR_parallel_shader_compile` it submits nothing and the open is as before; the describe line says which. Frames bit-identical to a blocking open on ANGLE (hardware, WARP) and Mesa llvmpipe; 9 of 9 new mutants caught.
- v0.11.0: `ygfx_prime()` draws every program made so far once, as zero-size quads, and waits for the GPU: on the Iris Xe each kind's first frame cost 2.5 to 12.9 ms more CPU in `ygfx_end()` without it (from a warm program cache too) and 0.09 to 0.42 ms with it; 20 programs prime in 63 to 73 ms; frames before and after are bit-identical; 3 of 3 new mutants caught. The first frame after a setup of 2 s or more was late or dropped in 21 of 22 runs, prime or not, and `ygfx_prime()` right after `ygfx_open()` hung, both from ysp/screen.h v0.5.0; with v0.5.1, 0 of 8 and no hang (docs/gfx.md, "v0.11: priming").
- v0.10.5: an R8 or RG8 texture may state a transfer (gray is the display's white), so a pack's TEXTURE entry loads into `ygfx_texture()` as stored (within 5.1e-7 of the formula on three renderers; 4 of 4 new mutants caught); `YGFX_SHADER_CONTRACT` is 2.
- v0.10.4: a user shader samples `stim.tex`, an image or a render target, as `ysp_tex0`; before, it read whatever the previous draw had left bound (found by `gfx_gratings`; 2 of 2 new mutants caught).
- v0.10.3: `YGFX_SHADER_CONTRACT` (1), the shader contract's version that packs record; the test pins the wrapper's text.
- v0.10.2: `ygfx_default_cache_dir()` is a wrapper of ysp/rt.h's `yrt_user_dir()`, same rules.
- v0.10.1: `ygfx_default_cache_dir()` gives the per-user cache folder (never a shared one such as /tmp), which the examples use unless `--cache` or `--no-cache` is given.
- v0.10.0: gradient noise (3D simplex fBm: scale, octaves, lacunarity, gain, z to evolve) in integers, bit for bit equal to its CPU twin on four renderers; not band-limited, as the manual states; 1.8 ms full screen for one octave.
- v0.9.0: new noise and dither patterns: the hash is triple32 and a point's key one hash of its index under seed keys; v0.8's nested hash gave 29 % of full-screen frames two rows that were copies (PractRand failed it at 256 MB; the new key passes 16 GB).
- v0.8.0: setup passes render text, artwork or a blur layer into a target outside a frame (the same bits as in a frame); turned and aliased curve runs take the rays program by themselves (the same bits; a turned 12 px page 5.1 ms, was 5.9).
- v0.7.0: curve runs with `.rays` on their own program (a 12 px page 5.3 ms against 7.5), and a page cached in a target equal to the drawn run bit for bit (0.5 ms a frame); Gaussian NOISE from a quantile table, the same bits on the GPU and the CPU; three specialized vector programs (RRECT 0.95 to 0.57 ms, a dashed circle 0.93 to 0.51, a compound of 8 with effects 1.6 to 0.96); ELLIPSE packs 600 times faster; the default test runs in under a minute.
- v0.6.0: curve runs within 1.8e-5 of the exact pixel coverage on resolved glyphs (rays elsewhere: mean edge error 0.019 at 12 px per em), the same on three renderers to 6.5e-5 but one pixel; a 12 px page of 26205 glyphs 5.5 to 8.2 ms of GPU, a 40-glyph line 0.09 ms; the blur within 0.0087 of the exact blur at sigma 2 (2x: 0.0021), 0.18 ms for a word at sigma 2; NV12 video 1.15 times RGBA8 (was 1.64); 29 of 29 v0.6 mutations caught.
- v0.5.0: the calibration moved to ysp/color.h with every readback of the test bit-identical on three renderers; OKLAB paint on absolute XYZ, the display's black included (5.2e-3 of full scale on a test display with 0.5 cd/m2 of black); `desc.cones`, `desc.lum`.
- v0.4.0: a program binary cache (open 11 to 35 ms warm, against 2 s or more), NV12 and I420 video converted to linear light (within 3.1e-7 of a double reference), texture import (GL; D3D11 RGBA and NV12 through ANGLE, shared handles included), instanced stimuli (10000 gabors in 0.18 ms of CPU and 1.3 ms of GPU) with a per-element hit test, and draws reordered where they do not overlap (bit-identical; 1000 interleaved stimuli 7.2 to 1.1 ms of GPU); 35 of 35 v0.4 mutations caught.
- v0.3.0: pixels checked offscreen against a CPU reference on four renderers (Iris Xe through ANGLE, WARP, SwiftShader, Mesa llvmpipe): largest error 1.7e-5 of full scale (1.4e-4 on SwiftShader), and exact for images, noise, hard edges and bands, sprite borders, targets against direct drawing, output codes and ordered dither; the v0.3 shapes within 4.9e-6 of the profile of the brute-force Euclidean distance, and the edge model's limits (corners, creases, smooth merges) measured against a true blur; 32 mutations in v0.1 and v0.2 and 25 of 26 in v0.3 caught (one equivalent). A stroke costs 1.05 to 1.17 times its fill; a full-screen target composite 0.43 ms of GPU. Calibration math within 3.9e-5 of IEC 61966-2-1's sRGB matrix and 7e-8 of Psychtoolbox's DKL matrix. On the Iris Xe at 1920 x 1200: an empty frame 0.42 to 1.34 ms of GPU, 1000 gabors 0.2 to 0.5 ms of CPU; no heap call per frame. No photometer: no number is a measurement of light. Open takes about 2 s (eleven programs). 10-bit, Bits#, stereo and text layout refused. [Design notes](gfx.md).

### ysp/video.h

- Header: [ysp/video.h](../include/ysp/video.h)
- Prefix: `yvid_`, `YVID_`
- Platforms: Windows through ysp/gfx.h; any platform on the simulated display and CPU decode
- Needs: ysp/gfx.h, ysp/timeline.h; pl_mpeg (optional)

Purpose: A movie as a stimulus: on each display frame the frame due at the predicted onset by ysp/timeline.h's lead rule, never by counting frames; a cadence that judders (3:2) is shown and named, slips against the display grid are found and named, and every display frame gets a record (frame, due time, flip onset, decision and reason). The movie clock is a ysp_timeline base, so annotations fire on the frame that shows them; it can follow the audio fit. Decode-ahead on a ysp/rt.h pump, upload into a ysp/gfx.h IMAGE; seek, loop, pause, manual frame index. A frame sequence container (raw or QOI, value-exact, read and write), MPEG-1 through pl_mpeg with a hashed index, and the Video decoder extension interface.

Status:

- v0.5.0 (2026-10-21): `yvid_params()` fills `yscr_param.values` (ysp/screen.h v0.9.0): `light`, `gpu_path`, `hw_decode` and `chroma`. Every default checked against a movie opened with a zero `ahead`; 2 of 2 new mutants killed. [Design notes](video.md#enum-values-in-the-parameter-table-v050).
- v0.4.0: on ysp/rt.h's virtual clock a movie decodes inline, and its decode and upload times stay real (`video_play --sim` showed 6 of 60 frames on a decode thread, a different count each run). 1 of 1 new mutant caught.
- v0.3.1: pl_mpeg.h builds under `-Wfloat-conversion` (gcc and clang); no change to ysp code.
- v0.3.0 (the [Conventions](conventions.md): `yvid_seq_open()`/`_finish()`/`_close()`, `yvid_probe()` returns a code, every failing call writes its message; 2 mutants added).
- v0.2.2: XXH64 twice as fast on MSVC (11.9 to 14.1 GB/s), the same hashes.
- v0.2.1: a custom decoder that returned not-ready was never woken again (a lost wake-up on the decode thread); now it is retried each display frame, and after an outage the due frame shows on time (docs/video.md).
- v0.2.0: Media Foundation (DXVA by default), planar NV12/I420 upload to ysp_gfx's shader by default, the shared D3D11 GPU path as an option, the soundtrack on ysp/audio.h streams, and ysp_timeline base rates for any rate but 1.
- v0.1.0. A core test with a scripted decoder and display on a virtual clock (cadences, slips against an exact model, stalls, seeks, the timeline agreement under 20 us of noise); 20 mutations caught. In a 640 x 360 window on the Iris Xe: `yvid_update()` with a 1920 x 1080 RGBA8 upload 0.95 ms mean, 1.8 ms p99; no heap call per frame. pl_mpeg 1080p 5.5 ms a frame plus 7 ms of I420 to RGBA8 on the decode thread. No Media Foundation, no GPU zero-copy, no capture yet. [Design notes](video.md).

## Formats not built

### The experiment definition

- Design: [experiment_format.md](experiment_format.md)
- Read by: the player (rig_spec 6); not built

Purpose: The pack's EXPERIMENT entry: a JSON file, read by ysp/json.h, that names stimuli, trial templates, adaptive methods, device roles and a tree of nodes, each lowered to ysp/trials.h, ysp/timeline.h, ysp/response.h, ysp/gfx.h and the method headers; and the data set the player writes.

Status:

- 2026-10-21: the data set's writer is built, ysp/dataset.h v0.1.0 (gap G13).
- Design decided, 2026-10-14: all 15 decisions accepted; D4 changed (durations are plain numbers in seconds, and every duration key and variable ends in `_s`); D11 records flagged frames only, and a requeue, if ever added, is an explicit rule in the definition. G7 (a canonical form for non-integer numbers, the prerequisite for the experiment hash) is closed by ysp/json.h v0.2.0 (2026-10-21). G9 (glyph runs from the definition's texts) is closed by the pack tool v0.6.0 (2026-10-21; `examples/pack/stroop/` is the Stroop example's pack source, with no listed glyph runs). G3 (variables from a per-trial stream), G4 (weight x reps), G10 (256 columns) and G11 (`rate=display`) are closed by ysp/trials.h v0.6.0 and ysp/table.h v0.3.0 (2026-10-21). No loader, no player.
- Proposal, 2026-10-13. Four worked examples (2AFC staircase, RDK, Stroop from a CSV, EEG trigger) were mapped to the C examples field by field; 13 gaps in the C APIs are listed.
