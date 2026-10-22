/* ysp/device.h - v0.5.0 - public domain single-header device layer
 *
 *   A response box, a trigger box or a microcontroller board as a producer
 *   of ysp/input.h events with times on the ysp_rt clock, and as a target
 *   for output codes (TTL triggers): one instance per physical device, each
 *   with its own reader thread, a decoder and encoders from ysp/box.h, a
 *   device clock fit from ysp/rt.h, timer queries for bracketed pairs,
 *   identity (a match key and the device's own answer), roles, and a
 *   lifecycle that survives an unplug: the device is found again by its
 *   key, the fit restarts, and the gap is in the log. Every output write
 *   is a record with the times before and after it.
 *   docs/devices_spec.md is the plan; this is its steps 2 and 3.
 *
 *   REQUIRES ysp/rt.h, ysp/input.h, ysp/box.h and ysp/serial.h beside it;
 *   this header includes them, and its implementation implements ysp/rt.h,
 *   ysp/box.h and ysp/serial.h unless the translation unit already does.
 *   Windows, Linux and macOS. C99 is the floor: it builds as C99, C11 and
 *   C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_DEVICE_IMPLEMENTATION
 *       #include "ysp/device.h"
 *
 *       static void to_bridge(void* ctx, const yin_event* e) {
 *           (void)ctx; yscr_push_input(e);              // ysp/screen.h
 *       }
 *       static ydev_device photo;                       // about 45 KB
 *       ydev_desc d = { 0 };
 *       d.role = "photo";
 *       d.family = YBOX_LINE;
 *       d.key = "serial:16C0:0483::";                   // any Teensy
 *       d.device = 1;
 *       d.kind = YIN_KIND_SYNC;
 *       d.sink = to_bridge;
 *       d.ring = &log_ring;                             // optional
 *       if (!ydev_open(&photo, &d)) die(ydev_error(&photo));
 *       ...
 *       ydev_close(&photo);
 *
 *       static ydev_device trig;                        // an output
 *       ydev_desc o = { 0 };
 *       o.role = "trig";
 *       o.family = YBOX_TRIGGERBOX;
 *       o.key = "serial:0403:6001:TB0123:";
 *       o.device = 2;
 *       o.ring = &log_ring;
 *       if (!ydev_open(&trig, &o)) die(ydev_error(&trig));
 *       // open returns while the device is still OPENING: wait for
 *       // RUNNING, or the first pulse fails with YDEV_ERR_STATE and the
 *       // first trigger is lost
 *       if (ydev_wait_running(&trig, YRT_S(3)) < 0) die(ydev_error(&trig));
 *       int rc = ydev_out_pulse(&trig, 12, YRT_MS(2));  // 12 for 2 ms, then 0
 *       if (rc < 0) fprintf(stderr, "trigger: %s\n", ydev_strerror(rc));
 *       ...
 *       ydev_close(&trig);
 *
 *   ---------------------------------------------------------------------
 *   MATCH KEYS (desc.key)
 *   ---------------------------------------------------------------------
 *   A key names a physical device so that it is found again after a
 *   replug and a reboot, whatever port name the OS gives it:
 *     serial:<vid>:<pid>:<serial>:<location>
 *   vid and pid in hex, the USB serial number and the port's location as
 *   ysp/serial.h lists them (yser_port_info). In a desc.key an empty or
 *   missing field matches anything, case-insensitively:
 *   "serial:16C0:0483::" is any Teensy in serial mode, "serial:0403:6001:
 *   FT4ABC12" one FTDI cable by its serial number. Cedrus ships FTDI's
 *   default vid and pid, so an XID box needs its serial number to be told
 *   from another FTDI cable. "port:COM5" or "port:/dev/ttyACM0" names a
 *   port instead, which a replug can change. ydev_port_key() writes the
 *   key of a listed port; ydev_key_match() is the rule.
 *   ydev_open() copies desc.key and desc.role into the handle, because the
 *   reader thread reads the key again at every reconnect: the caller's
 *   strings need not outlive the call. A key has at most YDEV_KEY_MAX - 1
 *   (159) characters, as a role's key in ysp/rigfile.h, which holds any
 *   "port:" with a yser_port_info name; a role at most YDEV_ROLE_MAX - 1
 *   (31, the length of a name in the role table). A longer one fails
 *   ydev_open() with a message. No allocation.
 *
 *   ---------------------------------------------------------------------
 *   LIFECYCLE
 *   ---------------------------------------------------------------------
 *     OPENING  looking for the key; every retry_ns (1 s) until it opens
 *     RUNNING  identified, reading
 *     LOST     the transport failed (unplugged) or nothing arrived for
 *              silent_ns (2 s); reopened by key every retry_ns
 *     FAILED   the device at the key did not answer as the family must
 *              (identify): a wrong device. No retry; close and open again
 *     CLOSED   closed
 *   IDENTIFY. XID: "_c1" must answer "_xid0" (a box in another mode is
 *   switched to XID mode with "c10", as pyxid2 does, and the log says so),
 *   then "_d2", "_d3", "_d4" give the product, model and firmware. LINE:
 *   "i" must answer an "I" line within 1.5 s (asked three times, for a
 *   board that resets when the port opens). PHOTO: none. desc.no_identify
 *   skips it.
 *   ydev_open() returns while the instance is OPENING: the first open and
 *   identify happen on the reader thread (or in the first ydev_poll()).
 *   ydev_wait_running(dev, timeout_ns) waits until the instance is
 *   RUNNING; call it before the first output, or the output fails with
 *   YDEV_ERR_STATE. It returns YDEV_RUNNING, YDEV_ERR_TIMEOUT, or
 *   YDEV_ERR_STATE when the instance ends FAILED (a wrong device) or is
 *   CLOSED, with the reason in ydev_error(). In manual mode it calls
 *   ydev_poll() itself.
 *   Each open starts a new fit: a device's clock can reset on power-up.
 *   Events of a LOST interval are lost; the GAP record gives the last time
 *   a byte arrived before and the time the device ran again.
 *
 *   ---------------------------------------------------------------------
 *   TIME
 *   ---------------------------------------------------------------------
 *   Every frame that carries the device's time is a pair for ysp/rt.h's
 *   fit in BRACKET mode: a frame the device sent on its own has width 0 (it
 *   is late, never early), and the answer to a timer query sent at t0 and
 *   read at t1 has width t1 - t0. Queries go every probe_ns (100 ms): the
 *   fit then has pairs between a box's rare presses (docs/rt.md: sparse
 *   presses behind a 16 ms latency timer gave millisecond errors for
 *   minutes). Brackets wider than max_width_ns are refused: 1.3 ms for
 *   LINE (native USB), 20 ms for XID, whose answers wait in the FTDI
 *   chip's latency timer (16 ms by default). An XID query writes "_e", waits
 *   1 ms and stamps t0 before the "5" the box answers. Each event's t is
 *   the fit's map of its device time, never later than its read time (a
 *   map past it is clamped and counted); unc_us is the device's tick plus
 *   the widest bracket the map rests on (without brackets, the fit's
 *   spread): a bound from the fit's own evidence, not a measurement; ticks
 *   holds the device time. Before the first pair maps, t is the read
 *   time. The fit's own CLOCK and FIT records go to desc.ring with
 *   clock_id = desc.device.
 *   ydev_source() gives the source entry: tier UNKNOWN, because no loopback
 *   has checked any device on this layer (docs/devices_spec.md, 12).
 *
 *   ---------------------------------------------------------------------
 *   THREADS, MANUAL MODE AND RECORDS
 *   ---------------------------------------------------------------------
 *   ydev_open() starts one reader thread for the instance, elevated with
 *   yrt_thread_elevate() unless desc.no_elevate. It blocks in the
 *   transport's read for at most read_timeout_ns (20 ms) and calls
 *   desc.sink on that thread for each event: a sink must be quick and
 *   thread-safe; yscr_push_input() is. desc.rule (RESPONSE RULES) comes
 *   before the sink. ydev_close() asks the transport to
 *   interrupt, joins the thread and closes the port.
 *   desc.manual: no thread. The caller calls ydev_poll() (one step: open,
 *   identify, read, decode, sink) from its own loop. With desc.now the
 *   instance reads that clock instead of yrt_now_ns() and does not wait
 *   the 1 ms between XID bytes: for tests and replays.
 *   desc.ring gets records of source YRT_SRC_DEVICE, aux = desc.device:
 *   YDEV_REC_STATE (u32[0] old, u32[1] new state, u32[2] YDEV_WHY_*,
 *   u32[3] the instance's open count), YDEV_REC_GAP (i64[0] the last byte
 *   before the loss, i64[1] running again), YDEV_REC_TEXT (39 characters:
 *   "role ...", "port ...", "ident ..."), YDEV_REC_GARBAGE (u64[0] the
 *   decoder's total, at most one a second).
 *   Nothing allocates after ydev_open(); the instance holds its buffers
 *   and the fit (about 45 KB: make it static or allocate it once).
 *
 *   ---------------------------------------------------------------------
 *   OUTPUTS
 *   ---------------------------------------------------------------------
 *   An instance of a family with outputs (ybox_caps() & YBOX_CAP_OUT)
 *   takes codes from any thread once it is RUNNING (ydev_wait_running()):
 *     ydev_out_set(dev, code)               hold code (0 is idle)
 *     ydev_out_pulse(dev, code, width_ns)   code, then 0 after width_ns
 *     ydev_out_mark(dev, code, text)        the code as desc.pulse_ns says
 *                                           (a pulse, or a set when 0) and
 *                                           the text in the log
 *     ydev_out_trigger(dev, code, deadline) the same for a ysp/screen.h
 *                                           trigger channel (AT THE FLIP)
 *   Each returns YDEV_OK or a YDEV_ERR_*: STATE when the device is not
 *   RUNNING (nothing is written), ARG for a code above ybox_code_max() or
 *   a width the family cannot give, IO when the write failed (the reader
 *   then finds the device LOST). They return the code only and do not
 *   write ydev_error(): several threads may call them at once, and the
 *   message would race (README.md, Conventions, Errors). ydev_strerror()
 *   names the code; ydev_out_last() gives the last call's result. Each
 *   call, failed or not, is one YDEV_REC_OUT record. Who times a pulse's end:
 *     the device     XID ("mp" then "mh"; the width in whole ms, sent only
 *                    when it changes) and LINE ("p <code> <us>"): flag
 *                    DEVICE_TIMED; no second write
 *     fixed          BioSemi and MMBT-S at switch P end every code after
 *                    8 ms by themselves: width_ns must be 0 or within 1 ms
 *                    of it; a set is that pulse too
 *     the host       TriggerBox, MMBT-S at switch S (desc.latched), DTR and
 *                    RTS, the parallel port: the instance's own ysp/rt.h
 *                    worker (elevated unless desc.no_elevate) writes 0 at
 *                    the later of the leading write's start + width and
 *                    its return + desc.min_hold_ns (yser_pulse_off_at_ns()
 *                    of ysp/serial.h). Its record (TRAILING) gives that
 *                    write's own times
 *   Why from the start of the leading write: over USB the code reaches the
 *   device during that write, not after it. On a Teensy 4.0 under Windows'
 *   usbser.sys a byte write took 423 us median and its byte arrived 179 us
 *   after the call started; a DTR call took 385 us and DTR arrived at
 *   83 us (docs/serial.md, Pulses). Timed from the return, as until v0.3.0,
 *   a pulse was +0.34 to +0.42 ms long at the device (median) and the
 *   spread of the write's return (about 0.4 ms, p5 to p95) became the
 *   width's; ysp/serial.h's pulses, timed from the start since its v0.6.0,
 *   were -57 to -22 us off (median). docs/device.md has this header's own
 *   numbers. desc.min_hold_ns (default 0.25 ms, YSER_DEFAULT_MIN_HOLD_NS;
 *   none on the parallel port, whose write does not wait in a USB queue)
 *   keeps a width shorter than the leading write, or a stalled write, from
 *   becoming no pulse: set it to the receiver's sample period if a shorter
 *   pulse could fall between two samples. A trigger channel's start is
 *   taken on the screen's worker, right before its write, so the worker's
 *   wake delays the pulse and does not shorten it.
 *   yser_pulse_async() and ypar_pulse_async() are not used: they give no
 *   time for the trailing write and know no modem lines. The worker keeps
 *   one trailing edge: a pulse while one is pending moves it (REPLACED); a
 *   set cancels it, because a set means "hold this". ydev_close() writes a
 *   pending trailing edge at once (FLUSHED), then 0 if a code is still
 *   held, then the family's close bytes (TriggerBox 0xFF), then closes.
 *   With desc.now (a virtual clock) there is no worker: ydev_poll() writes
 *   a trailing edge that is due.
 *   Opening: DTR and RTS start low (the port is opened with
 *   dtr_low_on_open and rts_low_on_open); the TriggerBox, MMBT-S, LINES and
 *   parallel outputs get one 0 after they open. desc.baud 0 is the
 *   family's rate (ybox_default_baud(): 9600 for the MMBT-S).
 *   Writes are serialized with the reader's timer queries under the
 *   instance's output lock; an XID query writes its 3 bytes 1 ms apart, so
 *   an output on an XID instance with queries can wait up to 2 ms
 *   (desc.probe_ns < 0 turns the queries off). Output commands are written
 *   whole: Cedrus says XID 2 devices need no gap between bytes.
 *   LINE boards report each change of their outputs ("O <t> <code>"): the
 *   reader maps t through the fit and writes a REPORTED record, the
 *   board's own time of the pin change.
 *   YDEV_REC_OUT: t_ns = i64[0]; i64[0] the time just before the write
 *   (REPORTED: the mapped board time), i64[1] just after (REPORTED: the read
 *   time), i64[2] the width (the device's or the requested one; TRAILING:
 *   the host width between the two writes), i64[3] the flip deadline (FLIP),
 *   the board ticks (REPORTED) or the press's t (RULE), u32[8] the code, u16[18] the YDEV_OUT_*
 *   flags, u16[19] the output's number (a pulse and its TRAILING record
 *   share it). MARK text follows as TEXT records "mark <text>", 34
 *   characters each. ydev_out_last() gives the last call's times.
 *   No output has a tier yet: write to edge is a loopback measurement
 *   (docs/devices_spec.md 12; examples/device/out_latency.c).
 *
 *   ---------------------------------------------------------------------
 *   AT THE FLIP (ysp/screen.h)
 *   ---------------------------------------------------------------------
 *   Include ysp/screen.h before this header (or include this header again
 *   after it) and the device layer supplies the trigger channel callback;
 *   its context is the output instance:
 *       yscr_trigger_desc ch[1];
 *       ch[0] = ydev_trigger_channel(&trig, 0);     // fn, ctx, offset, name
 *       sd.triggers = ch; sd.n_triggers = 1;
 *       ... yscr_trigger(screen, 0, 12);            // at this flip's vblank
 *   ydev_trigger_fn() runs on the screen's trigger worker at the planned
 *   vblank and calls ydev_out_trigger() with the code and the deadline: a
 *   pulse of desc.pulse_ns (a set when 0), recorded with FLIP and the
 *   deadline, so deadline to write is in the log. A flush at the screen's
 *   close writes nothing (TRIGGERS: "a callback should not pulse then").
 *   No second scheduler: the screen's worker is the clock.
 *
 *   ---------------------------------------------------------------------
 *   RESPONSE RULES (v0.5)
 *   ---------------------------------------------------------------------
 *   A response trigger (an EEG code for the button pressed) sent from the
 *   frame loop waits for the loop to see the press: up to a frame. A rule
 *   sends it where the press is read. The frame thread arms it with a
 *   window; the input's reader thread tries each event on it, before the
 *   sink, and writes the code to the output at once:
 *       ydev_rule_choice ch[2] = { 0 };
 *       ch[0].kind = YIN_KIND_BOX; ch[0].control = 1; ch[0].code = 101;
 *       ch[1].kind = YIN_KIND_BOX; ch[1].control = 2; ch[1].code = 102;
 *       ydev_rule_desc rd = { 0 };
 *       rd.out = &trig;                     // open, before the rule
 *       rd.choices = ch; rd.n_choices = 2;
 *       static ydev_rule rule;
 *       if (!ydev_rule_open(&rule, &rd)) die(ydev_rule_error(&rule));
 *       d.rule = &rule;                     // the response box's desc
 *       ... ydev_open(&resp, &d) ...
 *       // each trial, on the frame thread:
 *       ydev_rule_arm(&rule, onset + min_rt, onset + duration);
 *       ... ydev_rule_window(&rule, from, until);   // the onset moved
 *       ydev_rule_get(&rule, &info);       // the fire, for the data row
 *       ydev_rule_fires(&rule, f, 8);      // or each fire of the arm
 *       // at the end: close the box, then the rule, then the output
 *   ysp/response.h, included after this header, sets the window from a
 *   collector (yrsp_rule_arm(), yrsp_rule_window()) and the choices from
 *   its choices (yrsp_rule_choices()), so the rule and the collector agree;
 *   yrsp_rule_sent() hands the fires to the collector, whose result flags
 *   a code that its final responses do not match (its CODES SENT).
 *   ydev_rule_fires() gives each fire of the arm (YDEV_RULE_FIRES, 8,
 *   kept; more counted): the press, the code, the choice and the write's
 *   result (YDEV_RULE_PENDING while it runs).
 *   The rule: a PRESS, not a REPEAT, of a choice (kind exact, device and
 *   control 0 = any; the first choice that matches gives the code), whose
 *   t is in [from, until), while the arm has fires left (desc.max_fires:
 *   0 = 1, as the first response ends a trial; < 0 no limit). Times are
 *   the events' own t, so a box's mapped time decides, not its arrival.
 *   The code is a pulse of desc.width_ns, or of the output's
 *   desc.pulse_ns when 0 (a set when that is 0 too). Its YDEV_REC_OUT
 *   record has the RULE flag and the press's t in i64[3]: write start
 *   minus press is i64[0] - i64[3].
 *   Threads: ydev_rule_input() is for any thread (a reader, a sink of your
 *   own for a device on another layer); arm, window, disarm and get for
 *   one thread. A small lock guards the window and the count; the fire is
 *   claimed under it and written outside it, under the output's own lock,
 *   so the rule's code and the frame loop's codes (the trigger channel,
 *   ydev_out_pulse()) reach the output one write at a time, in the order
 *   the records show. A rule's pulse while another pulse is pending moves
 *   that pulse's end (REPLACED), as any pulse does. Nothing allocates.
 *   The reader thread waits for the write: a USB write takes about 0.4 ms
 *   (OUTPUTS), and the input's next read waits that long. Its events keep
 *   their times.
 *   The rule decides with the window it has when the press comes: when
 *   the final onset moves the window after a press fired (a frame shown
 *   late), the collector can call that press an anticipation (its
 *   RECLASSIFIED flags) while the code went out.
 *   Not hooked: ysp/screen.h's keys and raw mice (SDL's threads); call
 *   ydev_rule_input() from a producer of your own.
 *   Measured (examples/device/response_lag.c, in-process box and output,
 *   Windows 11, i7-1360P, AC, the timing guard): press stamp to write
 *   start through the frame loop at 60 Hz, p50 8.7 ms, max 16.7 ms (a
 *   frame); through the rule, p50 5.2 us, p99 19.3 us, max 38.7 us, 600
 *   presses each (docs/device.md). A real box adds its USB delivery
 *   before the stamp and its write after the start, on both paths.
 *
 *   ---------------------------------------------------------------------
 *   ROLES
 *   ---------------------------------------------------------------------
 *   A role is the experiment's name for an instance ("resp", "trig"). With
 *   desc.roles (a ydev_roles table the caller owns, zeroed once) and
 *   desc.device 0, ydev_open() gives the instance the role's index, 1 to
 *   YDEV_MAX_ROLES: the yin_event.device of its events and the aux of its
 *   records, so the data file joins them by role. A role keeps its index
 *   across a close and an open; two open instances cannot share a role.
 *   ydev_role_index(), ydev_role_device() and ydev_role_name() look roles
 *   up, so a timeline's "trigger trig 12" finds its output by name. Open
 *   and close instances from one thread; the lookups are safe while no
 *   open or close runs.
 *
 *   ---------------------------------------------------------------------
 *   PARAMETERS
 *   ---------------------------------------------------------------------
 *   ydev_params(family, &n) lists the desc fields that a rig profile or a
 *   definition's role sets for one YBOX_ family: only the fields that
 *   family reads, each with the value a zero field means for it (baud
 *   9600 for the MMBT-S, max_width_ns 20 ms for XID, kind SYNC for PHOTO,
 *   no min_hold_ns on the parallel port), so a loader checks a role's
 *   keys and a designer builds its panel from one source
 *   (docs/rig_spec.md, principle 8). NULL and *n 0 for another family.
 *   Each row: the field's name, its type ("enum", "bool", "u32", "i64",
 *   "str"), the inclusive range, the default, the unit ("ns" for int64
 *   ns: a profile's x_s for x_ns), a line of doc, and for an enum its
 *   names and values. Not in it: the program's fields (device, sink,
 *   ring, transport, manual, now, roles, rule). ybox_family_name() names the
 *   family.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.5.0 - RESPONSE RULES: ydev_rule, ydev_rule_open() and the others,
 *          ydev_rule_fires() and ydev_rule_fire, desc.rule, YDEV_OUT_RULE: a response's code written on the
 *          input's reader thread, not a frame later from the frame loop
 *          (docs/experiment_format.md, gap G8).
 *   v0.4.0 - PARAMETERS: ydev_params() and ydev_param, a table per family
 *          of the desc fields it reads, with that family's defaults.
 *          ydev_open() writes the family's baud into the handle's desc
 *          for a desc.baud of 0 (the rate opened is unchanged).
 *   v0.3.1 - A host-timed pulse's width runs from the START of the leading
 *          write, not from its return: the trailing 0 is written at
 *          yser_pulse_off_at_ns() (ysp/serial.h v0.6.1), at least the new
 *          desc.min_hold_ns after the leading write returned. OUTPUTS says
 *          why. Needs ysp/serial.h v0.6.1.
 *   v0.3.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          ydev_start() and ydev_stop() are ydev_open() and ydev_close().
 *          Times are int64_t ns: ydev_poll(dev, timeout_ns),
 *          desc.read_timeout_ns (was read_timeout_ms) and
 *          ydev_transport.read(conn, buf, cap, timeout_ns). New
 *          ydev_wait_running() and YDEV_ERR_TIMEOUT: the USAGE of v0.2
 *          pulsed while the device was still OPENING, so its first
 *          trigger failed with YDEV_ERR_STATE unchecked. New
 *          ydev_strerror(). ydev_open() copies desc.key and desc.role
 *          into the handle (YDEV_KEY_MAX, YDEV_ROLE_MAX; longer fails the
 *          open): the reader thread reread the caller's key at every
 *          reconnect. The output calls say that they return codes only.
 *   v0.2.1 - Builds with YRT_NO_THREADS again (v0.2.0 refused it): no
 *          trailing-edge worker there, so a host-timed pulse ends in
 *          ydev_poll(), as on a device with desc.now.
 *   v0.2.0 - Outputs: ydev_out_set(), ydev_out_pulse(), ydev_out_mark(),
 *          ydev_out_trigger() and the YDEV_REC_OUT record; the output
 *          families of ysp/box.h v0.2.0 (TriggerBox, BioSemi, MMBT-S, DTR
 *          and RTS, the parallel port, XID and line-protocol outputs);
 *          host-timed trailing edges on a ysp/rt.h worker; the
 *          ysp/screen.h trigger channel (ydev_trigger_fn); roles
 *          (ydev_roles, desc.roles); transport.lines; a transport without
 *          read. Inputs unchanged.
 *   v0.1.0 - first version: XID, ysp line protocol and photodiode frame
 *          devices on the serial transport (or the caller's), a reader
 *          thread or manual polling, BRACKET fits with timer queries,
 *          match keys, identify, reconnect, records in a ysp/rt.h ring.
 *
 *   STATUS: v0.5.0, 2026-10-21: the response rule checked on a virtual
 *   clock (window bounds, fire counts, choices, repeats, a re-arm during a
 *   write, the reader's hook, the response.h glue) and on the real clock
 *   with 4 producer threads against arms, window moves and the frame
 *   loop's own pulses, also under ThreadSanitizer on WSL2; 18 new mutants
 *   caught. Lag measured on in-process devices only (RESPONSE RULES).
 *   STATUS: v0.2.0, 2026-10-08. Built with MSVC 19.44 (/W4 /WX, C11 and
 *   C++17), MinGW-w64 gcc 16.1 and gcc 11.4 on WSL2 (C99, C11, C++17,
 *   -Werror). tests/adapt/device_test.c runs simulated devices through a
 *   fake transport on a virtual clock (manual mode) and on a real thread:
 *   identify, events mapped through the fit, timer queries and their
 *   widths, an unplug with a clock reset (LOST, the gap record, the fit
 *   restarted, RUNNING again), silence, a wrong device, the match keys;
 *   and the outputs: the bytes and lines of each family, the OUT records,
 *   trailing edges on the worker (timed on the real clock), the trigger
 *   channel with a fake flip, roles. tests/loopback/device_loopback.c runs
 *   the serial transport against a socat pty pair on WSL2;
 *   examples/device/out_latency.c measures write to edge on a simulated
 *   adapter and board. v0.3.1, 2026-10-12: the host-timed outputs (the
 *   TriggerBox byte path, DTR, RTS) into a Teensy 4.0 echo board on
 *   Windows' usbser.sys (tests/compare/serial_pulse/device_pulse.c), width
 *   error -65 to -16 us median, 0 of 2400 missed. No vendor box, line
 *   board, parallel port or FTDI bridge has been opened. docs/device.md
 *   has the numbers.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_DEVICE_H_INCLUDED
#define YSP_DEVICE_H_INCLUDED

#define YDEV_VERSION_MAJOR 0
#define YDEV_VERSION_MINOR 5
#define YDEV_VERSION_PATCH 0
#define YDEV_VERSION_STRING "0.5.0"

#include "ysp/rt.h"
#include "ysp/input.h"
#include "ysp/box.h"
#include "ysp/serial.h"

/* yser_pulse_off_at_ns() came with v0.6.1 */
#if YSER_VERSION_MAJOR == 0 && (YSER_VERSION_MINOR < 6 || (YSER_VERSION_MINOR == 6 && YSER_VERSION_PATCH < 1))
#error "ysp/device.h v0.3.1 needs ysp/serial.h v0.6.1 or later"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* With YRT_NO_THREADS there is no trailing-edge worker: a host-timed
 * pulse ends in ydev_poll() (manual mode), which is the threadless form. */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YDEV_API
#define YDEV_API extern
#endif

typedef enum ydev_lifecycle {
    YDEV_CLOSED  = 0,
    YDEV_OPENING = 1,
    YDEV_RUNNING = 2,
    YDEV_LOST    = 3,
    YDEV_FAILED  = 4
} ydev_lifecycle;

/* Why a state changed (YDEV_REC_STATE's u32[2]). */
#define YDEV_WHY_START        1u
#define YDEV_WHY_IDENTIFIED   2u
#define YDEV_WHY_DISCONNECTED 3u
#define YDEV_WHY_SILENT       4u
#define YDEV_WHY_WRONG_DEVICE 5u
#define YDEV_WHY_STOP         6u

/* Record kinds under YRT_SRC_DEVICE. */
#define YDEV_REC_STATE   1u
#define YDEV_REC_GAP     2u
#define YDEV_REC_TEXT    3u
#define YDEV_REC_GARBAGE 4u
#define YDEV_REC_OUT     5u   /* each output write (OUTPUTS)                */

/* Results of the output calls and of ydev_wait_running(). */
#define YDEV_OK           0
#define YDEV_ERR_ARG     -1   /* a code or width the family cannot give     */
#define YDEV_ERR_FAMILY  -2   /* the family has no outputs                  */
#define YDEV_ERR_STATE   -3   /* not RUNNING: nothing was written; or the
                               * wait ended FAILED or CLOSED                */
#define YDEV_ERR_IO      -4   /* the write failed                           */
#define YDEV_ERR_TIMEOUT -5   /* not RUNNING within the wait                */

/* The longest desc.key and desc.role, with the NUL: the handle keeps a
 * copy of each (MATCH KEYS). A role is a name in ydev_roles. */
#define YDEV_KEY_MAX  160
#define YDEV_ROLE_MAX 32

/* YDEV_REC_OUT flags (u16[18]) and ydev_out_info.flags. */
#define YDEV_OUT_DEVICE_TIMED 0x0001u /* the device ends the pulse           */
#define YDEV_OUT_FLUSHED      0x0002u /* a trailing edge written early by close */
#define YDEV_OUT_TRAILING     0x0004u /* the host-timed end of a pulse       */
#define YDEV_OUT_FAILED       0x0008u /* not written, or not whole           */
#define YDEV_OUT_MARK         0x0010u /* TEXT records with the text follow   */
#define YDEV_OUT_FLIP         0x0020u /* from a trigger channel; i64[3] its deadline */
#define YDEV_OUT_REPORTED     0x0040u /* the board's own report of a change  */
#define YDEV_OUT_REPLACED     0x0080u /* moved a pending trailing edge       */
#define YDEV_OUT_PULSE        0x0100u /* a pulse (else a set)                */
#define YDEV_OUT_RULE         0x0200u /* from a response rule; i64[3] the press's t */

/* How bytes move. Every function is called on the reader thread except
 * interrupt (from ydev_close()). */
typedef struct ydev_transport {
    /* Find and open the device at key; NULL when it is not there (the
     * instance tries again later). found: a name for the log. */
    void* (*open)(void* user, const char* key, char* found, size_t found_cap);
    /* > 0 bytes, 0 after timeout_ns (ns) with none, < 0 the device is
     * gone. */
    int   (*read)(void* conn, uint8_t* buf, int cap, int64_t timeout_ns);
    /* n, or < 0 the device is gone. */
    int   (*write)(void* conn, const uint8_t* buf, int n);
    void  (*interrupt)(void* conn);       /* end a blocked read; may be NULL */
    void  (*close)(void* conn);
    void* user;
    /* YBOX_LINES: set the modem lines to code (bit 0 DTR, bit 1 RTS),
     * touching only the bits in changed; 0, or < 0 the device is gone.
     * NULL for a transport without lines. */
    int   (*lines)(void* conn, uint32_t code, uint32_t changed);
} ydev_transport;
/* read may be NULL for an output-only transport (the parallel port): the
 * reader then only waits. read and write may run at the same time on two
 * threads (the reader and an output). */

typedef void (*ydev_sink)(void* ctx, const yin_event* e);

struct ydev_rule;   /* RESPONSE RULES */

/* Roles (ROLES): the caller's table, zeroed once. */
#define YDEV_MAX_ROLES 32
struct ydev_device;
typedef struct ydev_roles {
    int                 n;
    char                name[YDEV_MAX_ROLES][YDEV_ROLE_MAX];
    struct ydev_device* dev[YDEV_MAX_ROLES];   /* the running instance, or NULL */
} ydev_roles;

/* What the last output call did (ydev_out_last()). */
typedef struct ydev_out_info {
    int64_t  t_before;       /* the clock just before the write             */
    int64_t  t_after;        /* just after                                  */
    int64_t  width_ns;       /* the pulse's width; 0 for a set              */
    uint32_t code;
    uint32_t seq;            /* the output's number (YDEV_REC_OUT u16[19])  */
    uint32_t flags;          /* YDEV_OUT_*                                  */
    int      result;         /* YDEV_OK or a YDEV_ERR_*                     */
} ydev_out_info;

/* Zero-initialize, then set role, family, key, device and sink. */
typedef struct ydev_desc {
    const char*    role;            /* the experiment's name for it; logged;
                                     * copied, < YDEV_ROLE_MAX characters      */
    int            family;          /* a YBOX_ family (ysp/box.h)               */
    const char*    key;             /* the match key (MATCH KEYS); copied,
                                     * < YDEV_KEY_MAX characters               */
    uint32_t       device;          /* yin_event.device of its events, > 0;
                                     * 0 with desc.roles (ROLES)               */
    int            kind;            /* yin_event.kind; 0 = the family's: BOX for
                                     * XID and LINE, SYNC for PHOTO            */
    ydev_sink      sink;            /* called for each event                   */
    void*          sink_ctx;
    yrt_ring*      ring;            /* records; NULL = none                    */
    ydev_transport transport;       /* all NULL = ysp/serial.h                 */
    uint32_t       baud;            /* serial: 0 = ybox_default_baud()         */
    int64_t        probe_ns;        /* timer queries: 0 = 100 ms; < 0 = none   */
    int64_t        retry_ns;        /* reopen: 0 = 1 s                         */
    int64_t        silent_ns;       /* LOST after this long with no byte:
                                     * 0 = 2 s; < 0 = never                    */
    int64_t        max_width_ns;    /* 0 = 1.3 ms (LINE), 20 ms (XID)          */
    int64_t        read_timeout_ns; /* the reader's longest read: 0 = 20 ms    */
    bool           manual;          /* no thread: call ydev_poll()             */
    bool           no_identify;
    bool           no_elevate;
    int64_t      (*now)(void* ctx); /* the clock; NULL = yrt_now_ns()          */
    void*          now_ctx;
    /* outputs (OUTPUTS) and roles (ROLES) */
    ydev_roles*    roles;           /* with device 0: device = the role's index */
    int64_t        pulse_ns;        /* ydev_out_mark() and trigger channels:
                                     * > 0 a pulse this wide, 0 a set          */
    bool           latched;         /* MMBT-S with its switch at S: a byte holds
                                     * (default P: 8 ms pulses)                */
    int64_t        min_hold_ns;     /* host-timed pulses: the trailing write
                                     * starts no sooner than this after the
                                     * leading write returned; 0 = 0.25 ms
                                     * (YSER_DEFAULT_MIN_HOLD_NS), none for
                                     * YBOX_PARALLEL; < 0 fails ydev_open()   */
    struct ydev_rule* rule;         /* v0.5: tried on the reader thread with
                                     * each event, before the sink (RESPONSE
                                     * RULES); NULL = none                     */
} ydev_desc;

typedef struct ydev_stats {
    int           state;            /* ydev_lifecycle                          */
    uint32_t      opens;            /* successful opens (the first and every
                                     * reconnect)                              */
    uint64_t      reads, bytes, events, pairs, probes, answers, refused;
    uint64_t      garbage;          /* bytes that fit no frame                 */
    uint64_t      clamped;          /* events whose map passed their read time */
    uint64_t      outs;             /* output calls, trailing edges included   */
    uint64_t      out_errors;       /* of them, not written                    */
    uint64_t      reports;          /* LINE "O" reports                        */
    int64_t       last_rx_ns;       /* the last byte                           */
    yrt_fit_info  fit;
    char          found[64];        /* the port that matched                   */
    char          ident[96];        /* what the device said it is              */
} ydev_stats;

#define YDEV_EVENTS 64

typedef struct ydev_device {
    ydev_desc      d;
    ydev_transport tr;
    void*          conn;
    yser_port      port;            /* the serial transport's connection       */
    ybox_decoder   dec;
    yrt_fit        fit;
    yin_event      ev[YDEV_EVENTS];
    ybox_pair      pair[YDEV_EVENTS];
    uint8_t        rx[512];
    ydev_stats     st;              /* the reader's; copied under the lock     */
    ydev_stats     snap;
    int64_t        next_probe, probe_t0, next_try, lost_at, last_garbage_rec;
    uint32_t       probe_seq, probe_pending;
    int            state;
    int            stop;            /* written and read under the lock         */
    int            started, have_thread;
    union { unsigned char b[64]; void* p; int64_t i; double f; } thread_mem, lock_mem;
    /* outputs: written under the output lock */
    union { unsigned char b[64]; void* p; int64_t i; double f; } out_lock_mem;
#ifndef YRT_NO_THREADS
    yrt_worker     worker;          /* host-timed trailing edges               */
#endif
    ydev_out_info  last_out;
    uint64_t       outs, out_errors;
    int64_t        trail_at;        /* an armed trailing edge: its deadline    */
    int64_t        trail_lead;      /* and its pulse's leading write           */
    uint32_t       trail_token;     /* the worker's job; 0 = none armed        */
    uint32_t       trail_seq;       /* the pulse it ends                       */
    uint32_t       out_seq;
    uint32_t       out_code;        /* the code the outputs hold               */
    uint32_t       xid_width_ms;    /* the box's mp, in the XID protocol's ms;
                                     * unknown after an open                   */
    int            out_ready;       /* RUNNING with a connection               */
    int            have_worker;
    int            role;            /* the slot in desc.roles + 1, or 0        */
    char           key_buf[YDEV_KEY_MAX];   /* d.key points here               */
    char           role_buf[YDEV_ROLE_MAX]; /* d.role points here              */
    char           err[160];
} ydev_device;

YDEV_API const char* ydev_version(void);

/* Validate desc, copy its key and role, and start the instance (a thread,
 * or manual mode). false with a message in ydev_error() for a bad desc or a
 * thread that did not start. The device need not be there: the instance
 * looks for it (OPENING), and this returns before it is RUNNING; see
 * ydev_wait_running(). */
YDEV_API bool ydev_open(ydev_device* dev, const ydev_desc* desc);

/* Stop the thread, write a pending trailing edge, close the port, write
 * CLOSED. Safe on a zeroed instance, after a failed open, and twice. */
YDEV_API void ydev_close(ydev_device* dev);

/* Manual mode: one step, reading for at most timeout_ns (ns). Returns the
 * state. */
YDEV_API int  ydev_poll(ydev_device* dev, int64_t timeout_ns);

/* Wait at most timeout_ns (ns; 0 or less: look once) for the instance to
 * be RUNNING (LIFECYCLE). Returns YDEV_RUNNING; YDEV_ERR_TIMEOUT; or
 * YDEV_ERR_STATE when it ends FAILED or CLOSED. On failure ydev_error()
 * says why. In manual mode it calls ydev_poll() itself; with desc.now it
 * gives up (YDEV_ERR_TIMEOUT) when a poll does not advance that clock,
 * which belongs to the caller. Call it from the thread that opened dev. */
YDEV_API int  ydev_wait_running(ydev_device* dev, int64_t timeout_ns);

/* Static name of a YDEV_ERR_* code ("ok" for values >= 0). */
YDEV_API const char* ydev_strerror(int code);

YDEV_API int  ydev_state(ydev_device* dev);
YDEV_API void ydev_get_stats(ydev_device* dev, ydev_stats* out);
YDEV_API const char* ydev_error(const ydev_device* dev);
YDEV_API const char* ydev_state_name(int state);

/* The source entry (ysp/input.h SOURCES) of the instance's events. */
YDEV_API yin_source ydev_source(const ydev_device* dev);

/* The key of a listed port: "serial:<VID>:<PID>:<serial>:<location>". */
YDEV_API int  ydev_port_key(const yser_port_info* p, char* out, size_t cap);

/* Whether key matches pattern (MATCH KEYS). */
YDEV_API bool ydev_key_match(const char* pattern, const char* key);

/* The serial transport (the default), for a caller who wraps it. user is
 * the instance. */
YDEV_API ydev_transport ydev_serial_transport(ydev_device* dev);

/* --- outputs (OUTPUTS) ----------------------------------------------------- */

/* Any thread may call these at once: they return YDEV_OK or a YDEV_ERR_*
 * code and never write ydev_error(). */
YDEV_API int  ydev_out_set(ydev_device* dev, uint32_t code);
YDEV_API int  ydev_out_pulse(ydev_device* dev, uint32_t code, int64_t width_ns);
YDEV_API int  ydev_out_mark(ydev_device* dev, uint32_t code, const char* text);
/* desc.pulse_ns's pulse (or a set) for a trigger at deadline_ns, flagged
 * FLIP: what ydev_trigger_fn() calls. */
YDEV_API int  ydev_out_trigger(ydev_device* dev, uint32_t code, int64_t deadline_ns);
/* The last output call of any thread on dev. */
YDEV_API void ydev_out_last(ydev_device* dev, ydev_out_info* out);

/* --- response rules (RESPONSE RULES, v0.5) ----------------------------------- */

#define YDEV_RULE_CHOICES 32
/* A window bound that no press reaches: from = YDEV_RULE_NEVER closes the
 * window; until = YDEV_RULE_NEVER gives it no end. */
#define YDEV_RULE_NEVER INT64_MAX

/* A press that fires a code: kind exact, device and control 0 = any. */
typedef struct ydev_rule_choice {
    uint8_t  kind;          /* yin_kind                                   */
    uint8_t  reserved_[3];
    uint32_t device;        /* yin_event.device; 0 = any                  */
    uint32_t control;       /* button, channel or scancode; 0 = any       */
    uint32_t code;          /* the output code                            */
} ydev_rule_choice;

/* Zero-initialize, then set out and the choices. */
typedef struct ydev_rule_desc {
    struct ydev_device*     out;        /* the output; must outlive the rule */
    const ydev_rule_choice* choices;    /* copied at open                    */
    int                     n_choices;  /* 1 to YDEV_RULE_CHOICES            */
    int64_t                 width_ns;   /* > 0 a pulse this wide; 0 = the
                                         * output's desc.pulse_ns (a set when
                                         * that is 0), as ydev_out_mark()    */
    int                     max_fires;  /* per arm: 0 = 1; < 0 = no limit    */
} ydev_rule_desc;

/* The rule's state for the frame thread (ydev_rule_get()). */
typedef struct ydev_rule_info {
    int      armed;
    int      fired;         /* fires since the last arm                   */
    int      over;          /* presses of a choice in the window after the
                             * last fire it allowed; not written          */
    int      result;        /* the last fire's write: YDEV_OK or YDEV_ERR_* */
    int64_t  t_event;       /* the last fire's press: its t               */
    int64_t  t_before;      /* its write: just before                     */
    int64_t  t_after;       /* and just after                             */
    uint32_t device, control;   /* the press                              */
    uint32_t code;          /* the code written                           */
    uint32_t seq;           /* its YDEV_REC_OUT record's u16[19]          */
    int      choice;        /* the choice's index; -1 = no fire yet       */
} ydev_rule_info;

/* One fire of the current arm (ydev_rule_fires()). */
#define YDEV_RULE_FIRES   8   /* fires an arm keeps; more are counted    */
#define YDEV_RULE_PENDING 1   /* result: the write is still running      */
typedef struct ydev_rule_fire {
    int64_t  t;             /* the press's t                              */
    uint32_t device, control;
    uint32_t code;
    uint8_t  kind;
    uint8_t  reserved_[3];
    int      choice;        /* the rule's choice index                    */
    int      result;        /* YDEV_OK, a YDEV_ERR_*, or YDEV_RULE_PENDING */
} ydev_rule_fire;

/* Caller-allocated; every field is private. */
typedef struct ydev_rule {
    ydev_rule_desc   d;
    ydev_rule_choice ch[YDEV_RULE_CHOICES];
    ydev_rule_fire   fires[YDEV_RULE_FIRES];   /* this arm's, under the lock */
    int64_t          from, until;   /* the window, under the lock          */
    uint32_t         arms;          /* each arm's number                   */
    int              open;
    ydev_rule_info   info;          /* under the lock                      */
    union { unsigned char b[64]; void* p; int64_t i; double f; } lock_mem;
    char             err[128];
} ydev_rule;

/* Validate desc and copy the choices. false with a message in
 * ydev_rule_error(). The rule starts disarmed. */
YDEV_API bool ydev_rule_open(ydev_rule* r, const ydev_rule_desc* desc);
/* After every input instance that names it is closed. Safe on a zeroed
 * rule, after a failed open, and twice. */
YDEV_API void ydev_rule_close(ydev_rule* r);
YDEV_API const char* ydev_rule_error(const ydev_rule* r);
/* The frame thread. arm: a new window [from_ns, until_ns) on the press's
 * t, its fire count 0. window: move the bounds of the armed window, its
 * count kept (nothing when disarmed). disarm: no fire until the next arm;
 * the info stays. */
YDEV_API void ydev_rule_arm(ydev_rule* r, int64_t from_ns, int64_t until_ns);
YDEV_API void ydev_rule_window(ydev_rule* r, int64_t from_ns, int64_t until_ns);
YDEV_API void ydev_rule_disarm(ydev_rule* r);
/* Any thread: fire the code of the first choice that e matches, when e is
 * a PRESS (not a REPEAT) whose t is in the armed window and the arm has
 * fires left. 1 written, 0 no fire, or a YDEV_ERR_* of the write. Takes
 * the output's lock for the write; never allocates. */
YDEV_API int  ydev_rule_input(ydev_rule* r, const yin_event* e);
YDEV_API void ydev_rule_get(ydev_rule* r, ydev_rule_info* out);
/* The current arm's fires in claim order, at most cap of them (and at most
 * YDEV_RULE_FIRES: an arm keeps that many). Returns the arm's fire count,
 * which can be larger. A disarm keeps them; an arm clears them. */
YDEV_API int  ydev_rule_fires(ydev_rule* r, ydev_rule_fire* out, int cap);

/* --- roles (ROLES) ------------------------------------------------------------ */

/* The role's index (1 to YDEV_MAX_ROLES), or 0 when the table has none. */
YDEV_API int  ydev_role_index(const ydev_roles* r, const char* name);
/* The running instance of a role, or NULL. */
YDEV_API ydev_device* ydev_role_device(const ydev_roles* r, int index);
YDEV_API const char*  ydev_role_name(const ydev_roles* r, int index);

/* --- parameters (PARAMETERS) --------------------------------------------------- */

/* v0.4: one desc field a family reads. The shape of every ysp parameter
 * table. */
typedef struct ydev_param {
    const char* name;     /* the desc field                                 */
    const char* type;     /* "enum", "bool", "u32", "i64", "str"            */
    double      min, max; /* inclusive                                      */
    double      def;      /* the value a zero field means for this family   */
    const char* unit;     /* "", "ns", "baud"                               */
    const char* doc;      /* one line                                       */
    const char* values;   /* enum: "name=value" pairs joined by ',', in
                           * value order; NULL for other types              */
} ydev_param;

/* The fields family (a YBOX_ family) reads; *n gets the count. NULL and
 * *n 0 for anything else. */
YDEV_API const ydev_param* ydev_params(int family, int* n);

#ifdef __cplusplus
}
#endif

#endif /* YSP_DEVICE_H_INCLUDED */

/* Outside the include guard: including this header again after
 * ysp/screen.h (or ysp/parallel.h) adds the glue. */
#if defined(YSP_SCREEN_H_INCLUDED) && !defined(YDEV__SCREEN_GLUE)
#define YDEV__SCREEN_GLUE
/* A ysp/screen.h trigger channel's callback (AT THE FLIP); ctx is the
 * output instance. */
static inline void ydev_trigger_fn(void* ctx, const yscr_trigger_info* info) {
    if (info->flags & YSCR_TRIG_FLUSHED) return;   /* the screen's close: no pulse then */
    (void)ydev_out_trigger((ydev_device*)ctx, info->code, info->deadline_ns);
}
/* A channel for desc.triggers: the callback, the instance, the offset and
 * the role as its name. */
static inline yscr_trigger_desc ydev_trigger_channel(ydev_device* dev, int64_t offset_ns) {
    yscr_trigger_desc t;
    t.fn = ydev_trigger_fn;
    t.ctx = dev;
    t.offset_ns = offset_ns;
    t.name = dev->d.role;
    return t;
}
#endif
#if defined(YSP_PARALLEL_H_INCLUDED) && !defined(YDEV__PARALLEL_GLUE)
#define YDEV__PARALLEL_GLUE
#ifdef __cplusplus
extern "C" {
#endif
/* The parallel port as a transport for YBOX_PARALLEL: key "parallel:" (the
 * default port), "parallel:0x378" (a base address) or
 * "parallel:/dev/parport0" (Linux). The caller owns port, zeroed. Built
 * where the implementation sees ysp/parallel.h first. */
YDEV_API ydev_transport ydev_parallel_transport(ypar_port* port);
#ifdef __cplusplus
}
#endif
#endif

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_DEVICE_IMPLEMENTATION
#ifndef YSP_DEVICE_IMPLEMENTATION_GUARD
#define YSP_DEVICE_IMPLEMENTATION_GUARD

#ifndef YSP_SERIAL_IMPLEMENTATION_GUARD
    #define YSP_SERIAL_IMPLEMENTATION
    #include "ysp/serial.h"          /* implements ysp/rt.h too, unless done */
#endif
#ifndef YSP_BOX_IMPLEMENTATION_GUARD
    #define YSP_BOX_IMPLEMENTATION
    #include "ysp/box.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
#endif

/* --- the lock and the thread ---------------------------------------------------- */

#if defined(_WIN32)
typedef CRITICAL_SECTION ydev__mutex;
static void ydev__lock_init(ydev__mutex* m)  { InitializeCriticalSection(m); }
static void ydev__lock_free(ydev__mutex* m)  { DeleteCriticalSection(m); }
static void ydev__lock(ydev__mutex* m)       { EnterCriticalSection(m); }
static void ydev__unlock(ydev__mutex* m)     { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t ydev__mutex;
static void ydev__lock_init(ydev__mutex* m)  { pthread_mutex_init(m, NULL); }
static void ydev__lock_free(ydev__mutex* m)  { pthread_mutex_destroy(m); }
static void ydev__lock(ydev__mutex* m)       { pthread_mutex_lock(m); }
static void ydev__unlock(ydev__mutex* m)     { pthread_mutex_unlock(m); }
#endif
/* The lock lives in the instance (no allocation): 40 bytes on Windows and
 * Linux, 64 on macOS. */
typedef char ydev__lock_fits[sizeof(ydev__mutex) <= 64 ? 1 : -1];

static ydev__mutex* ydev__mx(ydev_device* dev) { return (ydev__mutex*)(void*)dev->lock_mem.b; }
/* The output lock: writes to the device (outputs, trailing edges, timer
 * queries) and the connection's use by them. Taken before the lock above,
 * never after it. */
static ydev__mutex* ydev__omx(ydev_device* dev) { return (ydev__mutex*)(void*)dev->out_lock_mem.b; }

static bool ydev__stopping(ydev_device* dev) {
    int s;
    ydev__lock(ydev__mx(dev));
    s = dev->stop;
    ydev__unlock(ydev__mx(dev));
    return s != 0;
}

YDEV_API const char* ydev_version(void) { return YDEV_VERSION_STRING; }

YDEV_API const char* ydev_strerror(int code) {
    switch (code) {
    case YDEV_ERR_ARG:     return "a code or width the family cannot give";
    case YDEV_ERR_FAMILY:  return "the family has no outputs";
    case YDEV_ERR_STATE:   return "the device is not running";
    case YDEV_ERR_IO:      return "the write failed";
    case YDEV_ERR_TIMEOUT: return "the device did not run in time";
    default:               return code >= 0 ? "ok" : "unknown error";
    }
}

YDEV_API const char* ydev_state_name(int state) {
    switch (state) {
    case YDEV_CLOSED:  return "closed";
    case YDEV_OPENING: return "opening";
    case YDEV_RUNNING: return "running";
    case YDEV_LOST:    return "lost";
    case YDEV_FAILED:  return "failed";
    default:           return "?";
    }
}

static int64_t ydev__now(const ydev_device* dev) {
    return dev->d.now ? dev->d.now(dev->d.now_ctx) : yrt_now_ns();
}

/* --- keys ---------------------------------------------------------------------- */

YDEV_API int ydev_port_key(const yser_port_info* p, char* out, size_t cap) {
    char tmp[256];          /* holds the longest key: 7 + 4 + 4 + 63 + 31 + 3 */
    int n;
    if (!p || !out || cap == 0) return -1;
    n = snprintf(tmp, sizeof tmp, "serial:%04X:%04X:%s:%s", (unsigned)p->vid, (unsigned)p->pid,
                 p->serial_number, p->location);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return -1; }
    memcpy(out, tmp, (size_t)n + 1);
    return n;
}

/* One ':'-separated field of s starting at *i, into f (cut to cap). */
static void ydev__field(const char* s, size_t* i, char* f, size_t cap) {
    size_t n = 0;
    while (s[*i] && s[*i] != ':') {
        if (n + 1 < cap) f[n++] = s[*i];
        (*i)++;
    }
    if (s[*i] == ':') (*i)++;
    f[n] = '\0';
}

static bool ydev__ieq(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

YDEV_API bool ydev_key_match(const char* pattern, const char* key) {
    size_t i = 0, j = 0;
    int k;
    if (!pattern || !key) return false;
    if (strncmp(pattern, "port:", 5) == 0) return strncmp(key, "port:", 5) == 0 && strcmp(pattern + 5, key + 5) == 0;
    if (strncmp(pattern, "serial:", 7) != 0 || strncmp(key, "serial:", 7) != 0) return false;
    i = 7; j = 7;
    for (k = 0; k < 4; k++) {
        char fp[96], fk[96];
        ydev__field(pattern, &i, fp, sizeof fp);
        ydev__field(key, &j, fk, sizeof fk);
        if (fp[0] && !ydev__ieq(fp, fk)) return false;
    }
    return true;
}

/* --- the serial transport ------------------------------------------------------- */

static void* ydev__ser_open(void* user, const char* key, char* found, size_t found_cap) {
    ydev_device* dev = (ydev_device*)user;
    yser_port_info info[32];
    yser_desc sd;
    char k[256];
    const char* name = NULL;
    int n, i;
    if (strncmp(key, "port:", 5) == 0) {
        name = key + 5;
    } else {
        n = yser_list_ports(info, 32);
        for (i = 0; i < n && !name; i++)
            if (ydev_port_key(&info[i], k, sizeof k) > 0 && ydev_key_match(key, k)) name = info[i].name;
    }
    if (!name || !name[0]) return NULL;
    memset(&sd, 0, sizeof sd);
    sd.device = name;
    sd.baud = dev->d.baud ? dev->d.baud : ybox_default_baud(dev->d.family);
    sd.low_latency = true;
    sd.write_timeout_ns = YRT_MS(100);
    sd.min_hold_ns = dev->d.min_hold_ns;   /* port.min_hold_ns then agrees */
    /* DTR and RTS as outputs start idle; yser_open() raises both otherwise */
    sd.dtr_low_on_open = sd.rts_low_on_open = dev->d.family == YBOX_LINES;
    if (!yser_open(&dev->port, &sd)) return NULL;
    snprintf(found, found_cap, "%s", name);
    return &dev->port;
}

static int ydev__ser_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    int n = yser_read((yser_port*)conn, buf, cap, timeout_ns, YSER_READ_ANY);
    if (n == YSER_ERR_INTERRUPTED) return 0;
    return n;
}

static int ydev__ser_write(void* conn, const uint8_t* buf, int n) {
    return yser_write((yser_port*)conn, buf, n);
}

static void ydev__ser_interrupt(void* conn) { (void)yser_interrupt((yser_port*)conn); }
static void ydev__ser_close(void* conn) { yser_close((yser_port*)conn); }

static int ydev__ser_lines(void* conn, uint32_t code, uint32_t changed) {
    yser_port* p = (yser_port*)conn;
    if ((changed & 1u) && yser_set_dtr(p, (code & 1u) != 0) < 0) return -1;
    if ((changed & 2u) && yser_set_rts(p, (code & 2u) != 0) < 0) return -1;
    return 0;
}

YDEV_API ydev_transport ydev_serial_transport(ydev_device* dev) {
    ydev_transport t;
    t.open = ydev__ser_open;
    t.read = ydev__ser_read;
    t.write = ydev__ser_write;
    t.interrupt = ydev__ser_interrupt;
    t.close = ydev__ser_close;
    t.user = dev;
    t.lines = ydev__ser_lines;
    return t;
}

/* --- the parallel transport (when ysp/parallel.h came first) ------------------ */

#if defined(YSP_PARALLEL_H_INCLUDED)
static void* ydev__par_open(void* user, const char* key, char* found, size_t found_cap) {
    ypar_port* port = (ypar_port*)user;
    ypar_desc pd;
    const char* a;
    if (strncmp(key, "parallel:", 9) != 0) return NULL;
    a = key + 9;
    memset(&pd, 0, sizeof pd);
    if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) pd.base_addr = (uint16_t)strtoul(a, NULL, 16);
    else if (a[0]) pd.device = a;
    if (!ypar_open(port, &pd)) return NULL;
    snprintf(found, found_cap, "%s", a[0] ? a : "parallel (default)");
    return port;
}

static int ydev__par_write(void* conn, const uint8_t* buf, int n) {
    /* one byte is the whole state of the data lines: the last one counts */
    if (n <= 0) return n;
    return ypar_write_data((ypar_port*)conn, buf[n - 1]) >= 0 ? n : -1;
}

static void ydev__par_close(void* conn) { ypar_close((ypar_port*)conn); }

YDEV_API ydev_transport ydev_parallel_transport(ypar_port* port) {
    ydev_transport t;
    memset(&t, 0, sizeof t);
    t.open = ydev__par_open;
    t.write = ydev__par_write;
    t.close = ydev__par_close;
    t.user = port;
    return t;
}
#endif

/* --- records -------------------------------------------------------------------- */

static void ydev__rec_at(ydev_device* dev, uint16_t kind, int64_t t, const yrt_payload* u) {
    yrt_event ev;
    if (!dev->d.ring) return;
    memset(&ev, 0, sizeof ev);
    ev.t_ns = (uint64_t)t;
    ev.source = (uint16_t)YRT_SRC_DEVICE;
    ev.kind = kind;
    ev.aux = dev->d.device;
    ev.u = *u;
    (void)yrt_ring_push(dev->d.ring, &ev);
}

static void ydev__rec(ydev_device* dev, uint16_t kind, const yrt_payload* u) {
    if (dev->d.ring) ydev__rec_at(dev, kind, ydev__now(dev), u);
}

static void ydev__text(ydev_device* dev, const char* what, const char* text) {
    yrt_payload u;
    memset(&u, 0, sizeof u);
    snprintf(u.text, sizeof u.text, "%s %s", what, text ? text : "");
    ydev__rec(dev, (uint16_t)YDEV_REC_TEXT, &u);
}

static void ydev__set_state(ydev_device* dev, int s, uint32_t why) {
    yrt_payload u;
    int old = dev->state;
    if (old == s) return;
    memset(&u, 0, sizeof u);
    u.u32[0] = (uint32_t)old;
    u.u32[1] = (uint32_t)s;
    u.u32[2] = why;
    u.u32[3] = dev->st.opens;
    ydev__rec(dev, (uint16_t)YDEV_REC_STATE, &u);
    dev->state = s;
    dev->st.state = s;
}

static void ydev__publish(ydev_device* dev) {
    yrt_fit_get(&dev->fit, &dev->st.fit);
    ydev__lock(ydev__mx(dev));
    dev->snap = dev->st;
    ydev__unlock(ydev__mx(dev));
}

/* --- writing ------------------------------------------------------------------------ */

/* XID commands go one byte at a time, 1 ms apart, as pyxid2 writes them.
 * *t_last gets the time just before the last byte: the start of a bracket. */
static int ydev__write(ydev_device* dev, const uint8_t* b, int n, int64_t* t_last) {
    int i;
    if (!ybox_slow_write(dev->d.family)) {
        if (t_last) *t_last = ydev__now(dev);
        return dev->tr.write(dev->conn, b, n) == n ? n : -1;
    }
    for (i = 0; i < n; i++) {
        if (i > 0 && !dev->d.now) (void)yrt_sleep_ns(YBOX_SLOW_WRITE_NS);
        if (i == n - 1 && t_last) *t_last = ydev__now(dev);
        if (dev->tr.write(dev->conn, b + i, 1) != 1) return -1;
    }
    return n;
}

/* --- identify ------------------------------------------------------------------- */

/* Reads until bytes arrive or ms pass (in reads of 10 ms, so a transport on
 * a virtual clock advances it), appending to acc; false when the device is
 * gone. */
static bool ydev__gather(ydev_device* dev, char* acc, size_t cap, size_t* len, int ms) {
    int64_t end = ydev__now(dev) + (int64_t)ms * 1000000;
    do {
        uint8_t b[128];
        int n = dev->tr.read(dev->conn, b, (int)sizeof b, YRT_MS(10));
        if (n < 0) return false;
        if (n > 0) {
            dev->st.last_rx_ns = ydev__now(dev);
            if (*len + (size_t)n >= cap) n = (int)(cap - 1 - *len);
            memcpy(acc + *len, b, (size_t)n);
            *len += (size_t)n;
            acc[*len] = '\0';
            return true;
        }
    } while (ydev__now(dev) < end && !ydev__stopping(dev));
    return true;
}

static const char* ydev__find(const char* acc, size_t len, const char* what) {
    size_t i, m = strlen(what);
    for (i = 0; i + m <= len; i++) if (memcmp(acc + i, what, m) == 0) return acc + i;
    return NULL;
}

/* XID: "_c1" answered "_xid<mode>"; the mode set to 0 if it is not; the
 * product, model and firmware asked. 1 done, -2 gone. */
static int ydev__xid_details(ydev_device* dev, char mode) {
    static const char* const cmd[3] = { "_d2", "_d3", "_d4" };
    char acc[64], id[3];
    size_t len;
    int k;
    if (mode != '0') {
        static const uint8_t c10[3] = { 'c', '1', '0' };
        if (ydev__write(dev, c10, 3, NULL) < 0) return -2;
        ydev__text(dev, "xid mode", "switched to 0");
    }
    for (k = 0; k < 3; k++) {
        len = 0;
        acc[0] = '\0';
        if (ydev__write(dev, (const uint8_t*)cmd[k], 3, NULL) < 0) return -2;
        if (!ydev__gather(dev, acc, sizeof acc, &len, 500)) return -2;
        id[k] = len ? acc[0] : '?';
    }
    snprintf(dev->st.ident, sizeof dev->st.ident, "xid product %c model %c firmware %c%s", id[0], id[1], id[2],
             mode != '0' ? " (was mode 1 or 2)" : "");
    return 1;
}

/* 1 identified, -1 a wrong device, -2 gone. Asks three times, 500 ms each:
 * a board that resets when its port opens needs a moment. In manual mode
 * the poll that opens the device blocks here, at most 1.5 s. */
static int ydev__identify(ydev_device* dev) {
    char acc[512];
    uint8_t q[8];
    int tries, nq;
    if (dev->d.no_identify || !(ybox_caps(dev->d.family) & YBOX_CAP_IDENT)) return 1;
    nq = ybox_identify(dev->d.family, q, (int)sizeof q);
    for (tries = 0; tries < 3 && !ydev__stopping(dev); tries++) {
        size_t len = 0;
        int64_t end;
        acc[0] = '\0';
        if (ydev__write(dev, q, nq, NULL) < 0) return -2;
        end = ydev__now(dev) + 500000000;
        while (ydev__now(dev) < end && !ydev__stopping(dev)) {
            if (!ydev__gather(dev, acc, sizeof acc, &len, (int)((end - ydev__now(dev)) / 1000000) + 1)) return -2;
            if (dev->d.family == YBOX_XID) {
                const char* x = ydev__find(acc, len, "_xid");
                if (x && (size_t)(x - acc) + 5 <= len) return ydev__xid_details(dev, x[4]);
            } else {
                /* LINE: the decoder finds the "I" line among syncs and edges */
                ybox_decoder tmp;
                ybox_out o;
                yin_event e[8];
                ybox_pair p[8];
                size_t used = 0;
                ybox_init(&tmp, YBOX_LINE, YIN_KIND_BOX, 0);
                memset(&o, 0, sizeof o);
                o.ev = e; o.ev_cap = 8; o.pair = p; o.pair_cap = 8;
                while (used < len) {
                    size_t u;
                    ybox_out_clear(&o);
                    u = ybox_decode(&tmp, (const uint8_t*)acc + used, len - used, 0, &o);
                    if (o.has_text) {
                        snprintf(dev->st.ident, sizeof dev->st.ident, "%s", o.text);
                        return 1;
                    }
                    if (u == 0) break;
                    used += u;
                }
            }
            if (len + 1 >= sizeof acc) break;
        }
    }
    return -1;
}

/* --- outputs --------------------------------------------------------------------- */

#define YDEV__WIDTH_UNKNOWN 0xFFFFFFFFu

/* The width every code gets from the device itself: BioSemi, and the MMBT-S
 * unless its switch is at S. */
static int64_t ydev__fixed_ns(const ydev_device* dev) {
    if (dev->d.family == YBOX_MMBTS && dev->d.latched) return 0;
    return ybox_fixed_pulse_ns(dev->d.family);
}

/* Whether the host times a pulse's end (the worker), or the device does. */
static bool ydev__host_pulse(const ydev_device* dev) {
    return !(ybox_caps(dev->d.family) & YBOX_CAP_PULSE) && ydev__fixed_ns(dev) == 0;
}

/* XID's mp is in whole ms and 0 means hold: a pulse is never 0 ms. */
static uint32_t ydev__xid_ms(int64_t width_ns) {
    int64_t ms = (width_ns + 500000) / 1000000;
    /* ybox_encode() takes at most 2^32 - 1 us: 4294967 ms */
    return ms < 1 ? 1u : ms > 4294967 ? 4294967u : (uint32_t)ms;
}

/* Puts code on the outputs: the modem lines, or the encoder's bytes in one
 * write. width_ns > 0 asks the device to time a pulse (PULSE families).
 * t0 and t1 bracket the write. Under the output lock, with out_ready. */
static int ydev__put(ydev_device* dev, uint32_t code, int64_t width_ns, int64_t* t0, int64_t* t1) {
    uint8_t b[24];
    int n = 0, k, r;
    int fam = dev->d.family;
    if (ybox_caps(fam) & YBOX_CAP_LINES) {
        if (!dev->tr.lines) { *t0 = *t1 = ydev__now(dev); return YDEV_ERR_FAMILY; }
        *t0 = ydev__now(dev);
        r = dev->tr.lines(dev->conn, code, code ^ dev->out_code);
        *t1 = ydev__now(dev);
        return r < 0 ? YDEV_ERR_IO : YDEV_OK;
    }
    if (fam == YBOX_XID) {
        /* the box keeps its pulse width: send mp only when it changes */
        uint32_t ms = width_ns > 0 ? ydev__xid_ms(width_ns) : 0u;
        if (ms != dev->xid_width_ms) {
            k = ybox_encode(fam, YBOX_OP_WIDTH, 0, (int64_t)ms * 1000000, b, (int)sizeof b);
            if (k < 0) return YDEV_ERR_ARG;
            n = k;
        }
        k = ybox_encode(fam, YBOX_OP_SET, code, 0, b + n, (int)sizeof b - n);
    } else {
        k = ybox_encode(fam, width_ns > 0 ? YBOX_OP_PULSE : YBOX_OP_SET, code,
                        width_ns > 0 ? width_ns : 0, b, (int)sizeof b);
    }
    if (k <= 0) { *t0 = *t1 = ydev__now(dev); return YDEV_ERR_ARG; }
    n += k;
    *t0 = ydev__now(dev);
    r = dev->tr.write(dev->conn, b, n);
    *t1 = ydev__now(dev);
    if (r != n) {
        if (fam == YBOX_XID) dev->xid_width_ms = YDEV__WIDTH_UNKNOWN;
        return YDEV_ERR_IO;
    }
    if (fam == YBOX_XID) dev->xid_width_ms = width_ns > 0 ? ydev__xid_ms(width_ns) : 0u;
    return YDEV_OK;
}

/* One YDEV_REC_OUT record and the last call's info. Under the output lock. */
static void ydev__out_done(ydev_device* dev, int rc, uint32_t flags, uint32_t code, int64_t t0, int64_t t1,
                           int64_t width, int64_t extra, uint32_t seq) {
    yrt_payload u;
    if (rc != YDEV_OK) { flags |= YDEV_OUT_FAILED; dev->out_errors++; }
    dev->outs++;
    dev->last_out.t_before = t0;
    dev->last_out.t_after = t1;
    dev->last_out.width_ns = width;
    dev->last_out.code = code;
    dev->last_out.seq = seq;
    dev->last_out.flags = flags;
    dev->last_out.result = rc;
    memset(&u, 0, sizeof u);
    u.i64[0] = t0;
    u.i64[1] = t1;
    u.i64[2] = width;
    u.i64[3] = extra;
    u.u32[8] = code;
    u.u16[18] = (uint16_t)flags;
    u.u16[19] = (uint16_t)seq;
    ydev__rec_at(dev, (uint16_t)YDEV_REC_OUT, t0, &u);
}

/* The end of a host-timed pulse: 0, if this job is still the armed one
 * (token 0: whatever is armed). */
static void ydev__trail(ydev_device* dev, uint32_t token, bool flushed) {
    ydev__lock(ydev__omx(dev));
    if (dev->trail_token && (token == 0 || token == dev->trail_token)) {
        int64_t t0, t1;
        int rc;
        dev->trail_token = 0;
        if (dev->out_ready) rc = ydev__put(dev, 0, 0, &t0, &t1);
        else { rc = YDEV_ERR_STATE; t0 = t1 = ydev__now(dev); }
        if (rc == YDEV_OK) dev->out_code = 0;
        ydev__out_done(dev, rc, YDEV_OUT_TRAILING | (flushed ? YDEV_OUT_FLUSHED : 0u), 0, t0, t1,
                       t0 - dev->trail_lead, 0, dev->trail_seq);
    }
    ydev__unlock(ydev__omx(dev));
}

#ifndef YRT_NO_THREADS
static void ydev__trail_job(void* ctx, const yrt_job_info* info) {
    ydev__trail((ydev_device*)ctx, info->seq, info->flushed);
}
#endif

/* Every output call. width_ns < 0: a set; else a pulse (0 = the family's
 * fixed width). info: this call's own result, which last_out may no longer
 * hold when the caller reads it. */
static int ydev__out(ydev_device* dev, uint32_t code, int64_t width_ns, uint32_t flags, int64_t extra,
                     ydev_out_info* info) {
    int64_t t0 = 0, t1 = 0, w = 0, fixed;
    uint32_t seq;
    int rc = YDEV_OK;
    bool pulse = width_ns >= 0;
    if (!dev || !dev->started) return YDEV_ERR_STATE;
    if (!(ybox_caps(dev->d.family) & YBOX_CAP_OUT)) return YDEV_ERR_FAMILY;
    fixed = ydev__fixed_ns(dev);
    ydev__lock(ydev__omx(dev));
    seq = ++dev->out_seq;
    if (pulse) flags |= YDEV_OUT_PULSE;
    if (code > ybox_code_max(dev->d.family)) rc = YDEV_ERR_ARG;
    else if (fixed > 0 && pulse && width_ns != 0 && (width_ns < fixed - 1000000 || width_ns > fixed + 1000000))
        rc = YDEV_ERR_ARG;   /* the device ends every code after its own width */
    else if (!fixed && pulse && width_ns == 0) rc = YDEV_ERR_ARG;
    else if (!dev->out_ready) rc = YDEV_ERR_STATE;
    if (rc != YDEV_OK) {
        t0 = t1 = ydev__now(dev);
        ydev__out_done(dev, rc, flags, code, t0, t1, 0, extra, seq);
        if (info) *info = dev->last_out;
        ydev__unlock(ydev__omx(dev));
        return rc;
    }
    if (dev->trail_token) {
        /* a set means "hold this"; a pulse moves the one trailing edge */
        dev->trail_token = 0;
        if (pulse) flags |= YDEV_OUT_REPLACED;
#ifndef YRT_NO_THREADS
        if (dev->have_worker) (void)yrt_worker_cancel(&dev->worker);
#endif
    }
    if (fixed > 0) {
        rc = ydev__put(dev, code, 0, &t0, &t1);
        w = fixed;
        flags |= YDEV_OUT_DEVICE_TIMED;
    } else if (pulse && !ydev__host_pulse(dev)) {
        rc = ydev__put(dev, code, width_ns, &t0, &t1);
        w = dev->d.family == YBOX_XID ? (int64_t)ydev__xid_ms(width_ns) * 1000000 : (width_ns + 500) / 1000 * 1000;
        flags |= YDEV_OUT_DEVICE_TIMED;
    } else {
        rc = ydev__put(dev, code, 0, &t0, &t1);
        if (rc == YDEV_OK && pulse) {
            /* From the leading write's start: over USB the code arrives
             * during that write, so a width from its return was long by
             * the write's tail (OUTPUTS). */
            w = width_ns;
            dev->trail_at = yser_pulse_off_at_ns(t0, t1, width_ns, dev->d.min_hold_ns);
            dev->trail_lead = t0;
            dev->trail_seq = seq;
#ifndef YRT_NO_THREADS
            if (dev->have_worker) {
                int job = yrt_worker_submit(&dev->worker, dev->trail_at, ydev__trail_job, dev);
                dev->trail_token = job > 0 ? (uint32_t)job : 0u;
                if (job <= 0) rc = YDEV_ERR_IO;
            } else
#endif
            {
                dev->trail_token = 1u;   /* a virtual clock: ydev_poll() ends it */
            }
        }
    }
    /* what the lines hold now: a device-timed pulse ends by itself */
    if (rc == YDEV_OK) dev->out_code = (flags & YDEV_OUT_DEVICE_TIMED) ? 0u : code;
    ydev__out_done(dev, rc, flags, code, t0, t1, w, extra, seq);
    if (info) *info = dev->last_out;
    ydev__unlock(ydev__omx(dev));
    return rc;
}

YDEV_API int ydev_out_set(ydev_device* dev, uint32_t code) { return ydev__out(dev, code, -1, 0u, 0, NULL); }

YDEV_API int ydev_out_pulse(ydev_device* dev, uint32_t code, int64_t width_ns) {
    if (width_ns < 0) width_ns = 0;
    return ydev__out(dev, code, width_ns, 0u, 0, NULL);
}

YDEV_API int ydev_out_trigger(ydev_device* dev, uint32_t code, int64_t deadline_ns) {
    if (!dev) return YDEV_ERR_STATE;
    return ydev__out(dev, code, dev->d.pulse_ns > 0 || ydev__fixed_ns(dev) > 0 ? dev->d.pulse_ns : -1,
                     YDEV_OUT_FLIP, deadline_ns, NULL);
}

YDEV_API int ydev_out_mark(ydev_device* dev, uint32_t code, const char* text) {
    int rc;
    if (!dev) return YDEV_ERR_STATE;
    rc = ydev__out(dev, code, dev->d.pulse_ns > 0 || ydev__fixed_ns(dev) > 0 ? dev->d.pulse_ns : -1,
                   text && text[0] ? YDEV_OUT_MARK : 0u, 0, NULL);
    if (text && text[0] && dev->started && dev->d.ring) {
        /* the text after its record, 34 characters a record */
        size_t n = strlen(text), i;
        for (i = 0; i < n; i += 34) {
            yrt_payload u;
            memset(&u, 0, sizeof u);
            snprintf(u.text, sizeof u.text, "mark %.34s", text + i);
            ydev__rec(dev, (uint16_t)YDEV_REC_TEXT, &u);
        }
    }
    return rc;
}

YDEV_API void ydev_out_last(ydev_device* dev, ydev_out_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!dev || !dev->started) return;
    ydev__lock(ydev__omx(dev));
    *out = dev->last_out;
    ydev__unlock(ydev__omx(dev));
}

/* --- response rules --------------------------------------------------------------- */

static ydev__mutex* ydev__rmx(ydev_rule* r) { return (ydev__mutex*)(void*)r->lock_mem.b; }

YDEV_API bool ydev_rule_open(ydev_rule* r, const ydev_rule_desc* desc) {
    int i;
    if (!r) return false;
    memset(r, 0, sizeof *r);
#define YDEV__RFAIL(msg) do { snprintf(r->err, sizeof r->err, "ysp_device: rule: %s", msg); return false; } while (0)
    if (!desc) YDEV__RFAIL("NULL desc");
    if (!desc->out) YDEV__RFAIL("desc.out is required");
    if (!(ybox_caps(desc->out->d.family) & YBOX_CAP_OUT)) YDEV__RFAIL("desc.out's family has no outputs");
    if (!desc->choices || desc->n_choices < 1 || desc->n_choices > YDEV_RULE_CHOICES)
        YDEV__RFAIL("desc.choices: 1 to 32 (YDEV_RULE_CHOICES)");
    if (desc->width_ns < 0) YDEV__RFAIL("desc.width_ns must be >= 0");
    for (i = 0; i < desc->n_choices; i++) {
        if (desc->choices[i].kind > 15) YDEV__RFAIL("a choice's kind is not a yin_kind");
        if (desc->choices[i].code > ybox_code_max(desc->out->d.family)) YDEV__RFAIL("a choice's code is above the output's ybox_code_max()");
    }
#undef YDEV__RFAIL
    r->d = *desc;
    memcpy(r->ch, desc->choices, (size_t)desc->n_choices * sizeof r->ch[0]);
    r->d.choices = r->ch;
    r->from = r->until = YDEV_RULE_NEVER;
    r->info.choice = -1;
    ydev__lock_init(ydev__rmx(r));
    r->open = 1;
    return true;
}

YDEV_API void ydev_rule_close(ydev_rule* r) {
    if (!r || !r->open) return;
    ydev__lock_free(ydev__rmx(r));
    r->open = 0;
}

YDEV_API const char* ydev_rule_error(const ydev_rule* r) { return r ? r->err : "ysp_device: NULL rule"; }

static void ydev__rule_set(ydev_rule* r, int64_t from, int64_t until, int arm) {
    if (!r || !r->open) return;
    ydev__lock(ydev__rmx(r));
    if (arm) {
        if (++r->arms == 0) r->arms = 1;   /* 0 means no claim in ydev_rule_input() */
        r->info.armed = 1;
        r->info.fired = 0;
        r->info.over = 0;
    }
    if (r->info.armed) { r->from = from; r->until = until; }
    ydev__unlock(ydev__rmx(r));
}
YDEV_API void ydev_rule_arm(ydev_rule* r, int64_t from_ns, int64_t until_ns) { ydev__rule_set(r, from_ns, until_ns, 1); }
YDEV_API void ydev_rule_window(ydev_rule* r, int64_t from_ns, int64_t until_ns) { ydev__rule_set(r, from_ns, until_ns, 0); }
YDEV_API void ydev_rule_disarm(ydev_rule* r) {
    if (!r || !r->open) return;
    ydev__lock(ydev__rmx(r));
    r->info.armed = 0;
    r->from = r->until = YDEV_RULE_NEVER;
    ydev__unlock(ydev__rmx(r));
}

YDEV_API int ydev_rule_input(ydev_rule* r, const yin_event* e) {
    int i, k = -1, rc, slot = 0;
    uint32_t arm = 0;
    ydev_device* out;
    ydev_out_info oi;
    int64_t w;
    if (!r || !r->open || !e || e->type != YIN_PRESS || (e->flags & YIN_REPEAT)) return 0;
    for (i = 0; i < r->d.n_choices && k < 0; i++) {
        const ydev_rule_choice* c = &r->ch[i];
        if (c->kind == e->kind && (!c->device || c->device == e->device) && (!c->control || c->control == e->control))
            k = i;
    }
    if (k < 0) return 0;
    /* claim the fire under the rule's lock and write outside it: the
     * write takes the output's lock and can take a USB write's time */
    ydev__lock(ydev__rmx(r));
    if (r->info.armed && e->t >= r->from && e->t < r->until) {
        if (r->d.max_fires < 0 || r->info.fired < (r->d.max_fires ? r->d.max_fires : 1)) {
            slot = r->info.fired++;
            arm = r->arms;
            if (slot < YDEV_RULE_FIRES) {
                ydev_rule_fire* f = &r->fires[slot];
                memset(f, 0, sizeof *f);
                f->t = e->t;
                f->device = e->device;
                f->control = e->control;
                f->code = r->ch[k].code;
                f->kind = e->kind;
                f->choice = k;
                f->result = YDEV_RULE_PENDING;
            }
        } else {
            r->info.over++;
        }
    }
    ydev__unlock(ydev__rmx(r));
    if (!arm) return 0;
    out = r->d.out;
    w = r->d.width_ns > 0 ? r->d.width_ns
      : out->d.pulse_ns > 0 || ydev__fixed_ns(out) > 0 ? out->d.pulse_ns : -1;
    memset(&oi, 0, sizeof oi);
    rc = ydev__out(out, r->ch[k].code, w, YDEV_OUT_RULE, e->t, &oi);
    ydev__lock(ydev__rmx(r));
    if (arm == r->arms) {
        if (slot < YDEV_RULE_FIRES) r->fires[slot].result = rc;
        r->info.result = rc;
        r->info.t_event = e->t;
        r->info.t_before = oi.t_before;
        r->info.t_after = oi.t_after;
        r->info.device = e->device;
        r->info.control = e->control;
        r->info.code = r->ch[k].code;
        r->info.seq = oi.seq;
        r->info.choice = k;
    }
    ydev__unlock(ydev__rmx(r));
    return rc == YDEV_OK ? 1 : rc;
}

YDEV_API int ydev_rule_fires(ydev_rule* r, ydev_rule_fire* out, int cap) {
    int n, i;
    if (!r || !r->open) return 0;
    ydev__lock(ydev__rmx(r));
    n = r->info.fired;
    for (i = 0; out && i < cap && i < n && i < YDEV_RULE_FIRES; i++) out[i] = r->fires[i];
    ydev__unlock(ydev__rmx(r));
    return n;
}

YDEV_API void ydev_rule_get(ydev_rule* r, ydev_rule_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->choice = -1;
    if (!r || !r->open) return;
    ydev__lock(ydev__rmx(r));
    *out = r->info;
    ydev__unlock(ydev__rmx(r));
}

/* After an open: outputs usable, the width of an XID box unknown, and a 0
 * for the boxes that hold a byte (and the modem lines), so they start
 * idle. */
static void ydev__out_open(ydev_device* dev) {
    int fam = dev->d.family;
    ydev__lock(ydev__omx(dev));
    dev->out_code = fam == YBOX_LINES ? 3u : 0u;   /* lines: write both, whatever the open left */
    dev->xid_width_ms = YDEV__WIDTH_UNKNOWN;
    dev->trail_token = 0;
    dev->out_ready = dev->conn != NULL;
    ydev__unlock(ydev__omx(dev));
    if (fam == YBOX_TRIGGERBOX || fam == YBOX_PARALLEL || fam == YBOX_LINES || (fam == YBOX_MMBTS && dev->d.latched))
        (void)ydev_out_set(dev, 0);
}

/* At stop, after the reader: a held code back to 0, the close bytes. */
static void ydev__out_close(ydev_device* dev) {
    uint8_t b[8];
    int n;
    if (!(ybox_caps(dev->d.family) & YBOX_CAP_OUT)) return;
    if (dev->out_code != 0 && ydev__fixed_ns(dev) == 0) (void)ydev_out_set(dev, 0);
    n = ybox_encode(dev->d.family, YBOX_OP_CLOSE, 0, 0, b, (int)sizeof b);
    ydev__lock(ydev__omx(dev));
    if (n > 0 && dev->out_ready) (void)dev->tr.write(dev->conn, b, n);
    ydev__unlock(ydev__omx(dev));
}

/* --- roles ------------------------------------------------------------------------ */

YDEV_API int ydev_role_index(const ydev_roles* r, const char* name) {
    int i;
    if (!r || !name) return 0;
    for (i = 0; i < r->n && i < YDEV_MAX_ROLES; i++)
        if (strcmp(r->name[i], name) == 0) return i + 1;
    return 0;
}

YDEV_API ydev_device* ydev_role_device(const ydev_roles* r, int index) {
    if (!r || index < 1 || index > r->n || index > YDEV_MAX_ROLES) return NULL;
    return r->dev[index - 1];
}

YDEV_API const char* ydev_role_name(const ydev_roles* r, int index) {
    if (!r || index < 1 || index > r->n || index > YDEV_MAX_ROLES) return NULL;
    return r->name[index - 1];
}

/* --- parameters --------------------------------------------------------------------- */

/* Rows in ydev_desc's order; each family takes the rows it reads, with its
 * own defaults (ydev_open()'s). Upper bounds of times are a sanity range
 * (1e6 s), not a limit of the header; a pulse's 10 s is the rig profile's. */
#define YDEV__P_ROLE { "role", "str", 0, 0, 0, "", "the experiment's name for the device; required, < 32 characters", NULL }
#define YDEV__P_KEY  { "key", "str", 0, 0, 0, "", "the match key (MATCH KEYS); required, < 160 characters", NULL }
#define YDEV__P_KIND(def) { "kind", "enum", 1, 15, def, "", "yin_event.kind of its events; 0 = the family's; 8 to 15 are the user's", \
    "mouse=1,touch=2,pen=3,gamepad=4,box=5,eye=6,sync=7,user=8" }
#define YDEV__P_BAUD(def) { "baud", "u32", 1, 10000000, def, "baud", "the serial rate; 0 = the family's", NULL }
#define YDEV__P_PROBE { "probe_ns", "i64", -1, 1e15, 1e8, "ns", "timer queries this far apart; < 0 = none", NULL }
#define YDEV__P_RETRY { "retry_ns", "i64", 1, 1e15, 1e9, "ns", "reopen by key this often while OPENING or LOST", NULL }
#define YDEV__P_SILENT { "silent_ns", "i64", -1, 1e15, 2e9, "ns", "LOST after this long with no byte; < 0 = never", NULL }
#define YDEV__P_WIDTH(def) { "max_width_ns", "i64", 1, 1e15, def, "ns", "clock pairs from wider query brackets are refused", NULL }
#define YDEV__P_READ { "read_timeout_ns", "i64", 1, 1e15, 2e7, "ns", "the reader's longest read", NULL }
#define YDEV__P_NO_ID { "no_identify", "bool", 0, 1, 0, "", "skip the identity query (IDENTIFY)", NULL }
#define YDEV__P_NO_EL { "no_elevate", "bool", 0, 1, 0, "", "the reader and the pulse worker keep normal priority", NULL }
#define YDEV__P_PULSE(max, doc) { "pulse_ns", "i64", 0, max, 0, "ns", doc, NULL }
#define YDEV__P_LATCH { "latched", "bool", 0, 1, 0, "", "the switch is at S: a byte holds (default P: 8 ms pulses)", NULL }
#define YDEV__P_HOLD(min, def) { "min_hold_ns", "i64", min, 1e15, def, "ns", \
    "a host-timed trailing write starts no sooner after the leading one returned", NULL }
#define YDEV__PULSE_DOC "ydev_out_mark() and trigger channels: a pulse this wide; 0 = a set"

static const ydev_param ydev__params_xid[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_KIND(YIN_KIND_BOX), YDEV__P_BAUD(115200), YDEV__P_PROBE, YDEV__P_RETRY,
    YDEV__P_SILENT, YDEV__P_WIDTH(2e7), YDEV__P_READ, YDEV__P_NO_ID, YDEV__P_NO_EL,
    YDEV__P_PULSE(1e10, YDEV__PULSE_DOC "; the box times it in whole ms"),
};
static const ydev_param ydev__params_line[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_KIND(YIN_KIND_BOX), YDEV__P_BAUD(115200), YDEV__P_PROBE, YDEV__P_RETRY,
    YDEV__P_SILENT, YDEV__P_WIDTH(1.3e6), YDEV__P_READ, YDEV__P_NO_ID, YDEV__P_NO_EL,
    YDEV__P_PULSE(1e10, YDEV__PULSE_DOC "; the board times it in us"),
};
static const ydev_param ydev__params_photo[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_KIND(YIN_KIND_SYNC), YDEV__P_BAUD(115200), YDEV__P_RETRY, YDEV__P_SILENT,
    YDEV__P_READ, YDEV__P_NO_EL,
};
static const ydev_param ydev__params_triggerbox[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_BAUD(115200), YDEV__P_RETRY, YDEV__P_READ, YDEV__P_NO_EL,
    YDEV__P_PULSE(1e10, YDEV__PULSE_DOC), YDEV__P_HOLD(1, 250000),
};
static const ydev_param ydev__params_biosemi[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_BAUD(115200), YDEV__P_RETRY, YDEV__P_READ, YDEV__P_NO_EL,
    YDEV__P_PULSE(9e6, "0, or 7 to 9 ms: the interface ends every code after 8 ms"),
};
static const ydev_param ydev__params_mmbts[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_BAUD(9600), YDEV__P_RETRY, YDEV__P_READ, YDEV__P_NO_EL,
    YDEV__P_PULSE(1e10, "switch P: 0, or 7 to 9 ms (the box ends every code after 8 ms); S: " YDEV__PULSE_DOC),
    YDEV__P_LATCH, YDEV__P_HOLD(1, 250000),
};
static const ydev_param ydev__params_lines[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_BAUD(115200), YDEV__P_RETRY, YDEV__P_READ, YDEV__P_NO_EL,
    YDEV__P_PULSE(1e10, YDEV__PULSE_DOC), YDEV__P_HOLD(1, 250000),
};
static const ydev_param ydev__params_parallel[] = {
    YDEV__P_ROLE, YDEV__P_KEY, YDEV__P_RETRY, YDEV__P_READ, YDEV__P_NO_EL,
    YDEV__P_PULSE(1e10, YDEV__PULSE_DOC), YDEV__P_HOLD(0, 0),
};

YDEV_API const ydev_param* ydev_params(int family, int* n) {
#define YDEV__ROWS(a) t = a; k = (int)(sizeof a / sizeof a[0]); break
    const ydev_param* t = NULL;
    int k = 0;
    switch (family) {
    case YBOX_XID:        YDEV__ROWS(ydev__params_xid);
    case YBOX_LINE:       YDEV__ROWS(ydev__params_line);
    case YBOX_PHOTO:      YDEV__ROWS(ydev__params_photo);
    case YBOX_TRIGGERBOX: YDEV__ROWS(ydev__params_triggerbox);
    case YBOX_BIOSEMI:    YDEV__ROWS(ydev__params_biosemi);
    case YBOX_MMBTS:      YDEV__ROWS(ydev__params_mmbts);
    case YBOX_LINES:      YDEV__ROWS(ydev__params_lines);
    case YBOX_PARALLEL:   YDEV__ROWS(ydev__params_parallel);
    default: break;
    }
#undef YDEV__ROWS
    if (n) *n = k;
    return t;
}

/* --- the step ----------------------------------------------------------------------- */

static void ydev__close_conn(ydev_device* dev) {
    void* c;
    ydev__lock(ydev__omx(dev));
    dev->out_ready = 0;
    ydev__lock(ydev__mx(dev));
    c = dev->conn;
    dev->conn = NULL;
    ydev__unlock(ydev__mx(dev));
    ydev__unlock(ydev__omx(dev));
    if (c) dev->tr.close(c);
}

static void ydev__lose(ydev_device* dev, uint32_t why) {
    ydev__close_conn(dev);
    dev->lost_at = dev->st.last_rx_ns ? dev->st.last_rx_ns : ydev__now(dev);
    ydev__set_state(dev, YDEV_LOST, why);
    dev->next_try = ydev__now(dev) + dev->d.retry_ns;
}

static void ydev__fit_start(ydev_device* dev) {
    yrt_fit_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.mode = YRT_FIT_BRACKET;
    fd.ns_per_tick = ybox_ns_per_tick(dev->d.family);
    fd.tick_bits = 32;
    fd.max_width_ns = dev->d.max_width_ns;
    fd.clock_id = dev->d.device;
    fd.ring = dev->d.ring;
    (void)yrt_fit_open(&dev->fit, &fd);
    ybox_init(&dev->dec, dev->d.family, (uint8_t)dev->d.kind, dev->d.device);
    dev->probe_pending = 0;
}

/* OPENING or LOST: try the key once. */
static void ydev__try_open(ydev_device* dev) {
    char found[64];
    void* c;
    int r;
    int64_t now = ydev__now(dev);
    if (now < dev->next_try) return;
    dev->next_try = now + dev->d.retry_ns;
    found[0] = '\0';
    c = dev->tr.open(dev->tr.user, dev->d.key, found, sizeof found);
    if (!c) return;
    ydev__lock(ydev__mx(dev));
    dev->conn = c;
    ydev__unlock(ydev__mx(dev));
    snprintf(dev->st.found, sizeof dev->st.found, "%s", found);
    ydev__text(dev, "port", found);
    dev->st.last_rx_ns = 0;
    r = ydev__identify(dev);
    if (r == -2) { ydev__close_conn(dev); return; }
    if (r == -1) {
        ydev__close_conn(dev);
        /* under the lock: ydev_wait_running() writes the message too */
        ydev__lock(ydev__mx(dev));
        snprintf(dev->err, sizeof dev->err, "ysp_device: %s at %s did not answer as a %s device", dev->d.role,
                 found, ybox_family_name(dev->d.family));
        ydev__unlock(ydev__mx(dev));
        ydev__set_state(dev, YDEV_FAILED, YDEV_WHY_WRONG_DEVICE);
        return;
    }
    if (dev->st.ident[0]) ydev__text(dev, "ident", dev->st.ident);
    dev->st.opens++;
    if (dev->state == YDEV_LOST) {
        yrt_payload u;
        memset(&u, 0, sizeof u);
        u.i64[0] = dev->lost_at;
        u.i64[1] = ydev__now(dev);
        ydev__rec(dev, (uint16_t)YDEV_REC_GAP, &u);
    }
    ydev__fit_start(dev);
    dev->st.last_rx_ns = ydev__now(dev);
    dev->next_probe = ydev__now(dev);
    ydev__set_state(dev, YDEV_RUNNING, YDEV_WHY_IDENTIFIED);
    ydev__out_open(dev);
}

static void ydev__probe(ydev_device* dev) {
    uint8_t q[24];
    int n;
    int64_t now = ydev__now(dev), t0 = 0;
    if (dev->d.probe_ns < 0 || now < dev->next_probe) return;
    dev->next_probe = now + dev->d.probe_ns;
    if (++dev->probe_seq == 0) dev->probe_seq = 1;
    n = ybox_probe(dev->d.family, dev->probe_seq, q, (int)sizeof q);
    if (n <= 0) return;
    ydev__lock(ydev__omx(dev));
    n = ydev__write(dev, q, n, &t0);
    ydev__unlock(ydev__omx(dev));
    if (n < 0) { ydev__lose(dev, YDEV_WHY_DISCONNECTED); return; }
    dev->probe_pending = dev->d.family == YBOX_XID ? 1u : dev->probe_seq;
    dev->probe_t0 = t0;
    dev->st.probes++;
}

/* The event's uncertainty from the fit's own evidence: the device's tick
 * (an event is somewhere in its tick) plus the widest bracket the map
 * rests on; without brackets, the fit's spread (how late its points sit
 * above the map), which bounds the map more loosely. Not a measurement:
 * only a loopback gives that. */
static uint16_t ydev__unc(const ydev_device* dev, const yrt_fit_info* fi) {
    int64_t ns = (int64_t)ybox_ns_per_tick(dev->d.family) + (fi->width_ns > 0 ? fi->width_ns : fi->spread_ns);
    int64_t us = (ns + 999) / 1000;
    return (uint16_t)(us >= (int64_t)YIN_UNC_MAX ? YIN_UNC_MAX : (us < 1 ? 1 : us));
}

static void ydev__read(ydev_device* dev, int64_t timeout_ns) {
    int n, i;
    size_t used = 0;
    int64_t t;
    ybox_out o;
    if (!dev->tr.read) {
        /* an output-only transport: nothing to read */
        if (timeout_ns > 0 && !dev->d.now) (void)yrt_sleep_ns(timeout_ns);
        return;
    }
    n = dev->tr.read(dev->conn, dev->rx, (int)sizeof dev->rx, timeout_ns);
    t = ydev__now(dev);
    if (n < 0) { ydev__lose(dev, YDEV_WHY_DISCONNECTED); return; }
    if (n == 0) {
        if (dev->d.silent_ns > 0 && t - dev->st.last_rx_ns > dev->d.silent_ns) ydev__lose(dev, YDEV_WHY_SILENT);
        return;
    }
    dev->st.reads++;
    dev->st.bytes += (uint64_t)n;
    dev->st.last_rx_ns = t;
    memset(&o, 0, sizeof o);
    o.ev = dev->ev; o.ev_cap = YDEV_EVENTS; o.pair = dev->pair; o.pair_cap = YDEV_EVENTS;
    while (used < (size_t)n) {
        yrt_fit_info fi;
        bool mapped;
        ybox_out_clear(&o);
        used += ybox_decode(&dev->dec, dev->rx + used, (size_t)n - used, t, &o);
        dev->st.garbage += o.garbage;
        /* the pairs first, so the events of this read map through them */
        for (i = 0; i < o.n_pair; i++) {
            int64_t w = 0;
            int rc;
            if (o.pair[i].probe && o.pair[i].probe == dev->probe_pending) {
                w = t - dev->probe_t0;
                dev->probe_pending = 0;
                dev->st.answers++;
            }
            rc = yrt_fit_add(&dev->fit, o.pair[i].ticks, t, w);
            if (rc >= 0 && (rc & YRT_FIT_REJECTED)) dev->st.refused++;
            dev->st.pairs++;
        }
        yrt_fit_get(&dev->fit, &fi);
        mapped = fi.ready;
        for (i = 0; i < o.n_ev; i++) {
            yin_event* e = &o.ev[i];
            if (mapped) {
                int64_t m = yrt_fit_map(&dev->fit, e->ticks);
                if (m > t) { m = t; dev->st.clamped++; }
                e->t = m;
                e->unc_us = ydev__unc(dev, &fi);
            }
            dev->st.events++;
            /* the rule first: its write is the time-critical one, and the
             * sink only queues */
            if (dev->d.rule) (void)ydev_rule_input(dev->d.rule, e);
            if (dev->d.sink) dev->d.sink(dev->d.sink_ctx, e);
        }
        if (o.has_out) {
            /* the board's own time of its pin change */
            yrt_payload u;
            int64_t m = mapped ? yrt_fit_map(&dev->fit, o.out_ticks) : t;
            memset(&u, 0, sizeof u);
            u.i64[0] = m > t ? t : m;
            u.i64[1] = t;
            u.i64[3] = (int64_t)o.out_ticks;
            u.u32[8] = o.out_code;
            u.u16[18] = (uint16_t)(YDEV_OUT_REPORTED | YDEV_OUT_DEVICE_TIMED);
            ydev__rec_at(dev, (uint16_t)YDEV_REC_OUT, u.i64[0], &u);
            dev->st.reports++;
        }
    }
    if (dev->d.ring && dev->st.garbage && t - dev->last_garbage_rec >= 1000000000) {
        yrt_payload u;
        memset(&u, 0, sizeof u);
        u.u64[0] = dev->st.garbage;
        ydev__rec(dev, (uint16_t)YDEV_REC_GARBAGE, &u);
        dev->last_garbage_rec = t;
    }
}

static void ydev__step(ydev_device* dev, int64_t timeout_ns) {
    if (!dev->have_worker && dev->trail_token && ydev__now(dev) >= dev->trail_at) ydev__trail(dev, 0, false);
    if (dev->state == YDEV_OPENING || dev->state == YDEV_LOST) ydev__try_open(dev);
    if (dev->state == YDEV_RUNNING && dev->conn) {
        ydev__probe(dev);
        if (dev->state == YDEV_RUNNING) ydev__read(dev, timeout_ns);
    }
    ydev__publish(dev);
}

/* --- the thread --------------------------------------------------------------------- */

static void ydev__loop(ydev_device* dev) {
    if (!dev->d.no_elevate) (void)yrt_thread_elevate(NULL);
    while (!ydev__stopping(dev)) {
        if (dev->state == YDEV_RUNNING) {
            ydev__step(dev, dev->d.read_timeout_ns);
        } else {
            ydev__step(dev, 0);
            /* not there yet, or FAILED: the retry is timed by next_try */
            if (dev->state != YDEV_RUNNING) (void)yrt_sleep_ns(20000000);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI ydev__thread(LPVOID arg) { ydev__loop((ydev_device*)arg); return 0; }
#else
static void* ydev__thread(void* arg) { ydev__loop((ydev_device*)arg); return NULL; }
#endif

YDEV_API bool ydev_open(ydev_device* dev, const ydev_desc* desc) {
    if (!dev) return false;
    memset(dev, 0, sizeof *dev);
#define YDEV__FAIL(msg) do { snprintf(dev->err, sizeof dev->err, "ysp_device: %s", msg); return false; } while (0)
    if (!desc) YDEV__FAIL("NULL desc");
    if (!desc->role || !desc->role[0]) YDEV__FAIL("desc.role is required");
    if (desc->family < YBOX_XID || desc->family > YBOX_FAMILY_LAST) YDEV__FAIL("desc.family is not a YBOX_ family");
    if (!desc->key || !desc->key[0]) YDEV__FAIL("desc.key is required (MATCH KEYS)");
    if (desc->family == YBOX_PARALLEL && !desc->transport.open)
        YDEV__FAIL("YBOX_PARALLEL needs desc.transport = ydev_parallel_transport(&port)");
    if (strncmp(desc->key, "serial:", 7) != 0 && strncmp(desc->key, "port:", 5) != 0 && !desc->transport.open)
        YDEV__FAIL("desc.key must start with serial: or port:");
    if (desc->device == 0 && !desc->roles) YDEV__FAIL("desc.device must be > 0 (0 means unknown in ysp/input.h), or set desc.roles");
    if (desc->device != 0 && desc->roles) YDEV__FAIL("desc.roles gives desc.device: leave it 0");
    if (desc->pulse_ns < 0) YDEV__FAIL("desc.pulse_ns must be >= 0");
    if (desc->min_hold_ns < 0) YDEV__FAIL("desc.min_hold_ns must be >= 0");
    if (desc->pulse_ns > 0 && ybox_fixed_pulse_ns(desc->family) > 0 && !(desc->family == YBOX_MMBTS && desc->latched) &&
        (desc->pulse_ns < ybox_fixed_pulse_ns(desc->family) - 1000000 ||
         desc->pulse_ns > ybox_fixed_pulse_ns(desc->family) + 1000000))
        YDEV__FAIL("desc.pulse_ns: this family ends every code after 8 ms by itself");
    if (desc->kind < 0 || desc->kind > 15) YDEV__FAIL("desc.kind is not a yin_kind");
    if (desc->transport.open && (!desc->transport.write || !desc->transport.close ||
                                 (!desc->transport.read && (ybox_caps(desc->family) & YBOX_CAP_IN))))
        YDEV__FAIL("desc.transport needs open, read, write and close (read only for an output-only family)");
    if (strlen(desc->key) >= sizeof dev->key_buf) YDEV__FAIL("desc.key: at most 159 characters (YDEV_KEY_MAX)");
    if (strlen(desc->role) >= sizeof dev->role_buf) YDEV__FAIL("desc.role: at most 31 characters (YDEV_ROLE_MAX)");
    dev->d = *desc;
    /* the reader thread reads the key again at every reconnect: the
     * caller's strings need not outlive this call */
    memcpy(dev->key_buf, desc->key, strlen(desc->key) + 1);
    memcpy(dev->role_buf, desc->role, strlen(desc->role) + 1);
    dev->d.key = dev->key_buf;
    dev->d.role = dev->role_buf;
    if (desc->roles) {
        ydev_roles* r = desc->roles;
        int i = ydev_role_index(r, dev->d.role);
        if (i && r->dev[i - 1] && r->dev[i - 1] != dev) YDEV__FAIL("desc.role is bound to another running instance");
        if (!i) {
            if (r->n >= YDEV_MAX_ROLES) YDEV__FAIL("desc.roles is full (YDEV_MAX_ROLES)");
            snprintf(r->name[r->n], sizeof r->name[0], "%s", dev->d.role);
            r->dev[r->n] = NULL;
            i = ++r->n;
        }
        r->dev[i - 1] = dev;
        dev->role = i;
        dev->d.device = (uint32_t)i;
    }
    if (dev->d.kind == 0) dev->d.kind = dev->d.family == YBOX_PHOTO ? YIN_KIND_SYNC : YIN_KIND_BOX;
    /* the handle states the rate it opens (PARAMETERS) */
    if (dev->d.baud == 0 && dev->d.family != YBOX_PARALLEL) dev->d.baud = ybox_default_baud(dev->d.family);
    if (dev->d.probe_ns == 0) dev->d.probe_ns = 100000000;
    if (dev->d.retry_ns <= 0) dev->d.retry_ns = 1000000000;
    if (dev->d.silent_ns == 0) dev->d.silent_ns = 2000000000;
    if (dev->d.max_width_ns <= 0) dev->d.max_width_ns = dev->d.family == YBOX_XID ? 20000000 : 1300000;
    if (dev->d.read_timeout_ns <= 0) dev->d.read_timeout_ns = 20000000;
    /* the hold guards a USB write that stalls; an outb does not queue */
    if (dev->d.min_hold_ns == 0 && dev->d.family != YBOX_PARALLEL) dev->d.min_hold_ns = YSER_DEFAULT_MIN_HOLD_NS;
    if (dev->d.family == YBOX_PHOTO) dev->d.probe_ns = -1;   /* no queries */
    if (!(ybox_caps(dev->d.family) & YBOX_CAP_IN)) {
        /* an output-only device says nothing: silence is not a loss */
        dev->d.probe_ns = -1;
        dev->d.silent_ns = -1;
    }
    dev->tr = desc->transport.open ? desc->transport : ydev_serial_transport(dev);
    ydev__lock_init(ydev__mx(dev));
    ydev__lock_init(ydev__omx(dev));
#ifndef YRT_NO_THREADS
    if (ydev__host_pulse(dev) && (ybox_caps(dev->d.family) & YBOX_CAP_OUT) && !dev->d.now) {
        yrt_worker_desc wd;
        memset(&wd, 0, sizeof wd);
        wd.no_elevate = dev->d.no_elevate;
        if (!yrt_worker_open(&dev->worker, &wd)) {
            snprintf(dev->err, sizeof dev->err, "ysp_device: the trailing-edge worker did not start: %.100s",
                     yrt_worker_error(&dev->worker));
            ydev__lock_free(ydev__omx(dev));
            ydev__lock_free(ydev__mx(dev));
            if (dev->role) desc->roles->dev[dev->role - 1] = NULL;
            return false;
        }
        dev->have_worker = 1;
    }
#endif
    dev->state = YDEV_CLOSED;
    ydev__text(dev, "role", dev->d.role);
    ydev__set_state(dev, YDEV_OPENING, YDEV_WHY_START);
    dev->next_try = ydev__now(dev);
    dev->started = 1;
    ydev__publish(dev);
    if (dev->d.manual) return true;
#if defined(_WIN32)
    {
        HANDLE h = CreateThread(NULL, 0, ydev__thread, dev, 0, NULL);
        if (h) { memcpy(dev->thread_mem.b, &h, sizeof h); dev->have_thread = 1; }
    }
#else
    {
        pthread_t th;
        typedef char ydev__thread_fits[sizeof(pthread_t) <= sizeof dev->thread_mem ? 1 : -1];
        (void)sizeof(ydev__thread_fits);
        if (pthread_create(&th, NULL, ydev__thread, dev) == 0) {
            memcpy(dev->thread_mem.b, &th, sizeof th);
            dev->have_thread = 1;
        }
    }
#endif
    if (!dev->have_thread) {
#ifndef YRT_NO_THREADS
        if (dev->have_worker) { yrt_worker_close(&dev->worker); dev->have_worker = 0; }
#endif
        ydev__lock_free(ydev__omx(dev));
        ydev__lock_free(ydev__mx(dev));
        if (dev->role) dev->d.roles->dev[dev->role - 1] = NULL;
        dev->started = 0;
        YDEV__FAIL("the reader thread did not start");
    }
#undef YDEV__FAIL
    return true;
}

YDEV_API void ydev_close(ydev_device* dev) {
    if (!dev || !dev->started) return;
    ydev__lock(ydev__mx(dev));
    dev->stop = 1;
    if (dev->conn && dev->tr.interrupt) dev->tr.interrupt(dev->conn);
    ydev__unlock(ydev__mx(dev));
    if (dev->have_thread) {
#if defined(_WIN32)
        HANDLE h;
        memcpy(&h, dev->thread_mem.b, sizeof h);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
#else
        pthread_t th;
        memcpy(&th, dev->thread_mem.b, sizeof th);
        pthread_join(th, NULL);
#endif
        dev->have_thread = 0;
    }
    /* outputs end idle: a pending trailing edge now, then a held code */
#ifndef YRT_NO_THREADS
    if (dev->have_worker) { yrt_worker_close(&dev->worker); dev->have_worker = 0; }
    else
#endif
    if (dev->trail_token) ydev__trail(dev, 0, true);
    ydev__out_close(dev);
    ydev__close_conn(dev);
    ydev__set_state(dev, YDEV_CLOSED, YDEV_WHY_STOP);
    ydev__publish(dev);
    ydev__lock_free(ydev__omx(dev));
    ydev__lock_free(ydev__mx(dev));
    if (dev->role && dev->d.roles && dev->d.roles->dev[dev->role - 1] == dev) dev->d.roles->dev[dev->role - 1] = NULL;
    dev->started = 0;
}

YDEV_API int ydev_poll(ydev_device* dev, int64_t timeout_ns) {
    if (!dev || !dev->started || !dev->d.manual) return dev ? dev->state : YDEV_CLOSED;
    ydev__step(dev, timeout_ns < 0 ? 0 : timeout_ns);
    return dev->state;
}

/* The wait's failure message, under the lock: the reader thread writes the
 * FAILED message there. */
static int ydev__wait_fail(ydev_device* dev, int rc, int state) {
    ydev__lock(ydev__mx(dev));
    if (state != YDEV_FAILED || !dev->err[0])
        snprintf(dev->err, sizeof dev->err, "ysp_device: %s: %s (state %s)", dev->d.role,
                 ydev_strerror(rc), ydev_state_name(state));
    ydev__unlock(ydev__mx(dev));
    return rc;
}

YDEV_API int ydev_wait_running(ydev_device* dev, int64_t timeout_ns) {
    int64_t end;
    if (!dev) return YDEV_ERR_STATE;
    if (!dev->started) {
        snprintf(dev->err, sizeof dev->err, "ysp_device: ydev_wait_running: not open");
        return YDEV_ERR_STATE;
    }
    end = ydev__now(dev) + (timeout_ns > 0 ? timeout_ns : 0);
    for (;;) {
        int64_t before = ydev__now(dev), now;
        /* a poll (manual mode) steps the instance; its timeout only bounds
         * a read, which happens only when RUNNING */
        int s = dev->d.manual ? ydev_poll(dev, 0) : ydev_state(dev);
        if (s == YDEV_RUNNING) return YDEV_RUNNING;
        if (s == YDEV_FAILED || s == YDEV_CLOSED) return ydev__wait_fail(dev, YDEV_ERR_STATE, s);
        now = ydev__now(dev);
        if (now >= end) return ydev__wait_fail(dev, YDEV_ERR_TIMEOUT, s);
        if (dev->d.now) {
            /* the caller's clock: a poll that does not move it never will */
            if (now == before) return ydev__wait_fail(dev, YDEV_ERR_TIMEOUT, s);
        } else {
            (void)yrt_sleep_ns(end - now < 1000000 ? end - now : 1000000);
        }
    }
}

YDEV_API int ydev_state(ydev_device* dev) {
    int s;
    if (!dev || !dev->started) return YDEV_CLOSED;
    ydev__lock(ydev__mx(dev));
    s = dev->snap.state;
    ydev__unlock(ydev__mx(dev));
    return s;
}

YDEV_API void ydev_get_stats(ydev_device* dev, ydev_stats* out) {
    if (!out) return;
    if (!dev || !dev->started) {
        if (dev) *out = dev->snap; else memset(out, 0, sizeof *out);
        return;
    }
    ydev__lock(ydev__omx(dev));
    ydev__lock(ydev__mx(dev));
    *out = dev->snap;
    ydev__unlock(ydev__mx(dev));
    out->outs = dev->outs;
    out->out_errors = dev->out_errors;
    ydev__unlock(ydev__omx(dev));
}

YDEV_API const char* ydev_error(const ydev_device* dev) { return dev ? dev->err : "ysp_device: NULL device"; }

YDEV_API yin_source ydev_source(const ydev_device* dev) {
    yin_source s;
    memset(&s, 0, sizeof s);
    if (!dev) return s;
    s.kind = (uint8_t)dev->d.kind;
    s.device = dev->d.device;
    s.tier = (uint8_t)YIN_TIER_UNKNOWN;
    s.note = dev->d.family == YBOX_XID ? "ysp_device xid: box clock fitted (BRACKET), no loopback"
           : dev->d.family == YBOX_LINE ? "ysp_device line: board clock fitted (BRACKET), no loopback"
           : dev->d.family == YBOX_PHOTO ? "ysp_device photo: board clock fitted (LATE), no loopback"
                                         : "ysp_device: output only, no loopback";
    return s;
}

#endif /* YSP_DEVICE_IMPLEMENTATION_GUARD */
#endif /* YSP_DEVICE_IMPLEMENTATION */

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
