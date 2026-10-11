# ysp/response.h

Status: v0.4.0, 2026-10-10. v0.4.0 adds helpers for the frame loop
(`yrsp_update_frame()`, `yrsp_feed_screen()`, `yrsp_set_landing()`) and
the data file (`YRSP_DATA_COLS`, `yrsp_data_set()`); "The first trial
(v0.4.0)" has the decisions and the measurements. v0.3.0 moved every time
to `int64_t` ns. v0.2.0 is a trim: the second-report rule moved to
`ysp/screen.h` v0.4.1's input bridge ("The trim of v0.2.0"). The input
event lives in `ysp/input.h` (`docs/input.md`); this header keeps its
names as aliases. The header's manual (its comment block) is the
reference for each rule. This page has a tutorial built on
`examples/response/trial_keyboard.c`, how-to guides for other devices,
the decisions with their evidence, and what the example trials show
about the header.

## Tutorial: a keyboard trial with RT from the flip onset

In this tutorial, you run `examples/response/trial_keyboard.c`, the ysp
version of jsPsych's `html-keyboard-response`. Then you read its data
file and its code. At the end, you know where each number in a data row
comes from.

You need the examples built with SDL3 (the `msvc-full` preset,
[build.md](build.md)).

### 1. Run the trial on the simulated display

Run:

```sh
trial_keyboard --sim
```

The simulated display needs no window, and a synthetic participant
presses the keys. The program shows its instructions for 0.5 s, runs six
trials, writes `trial_keyboard.csv` and `trial_keyboard.csv.meta`, and
checks its own results:

```text
trial  0 square rt 0.450 s, correct
trial  1 circle rt 0.520 s, correct
trial  2 square rt 0.400 s, wrong
trial  3 circle too slow
trial  4 circle rt 0.380 s, correct
trial  5 square rt 0.610 s, correct
6 trials written to trial_keyboard.csv
--sim: every trial came out as scripted
```

The seed sets the order of the stimuli, so your order can be different.

### 2. Run the trial with a keyboard

Run:

```sh
trial_keyboard --reps 2
```

A window opens with the instructions. Press space to start. Press F when
you see a circle and J when you see a square. After four trials, the
program stops. To stop earlier, press Shift+Esc; the rows of the
complete trials stay in the file.

### 3. Read the data

Open `trial_keyboard.csv` in a spreadsheet, R or pandas. Each row is one
trial. Times are in seconds. An empty field means "none": no response,
or no onset.

| Column | Meaning |
|---|---|
| `index` to `outcome` | The session's columns from `ysp/trials.h`: the trial number, the block, the condition row, and the outcome |
| `stimulus`, `key_answer` | The condition, from the conditions table |
| `iti` | The inter-trial interval that `ysp/trials.h` drew |
| `trial_index`, `correct` | The trial number again, and 1 when `response` is `key_answer` |
| `n_duplicates` | Second key reports that the input bridge dropped during the trial |
| `response` | The key's name |
| `rt` | From the stimulus's onset to the press |
| `rt_key_duration` | From the press to the release |
| `onset_src` | Where the onset came from: 1 a flip record, 3 a plan only |
| `onset_tier` | How good the onset is: 1 an OS-observed vblank, 2 a composed path, 4 the simulated display |
| `onset_residual` | How late the stimulus's frame appeared: the flip record's onset minus its planned onset |
| `landing_residual` | How far the stimulus's frame is from the time in the sequence |
| `onset_frame` | The frame the stimulus appeared on |
| `rsp_tier`, `rsp_flags` | The press's stamp tier, and the `YRSP_R_*` flags of the result |
| `n_anticipations`, `n_early` | Presses before 0.1 s, and presses before the stimulus |

A row with `onset_src` 3 has an RT from a prediction, not from a flip
record. Use such a row for task development only.

Open `trial_keyboard.csv.meta`. It has the session's settings, the
screen's describe line and the keyboard's timing note. The last line,
`rows=6 closed=1`, says that the program closed the file normally.

### 4. Read the setup

Open `examples/response/trial_keyboard.c`. `main()` first opens each
part:

1. **The display and the graphics.** One call opens the window and the
   graphics, and draws each kind of stimulus once, so that a trial's
   frames do not compile anything:

   ```c
   ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .windowed = !fullscreen },
                    &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } });
   ```

2. **The stimuli.** Six shapes: the fixation cross, the circle, the
   square and three feedback rings. Each one has a timeline channel that
   makes it visible.
3. **The instructions.** `ygfx_text_make()` lays out the text with the
   system's font and prepares its first draw. Later,
   `ygfx_show_until_key()` shows it until the participant presses space.
4. **The conditions.** A CSV string goes through `ysp/table.h` into
   `ysp/trials.h`, which also draws the inter-trial interval in whole
   frames.
5. **The response rules.** F or J, within 1.5 s of the onset, and no
   response before 0.1 s:

   ```c
   yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2,
       .duration_ns = YRT_S(RESPONSE_S), .min_rt_ns = YRT_S(MIN_RT_S), ... });
   ```

   `"f"` is the physical key at the F position of a US keyboard. C times
   are `int64_t` ns; `YRT_S()` converts the program's seconds.
6. **The data file.** The column list names the program's own columns
   and adds `YRSP_DATA_COLS`, the response's columns:

   ```c
   static const ytr_col cols[] = {
       YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, correct, YTR_COL_BOOL),
       YTR_COL(row, n_duplicates, YTR_COL_INT), YRSP_DATA_COLS,
   };
   ```

### 5. Read the frame loop

Each pass of the `while` loop is one frame:

1. `yscr_begin()` gives the frame and its predicted onset, `f.onset`.
2. At the start of a trial, a `ytl_seq` puts the fixation on now and the
   stimulus on 0.5 s later. `yrsp_arm(&rsp, f.onset)` opens the response
   window.
3. `yrsp_feed_screen(&scr, &rsp, 1)` gives every key event to the
   collector.
4. `yrsp_update_frame(&rsp, &f)` does two things. When the flip record
   of the stimulus's frame arrives, it makes that record's onset the
   onset that RT runs from. Then it returns `YRSP_ENDED` at the first
   response or at the deadline. The program then hides the stimulus and
   shows the feedback.
5. `ytl_evaluate()` gives the events that land on this frame.
   `yrsp_set_landing(&rsp, fired, n, stim_ch)` takes the stimulus's
   onset event, if it is there: its planned onset is the onset until the
   flip record arrives.
6. At the end of the inter-trial interval, `yrsp_finish()` fills the
   result, and the program writes the row:

   ```c
   yrsp_data_set(&data, &res);   /* the response's columns */
   ytr_data_write(&data, &r);    /* and the program's own */
   ```

7. `ygfx_apply()` sets each stimulus from its channel, the stimuli are
   drawn, and `yscr_flip()` shows the frame.

You ran a trial on the simulated display and with a keyboard, read its
data file, and found the call that puts each timing column in the row.
The how-to guides below change the input device.

## How-to guides

### Use a response box on a serial port

`ysp/serial.h` reads bytes; it does not parse a box's protocol. Parse
each report into a `yrsp_input`:

```c
yrsp_input in = { .t = t_read, .kind = YRSP_KIND_BOX, .device = 0,
                    .control = button, .type = down ? YRSP_PRESS : YRSP_RELEASE };
yrsp_feed(&rsp, &in);
```

- `t_read` is `yrt_now_ns()` when `yser_read()` returned. This time
  includes the USB-serial latency timer (16 ms on FTDI parts unless
  `desc.low_latency` is accepted). It is not measured: give the source
  tier UNKNOWN.
- A box with its own clock (Cedrus XID reports a timer) gives a better
  time. Map the box's time to ysp_rt with a fit of time pairs, and give
  tier 1 only after a loopback test checks the map.
- Read the port on another thread if you must, but queue the events: only
  the frame thread calls `yrsp_feed()`.

### Keep key stamps honest when a GUI library runs

Another library can turn SDL text input on for the stimulus window (Dear
ImGui's SDL3 backend does, on the window with the focus). Keys then come
on the message path, about 11 ms late (`docs/imgui_probe.md`).
`yscr_event_input()` reads the path for each event. With your own
adapter, read the path each frame and give it to `yrsp_from_sdl()`:

```c
yscr_get_caps(&scr, &caps);              /* about 0.2 us */
sdl.raw_keyboard = caps.raw_keyboard;
```

Keys of those frames then get tier 3 in their entries and in the result
(`rsp_tier`), and `ysp/screen.h` v0.3.3 flags the flips
(`YSCR_FLIP_TEXT_INPUT`) and writes a ring record marked external.

### Run two keyboards or two participants

Each keyboard on SDL's raw path has its own device id, so a response can
come from one keyboard only.

1. Find the ids. Run `screen_input --devices`, press a key on each
   keyboard, and read which id each key came from:

   ```text
     key F          from keyboard 65601
     key J          from keyboard 65659
   devices in the ring:
     keyboard present id 65659        Computer Corp Dell Univ
     keyboard present id 65601        Standard PS/2 Keyboard
   ```

   The ids are Windows handles. They change when a keyboard is plugged
   in again and from one session to the next, so do not store them in a
   design. Open the screen with `desc.ring`: `ysp/screen.h` logs every
   keyboard, mouse and touch device with its id and name at open
   (`YSCR_EV_DEVICE`), and on hot-plug, so the data log maps ids to
   devices.
2. Give each choice its keyboard:

   ```c
   yrsp_choice keys[] = {
       { .key = "f", .name = "p1_left",  .device = id_p1 },
       { .key = "j", .name = "p1_right", .device = id_p1 },
       { .key = "f", .name = "p2_left",  .device = id_p2 },
       { .key = "j", .name = "p2_right", .device = id_p2 },
   };
   ```

   A choice with `device` 0 takes any keyboard. Choose the ids at run
   time, for example by asking each participant to press a key at the
   start and reading `in.device`.
3. Use `persist` with `max_responses` 2 for one response from each, or one
   collector per participant.

Limits:
- Keys on the message path have device 0 and match every keyboard: keep
  SDL text input off during the trials (`caps.raw_keyboard` says so).
  Injected keys (SendInput, remote desktop) also have device 0.
- When both participants hold the same key at once, SDL's shared key
  state drops the first one's release (see "The double report").
- Mice: in SDL's absolute mode every mouse is device 0, one pointer, with
  message times (tier 3). For a mouse per participant, see the next
  section.
- The abort key (Shift+Esc) works from any keyboard, on purpose.

### Give each participant a mouse

`ysp/screen.h` v0.3.5 reads every mouse's Raw Input on its own thread when
asked (`desc.raw_mice`, Windows): each report carries the mouse's device
id and is stamped when read, like the raw keyboard. The system cursor,
the operator's console and ImGui keep working.

1. Open the screen with `desc.raw_mice = true` and `desc.ring` (the ring
   gets the device list and the reader's state).
2. Find the ids: `screen_input --mice` prints each click with its mouse and
   ends with one line per mouse, with its name. This separation is
   verified with injected input only: run `screen_input --mice` with two
   physical mice before you rely on it.
3. Each frame, one loop for every input (`ysp/screen.h` v0.4.0's input
   bridge: the raw mouse events arrive as SDL events, decoded by
   `yscr_event_input()`, with the keyboard's in the same stream):

   ```c
   SDL_Event ev;
   yrsp_input in;
   while (yscr_poll(&scr, &ev, NULL)) {
       if (!yscr_event_input(&scr, &ev, &in)) continue;
       yrsp_feed(&rsp, &in);
       yrsp_cursor_feed(&cursor_p1, &in);   /* if a cursor is drawn */
   }
   ```

   `yscr_poll_mouse()` (v0.3.5) still works in v0.4.0 but is
   deprecated.

4. Make each choice a button of one mouse: `{ .name = "p1", .kind =
   YRSP_KIND_MOUSE, .device = id_p1, .control = 1 }` (1 left, 2 middle,
   3 right). Add `yrsp_raw_mouse_source()` to `desc.sources`.
5. A drawn cursor per participant: `yrsp_cursor` with `device`, `gain`
   (pixels per count) and the area. The movement is unaccelerated, so it
   does not follow the system cursor's ballistics. State the gain in the
   data.

Reports from devices SDL does not list carry `YRSP_IN_UNLISTED`.
Device 0 is every pointer Windows synthesizes itself: a precision
touchpad's cursor, injected input, remote desktop. If
other code turns on SDL's relative mouse mode, the raw reports stop until
it ends; the ring says so (`YSCR_EV_RAW_MICE`).

### Use a gamepad trigger as the response

1. Open the screen with `desc.gamepads = true` (ysp/screen.h v0.3.3), and
   read events with `yscr_poll()`.
2. Make the choice a crossing:

   ```c
   static const yrsp_choice trig[] = {
       { .name = "trigger", .kind = YRSP_KIND_GAMEPAD, .cross = YRSP_CROSS_RISING,
         .control = YRSP_AXIS_GAMEPAD + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
         .level = 0.5f, .hysteresis = 0.2f } };
   ```

The gamepad's timing is not measured. XInput pads are polled.

### Measure RT from a sound

Give the sound's onset record to the collector:

```c
yau_onset rec;            /* from ysp/audio.h */
yrsp_onset o = yrsp_onset_audio(&rec);
yrsp_set_onset(&rsp, &o);
```

The onset is the device-clock fit's time, tier 2 at best (ysp/audio.h).

### Detect pen pressure onset or mouse movement onset

- Pen: a RISING crossing on `YRSP_KIND_PEN`, control
  `YRSP_AXIS_PEN_PRESSURE`, level about 0.05.
- Mouse: a DISTANCE crossing on `YRSP_KIND_MOUSE`, control
  `YRSP_AXIS_POSITION`, level in window coordinates. The distance is
  measured from the position at `yrsp_arm()`.

Add a trace (`desc.trace`, `desc.trace_cap`) to keep every sample of
the window for a mouse-tracking or drawing task.

### Check that your keyboard reports each key once

Run this command, click its window, and press keys for 30 s:

```sh
screen_input --hand
```

It prints one line for each press and, at the end, a summary:

```text
N presses, 0 second reports within 50 ms, M OS repeats; reports with a device id N, without 0
RESULT: one report per press
```

`RESULT: some keys reported twice` means that SDL sends those keys from
both paths on this machine. `yscr_event_input()` drops the second reports
and counts them, but tell the maintainers: it is not what was expected.

`which` is the keyboard's device id on SDL's raw path, and 0 on the
message path. A second report from the message path shows `which=0`.

## Reference

The header's comment block lists every field and flag. In short:

| Call | Does |
|---|---|
| `yrsp_open(&c, &desc)` | Validates and copies the rules. No allocation after it. |
| `yrsp_close(&c)` | Ends the use; safe on a zeroed collector and twice. |
| `yrsp_arm(&c, t)` | Opens a window; keeps which keys are down. |
| `yrsp_set_onset(&c, &o)` | Sets or refines the onset; classifies again. |
| `yrsp_feed(&c, &in)` | One input event, armed or not. |
| `yrsp_update(&c, now_ns)` | The deadline, and `max_hold_ns` while waiting for a release. |
| `yrsp_finish(&c, &r)` | The result. Call it again after a late release or onset. |
| `yrsp_entries(&c, &n)` | Every press of the window. |
| `yrsp_from_sdl`, `yrsp_sdl_source` | SDL3 events and their source entries (inline, with SDL3). Without the bridge's filter: see "The double report". |
| `yrsp_onset_flip`, `_audio`, `_landing` | Onsets from ysp/screen.h, ysp/audio.h, ysp/timeline.h records (inline). |
| `yrsp_plan_frame(&c)` | The frame whose flip record makes the onset final; -1 when there is none. |
| `yrsp_update_frame(&c, &f)` | That frame's record from `f.done` as the final onset, then `yrsp_update()` (inline, with ysp/screen.h). |
| `yrsp_feed_screen(&s, c, n)` | Every input event on the screen's queue to n collectors (inline, with ysp/screen.h and SDL3). |
| `yrsp_set_landing(&c, fired, n, ch)` | The plan from the ONSET event of channel `ch` (inline, with ysp/timeline.h). |
| `YRSP_DATA_COLS`, `yrsp_data_set(&w, &r)` | The result's columns for ysp/trials.h's data writer (inline, with ysp/trials.h). |

jsPsych option to ysp field:

| jsPsych | ysp/response.h | Difference |
|---|---|---|
| `choices` | `desc.choices` (`key`, `control`, `code`) | Physical key by default; keycode match on request |
| `"ALL_KEYS"` | `mode = YRSP_CHOICES_ALL` (the zero default) | `kinds` 0 is the keyboard only |
| `"NO_KEYS"` | `mode = YRSP_CHOICES_NONE` | Presses are logged |
| `trial_duration` | `desc.duration_ns`, from the onset | ns; from the stimulus, not the trial start |
| `response_ends_trial: false`, `persist` | `desc.persist`, `desc.max_responses` | |
| `minimum_valid_rt` | `desc.min_rt_ns` | Earlier presses are logged as anticipations, not dropped |
| `allow_held_key` | `desc.allow_held_key` | Same rule; held responses are flagged |
| `wait_for_key_release` | `desc.wait_for_key_release`, `desc.max_hold_ns` | Gives up after `max_hold_ns` |
| `rt` | `result.rt_ns` | ns; from the flip onset, with its tier; `YRSP_NO_TIME` for null |
| `rt_key_duration` | `result.rt_key_duration_ns` | ns; from the response's press |

## Decisions

### RT starts at the flip onset

jsPsych measures `rt` from the call that registers the key listener
(`performance.now()`, `KeyboardListenerAPI.ts`). That moment is not the
stimulus's screen time. ysp measures from the flip record's onset of the
frame that showed the stimulus, and the row carries that onset's tier and
the landing residual. When only a plan is known, the result says so
(`ONSET_PLAN`).

The record arrives a frame or more after the onset. So the collector
keeps absolute times and classifies again at each `set_onset()`. A press
that ended the trial live but is an anticipation against the final onset
gets `ENDED_EARLY` in the result. The caller decides what to do (for
example, re-queue the trial).

### Physical keys by default

The coordinator chose scancode matching on 2026-10-07: "the F and J keys"
means positions, and positions do not move with the layout. Key names
resolve to US-layout scancodes at `yrsp_open()`. Keycode matching is an
option. Every entry logs both.

### `kinds` 0 is the keyboard only

In ALL mode, `kinds` 0 takes keyboard presses only. A click that brings
the window to the front must not end a keyboard trial. This breaks "zero
means all" on purpose. `YRSP_KINDS_ALL` takes every kind but SYNC
(v0.1.4, a default the user adopted on 2026-10-08, `docs/devices_spec.md`
section 16): a scanner pulse or a photodiode edge in an ALL_KEYS trial is
logged and does not end it. A mask that names the SYNC bit
(`YRSP_KIND_BIT(YRSP_KIND_SYNC)`), or a LIST choice of kind SYNC, takes
SYNC events: that is how a trial waits for the scanner.

### Held keys follow jsPsych

Read in jsPsych's `KeyboardListenerAPI.ts` (`main`, 2026-10-07): the
listener keeps a set of held keys, adds a key at its keydown and removes
it at its keyup. With `allow_held_key` false, a keydown of a key in the
set is not a response. So the OS repeats of a key held from before the
window never count. With true, every keydown counts, an OS repeat too.
ysp does the same and flags a held response `HELD`. Differences:

- jsPsych drops a press before `minimum_valid_rt`; ysp logs it as an
  anticipation.
- jsPsych keys the held set by `KeyboardEvent.key`; ysp by scancode, so a
  Shift change during a hold does not make a new key.
- jsPsych releases every key when the window loses focus. ysp waits for
  SDL's key-up events.
- jsPsych's `rt_key_duration` runs from the key's first keydown; ysp's
  from the response's press. They differ only for a held response.
- With `persist` and `wait_for_key_release`, jsPsych reports only the
  newest press's release. ysp ends at the release of the press that
  reached `max_responses`.

### The double report

SDL 3.4.0's Windows code sends a key from the window message only when
the raw keyboard is off, text input is on, or the message has no scan
code (`virtual_key`). With the raw path on, a key whose message has no
scan code arrives two times: from the raw-input thread and from the
message loop.

Measured with `screen_input --reports` (F24 injected while the window had
the focus; 20, 50, 50, 199 and 199 presses per row in five runs, 518
taps in all):

| Injection | Hold | Reports per press | Second report after the first |
|---|---|---|---|
| Virtual key (`wVk`, scan code 0) | tap | 2 press-release pairs | -0.1 to 34.2 ms; 517 of 518 within 17.1 ms |
| Virtual key | 100 ms | 1 press, then 1 repeat | |
| Scan code (`KEYEVENTF_SCANCODE`) | tap | 1 | |
| Scan code | 100 ms | 1 | |

Before this probe, `docs/screen.md` said that SDL 3.4 reports each
key two times. The probes that found it injected by virtual key only.
A physical key carries a scan code, so it most likely reports once.
`screen_input --hand` lets a user check keys pressed by hand; this was
not run here.

The rule (R3 in v0.1): a press of the same key within 50 ms of the last
kept press, in either time order, is a second report. The first report to
arrive is kept. 50 ms covers the largest measured gap (34.2 ms). Pressing
one key two times within 50 ms is not a response a person makes.

v0.2.0 moved the rule out of the collector into `ysp/screen.h` v0.4.1's
input bridge (`yscr_event_input()`). In v0.1 only the collector got it:
`trial_adjustment` stepped its bar from the same events, so a
virtual-key tap stepped two times. The bridge also drops the second
report's key-up, so a program sees one key-down and one key-up per
press. `ysp/screen.h`'s manual (INPUT, "Second key reports") has the rule,
its counts (`yscr_get_input_stats()`), its ring record
(`YSCR_EV_KEY_DOUBLE`) and the opt-out (`desc.key_dedup_ns` < 0). A
program that feeds `yrsp_from_sdl()` itself, or any other producer of
key events that are not from the bridge, must drop second reports before
`yrsp_feed()`: call `yscr_key_filter()` when `ysp/screen.h` is there, or
apply the rule of the manual's REPEATS, HELD KEYS AND DOUBLE REPORTS. The
collector itself takes a second report as a new press: AFTER_END when the
first ended the window, a second response with `persist`.

v0.1.1 adds the device to the rule. On the raw path, SDL gives each key
the Raw Input handle of its keyboard (`which`); the message path gives 0.
SendInput gave 0 on both paths: every report in the table above had
`which` 0, because injected input has no device handle. So two reports
merge only when their devices are equal or one of them is 0. Two
different keyboards pressing one key within 50 ms are two presses (two
participants, or two hands on two boxes). The collector uses the same
device rule to decide whether a key is down (R2) and which press a release
closes.

SDL 3.4 keeps one key state for all keyboards. While keyboard A holds F,
keyboard B's press of F arrives marked as a repeat, and B's release ends
SDL's state, so SDL drops A's release later. The collector takes B's
"repeat" for B's first press, because only A holds the key; A's press
then has no release (`rt_key_duration` empty).

`ysp/screen.h` merged abort presses within 30 ms. One of the 518
measured gaps (34.2 ms) is longer, so two Shift+Esc presses through a
remote-desktop or assistive tool (they inject by virtual key) could count
as four and trigger the panic watchdog. v0.3.3 merges within 50 ms too.

### No interpolation at a crossing

SDL sends an axis value when it changes, so the signal is a step. The
crossing time is the time of the first sample at or past the level.

### No combined uncertainty

The result carries the onset's tier and the response stamp's tier and
bounds side by side. The two rest on different evidence (an OS vblank
time, a measured host-side stamp), and a sum would hide which part is
unknown.

## What the examples show

Status: 2026-10-08, the seven programs in `examples/response/`; on
2026-10-09, five more examples that use the header (the last five rows). The
question was whether `ysp/response.h` does work that plain SDL events
would leave to each program. This section gives the facts per example. It
does not recommend.

How the lines were counted: "trial logic" is the non-blank, non-comment
lines of the frame loop in `main()` and of its input helpers (`adjust()`,
`pointer()`, `inside()`), without the `--sim` code. "yrsp lines" is the
lines of that logic that name a `yrsp_` function or type. Setup (window,
conditions, the collector's description) and the data file's header are
not counted.
The last five rows were counted by a script with this rule: comments and
blank lines out, and each `if`, `for` or `while` whose condition starts
with `sim` out with its body. On the current sources the script gives
exactly the rows of `trial_audio_keyboard` (66, 8) and
`trial_stop_signal` (120, 9), and 101 and 10 for `trial_keyboard`, whose
loop has changed since its row was counted.

| Example | jsPsych | Trial logic | yrsp lines | `ysp/response.h` features used | Plain SDL events would need |
|---|---|---|---|---|---|
| `trial_keyboard` | `html-keyboard-response` | 123 | 12 | Choices by scancode; `duration` from the onset; `minimum_valid_rt`; the onset as a plan, then the flip record; R3 duplicates; release pairing (`rt_key_duration`); counts; the stamp tier | A list of presses kept until the flip record arrives, then classified against it; a deadline that moves with the onset; a table of keys that are down; a second-report filter; release pairing |
| `trial_same_different` | `same-different-html` | 121 | 13 | As `trial_keyboard`, and EARLY: the window opens at the fixation, so presses during the first bar and the gap are logged and counted (`n_early`), not responses | As `trial_keyboard`. The SOA (second onset minus first, from flip records) is the example's own code with or without the header |
| `trial_srt` | `serial-reaction-time` | 100 | 11 | Four choices, no deadline, the first key ends the window; the onset as a plan, then the flip record; presses in the RSI are AFTER_END, because `yrsp_finish()` comes at the end of the RSI (`n_rsi_presses`) | A map from scancode to position; the first key-down at or after the onset, with OS repeats skipped; a counter for key-downs in the RSI. With no deadline and no minimum RT, no press needs a class before the final onset |
| `trial_adjustment` | `reconstruction` | 152 | 16 | The confirm key only: RT from the flip onset, the down state of the key across trials | The confirm key: one scancode test and the repeat flag. The adjustment steps read `ysp/input.h` events in both versions |
| `trial_mouse_tracking` | `extension-mouse-tracking` (MouseTracker style) | 150 | 17 | A DISTANCE crossing (movement onset, 10 px from the position at `yrsp_arm()`); `persist`; the trace (one entry per event); the source's tier (3 for SDL's pointer, 0 for raw mice); `yrsp_cursor` for raw counts; `yrsp_entries()` for an EARLY crossing | A start point and a distance test; an array of samples; the onset kept to subtract. The hit tests, the conversion of raw counts to positions and the choice between SDL's and the bridge's mouse events are the example's own code in both versions |
| `trial_audio_keyboard` | `audio-keyboard-response` | 66 | 8 | The onset from the tone's record (`yrsp_onset_audio()`: the target time, then the fit's time); `duration` from that onset; EARLY in the foreperiod; anticipations; the onset tier in the row | As `trial_keyboard`, with the record from `yau_result()` in place of the flip record |
| `trial_stop_signal` | contrib `plugin-stop-signal` | 120 | 9 | As `trial_keyboard`; TIMEOUT is a successful stop, a response on a signal trial a failed one | As `trial_keyboard`. The SSD as shown (tone record minus go flip onset) and the staircase are the example's code with `ysp/audio.h` and `ysp/stair.h` |
| `trial_2afc_adaptive` | None in the core (a 2AFC keyboard trial) | 91 | 9 | Two arrow keys; the window opens at the gabor's flip and lasts 2.5 s; presses in the foreperiod are EARLY (`n_early`); the onset as a plan, then the flip record, with its landing residual | As `trial_keyboard`. A trial with no response goes back to its track; the contrast shown and the two tracks are the example's code with `ysp/color.h`, `ysp/stair.h` and `ysp/quest.h` |
| `trial_rdk` (`examples/rdk/`) | contrib `plugin-rdk` | 108 | 9 | Two arrow keys; RT from the motion onset's flip record; `duration` 1.5 s; `minimum_valid_rt` 0.1 s (`n_anticipations`) | As `trial_keyboard`. The dots' replay digest and the steps file are `ysp/rdk.h` and the example's code |
| `trial_stroop` | `html-keyboard-response` | 111 | 11 | Three choices by scancode; RT from the word's flip record; `duration` 2 s; anticipations; TIMEOUT and the key choose the practice feedback; the result's flags in the row | As `trial_keyboard`. The words, the feedback text and the order rule are the example's code with `pack/layout`, `ysp/table.h` and `ysp/trials.h` |
| `trial_sternberg` (`examples/timeline/`) | `animation`, then `html-keyboard-response` | 153 | 12 | Two choices; presses during the memory set are EARLY (`n_early`); RT from the probe's flip record; `duration` 2 s; anticipations; the flags | As `trial_same_different`. The op table and the SOAs from the digits' flip records are the example's code with `ysp/timeline.h` |
| `trial_images` (`examples/pack/`) | `image-keyboard-response` | 105 | 9 | Two choices; RT from the image's flip record; `duration` 2.5 s; anticipations; the flags | As `trial_keyboard`. The blocks and the textures from the pack are the example's code with `ysp/trials.h` and `ysp/pack.h` |

### Where the header removed work

- A late onset. In each example with a response window, presses can
  arrive before the final onset is known: the flip record completes one
  frame or more after the onset, and the tone's record about a buffer after
  the tone starts. The collector keeps absolute times and classifies every
  press again at each `yrsp_set_onset()`, and it ends the window live
  (`yrsp_update()`) against the onset it has.
- Presses that are not responses are data. `n_early` (presses from the
  fixation to the second bar in `trial_same_different`: 2, 1, 1 and 0 in
  the four trials of the window run), `n_anticipations`, and AFTER_END
  presses (the RSI of `trial_srt`) are each one field of the result.
- Tiers in the row. Each row has the onset's tier and source and the
  response stamp's tier. In `trial_mouse_tracking` the movement onset has
  tier 3 on SDL's pointer (window message times) and the raw mouse
  source's tier (0, not measured end to end) with `--raw-mice`.

### Where the header added nothing or got in the way

- `trial_srt` has no deadline, no minimum RT and no release rule. What is
  left for the header is the choice table and the RSI count.
- `trial_adjustment`: the collector models one window that ends at a
  response, and a stream of steps does not fit it. Its log holds 64
  entries per window, 8 of them kept for choices, so more than 56 taps of
  G or H in one trial set LOG_FULL (the confirm still fits; OS repeats are
  counted, not logged). The double-report rule (R3) worked only inside the
  collector, so the steps did not get it: a key that SDL reports two times
  (a virtual-key tap, "The double report") stepped two times. Fixed in
  v0.2.0: the rule is in the input bridge (see the next section).
- `trial_mouse_tracking`: the collector has no spatial choices, and an
  entry has no position. The example tests the click's position itself
  (`ygfx_hit()` of an outlined box is its band only, so it uses
  `ygfx_bounds()`). With raw mice the example integrates the counts
  (`yrsp_cursor`) and rewrites each DELTA sample as a POSITION sample,
  because DISTANCE reads positions. SDL's own mouse events still arrive
  with raw mice on, so the example drops the events that do not come from
  the input bridge. The trace stops at `yrsp_finish()`, so the example
  calls it at the response click and again at the end of the trial for
  the final onset. A movement of 10 px between the start click and the
  disc's onset makes the crossing EARLY, and the channel arms again only
  within 10 px of the start: that trial has no movement onset
  (`early_move` 1). `n_early` counts the start click too, so the example
  reads the entries for this flag.
- One onset per window. The first bar (`trial_same_different`) and the
  stop signal (`trial_stop_signal`) are second onsets; each example reads
  their records itself.
- Held keys with no deadline. The first synthetic participant of
  `trial_adjustment --sim` pressed space with no release. The next trial's
  press of space was then a held key (R2), not a response, and with no
  deadline the run did not end. A keyboard sends the release; the rule is
  jsPsych's.
- Not used by any of the seven examples: `wait_for_key_release` and
  `max_hold`, `allow_held_key`, `settle`, KEYCODE matching, RISING and
  FALLING crossings, `yrsp_format_header()` and `yrsp_format_row()`. Each
  example writes its own columns, with jsPsych's names first. The how-to
  guides use the crossings; no example runs them. v0.2.0 removed some of
  these (next section).

### The trim of v0.2.0

On 2026-10-09 the user approved a trim based on the findings above.

| Feature | Decision | Why |
|---|---|---|
| R3, the double-report rule (`desc.dedup`, `YRSP_E_DUPLICATE`, `n_duplicates`) | Moved to `ysp/screen.h` v0.4.1's input bridge | Every consumer of `yscr_event_input()` needs it, not only the collector (`trial_adjustment`'s steps). |
| `yrsp_format_header()`, `yrsp_format_row()` | Removed | No example used them: each writes its own columns, with jsPsych's names first. |
| `desc.settle` | Removed | It only delayed `YRSP_ENDED`. A press stamped before the deadline counts when it is fed after the end, until the next `yrsp_arm()` (the collector classifies by time), and each example's feedback and inter-trial interval give a late report that time. |
| `wait_for_key_release`, `max_hold` | Kept | A jsPsych option that ported designs set; about 20 lines; tests and mutants cover it. |
| `allow_held_key` | Kept | A jsPsych option; 2 lines beyond the held table, which R1 and R2 need anyway. |
| KEYCODE matching | Kept | The only way to match the key a layout types (jsPsych's `KeyboardEvent.key`); about 10 lines. |
| RISING and FALLING crossings | Kept | The how-to guides use them (a gamepad trigger, pen pressure); FALLING is 2 lines beside DISTANCE, which `trial_mouse_tracking` runs. |

Lines: `ysp/response.h` went from 1551 to 1415 lines (136 fewer: 128
lines of code out, 11 in, the rest its manual and changelog).
`ysp/screen.h` went from 7697 to 7860 (163 more: 96 lines of code in,
with the struct fields and the parameter table, 4 out; the rest its
manual and changelog). The filter costs 9.6 ns per keyboard event at p50 (9.1 to 11.0
ns, 15 runs of 1,000,000 events through a full 16-key table; MinGW gcc
16.1 -O2, AC, under the timing guard).

The examples, by `wc -l`: `trial_keyboard.c` 402 to 418,
`trial_same_different.c` 373 to 384, `trial_srt.c` 354 to 365,
`trial_adjustment.c` 415 to 427; the other three did not change. Trial
logic (counted as above, without the `--sim` code): `trial_keyboard` 123
to 123 (2 lines for the key path went, since `yscr_event_input()` reads
it, and 2 came for the per-trial count), `trial_same_different` 121 to
123 and `trial_srt` 100 to 101 (the count of dropped reports per trial:
`n_duplicates` now comes from `yscr_get_input_stats()`),
`trial_adjustment` unchanged (its steps get the filter through
`yscr_event_input()`, which it called already). The `--sim` participant
of `trial_adjustment` now reports one G tap two times, 9 ms apart, through
`yscr_key_filter()`: the bar steps once (5 steps in trial 0, as before).
`trial_keyboard --sim` sends its doubled wrong key through the same
filter.

### The runs

On 2026-10-08, Windows 11, the Iris Xe laptop, MSVC 19.44 (`/W4 /WX`,
`msvc-full`) and MinGW-w64 gcc 16.1 (C11, `-Wall -Wextra -Wpedantic
-Wshadow -Werror`). In a window, an injector sent keys by scan code and
mouse input with SendInput, only while the program's window was in front.

| Example | `--sim`, both compilers | In a window (MSVC) |
|---|---|---|
| `trial_same_different` | 4 trials as scripted | 4 trials; SOA 0.79996 to 0.80000 s from flip records (planned 0.8 s); onset tiers 1 and 2 |
| `trial_srt` | 24 trials as scripted | 24 trials (blocks S and R); onset tier 1 |
| `trial_adjustment` | 3 trials as scripted | 3 trials; steps in the trajectory file |
| `trial_mouse_tracking` | 2 trials as scripted, SDL's pointer and `--raw-mice` | SDL's pointer: 34 and 37 samples of 40 injected moves 10 ms apart, movement onset tier 3. `--raw-mice`: 40 and 40 samples, tier 0. The first two of eight `--raw-mice` runs lost the focus after the first click and got no more input (one before and one after the example confined the cursor); the cause was not found. The example confines the hidden system cursor to its window (`SDL_SetWindowMouseGrab()`), since a click goes to the window under that cursor |
| `trial_audio_keyboard` | 4 trials as scripted; tone tier 4 (null device) | 4 trials; WASAPI shared, tone tier 2, residuals within 6 us, every onset confirmed |
| `trial_stop_signal` | 8 trials as scripted; SSD 0.25 then 0.30 s | 8 trials; SSD as shown within 2 us of the plan (0.249998 and 0.199999 s); go onset tiers 1 and 2, tone tier 2 |

## Times in ns, open and close (v0.3.0)

v0.3.0 applies the repository conventions (README.md, Conventions).
Three decisions:

- **Every time is `int64_t` ns.** The desc had seconds as doubles
  (`duration`, `minimum_valid_rt`, `max_hold`), and the result had RT and
  the hold duration as doubles with NaN for none. Now they are
  `duration_ns`, `min_rt_ns`, `max_hold_ns`, `rt_ns` and
  `rt_key_duration_ns`. Old code that wrote `.duration = 1.5` now fails
  to compile, and it does not truncate. The collector already worked in
  ns inside, so no rule changes. The seconds-to-ns rounding moved to the
  caller (`YRT_S()` rounds to the nearest ns, as `llround` did). The
  source bounds are `stamp_lo_ns` and `stamp_hi_ns`, from `ysp/input.h`
  v0.4.0. `yrsp_onset`'s fields are `t_ns`, `residual_ns` and
  `uncertainty_ns`, because a caller can fill one.
- **"None" is `YRSP_NO_TIME` (`INT64_MIN`), not -1.** A response's RT is
  never negative, but a hold duration can be: a release on a path that
  stamps earlier than its press's path. `INT64_MIN` cannot be a real
  difference of two clock times, and `YRT_TO_S()` of it prints as an
  obvious error. The examples write an empty CSV cell for it, as they
  did for NaN, and their columns stay in seconds.
- **`yrsp_open()` and `yrsp_close()`.** `yrsp_init()` took a desc and
  checked it, which is what open means. Close marks the collector closed;
  it owns nothing. Every failing call writes `yrsp_error()`.

Checked on 2026-10-10 (Windows 11, MinGW-w64 gcc 16.1 and MSVC 19.44):
`tests/adapt/response_test.c` passes 20,595 checks (20,660 with
`YRSP_TEST_SDL`), with a new test for close and the messages. The USAGE
block compiles as C99 and C++17 against `ysp/screen.h` and
`ysp/timeline.h`. The six mutants whose anchors moved and `conv-01`
(no message) and `conv-02` (close keeps the collector open) are caught;
`w-06` now checks the range of a desc duration, because the rounding it
checked moved to the caller.

## The first trial (v0.4.0)

On 2026-10-10 the user approved item D of the usability review: make the
first trial short. The smallest trial with a fixation, a stimulus,
keyboard RT and saved data (`trial_keyboard.c`) had 256 lines of code
without its `--sim` participant, and 12 of 13 trial examples had a copy of
the same plumbing: arm at the fixation, take the planned onset when the
stimulus lands, replace it with the flip record's onset when the record
arrives, feed every input event, write the row by hand.

### The helpers and where they live

| Helper | Header | Why there |
|---|---|---|
| `yrsp_set_landing()` | `ysp/response.h`, inline with `ysp/timeline.h` | It reads a `ytl_event` and calls `yrsp_set_onset()`. `ysp/response.h` must not include `ysp/timeline.h` (both are pure computation), so it appears when the timeline came first, as `yrsp_onset_landing()` does since v0.1. |
| `yrsp_update_frame()` | `ysp/response.h`, inline with `ysp/screen.h` | It reads `yscr_frame.done` and calls `yrsp_onset_flip()`, `yrsp_set_onset()` and `yrsp_update()`. The same rule. |
| `yrsp_feed_screen()` | `ysp/response.h`, inline with `ysp/screen.h` and SDL3 | `yscr_poll()` needs a complete `SDL_Event`. A file that compiles `ysp/screen.h`'s implementation has it. |
| `yrsp_plan_frame()` | `ysp/response.h`, a function | The frame the collector waits for. A function, so the test reaches it without a screen, and a program with its own record source (`yscr_on_flip()`) can use it. |
| `YRSP_DATA_COLS`, `yrsp_data_set()` | `ysp/response.h`, inline with `ysp/trials.h` | The result's fields are this header's. `ysp/trials.h` includes only `ysp/table.h` and must not know a collector. |
| `ygfx_open_screen()`, `ygfx_show_until_key()` | `ysp/gfx.h` | They need the screen and the gfx, which `ysp/gfx.h` has. |
| `ygfx_text_make()` | `ysp/gfx.h`, with `ysp/outline.h` first | The font reader and the curve sets are `ysp/outline.h`'s, which is pure computation; `ysp/gfx.h` builds on `ysp/screen.h` and `ysp/color.h` only, so the text appears when `ysp/outline.h` came first ([conventions.md](conventions.md), Dependencies). |

Every helper fits an existing header under the dependency rules, so
there is no new header. The inline helpers call this header's functions
only: a frame loop with them makes the same calls as the loop written out
(the USAGE block has both forms).

Timing stays visible in the row. `YRSP_DATA_COLS` writes the onset's
source, tier and residual and the landing residual in every row, so a row
whose RT came from a plan says so. The collector now keeps its first plan
beside the final onset (`result.plan_residual`), because the final onset
replaced the landing residual that `trial_keyboard.c` wrote before.

Not done, and why:

- One call for the input, the record and the deadline. The screen's
  queue has more than one reader: `trial_adjustment` steps a value per
  key, `trial_mouse_tracking` routes the bridge's events and SDL's, and
  `trial_two_keyboards` routes by device. Those keep their own loop and
  use `yrsp_update_frame()` alone.
- `YRSP_ENDED` once. The status stays `YRSP_ENDED` until the next
  `yrsp_arm()`, as before; the examples keep their `responding` flag.
- A new thin header for the first trial. Not needed: see the table.

### The examples

`trial_keyboard.c` uses every helper, `ysp/trials.h`'s data writer and an
instruction screen. Its columns are a superset of v0.3.0's: the
self-check of `--sim` reads the file back and finds each one. The other
examples in `examples/response/` (not `trial_two_keyboards.c`) use the
helpers where they fit:

| Example | total | code | loop | Helpers used | Data writer |
|---|---|---|---|---|---|
| `trial_keyboard` | 419 to 371 | 256 to 184 | 96 to 61 | all, and the instruction screen | yes |
| `trial_same_different` | 397 to 363 | 253 to 213 | 97 to 78 | open, feed, update, landing | yes |
| `trial_srt` | 366 to 338 | 237 to 211 | 79 to 66 | open, feed, update, landing | no |
| `trial_adjustment` | 428 to 396 | 248 to 217 | 86 to 78 | open, update, landing | no |
| `trial_mouse_tracking` | 496 to 482 | 315 to 300 | 119 to 110 | open, update, landing | no |
| `trial_audio_keyboard` | 333 to 312 | 196 to 169 | 63 to 53 | open, feed | yes |
| `trial_stop_signal` | 422 to 393 | 297 to 261 | 116 to 100 | open, feed, update, landing | yes |
| `trial_2afc_adaptive` | 512 to 501 | 345 to 327 | 90 to 81 | open, feed, update, landing | yes |
| `trial_stroop` | 510 to 492 | 353 to 333 | 103 to 90 | open, feed, update, landing | no |

The counts: `total` is `wc -l`; `code` is the non-blank, non-comment
lines without the `--sim` participant (a line that names a `sim` variable
goes, with the block it opens); `loop` is the same inside the frame loop.
Part of `code` is the setup's descs, now compound literals.

The data writer is in `trial_keyboard`, `trial_same_different`,
`trial_audio_keyboard`, `trial_stop_signal` and `trial_2afc_adaptive`.
Not in the others: `trial_srt` opens a new trial list for each block,
which a writer bound to one session does not follow; `trial_adjustment`
and `trial_mouse_tracking` write a trajectory file beside the trial file
and their columns are mostly their own; `trial_stroop` writes its rules
text and the layout's stamp, and its conditions file can change the
session's columns. Their files keep v0.3.0's format.

### Measured: no flip moved

`trial_keyboard` before (v0.3.0 example and headers) and after, built
the same way (MSVC 19.44 `/O2`, a wrapper that logs every flip record and
the frame thread's time from `yscr_begin()` returning to `yscr_flip()`
being called), 20 trials with no key (each trial times out), two runs of
each, interleaved, under the timing guard. Windows 11, the Iris Xe laptop,
1920 x 1200 at 60 Hz, AC, 2026-10-10. The first 120 frames of each run
are left out (the wrapper takes the foreground there); they had no late
frame either, but for one in a before run.

| Run | Frames | Late | Late target | Early | Tier | Interval mean, sd, max (ms) | CPU p50, p99, max (ms) |
|---|---|---|---|---|---|---|---|
| window, before 1 | 4055 | 0 | 0 | 0 | 1 | 16.6664, 0.0008, 16.672 | 0.144, 0.418, 1.150 |
| window, before 2 | 4056 | 0 | 0 | 0 | 1 | 16.6664, 0.0008, 16.669 | 0.118, 0.272, 1.882 |
| window, after 1 | 4056 | 0 | 0 | 0 | 1 | 16.6664, 0.0008, 16.673 | 0.128, 0.261, 0.922 |
| window, after 2 | 4055 | 0 | 0 | 0 | 1 | 16.6664, 0.0009, 16.675 | 0.119, 0.268, 2.023 |
| fullscreen, before 1 | 4060 | 0 | 0 | 0 | 1 | 16.6664, 0.0008, 16.670 | 0.113, 0.211, 0.379 |
| fullscreen, before 2 | 4060 | 0 | 0 | 0 | 1 | 16.6664, 0.0003, 16.672 | 0.138, 0.310, 2.130 |
| fullscreen, after 1 | 4059 | 0 | 0 | 0 | 1 | 16.6664, 0.0009, 16.672 | 0.125, 0.387, 1.661 |
| fullscreen, after 2 | 4060 | 0 | 0 | 0 | 1 | 16.6664, 0.0003, 16.674 | 0.131, 0.326, 0.956 |

No flip moved: no late, early or skipped frame in any run, every record
tier 1 on the independent-flip path, and the frame thread's median time
within the spread of the before runs (0.113 to 0.144 ms before, 0.119 to
0.131 ms after). A first try of the window runs ran with the window in
the background: DWM showed it at 15 Hz and every record was ESTIMATED, in
both builds. A window that does not get the foreground is not a timing
run.

### Tests and mutations

Checked on 2026-10-11 with MSVC 19.44 (`msvc-full`) and MinGW-w64 gcc
16.1 (the `mingw` preset with SDL3 and the examples):

- `tests/adapt/response_test.c`: 20,644 checks pass (20,709 with
  `YRSP_TEST_SDL`). New: the plan kept beside the final onset, a
  never-shown frame's record, `yrsp_plan_frame()`, `yrsp_set_landing()`
  on a filled `ytl_event` array, `yrsp_update_frame()` on filled
  `yscr_frame` records (the record of another frame ignored, the deadline
  from `f.onset`), and the bytes `yrsp_data_set()` gives the data writer,
  with and without an onset.
- `tests/compile/response.c` and `.cpp` run the helpers with
  `ysp/screen.h` and `ysp/timeline.h` included for their structs;
  `tests/compile/gfx.c` and `.cpp` include `ysp/outline.h` first and run
  `ygfx_show_until_key()`, a text whose font does not open and
  `ygfx_open_screen()` on the simulated display.
- Mutations: `fl-01` to `fl-08` in `tests/mutate/response.toml`, 8 of 8
  caught (and `o-01`, whose anchor moved); `ft-01` to `ft-05` in
  `tests/mutate/gfx.toml`, 5 of 5 caught.
- The `--sim` runs of the nine converted examples pass on both compilers.
  `trial_2afc_adaptive --sim` failed once of four while other builds
  loaded the machine: one onset a frame after its plan, which the RT
  check does not allow for (as before this change).

Not checked: `yrsp_feed_screen()` outside the examples (no test has SDL
input; the key path of `ygfx_show_until_key()` was checked with pushed
SDL events, docs/gfx.md "v0.15"), a
key pressed by hand, Linux and macOS (CI runs the tests and the
`--sim` runs there; without DejaVu Sans or Liberation Sans the
instruction screen of `trial_keyboard` is blank and says so).

## Verification

All on 2026-10-07, Windows 11, the Iris Xe laptop of `ysp/screen.h`'s
STATUS, with MinGW-w64 gcc 16.1 (C11, C99, C++17; `-Wall -Wextra
-Wpedantic -Wshadow -Werror`) and MSVC 19.44 (C11, C++17; `/W4 /WX`).

| Check | Result |
|---|---|
| v0.2.0 (2026-10-09) | `tests/adapt/response_test.c`: 20,576 checks pass; 20,641 with `YRSP_TEST_SDL` (MSVC 19.44 `/W4 /WX` and MinGW-w64 gcc 16.1 `-Werror`, through CMake). The double-report cases now check what the collector makes of a second report (AFTER_END, or a second response with `persist`; R2 when it comes before the first's key-up). Mutations: 43 of 43 caught (`w-05`, `d-01` to `d-04`, `f-01` and `f-02` went with the features; the `d-` four became `ysp/screen.h`'s `kd-01` to `kd-07`). The seven examples' `--sim` runs as scripted on both compilers, `trial_mouse_tracking` also with `--raw-mice`. |
| `tests/adapt/response_test.c` | v0.1.4 (2026-10-08): 20,617 checks pass; a SYNC press in ALL mode with `YRSP_KINDS_ALL` is not a response, with the SYNC bit named it is, and a hand-made mask of every bit but one keeps SYNC. v0.1.2: 20,604 checks pass; 20,669 with `YRSP_TEST_SDL`. Raw mice: a report into presses, releases, movement and wheel in order, the cap, absolute devices, a choice on one mouse, the cursor's gain, clamp and device. v0.1.1: 20,545 checks pass; 20,608 with `YRSP_TEST_SDL` (the SDL adapter on `SDL_Event` structs filled by hand, SDL 3.4.0 headers, nothing linked). Devices: two keyboards 5 ms apart (two responses, also when SDL marks the second a repeat), a raw report and its device-0 report (one), releases per device, HELD_AT_OPEN per device |
| Random streams | 20,000 streams of 40 presses, releases and repeats with random onsets, rules and deadlines: one class per entry, every VALID entry eligible, the result's response the earliest VALID entry |
| Mutations (`tests/mutate/response.toml`) | v0.1.4: 50 of 50 caught (`sync-01`, SYNC in ALL mode). v0.1.2: 49 of 49 caught (`ms-01` to `ms-04`; `ms-04` survived until the SDL mouse tier moved into a helper the SDL-free test reaches). v0.1.1: 45 of 45 caught (`dv-01` devices ignored, `dv-02` device 0 not a wildcard, `dv-03` another keyboard's "repeat" held). v0.1.0: 42 of 42 caught. The first run caught 37; the 5 survivors (rounding of seconds to ns, TIMEOUT without a deadline, a repeat whose press was never seen, release pairing, NaN in a row) each got a test. The release one found a fault: a release closed only the newest entry, so with `allow_held_key` a response followed by its HELD repeats had no duration. A release now closes the press and its repeats. |
| `trial_keyboard --sim` | Both compilers: 6 trials as scripted, onset tier 4 (SIM) from the flip records |
| `trial_keyboard` in a window | No keys: 2 timeouts, final onsets of tier 2 (composed) and 1 (independent flip), landing residuals 5 to 49 us. Keys from SendInput (scan codes and virtual-key taps; sent only while the program's window was in front): 6 trials through `yscr_poll()` and `yrsp_from_sdl()`, the virtual-key taps' second reports counted as duplicates. A virtual-key tap's press and release came with the same stamp (one raw-input poll), so its `rt_key_duration` was 0. |
| `screen_input --reports` | The table in "The double report" |

Not run: a key pressed by hand (`screen_input --hand` is the check); Linux
and macOS (CI runs the test and the examples' `--sim` runs there); mouse,
touch, pen and gamepad events from a device (the adapter is checked on
filled structs only); any response box.

## Not done

- Output to a `ysp/rt.h` ring (v0.2, `YRT_SRC_INPUT`).
- Stimulus boxes at each flip for mouse tracking; velocity-based movement
  onset; chords.
- A protocol parser for any response box.
- Timing of mouse, touch, pen and gamepad events. Keyboard timing with a
  physical key.
