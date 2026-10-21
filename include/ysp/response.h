/* ysp/response.h - v0.6.0 - public domain single-header response collector
 *
 *   Turns timestamped input events from any device into the response of a
 *   trial: which key or button, when, measured from the stimulus onset that
 *   the display or the sound card reported, and how good both times are.
 *   The jsPsych keyboard-response options (choices, ALL_KEYS and NO_KEYS,
 *   minimum_valid_rt, response_ends_trial and persist, allow_held_key,
 *   wait_for_key_release, rt_key_duration) on a device-neutral event, plus
 *   threshold crossings on analog channels and a trace buffer.
 *
 *   REQUIRES ysp/input.h beside it: the input event, its kinds and the
 *   source tiers live there, so producers depend on ysp/input.h alone.
 *   This header includes it; copy both files.
 *
 *   Pure computation: no heap, no OS calls, no threads, no file I/O. Times
 *   are int64 ns on the ysp/rt.h clock, passed in. Needs only the C
 *   standard library. C99 is the floor: it builds as C99, C11 and C++17,
 *   and in the C dialect MSVC compiles by default. Small inline helpers
 *   appear when ysp/screen.h, ysp/audio.h, ysp/timeline.h, ysp/trials.h or
 *   SDL3's SDL_events.h was included before it (THE FRAME LOOP, DATA).
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_RESPONSE_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *   Include ysp/screen.h (or ysp/gfx.h), ysp/timeline.h and ysp/trials.h
 *   first where you want the frame-loop and data helpers.
 *
 *   The jsPsych html-keyboard-response trial, choices f and j, a response
 *   window of 1.5 s from the stimulus onset, responses before 0.1 s are
 *   anticipations (examples/response/trial_keyboard.c is the whole program):
 *
 *       static yrsp_collector rsp;
 *       yrsp_choice keys[2] = { 0 };                  // copied at open
 *       keys[0].key = "f";
 *       keys[1].key = "j";
 *       yrsp_desc d = { 0 };
 *       d.choices = keys;
 *       d.n_choices = 2;
 *       d.duration_ns = YRT_S(1.5);                   // jsPsych trial_duration
 *       d.min_rt_ns = YRT_MS(100);                    // minimum_valid_rt
 *       if (!yrsp_open(&rsp, &d)) { fputs(yrsp_error(&rsp), stderr); return 1; }
 *
 *       yrsp_arm(&rsp, f.onset);                      // at the fixation
 *       // every frame, after yscr_begin(&scr, &f):
 *       yrsp_feed_screen(&scr, &rsp, 1);              // every input event
 *       if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED) { ... hide, feedback }
 *       n = ytl_evaluate(&tl, &tf, fired, cap);
 *       yrsp_set_landing(&rsp, fired, n, STIM_ON);    // the plan, when it lands
 *       // after the trial:
 *       yrsp_result r;
 *       if (yrsp_finish(&rsp, &r) < 0) fputs(yrsp_error(&rsp), stderr);
 *       if (r.rt_ns != YRSP_NO_TIME)                  // seconds in the data
 *           printf("rt %.3f s, key %s, onset tier %d\n", YRT_TO_S(r.rt_ns),
 *                  r.response_name, r.onset_tier);
 *       yrsp_data_set(&data, &r);                     // YRSP_DATA_COLS
 *       // at the end of the session:
 *       yrsp_close(&rsp);
 *
 *   yrsp_update_frame() takes the stimulus frame's flip record from f.done
 *   when it arrives, so RT is from the final onset; until then it is from
 *   the plan, and the result says which (onset_src, ONSET_PLAN). Without
 *   ysp/screen.h and ysp/timeline.h, the same in calls of this header:
 *
 *       // the stimulus event landed: its planned onset
 *       yrsp_onset o = yrsp_onset_landing(&fired[i]);
 *       yrsp_set_onset(&rsp, &o);
 *       // its flip record completed (f.done[], a later begin()): the onset
 *       o = yrsp_onset_flip(&f.done[k]);
 *       yrsp_set_onset(&rsp, &o);
 *       if (yrsp_update(&rsp, f.onset) == YRSP_ENDED) { ... }
 *
 *   ---------------------------------------------------------------------
 *   THE EVENT (yrsp_input)
 *   ---------------------------------------------------------------------
 *   yrsp_input is ysp/input.h's yin_event, and every YRSP_KIND_*,
 *   YRSP_PRESS..., YRSP_IN_*, YRSP_AXIS_* and YRSP_TIER_* name is
 *   its YIN_ name (see ysp/input.h for the fields). yrsp_feed() takes
 *   events in arrival order. Their times need not rise: sources stamp
 *   differently (a response box's device clock, SDL's raw and message
 *   paths), and every rule below reads t, not the order.
 *   ---------------------------------------------------------------------
 *   SOURCES AND TIMING QUALITY (yrsp_source, desc.sources)
 *   ---------------------------------------------------------------------
 *   ysp/input.h's yin_source (its SOURCES section): a tier and measured
 *   bounds per kind or device. The result copies the responding source's
 *   tier, bounds and partial flag next to the onset's tier.
 *   ---------------------------------------------------------------------
 *   CHOICES (desc.choices, desc.mode, desc.kinds)
 *   ---------------------------------------------------------------------
 *   A choice is a key or button by kind, device (0 = any) and control,
 *   or an analog crossing (CROSSINGS). Keys match by SCANCODE by default:
 *   the physical key, which is what a lab means by "the F and J keys" and
 *   which a keyboard layout does not move. match = YRSP_MATCH_KEYCODE
 *   matches the layout's key instead (jsPsych's KeyboardEvent.key).
 *   choice.key is a convenience: a key name, resolved at open to the key's
 *   scancode ON THE US LAYOUT ("f" is the key right of D on any keyboard,
 *   which types "f" on QWERTY and AZERTY alike and "u" on Dvorak), or with
 *   KEYCODE to the keycode that name types. Names, case-insensitive: a to
 *   z, 0 to 9, space (or " "), enter (return), escape (esc), backspace,
 *   tab, - = [ ] \ ; ' ` , . /, f1 to f12, arrowleft arrowright arrowup
 *   arrowdown (left right up down). choice.name is the `response` column
 *   (NULL = choice.key). Every entry logs both the scancode and keycode.
 *   desc.mode:
 *     YRSP_CHOICES_ALL (0)   any press of a kind in desc.kinds; jsPsych
 *                              "ALL_KEYS"
 *     YRSP_CHOICES_LIST      the list; n_choices > 0 implies it
 *     YRSP_CHOICES_NONE      nothing is a response; presses are logged.
 *                              jsPsych "NO_KEYS"
 *   desc.kinds, in ALL mode: !!! 0 MEANS THE KEYBOARD ONLY, NOT EVERY KIND
 *   !!! A click that brings a window to the front must not end a keyboard
 *   trial. YRSP_KINDS_ALL takes every kind but SYNC: a scanner pulse
 *   or a photodiode edge is not a participant's response. To take SYNC
 *   events (wait for the scanner), name the bit, YRSP_KIND_BIT(
 *   YRSP_KIND_SYNC), or use a LIST choice of kind SYNC.
 *   YRSP_KIND_BIT(k) takes one kind.
 *
 *   ---------------------------------------------------------------------
 *   THE WINDOW AND RT
 *   ---------------------------------------------------------------------
 *   yrsp_arm(t) starts a window: presses from then on are logged.
 *   yrsp_set_onset() gives the onset that RT is measured from. For each
 *   press, rt = t - onset, and the press is
 *     EARLY         rt < 0, or no onset yet
 *     ANTICIPATION  0 <= rt < min_rt_ns (jsPsych's minimum_valid_rt)
 *     on time       min_rt_ns <= rt < duration_ns (no upper bound when
 *                   duration_ns is 0)
 *     LATE          rt >= duration_ns
 *   An on-time press of a choice is VALID: a response. EARLY, ANTICIPATION
 *   and LATE presses are logged and counted, never dropped, and never end
 *   the window. jsPsych drops a press before minimum_valid_rt; ysp keeps
 *   it, flagged, because anticipations are data.
 *   The window ends (YRSP_ENDED from feed(), set_onset() or update()):
 *     - at the first response, unless desc.persist (jsPsych's
 *       response_ends_trial = false, or persist); with persist, after
 *       desc.max_responses responses (0 = never);
 *     - with desc.wait_for_key_release, at that response's release, or
 *       desc.max_hold_ns (0 = 2 s) after the press;
 *     - at onset + duration_ns, in update(). A press fed after ENDED with t
 *       before the end still counts: a report stamped before the deadline
 *       and delivered after it.
 *   Presses with t at or after the press that ended the window are
 *   AFTER_END: logged, never responses (jsPsych cancels the listener).
 *   The result's response is the VALID entry with the earliest t, not the
 *   first to arrive: a box report read late but stamped by the device's
 *   clock wins over a key press that arrived first and happened later.
 *   RT in the result: rt_ns, YRSP_NO_TIME when there is none (jsPsych's
 *   null); YRT_TO_S(rt_ns) is jsPsych's rt in seconds, a data file's
 *   rt_s (jsPsych's own rt column is in ms, so the name says the unit).
 *   The times it comes from are there too (t_response, t_onset).
 *
 *   ---------------------------------------------------------------------
 *   THE ONSET (yrsp_onset, yrsp_set_onset)
 *   ---------------------------------------------------------------------
 *   The onset is the stimulus's best-known time: the flip record's onset
 *   of the frame the stimulus landed on (ysp/screen.h: the OS's vblank
 *   time, plus onset_offset_ns when a photodiode test set it), or a sound's
 *   onset record (ysp/audio.h: a device-clock fit's time). The record
 *   completes a frame or more after the onset, so give the plan first
 *   (final = 0: ytl_event.onset, the landing frame's predicted onset)
 *   and the record when it completes (final = 1). Each set_onset()
 *   classifies every entry again. A plan does not replace a final onset.
 *   When the final onset differs from the plan (a dropped frame shows the
 *   stimulus a frame later), a press can change class: its entry gets
 *   RECLASSIFIED, and when the press that ended the window is no longer a
 *   response, the result gets ENDED_EARLY: the trial ended on a press that
 *   does not count. The caller decides (re-queue the trial). The result
 *   carries the onset's tier, source (FLIP, AUDIO, PLAN, OTHER), residual,
 *   frame and an uncertainty the caller states (from a loopback; 0 = not
 *   stated: the producers give tiers, not bounds). ONSET_PLAN in the
 *   result: no final onset came, so RT is from a prediction.
 *   The first plan since arm() is kept apart: result.plan_residual is its
 *   residual (a timeline event's landing residual), after the final onset
 *   replaced it. A later plan (a never-shown frame's record) does not
 *   replace it.
 *
 *   ---------------------------------------------------------------------
 *   THE FRAME LOOP (v0.4)
 *   ---------------------------------------------------------------------
 *   Inline helpers that do THE ONSET's steps, and the input's, for a frame
 *   loop on ysp/screen.h and ysp/timeline.h. Each is a few calls of this
 *   header, so the timing is the same as written out (USAGE has both):
 *     yrsp_set_landing(c, fired, n, channel)    with ysp/timeline.h: the
 *         plan from the ONSET event of `channel` among a frame's fired
 *         events; returns its index or -1
 *     yrsp_update_frame(c, &f)                  with ysp/screen.h: the
 *         flip record of yrsp_plan_frame()'s frame, when f.done has it,
 *         as the final onset; then yrsp_update(c, f.onset). Call it every
 *         frame, also after the window ended, so a late record still
 *         becomes the final onset before the last finish()
 *     yrsp_feed_screen(s, c, n)                 with ysp/screen.h and
 *         SDL_events.h: every input event on the screen's queue
 *         (yscr_event_input(): second key reports dropped) to the n
 *         collectors c[0 .. n - 1]; other events are dropped. A program
 *         that reads the events too (an adjustment, a pointer) keeps its
 *         own loop
 *   yrsp_plan_frame(c) is the frame the collector waits for: the plan's
 *   frame while the onset is a plan, -1 after the final onset, for a sound
 *   and before any onset. A frame shown late keeps its index, so its
 *   record is the stimulus's onset with the lateness in its residual
 *   (onset_residual) and the drops in the record. A frame never shown
 *   (SKIPPED) leaves the onset a plan: the row says ONSET_PLAN.
 *
 *   ---------------------------------------------------------------------
 *   DATA (v0.4)
 *   ---------------------------------------------------------------------
 *   With ysp/trials.h included first: YRSP_DATA_COLS, YRSP_N_DATA_COLS
 *   (12) columns for its data writer, and yrsp_data_set(w, &r), which
 *   sets each one the file has from a result, for the next
 *   ytr_data_write(). Columns (an empty field where the result has none);
 *   a column of seconds ends in _s (v0.5, ysp/trials.h DATA FILE): jsPsych
 *   writes rt in ms and PsychoPy in s, and the suffix says which:
 *     response            the choice's name
 *     rt_s                rt_ns
 *     rt_key_duration_s   rt_key_duration_ns
 *     onset_src           yrsp_onset_src: 1 FLIP, 2 AUDIO, 3 PLAN
 *     onset_tier          the onset's tier
 *     onset_residual_s    the onset's residual: a flip record's onset
 *                         minus its target, the frame's lateness
 *     landing_residual_s  plan_residual
 *     onset_frame         the onset's frame
 *     rsp_tier            the response's stamp tier
 *     rsp_flags           YRSP_R_*
 *     n_anticipations, n_early
 *   The onset's source, tier and residual are in every row, so a row whose
 *   RT came from a plan says so. A file that wants fewer declares them by
 *   name with YTR_COL_SET; yrsp_data_set() finds the columns by name.
 *
 *   ---------------------------------------------------------------------
 *   REPEATS, HELD KEYS AND DOUBLE REPORTS
 *   ---------------------------------------------------------------------
 *     R1  A PRESS with YRSP_IN_REPEAT is not a new press, unless only
 *         another device holds the control (DEVICES).
 *     R2  A PRESS of a control that is down (no RELEASE since) on the same
 *         device is not a new press. R1 and R2 are HELD (HELD KEYS).
 *   A second report of one press is the producer's to drop, not the
 *   collector's: SDL 3.4 sends a key with no scan code (virtual-key
 *   injection, some media keys, remote tools) from its raw and its
 *   message path, -0.1 to 34.2 ms apart (518 taps, ysp/screen.h INPUT),
 *   and a program that steps a value per key-down needs the filter as
 *   much as this collector. ysp/screen.h's yscr_event_input() (v0.4.1)
 *   drops them and counts them (yscr_get_input_stats()). A producer that
 *   is not the bridge (yin_from_sdl() in your own loop) must drop, before
 *   yrsp_feed(): a key-down that is not a repeat, of the key of a key-down
 *   kept less than 50 ms before or after it, of the same device or with
 *   either device 0; and the first key-up of that key that finds no kept
 *   key-down still down. With ysp/screen.h, yscr_key_filter() does it.
 *   Fed to the collector, a second report is a new press: AFTER_END when
 *   the first ended the window, a second response with persist.

 *   ---------------------------------------------------------------------
 *   DEVICES (input.device)
 *   ---------------------------------------------------------------------
 *   Two inputs are of one device when their devices are equal or one of
 *   them is 0. SDL 3.4 on Windows gives a key on the raw path the
 *   keyboard's Raw Input handle (each keyboard its own), a key on the
 *   message path 0, and injected input 0 on both paths (measured with
 *   SendInput: every report had 0). So a raw report and its message
 *   report merge, and two keyboards pressing one key never do. Mice in
 *   SDL's absolute mode all come as device 0 (one pointer);
 *   ysp/screen.h's desc.raw_mice gives each mouse its own id (MICE). This rule
 *   applies to R2, releases and HELD_AT_OPEN. SDL keeps one key state
 *   for all keyboards: while keyboard A holds F, keyboard B's press of F
 *   comes marked as a repeat, and B's release ends SDL's state, so SDL
 *   drops A's later release. The collector takes B's "repeat" for B's
 *   press (R1); A's press then has no release. ysp/screen.h logs the
 *   devices with their names (its INPUT section) to map the ids.
 *
 *   ---------------------------------------------------------------------
 *   MICE (yrsp_from_mouse, yrsp_cursor)
 *   ---------------------------------------------------------------------
 *   With ysp/screen.h's desc.raw_mice (v0.3.5, Windows), each report of
 *   yscr_poll_mouse() becomes inputs of YRSP_KIND_MOUSE with the
 *   mouse's device id: presses and releases (buttons 1 left, 2 middle, 3
 *   right, 4 and 5 the side buttons), a YRSP_AXIS_DELTA sample (x, y in
 *   counts, unaccelerated), a YRSP_AXIS_ABSOLUTE sample for a tablet or
 *   remote desktop (x, y 0..1), a YRSP_AXIS_WHEEL sample (value in
 *   notches, x the horizontal wheel). A mouse SDL does not list has
 *   YRSP_IN_UNLISTED. yrsp_raw_mouse_source() is the source entry
 *   (stamped at read, host part measured with injected input only). A
 *   button of one participant's mouse is a choice with kind MOUSE, the
 *   device's id and the button; a movement onset is a DISTANCE choice on
 *   YRSP_AXIS_DELTA (cumulate it with yrsp_cursor first: DISTANCE
 *   reads positions) or a crossing on the cursor's position. yrsp_cursor
 *   is one participant's cursor: gain pixels per count, clamped to an
 *   area, no acceleration, so the gain goes in the data file.
 *
 *   ---------------------------------------------------------------------
 *   HELD KEYS (desc.allow_held_key)
 *   ---------------------------------------------------------------------
 *   The collector tracks which keys and buttons are down from every event
 *   fed, armed or not, so feed events between windows too. jsPsych's rule
 *   (KeyboardListenerAPI.ts, getKeyboardResponse): with allow_held_key
 *   false, a keydown of a key already down (seen down, no keyup since) is
 *   not a response, so a key held from before the window and its OS
 *   repeats never count; with true, every keydown counts, an OS repeat
 *   too, at the repeat's time. ysp follows it: with allow_held_key, a held
 *   press of a choice is logged with HELD and can be a response; its RT is
 *   the OS repeat's time, which the flag says. Without it, held presses
 *   are counted (n_held), not logged. A choice down at arm() sets
 *   HELD_AT_OPEN in the result. Differences from jsPsych: ysp matches by
 *   scancode, so a Shift change while held is the same key (jsPsych keys
 *   its held set on KeyboardEvent.key, its press times on .code); and a
 *   window blur in jsPsych releases every key, where ysp waits for SDL's
 *   key-up events (SDL sends them when the window loses the keyboard
 *   focus; not measured).
 *
 *   ---------------------------------------------------------------------
 *   RELEASES AND HOLD DURATION
 *   ---------------------------------------------------------------------
 *   A RELEASE closes every logged press of the same kind, control and
 *   device (DEVICES) that has none: the press and, with allow_held_key, its
 *   HELD repeats, which are one press. rt_key_duration_ns = release -
 *   press of the response, YRSP_NO_TIME when no release came. A release of a control
 *   that is not down is dropped (counted). Releases pair after ENDED and
 *   after finish() too; call finish() again for the duration. jsPsych's
 *   rt_key_duration runs from the key's first keydown; ysp's from the
 *   response's press, which differ only for a held key's response.
 *
 *   ---------------------------------------------------------------------
 *   CROSSINGS (choice.cross)
 *   ---------------------------------------------------------------------
 *   A choice with cross set reads SAMPLE events of its kind, device and
 *   control: a gamepad trigger or stick past a level, pen pressure onset,
 *   mouse movement onset.
 *     RISING    a sample >= level while armed is a crossing at that
 *               sample's t; armed again when the value drops below level -
 *               hysteresis
 *     FALLING   a sample <= level while armed; armed again above level +
 *               hysteresis
 *     DISTANCE  (x, y) at a distance >= level from where the channel was
 *               at arm() (or its first sample after arm()): movement onset
 *   A channel starts armed (at rest). A channel past its level at arm() is
 *   held: no crossing until it re-arms, whatever allow_held_key says, and
 *   HELD_AT_OPEN is set. No interpolation between samples: SDL sends an
 *   axis when it changes, so the signal is a step, and an interpolated time
 *   would claim a precision the samples do not have.
 *
 *   ---------------------------------------------------------------------
 *   TRACE (desc.trace)
 *   ---------------------------------------------------------------------
 *   Every input of desc.trace_kinds (0 = every kind) fed between arm() and
 *   finish() is copied into desc.trace (the caller's array, 48 bytes each):
 *   pointer, pen, touch and stick traces for mouse-tracking and drawing
 *   tasks. A full trace counts what it drops (TRACE_FULL). Mouse motion at
 *   1 kHz for 2 s is 96 KB.
 *
 *   ---------------------------------------------------------------------
 *   THE RESULT (yrsp_result)
 *   ---------------------------------------------------------------------
 *   jsPsych's names, in ns: rt_ns, response, rt_key_duration_ns. Then the
 *   response's kind, scancode (control), keycode, device and stamp tier
 *   and bounds (stamp_lo_ns, stamp_hi_ns);
 *   the onset's time, tier, source, residual, uncertainty and frame; the
 *   first plan's residual (plan_residual, v0.4); counts; flags (YRSP_R_*).
 *   The program writes the columns it wants, or YRSP_DATA_COLS (DATA).
 *   The entries (yrsp_entries()) hold every press of the window, in
 *   arrival order.
 *
 *   ---------------------------------------------------------------------
 *   SDL3 AND RAW MICE
 *   ---------------------------------------------------------------------
 *   ysp/input.h's adapters (yin_from_sdl, yin_from_mouse) under this
 *   header's old names (yrsp_from_sdl, yrsp_from_mouse): see there.
 *   ---------------------------------------------------------------------
 *   LIMITS, THREADS AND MEMORY
 *   ---------------------------------------------------------------------
 *   YRSP_MAX_CHOICES 32 choices; YRSP_LOG_CAP 64 entries per window,
 *   the last YRSP_LOG_RESERVE (8) of them only for presses of a choice,
 *   so presses of other keys cannot push the response out (LOG_FULL
 *   counts what did not fit); YRSP_MAX_SOURCES 8; 64 controls tracked
 *   as down (per device). The collector is about 6 KB, caller-allocated;
 *   nothing is allocated. One thread: a
 *   reader thread (a serial port, an eye tracker) queues its events, and
 *   the frame thread feeds them.
 *   Errors: yrsp_open() clears the message, and a failed open leaves one.
 *   Every other call that returns an error code (set_onset, feed, update,
 *   finish) writes the message before it returns ("ysp_response:
 *   yrsp_feed: bad argument"), on the failure path only. yrsp_close()
 *   marks the collector closed (it owns nothing) and keeps the message;
 *   it is safe on a zeroed collector, after a failed open, and twice.
 *
 *   ---------------------------------------------------------------------
 *   PARAMETERS
 *   ---------------------------------------------------------------------
 *   yrsp_params(&n) lists the fields of yrsp_desc and of a yrsp_choice
 *   that a definition or a designer sets, so a loader checks its keys and
 *   a designer builds its panel from one source (docs/rig_spec.md,
 *   principle 8). Each row: the field's name (a choice's is
 *   "choices.<field>"), its type ("enum", "bool", "u16", "u32", "i32",
 *   "i64", "f32", "str"), the inclusive range, the value a zero field
 *   means, the unit ("ns" for int64 ns: a definition's x_s for x_ns), a
 *   line of doc, and for an enum its names and values ("all=0,list=1,
 *   none=2"). Not in it: the caller's arrays and their counts (choices
 *   and n_choices as such, sources, trace).
 *
 *   ---------------------------------------------------------------------
 *   NOT DONE
 *   ---------------------------------------------------------------------
 *   Output to a ysp/rt.h ring (v0.2); stimulus boxes at each flip for
 *   mouse tracking; velocity-based movement onset; chords and modifiers as
 *   part of a choice (entries log the modifiers); a protocol parser for any
 *   response box; text entry.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link but the math library. Define YRSP_API to override the
 *   default `extern` linkage.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.6.0 - PARAMETERS: yrsp_params() and yrsp_param, the table of the
 *          desc and choice fields a definition sets, with enum values.
 *          No rule changed.
 *   v0.5.0 - DATA: the columns of seconds end in _s (user decision,
 *          2026-10-20; ysp/trials.h v0.5.0 refuses a TIME_NS column
 *          without it): rt_s, rt_key_duration_s, onset_residual_s,
 *          landing_residual_s (were rt, rt_key_duration, onset_residual,
 *          landing_residual). BREAKING for a reader of those names, and
 *          for a file that declares them with YTR_COL_SET. No rule changed.
 *   v0.4.0 - THE FRAME LOOP and DATA: yrsp_update_frame(),
 *          yrsp_feed_screen(), yrsp_set_landing(), YRSP_DATA_COLS and
 *          yrsp_data_set(), inline where ysp/screen.h, ysp/timeline.h and
 *          ysp/trials.h came first; yrsp_plan_frame(). The collector keeps
 *          the first plan beside the final onset: result.plan_residual (a
 *          new field; the result and the collector grew 8 and 56 bytes).
 *          No rule changed.
 *   v0.3.0 - the repository conventions (README.md). Breaks the API:
 *          yrsp_init() is yrsp_open(), with yrsp_close(). Times are int64_t
 *          ns: desc.min_rt_ns (was minimum_valid_rt, seconds),
 *          duration_ns, max_hold_ns; result.rt_ns and rt_key_duration_ns
 *          (were seconds, NaN for none; now YRSP_NO_TIME), stamp_lo_ns and
 *          stamp_hi_ns (were us); yrsp_onset.t_ns, residual_ns and
 *          uncertainty_ns. The header defines ysp/rt.h's YRT_* macros under
 *          the same guard. Every failing call writes yrsp_error(). Needs
 *          ysp/input.h v0.4.0 (lo_ns, hi_ns). USAGE builds as C99 (C,
 *          { 0 }); the CHANGELOG moved to the end of this comment.
 *   v0.2.0 - Trimmed (docs/response.md, "What the examples show"). The
 *          second-report rule R3 moved to ysp/screen.h v0.4.1's input
 *          bridge, so every consumer of yscr_event_input() gets it, not
 *          only the collector: desc.dedup, YRSP_E_DUPLICATE and
 *          result.n_duplicates are gone (REPEATS, HELD KEYS AND DOUBLE
 *          REPORTS says what a producer that is not the bridge must do).
 *          Also gone, used by no example: yrsp_format_header() and
 *          yrsp_format_row() (each program writes its own columns) and
 *          desc.settle (a press stamped before the deadline counts
 *          whenever it is fed).
 *   v0.1.4 - YRSP_KINDS_ALL in ALL mode takes every kind but SYNC
 *          (ysp/input.h v0.3.0's YIN_KIND_SYNC: a scanner pulse, a TTL
 *          input, a photodiode edge), so a timing event never ends a trial.
 *          A mask that names the SYNC bit, or a LIST choice of kind SYNC,
 *          still takes it. YRSP_KIND_SYNC; the data row names kind 7
 *          "sync". Needs ysp/input.h v0.3.0.
 *   v0.1.3 - The input event, its constants, the source entry and the SDL
 *          and raw mouse adapters moved to ysp/input.h (yin_ names);
 *          this header includes it and keeps every yrsp_ and YRSP_
 *          name as an alias. API and behavior unchanged.
 *   v0.1.2 - Raw mice: yrsp_from_mouse() turns ysp/screen.h v0.3.5's
 *          raw mouse reports (desc.raw_mice) into mouse inputs per device
 *          (axes YRSP_AXIS_DELTA, _ABSOLUTE, _WHEEL), with
 *          yrsp_raw_mouse_source(); yrsp_cursor integrates one mouse's
 *          movement with a stated gain. SDL's mouse events with which 0
 *          (its absolute mode, message-timed) now get tier 3.
 *   v0.1.1 - Devices (DEVICES): two reports merge only when their devices
 *          are equal or one is 0, so two keyboards (or boxes) pressing one
 *          key within 50 ms are two presses; down state and releases are
 *          per device too, and a repeat of a key that only another
 *          keyboard holds is a press (SDL keeps one key state for all
 *          keyboards). The keyboard's 512-bit down table is gone: every
 *          kind uses the held table, now 64 entries.
 *   v0.1.0 - first version: the input event, the source table, choices
 *          (scancode, keycode, key names, ALL, NONE, analog crossings),
 *          the window, onset refinement, double-report removal, held keys,
 *          releases, the trace, the result and its CSV row, the SDL3
 *          adapter.
 *
 *   STATUS: v0.5.0, 2026-10-20: the column names only. The test (20,644
 *   checks; 20,709 with the SDL adapter) passes with MSVC 19.44 and
 *   MinGW-w64 gcc 16.1, on Linux (WSL2) and under emcc; fl-08 to fl-10
 *   caught; the CI --sim lists pass, their files' headers read with _s.
 *   STATUS: v0.4.0, 2026-10-11: the test (20,644 checks; 20,709 with the
 *   SDL adapter) and the compile checks pass with MinGW-w64 gcc 16.1 and
 *   MSVC 19.44; the frame-loop and data helpers are checked on filled
 *   structs (records, fired events, the data writer's bytes); 8 of 8 new
 *   mutants caught. The nine converted examples' --sim runs pass on both
 *   compilers; trial_keyboard moved no flip in a window or fullscreen
 *   (docs/response.md, "The first trial (v0.4.0)"). yrsp_feed_screen()
 *   runs in the examples only.
 *   STATUS: v0.3.0, 2026-10-10: the test (20,595 checks; 20,660 with the
 *   SDL adapter) and the compile checks pass with MinGW-w64 gcc 16.1 and
 *   MSVC 19.44, with new checks of close, the messages and YRSP_NO_TIME;
 *   the USAGE block compiles as C99 (C, { 0 }); every mutant whose anchor
 *   moved, and 2 new ones, caught (docs/response.md, "Times in ns").
 *   v0.2.0, 2026-10-09. Built and run on Windows 11 with MinGW-w64
 *   gcc 16.1 as C11, C99 and C++17 under -Wall -Wextra -Wpedantic -Wshadow
 *   -Werror and with MSVC 19.44 under /W4 /WX as C11 and C++17.
 *   tests/adapt/response_test.c covers every rule on synthetic streams,
 *   with 20,000 random streams against a direct reading of the entries,
 *   and the SDL adapter against SDL 3.4.0's headers (docs/response.md has
 *   the counts). Mutations: every mutant of tests/mutate/response.toml
 *   caught.
 *   examples/response/trial_keyboard.c --sim (simulated display, synthetic
 *   participant) passes its own checks on both compilers; in a window on
 *   the Iris Xe laptop, keys sent with SendInput (scan codes and
 *   virtual-key taps) went through yscr_poll(), yrsp_from_sdl() and
 *   the collector: 6 trials, final onsets from flip records of tier 1
 *   and 2, the virtual-key taps' second reports removed. NOT done: a key
 *   pressed by hand (screen_input --hand is the check); Linux and macOS
 *   (CI runs the test and the example on the simulated display); the
 *   timing of mouse, touch, pen and gamepad events; any response box.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_RESPONSE_H_INCLUDED
#define YSP_RESPONSE_H_INCLUDED

#define YRSP_VERSION_MAJOR 0
#define YRSP_VERSION_MINOR 6
#define YRSP_VERSION_PATCH 0
#define YRSP_VERSION_STRING "0.6.0"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ysp/input.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YRSP_API
#define YRSP_API extern
#endif

#define YRSP_MAX_CHOICES  32
#define YRSP_MAX_SOURCES  8
#define YRSP_LOG_CAP      64
#define YRSP_LOG_RESERVE  8
#define YRSP_MAX_HELD     64

/* Status: what feed(), set_onset() and update() return. */
#define YRSP_IDLE      0   /* not armed, or finished                    */
#define YRSP_OPEN      1   /* armed, not ended                          */
#define YRSP_ENDED     2   /* a response or the deadline ended it       */
#define YRSP_ERR_ARG   (-1)
#define YRSP_ERR_STATE (-2) /* the collector is not open                 */

/* rt_ns and rt_key_duration_ns of a result that has none (jsPsych's
 * null). Not -1: a release stamped on another path can come a little
 * before its press. */
#define YRSP_NO_TIME   INT64_MIN

/* Units, the same block as ysp/rt.h's under the same guard, so one
 * spelling works with or without it: d.duration_ns = YRT_S(1.5). */
#ifndef YRT_S
#define YRT_NS(x)    ((int64_t)(x))
#define YRT_US(x)    ((int64_t)((x) * 1e3 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_MS(x)    ((int64_t)((x) * 1e6 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_S(x)     ((int64_t)((x) * 1e9 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_TO_MS(ns) ((double)(ns) / 1e6)
#define YRT_TO_S(ns)  ((double)(ns) / 1e9)
#endif

/* ysp/input.h's names under this header's prefix: ysp/response.h's API
 * as it was before the split (v0.1.2). */
typedef yin_kind   yrsp_kind;
typedef yin_type   yrsp_type;
typedef yin_tier   yrsp_tier;
typedef yin_event  yrsp_input;
typedef yin_source yrsp_source;
#define YRSP_KIND_KEYBOARD     YIN_KIND_KEYBOARD
#define YRSP_KIND_MOUSE        YIN_KIND_MOUSE
#define YRSP_KIND_TOUCH        YIN_KIND_TOUCH
#define YRSP_KIND_PEN          YIN_KIND_PEN
#define YRSP_KIND_GAMEPAD      YIN_KIND_GAMEPAD
#define YRSP_KIND_BOX          YIN_KIND_BOX
#define YRSP_KIND_EYE          YIN_KIND_EYE
#define YRSP_KIND_SYNC         YIN_KIND_SYNC
#define YRSP_KIND_USER         YIN_KIND_USER
#define YRSP_KIND_BIT(k)       YIN_KIND_BIT(k)
#define YRSP_KINDS_ALL         YIN_KINDS_ALL
#define YRSP_NONE              YIN_NONE
#define YRSP_PRESS             YIN_PRESS
#define YRSP_RELEASE           YIN_RELEASE
#define YRSP_SAMPLE            YIN_SAMPLE
#define YRSP_PROXIMITY         YIN_PROXIMITY
#define YRSP_IN_REPEAT         YIN_REPEAT
#define YRSP_IN_SYNTHETIC      YIN_SYNTHETIC
#define YRSP_IN_ERASER         YIN_ERASER
#define YRSP_IN_UNLISTED       YIN_UNLISTED
#define YRSP_AXIS_POSITION     YIN_AXIS_POSITION
#define YRSP_AXIS_PEN          YIN_AXIS_PEN
#define YRSP_AXIS_PEN_PRESSURE YIN_AXIS_PEN_PRESSURE
#define YRSP_AXIS_GAMEPAD      YIN_AXIS_GAMEPAD
#define YRSP_AXIS_DELTA        YIN_AXIS_DELTA
#define YRSP_AXIS_ABSOLUTE     YIN_AXIS_ABSOLUTE
#define YRSP_AXIS_WHEEL        YIN_AXIS_WHEEL
#define YRSP_TIER_UNKNOWN      YIN_TIER_UNKNOWN
#define YRSP_TIER_1            YIN_TIER_1
#define YRSP_TIER_2            YIN_TIER_2
#define YRSP_TIER_3            YIN_TIER_3
#define YRSP_TIER_SIM          YIN_TIER_SIM
#define yrsp_sdl_mouse_stamp   yin_sdl_mouse_stamp
/* defined by ysp/input.h when SDL3, or ysp/screen.h v0.3.5, came first */
#define yrsp_sdl_ctx           yin_sdl_ctx
#define yrsp_from_sdl          yin_from_sdl
#define yrsp_sdl_source        yin_sdl_source
#define yrsp_from_mouse        yin_from_mouse
#define yrsp_raw_mouse_source  yin_raw_mouse_source

/* One participant's cursor from one mouse's raw movement: each DELTA
 * sample of `device` (0 = any) moves it by gain pixels per count, clamped
 * to [0, w] x [0, h]. No acceleration: the gain is the design's, stated in
 * the data. Absolute devices (ABSOLUTE samples) are not integrated. */
typedef struct yrsp_cursor {
    uint32_t device;
    float    gain;             /* pixels per count; 0 = 1                  */
    float    w, h;             /* the area, pixels                         */
    float    x, y;             /* the position, start it where it belongs  */
} yrsp_cursor;

/* Returns 1 when the sample moved this cursor. */
static inline int yrsp_cursor_feed(yrsp_cursor* c, const yrsp_input* in) {
    float g = c->gain > 0 ? c->gain : 1.0f;
    if (in->kind != YRSP_KIND_MOUSE || in->type != YRSP_SAMPLE || in->control != YRSP_AXIS_DELTA) return 0;
    if (c->device && in->device != c->device) return 0;
    c->x += g * in->x;
    c->y += g * in->y;
    if (c->x < 0) c->x = 0;
    if (c->x > c->w) c->x = c->w;
    if (c->y < 0) c->y = 0;
    if (c->y > c->h) c->y = c->h;
    return 1;
}

typedef enum yrsp_match {
    YRSP_MATCH_SCANCODE = 0, /* the physical key                         */
    YRSP_MATCH_KEYCODE  = 1  /* the layout's key                         */
} yrsp_match;

typedef enum yrsp_cross {
    YRSP_CROSS_NONE     = 0, /* a PRESS                                  */
    YRSP_CROSS_RISING   = 1,
    YRSP_CROSS_FALLING  = 2,
    YRSP_CROSS_DISTANCE = 3
} yrsp_cross;

/* One accepted response. A zeroed choice is keyboard scancode 0, which
 * matches no key: set key, control or code. */
typedef struct yrsp_choice {
    const char* key;           /* a key name (CHOICES), resolved at open   */
    const char* name;          /* the response column; NULL = key          */
    uint8_t     kind;          /* yrsp_kind; 0 = keyboard                */
    uint8_t     match;         /* yrsp_match                             */
    uint8_t     cross;         /* yrsp_cross                             */
    uint8_t     reserved_;
    uint32_t    device;        /* 0 = any                                  */
    uint32_t    control;       /* scancode, button, axis                   */
    uint32_t    code;          /* keycode, with match = KEYCODE            */
    float       level;         /* crossings: the threshold                 */
    float       hysteresis;    /* crossings: re-arm this far back; >= 0    */
} yrsp_choice;

typedef enum yrsp_choices {
    YRSP_CHOICES_ALL  = 0,
    YRSP_CHOICES_LIST = 1,
    YRSP_CHOICES_NONE = 2
} yrsp_choices;

/* The rules, constant over a block. Zero is the jsPsych default trial: any
 * key, no minimum, no deadline, the first press ends it, held keys do not
 * count. Durations in ns (jsPsych's minimum_valid_rt, trial_duration and
 * the like are seconds in a definition: convert at the script layer). */
typedef struct yrsp_desc {
    const yrsp_choice* choices;   /* copied at open                      */
    int       n_choices;            /* > 0 implies LIST                    */
    int       mode;                 /* yrsp_choices                      */
    uint16_t  kinds;                /* ALL mode: 0 = THE KEYBOARD ONLY;
                                     * YRSP_KINDS_ALL = every kind but
                                     * SYNC                                */
    int64_t   min_rt_ns;            /* jsPsych's minimum_valid_rt; earlier
                                     * is ANTICIPATION, logged; >= 0       */
    int64_t   duration_ns;          /* from the onset; 0 = no deadline     */
    bool      persist;              /* the first response does not end it  */
    int       max_responses;        /* with persist: end after this many;
                                     * 0 = never                           */
    bool      allow_held_key;       /* a held key's press can count        */
    bool      wait_for_key_release; /* end at the response's release       */
    int64_t   max_hold_ns;          /* 0 = 2 s                             */
    const yrsp_source* sources;   /* copied at open                      */
    int       n_sources;
    yrsp_input* trace;            /* the caller's array; NULL = none     */
    int       trace_cap;
    uint16_t  trace_kinds;          /* 0 = every kind                      */
} yrsp_desc;

typedef enum yrsp_onset_src {
    YRSP_ONSET_OTHER = 0,
    YRSP_ONSET_FLIP  = 1,         /* yscr_record.onset                 */
    YRSP_ONSET_AUDIO = 2,         /* yau_onset.onset                   */
    YRSP_ONSET_PLAN  = 3          /* a prediction                        */
} yrsp_onset_src;

typedef struct yrsp_onset {
    int64_t t_ns;                   /* on the ysp_rt clock                 */
    int64_t residual_ns;            /* the landing's or the record's       */
    int64_t uncertainty_ns;         /* half-width you state; 0 = none      */
    int64_t frame;                  /* flip index, or -1                   */
    uint8_t tier;                   /* the producer's tier                 */
    uint8_t src;                    /* yrsp_onset_src                    */
    uint8_t final;                  /* 1: a completed record; 0: a plan    */
    uint8_t reserved_[5];
} yrsp_onset;

/* Entry flags */
#define YRSP_E_VALID        0x0001u /* a response                        */
#define YRSP_E_EARLY        0x0002u /* before the onset, or no onset     */
#define YRSP_E_ANTICIPATION 0x0004u /* 0 <= rt < minimum_valid_rt         */
#define YRSP_E_LATE         0x0008u /* rt >= duration                    */
#define YRSP_E_AFTER_END    0x0010u /* at or after the press that ended it */
#define YRSP_E_NOT_CHOICE   0x0020u /* a press that is no choice         */
/*      0x0040u was YRSP_E_DUPLICATE (v0.1); unused                        */
#define YRSP_E_HELD         0x0080u /* a held press, allow_held_key      */
#define YRSP_E_CROSSING     0x0100u /* a threshold crossing              */
#define YRSP_E_RECLASSIFIED 0x0200u /* its class changed after arrival   */
#define YRSP_E_RELEASED     0x0400u /* t_release is set                  */
#define YRSP_E_ENDED        0x0800u /* it ended the window               */
#define YRSP_E_CLASS_MASK   0x000Fu

/* One press or crossing of the window. 48 bytes. */
typedef struct yrsp_entry {
    int64_t  t;                     /* ysp_rt ns                           */
    int64_t  t_release;             /* 0 = none                            */
    uint32_t device;
    uint32_t control;               /* scancode, button, axis              */
    uint32_t code;                  /* keycode                             */
    int16_t  choice;                /* index into choices; -1 = none       */
    uint16_t flags;                 /* YRSP_E_*                          */
    uint8_t  kind;
    uint8_t  stamp;                 /* its tier                            */
    uint16_t mods;
    float    value;
    uint8_t  arrival_class;         /* class bits when it arrived          */
    uint8_t  reserved_[3];
} yrsp_entry;

/* Result flags */
#define YRSP_R_RESPONDED    0x0001u
#define YRSP_R_TIMEOUT      0x0002u /* a deadline, no response           */
#define YRSP_R_ENDED_EARLY  0x0004u /* ended on a press that no longer counts */
#define YRSP_R_ONSET_PLAN   0x0008u /* RT from a plan, no final onset    */
#define YRSP_R_NO_ONSET     0x0010u /* no onset: no RT                   */
#define YRSP_R_LOG_FULL     0x0020u
#define YRSP_R_TRACE_FULL   0x0040u
#define YRSP_R_RECLASSIFIED 0x0080u
#define YRSP_R_HELD_AT_OPEN 0x0100u /* a choice was down at arm()        */
#define YRSP_R_HOLD_TIMEOUT 0x0200u /* wait_for_key_release gave up      */

typedef struct yrsp_result {
    int64_t     rt_ns;              /* YRSP_NO_TIME = none                 */
    int64_t     rt_key_duration_ns; /* YRSP_NO_TIME = no release           */
    int32_t     response;           /* choice index; -1 = none or ALL      */
    int32_t     entry;              /* the response's entry; -1 = none     */
    const char* response_name;      /* the choice's name, or NULL          */
    int64_t     t_response;         /* ysp_rt ns; 0 = none                 */
    uint32_t    control, code, device;
    uint8_t     kind;
    uint8_t     stamp_tier;
    uint8_t     stamp_partial;
    uint8_t     onset_tier;
    int64_t     stamp_lo_ns, stamp_hi_ns; /* the source's bounds          */
    uint8_t     onset_src;
    uint8_t     reserved_[3];
    uint32_t    flags;              /* YRSP_R_*                          */
    int64_t     t_onset;            /* 0 = none                            */
    int64_t     onset_residual;
    int64_t     onset_uncertainty;
    int64_t     onset_frame;
    int64_t     plan_residual;      /* v0.4: the first plan's residual (a
                                     * timeline event's landing residual);
                                     * YRSP_NO_TIME = no plan came         */
    int32_t     n_responses, n_early, n_anticipations, n_late, n_after_end,
                n_not_choice, n_held, n_entries, n_lost, n_trace;
} yrsp_result;

/* Private state, sized for no allocation. */
typedef struct yrsp__chan { float v, x, y, rx, ry; uint8_t has, armed, has_ref, reserved_; } yrsp__chan;
typedef struct yrsp__held { uint32_t device, control; uint8_t kind, used, reserved_[2]; } yrsp__held;

/* The collector. Caller-allocated; every field is private. */
typedef struct yrsp_collector {
    yrsp_desc    d;
    yrsp_choice  ch[YRSP_MAX_CHOICES];
    yrsp_source  src[YRSP_MAX_SOURCES];
    int64_t        min_rt_ns, duration_ns, max_hold_ns;
    yrsp_onset   onset;
    yrsp_onset   plan;            /* the first plan (final 0) since arm() */
    int32_t        have_plan;
    int64_t        t_arm, t_end;
    int32_t        ok, armed, finished, have_onset;
    int32_t        status;
    int32_t        ended_by;        /* 0, 1 a response, 2 the deadline, 3 a hold */
    int32_t        end_entry;       /* -1                                   */
    int32_t        closing;         /* waiting for end_entry's release      */
    yrsp__held   held[YRSP_MAX_HELD];
    yrsp__chan   chan[YRSP_MAX_CHOICES];
    yrsp_entry   log[YRSP_LOG_CAP];
    int32_t        n_log, n_trace, n_lost, n_lost_trace, n_held, n_stray;
    uint32_t       held_at_open;
    char           error[160];
} yrsp_collector;

YRSP_API const char* yrsp_version(void);
YRSP_API const char* yrsp_strerror(int code);

/* Validates and copies the rules (choices and sources too); clears the
 * held state and the message. false with yrsp_error() set. */
YRSP_API bool        yrsp_open(yrsp_collector* c, const yrsp_desc* d);

/* Ends the use of the collector: it owns nothing, so this marks it closed
 * and keeps the message. Safe on a zeroed collector, after a failed open,
 * and twice. Calls after it return YRSP_ERR_STATE. */
YRSP_API void        yrsp_close(yrsp_collector* c);

/* The message of the last failure ("" after a successful open). */
YRSP_API const char* yrsp_error(const yrsp_collector* c);

/* Starts a window at t_ns: clears the entries, the onset and the trace
 * count; keeps which keys are down. Does nothing on a closed collector. */
YRSP_API void yrsp_arm(yrsp_collector* c, int64_t t_ns);

/* Sets or refines the onset and classifies every entry again. A plan
 * (final 0) does not replace a final onset. Returns the status. */
YRSP_API int  yrsp_set_onset(yrsp_collector* c, const yrsp_onset* o);

/* One input, armed or not. Returns the status, or YRSP_ERR_ARG for a
 * type or kind out of range. */
YRSP_API int  yrsp_feed(yrsp_collector* c, const yrsp_input* in);

/* The clock without input: ENDED at onset + duration_ns, or at the
 * press + max_hold_ns while waiting for a release. Call it once a frame
 * with f.onset or yrt_now_ns(). */
YRSP_API int  yrsp_update(yrsp_collector* c, int64_t now_ns);

/* Classifies against the onset as it is now and fills *out. Ends the
 * window: no later press ends anything, and the trace stops. Presses fed
 * until the next arm() are still logged and classified (a report stamped
 * before the end and delivered after it still counts; a press after a
 * finish() that came before any end is AFTER_END), and releases pair, so
 * call it again for the final result (a release, the final onset, a late
 * report). Returns YRSP_ENDED, or an error. */
YRSP_API int  yrsp_finish(yrsp_collector* c, yrsp_result* out);

/* The window's entries in arrival order; valid until the next arm(). */
YRSP_API const yrsp_entry* yrsp_entries(const yrsp_collector* c, int* n);

/* v0.4: the frame of the onset while it is a plan: the frame whose flip
 * record makes it final (yrsp_update_frame() takes it). -1 when the onset
 * is final, is not tied to a frame (a sound, OTHER), or there is none. */
YRSP_API int64_t yrsp_plan_frame(const yrsp_collector* c);

/* Key names on the US layout (CHOICES): the scancode of a name, -1 when
 * unknown; the name of a scancode, NULL when it has none. */
YRSP_API int         yrsp_scancode(const char* name);
YRSP_API const char* yrsp_key_name(uint32_t scancode);

/* v0.6: one field of yrsp_desc or of a yrsp_choice that a definition sets
 * (PARAMETERS). The shape of every ysp parameter table. */
typedef struct yrsp_param {
    const char* name;     /* the desc field; "choices.key" for a choice's   */
    const char* type;     /* "enum", "bool", "u16", "u32", "i32", "i64",
                           * "f32", "str"                                   */
    double      min, max; /* inclusive                                      */
    double      def;      /* the value a zero field means                   */
    const char* unit;     /* "", "ns"                                       */
    const char* doc;      /* one line                                       */
    const char* values;   /* enum: "name=value" pairs joined by ',', in
                           * value order; NULL for other types              */
} yrsp_param;

/* The table; *n gets its row count. */
YRSP_API const yrsp_param* yrsp_params(int* n);

/* --- helpers for the other headers, inline, when they came first ----- */

#if defined(YSP_SCREEN_H_INCLUDED)
/* The onset of a flip record: final once shown. A frame never shown
 * (SKIPPED, CANCELED: onset 0) gives its planned vblank as a plan. */
static inline yrsp_onset yrsp_onset_flip(const yscr_record* r) {
    yrsp_onset o;
    o.t_ns = r->onset ? r->onset : r->planned;
    o.residual_ns = r->residual;
    o.uncertainty_ns = 0;
    o.frame = r->index;
    o.tier = r->tier;
    o.src = (uint8_t)(r->onset ? YRSP_ONSET_FLIP : YRSP_ONSET_PLAN);
    o.final = (uint8_t)(r->onset != 0 && !(r->flags & YSCR_FLIP_PENDING));
    o.reserved_[0] = o.reserved_[1] = o.reserved_[2] = o.reserved_[3] = o.reserved_[4] = 0;
    return o;
}

/* v0.4, THE FRAME LOOP: the flip record of yrsp_plan_frame()'s frame,
 * when f->done[] has it, as the final onset (yrsp_onset_flip()); then
 * yrsp_update(c, f->onset). Call it once a frame after yscr_begin().
 * Returns the status, or a code (yrsp_error() set). */
static inline int yrsp_update_frame(yrsp_collector* c, const yscr_frame* f) {
    int64_t k = yrsp_plan_frame(c);
    int i;
    if (!f) return yrsp_set_onset(c, NULL);   /* YRSP_ERR_ARG, with the message */
    for (i = 0; k >= 0 && i < f->n_done; i++)
        if (f->done[i].index == k) {
            yrsp_onset o = yrsp_onset_flip(&f->done[i]);
            yrsp_set_onset(c, &o);
        }
    return yrsp_update(c, f->onset);
}
#endif

#if defined(YSP_SCREEN_H_INCLUDED) && defined(SDL_events_h_)
/* v0.4, THE FRAME LOOP: every event waiting on the screen's queue
 * (yscr_poll()); each input (yscr_event_input(): the key path, second
 * reports dropped) goes to the n collectors of c[], and other events are
 * dropped. Returns the inputs fed. Needs SDL3's SDL_events.h before this
 * header: a translation unit that compiles ysp/screen.h's implementation
 * has it. */
static inline int yrsp_feed_screen(yscr_screen* s, yrsp_collector* c, int n) {
    SDL_Event ev;
    yrsp_input in;
    int k, fed = 0;
    while (yscr_poll(s, &ev, NULL))
        if (yscr_event_input(s, &ev, &in)) {
            for (k = 0; k < n; k++) yrsp_feed(&c[k], &in);
            fed++;
        }
    return fed;
}
#endif


#if defined(YSP_AUDIO_H_INCLUDED)
/* The onset of a sound: final once it is not pending. */
static inline yrsp_onset yrsp_onset_audio(const yau_onset* r) {
    yrsp_onset o;
    o.t_ns = r->onset;
    o.residual_ns = r->residual;
    o.uncertainty_ns = 0;
    o.frame = -1;
    o.tier = r->tier;
    o.src = (uint8_t)YRSP_ONSET_AUDIO;
    o.final = (uint8_t)(r->onset != 0 && !(r->flags & YAU_ONSET_PENDING));
    o.reserved_[0] = o.reserved_[1] = o.reserved_[2] = o.reserved_[3] = o.reserved_[4] = 0;
    return o;
}
#endif

#if defined(YSP_TIMELINE_H_INCLUDED)
/* The plan from a fired timeline event: the landing frame's predicted
 * onset and the landing residual. */
static inline yrsp_onset yrsp_onset_landing(const ytl_event* e) {
    yrsp_onset o;
    o.t_ns = e->onset;
    o.residual_ns = e->residual;
    o.uncertainty_ns = 0;
    o.frame = e->frame;
    o.tier = 0;
    o.src = (uint8_t)YRSP_ONSET_PLAN;
    o.final = 0;
    o.reserved_[0] = o.reserved_[1] = o.reserved_[2] = o.reserved_[3] = o.reserved_[4] = 0;
    return o;
}

/* v0.4, THE FRAME LOOP: the plan from the ONSET event of `channel` among
 * fired[0 .. n - 1] (ytl_evaluate()'s output; n at most the array's
 * size), when one is there. Its landing frame is then yrsp_plan_frame().
 * Returns the event's index in fired, or -1 when none of them is it. */
static inline int yrsp_set_landing(yrsp_collector* c, const ytl_event* fired, int n, int channel) {
    int i;
    for (i = 0; fired && i < n; i++)
        if (fired[i].kind == YTL_ONSET && fired[i].target == channel) {
            yrsp_onset o = yrsp_onset_landing(&fired[i]);
            yrsp_set_onset(c, &o);
            return i;
        }
    return -1;
}
#endif

#if defined(YSP_TRIALS_H_INCLUDED)
/* v0.4, DATA: the result's columns for ysp/trials.h's data writer, in a
 * ytr_col array: { YTR_COL(row, correct, YTR_COL_BOOL), YRSP_DATA_COLS }.
 * Times in seconds; an empty field where the result has none. */
#define YRSP_DATA_COLS \
    YTR_COL_SET("response", YTR_COL_STRING), \
    YTR_COL_SET("rt_s", YTR_COL_TIME_NS), \
    YTR_COL_SET("rt_key_duration_s", YTR_COL_TIME_NS), \
    YTR_COL_SET("onset_src", YTR_COL_INT), \
    YTR_COL_SET("onset_tier", YTR_COL_INT), \
    YTR_COL_SET("onset_residual_s", YTR_COL_TIME_NS), \
    YTR_COL_SET("landing_residual_s", YTR_COL_TIME_NS), \
    YTR_COL_SET("onset_frame", YTR_COL_INT64), \
    YTR_COL_SET("rsp_tier", YTR_COL_INT), \
    YTR_COL_SET("rsp_flags", YTR_COL_INT), \
    YTR_COL_SET("n_anticipations", YTR_COL_INT), \
    YTR_COL_SET("n_early", YTR_COL_INT)
#define YRSP_N_DATA_COLS 12

/* Sets, for the next ytr_data_write(), each YRSP_DATA_COLS column that
 * the file has (found by name, so a file may declare some of them with
 * YTR_COL_SET). Call it between trials. Returns the columns set, or the
 * first negative code (ytr_data_error() set). */
static inline int yrsp_data_set(ytr_data* w, const yrsp_result* r) {
    int n = 0, rc = 0, col;
    bool on = r && r->t_onset != 0;
    if (!r) return YTR_ERR_ARG;
#define YRSP__DSET(name, call) \
    if (rc >= 0 && (col = ytr_data_col(w, name)) >= 0) { rc = call; if (rc >= 0) n++; }
    YRSP__DSET("response", ytr_data_set_string(w, col, r->response_name))
    YRSP__DSET("rt_s", ytr_data_set_time_ns(w, col, r->rt_ns))
    YRSP__DSET("rt_key_duration_s", ytr_data_set_time_ns(w, col, r->rt_key_duration_ns))
    YRSP__DSET("onset_src", ytr_data_set_int(w, col, on ? (int)r->onset_src : YTR_MISSING_INT))
    YRSP__DSET("onset_tier", ytr_data_set_int(w, col, on ? (int)r->onset_tier : YTR_MISSING_INT))
    YRSP__DSET("onset_residual_s", ytr_data_set_time_ns(w, col, on ? r->onset_residual : YTR_MISSING_NS))
    YRSP__DSET("landing_residual_s", ytr_data_set_time_ns(w, col, r->plan_residual))
    YRSP__DSET("onset_frame", ytr_data_set_int64(w, col, r->onset_frame >= 0 ? r->onset_frame : YTR_MISSING_I64))
    YRSP__DSET("rsp_tier", ytr_data_set_int(w, col, (r->flags & YRSP_R_RESPONDED) ? (int)r->stamp_tier : YTR_MISSING_INT))
    YRSP__DSET("rsp_flags", ytr_data_set_int(w, col, (int)r->flags))
    YRSP__DSET("n_anticipations", ytr_data_set_int(w, col, r->n_anticipations))
    YRSP__DSET("n_early", ytr_data_set_int(w, col, r->n_early))
#undef YRSP__DSET
    return rc < 0 ? rc : n;
}
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_RESPONSE_H_INCLUDED */

/* =======================================================================
 *
 * IMPLEMENTATION
 *
 * ======================================================================= */
#ifdef YSP_RESPONSE_IMPLEMENTATION
#ifndef YSP_RESPONSE_IMPLEMENTATION_GUARD
#define YSP_RESPONSE_IMPLEMENTATION_GUARD

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

YRSP_API const char* yrsp_version(void) { return YRSP_VERSION_STRING; }

YRSP_API const char* yrsp_strerror(int code) {
    switch (code) {
    case YRSP_IDLE:      return "idle";
    case YRSP_OPEN:      return "open";
    case YRSP_ENDED:     return "ended";
    case YRSP_ERR_ARG:   return "bad argument";
    case YRSP_ERR_STATE: return "the collector is not open";
    default:               return "unknown";
    }
}

YRSP_API const char* yrsp_error(const yrsp_collector* c) {
    return c ? c->error : "ysp_response: NULL collector";
}

/* isfinite() is C99 and MSVC's default C dialect does not declare it. */
static bool yrsp__finite(double x) { return x > -HUGE_VAL && x < HUGE_VAL; }

static bool yrsp__fail(yrsp_collector* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof c->error, fmt, ap);
    va_end(ap);
    c->ok = 0;
    return false;
}

/* A failed call's message, written on the failure path only. */
static int yrsp__err(yrsp_collector* c, int rc, const char* what) {
    if (c) snprintf(c->error, sizeof c->error, "ysp_response: %s: %s", what, yrsp_strerror(rc));
    return rc;
}

YRSP_API void yrsp_close(yrsp_collector* c) {
    if (c) c->ok = 0;
}

/* --- key names --------------------------------------------------------- */

typedef struct yrsp__name { const char* name; uint16_t sc; uint16_t ch; } yrsp__name;

/* US layout. ch: the character the key types unshifted (its SDL keycode);
 * 0 for keys whose SDL keycode is the scancode with bit 30 set. */
static const yrsp__name yrsp__names[] = {
    { "enter", 40, 13 },  { "return", 40, 13 }, { "escape", 41, 27 }, { "esc", 41, 27 },
    { "backspace", 42, 8 }, { "tab", 43, 9 },   { "space", 44, 32 },  { " ", 44, 32 },
    { "-", 45, '-' },  { "=", 46, '=' },  { "[", 47, '[' },  { "]", 48, ']' },
    { "\\", 49, '\\' }, { ";", 51, ';' }, { "'", 52, '\'' }, { "`", 53, '`' },
    { ",", 54, ',' },  { ".", 55, '.' },  { "/", 56, '/' },
    { "f1", 58, 0 },  { "f2", 59, 0 },  { "f3", 60, 0 },  { "f4", 61, 0 },
    { "f5", 62, 0 },  { "f6", 63, 0 },  { "f7", 64, 0 },  { "f8", 65, 0 },
    { "f9", 66, 0 },  { "f10", 67, 0 }, { "f11", 68, 0 }, { "f12", 69, 0 },
    { "arrowright", 79, 0 }, { "right", 79, 0 }, { "arrowleft", 80, 0 }, { "left", 80, 0 },
    { "arrowdown", 81, 0 },  { "down", 81, 0 },  { "arrowup", 82, 0 },   { "up", 82, 0 },
};

static int yrsp__lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch; }

static bool yrsp__name_eq(const char* a, const char* b) {
    while (*a && *b && yrsp__lower((unsigned char)*a) == yrsp__lower((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* The scancode and keycode of a name; false when unknown. */
static bool yrsp__lookup(const char* name, uint32_t* sc, uint32_t* kc) {
    size_t i;
    if (!name || !name[0]) return false;
    if (name[1] == 0) {
        int ch = yrsp__lower((unsigned char)name[0]);
        if (ch >= 'a' && ch <= 'z') { *sc = (uint32_t)(4 + ch - 'a'); *kc = (uint32_t)ch; return true; }
        if (ch >= '1' && ch <= '9') { *sc = (uint32_t)(30 + ch - '1'); *kc = (uint32_t)ch; return true; }
        if (ch == '0') { *sc = 39; *kc = '0'; return true; }
    }
    for (i = 0; i < sizeof yrsp__names / sizeof yrsp__names[0]; i++)
        if (yrsp__name_eq(name, yrsp__names[i].name)) {
            *sc = yrsp__names[i].sc;
            *kc = yrsp__names[i].ch ? yrsp__names[i].ch : (yrsp__names[i].sc | 0x40000000u);
            return true;
        }
    return false;
}

YRSP_API int yrsp_scancode(const char* name) {
    uint32_t sc, kc;
    return yrsp__lookup(name, &sc, &kc) ? (int)sc : -1;
}

YRSP_API const char* yrsp_key_name(uint32_t sc) {
    static const char letters[26][2] = { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                         "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z" };
    static const char digits[10][2] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
    size_t i;
    if (sc >= 4 && sc <= 29) return letters[sc - 4];
    if (sc >= 30 && sc <= 39) return digits[sc - 30];
    for (i = 0; i < sizeof yrsp__names / sizeof yrsp__names[0]; i++)
        if (yrsp__names[i].sc == sc && yrsp__names[i].name[0] != ' ' &&
            strcmp(yrsp__names[i].name, "return") && strcmp(yrsp__names[i].name, "esc") &&
            strncmp(yrsp__names[i].name, "arrow", 5))
            return yrsp__names[i].name;
    return NULL;
}

/* --- parameters ------------------------------------------------------------ */

YRSP_API const yrsp_param* yrsp_params(int* n) {
    /* durations: yrsp_open() refuses more than 1e6 s */
    static const yrsp_param table[] = {
        { "mode", "enum", 0, 2, 0, "", "which presses count: kinds (all), choices (list), none; choices make it list",
          "all=0,list=1,none=2" },
        { "kinds", "u16", 0, 65535, 1, "", "all mode: bits 1 << yin_kind; 0 = the keyboard only; 65535 = all but sync",
          NULL },
        { "min_rt_ns", "i64", 0, 1e15, 0, "ns", "earlier from the onset is an anticipation, logged (minimum_valid_rt)",
          NULL },
        { "duration_ns", "i64", 0, 1e15, 0, "ns", "the deadline from the onset; 0 = none (trial_duration)", NULL },
        { "persist", "bool", 0, 1, 0, "", "a response does not end the window (response_ends_trial false)", NULL },
        { "max_responses", "i32", 0, 2147483647.0, 0, "", "with persist: end after this many; 0 = never", NULL },
        { "allow_held_key", "bool", 0, 1, 0, "", "a held key's press can count (allow_held_key)", NULL },
        { "wait_for_key_release", "bool", 0, 1, 0, "", "the window ends at the response's release", NULL },
        { "max_hold_ns", "i64", 0, 1e15, 2e9, "ns", "wait_for_key_release gives up after this; 0 = 2 s", NULL },
        { "trace_cap", "i32", 0, 2147483647.0, 0, "", "inputs kept in the caller's trace array; 0 = no trace", NULL },
        { "trace_kinds", "u16", 0, 65535, 65535, "", "bits 1 << yin_kind of the traced inputs; 0 = every kind", NULL },
        { "choices.key", "str", 0, 0, 0, "", "a key name (CHOICES); sets control, or code with match keycode", NULL },
        { "choices.name", "str", 0, 0, 0, "", "the response's name in the data; NULL = key", NULL },
        { "choices.kind", "enum", 0, 15, 0, "", "the input's kind; 8 to 15 are the user's",
          "keyboard=0,mouse=1,touch=2,pen=3,gamepad=4,box=5,eye=6,sync=7,user=8" },
        { "choices.match", "enum", 0, 1, 0, "", "a key by its position (scancode) or by the layout's key (keycode)",
          "scancode=0,keycode=1" },
        { "choices.cross", "enum", 0, 3, 0, "", "none: a press; else a threshold crossing on an analog channel",
          "none=0,rising=1,falling=2,distance=3" },
        { "choices.device", "u32", 0, 4294967295.0, 0, "", "yin_event.device; 0 = any", NULL },
        { "choices.control", "u32", 0, 4294967295.0, 0, "", "scancode, button or axis", NULL },
        { "choices.code", "u32", 0, 4294967295.0, 0, "", "keycode, with match keycode", NULL },
        { "choices.level", "f32", -3.4e38, 3.4e38, 0, "", "crossings: the threshold; distance: above 0", NULL },
        { "choices.hysteresis", "f32", 0, 3.4e38, 0, "", "crossings: re-arm this far back", NULL },
    };
    if (n) *n = (int)(sizeof table / sizeof table[0]);
    return table;
}

/* --- init ---------------------------------------------------------------- */

/* 1e6 s bounds a duration so that onset + duration cannot overflow. */
static bool yrsp__dur(yrsp_collector* c, const char* what, int64_t ns, int64_t* out) {
    if (ns < 0 || ns > YRT_S(1e6))
        return yrsp__fail(c, "ysp_response: desc.%s %.9g s is not a duration in 0..1e6 s", what, YRT_TO_S(ns));
    *out = ns;
    return true;
}

YRSP_API bool yrsp_open(yrsp_collector* c, const yrsp_desc* d) {
    int i;
    if (!c) return false;
    memset(c, 0, sizeof *c);
    if (!d) return yrsp__fail(c, "ysp_response: NULL desc");
    c->d = *d;
    if (d->n_choices < 0 || d->n_choices > YRSP_MAX_CHOICES)
        return yrsp__fail(c, "ysp_response: n_choices %d is outside 0..%d", d->n_choices, YRSP_MAX_CHOICES);
    if (d->n_choices > 0 && !d->choices) return yrsp__fail(c, "ysp_response: n_choices %d with NULL choices", d->n_choices);
    if (d->mode < YRSP_CHOICES_ALL || d->mode > YRSP_CHOICES_NONE)
        return yrsp__fail(c, "ysp_response: mode %d is not a yrsp_choices", d->mode);
    if (d->mode == YRSP_CHOICES_NONE && d->n_choices > 0)
        return yrsp__fail(c, "ysp_response: mode NONE takes no choices");
    if (d->n_choices > 0) c->d.mode = YRSP_CHOICES_LIST;
    if (c->d.mode == YRSP_CHOICES_LIST && d->n_choices == 0)
        return yrsp__fail(c, "ysp_response: mode LIST with no choices");
    if (c->d.kinds == 0) c->d.kinds = YRSP_KIND_BIT(YRSP_KIND_KEYBOARD);
    /* "every kind" means every response: a scanner pulse arriving in an
     * ALL_KEYS trial must not end it. Only an explicit mask takes SYNC. */
    if (c->d.kinds == YRSP_KINDS_ALL) c->d.kinds = (uint16_t)(c->d.kinds & ~YRSP_KIND_BIT(YRSP_KIND_SYNC));
    if (c->d.trace_kinds == 0) c->d.trace_kinds = YRSP_KINDS_ALL;
    for (i = 0; i < d->n_choices; i++) {
        yrsp_choice* h = &c->ch[i];
        *h = d->choices[i];
        if (h->kind > 15) return yrsp__fail(c, "ysp_response: choice %d: kind %d is outside 0..15", i, h->kind);
        if (h->match > YRSP_MATCH_KEYCODE) return yrsp__fail(c, "ysp_response: choice %d: match %d", i, h->match);
        if (h->cross > YRSP_CROSS_DISTANCE) return yrsp__fail(c, "ysp_response: choice %d: cross %d", i, h->cross);
        if (h->key) {
            uint32_t sc, kc;
            if (h->kind != YRSP_KIND_KEYBOARD || h->cross)
                return yrsp__fail(c, "ysp_response: choice %d: a key name on a choice that is no key", i);
            if (!yrsp__lookup(h->key, &sc, &kc))
                return yrsp__fail(c, "ysp_response: choice %d: '%s' is not a key name (CHOICES)", i, h->key);
            if (h->match == YRSP_MATCH_SCANCODE) {
                if (h->control && h->control != sc)
                    return yrsp__fail(c, "ysp_response: choice %d: key '%s' is scancode %u, control says %u", i,
                                        h->key, (unsigned)sc, (unsigned)h->control);
                h->control = sc;
            } else {
                if (h->code && h->code != kc)
                    return yrsp__fail(c, "ysp_response: choice %d: key '%s' is keycode %u, code says %u", i,
                                        h->key, (unsigned)kc, (unsigned)h->code);
                h->code = kc;
            }
            if (!h->name) h->name = h->key;
        }
        if (h->cross) {
            if (h->kind == YRSP_KIND_KEYBOARD)
                return yrsp__fail(c, "ysp_response: choice %d: a keyboard has no analog channel to cross", i);
            if (!yrsp__finite(h->level) || !yrsp__finite(h->hysteresis) || h->hysteresis < 0)
                return yrsp__fail(c, "ysp_response: choice %d: level %g and hysteresis %g must be finite, "
                                    "hysteresis >= 0", i, (double)h->level, (double)h->hysteresis);
            if (h->cross == YRSP_CROSS_DISTANCE && !(h->level > 0))
                return yrsp__fail(c, "ysp_response: choice %d: a DISTANCE level must be > 0", i);
        } else if (h->kind == YRSP_KIND_KEYBOARD && h->match == YRSP_MATCH_SCANCODE && h->control == 0) {
            return yrsp__fail(c, "ysp_response: choice %d: no key (set key or control)", i);
        } else if (h->match == YRSP_MATCH_KEYCODE && h->code == 0) {
            return yrsp__fail(c, "ysp_response: choice %d: KEYCODE with no code", i);
        }
    }
    c->d.choices = c->ch;
    if (d->n_sources < 0 || d->n_sources > YRSP_MAX_SOURCES)
        return yrsp__fail(c, "ysp_response: n_sources %d is outside 0..%d", d->n_sources, YRSP_MAX_SOURCES);
    if (d->n_sources > 0 && !d->sources) return yrsp__fail(c, "ysp_response: n_sources %d with NULL sources", d->n_sources);
    for (i = 0; i < d->n_sources; i++) {
        c->src[i] = d->sources[i];
        if (c->src[i].kind > 15 || c->src[i].tier > YRSP_TIER_SIM || c->src[i].lo_ns > c->src[i].hi_ns)
            return yrsp__fail(c, "ysp_response: source %d: kind, tier or bounds out of range", i);
    }
    c->d.sources = c->src;
    if (d->trace_cap < 0 || (d->trace_cap > 0 && !d->trace))
        return yrsp__fail(c, "ysp_response: trace_cap %d with %s trace", d->trace_cap, d->trace ? "a" : "a NULL");
    if (d->max_responses < 0) return yrsp__fail(c, "ysp_response: max_responses %d < 0", d->max_responses);
    if (!yrsp__dur(c, "min_rt_ns", d->min_rt_ns, &c->min_rt_ns) ||
        !yrsp__dur(c, "duration_ns", d->duration_ns, &c->duration_ns) ||
        !yrsp__dur(c, "max_hold_ns", d->max_hold_ns, &c->max_hold_ns))
        return false;
    if (c->duration_ns > 0 && c->min_rt_ns >= c->duration_ns)
        return yrsp__fail(c, "ysp_response: min_rt_ns %.9g s leaves no time in a duration of %.9g s",
                            YRT_TO_S(d->min_rt_ns), YRT_TO_S(d->duration_ns));
    if (c->max_hold_ns == 0) c->max_hold_ns = YRT_S(2);
    c->end_entry = -1;
    c->status = YRSP_IDLE;
    c->ok = 1;
    c->error[0] = 0;
    return true;
}

/* --- state ------------------------------------------------------------- */

/* Two reports can be of one press when their devices are equal or one is
 * 0: SDL's message path and injected input carry no device (DEVICES). Two
 * different devices are two presses. */
static bool yrsp__same_dev(uint32_t a, uint32_t b) { return a == b || a == 0 || b == 0; }

static bool yrsp__is_down(const yrsp_collector* c, const yrsp_input* in) {
    int i;
    for (i = 0; i < YRSP_MAX_HELD; i++) {
        const yrsp__held* h = &c->held[i];
        if (h->used && h->kind == in->kind && h->control == in->control && yrsp__same_dev(h->device, in->device))
            return true;
    }
    return false;
}

/* Down on another device, both known: SDL keeps one key state for all
 * keyboards, so a second keyboard's press of a held key comes as a repeat. */
static bool yrsp__down_elsewhere(const yrsp_collector* c, const yrsp_input* in) {
    int i;
    if (!in->device) return false;
    for (i = 0; i < YRSP_MAX_HELD; i++) {
        const yrsp__held* h = &c->held[i];
        if (h->used && h->kind == in->kind && h->control == in->control && h->device && h->device != in->device)
            return true;
    }
    return false;
}

static void yrsp__set_down(yrsp_collector* c, const yrsp_input* in, bool down) {
    int i, free_i = -1;
    for (i = 0; i < YRSP_MAX_HELD; i++) {
        yrsp__held* h = &c->held[i];
        if (!h->used) { if (free_i < 0) free_i = i; continue; }
        if (h->kind != in->kind || h->control != in->control) continue;
        if (!down && yrsp__same_dev(h->device, in->device)) h->used = 0;
        else if (down && h->device == in->device) return;
    }
    if (down && free_i >= 0) {
        c->held[free_i].used = 1;
        c->held[free_i].kind = in->kind;
        c->held[free_i].device = in->device;
        c->held[free_i].control = in->control;
    }
}

static const yrsp_source* yrsp__source(const yrsp_collector* c, int kind, uint32_t device) {
    int i;
    const yrsp_source* any = NULL;
    for (i = 0; i < c->d.n_sources; i++) {
        if (c->src[i].kind != kind) continue;
        if (c->src[i].device == device && device != 0) return &c->src[i];
        if (c->src[i].device == 0 && !any) any = &c->src[i];
    }
    return any;
}

/* The class bits of an entry against the onset as it is now. */
static uint16_t yrsp__time_class(const yrsp_collector* c, int64_t t) {
    int64_t rt;
    if (!c->have_onset) return YRSP_E_EARLY;
    rt = t - c->onset.t_ns;
    if (rt < 0) return YRSP_E_EARLY;
    if (rt < c->min_rt_ns) return YRSP_E_ANTICIPATION;
    if (c->duration_ns > 0 && rt >= c->duration_ns) return YRSP_E_LATE;
    return 0;
}

static uint16_t yrsp__class(const yrsp_collector* c, const yrsp_entry* e) {
    uint16_t k = yrsp__time_class(c, e->t);
    if (k) return k;
    if (e->flags & (YRSP_E_NOT_CHOICE | YRSP_E_AFTER_END)) return 0;
    if ((e->flags & YRSP_E_HELD) && !c->d.allow_held_key) return 0;
    return YRSP_E_VALID;
}

static void yrsp__reclassify(yrsp_collector* c) {
    int i;
    for (i = 0; i < c->n_log; i++) {
        yrsp_entry* e = &c->log[i];
        e->flags = (uint16_t)((e->flags & ~YRSP_E_CLASS_MASK) | yrsp__class(c, e));
    }
}

/* Ends the window once enough responses are in (THE WINDOW). */
static void yrsp__check_end(yrsp_collector* c) {
    int need, i, k, best = -1;
    int64_t prev_t = INT64_MIN;
    int prev_i = -1;
    if (!c->armed || c->finished || c->status != YRSP_OPEN || c->closing) return;
    need = c->d.persist ? c->d.max_responses : 1;
    if (need <= 0) return;
    /* the need-th earliest response, ties by arrival */
    for (k = 0; k < need; k++) {
        best = -1;
        for (i = 0; i < c->n_log; i++) {
            const yrsp_entry* e = &c->log[i];
            if (!(e->flags & YRSP_E_VALID)) continue;
            if (e->t < prev_t || (e->t == prev_t && i <= prev_i)) continue;
            if (best < 0 || e->t < c->log[best].t) best = i;
        }
        if (best < 0) return;
        prev_t = c->log[best].t;
        prev_i = best;
    }
    c->end_entry = best;
    c->t_end = c->log[best].t;
    c->log[best].flags |= YRSP_E_ENDED;
    for (i = 0; i < c->n_log; i++)
        if ((c->log[i].t > c->t_end || (c->log[i].t == c->t_end && i > best)) &&
            !(c->log[i].flags & YRSP_E_AFTER_END)) {
            c->log[i].flags |= YRSP_E_AFTER_END;
            c->log[i].flags = (uint16_t)((c->log[i].flags & ~YRSP_E_CLASS_MASK) | yrsp__class(c, &c->log[i]));
        }
    if (c->d.wait_for_key_release && !(c->log[best].flags & YRSP_E_CROSSING) &&
        !(c->log[best].flags & YRSP_E_RELEASED)) {
        c->closing = 1;
        return;
    }
    c->status = YRSP_ENDED;
    c->ended_by = 1;
}

static int yrsp__log(yrsp_collector* c, const yrsp_input* in, int choice, uint16_t flags, float value) {
    yrsp_entry* e;
    const yrsp_source* s;
    bool match = !(flags & YRSP_E_NOT_CHOICE);
    int cap = match ? YRSP_LOG_CAP : YRSP_LOG_CAP - YRSP_LOG_RESERVE;
    if (c->n_log >= cap) { c->n_lost++; return -1; }
    e = &c->log[c->n_log];
    memset(e, 0, sizeof *e);
    e->t = in->t;
    e->device = in->device;
    e->control = in->control;
    e->code = in->code;
    e->choice = (int16_t)choice;
    e->kind = in->kind;
    e->mods = in->mods;
    e->value = value;
    s = yrsp__source(c, in->kind, in->device);
    e->stamp = in->stamp ? in->stamp : (uint8_t)(s ? s->tier : 0);
    /* a press at or after the one that ended the window is no response,
     * nor any press after a finish() that came before an end */
    if (((c->status == YRSP_ENDED && (c->ended_by == 1 || c->ended_by == 3)) || c->closing) &&
        in->t >= c->t_end)
        flags |= YRSP_E_AFTER_END;
    if (c->finished && c->ended_by == 0) flags |= YRSP_E_AFTER_END;
    e->flags = flags;
    e->flags |= yrsp__class(c, e);
    e->arrival_class = (uint8_t)(e->flags & YRSP_E_CLASS_MASK);
    return c->n_log++;
}

static int yrsp__match(const yrsp_collector* c, const yrsp_input* in) {
    int i;
    if (c->d.mode == YRSP_CHOICES_NONE) return -2;
    if (c->d.mode == YRSP_CHOICES_ALL) return (c->d.kinds >> in->kind) & 1u ? -1 : -2;
    for (i = 0; i < c->d.n_choices; i++) {
        const yrsp_choice* h = &c->ch[i];
        if (h->cross || h->kind != in->kind) continue;
        if (h->device && h->device != in->device) continue;
        if (h->match == YRSP_MATCH_KEYCODE ? h->code == in->code : h->control == in->control) return i;
    }
    return -2;
}

/* Presses are logged from arm() to the next arm(), after finish() too, so
 * a report delivered late still counts. */
static bool yrsp__window(const yrsp_collector* c) { return c->armed != 0; }

static void yrsp__press(yrsp_collector* c, const yrsp_input* in) {
    int choice;
    bool held, repeat = (in->flags & YRSP_IN_REPEAT) != 0;
    /* a "repeat" of a key down only on another keyboard is this keyboard's
     * first press (one key state for all keyboards in SDL) */
    if (repeat && !yrsp__is_down(c, in) && yrsp__down_elsewhere(c, in)) repeat = false;
    held = repeat || yrsp__is_down(c, in);
    if (!held) yrsp__set_down(c, in, true);
    if (!yrsp__window(c)) return;
    choice = yrsp__match(c, in);
    if (held) {
        if (choice >= -1 && c->d.allow_held_key) {
            yrsp__log(c, in, choice, YRSP_E_HELD, in->value);
            yrsp__check_end(c);
        } else {
            c->n_held++;
        }
        return;
    }
    if (choice < -1) yrsp__log(c, in, -1, YRSP_E_NOT_CHOICE, in->value);
    else yrsp__log(c, in, choice, 0, in->value);
    yrsp__check_end(c);
}

static void yrsp__release(yrsp_collector* c, const yrsp_input* in) {
    int i;
    if (!yrsp__is_down(c, in)) { c->n_stray++; return; }
    yrsp__set_down(c, in, false);
    if (!c->armed) return;
    /* every open entry of this control is one press: the press and, with
     * allow_held_key, its HELD repeats */
    for (i = 0; i < c->n_log; i++) {
        yrsp_entry* e = &c->log[i];
        if (e->kind != in->kind || e->control != in->control) continue;
        if (!yrsp__same_dev(e->device, in->device)) continue;
        if (e->flags & (YRSP_E_CROSSING | YRSP_E_RELEASED)) continue;
        e->t_release = in->t;
        e->flags |= YRSP_E_RELEASED;
        if (c->closing && i == c->end_entry) {
            c->closing = 0;
            c->status = YRSP_ENDED;
            c->ended_by = 1;
        }
    }
}

static bool yrsp__past(const yrsp_choice* h, float v) {
    return h->cross == YRSP_CROSS_FALLING ? v <= h->level : v >= h->level;
}
static bool yrsp__rearmed(const yrsp_choice* h, float v) {
    return h->cross == YRSP_CROSS_FALLING ? v > h->level + h->hysteresis : v < h->level - h->hysteresis;
}

static void yrsp__sample(yrsp_collector* c, const yrsp_input* in) {
    int i;
    if (c->d.mode != YRSP_CHOICES_LIST) return;
    for (i = 0; i < c->d.n_choices; i++) {
        const yrsp_choice* h = &c->ch[i];
        yrsp__chan* ch = &c->chan[i];
        float v;
        if (!h->cross || h->kind != in->kind || h->control != in->control) continue;
        if (h->device && h->device != in->device) continue;
        if (h->cross == YRSP_CROSS_DISTANCE) {
            ch->x = in->x;
            ch->y = in->y;
            ch->has = 1;
            if (!yrsp__window(c)) continue;
            if (!ch->has_ref) { ch->rx = in->x; ch->ry = in->y; ch->has_ref = 1; continue; }
            v = (float)sqrt((double)(in->x - ch->rx) * (in->x - ch->rx) + (double)(in->y - ch->ry) * (in->y - ch->ry));
        } else {
            v = in->value;
            ch->v = v;
            ch->has = 1;
        }
        if (ch->armed && yrsp__past(h, v)) {
            ch->armed = 0;
            if (yrsp__window(c)) {
                yrsp__log(c, in, i, YRSP_E_CROSSING, v);
                yrsp__check_end(c);
            }
        } else if (!ch->armed && yrsp__rearmed(h, v)) {
            ch->armed = 1;
        }
    }
}

/* --- API ------------------------------------------------------------------ */

YRSP_API void yrsp_arm(yrsp_collector* c, int64_t t_ns) {
    int i;
    if (!c || !c->ok) return;
    c->armed = 1;
    c->finished = 0;
    c->have_onset = 0;
    memset(&c->onset, 0, sizeof c->onset);
    c->have_plan = 0;
    memset(&c->plan, 0, sizeof c->plan);
    c->t_arm = t_ns;
    c->t_end = 0;
    c->status = YRSP_OPEN;
    c->ended_by = 0;
    c->end_entry = -1;
    c->closing = 0;
    c->n_log = c->n_trace = c->n_lost = c->n_lost_trace = c->n_held = c->n_stray = 0;
    c->held_at_open = 0;
    for (i = 0; i < c->d.n_choices; i++) {
        const yrsp_choice* h = &c->ch[i];
        yrsp__chan* ch = &c->chan[i];
        if (h->cross == YRSP_CROSS_DISTANCE) {
            ch->has_ref = ch->has;
            ch->rx = ch->x;
            ch->ry = ch->y;
            ch->armed = 1;
        } else if (h->cross) {
            if (!ch->has) ch->armed = 1;
            else if (yrsp__past(h, ch->v)) ch->armed = 0;
            if (!ch->armed) c->held_at_open = 1;
        } else if (h->match == YRSP_MATCH_SCANCODE) {
            int k;   /* a choice's device 0 is any device */
            for (k = 0; k < YRSP_MAX_HELD; k++)
                if (c->held[k].used && c->held[k].kind == h->kind && c->held[k].control == h->control &&
                    yrsp__same_dev(c->held[k].device, h->device))
                    c->held_at_open = 1;
        }
    }
}

YRSP_API int yrsp_set_onset(yrsp_collector* c, const yrsp_onset* o) {
    if (!c || !c->ok) return yrsp__err(c, YRSP_ERR_STATE, "yrsp_set_onset");
    if (!o) return yrsp__err(c, YRSP_ERR_ARG, "yrsp_set_onset");
    if (!c->armed) return c->status;
    if (!o->final && !c->have_plan) {   /* kept apart: the final onset
                                         * replaces it, and the landing
                                         * residual still belongs in the
                                         * data; a never-shown frame's
                                         * record (a later plan) must not
                                         * overwrite it */
        c->plan = *o;
        c->have_plan = 1;
    }
    if (c->have_onset && c->onset.final && !o->final) return c->status;
    c->onset = *o;
    c->have_onset = 1;
    yrsp__reclassify(c);
    yrsp__check_end(c);
    return c->status;
}

YRSP_API int yrsp_feed(yrsp_collector* c, const yrsp_input* in) {
    if (!c || !c->ok) return yrsp__err(c, YRSP_ERR_STATE, "yrsp_feed");
    if (!in || in->type < YRSP_PRESS || in->type > YRSP_PROXIMITY || in->kind > 15)
        return yrsp__err(c, YRSP_ERR_ARG, "yrsp_feed");
    if (c->armed && !c->finished && c->d.trace && (c->d.trace_kinds == 0 || ((c->d.trace_kinds >> in->kind) & 1u))) {
        if (c->n_trace < c->d.trace_cap) c->d.trace[c->n_trace++] = *in;
        else c->n_lost_trace++;
    }
    switch (in->type) {
    case YRSP_PRESS:   yrsp__press(c, in); break;
    case YRSP_RELEASE: yrsp__release(c, in); break;
    case YRSP_SAMPLE:  yrsp__sample(c, in); break;
    default: break;
    }
    return c->status;
}

YRSP_API int yrsp_update(yrsp_collector* c, int64_t now_ns) {
    if (!c || !c->ok) return yrsp__err(c, YRSP_ERR_STATE, "yrsp_update");
    if (c->status != YRSP_OPEN || c->finished) return c->status;
    if (c->closing) {
        if (now_ns >= c->t_end + c->max_hold_ns) {
            c->closing = 0;
            c->status = YRSP_ENDED;
            c->ended_by = 3;
        }
        return c->status;
    }
    if (c->have_onset && c->duration_ns > 0 && now_ns >= c->onset.t_ns + c->duration_ns) {
        c->status = YRSP_ENDED;
        c->ended_by = 2;
        c->t_end = c->onset.t_ns + c->duration_ns;
    }
    return c->status;
}

YRSP_API int yrsp_finish(yrsp_collector* c, yrsp_result* r) {
    int i, best = -1;
    if (!c || !c->ok) return yrsp__err(c, YRSP_ERR_STATE, "yrsp_finish");
    if (!r) return yrsp__err(c, YRSP_ERR_ARG, "yrsp_finish");
    memset(r, 0, sizeof *r);
    r->rt_ns = r->rt_key_duration_ns = YRSP_NO_TIME;
    r->response = -1;
    r->entry = -1;
    r->onset_frame = -1;
    r->plan_residual = c->have_plan ? c->plan.residual_ns : YRSP_NO_TIME;
    yrsp__reclassify(c);
    c->finished = 1;
    if (c->status == YRSP_OPEN) c->status = YRSP_ENDED;
    c->closing = 0;
    for (i = 0; i < c->n_log; i++) {
        const yrsp_entry* e = &c->log[i];
        uint16_t k = (uint16_t)(e->flags & YRSP_E_CLASS_MASK);
        if (k != e->arrival_class) {
            c->log[i].flags |= YRSP_E_RECLASSIFIED;
            r->flags |= YRSP_R_RECLASSIFIED;
        }
        if (e->flags & YRSP_E_VALID) {
            r->n_responses++;
            if (best < 0 || e->t < c->log[best].t) best = i;
        }
        if (e->flags & YRSP_E_EARLY) r->n_early++;
        if (e->flags & YRSP_E_ANTICIPATION) r->n_anticipations++;
        if (e->flags & YRSP_E_LATE) r->n_late++;
        if (e->flags & YRSP_E_AFTER_END) r->n_after_end++;
        if (e->flags & YRSP_E_NOT_CHOICE) r->n_not_choice++;
    }
    r->n_held = c->n_held;
    r->n_entries = c->n_log;
    r->n_lost = c->n_lost;
    r->n_trace = c->n_trace;
    if (c->n_lost) r->flags |= YRSP_R_LOG_FULL;
    if (c->n_lost_trace) r->flags |= YRSP_R_TRACE_FULL;
    if (c->held_at_open) r->flags |= YRSP_R_HELD_AT_OPEN;
    if (c->ended_by == 3) r->flags |= YRSP_R_HOLD_TIMEOUT;
    if (c->have_onset) {
        r->t_onset = c->onset.t_ns;
        r->onset_residual = c->onset.residual_ns;
        r->onset_uncertainty = c->onset.uncertainty_ns;
        r->onset_frame = c->onset.frame;
        r->onset_tier = c->onset.tier;
        r->onset_src = c->onset.src;
        if (!c->onset.final) r->flags |= YRSP_R_ONSET_PLAN;
    } else {
        r->flags |= YRSP_R_NO_ONSET;
    }
    if (c->ended_by == 1 && c->end_entry >= 0 && !(c->log[c->end_entry].flags & YRSP_E_VALID))
        r->flags |= YRSP_R_ENDED_EARLY;
    if (best >= 0) {
        const yrsp_entry* e = &c->log[best];
        const yrsp_source* s = yrsp__source(c, e->kind, e->device);
        r->flags |= YRSP_R_RESPONDED;
        r->entry = best;
        r->response = e->choice;
        r->response_name = e->choice >= 0 ? c->ch[e->choice].name : NULL;
        r->t_response = e->t;
        r->rt_ns = e->t - c->onset.t_ns;
        if (e->flags & YRSP_E_RELEASED) r->rt_key_duration_ns = e->t_release - e->t;
        r->control = e->control;
        r->code = e->code;
        r->device = e->device;
        r->kind = e->kind;
        r->stamp_tier = e->stamp;
        if (s && s->tier == e->stamp) {
            r->stamp_partial = s->partial;
            r->stamp_lo_ns = s->lo_ns;
            r->stamp_hi_ns = s->hi_ns;
        }
    } else if (c->duration_ns > 0) {
        r->flags |= YRSP_R_TIMEOUT;
    }
    return YRSP_ENDED;
}

YRSP_API const yrsp_entry* yrsp_entries(const yrsp_collector* c, int* n) {
    if (n) *n = c ? c->n_log : 0;
    return c ? c->log : NULL;
}

YRSP_API int64_t yrsp_plan_frame(const yrsp_collector* c) {
    if (!c || !c->ok || !c->armed || !c->have_onset || c->onset.final) return -1;
    return c->onset.frame >= 0 ? c->onset.frame : -1;
}

#endif /* YSP_RESPONSE_IMPLEMENTATION_GUARD */
#endif /* YSP_RESPONSE_IMPLEMENTATION */

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
