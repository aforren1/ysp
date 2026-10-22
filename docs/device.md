# ysp/device.h

Status: v0.5.0, 2026-10-21 (response rules); v0.3.0, 2026-10-10 (v0.2.0, 2026-10-08, for the outputs); the rig profile, `ysp/rigfile.h` v0.2.0,
2026-10-09 (v0.2.0 adds roles bound to LSL streams, docs/net.md; v0.3.0,
2026-10-10, opens the device with `yrig_open_device()`; v0.4.0, 2026-10-13,
adds the photodiode run behind the display's onset offset and the input
bounds file, with the tools `rigfile_bounds` and `rigfile_onset`; v0.5.0,
2026-10-21, adds the viewing geometry, `view`). The headers' manuals are the reference (OUTPUTS, AT THE FLIP
and ROLES of `ysp/device.h`; THE FILE, CHECKS AT LOAD and BINDING of
`ysp/rigfile.h`). This page records the decisions the plan
(`docs/devices_spec.md`, steps 2 and 3 of section 14.2, and 11.2) left
open, the results, and what needs the user's hardware.

## How-to: run the photodiode board

1. Wire the photodiode module's digital output to pin 2 of a Teensy 4.x
   or a Raspberry Pi Pico (light gives HIGH; define `YSP_INVERT 1` if it
   gives LOW). For a bare photodiode on an analog pin, define `YSP_ANALOG 1`
   and set `YSP_PIN`, `YSP_HI` and `YSP_LO`.
2. Compile and upload `firmware/ysp_line/` with the Arduino IDE or
   arduino-cli:
   ```
   arduino-cli compile --fqbn teensy:avr:teensy41 firmware/ysp_line
   arduino-cli upload  --fqbn teensy:avr:teensy41 -p <port> firmware/ysp_line
   ```
   (`rp2040:rp2040:rpipico` for a Pico.)
3. Find its key: `photodiode_check --list`.
4. Check the board alone: `photodiode_check --key serial:16C0:0483:: --monitor 10`
   prints each edge, the state changes and the fit.
5. Tape it over the top-left corner of the display and run
   `photodiode_check --key <key> --csv run.csv` (WARNING: flicker up to
   30 Hz). It prints edge minus flip onset. To put the offset into the rig
   profile, see [set the display's onset offset](#how-to-set-the-displays-onset-offset-from-a-photodiode-run).

## How-to: send a trigger code

1. Find the output's key: `device_out_latency --list`.
2. Open an instance of its family, and wait until it runs. `ydev_open()`
   returns while the instance is still OPENING; an output before
   `YDEV_RUNNING` fails with `YDEV_ERR_STATE`, and its trigger is lost:

   ```c
   static ydev_device trig;
   ydev_desc d = { 0 };
   d.role = "trig";
   d.family = YBOX_TRIGGERBOX;          /* or BIOSEMI, MMBTS, LINES, LINE, XID */
   d.key = "serial:0403:6001:TB0123:";
   d.device = 2;                        /* or d.roles = &roles */
   d.ring = &log_ring;
   if (!ydev_open(&trig, &d)) die(ydev_error(&trig));
   if (ydev_wait_running(&trig, YRT_S(3)) < 0) die(ydev_error(&trig));
   ```
3. Call `ydev_out_pulse(&trig, 12, YRT_MS(2))` (12 for 2 ms, then 0) or
   `ydev_out_set(&trig, 12)` (hold 12), and check the result. Each call
   returns `YDEV_OK` or a `YDEV_ERR_*` (`ydev_strerror()` names it; the call
   does not write `ydev_error()`, because other threads may call it at the
   same time) and puts one `YDEV_REC_OUT` record in the ring:

   ```c
   int rc = ydev_out_pulse(&trig, 12, YRT_MS(2));
   if (rc < 0) fprintf(stderr, "trigger 12: %s\n", ydev_strerror(rc));
   ```
4. `ydev_close(&trig)` ends a pending pulse, writes 0 if a code is held,
   writes the family's close bytes (0xFF for the TriggerBox) and closes.

For the parallel port, include `ysp/parallel.h` before `ysp/device.h` and
set `d.transport = ydev_parallel_transport(&port)` with a zeroed
`ypar_port` and the key `"parallel:"` (or `"parallel:0x378"`,
`"parallel:/dev/parport0"`).

## How-to: send the code at the flip

1. Include `ysp/screen.h` before `ysp/device.h`.
2. Set `d.pulse_ns` of the output (0 = hold the code).
3. Give the screen the output as a trigger channel:

   ```c
   yscr_trigger_desc ch = ydev_trigger_channel(&trig, 0);
   sd.triggers = &ch;
   sd.n_triggers = 1;
   ```
4. Between `yscr_begin()` and the flip, call `yscr_trigger(&scr, 0, code)`.
   The screen's deadline worker calls `ydev_trigger_fn()` at the planned
   vblank. Its OUT record has the FLIP flag and the deadline in `i64[3]`.

`examples/device/trigger_flip.c` does this on the simulated display.

## How-to: send a code at the response

A code sent from the frame loop goes out up to a frame after the press.
A response rule sends it from the response box's reader thread.

1. Open the output and wait until it runs ([send a trigger code](#how-to-send-a-trigger-code)).
2. Include `ysp/response.h` after `ysp/device.h` (or include it again
   after `ysp/device.h`), so that its rule helpers exist.
3. Make the rule's choices from the collector's choices, one code for
   each, and open the rule on the output:

   ```c
   static const uint32_t codes[2] = { 101, 102 };   /* choice 0, choice 1 */
   ydev_rule_choice ch[2];
   ydev_rule_desc rd = { 0 };
   static ydev_rule rule;
   if (yrsp_rule_choices(&rsp, codes, ch, 2) < 0) die("a choice the rule cannot read");
   rd.out = &trig;
   rd.choices = ch;
   rd.n_choices = 2;
   if (!ydev_rule_open(&rule, &rd)) die(ydev_rule_error(&rule));
   ```

4. Give the rule to the response box: `d.rule = &rule;` before
   `ydev_open()`.
5. In each trial, on the frame thread:

   ```c
   yrsp_arm(&rsp, f.onset);
   yrsp_rule_arm(&rule, &rsp);          /* a new window, no fire yet */
   ...                                  /* every frame: */
   yrsp_set_landing(&rsp, fired, n, STIM_ON);
   yrsp_update_frame(&rsp, &f);
   yrsp_rule_window(&rule, &rsp);       /* the window follows the onset */
   ```

6. At the end of the trial, before `yrsp_finish()`, call
   `yrsp_rule_sent(&rule, &rsp)`. The result then has
   `YRSP_R_SENT_MISMATCH` (in the `rsp_flags` column) when a code went
   out that the final responses do not match, or a response has no code
   ([response.md](response.md#codes-sent-v070)). `ydev_rule_get(&rule,
   &info)` gives the last fire's `code`, `t_event` (the press) and
   `t_before` (the write's start) for the row.
7. At the end of the session, close the response box, then the rule,
   then the output.

The output's record of the write has the `YDEV_OUT_RULE` flag and the
press's time in `i64[3]`. `examples/device/response_lag.c` does this and
measures both paths.

## How-to: measure an output's write-to-edge latency

The loopback of devices_spec section 12. The input is a board with
`firmware/ysp_line/` (pin 2 is its input, pin 3 its first output).

| Output | Wiring | Command |
|---|---|---|
| A USB-serial adapter's DTR | the adapter's DTR pin to the board's pin 2, grounds joined. A TTL-level adapter (3.3 V) only: an RS-232-level DTR (up to 12 V) needs a level shifter | `device_out_latency --out lines:serial:<vid>:<pid>:<serial>: --in serial:16C0:0483::` |
| A board's own output | the board's pin 3 to its pin 2 | `device_out_latency --out line:serial:16C0:0483::` |
| A trigger box's bit 0 | its bit 0 line to the board's pin 2, grounds joined (check the box's output voltage against the board's input range first) | `device_out_latency --out triggerbox:<key> --in <board key>` |

The program waits 6 s (`--warmup`) for the board's clock fit, sends 200
pulses (`--n`) of 5 ms (`--width`) 50 to 150 ms apart, and takes the first
edge of either polarity after each write. It prints n, missed, min, p5,
median, p95, max and mean in seconds, writes them with the samples to
`out_latency.txt` (`--file`), and prints the file's SHA-256: the rig
profile names the file by that hash (devices_spec 11.2). With `--store`
it also keeps the file in the rig profile's `loopback/` and writes the
role's binding and summary into the profile (next how-to). With a line
board as the output, it also prints the board's own time of each pin
change ("O" reports) minus the write.

`device_out_latency --sim` and `--sim --self` run the same procedure
against an in-process model and print the model's true latencies beside
the measured ones.

## How-to: keep a rig's devices and latencies in its rig profile

The rig profile (`ysp/rigfile.h`, devices_spec 11.2) is a JSON file in the
user's config folder: `%APPDATA%\ysp\rig\profile.json` on Windows,
`~/.config/ysp/rig/profile.json` on Linux. Tools write it; read it with
`rigfile_profile`.

1. Make the profile for the rig: `rigfile_profile --new booth-2`.
2. Find the devices' match keys: `device_out_latency --list`.
3. Bind each role to its device:
   `rigfile_profile --bind resp xid serial:0403:6001:FT4ABC12:` and
   `rigfile_profile --bind trig triggerbox serial:0403:6001:TB0123:`.
   A new binding drops the role's old loopback numbers.
4. Measure each output's write-to-edge latency into the profile (the
   wiring of the previous how-to):
   `device_out_latency --out triggerbox:serial:0403:6001:TB0123: --in <board key> --store --role trig`.
   The result file goes to `loopback\<sha256>.txt` beside the profile,
   and the profile gets the role's family, key and summary.
5. Check the profile: `rigfile_profile`. It prints each role, the check of
   each loopback file (`ok`, `missing`, `changed`, `differs`), the notes,
   and the profile's SHA-256. Exit code 3 means that a file failed its
   check: that role runs with tier UNKNOWN.

A machine with two rigs keeps `<NAME>.json` beside `profile.json`: give
`--rig NAME` to each program. `--dir DIR` (`rigfile_profile`) and
`--rig-dir DIR` (`device_out_latency`, `rigfile_bounds`, `rigfile_onset`)
use another folder, for a test.

## How-to: measure an input's bounds

An input's bounds are its event time minus the true time of the event
(devices_spec 12, "Input device with a clock"). A tier 1 input needs them
(`yrig_source()`). The reference is a second board with `firmware/ysp_line/`:
it reports each change of its output pin in its own clock.

1. Bind the input's role in the profile: for example
   `rigfile_profile --bind photo line serial:16C0:0483:<serial>:` or
   `rigfile_profile --bind resp xid serial:0403:6001:FT4ABC12:`. The role's
   family must be `xid`, `line` or `photo`.
2. Wire the reference board's pin 3 to the input, grounds joined: a line
   board's pin 2, a box's external input, or a relay on a button. A
   mechanical relay's operate time and bounce go into the bounds; prefer an
   optocoupler or a solid-state relay.
3. Find the reference board's key: `rigfile_bounds --list`.
4. Run `rigfile_bounds --role photo --ref serial:16C0:0483:<serial>: --store`.
   The program waits 6 s (`--warmup`) for both clock fits, pulses the pin
   200 times (`--n`) for 20 ms (`--width`), 50 to 150 ms apart, and matches
   each of the 400 changes to the first press or release of the input
   within 10 ms. It prints n, missed, extra, lo, p5, median, p95, hi and
   mean in seconds, writes them with the samples to `in_bounds.txt`
   (`--file`), and prints the file's SHA-256.
5. If at least 90% of the changes matched and the input sent no other
   event, `--store` keeps the file as `loopback\<sha256>.txt` and writes
   `roles.photo.bounds`. Each difference is widened on both sides by the
   widest bracket the reference board's clock map rested on at that change,
   and 1 us; lo is the least of these, hi the most. The file also keeps the
   raw least and most difference (`raw_lo_s`, `raw_hi_s`). Otherwise the
   exit code is 1 and the profile does not change.
6. Check: `rigfile_profile` shows the bounds with the check `ok`. The role
   now gets tier 1 from `yrig_source()`.

`rigfile_bounds --sim` runs the same procedure on two simulated boards on a
virtual clock and prints the model's truth beside the measurement.

## How-to: set the display's onset offset from a photodiode run

1. Run the photodiode on the display with a CSV:
   `photodiode_check --key <key> --csv run.csv` (or
   `screen_loopback <port> 3600 run.csv`). Use a run that exits 0: no edge
   missing or extra. Leave `desc.onset_offset_ns` at 0 for the run; both
   programs do.
2. Run `rigfile_onset run.csv --store`. It reads each row's
   `photodiode_ns - onset_ns`, writes n, min, p5, median, p95, max, mean,
   SD, the median of each polarity, `onset_offset_s` (the median) and the
   samples to `display_onset.txt` (`--file`), and prints the file's SHA-256.
   `--store` keeps the file as `loopback\<sha256>.txt` and writes
   `display.onset_offset_s` and `display.latency` into the profile.
3. If the CSV has rows of two presentation paths (composed and
   independent, for example), the program refuses it. Give `--path N` to
   keep one path; the offset is for that path only.
4. Give the profile's `display.onset_offset_ns` to `yscr_desc.onset_offset_ns`.
   `rigfile_profile` shows the run with its check; a changed or missing
   file, or an offset edited by hand, gives a note.

## How-to: set the rig's viewing geometry

Sizes in degrees of visual angle need the distance from the eye to the
screen and the size of a pixel on it. The rig profile keeps them in
`view` (ysp/rigfile.h v0.5.0, VIEWING GEOMETRY).

1. Set the display to the mode the experiments use, full screen.
2. Measure the distance from the eye to the screen's center along the
   line of sight, in mm (the chin rest's position).
3. Measure the width of the lit image on the screen, in mm, edge to edge,
   and read the mode's width in pixels.
4. Run `rigfile_profile --view DISTANCE_MM WIDTH_MM WIDTH_PX`, for example
   `rigfile_profile --view 570 518.4 1920`. It writes the profile and
   prints the pixels per degree at the center (36.8496 for that example).
5. Give the profile to the program: `trial_2afc_adaptive --rig NAME` (or
   the default profile). In C, `yrig_ppd(&prof)` gives the pixels per
   degree; for ysp/gfx.h's `ygfx_view` of a drawable w pixels wide, use
   `distance_mm` and `w * width_mm / width_px`.

The hash of the profile changes with `view`, so a data file's header
names the geometry its degrees came from. Change the chin rest or the
mode, and measure again.

## How-to: open a role's device from the profile

```c
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

static yrig_profile prof;            /* about 50 KB */
static ydev_device trig;
static ydev_roles roles;
char err[256], hash[65];
if (yrig_load(&prof, NULL, err, sizeof err) != YRIG_OK) die(err);
for (int i = 0; i < prof.n_notes; i++) log_line(prof.notes[i]);
if (yrig_hash(&prof, hash) != YRIG_OK) die("hash");   /* into the data file header */
ydev_desc base = { 0 };
base.roles = &roles;
base.ring = &log_ring;
if (!yrig_open_device(&trig, &prof, "trig", &base)) die(ydev_error(&trig));
if (ydev_wait_running(&trig, YRT_S(3)) < 0) die(ydev_error(&trig));
```

`yrig_open_device()` takes family, key, baud, latched and the rig's pulse
width from the profile and everything else from `base`. Its message is in
`ydev_error()`, also when the profile refuses the role. The device copies
the role and the key, so the profile may go before the instance. `yrig_source(&prof, &dev)` gives an input's
source entry with tier 1 and its bounds only when its loopback file
checked.

## Decisions

| Question | Decision | Why |
|---|---|---|
| The transport | `ysp/serial.h` is required and is the default; `desc.transport` replaces it | Every family of step 2 is serial. A header that must also work without serial would need a second build variant for no device yet. A test, a replay or a later HID or network transport passes its own functions |
| Threads | One reader thread per instance (devices_spec 4.8); `desc.manual` and `ydev_poll()` for no thread | Blocking reads are the simplest correct model, and a hung driver hangs only its own instance. Manual mode is also how the test runs the layer on a virtual clock (`desc.now`) |
| The fit | BRACKET mode for every family: frames the device sends on its own are pairs of width 0, answers to timer queries have the round trip as width | BRACKET is LATE plus a width bound, so both kinds of pair go into one fit |
| Timer queries | Every 100 ms; LINE `q <seq>`, XID `_e5`; none for the photodiode frame | The fit needs pairs between a box's rare presses (docs/rt.md) |
| Bracket bound | 1.3 ms for LINE (native USB); 20 ms for XID | An XID answer waits in the FTDI chip's latency timer, 16 ms by default; a 1.3 ms bound would refuse every answer. The lower envelope of the bracket ends still finds the least late ones |
| XID's 1 ms between bytes | Kept, as pyxid2 does; the bracket starts before the last byte | The box answers after the last byte, so the gap is not in the bracket |
| Identify | XID `_c1` must answer `_xid0` (another mode is switched with `c10`, logged), then `_d2`, `_d3`, `_d4`; LINE `i` must answer an `I` line; asked three times, 500 ms each | A wrong device at the key fails before the session (FAILED, not retried). Three tries cover a board that resets when its port opens |
| Silence | LOST after 2 s with no byte | Some USB-serial drivers never fail a read after an unplug (docs/serial.md). Syncs and query answers keep a live device talking |
| Reconnect | Every 1 s by key; each open starts a new fit; a GAP record gives the last byte before and the time it ran again | A device's clock can reset on power-up |
| `unc_us` | The device's tick plus the widest bracket the map rests on; without brackets, the fit's spread | The spread of XID answers (15.5 ms behind the latency timer) is not the map's error (about 1 ms): the first version used it and gave 16 ms per event. Mutant `dev-12` keeps the fix |
| A map past the read time | Clamped to the read time and counted (`clamped`) | An event cannot happen after it arrived; such a map is fit error or a board bug |
| Kind | 0 = the family's: BOX for XID and LINE, SYNC for the photodiode frame | `desc.kind` 0 would otherwise be KEYBOARD, which no box is |
| The bridge | The caller's sink; the example uses `yscr_push_input()` | `ysp/screen.h` stays out of this header; a logger or a test is another sink |
| Source tier | UNKNOWN | No loopback has checked any device on this layer (devices_spec 12) |

### Outputs (v0.2.0)

| Question | Decision | Why |
|---|---|---|
| Who ends a pulse | The device when it can: XID (`mp` then `mh`), line boards (`p <code> <us>`); BioSemi and the MMBT-S at switch P end every code after 8 ms by themselves. Otherwise the instance's own ysp/rt.h worker writes 0 at the deadline | A device-timed pulse is one write and has no host jitter on its end. The fixed-width boxes cannot give another width, so a request for one is refused (`YDEV_ERR_ARG`), and so is a `desc.pulse_ns` other than 8 ms at start: the binding fails before the session, not in the data |
| `yser_pulse_async()`, `ypar_pulse_async()` | Not used | They give no time for the trailing write, so the TRAILING record could not have its times, and they know no modem lines and no custom transport. The worker here is the same ysp/rt.h worker with the same rules: one pending edge, a set cancels it, a stop flushes it |
| The deadline | v0.2.0 to v0.3.0: the end of the leading write plus the width. Since v0.3.1: the start of the leading write plus the width, at least `desc.min_hold_ns` after its end ([Host-timed width](#host-timed-width-v031)) | The v0.2.0 reason, "the width does not include the leading write's own cost", was wrong over USB: the code arrives during that write |
| XID writes | Output commands in one write; queries still 1 ms per byte | Cedrus's command page: "XID 2 devices ... eliminates the need for an inter-byte delay". Per byte, `mp` and `mh` would delay the edge by 9 ms. Queries keep pyxid2's gap, as in v0.1.0, until a box shows it is not needed |
| XID width | `mp` is sent only when the width changes; unknown after each open | The box keeps its width, so a set after a pulse must send `mp 0` first (Cedrus: with 0, `mh` holds) |
| Writes and queries | One output lock serializes them | Two writers on one byte stream: an `_e5` between `mh` and its bytes would corrupt both. The cost: an output on an XID instance with queries can wait up to 2 ms (`desc.probe_ns < 0` turns queries off) |
| At the open | DTR and RTS opened low; a 0 to the TriggerBox, the MMBT-S at switch S, the parallel port and the lines | `yser_open()` raises DTR and RTS by default; a box that holds a byte can hold the last session's code |
| At the stop | The pending edge at once (FLUSHED), then 0 if a code is held, then the close bytes, then close | A latching box must never be left holding a trigger code (as serial.h's close) |
| Output-only families | No identify, no queries, no silence timeout; a transport may have no `read` | A TriggerBox, BioSemi or MMBT-S says nothing; silence is not a loss. The TriggerBox's input byte (its input lines changed) is counted as garbage, not decoded |
| `ydev_out_mark()` | The code as `desc.pulse_ns` says, then the text in TEXT records ("mark ...", 34 characters each) | No family of this step takes text; EyeLink and LSL markers come with their headers. The text is in the log either way |
| Failed calls | A record too, with FAILED | The log must show a trigger that was not sent |
| The trigger channel | `ydev_trigger_fn` and `ydev_trigger_channel()` are static inline functions defined when `ysp/screen.h` came first; a FLUSHED call (the screen's close) writes nothing | `ysp/screen.h` stays out of this header's dependencies. The screen's manual says a callback should not pulse at the close |
| Roles | `ydev_roles`, a table of 32 names the caller owns; `desc.roles` with `desc.device` 0 gives the index | The index is `yin_event.device` and the records' `aux`, so the data file joins by role. A role keeps its index across a restart. No global state |
| MMBT-S switch | `desc.latched` says S; the default is P | The switch cannot be read (the open-source driver's notes) |

### The rig profile (`ysp/rigfile.h` v0.1.0, 2026-10-09)

| Question | Decision | Why |
|---|---|---|
| Where the code lives | `ysp/rigfile.h`, prefix `yrig_`, on `ysp/device.h` and `ysp/json.h` | The profile is the rig's, not only its devices' (the display's onset offset and calibration are in it); it is files, hashes and a parser, which the device layer's run-time path does not need; a format change does not change `ysp/device.h`'s version. Binding fills a `ydev_desc` and families are `ysp/box.h`'s names, so it includes the device layer (one direction) |
| The parser | `ysp/json.h`, the repository's one strict reader (docs/json.md) | The pack tool had a strict reader; one reader for both means one set of edge cases |
| Canonical form | `ysp/json.h`'s YJS_WRITE_PRETTY and a final LF; seconds as the exact decimal of the nanoseconds; defaults left out | The hash then depends on the values only. The pack manifest has the same form |
| Unknown keys | Refused at every level, with the field path, line and column | A misspelled `ftdi_latency_s` would otherwise be a silent default |
| A loopback file's name | `loopback/<sha256>.txt`; the profile keeps the hash only | A name and a hash that could disagree are one more check that can fail |
| The checks | MISSING, CHANGED, and DIFFERS (an output latency file whose summary is not the profile's) | A hand-edited median with a genuine file is as wrong as a changed file |
| A failed check | Not a load error: a note, and tier UNKNOWN for the role | The rig can still run, with the tier it can prove, and the log says why |
| The tier | `yrig_source()`: tier 1 only for an input family with a device clock whose `bounds` file checks | ysp/input.h's tier 1 is a device clock mapped by a fit that a loopback checked. No output has a tier |
| A new binding | Drops the role's latency and bounds | They were measured on the device the role had before |
| Writing | `yrig_save()` reads its own bytes back before it writes, sets `written`, makes the folder (0700 on POSIX), writes a temporary file and renames it | A profile that would not load is never written; a reader never sees half a file |
| The folder | `yrt_user_dir(YRT_DIR_CONFIG, "rig")` only; refused means `YRIG_ERR_FOLDER`, no other folder | A folder another user can write may hold a planted profile (POSIX; on Windows `yrt_user_dir` reads no ACL) |
| Limits | 1 MiB of text, 32 roles (`ydev_roles`), 16 buttons, nesting 8 | A profile is a few KB; bounds keep a hostile file cheap |
| LSL streams (v0.2.0) | Family `lsl` (`YRIG_FAMILY_LSL`, 64: no `ysp/box.h` number), key `lsl:<name>:<type>:<source_id>:<hostname>` without a quote; no `baud`, `ftdi_latency_s`, `latched` or `pulse_s`; `yrig_desc()` and `yrig_start()` (v0.3.0: `yrig_open_device()`) refuse it and name `ysp/net.h`; `yrig_source_role()` gives the tier rule by role name, and counts `lsl` as a device clock | A rig profile names a stream as it names a port. The file format does not change, so v0.1.0 profiles load unchanged. `ysp/rigfile.h` does not include `ysp/net.h`: the player passes the role's key to the inlet |
| Opening a role's device (v0.3.0) | `bool yrig_open_device(dev, p, role, base)`, the message in `ydev_error(dev)`; it was `yrig_start(..., err, cap)` | It opens a ysp/device.h handle, so its message belongs where every open's message is (README.md, Conventions). A role the profile refuses (missing, unbound, lsl) writes its message there before the open, so the caller reads one place. `yrig_init()` stays an init; `yrig_load*()` and `yrig_save*()` stay load and save with a caller buffer: they fill a plain struct from a file |

The rig profile v0.3.0 was checked on 2026-10-10 (Windows 11, MSVC 19.44
/W4 /WX, MinGW-w64 gcc 16.1 -Werror): `tests/adapt/rigfile_test.c` with new
checks (the messages of a refused role and of a refused open in
`ydev_error()`, a close after each, the device's copies of role and key
outliving the profile); `tests/compile/rigfile.c` names every entry point by
type. Its mutation list's anchors all match; mutant `rig-29` (a refused
role's message not in `ydev_error()`) is new and was caught.

### The rig profile tools (`ysp/rigfile.h` v0.4.0, 2026-10-13)

devices_spec 11.2 names the fields (`bounds` per input, the display's
`onset_offset_s`) and 12 the loopbacks; it does not say what a tool reads,
which statistic it stores, or what the reference is. These are the choices.

| Question | Decision | Why |
|---|---|---|
| The bounds' reference | A second line board (`firmware/ysp_line/`): its output pin drives the input, and its "O" report, mapped by its own bracket fit, is the event's time | devices_spec 12 proposes the line-in capture. Its times are tier 3 (ysp/audio.h: no capture path has had a loopback check), and a capture's fixed delay would go into every bound. The board stamps its pin change in its interrupt, on native USB. Its own error stays in the result: the file states its fit's widest bracket and spread |
| lo and hi (changed 2026-10-14, user) | Each difference widened on both sides by the widest bracket the reference's map rested on at that change, plus 1 us (the board's tick); lo the least, hi the most; not percentiles. The file says `bounds_method reference-bracket` and keeps the raw range | The reference's own error is in every difference, so the raw range need not contain the truth: in the first simulated run it did not (truth -57.5 to +59.8 us, raw -106.2 to +34.7 us). A correction by ysp/rt.h's "a BRACKET map is late by 0 to its widest bracket" (d - 1 us to d + bracket + 1 us) missed the truth in 2 of 60 runs, by 1.0 and 5.1 us: the map also rests on the board's syncs, pairs of width 0 whose delivery no bracket bounds, and was early by up to 65 us. Widening both sides contained the truth in 160 of 160 runs (Results). A bound that leaves out 1% of events is not a bound |
| Matching | Both changes of each pulse (20 ms by default), each to the first press or release within half a width; any other event of the input fails the run | Both polarities are checked. An extra event means a wrong wire or a bouncing contact, and a bound from such a run is not one |
| When `--store` writes | Only when at least 90% of the changes matched and nothing was extra | A profile is evidence; a failed run must not replace a good one |
| Families | `xid`, `line`, `photo` (ysp/device.h with a device clock); `lsl` is open | tier 1 is for a device clock; an LSL inlet is ysp/net.h's and needs its own run loop |
| The onset tool's input | The per-edge CSV that `photodiode_check --csv` and `screen_loopback` write (`frame,level,onset_ns,photodiode_ns,diff_us,path,flags`), read strictly; integer ns, not the rounded `diff_us` | It is what a photodiode run produces today. The run must have `desc.onset_offset_ns` 0 (both programs leave it 0); the CSV does not say, so the how-to does |
| The offset | The median of light minus flip onset over both polarities | One edge matched to the wrong flip moves a mean of 200 edges by 80 us at 60 Hz and the median not at all. A stimulus change can go either way; the file keeps each polarity's median for a display whose rise and fall differ |
| Presentation paths | One path per offset: a CSV with two is refused unless `--path N` | A composed frame goes through the compositor and an independent flip does not (docs/screen.md); no measurement shows that one offset fits both, so a mix is refused rather than averaged |
| Minimum | 100 edges (`--min-n`) | A 600-frame run at 60 Hz gives about 200 |
| The file the profile names | The tool's own summary file (`ysp-display-onset 1`, `ysp-in-bounds 1`), with the samples and, for the onset, the CSV's SHA-256 | As `device_out_latency` (11.2: "the tool's own file"); the CSV's hash ties the summary to the raw run |
| Where the run goes in the profile | `display.latency`, in a role latency's shape; `display.onset_offset_s` must equal the file's `onset_offset_s` | 11.2 lists only `onset_offset_s` for the display; without the run's hash, the offset could not be checked as a role's latency is. An offset edited by hand with a genuine file gives DIFFERS |
| Shared code | Only the readers of the two new formats, in `ysp/rigfile.h` (`yrig_read_loopback()`); the statistics and the CSV parser stay in the tools | The player needs the readers for the DIFFERS check at load; it never computes a distribution |
| Bounds files at load | Compared with the profile's summary like latency files (they were checked by hash only); a file of another kind DIFFERS | A hand-edited bound would otherwise give tier 1 to a range nobody measured. A file with no format line is still checked by hash only, so hand-made files of v0.3 profiles keep their check |

### The viewing geometry (`ysp/rigfile.h` v0.5.0, 2026-10-21)

Gap G2 of [experiment_format.md](experiment_format.md): D4 says a
definition's degrees come from the rig, never the pack, and the profile
had no geometry. The 2AFC example hard-coded a distance and a pixel pitch.

| Question | Decision | Why |
|---|---|---|
| Where in the file | A top-level `view` object, beside `display` | `display` requires `onset_offset_s`, a photodiode result; a rig with a ruler and no photodiode must still have degrees, and making the offset optional would change what `display.set` means for every reader. The distance is the chin rest's, not the display's. If a rig gets two displays, each gets its own geometry then |
| The fields | `distance_mm`, `width_mm`, `width_px`, all required | ysp/gfx.h's `ygfx_view` has the first two, and its `width_mm` is the drawable's. A window is narrower than the screen, so the screen's mm alone give a wrong scale in a window; `width_px` gives the pitch (`width_mm / width_px`), which a window shares. It also says which mode the width was measured in |
| Numbers | mm as doubles, written as the shortest decimal that reads back the same (ysp/json.h v0.2.0); `width_px` an integer, 1 to 100000; each mm above 0 and at most 100000 (ysp/gfx.h's table's bound) | A hand edit (570.0, 5.7e2) gives the same bytes and hash. Seconds are fixed-point because a duration must be exact to the ns; no length needs that |
| The hash | A profile without `view` writes the bytes, and so the hash, of v0.4.0; `view` is in the hash once set | The geometry changes every size in degrees, so the data file's `yrig_hash()` must change with it. Existing profiles keep their hash |
| The helper | `yrig_ppd()`: width_px / width_mm * distance_mm * tan(1 deg), ysp/gfx.h's YGFX_DEG expression, 0 without a view | ysp/rigfile.h does not include ysp/gfx.h; the manual gives the two lines that fill a `ygfx_view` |
| No view | `yrig_ppd()` gives 0; the program decides. The 2AFC example falls back to nominal numbers (570 mm, 0.27 mm a pixel), flagged NOMINAL in its meta, as it does for the calibration; the definition has no fallback (experiment_format.md 2.14) | An example must run on CI with no profile; a definition's degrees are a claim |
| A full-screen mode of another width | Not converted; the 2AFC example prints the ratio and flags MODE in its meta | The panel may scale, letterbox or center the image: no rule fits every display, so the user measures in the mode used |

### Open, close and the wait for RUNNING (v0.3.0)

v0.3.0 applies the repository conventions (README.md, Conventions). It
breaks the API.

| Question | Decision | Why |
|---|---|---|
| Names | `ydev_open()` and `ydev_close()` replace `ydev_start()` and `ydev_stop()` | The call takes a desc and fills a handle with a thread, a lock and a connection: that is an open in every header. start and stop are for an activity on an open handle |
| The first output | `ydev_wait_running(dev, timeout_ns)`: `YDEV_RUNNING`, `YDEV_ERR_TIMEOUT` (new), or `YDEV_ERR_STATE` when the instance ends FAILED or CLOSED; the reason in `ydev_error()` | The v0.2 USAGE and this page sent a pulse right after the start, while the device was OPENING. The pulse returned `YDEV_ERR_STATE`, nobody checked it, and the first EEG trigger of a session was lost. Each example had its own wait loop; one call replaces them |
| The wait in manual mode | It calls `ydev_poll()` itself and sleeps 1 ms between steps; with `desc.now` it gives up when a poll does not move that clock | A manual instance has no thread to advance it. The caller owns a virtual clock, so a wait that cannot move it must not spin |
| The wait's message | Written under the instance's lock; a FAILED instance keeps the reader thread's reason ("did not answer as a line device") | The reader thread writes that message, so both writers take one lock, and the more specific reason wins |
| Key and role | Copied into the handle: `key_buf[YDEV_KEY_MAX]` (160, as a role's key in a rig profile, and enough for `port:` and any port name ysp/serial.h lists) and `role_buf[YDEV_ROLE_MAX]` (32, a role table's name); longer fails `ydev_open()` with a message | The reader thread read `desc.key` again at every reconnect, through the caller's pointer. A key built in a stack buffer was gone by the first replug. No allocation: the handle grows by 192 bytes |
| Times | `ydev_poll(dev, timeout_ns)`, `desc.read_timeout_ns`, `ydev_transport.read(conn, buf, cap, timeout_ns)` | Every C time is `int64_t` ns. A transport written for v0.2 fails to compile instead of reading 20 ns as 20 ms |
| `xid_width_ms` | Kept, in the handle | It is the XID protocol's own unit (`mp` takes whole ms), state the encoder compares with, not a time the caller writes |
| The output calls | Return a code only; `ydev_strerror()` (new) names it | Several threads call them at once (README.md, Conventions, Errors) |

Checked on 2026-10-10 on Windows 11, MSVC 19.44 (/W4 /WX) and MinGW-w64
gcc 16.1 (-Werror): `tests/adapt/device_test.c` 878 checks pass, with new
ones for the wait (a pulse before RUNNING refused, the wait reaching
RUNNING in manual and thread mode, at once on FAILED with the reason kept,
on a closed instance, and a timeout when the virtual clock stands still),
the copied key surviving its buffer and a reconnect, and the key and role
limits. `tests/compile/device.c` names every entry point by type and runs
the wait, a failed open, and a close twice. Mutants `dev-26` (the caller's
key kept), `dev-27` (a FAILED wait waits out its timeout) and `dev-28` (the
wait overwrites the reason) are new; all three were caught, and so was the
moved anchor of `dev-25`.

### Host-timed width (v0.3.1)

| Question | Decision | Why |
|---|---|---|
| Where the width starts | Just before the leading write (`t0` of its record), as ysp/serial.h v0.6 does for `yser_pulse()` | Over USB the code reaches the device during the leading write: a byte write on the Teensy 4.0 took 423 us median and its byte arrived at 179 us; a DTR call took 385 us and DTR arrived at 83 us (docs/serial.md, Pulses). From the write's return, every host-timed pulse was long by the rest of the write, with the write's return jitter in it |
| The rule | `yser_pulse_off_at_ns(t0, t1, width, desc.min_hold_ns)`: the later of `t0 + width` and `t1 + min_hold` | One rule for both headers, in one function that ysp/serial.h v0.6.1 exports, so they cannot drift apart. device.h writes through its transport (TRAILING records, modem lines, the parallel port), so it cannot call `yser_pulse()` itself |
| `desc.min_hold_ns` | 0 is 0.25 ms (`YSER_DEFAULT_MIN_HOLD_NS`) for every family but `YBOX_PARALLEL`, where 0 is none; the serial transport passes it to `yser_desc.min_hold_ns` | The hold keeps a width shorter than a stalled leading write from becoming no pulse at the device. An `outb` does not wait in a USB queue, so the parallel port keeps its width unless the caller asks for a hold |
| A trigger channel | Its start is taken on the screen's trigger worker, right before that worker's write | The worker's wake delays the pulse and does not shorten it |
| Device-timed families | Unchanged: XID (`mp`, `mh`), the line board (`p <code> <us>`), BioSemi and the MMBT-S at P | The device times the end; no host deadline |

### Parameter tables (v0.4.0)

A definition's devices-by-role block and the rig profile set fields of
`ydev_desc`. The loader and the designer check them against one source
([rig_spec.md](rig_spec.md), principle 8; gap G6 of
[experiment_format.md](experiment_format.md)).

| Question | Decision | Why |
|---|---|---|
| One call per family, or one call that takes the family | One call: `ydev_params(family, &n)`, NULL and `n` 0 for a value that is not a `YBOX_` family | The families share one desc. A new family is a new case, not a new function, and a loader iterates `YBOX_XID` to `YBOX_FAMILY_LAST` with `ybox_family_name()` |
| Which rows a family has | Only the fields it reads: `kind` and `silent_ns` for input families; `probe_ns`, `max_width_ns` and `no_identify` for the families that answer queries (XID, LINE); `baud` for every serial family; `pulse_ns` for output families; `latched` for the MMBT-S; `min_hold_ns` for host-timed outputs and the MMBT-S | A key that the family ignores is a mistake in a profile, so the loader can refuse it. The test derives the expected rows from `ybox_caps()`, so a family's rows follow its capabilities |
| The defaults | The family's own, as `ydev_open()` resolves them: `baud` 9600 for the MMBT-S and 115200 for the rest, `max_width_ns` 20 ms for XID and 1.3 ms for LINE, `kind` SYNC for PHOTO and BOX for the rest, `min_hold_ns` 0.25 ms and 0 on the parallel port | This is why the table is per family: one table could not state one default |
| A zero `baud` in the handle | `ydev_open()` writes the family's baud into the handle's desc | The table's default and the handle then agree, and the handle states the rate it opens. The serial transport opened at that rate before, so nothing on the wire changed |
| `kind` values | `mouse=1` to `user=8`, without `keyboard=0` | A zero `kind` is the family's kind, so a desc cannot choose the keyboard |
| The shape | `name`, `type`, `min`, `max`, `def`, `unit`, `doc`, `values`: the shape of every new ysp table | The tables of response, device, the methods and the timeline read alike |
| Not in the tables | `device`, `sink`, `ring`, `transport`, `manual`, `now`, `roles` | The program sets them, not a profile or a role. LSL roles (`family lsl` in the rig profile) are ysp/net.h's |
| Upper bounds | Times to 1e6 s; `pulse_ns` to 10 s (the rig profile's bound), 9 ms on BioSemi; `baud` to 10,000,000 (the profile's bound) | A range for the designer's input, not a limit that `ydev_open()` enforces |

The test opens a manual instance of each family with zero fields and
compares every row's default with the handle's desc, checks that every
row names a field and that the rows match the family's capabilities, and
checks the `kind` values against `yin_kind` with a `switch` without a
`default` (gcc and clang `-Wall -Werror` stop when a kind is added).
Checked on 2026-10-21 with MinGW-w64 gcc 16.1, MSVC 19.44 (`msvc-full`)
and gcc 11.4 on WSL2: 8 families, 71 rows; mutants `dp-01` to `dp-07`
caught, 7 of 7.

### Response rules (v0.5.0)

Gap G8 of [experiment_format.md](experiment_format.md): a definition's
`response.trigger` sends a code when the response comes, and the frame
loop saw a press up to a frame after it. The lag was measured first
([Results](#response-rules-the-lag-v050)).

| Question | Decision | Why |
|---|---|---|
| A callback or a rule | A rule: choices with a code each, a window, a fire count; armed by the frame thread, tried by the reader thread | A rule is data that the player lowers from the definition and `ydev_rule_open()` checks. A callback is user code on an elevated thread that holds the device's next read. A program that needs more calls `ydev_rule_input()` from a sink of its own |
| Where it lives | `ysp/device.h`; the glue to the collector (`yrsp_rule_*`) in `ysp/response.h`, outside its include guard | `ysp/response.h` is pure computation (no threads, no OS calls); the rule needs a lock and writes to an output. `ysp/device.h` has the reader thread, the output's lock and the records. The glue reads the collector's state, so the collector's header owns it, as `ysp/device.h` owns its `ysp/screen.h` glue |
| Where it runs | On the reader thread, after the event's time is mapped and before the sink | The sink only queues (`yscr_push_input()` 1.9 to 2.6 us); the code is the write that the gap is about |
| Write on the reader or hand it to a worker | On the reader | A worker adds its wake: most of `ydev_out_pulse()`'s 5.3 to 8.7 us (p50, v0.2.0) was the worker's wake. The reader's next read waits for the write (about 0.4 ms over USB); the events keep their device times |
| The lock | One lock per rule. The fire is claimed under it; the write is made outside it, under the output's lock | No lock is taken inside another, so the rule cannot deadlock with the screen's trigger worker or the frame loop's own writes. The fire count stays exact with two producers (a box and a sink of your own). Atomics would save about 100 ns beside a 0.4 ms USB write |
| Which time | The event's t (the device's mapped time), against `[from, until)` | The collector classifies a press by its t; the same comparison gives the same class. A report read late keeps its time |
| Which presses | A PRESS without REPEAT; the first choice that matches; at most `max_fires` per arm (0 = 1) | jsPsych's default: the first response ends the trial. ysp/response.h's R1. R2 (a press of a control already down) is not repeated: a box sends no repeats, and the fire count bounds the codes |
| Window from the collector | `[onset + min_rt, onset + duration)`, from the plan until the final onset; closed after a response ended the collector's window, not at its deadline | The collector counts a press stamped before the deadline and read after it, so its code goes too. A plan is known before the frame is shown, so the window is open before any valid press |
| The record | `YDEV_REC_OUT` with `YDEV_OUT_RULE`, the press's t in `i64[3]` | The payload has no free field. A RULE write has no flip deadline and no board ticks, the other uses of `i64[3]`. Write start minus press is `i64[0] - i64[3]`, as deadline to write is for FLIP |
| A code that the final classification does not match (user decision, 2026-10-21) | `ydev_rule_fires()` keeps each fire of the arm; `yrsp_rule_sent()` gives them to the collector, and the result flags `YRSP_R_SENT_MISMATCH` | The rule decides on the window it has at the press; a final onset a frame later can make that press an anticipation. The code is in the EEG, so the trial's row must say so. A flag in `rsp_flags`, not a new column: the row already has the result's flags, and the files' columns stay the same |
| Info for the frame thread | `ydev_rule_get()`: the arm's fire count, the last fire's press, code, result and write times; a write that ends after a re-arm does not touch the new arm's info | The data row needs the code and its lag; a late write belongs to the trial before |
| Keys and raw mice | Not hooked | SDL's raw keyboard thread and the raw-mouse reader are `ysp/screen.h`'s. A keyboard is not a response box (ysp/screen.h, INPUT) |

## Results

On 2026-10-08, MSVC 19.44 and MinGW-w64 gcc 16.1 on Windows 11, gcc 11.4
on WSL2. "Event t minus edge" is the mapped time minus the true host time
of the simulated edge.

| Check | Result |
|---|---|
| `tests/adapt/device_test.c`, 373 checks | Pass on all three compilers, also under ThreadSanitizer, ASan and UBSan on WSL2 |
| LINE board, native USB (125 us microframe, 50 us tail), 30 ppm, counter wrapping | 10 to 30 s: +6.0 to +15.1 us; unplugged at 30 s for 3 s with a clock reset; 43 to 60 s: +5.4 to +13.6 us. States OPENING, RUNNING, LOST, RUNNING, CLOSED; one GAP of 3.03 s; 522 queries, 522 answers; fit +29.67 ppm, widest bracket 211 us |
| XID box, a press every 2 to 4 s behind a 16 ms latency timer, -50 ppm | from 10 s: -972 to +852 us; from 120 s: -972 to +481 us (an event lies anywhere in its 1 ms tick, hence the -1 ms); 2768 queries, all answered, none refused; `unc_us` 1428 |
| Photodiode frame, 1 ms USB frame, no queries | +6.0 to +100.8 us |
| Silence 3 s | LOST after 2 s, RUNNING again |
| A device that does not identify | FAILED after 1.5 s, not opened again |
| An edge stamped 60 ms ahead | Clamped to its read time, counted once |
| The reader thread on the real clock, 1.2 s | 20 events, 12 queries, CLOSED after stop |
| `tests/loopback/device_loopback.c` on a socat pty pair (WSL2) | The serial transport: identified, 120 of 120 edges, the 100 after the first second within 229 us of their stamps, 55 queries answered; socat killed: LOST in 10 ms |
| `photodiode_check --list` (`examples/device/photodiode_check.c`) on the development laptop | "no serial ports" (none attached) |
| Mutants (`tests/mutate/device.toml`) | 11 of 12 caught; `dev-11` (an answer matched to any query) is equivalent here, because the simulated boards answer each query before the next. The first run missed `dev-10` (no clamp; `test_clamp` added) and `dev-12` (the uncertainty bound was too loose) |

On 2026-10-22 the reader-thread case (`test_thread`) failed in 1 of 10
runs with 128 busy threads on 16 logical processors: after 1.2 s the
reader had fewer than 15 events and 8 queries. The header was right. The
case now waits after the 1.2 s until it has 15 events and 8 queries. The
reader's calls to the simulated board bound the wait: it fails, with its
own message, after 5000 more calls, which are at least 5 s of the
reader's time. Mutant `ld-01` (no queries by default) checks it.
After the change: 0 of 30 runs failed under the same load (MSVC Debug,
ctest). The wait also ends when the device is not running, so a mutant
that never opens fails at once (`dev-09` timed out at 120 s before).
The rule race case (`test_rule_race`) did not fail in 18 runs under the
same load. It made 515 to 599 arms in its 600 ms, against a limit of 51,
so it did not change.

### Outputs (v0.2.0)

On 2026-10-08, MSVC 19.44 (Release) on Windows 11 for the numbers, under
`psy-guard.sh time` (load 18 %, on AC); MinGW-w64 gcc 16.1 and gcc 11.4
on WSL2 for the checks.

| Check | Result |
|---|---|
| `tests/adapt/device_test.c`, 853 checks | Pass on MSVC, MinGW and WSL2 (C99, C11), also under ThreadSanitizer, ASan and UBSan on WSL2 |
| Bytes per family, virtual clock | TriggerBox `00 05 07 00 09 03 00 FF` for open, set 5, pulse 7, its 0, pulse 9, set 3 (cancels the 0), stop; BioSemi and MMBT-S at P one byte per code, a 2 ms request refused; MMBT-S at S and the parallel port host-timed; DTR and RTS: low at the open, only the changed line; XID `mp` once per width, `mh` low byte first; line `o 5`, `p 1 2000` |
| Records | A pulse and its TRAILING record share a number; TRAILING times its own write and gives the host width; REPLACED, a set's cancel, FAILED (bad code, write error, not running), MARK text, FLIP with the deadline, FLUSHED at the stop |
| Line board reports | 20 of 20 "O" reports, mapped within 100 us of the board's true change |
| Trailing edge on the worker, fake transport, 2 ms pulses 3 to 7 ms apart, 3 runs of 300 | Width minus 2 ms: p50 +0.7 to +1.3 us, p95 +1.1 to +2.7, p99 +3.3 to +13.4, max +15.6 to +454 us. Trailing write minus its deadline: p50 0.4 to 0.7 us, p99 1.7 to 4.5 us. No pulse short. Worker TIME_CRITICAL |
| The same test 20 times on a loaded machine (other workers' builds, not under the guard) | In 2 runs one trailing edge was still pending when the next pulse came (3 to 7 ms after the last, so the worker was at least 1 ms late), and the next pulse moved it (REPLACED). The test checks the contract (each pulse ends, or the next one moves its end; none is short), not that the worker is never late |
| Cost of `ydev_out_pulse()` on the caller | p50 5.3 to 8.7 us, p99 28 to 60 us, max 5.5 ms once (the caller's thread, not elevated); the bracket of the fake write 0.3 to 0.6 us. Most of the call is the worker's wake-up (`yrt_worker_submit`) |
| `examples/device/trigger_flip.c`, simulated display, 600 triggers | 600 written; write start minus the planned vblank p50 4.3 us, p99 9.5 us, max 594 us (the screen's worker, its lock and the output lock) |
| `tests/loopback/device_loopback.c` on a socat pty pair (WSL2) | The serial transport's outputs: 20 `o` codes written, 20 reports back, the worst 718 us after its write |
| `device_out_latency --sim`, 100 pulses: DTR into a board | measured median 577 us, p95 983 us; the model's truth median 556 us, p95 949 us; the input's own `unc_us` median 142 us |
| `device_out_latency --sim --self`, 100 pulses | measured median 122 us, p95 187 us; truth 87 and 141 us; the board's "O" reports median 121 us |
| Mutants (`tests/mutate/device.toml`) | 24 of 25 caught; `dev-11` equivalent as before. All 13 output mutants (`dev-13` to `dev-25`) caught on the first run |
| Firmware (`firmware/ysp_line/` with `o`, `p`, `O`) | Compiles with arduino-cli 1.5.1 for `teensy41`, `teensy40` and `rpipico`, and in analog mode with three outputs for `teensy41` and `rpipico`. Not run on a board |

The simulated latencies check the procedure, not a device: measured minus
truth is +21 to +46 us, inside the input's stated uncertainty (142 to 202 us).

### Host-timed pulses on a Teensy 4.0 (v0.3.1)

The first real device on this layer's outputs: the echo board of
docs/serial.md (Teensy 4.0, `firmware/ysp_echo` version 2, Windows'
`usbser.sys`, COM3), which stamps each byte it receives and each change
of DTR and RTS. It is not a trigger box: the edge is the board's receive
time. `tests/compare/serial_pulse/device_pulse.c` opens an output
instance on a transport over a `yser_port` it owns, so it can read the
board's reports on the same port, and calls `ydev_out_pulse()`; the
instance's own worker (TIME_CRITICAL) writes the trailing edge.
`pulse_stats.py` computes the numbers. 2026-10-12, 11:43 to 11:57,
Windows 11, i7-1360P, AC power, under the timing guard, in the session
of docs/serial.md "Results with v0.6.1" (with the load of "The DTR call
tail" on the machine). MSVC 2022 /O2; v0.3.0 (with ysp/serial.h 0.6.0)
and v0.3.1 built from the same bench source. 200 pulses per cell, 50 to
150 ms apart.

Missed: 0 of 2400.

Width error (board width minus requested), microseconds:

| Family | Width | v0.3.0 median (p5 / p95 / p99) | v0.3.1 median (p5 / p95 / p99) |
|---|---|---|---|
| TriggerBox (a byte; the MMBT-S at S is the same path) | 1 ms | +352 (+181 / +537 / +628) | -65 (-137 / -6 / +87) |
| | 5 ms | +401 (+205 / +582 / +625) | -47 (-149 / +18 / +60) |
| LINES, DTR | 1 ms | +350 (+165 / +1474 / +2694) | -29 (-55 / +65 / +664) |
| | 5 ms | +354 (+177 / +1366 / +4168) | -16 (-45 / +7 / +18) |
| LINES, RTS | 1 ms | +363 (+182 / +1195 / +3030) | -34 (-72 / +503 / +4163) |
| | 5 ms | +379 (+190 / +970 / +2948) | -27 (-71 / +11 / +33) |

Onset (the board's edge minus the time before `ydev_out_pulse()`),
median (p99): byte 212 us (342) in v0.3.1 against 242 us (362); DTR
88 us (153) against 115 us (160); RTS 116 us (187) against 140 us (205).
The rule does not touch the onset; the difference is within the spread
between cells. `ydev_out_pulse()` returned in 368 to 421 us median: the
call makes the leading write itself.

- The median error went from +0.35 to +0.40 ms to -65 to -16 us, as
  `yser_pulse()` did in ysp/serial.h 0.6.0 (-57 to -4 us in the same
  session). The p5 to p95 spread of a byte pulse went from about 0.36 ms
  to 0.13 to 0.17 ms.
- The 1 ms DTR and RTS p99 (+664 and +4163 us) are the DTR call tail of
  docs/serial.md: in this session 2 to 4 % of the line calls returned
  more than 1 ms late, and a trailing write waits for the leading call's
  return plus the hold. At 5 ms the p99 is +18 and +33 us.
- Not measured: a real TriggerBox, BioSemi, MMBT-S, XID box or line board
  (`p`), whose edge is an output pin, not a receive time; the parallel
  port (none here); an FTDI or other bridge; Linux and macOS. The
  device-timed families (XID, line board, BioSemi, MMBT-S at P) do not
  use the rule.

### Response rules: the lag (v0.5.0)

`examples/device/response_lag.c` (`device_response_lag`), MSVC 19.44 /O2
(`msvc-full`, Release), Windows 11, i7-1360P, AC power, under
`psy-guard.sh time` (load 7 % before, 1 % after), 2026-10-21. No serial
device was attached, so both ends are in-process: a line-protocol box on a
fake transport, read by a `ysp/device.h` reader thread, presses every 80
to 180 ms; two in-process TriggerBoxes take the codes. The input reaches
the frame loop through `ysp/screen.h`'s input bridge (a 320 x 200 window
open for it), and the loop runs on the simulated 60 Hz display (16.667 ms
between `yscr_begin()` returns, p5 to p95). Before: the tool's first
version built on main (f2fb5f3), frame loop only. After: this version,
both paths for each press.

Press stamp to write start, in microseconds:

| Path | n | min | p5 | p50 | p95 | p99 | max | mean |
|---|---|---|---|---|---|---|---|---|
| Frame loop, before | 600 | 21 | 977 | 8785 | 15898 | 16618 | 16667 | 8706 |
| Frame loop, after | 600 | 54 | 1054 | 8713 | 15962 | 16588 | 16680 | 8601 |
| Rule, after | 600 | 1.9 | 2.7 | 5.2 | 15.4 | 19.3 | 38.7 | 7.1 |

- The frame loop's lag is the press's phase in the frame: flat from 0
  to one period. It did not change with the rule beside it (a run
  without the rule: p50 8059 us, max 16659 us).
- The rule's lag is the decode, the rule's lock, the output's lock and
  the clock reads. A real box adds its USB delivery before the stamp (a
  board's own time, mapped, is earlier still) and the write itself after
  the start (about 0.4 ms over USB CDC, the code arriving about 0.2 ms
  after the start: [Host-timed pulses](#host-timed-pulses-on-a-teensy-40-v031));
  both are the same on either path.
- The bridge's own part: the stamp to the frame's `yscr_begin()` return
  was within 15 us of the frame loop's total in every row.
- With the window's own pacing (`--pace window`; the window was in the
  background, not the foreground, and Windows ran it at about 15 Hz:
  61.5 ms between frames, p50), the frame loop's lag was p50 32.9 ms, max
  74.5 ms (300 presses), and the rule's p50 5.1 us, max 19.0 us. A
  frame loop that stalls takes the frame-loop path's codes with it; the
  rule does not wait for it.
- Not measured: a real response box or trigger output on this path, and
  a keyboard (SDL's reader is not hooked).

Checks: `tests/adapt/device_test.c` 1951 checks pass (1819 before) with MinGW-w64 gcc
16.1 (`-Werror`), MSVC 19.44 (`msvc-full`), and gcc 11.4 on WSL2, also
under ThreadSanitizer (the race part: about 570 arms, 1140 fires from 4
threads, each fire one RULE record, records in write order, never more
than 2 fires an arm); `tests/compile/device.c` names every new entry point
and the glue by type, as C99, C11 and C++17; `response_lag --sim` checks
one write per press on each path, the RULE records, and no row with
`YRSP_R_SENT_MISMATCH`. Mutants `rule-01` to `rule-18` caught, 18 of 18,
and `dev-22`, whose anchor moved.

### The rig profile (v0.1.0)

On 2026-10-09, MSVC 19.44 and MinGW-w64 gcc 16.1 on Windows 11, gcc 11.4
on WSL2.

| Check | Result |
|---|---|
| `tests/adapt/rigfile_test.c` | 373 checks pass on MSVC (Release) and MinGW (Release, -O1); 381 on WSL2 gcc 11.4 (-O2, ASan with UBSan, and as C++17; 380 when the working folder is not under /tmp), where the planted-folder checks run too |
| Canonical bytes | A profile of every field written to a pinned text; read back and written again: the same bytes; a hand edit in another key order, whitespace and number spelling (`983e-6`, `0.0005770`, `2.000`, a BOM): the same bytes and hash |
| Refusals | 66 edits of the pinned text, each refused with the field it names: bad JSON (line and column), a duplicate role, the format, each required field missing, wrong types, ranges, an unknown key at each level, role names, families, `latched` on another family, percentile order, hashes, dates, calibration paths; 33 roles; 17 buttons; more than 1 MiB; nesting |
| Checks at load | A stored output latency file checks OK; its summary edited in the profile: DIFFERS; the file changed: CHANGED; removed: MISSING; the calibration changed: CHANGED; each with a note; more than 16 failures stop at 16 notes; without a folder: NOT_RUN |
| The user folder | `APPDATA` (Windows) or `XDG_CONFIG_HOME` (POSIX) pointed at the test's folder: save makes `ysp/rig/`, load reads it back, `<NAME>.json`, bad names refused; a relative variable is no folder (Windows). On WSL2: a base folder that all users can write, and a planted `ysp/rig` with mode 0777 under a good base, refused with `YRIG_ERR_FOLDER`; 0700 again, loaded |
| Binding | Through a fake transport on a virtual clock: the key reaches the transport, the role gets its index, the TriggerBox gets 00 05 07 00 for open, set 5, a mark at the profile's 2 ms pulse; `latched` and `baud` reach the desc; an unknown or unbound role is refused by name |
| Tier | Tier 1 with the bounds (-300 and 1100 us) for a checked xid role; UNKNOWN when not checked, changed, or for a trigger box with checked bounds |
| Damage | 20,000 random edits of the pinned text: no crash; the 2169 still valid write canonical text that reads back to the same bytes (the same count on every compiler) |
| `device_out_latency --sim --store` | The file stored as `loopback/<sha256>.txt`, the role written, `rigfile_profile` reads it back with the check `ok` (Windows and WSL2) |
| Fuzzing (`tests/fuzz/json_fuzz.c`, target 2) | MSVC libFuzzer with ASan, all four targets (JSON in a fixed arena, in a heap arena with a depth from the input, the rig profile, the latency file and the number parsers): 14,150,487 inputs in 1501 s, no crash and no broken round trip; the 2166 corpus files replayed on WSL2 gcc under ASan and UBSan with no report |
| Mutants (`tests/mutate/rigfile.toml`) | 24 of 24 caught on the first run |

v0.2.0 (2026-10-09, MSVC 19.44 and MinGW-w64 gcc 16.1 on Windows 11, gcc
11.4 on WSL2): 403 checks on MSVC and MinGW, 410 on WSL2: a profile with
two `lsl` roles read and written back to the same bytes; refused for
`ysp/device.h` with the header to use; tier 1 by role name only when the
bounds checked; 7 refusals (a serial key, a quote, an `lsl:` key on a box
family, `pulse_s`, `baud`, `latched`, a misspelled family); bindings that
check family against key. Mutants: 28 of 28 caught (`rig-25` to `rig-28`
new; the anchors of `rig-04`, `rig-13` and `rig-14` moved with the code).

Costs (`examples/rigfile/bench.c`, Iris Xe laptop on AC, under the measurement lock (load 9 %), 21 rounds, MSVC 19.44 /O2 and MinGW-w64 gcc 16.1 -O2; a 3453-byte profile with 4 roles and every field): `yrig_parse()` of a 3453-byte profile with 4 roles, every field checked: 23.3 us (MSVC) and 17.7 us (MinGW), medians; the JSON parse alone 4.9 and 4.4 us; `yrig_write()` 7.0 and 6.6 us; `yrig_hash()` 43 us. docs/json.md has the table.

### The rig profile tools (v0.4.0)

On 2026-10-13, MSVC 19.44 (/W4 /WX) and MinGW-w64 gcc 16.1 (-Werror) on
Windows 11, gcc 11.4 on WSL2 (C99, C11, the compile check as C++17; the
test and both tools also under ASan and UBSan). Nothing here is timed. No
board was attached: the tools ran on simulated boards and a generated CSV
only.

| Check | Result |
|---|---|
| `tests/adapt/rigfile_test.c` | 501 checks pass on MSVC and MinGW (409 before), 508 on WSL2 with the planted-folder checks: the three formats read, each refusal by message (another format, no format line, a missing `hi_s` or `onset_offset_s`, two format lines, a bad number); a profile with `display.latency` and a bounds file stored, loaded with both checks `ok`, written in canonical form; tier 1 from the checked bounds; DIFFERS for an edited bound, an edited offset, an edited display summary or date, and a genuine file of another kind (also one whose every field matches); MISSING with its note; NOT_RUN without a folder; 5 refusals of `display.latency` by field |
| `rigfile_bounds --sim` (defaults: 200 pulses, 6 s warm-up), first version (raw range) | 400 of 400 changes matched; raw -106.2 to +34.7 us against the truth -57.5 to +59.8 us (the reference's own error +8.2 to +68.0 us): the truth was outside. Bytes identical from all three compilers |
| `rigfile_bounds --sim`, 2026-10-14, widened on both sides (25 us virtual-clock steps) | 160 runs: seeds 1 to 20, delivery tails of 50, 200, 500 and 1000 us (`--sim-tail-us`), 6 s warm-up with 200 pulses and 2 s with 50. The bounds contained the truth in 160 of 160. How much wider than the truth, below and above, median (min to max) per tail at 6 s: 50 us: 169 (112 to 559) and 148 (91 to 529) us, truth 37 us wide; 200 us: 854 (99 to 2011) and 839 (51 to 2081) us; 500 us: 1673 (159 to 3291) and 1595 (73 to 3293) us; 1000 us: 3957 (215 to 6361) and 3921 (115 to 6197) us, truth 119 us wide. At 2 s: 50 us: 235 and 198 us; 1000 us: 3861 and 3534 us. The width comes from the reference's bracket, which grows with the tail (largest 0.48 ms in the default run; at most 0.56, 3.4 and 6.5 ms at tails of 50, 500 and 1000 us: with no bracket the width is the fit's spread); per change, 63 of the 40,000 reference errors lay outside that change's own bracket, but never outside the run's range. The "late by 0 to the bracket" correction, measured first on 60 of these runs (100 us steps then): truth outside in 2 |
| `rigfile_bounds --sim --role photo --n 50 --warmup 2 --store`, then `rigfile_onset run.csv --store` on a generated 300-row CSV, then `rigfile_profile` | Both stored and both checks `ok` (the CI steps); a CSV with two paths refused (exit 1) and kept with `--path`; a CSV with a wrong header refused (exit 2); fewer edges than `--min-n` refused (exit 1) |
| Fuzzing (`tests/fuzz/json_fuzz.c`, target 3 now also `yrig_read_loopback()`, which must agree with `yrig_read_latency()` on an output latency file; 3 new seeds) | MSVC libFuzzer with ASan, all four targets from the 15 seeds: 7,533,408 inputs in 901 s, no crash, no broken round trip, no disagreement; the 1297 corpus files replayed on WSL2 gcc under ASan and UBSan with no report |
| Mutants (`tests/mutate/rigfile.toml`) | 37 of 37 caught; `rig-30` to `rig-37` new, the anchors of `rig-10` and `rig-24` moved |

### The viewing geometry (v0.5.0)

On 2026-10-21, MinGW-w64 gcc 16.1 (`-std=c11 -Wall -Wextra -Wpedantic
-Wshadow -Werror`) and MSVC 19.44 through ctest. Arithmetic and files
only.

| Check | Result |
|---|---|
| `tests/adapt/rigfile_test.c` (`check_view()`) | 552 checks pass on MinGW (501 before), 560 on WSL2 gcc 11.4 under ASan and UBSan with the planted-folder checks. GOLDEN (no view) keeps the v0.4.0 bytes and hash; with a view, the block's exact bytes and a new hash, parsed back to the same values and hash; another spelling (`518.40`, `5.7e2`, keys in another order) gives the same hash; `yrig_ppd()` equals 570 mm * tan(1 deg) / 0.27 mm within 1e-12 (36.84958) and ysp/gfx.h's expression bit for bit; a view without display or roles; 16 refusals by field (missing, 0, negative, above 100000, not a number, `1e400`, `width_px` 0, 100001 and 1920.5, an unknown key, not an object); a view in memory with a negative distance, `width_px` 0 or a NaN is not written or hashed; cleared again, the v0.4.0 hash |
| `rigfile_profile --view 570 518.4 1920` | Written, shown with 36.8496 px/deg, canonical bytes as above; `--view 0 518.4 1920` refused (exit 2) |
| `trial_2afc_adaptive --sim --rig-dir DIR` with that profile (the CI steps, run locally on MSVC) | The view's numbers and the profile's hash in the meta; the data file byte for byte the same as without a profile (NOMINAL, the same 0.27 mm a pixel) and as the program before the change; a named folder with no profile exits 1 |
| Fuzzing | A seed with `view` added to target 2 of `tests/fuzz/json_fuzz.c`; the 18 seeds replayed on WSL2 gcc 11.4 under ASan and UBSan with no report. No libFuzzer run yet |
| Mutants (`tests/mutate/rigfile.toml`) | `rig-38` to `rig-45`: 8 of 8 killed |

## What needs the user's hardware

| Hardware | Checks | How |
|---|---|---|
| Teensy 4.x or Pico with the photodiode module | The firmware runs; identify; query widths on native USB; the fit on real USB delivery; edge minus flip onset; the bridge under a window | Steps 1 to 5 above |
| Unplugging that board during `--monitor` | LOST, reconnect by key, the gap | Pull the cable, put it back |
| The USB-serial adapter and a Teensy or Pico with `firmware/ysp_line/` | DTR as an output; write to edge for the adapter on this laptop | Wire the adapter's DTR to the board's pin 2 and the grounds together (3.3 V TTL adapter only). `device_out_latency --list`, then `device_out_latency --out lines:<adapter key> --in <board key>` |
| A Teensy, Pico or Arduino board alone | The board's outputs (`o`, `p`, `O`) and its own write-to-edge | Upload the new `firmware/ysp_line/`, wire pin 3 to pin 2, run `device_out_latency --out line:<board key>`. A classic Arduino (ATmega, USB-serial chip) works too, but its USB bridge adds its own latency |
| A trigger box at a flip | The trigger channel with a real output | `device_trigger_flip --out lines:<adapter key>` (simulated display; a windowed program sets only `sd.backend`) |
| A line board's button (or a Cedrus box) and a trigger output | The response rule on real USB: press to code at the output, against the frame loop's path | Wire the output to a second line board, give the box `desc.rule` as in [send a code at the response](#how-to-send-a-code-at-the-response); compare each RULE record's write start, and the second board's edge, with the press's stamp |
| A trigger box during a trial | Stimulus, response and end codes from the rig profile's role, at the box and in the EEG recording | `rigfile_profile --bind trig <family> <key>`, then `trial_eeg_triggers`; compare each code's time in the recording with the row's write offsets |
| A line board on a scanner's TTL output | Pulses as SYNC events through the role `scanner`, the anchor, each onset's volume | `rigfile_profile --bind scanner line <board key>`, then `trial_scanner_sync --sync board --tr <TR>` during a scan, or with a pulse generator on pin 2 |
| A Cedrus box (XID) | The decoder on real bytes, `_c1` and `_d` answers, the 1 ms byte gap, brackets behind the FTDI latency timer | `photodiode_check --family xid --key serial:0403:6001:<serial>: --monitor 30`, pressing buttons |
| The LabStreamer | An independent reference for the same flips | `net_labstreamer_flip` (docs/net.md, "LabStreamer hand test") |
| Two Teensy or Pico boards with `firmware/ysp_line/` (one with the photodiode module) | An input's bounds on real USB delivery; the reference's bracket widths | [Measure an input's bounds](#how-to-measure-an-inputs-bounds): the reference's pin 3 to the photodiode board's pin 2, `rigfile_bounds --role photo --ref <reference key>`. Compare `raw_lo_s` and `raw_hi_s` with lo and hi: the gap is the reference's bracket (`ref_bracket_max_s`), which only real USB delivery sizes; `extra` above 0 is a wire or contact fault |
| A Cedrus box with an external input, or a relay on a button | The XID FIT map behind the FTDI latency timer | The same, with the box's role: `rigfile_bounds --role resp --ref <reference key>`. The box's 1 ms tick puts up to 1 ms in the range |
| The photodiode board on the display | The display's onset offset into the profile | [Set the display's onset offset](#how-to-set-the-displays-onset-offset-from-a-photodiode-run): `photodiode_check --key <key> --csv run.csv`, then `rigfile_onset run.csv --store`; once for each presentation path the experiments use |

## Not done

- The `ysp/screen.h` match key for keyboard-mode boxes; HID, network and
  vendor transports; EyeLink markers. LSL markers are `ysp/net.h`'s
  (`ynet_out_mark()`); `ydev_out_mark()` text still goes to the log only.
- The parallel transport (`ydev_parallel_transport()`): compiled with
  MSVC, MinGW and gcc, never run (no LPT port here; devices_spec 16,
  decision 12).
- The rig profile: `rigfile_bounds` and `rigfile_onset` (v0.4.0) ran on
  simulated boards and a generated CSV only. Not built: bounds of an `lsl`
  role (the LabStreamer), the line-in as the bounds' reference, one onset
  offset per display on a rig with two (`display` is one object), and the
  player that refuses a session below `min_tier`.
- Every vendor output: no TriggerBox, BioSemi, MMBT-S, c-pod or
  StimTracker was attached; their bytes come from documentation and
  open-source code (docs/box.md).
- A vendor device on this layer; inputs from a real board. The only
  timing numbers on a device are the host-timed outputs into the Teensy
  4.0 echo board ([v0.3.1](#host-timed-pulses-on-a-teensy-40-v031)).
