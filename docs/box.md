# ysp/box.h

Status: v0.3.0, 2026-10-10 (v0.2.0, 2026-10-08, for the bytes). The header's manual (its comment block) is the
reference for each frame, query and output. This page records why the
header has its form, what its bytes were checked against, and what is not
checked. The plan is `docs/devices_spec.md` (sections 4.3 and 10); this
header is part of its steps 2 and 3.

## Why a pure header

A decoder takes bytes and the time they were read, and gives events and
clock pairs. It does no I/O, starts no thread, reads no clock and
allocates nothing (devices_spec principle 3). So:

- a test feeds it generated or captured bytes, split anywhere;
- an analysis can decode a raw capture of a session with this header
  alone;
- `ysp/device.h` owns the threads, the transport and the clock.

## Families

| Family | Frames | Clock | Queries | Checked against |
|---|---|---|---|---|
| `YBOX_XID` | `'k'`, info, uint32 LE ms (6 bytes); `"_e5"` + uint32 LE (7 bytes) | the box's ms timer | `"_e5"` (timer), `"_c1"` (identity) | pyxid2 (cedrus-opensource/pyxid at `d5b0a73`, 2025-01-14): `unpack('<cBI')` for key frames with port `& 0x0F`, key `(info & 0xE0) >> 5` (0 means 8), pressed `0x10`, frames with `info & 0x0C` refused; `unpack('<cccI')` for `"_e5"`; `"_c1"` answered `"_xid0"` in XID mode; `"_d2"`, `"_d3"`, `"_d4"` one byte each. Cedrus's XID command page agrees on the frame |
| `YBOX_LINE` | text lines (below) | the board's microsecond counter | `"q <seq>"`, `"i"` | this repository's own protocol and firmware |
| `YBOX_PHOTO` | `0xA5`, level, uint32 LE us; `0x5A`, 0, uint32 LE us | the board's microsecond counter | none | `tests/loopback/screen_loopback.c` |

XID also decodes the StimTracker 2 frame since v0.2.0: `'o'`, port, key
(0 means 8), `'1'` or `'0'`, uint32 LE ms, 0 (9 bytes; pyxid2
`unpack('<ccBcIB')`, Cedrus: "a terminating null byte").

## Outputs (v0.2.0)

`ybox_encode(family, op, code, width_ns, buf, cap)` gives the bytes; the
manual's OUTPUTS section has every byte. Each family's bytes are pinned to
a source:

| Family | Bytes | Who ends a pulse | Source |
|---|---|---|---|
| `YBOX_XID` | set `'m' 'h' lo hi`; width `'m' 'p'` + uint32 LE ms | the box, with the width of the last `mp`; `mp 0` makes `mh` hold | Cedrus [XID commands](https://www.cedrus.com/support/xid/commands.htm): "If the duration is 0 [default], then an mh command will set the output lines and hold them"; pyxid2 `d5b0a73`: `pack('<ccI', b'm', b'p', duration)`, `'mh'+chr(lines & 0xFF)+chr((lines >> 8) & 0xFF)` |
| `YBOX_LINE` | `o <code>\n`; `p <code> <us>\n` | the board | this repository (`firmware/ysp_line/`) |
| `YBOX_TRIGGERBOX` | one byte; `0xFF` before closing | the host | Brain Products, "TriggerBox Programming Examples" Rev03 ([PDF](https://www.nmr.mgh.harvard.edu/~tatiana/BrainVisionManuals/TriggerBox/TriggerBox_Programming_Examples_Rev03.pdf)): "Each byte written to the COM port is transmitted to the eight output lines (Bit 0 - Bit 7)"; "the output line should be reset to their default levels by writing a 0xFF"; port settings have no influence |
| `YBOX_BIOSEMI` | one byte | the interface, after 8 ms | BioSemi, [USB Trigger interface cable](https://www.biosemi.com/faq/USB%20Trigger%20interface%20cable.htm): 115200 baud, 8N1; "will hold this value for 8mS", then "all pins will be reset to zero". The USB-C model takes 1 to 99 ms by a command ([BioSemi](https://biosemi.com/faq/USB_Trigger_interface_ProgramPulseLength.htm)), not encoded |
| `YBOX_MMBTS` | one byte at 9600 baud | the box after 8 ms (switch P, factory) or the host (switch S) | Not the vendor's manual: chrplr/goxpyriment `triggers/notes/mmbts.md` (written from the manual v2.3, 2024: 9600 baud fixed, P and S modes, 1200 baud resets the box) and ebadier/NeurospecTriggerBox-Unity `SerialPort_MMBTS.cs` (9600 baud, one byte per trigger) |
| `YBOX_LINES` | none: DTR (bit 0) and RTS (bit 1) | the host | `ysp/serial.h` |
| `YBOX_PARALLEL` | one byte, the data lines | the host | `ysp/parallel.h` |

Left out: XID 1 devices' `ah` command (pyxid2 sends the inverted lines,
one byte), the BioSemi USB-C pulse-length command, BBTK TTL modules (no
byte protocol found), and the TriggerBox's input byte (counted as garbage).

## XID input layout: checked again (v0.2.0)

The v0.1.0 layout was checked a second time on 2026-10-08 against
pyxid2 at `d5b0a73` (fetched with `gh`, identical to the copy used for
v0.1.0) and Cedrus's command page:

| Item | v0.1.0 | Sources | Result |
|---|---|---|---|
| Key frame | `'k'`, info, uint32 LE ms; 6 bytes | Cedrus "six bytes ... `<"k"><key info><RT>`"; pyxid2 `'<cBI'` | Same |
| Info byte | bits 0 to 3 port, bit 4 pressed, bits 5 to 7 key, key 0 means 8; port bits 2 and 3 set: broken | Cedrus; pyxid2 `INVALID_PORT_BITS = 0x0C`, `KEY_RELEASE_BITMASK = 0x10` | Same |
| Timer answer | `"_e5"` + uint32 LE; 7 bytes | Cedrus; pyxid2 `'<cccI'` | Same |
| StimTracker 2 | not decoded | pyxid2 `'<ccBcIB'`, 9 bytes, for firmware 2 and product `S` | Missing: added in v0.2.0 |
| A broken frame | drop one byte, find the next frame | pyxid2 empties its buffer and flushes the port | Different on purpose: a resync keeps the frames after the noise |
| Baud | 115200 | pyxid2 tries 115200, 19200, 9600, 57600 and 38400 (a box's switches set it) | Different: `desc.baud` must be set for a box not at 115200 |
| Gaps between bytes | 1 ms per byte, every command | pyxid2: 1 ms; Cedrus: "XID 2 devices ... eliminates the need for an inter-byte delay" | Kept for queries; outputs go whole (docs/device.md) |
## The ysp line protocol

The plan's first draft had comma-separated lines (`t,<us>,<button>,<0|1>`).
The protocol as built has one letter per line type and spaces:

```
S <t>                  sync, every 100 ms
E <t> <ch> <0|1>       edge
A <t> <ch> <value>     sample
Q <seq> <t>            answer to "q <seq>"
I <text>               answer to "i"
O <t> <code>           the outputs changed (v0.2.0)
# <text>               comment
```

Host to board: `q <seq>`, `i`, and since v0.2.0 `o <code>` (hold) and
`p <code> <us>` (a pulse the board times). A board stamps `O` after its
pin writes, so the host has the board's own time of each output change.
Firmware without outputs ignores `o` and `p`.

Why:

- A letter per type lets one parser take syncs, edges, samples, answers
  and identity, and refuse the rest by name.
- Spaces are what `Serial.print` writes between numbers without format
  strings, so the firmware stays small and board-neutral.
- The sync gives the host pairs and a heartbeat when no edge comes; the
  query gives a bracketed pair (its width is the round trip), which a
  sparse device needs (docs/rt.md: sparse presses behind a 16 ms latency
  timer gave millisecond errors for minutes).
- A 64-bit decimal time is allowed, so a board with a 64-bit counter
  (the RP2040's `time_us_64()`) can send it; the reference firmware sends
  `micros()`, 32 bits, which the host unwraps.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Output | The caller's arrays of events and pairs; the decoder stops when one is full, and after one LINE `O` report or `I` identity line (v0.3.1), and returns the bytes it used | No allocation, and a bounded output per call. A byte finishes at most one frame (a broken frame shows by its ninth byte at the latest, and the eight bytes fed again hold at most one frame of 6 bytes or more), so arrays of one entry suffice; mutant `box-11` writes one past them and the test catches it |
| A broken frame | Drop its first byte and feed the rest again | A frame that starts inside a broken one is still found. XID frames with port bits 2 or 3 set are broken, as pyxid2 says |
| Garbage | Counted per call and in total, never an event | A misaligned stream must not give a press. The device layer logs the total at most once a second |
| Event time | The read time given; `ticks` the device time with `YIN_DEVTICKS` | The decoder reads no clock; the device layer maps `ticks` through the fit |
| XID writes | `ybox_slow_write()`: one byte at a time, 1 ms apart | pyxid2 writes every command this way. Its reason is not stated; it is kept until a box shows it is not needed |
| A text line | At most 94 characters; printable ASCII only; an overlong line or a control byte drops the whole line | The rest of an overlong line must not read as a line of its own |
| Encoders | One function, `ybox_encode()`, with an op; capabilities in `ybox_caps()` | The device layer asks what a family can do (who ends a pulse, lines or bytes) instead of naming families |
| Output-only families | `ybox_init()` takes them; every byte is garbage | One decoder path for every instance; a TriggerBox that reports its inputs cannot be mistaken for frames |

## Init and widths in nanoseconds (v0.3.0)

v0.3.0 applies the repository conventions (README.md, Conventions). It breaks
the API; no byte changes.

- **`ybox_init()` is an init that returns `int`.** It fills a decoder from
  arguments, takes no desc and has nothing to close, so it is not an open.
  It checks its arguments, so it returns 0 or `YBOX_ERR_ARG` (a NULL decoder
  or an unknown family) instead of a bool. `ybox_strerror()` names the code.
  `if (!ybox_init(...))` is now wrong: 0 is success.
- **`ybox_encode()` takes `int64_t width_ns`.** The width was `uint32_t arg`
  in microseconds, the line protocol's unit. Every C time is nanoseconds
  now. The encoder converts to the wire's unit: XID's `mp` rounds to the
  nearest millisecond (at least 1 for a pulse, because 0 means hold), the
  line protocol's `p` rounds to the nearest microsecond and refuses 0. A
  negative width, or one past the 32-bit field the old parameter could
  carry (2^32 - 1 us, 71.6 min), is `YBOX_ERR_ARG`; it was impossible to
  pass before and must not wrap now. SET ignores the width.
- **The unit macros.** This header does not include `ysp/rt.h`, so it
  defines `YRT_NS()`, `YRT_US()`, `YRT_MS()`, `YRT_S()` and the two
  read-back macros under the same `#ifndef YRT_S` guard, copied from it.

`ybox_ns_per_tick()` stays a `double`: it is a conversion factor, not a
time. The CHANGELOG and STATUS moved to the end of the top comment; the
USAGE block now tests `ybox_init()`'s result.

Checked on 2026-10-10 on Windows 11: `tests/adapt/box_test.c` with the new
checks (init codes, `ybox_strerror()`, negative and oversized widths, the
rounding of a line pulse to the microsecond), `tests/compile/box.c` naming
every entry point by type, MSVC 19.44 (/W4 /WX) and MinGW-w64 gcc 16.1
(-Werror). Mutants `box-25` (no width range check) and `box-26` (a line
pulse rounded up instead of to the nearest microsecond) are new; both were
caught, and so were the moved anchors `box-15`, `box-16` and `box-23`.

## One identity line per call (v0.3.1)

`ybox_out` holds one identity line (`has_text`, `text`) and one `O`
report (`has_out`). Up to v0.3.0, `ybox_decode()` stopped after an `O`
report but not after an `I` line, so a second `I` line in the same call
replaced the first. The Python binding's test found it:
`ysp.box.decode("line", b"I first\nI second\n").text` was `["second"]`.

v0.3.1 stops the call after an identity line too and returns the bytes it
used, up to and including that line's newline. A caller that loops while
bytes remain (USAGE in the header) gets every line. No byte is decoded
differently.

Callers in this repository:

- `ysp/device.h` identifies a line board by decoding its answer in a loop
  and stops at the first identity line; the reader thread loops over each
  read until every byte is used and ignores identity lines. Neither
  changes. `tests/adapt/device_test.c` passes.
- `ysp.box` and the MEX `ysp_box` already loop and gather every identity
  line, so they now return both. The binding tests check two and three
  identity lines in one decode: `ysp-box` 0.3.1, 88 tests pass (CPython
  3.14.3 with MSVC on Windows 11, CPython 3.12.14 with gcc 11.4 on WSL2);
  `test_mex_box.m` PASS, 71 checks, in MATLAB R2023a and Octave 10.1.
  `test_mex_device.m` PASS, 59 checks, in both.

Tests: `tests/adapt/box_test.c` puts identity lines (`I board <n>`) in
about 1 of 7 frames of every generated line stream and compares each, in
order, whole, byte by byte, in random chunks, one entry per call and with
garbage. `test_line_ident()` checks the bytes each call uses for three
identity lines (one bare `I`), then a pair, and an `O` report followed by
an identity line. 256 checks pass (MSVC 19.44, MinGW-w64 gcc 16.1, gcc
11.4 on WSL2 with ASan and UBSan); `device_test` passes its 1,951 checks
on all three. Against v0.3.0 the generated streams
fail: 128 to 156 frames wrong per pass. Mutant `box-27` (no stop after an
identity line) is new and caught; `box-11` and `box-20` moved with the
line and are caught; 27 of 27 in all.

There is no fuzz target for this header: `box_test.c` decodes 200,000
random bytes per family against a counting model instead.

## Verification

On 2026-10-08, MSVC 19.44 (`/W4 /WX`, C11, C++17), MinGW-w64 gcc 16.1 and
gcc 11.4 on WSL2 (C99, C11, C++17, `-Werror`), ASan and UBSan on WSL2:

| Check | Result |
|---|---|
| `tests/adapt/box_test.c`, v0.2.0 | 234 checks pass: the v0.1.0 checks, StimTracker 2 frames in every generated XID stream (whole, byte by byte, in chunks, with garbage), a broken StimTracker 2 frame with a key frame inside, `O` reports in every generated line stream and one per call, the bytes of every encoder and op, and 200,000 random bytes for each of the eight families (output-only families: every byte garbage) |
| `tests/adapt/box_test.c`, v0.1.0 | 135 checks pass. Each family: 1000 generated frames decoded whole, byte by byte and in random chunks, with output arrays of 64 and of 1, every field compared; 3 garbage bytes before each frame counted exactly; broken frames with a real frame inside found again; the line rules (seq 0, channel 0 and 256, level 2, missing and extra fields, 64-bit overflow, signs and exponents in samples, identity, comments, overlong lines, control bytes); 200,000 random bytes per family |
| `tests/compile/box.c`, `.cpp` | Build and run on both compilers |
| Mutants (`tests/mutate/box.toml`), v0.2.0 | 24 of 24 caught, the 11 new ones (`box-14` to `box-24`: XID byte order, ms rounding, a 0 ms pulse, the TriggerBox close byte, StimTracker 2 checks, one O report per call, output-only decoding, the DTR code range, single-byte pulses, the O code range) on the first run |
| Mutants, v0.1.0 | 13 of 13 caught. The first run caught 11: `box-11` (the reserve of 2 entries) was equivalent, which showed the reserve's reason was wrong (now 1 entry, and the mutant writes past the array), and `box-13` (an overlong line's tail read as a line) needed a check |
| Firmware (`firmware/ysp_line/ysp_line.ino`) | Compiles with arduino-cli 1.5.1 for `teensy:avr:teensy41` and `teensy40` (core 1.62.0) and `rp2040:rp2040:rpipico` (core 6.3.0, earlephilhower), in digital and analog mode; v0.2.0's outputs (`o`, `p`, `O`) also with three output pins in analog mode. Installed as a user, in `C:\tmp\psy-work\devices\arduino\`, not in the repository |

Not checked:

- Any byte from or to a Cedrus device. The XID decoder and encoders follow
  pyxid2's struct formats and Cedrus's page; a capture from a real box pins
  them before a timing claim. Whether the StimTracker 2 port byte is a
  number or an ASCII digit: pyxid2 keeps the raw byte, and so does `code`.
- Any byte to a TriggerBox, BioSemi interface or MMBT-S. The MMBT-S bytes
  come from open-source drivers, not from Neurospec's manual.
- The firmware on a board: not flashed, not run.
- XID1 devices, which have no timer query and use `ah` for outputs.

## Bindings

The analysis side decodes a raw capture without the device layer
(docs/devices_spec.md, section 5). Two bindings do that, added 2026-10-25:

- **Python**: `ysp-box` (`bindings/python/ysp_box`, the module
  `ysp.box`), a C extension on the Limited API (one abi3 wheel for
  CPython 3.10 and later), no run-time dependencies.
  `Decoder(family, kind, device).decode(data, t)` and the one-shot
  `decode(family, data, t)` return `Decoded(events, pairs, outputs, text,
  garbage)`. An event is the structseq `Event` with the fields of
  `yin_event` in the order that `ysp.device` and `ysp.response` also use:
  `t` (s), `t_ns`, `device`, `control`, `code`, `mods`, `kind`, `type`,
  `flags`, `stamp`, `unc` (s), `x`, `y`, `value`, `aux`, `ticks`.
- **MATLAB and Octave**: the MEX function `ysp_box`
  (`bindings/mex/ysp_box.c`). `'decode'` on a handle or on a family name
  returns the events as a 1 x N struct array with the same field names,
  the pairs as `uint64` and double columns, and the identity lines, the
  output reports and the garbage count.

What each binding does with the header:

- It feeds every byte of a call through `ybox_decode()`, in calls that
  fill 64-entry arrays in the decoder's own memory, and gathers the
  results. A frame split across two calls waits in the decoder, as in C.
- A read time is seconds; the binding rounds the exact product t x 1e9 to
  the nearest ns (as `ysp.rdk` does), so `t = ns / 1e9` gives back `ns`,
  and refuses a negative time, NaN and infinity. Device times stay counts;
  `tick()` is the nominal unit in seconds.
- It refuses an unknown family, an op, code, kind, device, sequence
  number or width out of its range, and data that is not bytes, before
  the C call. The header's `YBOX_ERR_ARG` from `ybox_encode()` raises
  `ArgumentError` (`ysp_box:arg`).
- The encoders, the queries and the family facts (`caps`, `code_max`,
  `fixed_pulse`, `default_baud`, `tick`, `slow_write`) are bound, so a
  PsychoPy or Psychtoolbox script can write trigger codes through its own
  serial library.

Not bound: `ybox_out_clear()` and the `ybox_out` arrays (the binding owns
them), and `ybox_strerror()` (the exceptions carry the message).

Tests (2026-10-25, Windows 11, i7-1360P):

| Test | Platforms | Result |
|---|---|---|
| `bindings/python/ysp_box/tests/test_box.py`: each family's frames to fields; a stream of 2,000 frames whole, byte by byte and in random chunks; 200,000 random bytes per family in random chunks (the line protocol's from random lines of random fields); encoder bytes; 49 refused calls and 25,000 calls with random arguments | CPython 3.14 and 3.9 with MSVC 19.44; CPython 3.10 with gcc 11.4 on WSL2 (`-Wall -Wextra -Wshadow -Werror` clean) | 87 pass, 1 expected failure (fixed in v0.3.1, below) |
| `bindings/mex/test_mex_box.m`: the same groups, 600-frame streams, 1,000 read times | MATLAB R2023a (MSVC 19.44), Octave 10.1 (MinGW-w64) | 69 checks pass |

Cost of `ysp.box` (CPython 3.14, Windows 11, i7-1360P on AC power, the
shared timing guard, best of 5): a 1-hour line-board capture (517 kB:
36,000 syncs and 3,600 edges) decodes in 12.0 ms whole and 19.8 ms in
64-byte reads; 100,000 XID key frames (600 kB) in 69.0 ms whole and 44.7
ms in 64-byte reads. That is 0.22 to 0.46 us per event or pair, almost
all of it in making the Python objects. Not measured: the MEX function's
cost.

The expected failure was a limit of the header up to v0.3.0: two `I`
lines in one `ybox_decode()` call kept only the second. v0.3.1 fixed it
([One identity line per call](#one-identity-line-per-call-v031)); the
test is now a normal one, and the MEX test has the same check.
