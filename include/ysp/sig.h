/* ysp/sig.h - v0.6.1 - public domain single-header signal path
 *
 *   Continuous input to stimuli: a small graph, fixed at open, from input
 *   sources through per-sample transforms and maps to stimulus channels,
 *   detectors, derived events and records. A cursor from a raw mouse is one
 *   graph; one cursor from two hands is another. The graph is data, so a
 *   loader can check it before a session and a run can be replayed bit for
 *   bit (docs/signal_spec.md is the design; docs/sig.md the manual).
 *
 *   REQUIRES ysp/input.h beside it (the input event, its kinds and tiers).
 *   Pure computation: no heap, no OS calls, no threads, no file I/O, no
 *   clock. The caller gives the state arena (ysig_arena_size()). C99 is the
 *   floor: it builds as C99, C11, C++17 and C++20.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_SIG_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header. A cursor from
 *   one raw mouse at 0.25 px per count, turned 30 deg about the start, on
 *   two channels that ygfx_apply() reads:
 *
 *       ysig_node n[4];
 *       n[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
 *       n[0].p[YSIG_SOURCE_KIND] = YIN_KIND_MOUSE;
 *       n[0].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_DELTA;
 *       n[0].lanes = 2;
 *       n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);      // hand, px
 *       n[1].p[YSIG_INTEGRATE_GAIN_X] = n[1].p[YSIG_INTEGRATE_GAIN_Y] = 0.25;
 *       n[1].p[YSIG_INTEGRATE_ORIGIN_X] = sx;
 *       n[1].p[YSIG_INTEGRATE_ORIGIN_Y] = sy;
 *       n[2] = ysig_node_of(YSIG_OP_AFFINE, 1, -1);         // cursor
 *       n[2].p[YSIG_AFFINE_PRE_X] = n[2].p[YSIG_AFFINE_POST_X] = sx;
 *       n[2].p[YSIG_AFFINE_PRE_Y] = n[2].p[YSIG_AFFINE_POST_Y] = sy;
 *       n[2].p[YSIG_AFFINE_ANGLE] = -30;                    // y down: ccw on screen
 *       n[3] = ysig_node_of(YSIG_OP_BIND, 2, -1);           // channels 0, 1
 *       ysig_desc d = { 0 };
 *       d.nodes = n; d.n_nodes = 4;
 *       d.channels = values; d.n_channels = 2;
 *       d.arena_size = ysig_arena_size(&d);
 *       d.arena = arena;                                    // that many bytes
 *       if (!ysig_open(&g, &d)) { fputs(ysig_error(&g), stderr); return 1; }
 *       ysig_reset(&g, t0);
 *       // every frame, after the input is polled:
 *       ysig_feed(&g, &e);                                  // each input event
 *       fi.onset_ns = f.onset; fi.poll_ns = yrt_now_ns();  // input known to here
 *       fi.has_poll = true;
 *       ysig_frame(&g, &fi);                                // binds, records
 *       ygfx_apply(binds, 2, values);
 *
 *   ---------------------------------------------------------------------
 *   THE GRAPH (ysig_node)
 *   ---------------------------------------------------------------------
 *   An array of nodes. A node has an op, up to 4 inputs (other nodes'
 *   indices, -1 = none, leading, no gaps), 16 parameters p[] whose meaning
 *   is the op's table (ysig_op_params()) and whose indices have names
 *   (YSIG_DETECT_LEVEL is detect's row "level"), an optional data array
 *   (sines' components), and one output of 1 to 4 lanes (x, y, z, w). Each input
 *   comes before its user: an input at or after the node is refused, so a
 *   cycle cannot be built. node.lanes 0 takes the op's rule. Start from
 *   ysig_node_of(), which sets unused inputs to -1: a zeroed in[] reads
 *   node 0 four times. Durations in p[] are seconds, as in a definition.
 *   Arithmetic is double; a channel is float. "Nonzero" (a selector, a
 *   trigger, a gate, all's inputs) counts NaN as zero.
 *
 *   Ops (lanes in, out):
 *     source     0, 1..4   the samples of one kind, type, control and
 *                          device (0 = any): fields x, y, value, aux
 *     role       0, type   a role's value (ROLES AND ADAPTERS)
 *     integrate  n, n      a raw mouse's DELTA (kind MOUSE): an int64 count
 *                          sum per lane, position = origin + gain x counts,
 *                          the count sum held inside optional bounds
 *                          (yrsp_cursor's rule); otherwise rate control:
 *                          gain x held value x dt
 *     lowpass    n, n      first or second order, on each sample's dt
 *     velocity   n, n      the difference over at least `lag` s
 *     speed      n, 1      its norm
 *     delay      n, n      the input as it was `delay` s ago
 *     deadzone   n, n      radial: 0 inside `radius`, then shifted by it
 *     affine     2, 2      out = M_eff (in - pre) + post; M_eff = Rot(angle) M,
 *                          angle = p8 + input 1 (deg); a zero term is skipped
 *     scale      n, n      per lane, times input 1 (1 lane) when given
 *     offset     n, n      plus per lane
 *     clamp      n, n      low, high per lane
 *     select     n, m      out lane k = input lane p[k]
 *     join       k x n, sum(n) <= 4   lanes concatenated
 *     sum, mean  k x n, n  weighted; mean divides by the weights' sum
 *     diff       2 x n, n  input 0 minus input 1
 *     mux        a, b, s   input a when lane 0 of s is nonzero, else b; a and
 *                          b the same lanes; out = the chosen input, with
 *                          its time, source and tier (a sample of either
 *                          input runs its users: repeated rows and events
 *                          carry the chosen input's value and time)
 *     latch      n, n      passes input 0 until lane 0 of input 1 goes from
 *                          zero to nonzero, then holds until a reset
 *                          (CROSSINGS AND LATCHES)
 *     distance   2-3, 1    the Euclidean distance to a point (p, or a second
 *                          input of the same lanes)
 *     peak       n, 1      the largest of a lane, or the norm, since the
 *                          reset or since a gate input last rose; holds
 *                          while the gate is zero; t is the peak sample's
 *     all        2-4, 1    1 when lane 0 of every input is nonzero
 *     silent     n, 1      1 once no sample has arrived for `after` s,
 *                          judged at the poll; before any sample, from the
 *                          reset; a view
 *     sines      0, 1..2   A(t) sum a cos(2 pi f t + phi) at the frame's
 *                          base time; A ramps 0 to 1 over `ramp` s
 *     noise      0, 1..4   seeded Gaussian, optionally low-passed
 *     channel    0, 1      a timeline channel's value at the frame
 *     bind       n, -      writes desc.channels[p0 + lane] at each frame
 *     detect     n, 1      a test with a hold (DETECTORS); out = the test,
 *                          or (p13 out = hit) 1 from a hit until ready
 *     record     n, -      one row per sample or per frame (RECORDS)
 *     emit       n, -      a derived yin_event per update (DETECTORS)
 *     custom     YSIG_OP_CUSTOM + i: desc.custom[i] (CUSTOM OPS)
 *
 *   ---------------------------------------------------------------------
 *   SCHEDULES AND TIME
 *   ---------------------------------------------------------------------
 *   ysig_open() compiles a list per source (every node the source reaches,
 *   in order) and a frame list, so no evaluation walks the graph. A sample
 *   runs its source's list and nothing else. A node that two sources
 *   reach is on both lists and reads its inputs' held values.
 *   A value's t, device, unc_us and source node: in a sample's pass, from
 *   the input that changed, along the path (the report that arrived, also
 *   when another source's held part is newer in time; after a delay, the
 *   delayed sample's); in a frame's pass and in ysig_value_at(), from the
 *   newest input. Emit and record carry the value's true t, which need
 *   not rise in arrival order (as ysp/input.h's events). A detect's hold
 *   clock never goes back: a hold starts and ends on max(new t, its last
 *   t), clamps counted in ysig_stats.time_clamped; a hit at a crossing
 *   has the time of the value that met the test. A value also carries t_oldest (the oldest
 *   input's time) and its source, and the worst stamp tier of its inputs (SIM best, then 1, 2,
 *   3, UNKNOWN worst). A source's tier is the event's stamp, or p4 where
 *   it is 0; before its first sample a source has no value: tier UNKNOWN,
 *   nothing in its ring. Order is kept per source and device: a sample
 *   older than the last of its device is counted and dropped
 *   (ysig_stats.dropped_order); equal times pass; a raw mouse's counts
 *   are never dropped (a sum does not depend on order). A source keeps 8
 *   devices; a ninth takes the slot of the least recently seen one
 *   (ysig_stats.order_evicted), whose next late sample then passes.
 *   Times saturate at the ends of int64.
 *   ysig_frame() runs, in node order: the generators (sines, noise,
 *   channel, a custom op with no input) at the frame's base time; the
 *   nodes that change with time alone (velocity, speed, delay, lowpass,
 *   rate integrate); every node after those; then the sinks: bind, frame
 *   records, detects. The time-alone nodes give a view, state unchanged,
 *   up to min(onset, f.poll_ns). An event a source took that is stamped
 *   after the poll is counted (ysig_stats.late_poll, late_poll_max_ns)
 *   and moves nothing; a view whose last sample is past the poll stands.
 *   A poll a little behind the newest sample is normal and changes
 *   nothing. A stamp more than YSIG_FUTURE_NS (1 s) past a frame's poll
 *   is a bad one: a source's order slots and a silent's last time come
 *   down to the poll, so later samples flow again one frame later and a
 *   silent recovers. That stamp's effect on rings, filters, velocity,
 *   delay, a rate integrator (it takes one step over the bad time, then
 *   stays frozen) and a hold's clock stays until the node's next reset
 *   (ysig_reset_trial() for the experiment's nodes, ysig_reset() for an
 *   adapter's); ysig_stats.late_poll is the alarm. A frame with no poll
 *   has no such recovery.
 *   ysig_frame() refuses only bad arguments, and a frame without a poll
 *   (has_poll false, as in a zeroed ysig_frame_in) in a graph with a view
 *   (velocity, speed, delay, lowpass, rate integrate, silent), on every call:
 *   never because of the data. poll_ns 0 with has_poll is a valid time.
 *   A reset does not change the newest event's time. a lowpass or a rate
 *   integral runs to the poll, a delay reads at most there, and a
 *   velocity or speed is 0 only when its input was silent for the lag up
 *   to the poll (else the last sample's value stands). A sink reads held
 *   values: poll and feed the frame's input before ysig_frame()
 *   (docs/examples_findings.md, D10).
 *   A value at another time: ysig_value_at() for a node with
 *   node.history > 0 (a ring of its values at sample times; a frame's
 *   view never enters it) reads the last value at or before t; a
 *   stateless node after two sources (join, mean, affine of those), or
 *   with a generator or another frame-updated input, is computed from its
 *   inputs' values at t, so their rings are kept instead (a generator's
 *   ring holds its frames). For a flip record that arrives a frame later. Integrate,
 *   lowpass, velocity, speed, delay, peak and silent keep state per
 *   sample: with no source upstream they are refused at open. A mux, all or
 *   distance after a frame-updated input (a channel selector) reads its
 *   inputs' rings, as above; a latch, a peak and a silent keep their own
 *   ring of sample-time values (a latch's does not read a generator part
 *   of its input back at a past time; a peak's entry time is the later of
 *   the sample's and the ring's last, so a late larger peak is kept;
 *   silent: its input's, so a past read judges at that time, at most at
 *   the last poll); a detect's ring holds its output at each value it
 *   tested. An emit on a frame-updated node emits at every frame.
 *
 *   ---------------------------------------------------------------------
 *   DETECTORS (detect, latch, emit, ysig_detected, ysig_derived)
 *   ---------------------------------------------------------------------
 *   detect tests its input: >= or <= on a lane or on the norm of (value -
 *   center), inside or outside a rectangle on lanes 0 and 1. The crossing
 *   rule is ysp/response.h's (CROSSINGS): the first value past the test
 *   while ready is a hit at that value's t; ready again when the test is
 *   false with the level moved back by the hysteresis. A value that meets
 *   the test when the detect is armed is held: no hit until it re-arms.
 *   With hold > 0 a hit needs the test true for hold s: the hit is at
 *   start + hold, found by the next value (before it is tested) or by a
 *   frame whose onset is at or after it, so a still hand that sends no
 *   samples completes a hold. A hold armed while its test is true counts
 *   from the arm (p12 entry arm, 0: a hand already on the start), or
 *   waits for the test to fail and come true again (entry fresh, 1).
 *   AN ARM IN THE PAST (v0.6.0): ysig_arm() at a time t before input the
 *   graph already has (N: the newest input's time since the last reset,
 *   or the reset's time). The result is that of a reference order R of
 *   the same calls (docs/sig.md, "An arm in the past: the semantics"):
 *   input keeps call order; the state calls made with no input between
 *   them and an arm (set, set_data, disarm, recenter) are its group and
 *   move with it; a past group goes as one block after the last call that
 *   must come before it: a barrier (reset, ysig_flush()), or a call at or
 *   before t whose footprint (the nodes it can change) meets the
 *   group's. Input that shares no node with the group passes it. A group
 *   placed after a later input it shares a node with (a late report of
 *   its own input) is pinned, counted (arm_past_pinned). An arm ahead of
 *   the input (t later than N) waits, its group with it, until a call
 *   later than t that meets it (input, or a state call at N); a disarm
 *   of its detect at or before t cancels it, a barrier cancels every
 *   waiting arm (arm_ahead_cancelled). At its place an arm first meets
 *   its detect's hold up to t, then arms; so does a disarm at its time.
 *   A change an arm makes to a detect's output (a fall, a rise) goes
 *   through every user as a sample's value. The result is verified for
 *   the shapes docs/sig.md lists ("Exactness"); an arm whose group is
 *   outside them is made as usual and counted once (arm_unverified).
 *   With desc.catchup_frames above 0 the graph keeps a window: a
 *   checkpoint of its state after each frame and each reset (the last
 *   catchup_frames of them), every call since the oldest, and the old
 *   value of every ring slot those calls wrote. The arm goes into the
 *   call log at its place; the graph goes back to the newest checkpoint
 *   at or before it and runs the logged calls again. Every node then
 *   holds what the calls in that order leave (values, rings, states,
 *   stats and the digest): it is the same code on the same calls. At its
 *   place an arm first meets its detect's hold up to t (any detect: on a
 *   view or at frame rate too): that hold ended under the earlier arm.
 *   Hits the caller already had are not queued again, but for the armed
 *   detect's from the new arm on: those are the new arm's, also when its
 *   earlier arm handed out an equal one; with a window the queues hold
 *   catchup_calls beyond their 64. A hit handed out that
 *   the new history does not give is counted: the armed detect's own
 *   later than the arm's time in arm_hits_superseded (its earlier arm's:
 *   a caller re-arming at t took none of them); any other, another
 *   detect's at any time too, in ysig_stats.arm_hits_retracted (a caller
 *   may have taken it: the caller must watch this). on_row does not get
 *   the window's rows again; a change in any of their columns is counted
 *   (arm_rows_stale). v0.7.0: a hit or a derived event is the caller's
 *   once ysig_detected_to() or ysig_derived_to() handed it out; the run
 *   again makes the queued ones again (no count) and queues what it gives
 *   that the caller does not have. Only handed-out ones count: hits as
 *   above, events in arm_derived_stale when the run again does not give
 *   them again in their node's sequence.
 *   With desc.rows_final (derived_final) they come only once no arm can
 *   change them, up to catchup_frames frames late, and are R's. A reset
 *   and ysig_flush() are barriers: no arm goes before one. An arm whose
 *   place is before the window's oldest checkpoint, or after its logs ran
 *   full (catchup_calls calls, catchup_writes ring writes), is made at its
 *   call on the input seen, counted (arm_past_far, arm_past_full); with no
 *   window (catchup_frames 0, a zeroed desc) the same, counted in
 *   arm_past_off. ysig_arm() from a callback (on_row,
 *   the journal, a custom op) during another call is refused and counted
 *   (arm_reentry); so is any other call that changes the graph
 *   (call_reentry): it would run inside that call but be logged after
 *   it, so a run again and a replay would run it in another order. A
 *   custom op runs again in a run again: one with
 *   effects outside its state repeats them. The cost of such an arm is
 *   the calls since that checkpoint, run again; the window adds a copy of
 *   the state at each frame (docs/sig.md, "An arm in the past"). No frame
 *   is run twice outside the window; the journal holds the arm at its
 *   call, and a replay runs the same window again.
 *   Entry now (2) with no hold: a test true at the arm is a hit at the
 *   arm's time with the input's held value, also at a reset that arms
 *   (a cursor already in a target; the output rises after the reset);
 *   a frame-rate detect armed between frames tests at the next onset
 *   instead; an input with no sample since its reset (tier UNKNOWN)
 *   has nothing held, so its first value is tested. With a hold, now
 *   is arm.
 *   A sample-rate detect on a frame-updated input (a view, a mux or a
 *   latch of a channel): in a frame's pass its hold's clock is the time
 *   the frame knows input up to, min(poll, onset), while the test is
 *   true, because a still input's value keeps its old t and the hold
 *   would never end. So its hold completes at a poll, up to a frame after
 *   the onset that would end a hold on samples. A hit is at start + hold
 *   on that clock. A frame-rate detect tests at the onset. p6: armed at
 *   reset (0) or by ysig_arm().
 *   p7: one hit per arm (0) or every crossing. A detect at frame rate
 *   (p5) tests the held value at each onset. Hits queue (64, the rest
 *   counted) until ysig_detected().
 *   A detect's output may feed a mux, an all, a latch or a peak's gate. It
 *   is the test's state by default (p13 out test); with out hit (1) it is
 *   1 from a hit until the detect is ready again, and until the next arm
 *   or reset for a one-hit or a disarmed detect, so it rises at a hold's
 *   completion, which may be in a frame's pass. Its rise is at the hit's
 *   time, also in its ring, with the sources of the value that met the
 *   test; a later evaluation of a standing hit is at its own time, with
 *   its input's sources, as any value. ysig_arm() takes it to 0 for its users at
 *   once, at the arm's time, so the next hit is a new rise. Before that,
 *   an arm at t completes a sample-rate hold met by t (v0.6.0, as
 *   ysig_settle() at t would): the hold ended under the earlier arm, so it
 *   is that arm's hit, also when no input after it has found it yet.
 *   CROSSINGS AND LATCHES: a latch takes its value when the trigger's lane
 *   0 goes from zero to nonzero (NaN is zero); a trigger already nonzero
 *   at a reset holds no rise. A rise in a sample's pass holds that sample's
 *   value and its t. A rise in a frame's pass (a channel set by a step, a
 *   frame-rate detect, a hold that a frame completes) has no sample: the
 *   latch holds the input's held value with that value's t. When the
 *   trigger is a detect with out hit on the same input as the latch, the
 *   latch holds the hit's value and time (ysig_hit t and value, for a hold
 *   start + hold and the value at its start) so the shown endpoint and the
 *   recorded hit are the same, also when a frame completes the hold.
 *   Otherwise record the latch's own output for the shown value.
 *   emit makes a derived yin_event of its input at each update, for the
 *   response collector (docs/signal_spec.md section 7, way (a)): kind
 *   YIN_KIND_USER + p0, device the emit node's index, control p1, type
 *   SAMPLE, x y value aux the lanes (float), t the value's t (the report
 *   that arrived), unc_us that report's, stamp the value's tier, code the
 *   arriving report's source node | the oldest input's << 16. The source
 *   table's bounds (yin_source lo_ns, hi_ns) do not travel: the
 *   collector sees kind USER, so its result's stamp_lo_ns and stamp_hi_ns
 *   are 0; take them from the hand's source entry.
 *   ysp/input.h has no flag for a derived event: tell it by kind and
 *   device (docs/sig.md, "Derived events"). Queued (64) until
 *   ysig_derived().
 *
 *   ---------------------------------------------------------------------
 *   RECORDS AND REPLAY
 *   ---------------------------------------------------------------------
 *   record calls desc.on_row with a ysig_row: per sample of its input
 *   (p0 = 0) or per frame (p0 = 1: the frame index, the onset, the held
 *   value). A row has its value's true t: a sample stream may be out of
 *   time order when two sources feed the node. Every row, hit and derived event, every bound channel value
 *   and every frame's index and onset go into a 64-bit digest
 *   (ysig_stats).
 *   desc.journal gets every call that changes the graph: reset, reset_trial,
 *   recenter, set, set_data, arm, disarm, every event a source takes or
 *   drops, every frame (with its channel values, valid during the call).
 *   ysig_replay()
 *   runs such a journal on a graph opened from the same desc and gives
 *   the same values, rows, hits and digest bit for bit.
 *
 *   ---------------------------------------------------------------------
 *   ROLES AND ADAPTERS (ysig_role, ysig_adapter)
 *   ---------------------------------------------------------------------
 *   The experiment's nodes read roles (op role, p0 the role's index): a
 *   name, a type (position2d, position2d_relative, position3d, scalar,
 *   gaze) and units (device, workspace_cm, display_cm, display_deg,
 *   display_px). The rig supplies one adapter per role: nodes that end in
 *   the role's value. At open an adapter of another type or units is
 *   refused; a position2d role takes a position2d_relative adapter only
 *   with allow_relative. The experiment's nodes keep their indices 0..n-1;
 *   adapter a's node i is ysig_adapter_node(g, a, i).
 *   Between trials ysig_reset_trial() resets the experiment's nodes only
 *   (D19): a hand does not jump, so an adapter's nodes keep their state
 *   (the source's order slots, integrators, filters, rings). With no
 *   adapters it is ysig_reset(). ysig_recenter(g, role, x, y, t) places a
 *   relative role at time t: role x, y (in the role's units) go back
 *   through the adapter's scale and offset nodes to the integrator, whose
 *   count grid then continues from that point, with a new ring entry at t
 *   (values before t stand) and, for a rate integrator, no jump by the
 *   time since its last sample. t must be no later than the input already
 *   fed, normally the poll's time: a planned onset is ahead of samples
 *   not yet fed, which a ring would then skip. It refuses an absolute adapter, an adapter
 *   with no integrator, another op between the integrator and the role,
 *   a point not finite after that chain, and a point outside the
 *   integrator's bounds. After the integrator, velocity, speed, lowpass
 *   and delay restart (the jump would spike them) and stateless nodes take
 *   the new value; a rate integrator, peak, latch, silent, detect and
 *   custom op keep their state (a custom op gets no reset()). Detects see
 *   the point at their next value, so call it before ysig_reset_trial() or
 *   ysig_arm() when a hold must start from it.
 *
 *   ---------------------------------------------------------------------
 *   CUSTOM OPS (ysig_op_vtable)
 *   ---------------------------------------------------------------------
 *   desc.custom[i] is op YSIG_OP_CUSTOM + i: lanes per input (0 = any) and
 *   out (0 = input 0's), state_size bytes in the arena, a parameter table,
 *   reset, sample (with inputs) and frame (without: a generator). The
 *   graph fills out's t, tier and sources before sample(); the op writes
 *   the lanes. frame() gets the onset, not the poll: a custom op cannot
 *   implement a view. No allocation, no I/O in the calls. version must be
 *   YSIG_OP_ABI.
 *
 *   ---------------------------------------------------------------------
 *   LIMITS, THREADS AND MEMORY
 *   ---------------------------------------------------------------------
 *   YSIG_MAX_NODES (512) nodes with the adapters'; 64 hits and 64 derived
 *   events between drains; 4 lanes. Nothing is allocated: the arena holds
 *   the nodes, values (64 bytes each), schedules, state, rings and queues.
 *   One thread, the frame thread. Errors: ysig_open() writes
 *   ysig_error(); ysig_check() writes the caller's buffer.
 *
 *   ---------------------------------------------------------------------
 *   NOT DONE
 *   ---------------------------------------------------------------------
 *   The ops rectify, envelope, curve and units; pose (7 values); a stream
 *   sample source (ysig_stream_sample is defined, not read); prediction;
 *   the JSON loader and the rig profile's adapter block; a second latch in
 *   one trial; ysig_recenter() through an affine.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.6.1 - pending groups (docs/sig.md, "Pending groups"): a past group
 *          of an arm and disarms only, whose input time at its place is
 *          before its time and with no L in R yet, is made at its place
 *          as before and decided later instead of counted at once: at its
 *          call when a read sees a change, at L or a state call at the
 *          input time (exact), counted at an input at or before its time,
 *          a call of another waiting group or a barrier. Two hands: the
 *          second hand's control, armed at a hit the first hand's sample
 *          found, counted one arm_unverified at each entry. No API or
 *          struct change.
 *   v0.6.0 - an arm in the past is exact: the review of 0.5.0 found its
 *          catch-up right for one source with rising times only. Now a
 *          window (desc.catchup_frames, _calls, _writes) of checkpoints,
 *          calls and ring writes, and an arm in the past runs the graph
 *          again from a checkpoint with the arm placed where it would have
 *          been made. ysig_settle(): holds that input fed up to a time
 *          completes, journaled (YSIG_J_SETTLE); an arm at t does the
 *          same for its detect first (any detect; ahead of the input, up
 *          to the newest input). A reset is a barrier. The third review
 *          (2026-11-13): the arm's place by time order (after sets,
 *          disarms, recenters and arms at or before t, after a late
 *          report at or before t), a run again's queue room,
 *          every column hashed for the stale counts, ysig_set() of a
 *          channel node above the open's channels refused,
 *          ysig_stats.arm_reentry (a callback's ysig_arm() refused).
 *          The second review of that round: a standing hit output's later
 *          evaluations are at their own time again (the round had kept
 *          the hit's time, so a hold on the output did not complete from
 *          samples and a late report's hit skipped every later ring
 *          entry); another detect's hit that a run again voids is
 *          retracted, not superseded; ysig_feed(), ysig_frame(),
 *          ysig_set() and every other call that changes the graph are
 *          refused from a callback, custom ops included
 *          (ysig_stats.call_reentry, a struct change).
 *          The semantics written for an independent oracle
 *          (docs/sig.md): groups, footprints, the reference order's
 *          placement, pinned groups (arm_past_pinned), arms ahead of the
 *          input that wait and their cancels (arm_ahead_cancelled), an
 *          arm's change through every user, the input time after a reset
 *          its time, a disarm's hold, final-only rows and derived events
 *          (desc.rows_final, derived_final, catchup_rows), ysig_flush()
 *          (YSIG_J_FLUSH), images that keep 64 hits (struct changes).
 *          The oracle's rounds: an ahead group waits whole, its triggers
 *          too, and its arm lets in only when the group goes in; a past
 *          group's calls let waiting groups in at its place; final-only
 *          events in the queue or handed out during a run again stay; a
 *          checkpoint dropped for held rows is a full log; an arm going
 *          in lets an equal time's later group in after itself; a past
 *          arm lets in at its place; with no checkpoint the log is not
 *          read; a gap's disarms at its end in the log too. The bound of
 *          what is verified: ysig_stats.arm_unverified (a struct change),
 *          the conditions of docs/sig.md, "Exactness". The reviewer's
 *          oracle and spec cases are in ctest. The check round: 2 (b)'s
 *          exception needs G's arm in R before H's calls, by R's order of
 *          the groups at G's time that wait for its L (the oracle's case
 *          1061), tested again over every call between G and L; a waiting
 *          group's disarms after its other calls, as a gap's (3578).
 *          Stats arm_caught_up, arm_past_off, _far, _full,
 *          arm_hits_retracted, arm_hits_superseded (the armed detect's own
 *          hits at or after the arm's time: its earlier arm's),
 *          arm_rows_stale, arm_derived_stale replace 0.5.0's two; the
 *          graph's framed goes, t_cover and cu come (struct changes).
 *   v0.5.0 - an arm in the past (AN ARM IN THE PAST, under DETECTORS): a
 *          sample-rate detect armed before its input's newest value or a
 *          frame's onset that tested it catches up on the input's ring;
 *          a hold a frame already covers is a hit at start + hold at the
 *          arm's call (the experiment format's 3.11 rule 2 in the
 *          player). ysig_stats.arm_caught_up and arm_no_history, and the
 *          graph's framed flag (struct changes).
 *   v0.4.1 - the review of 0.3.0 and 0.4.0: an entry-now hit at a reset
 *          rises after every node's reset, so a latch freezes and a peak's
 *          gate restarts, as at ysig_arm(); no entry-now hit at an arm on
 *          an input with no sample since its reset (tier UNKNOWN): the
 *          first value is tested, ready; ysig_set()'s recompute changes
 *          the lanes only (t, sources and tier stay) and gives a node
 *          with a ring an entry at the set's time, the later of the
 *          newest event fed and the last frame's onset, so
 *          ysig_value_at() at or after it gives the new value and before
 *          it the old (a node computed from its inputs at a past time
 *          uses the parameters it has now).
 *   v0.4.0 - ysig_set() on a stateless node recomputes its held value
 *          and its stateless users' from their inputs' held values at
 *          once (gap G17 of the experiment format), journaled as before,
 *          so a block's new transform reaches a still hand's cursor
 *          before its next report. No ring entry is added.
 *   v0.3.0 - detect's entry now (2, YSIG_ENTRY_NOW; gap G16 of the
 *          experiment format, D35 (f)): with no hold, a test true at the
 *          arm is a hit at the arm's time, its output rising then; at
 *          frame rate the next onset tests it. Arm and fresh unchanged.
 *   v0.2.1 - a ring push returns at once on a ring of no capacity, so
 *          MSVC /W4 /WX (C4724, "potential mod by 0") builds it; no
 *          ring has capacity 0, so nothing else changes.
 *   v0.2.0 - the ops mux, latch, distance, peak, all and silent (D17,
 *          D18, the paper test); detect's output as an input, and
 *          detect's `out` row (hit); ysig_reset_trial() and ysig_recenter()
 *          (D19, journaled); the C++20 enum-float comparisons fixed.
 *   v0.1.0 - first version: the ops above, the schedules, detectors,
 *          derived events, records, journal and replay, roles and
 *          adapters, custom ops, parameter tables, and a name for
 *          each p[] index (YSIG_<OP>_<ROW>).
 *
 *   STATUS: v0.6.1, 2026-11-14, the same machine: the oracle at 3,000
 *   cases on seeds 20261114 and 778001 to 778004 and at 20,000 on 61441:
 *   both targets hold, the same wrong cases as 0.6.0, 32 % fewer arms
 *   counted in right cases; ctest Release and the sanitizers tree pass;
 *   323 of 323 mutants as expected; docs/sig.md has the numbers.
 *   STATUS: v0.6.0, the check round, 2026-11-14, the same machine: the
 *   oracle at 3,000 cases on 17 seeds and at 20,000 on seed 31415
 *   (Release): every case its B cannot run counted (23,218), every wrong
 *   case with a count (98; case 2201 by arm_past_pinned, which the
 *   oracle's own tally leaves out); ctest Release and the sanitizers
 *   tree pass; 315 of 315 mutants as expected.
 *   STATUS: v0.6.0, 2026-11-14, Linux desktop (i5-14500, GCC 12.2):
 *   sig_test as C, and as C and C++17 under ASan and UBSan; the
 *   reviewer's oracle (tests/adapt/sig_arm_ref.c, written from
 *   docs/sig.md) at 20,000 cases under ASan and UBSan: every case its B
 *   cannot run counted in arm_unverified (6,406), every wrong case
 *   counted (16), no sanitizer report; its spec cases; sig_fuzz 10,000,000
 *   inputs; 311 of 311 mutants as expected; docs/sig.md has the numbers.
 *   STATUS: v0.5.0, 2026-11-12, Linux desktop (GCC 12.2):
 *   test_arm_catch_up as C and as C++ under ASan and UBSan, the compile
 *   checks, the fuzz target, the mutants (docs/sig.md, "Tests, fuzzing
 *   and mutations").
 *   STATUS: v0.4.1, 2026-11-11, Linux desktop (GCC 12.2):
 *   test_review_fixes as C and as C++ under ASan and UBSan; 203 of 203
 *   mutants as expected (206 to 209 new).
 *   STATUS: v0.4.0, 2026-11-11, Linux desktop (GCC 12.2):
 *   test_set_recompute as C and as C++ under ASan and UBSan; mutants 202
 *   to 205 killed.
 *   STATUS: v0.3.0, 2026-11-11, Linux desktop (GCC 12.2): test_entry_now
 *   as C and as C++ under ASan and UBSan, the compile checks, 195 of 195
 *   mutants as expected (193 to 201 new, killed).
 *   STATUS: v0.2.0, 2026-11-09, Linux desktop (Debian 12, GCC 12.2):
 *   tests/adapt/sig_test.c (the new ops beyond closed forms: late reports,
 *   a planned onset ahead of the poll, past reads, resets, extreme times,
 *   replay), the compile checks (C99, C11, C++17, C++20), the fuzz target
 *   under ASan and UBSan (20 million inputs, the new ops and calls, valid
 *   adapter graphs), 183 of 184 mutants killed (one is a cost only: docs/sig.md),
 *   and trial_visuomotor and
 *   trial_bimanual
 *   byte-identical to v0.1.0's CI data. Four raw mice at 8 kHz into 32
 *   nodes: 144 ns a sample and 0.29 us a ysig_frame() at the median on a
 *   performance core of the i5-14500, no allocator call (docs/sig.md,
 *   "Cost"). Not run: MSVC, macOS, a window, a real device.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_SIG_H_INCLUDED
#define YSP_SIG_H_INCLUDED

#define YSIG_VERSION_MAJOR 0
#define YSIG_VERSION_MINOR 6
#define YSIG_VERSION_PATCH 1
#define YSIG_VERSION_STRING "0.6.1"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ysp/input.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YSIG_API
#define YSIG_API extern
#endif

#define YSIG_LANES     4
#define YSIG_NP        16
#define YSIG_MAX_IN    4
#define YSIG_MAX_NODES 512
#define YSIG_QUEUE_CAP 64
#define YSIG_MAX_HISTORY (1 << 20)
#define YSIG_OP_ABI    1
#define YSIG_NONE      (-1)
#define YSIG_FUTURE_NS 1000000000LL /* a stamp later than the poll by more
                                     * than this is a bad one (SCHEDULES AND
                                     * TIME); a poll a little behind the
                                     * newest sample is normal               */

typedef enum ysig_op {
    YSIG_OP_NONE = 0,
    YSIG_OP_SOURCE, YSIG_OP_ROLE, YSIG_OP_INTEGRATE, YSIG_OP_LOWPASS, YSIG_OP_VELOCITY, YSIG_OP_SPEED,
    YSIG_OP_DELAY, YSIG_OP_DEADZONE, YSIG_OP_AFFINE, YSIG_OP_SCALE, YSIG_OP_OFFSET, YSIG_OP_CLAMP,
    YSIG_OP_SELECT, YSIG_OP_JOIN, YSIG_OP_SUM, YSIG_OP_MEAN, YSIG_OP_DIFF, YSIG_OP_SINES, YSIG_OP_NOISE,
    YSIG_OP_CHANNEL, YSIG_OP_BIND, YSIG_OP_DETECT, YSIG_OP_RECORD, YSIG_OP_EMIT,
    YSIG_OP_MUX, YSIG_OP_LATCH, YSIG_OP_DISTANCE, YSIG_OP_PEAK, YSIG_OP_ALL, YSIG_OP_SILENT,
    YSIG_OP_COUNT,
    YSIG_OP_CUSTOM = 64             /* + i: desc.custom[i]                 */
} ysig_op;

/* detect p0 */
typedef enum ysig_test { YSIG_TEST_GE = 0, YSIG_TEST_LE = 1, YSIG_TEST_IN_RECT = 2, YSIG_TEST_OUT_RECT = 3 } ysig_test;

/* detect p13: its output */
typedef enum ysig_detect_out { YSIG_OUT_TEST = 0, YSIG_OUT_HIT = 1 } ysig_detect_out;

/* detect p12: a detect armed while its test is true (DETECTORS) */
typedef enum ysig_detect_entry {
    YSIG_ENTRY_ARM = 0,         /* a hold counts from the arm             */
    YSIG_ENTRY_FRESH = 1,       /* waits for the test to fail first       */
    YSIG_ENTRY_NOW = 2          /* no hold: a hit at the arm              */
} ysig_detect_entry;

/* p[] indices by name (THE GRAPH): YSIG_<OP>_<ROW> is the row of
 * ysig_op_params(op) named <row>, in upper case with '.' as '_', so a
 * graph reads n.p[YSIG_DETECT_LEVEL] and ysig_set(g, k, YSIG_AFFINE_ANGLE,
 * deg) without the table beside it. speed has velocity's rows and mean
 * sum's, under their own names too. The test checks every name against
 * its row. */
enum { YSIG_SOURCE_KIND = 0, YSIG_SOURCE_CONTROL, YSIG_SOURCE_DEVICE, YSIG_SOURCE_FIELDS, YSIG_SOURCE_TIER,
       YSIG_SOURCE_TYPE };
enum { YSIG_ROLE_ROLE = 0 };
enum { YSIG_INTEGRATE_GAIN_X = 0, YSIG_INTEGRATE_GAIN_Y, YSIG_INTEGRATE_GAIN_Z, YSIG_INTEGRATE_GAIN_W,
       YSIG_INTEGRATE_ORIGIN_X, YSIG_INTEGRATE_ORIGIN_Y, YSIG_INTEGRATE_ORIGIN_Z, YSIG_INTEGRATE_ORIGIN_W,
       YSIG_INTEGRATE_LOW_X, YSIG_INTEGRATE_LOW_Y, YSIG_INTEGRATE_LOW_Z, YSIG_INTEGRATE_LOW_W,
       YSIG_INTEGRATE_HIGH_X, YSIG_INTEGRATE_HIGH_Y, YSIG_INTEGRATE_HIGH_Z, YSIG_INTEGRATE_HIGH_W };
enum { YSIG_LOWPASS_CUTOFF = 0, YSIG_LOWPASS_ORDER, YSIG_LOWPASS_RATE };
enum { YSIG_VELOCITY_LAG = 0, YSIG_VELOCITY_CAPACITY };
enum { YSIG_SPEED_LAG = 0, YSIG_SPEED_CAPACITY };
enum { YSIG_DELAY_DELAY = 0, YSIG_DELAY_CAPACITY };
enum { YSIG_DEADZONE_RADIUS = 0 };
enum { YSIG_AFFINE_M_XX = 0, YSIG_AFFINE_M_XY, YSIG_AFFINE_M_YX, YSIG_AFFINE_M_YY, YSIG_AFFINE_PRE_X,
       YSIG_AFFINE_PRE_Y, YSIG_AFFINE_POST_X, YSIG_AFFINE_POST_Y, YSIG_AFFINE_ANGLE };
enum { YSIG_SCALE_K_X = 0, YSIG_SCALE_K_Y, YSIG_SCALE_K_Z, YSIG_SCALE_K_W };
enum { YSIG_OFFSET_D_X = 0, YSIG_OFFSET_D_Y, YSIG_OFFSET_D_Z, YSIG_OFFSET_D_W };
enum { YSIG_CLAMP_LOW_X = 0, YSIG_CLAMP_LOW_Y, YSIG_CLAMP_LOW_Z, YSIG_CLAMP_LOW_W, YSIG_CLAMP_HIGH_X,
       YSIG_CLAMP_HIGH_Y, YSIG_CLAMP_HIGH_Z, YSIG_CLAMP_HIGH_W };
enum { YSIG_SELECT_LANE_X = 0, YSIG_SELECT_LANE_Y, YSIG_SELECT_LANE_Z, YSIG_SELECT_LANE_W };
enum { YSIG_SUM_W_0 = 0, YSIG_SUM_W_1, YSIG_SUM_W_2, YSIG_SUM_W_3 };
enum { YSIG_MEAN_W_0 = 0, YSIG_MEAN_W_1, YSIG_MEAN_W_2, YSIG_MEAN_W_3 };
enum { YSIG_SINES_N = 0, YSIG_SINES_RAMP };
enum { YSIG_NOISE_SEED = 0, YSIG_NOISE_SD, YSIG_NOISE_CUTOFF };
enum { YSIG_CHANNEL_CHANNEL = 0 };
enum { YSIG_BIND_CHANNEL = 0 };
enum { YSIG_DETECT_TEST = 0, YSIG_DETECT_LANE, YSIG_DETECT_LEVEL, YSIG_DETECT_HYSTERESIS, YSIG_DETECT_HOLD,
       YSIG_DETECT_AT, YSIG_DETECT_START, YSIG_DETECT_REPEAT, YSIG_DETECT_A_X, YSIG_DETECT_A_Y, YSIG_DETECT_B_X,
       YSIG_DETECT_B_Y, YSIG_DETECT_ENTRY, YSIG_DETECT_OUT };
enum { YSIG_RECORD_PER = 0 };
enum { YSIG_EMIT_KIND = 0, YSIG_EMIT_CONTROL };
enum { YSIG_DISTANCE_POINT_X = 0, YSIG_DISTANCE_POINT_Y, YSIG_DISTANCE_POINT_Z };
enum { YSIG_PEAK_LANE = 0 };
enum { YSIG_SILENT_AFTER = 0 };

/* Role types and unit spaces (ROLES AND ADAPTERS). */
typedef enum ysig_type {
    YSIG_TYPE_NONE = 0, YSIG_TYPE_POSITION2D, YSIG_TYPE_POSITION2D_RELATIVE, YSIG_TYPE_POSITION3D,
    YSIG_TYPE_SCALAR, YSIG_TYPE_GAZE, YSIG_TYPE_COUNT
} ysig_type;
typedef enum ysig_units {
    YSIG_UNITS_DEVICE = 0, YSIG_UNITS_WORKSPACE_CM, YSIG_UNITS_DISPLAY_CM, YSIG_UNITS_DISPLAY_DEG,
    YSIG_UNITS_DISPLAY_PX, YSIG_UNITS_COUNT
} ysig_units;

/* One node of the graph (THE GRAPH). */
typedef struct ysig_node {
    uint16_t      op;           /* ysig_op                               */
    int16_t       in[4];        /* node indices; -1 = none               */
    uint8_t       lanes;        /* 1..4; 0 = the op's rule               */
    uint8_t       reserved_;
    int32_t       history;      /* past values kept for ysig_value_at()  */
    double        p[YSIG_NP];   /* by the op's table (ysig_op_params())  */
    const double* data;         /* sines: lanes x n x (a, f, phi); copied */
    int32_t       n_data;
    const char*   name;         /* records and errors; not copied        */
} ysig_node;

/* A node's output. 64 bytes. */
typedef struct ysig_value {
    double   v[YSIG_LANES];
    int64_t  t;                 /* the arriving sample's, else the newest */
    int64_t  t_oldest;          /* the oldest input sample's, ns         */
    uint32_t device;            /* that sample's device                  */
    uint16_t src;               /* that sample's source node             */
    uint16_t src_oldest;        /* ... of the oldest                     */
    uint8_t  tier;              /* the worst (yin_tier)                  */
    uint8_t  lanes;
    uint16_t unc_us;            /* the arriving event's unc_us           */
    uint8_t  reserved_[4];
} ysig_value;

/* A parameter row, the shape of the other headers' tables. */
typedef struct ysig_param {
    const char* name;
    const char* type;           /* "f64", "i32", "u32", "enum"           */
    double      min, max;       /* inclusive                             */
    double      def;            /* what a zero field means               */
    const char* unit;
    const char* doc;
    const char* values;         /* enum: "name=value" pairs; else NULL   */
} ysig_param;

/* A custom op (CUSTOM OPS). */
typedef struct ysig_op_vtable {
    uint32_t          version;    /* YSIG_OP_ABI                         */
    const char*       name;
    uint8_t           lanes_in[4], lanes_out; /* 0 = any, input 0's      */
    size_t            state_size;
    const ysig_param* params;
    int               n_params;
    void (*reset)(void* state, const double* p, int64_t t_ns);
    void (*sample)(void* state, const double* p, const ysig_value* in, int n_in, ysig_value* out);
    void (*frame)(void* state, const double* p, int64_t onset_ns, ysig_value* out);
} ysig_op_vtable;

typedef struct ysig_role {
    const char* name;
    uint8_t     type;           /* ysig_type                             */
    uint8_t     units;          /* ysig_units                            */
    bool        allow_relative; /* position2d takes position2d_relative  */
} ysig_role;

typedef struct ysig_adapter {
    const char*      role;      /* the role's name                       */
    const char*      name;      /* the adapter's, for the data           */
    uint8_t          type, units;
    const ysig_node* nodes;     /* the last node is the role's value     */
    int              n_nodes;
} ysig_adapter;

/* A stream sample (docs/signal_spec.md 5.5): more values than yin_event
 * holds. Defined for sources to come; nothing here reads it yet. */
typedef struct ysig_stream_sample {
    int64_t       t;
    uint32_t      stream;
    uint8_t       tier;
    uint8_t       reserved_[3];
    uint64_t      index;
    const double* values;
    int32_t       n_values;
} ysig_stream_sample;

/* One frame for ysig_frame(). */
typedef struct ysig_frame_in {
    int64_t      index;
    int64_t      onset_ns;      /* the plan's, or the flip record's      */
    int64_t      base_ns;       /* the trial base's time (ytl_base_time) */
    const float* values;        /* timeline channels for channel nodes   */
    int          n_values;
    int64_t      poll_ns;       /* input is known up to here (the poll's
                                 * time), with has_poll                  */
    bool         has_poll;      /* poll_ns is set; false (a zeroed struct)
                                 * is refused in a graph with a view     */
} ysig_frame_in;

typedef struct ysig_row {
    const char* name;           /* the record node's name                */
    int32_t     node;
    int32_t     per_frame;      /* 0: a sample row; 1: a frame row       */
    int64_t     frame;          /* frame rows: the index                 */
    int64_t     onset;          /* frame rows: the onset                 */
    ysig_value  value;
} ysig_row;

typedef struct ysig_hit {
    int32_t    node;            /* the detect                            */
    int32_t    reserved_;
    int64_t    t;               /* the test met: a value's t, or a hold's
                                 * start + hold                           */
    int64_t    t_found;         /* the value or onset that found it      */
    ysig_value value;           /* the input that met it                 */
} ysig_hit;

typedef enum ysig_entry_kind {
    YSIG_J_RESET = 1, YSIG_J_SET, YSIG_J_SET_DATA, YSIG_J_ARM, YSIG_J_DISARM, YSIG_J_FEED, YSIG_J_FRAME,
    YSIG_J_RESET_TRIAL, YSIG_J_RECENTER, YSIG_J_SETTLE, YSIG_J_FLUSH,
    YSIG_J_DISARM_AT            /* v0.7.0: node, t                       */
} ysig_entry_kind;

/* One journal entry (RECORDS AND REPLAY). */
typedef struct ysig_entry {
    int32_t       kind;         /* ysig_entry_kind                       */
    int32_t       node, param;  /* SET, SET_DATA (param: the index), ARM;
                                 * RECENTER (node: the role)              */
    int32_t       reserved_;
    double        value;        /* SET, SET_DATA; RECENTER's x            */
    double        y;            /* RECENTER                               */
    int64_t       t;            /* RESET, RESET_TRIAL, RECENTER, ARM      */
    yin_event     event;        /* FEED                                  */
    ysig_frame_in frame;        /* FRAME                                 */
} ysig_entry;

typedef struct ysig_stats {
    int64_t  fed;               /* ysig_feed() calls                     */
    int64_t  taken;             /* events a source took                  */
    int64_t  dropped_order;     /* older than their source's last        */
    int64_t  hits_lost, derived_lost;
    int64_t  rows, hits;
    int64_t  ring_short;        /* a delay or velocity ring too short    */
    int64_t  history_skipped;   /* a value older than its ring's newest  */
    int64_t  order_evicted;     /* a source's 9th device took the slot of
                                 * its least recently seen one            */
    int64_t  late_poll;         /* frames whose poll_ns was before the
                                 * newest event fed (that time was used)  */
    int64_t  late_poll_max_ns;  /* the largest such excess                */
    int64_t  time_clamped;      /* a detect held its hold clock where a
                                 * value's t went back                    */
    int64_t  arm_caught_up;     /* v0.6.0: arms before input the graph
                                 * already had, run again from a
                                 * checkpoint (AN ARM IN THE PAST)        */
    int64_t  arm_past_off;      /* such arms with no window (desc.
                                 * catchup_frames 0): the newest value    */
    int64_t  arm_past_far;      /* such arms before the window's oldest
                                 * checkpoint: the newest value           */
    int64_t  arm_past_full;     /* such arms after the window's logs ran
                                 * full: the newest value                 */
    int64_t  arm_hits_retracted;/* hits handed out before such an arm
                                 * that the run again does not find and
                                 * the caller may have taken: any
                                 * detect's, but the armed detect's own
                                 * later than the arm's time              */
    int64_t  arm_rows_stale;    /* such arms whose run again changed rows
                                 * on_row already had                     */
    int64_t  arm_derived_stale; /* such arms whose run again changed
                                 * derived events                         */
    int64_t  arm_hits_superseded;/* the armed detect's own hits handed
                                 * out before such an arm, later than the
                                 * arm's time, that the new history does
                                 * not give as they were (its earlier
                                 * arm's: a caller re-arming at t took
                                 * none of them)                          */
    int64_t  arm_reentry;       /* ysig_arm() refused: called from a
                                 * callback during another call           */
    int64_t  call_reentry;      /* any other call that changes the graph
                                 * (feed, frame, set, set_data, recenter,
                                 * reset, reset_trial, settle, disarm,
                                 * replay) refused: the same              */
    int64_t  arm_past_pinned;   /* a past group placed after an input
                                 * later than its time that it shares a
                                 * node with (a late report): a fallback  */
    int64_t  arm_ahead_cancelled;/* waiting arms a disarm or a barrier
                                 * cancelled                              */
    int64_t  arm_unverified;    /* arms whose group is outside the shapes
                                 * whose result is verified (docs/sig.md,
                                 * "Exactness"): R is made, not checked   */
    /* v0.7.0, LATE INPUT: calls whose place in R (the order by key) is
     * before a call the graph already had that shares a node with them */
    int64_t  late_far;          /* placed before the window's oldest
                                 * checkpoint: made at the end of R       */
    int64_t  late_full;         /* the window's logs ran full: the same   */
    int64_t  late_barrier;      /* inputs before the last barrier's time:
                                 * dropped (R drops them too)             */
    int64_t  late_off;          /* late calls with no window: made at the
                                 * end of R                               */
    int64_t  late_frames;       /* frames made before a late call whose
                                 * bound channels R now gives otherwise
                                 * (drawn already: not given again)       */
    int64_t  late_unverified;   /* late calls outside the shapes whose
                                 * result is verified (docs/sig.md,
                                 * "Exactness for late calls")            */
    int64_t  late_hits_voided;  /* hits handed out that a late call's run
                                 * again does not give (the coordinator's
                                 * ruling, 2026-11-15): the caller may
                                 * have acted on them; the guard stops    */
    uint64_t digest;            /* FNV-1a: rows, hits, events, binds,
                                 * frames                                */
} ysig_stats;

struct ysig__nd;
typedef struct ysig_desc {
    const ysig_node*              nodes;
    int                           n_nodes;
    const ysig_role*              roles;
    int                           n_roles;
    const ysig_adapter*           adapters;
    int                           n_adapters;
    const ysig_op_vtable* const*  custom;
    int                           n_custom;
    void*                         arena;      /* 8-byte aligned          */
    size_t                        arena_size;
    float*                        channels;   /* bind writes here        */
    int                           n_channels;
    void (*on_row)(void* user, const ysig_row* row);
    void (*journal)(void* user, const ysig_entry* e);
    void*                         user;
    /* v0.6.0, AN ARM IN THE PAST: how far back an arm may be run again,
     * and the logs that window holds (arena: ysig_arena_size()). 0 frames
     * (a zeroed desc): no window, an arm tests the newest value. */
    int                           catchup_frames;   /* checkpoints, 1 to 8   */
    int                           catchup_calls;    /* 0: 1024              */
    int                           catchup_writes;   /* ring writes; 0: 8192 */
    /* with a window: hand out a row (a derived event) only once no arm
     * can change it, catchup_frames frames later at most; held in
     * catchup_rows each (0: 4 x catchup_calls) */
    bool                          rows_final;
    bool                          derived_final;
    int                           catchup_rows;
    /* v0.7.0, LATE INPUT: the window's span in input time; checkpoints
     * are spaced catchup_ns / 8 apart (0: 50 ms) */
    int64_t                       catchup_ns;
} ysig_desc;

/* The handle: caller-allocated, all state in the arena. */
typedef struct ysig_graph {
    int               ok;
    int               n;          /* nodes, adapters' included            */
    int               n_exp;      /* the experiment's                     */
    int               n_src, n_frame;
    struct ysig__nd*  nd;
    ysig_value*       val;
    int16_t*          src_ids;    /* source nodes                         */
    int32_t*          src_off;    /* n_src + 1 offsets into sched         */
    int16_t*          sched;
    int16_t*          frame;
    int16_t*          order;      /* every node, inputs first             */
    ysig_hit*         hitq;
    yin_event*        derq;
    int               n_hit, n_der;
    int32_t           adapter_base[16], adapter_n[16];
    uint8_t           adapter_type[16];   /* ysig_recenter(): relative or not */
    int               n_adapters;
    int64_t           frame_index, onset, base;  /* the frame being run  */
    int64_t           t_known;    /* min(onset, poll): views stop here    */
    int64_t           t_cover;    /* the newest input since the reset: a
                                   * sample's or a frame's onset            */
    int               has_view;   /* a view op: poll_ns 0 is refused      */
    int64_t           t_fed;      /* the newest event time fed            */
    const float*      fvals;
    int               n_fvals;
    int64_t           pass;       /* one per evaluation (a source's sample,
                                   * a frame, a reset): a node run in it
                                   * changed, and gives its t downstream */
    ysig_stats        st;
    float*            channels;
    int               n_channels;
    void (*on_row)(void* user, const ysig_row* row);
    void (*journal)(void* user, const ysig_entry* e);
    void*             user;
    struct ysig__cu*  cu;         /* the arm's window, NULL with none     */
    int               ch_hi;      /* v0.6.0: 1 + the highest channel a
                                   * channel node read at the open         */
    int               busy;       /* v0.6.0: in a callback (its ysig_arm()
                                   * is refused)                            */
    uint64_t*         down;       /* v0.6.0: each node's downstream set, dw
                                   * words a node (the footprints)          */
    int               dw;
    struct ysig__wt*  wt;         /* arms that wait for their input        */
    int64_t           bar_t;      /* v0.7.0: the last barrier's time       */
    uint64_t          cs;         /* v0.7.0: the call number of the disarm
                                   * made now (the cancel by key)          */
    int64_t           tk;         /* v0.7.0: the key time of the state call
                                   * being made                            */
    uint64_t          ko;         /* v0.7.0: the key's call order of the
                                   * call being made                       */
    char              err[256];
} ysig_graph;

/* A node with op and inputs set, every other input -1, the rest zero. */
static inline ysig_node ysig_node_of(int op, int in0, int in1) {
    ysig_node n;
    memset(&n, 0, sizeof n);
    n.op = (uint16_t)op;
    n.in[0] = (int16_t)in0;
    n.in[1] = (int16_t)in1;
    n.in[2] = n.in[3] = -1;
    return n;
}

/* One node in one C expression. The trailing arguments are designated
 * initializers of ysig_node, so a graph reads node by node:
 *     n[HOLD] = YSIG_NODE(YSIG_OP_DETECT, HAND, -1, .name = "hold", .p = {
 *         [YSIG_DETECT_LEVEL] = 12, [YSIG_DETECT_HOLD] = 0.5 });
 * Fields not named are zero, as after ysig_node_of(). At least one must be
 * named; a node with none is ysig_node_of(). C only: C++ has no array
 * designators, so C++ uses ysig_node_of() and assigns. */
#define YSIG_NODE(op_, in0_, in1_, ...) \
    ((ysig_node){ .op = (uint16_t)(op_), .in = { (int16_t)(in0_), (int16_t)(in1_), -1, -1 }, __VA_ARGS__ })

YSIG_API const char* ysig_version(void);

/* Check a desc as ysig_open() would, but the arena. false with the reason
 * in err (may be NULL). */
YSIG_API bool ysig_check(const ysig_desc* d, char* err, size_t err_size);

/* Bytes of arena a desc needs; 0 when ysig_check() refuses it. */
YSIG_API size_t ysig_arena_size(const ysig_desc* d);

YSIG_API bool ysig_open(ysig_graph* g, const ysig_desc* d);
YSIG_API const char* ysig_error(const ysig_graph* g);

/* Trial start: integrators to their origins, filters, rings, delays and
 * queues empty, detects armed by YSIG_DETECT_START. Every value gets t. */
YSIG_API void ysig_reset(ysig_graph* g, int64_t t_ns);

/* A reset of the experiment's nodes only (D19): an adapter's nodes keep
 * their state, so a relative role does not jump between trials. A graph
 * with no adapters resets every node, as ysig_reset(). */
YSIG_API void ysig_reset_trial(ysig_graph* g, int64_t t_ns);

/* Place relative role `role` (its index in desc.roles) at x, y, in the role's
 * units, at time t_ns, no later than the input already fed (normally the
 * poll's time); its integrator continues from there on the count grid (ROLES
 * AND ADAPTERS). false with the reason in ysig_error(). */
YSIG_API bool ysig_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns);

/* p[param] of node := v, checked by the op's table; between trials. param
 * is the row's index, by name YSIG_<OP>_<ROW>. A parameter that sizes the
 * arena (a ring's capacity, sines' count) is refused. State is kept. A
 * stateless node (affine, scale, select, join, mux, distance, ...) and its
 * stateless users recompute their held values' lanes from their inputs'
 * at once (t and sources stay), so a frame test or a bind reads the new
 * parameter before the next sample (G17). A node with a ring gets an
 * entry at the set's time, the later of the newest event fed and the
 * last frame's onset: ysig_value_at() at or after it gives the new value,
 * before it the old. A node computed from its inputs at a past time
 * (SCHEDULES AND TIME) uses the parameters it has now. */
YSIG_API bool ysig_set(ysig_graph* g, int node, int param, double v);
YSIG_API bool ysig_set_data(ysig_graph* g, int node, int index, double v);

/* Every input event. Returns how many sources took it (0 or more), -1 for
 * a bad argument. */
YSIG_API int ysig_feed(ysig_graph* g, const yin_event* e);
/* v0.7.0: ysig_feed() of each event in array order, the window run again
 * once at the end for the late ones (LATE INPUT, "The batch"). The events
 * the sources took, or -1. */
YSIG_API int ysig_feed_n(ysig_graph* g, const yin_event* e, int n);

/* Once per frame, after the frame's input was fed. 0, or -1. */
YSIG_API int ysig_frame(ysig_graph* g, const ysig_frame_in* f);

/* Detects: arm from t (DETECTORS; a t before input the graph already had
 * runs the window again, AN ARM IN THE PAST), or stop. */
YSIG_API bool ysig_arm(ysig_graph* g, int node, int64_t t_ns);
/* v0.6.0: the caller has fed all input up to t: a sample-rate detect's
 * hold that the held value meets by t is a hit now, at start + hold, as
 * a frame's pass would find it (no frame rows, no views). Journaled. */
YSIG_API int  ysig_settle(ysig_graph* g, int64_t t_ns);
YSIG_API bool ysig_disarm(ysig_graph* g, int node);
/* v0.7.0: a disarm at the caller's time t: an arm of node at or before t
 * is made, then ended at t; one later than t, made before this call, is
 * cancelled (LATE INPUT, "The cancel, by key"). Journaled. */
YSIG_API bool ysig_disarm_at(ysig_graph* g, int node, int64_t t_ns);
/* v0.6.0: a barrier that resets nothing: no arm goes before it, waiting
 * arms are cancelled, and every row and derived event so far is final
 * (handed out now in desc.rows_final, desc.derived_final). Journaled. */
YSIG_API bool ysig_flush(ysig_graph* g);

/* The queues since the last call: hits, derived events. */
YSIG_API int ysig_detected(ysig_graph* g, ysig_hit* out, int cap);
YSIG_API int ysig_derived(ysig_graph* g, yin_event* out, int cap);
/* v0.7.0: the same, only the queued ones at or before t_ns, in the
 * queue's order; the rest stay queued and are not handed out. A queued
 * hit or event is the caller's only once handed out: a run again that no
 * longer gives it removes it with no count, and arm_hits_superseded,
 * arm_hits_retracted, late_hits_voided and arm_derived_stale count only
 * those handed out. Each queue holds YSIG_QUEUE_CAP plus catchup_calls
 * (with a window); hits_lost, derived_lost count the rest ("Holding hits
 * and derived events"). ysig_detected(), ysig_derived(): t_ns INT64_MAX */
YSIG_API int ysig_detected_to(ysig_graph* g, ysig_hit* out, int cap, int64_t t_ns);
YSIG_API int ysig_derived_to(ysig_graph* g, yin_event* out, int cap, int64_t t_ns);

/* A node's held value; NULL for a bad index. */
YSIG_API const ysig_value* ysig_value_of(const ysig_graph* g, int node);

/* The value at or before t (SCHEDULES AND TIME): 1, or 0 when t is older
 * than the ring (the oldest given) or the ring is empty (the held value);
 * -1 when the node keeps no history. */
YSIG_API int ysig_value_at(const ysig_graph* g, int node, int64_t t_ns, ysig_value* out);

/* Adapter a's node i, or -1. */
YSIG_API int ysig_adapter_node(const ysig_graph* g, int adapter, int i);

YSIG_API void ysig_get_stats(const ysig_graph* g, ysig_stats* out);

/* Run a journal (RECORDS AND REPLAY). The number of entries run, -1 at
 * the first bad one. */
YSIG_API int ysig_replay(ysig_graph* g, const ysig_entry* e, int n);

/* The parameter table of op `op`: one row per p[] it reads, in order.
 * YSIG_OP_NONE gives the one row "op", whose values name every op. */
YSIG_API const ysig_param* ysig_op_params(int op, int* n);
YSIG_API const char* ysig_op_name(int op);

/* The worse of two stamp tiers (SIM best, UNKNOWN worst). */
static inline uint8_t ysig_tier_worst(uint8_t a, uint8_t b) {
    int ra = a == YIN_TIER_SIM ? 0 : (a >= 1 && a <= 3) ? a : 4;
    int rb = b == YIN_TIER_SIM ? 0 : (b >= 1 && b <= 3) ? b : 4;
    return ra >= rb ? (ra == 4 ? (uint8_t)YIN_TIER_UNKNOWN : a) : (rb == 4 ? (uint8_t)YIN_TIER_UNKNOWN : b);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_SIG_H_INCLUDED */

/* ======================================================================== */
#if defined(YSP_SIG_IMPLEMENTATION) && !defined(YSP_SIG_IMPLEMENTED)
#define YSP_SIG_IMPLEMENTED

#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YSIG__PI 3.14159265358979323846

/* node flags */
#define YSIG__F_FRAME  0x01u  /* changes at a frame                       */
#define YSIG__F_MULTI  0x02u  /* two or more sources reach it             */
#define YSIG__F_AT     0x04u  /* value_at computes it from its inputs     */
#define YSIG__F_COUNTS 0x08u  /* integrate: an int64 count sum            */
#define YSIG__F_SINK   0x10u
#define YSIG__F_GEN    0x20u
#define YSIG__F_VIEW   0x40u  /* has a value at the onset (state kept)    */
#define YSIG__F_NOSRC  0x80u  /* no source upstream: changes at frames only */

/* op classes */
#define YSIG__K_PURE  0x01u
#define YSIG__K_GEN   0x02u
#define YSIG__K_SINK  0x04u
#define YSIG__K_VIEW  0x08u

/* lane rules */
enum { YSIG__R_SOURCE, YSIG__R_ROLE, YSIG__R_SAME, YSIG__R_ONE, YSIG__R_JOIN, YSIG__R_SELECT, YSIG__R_GEN,
       YSIG__R_SINK, YSIG__R_TWO, YSIG__R_MUX, YSIG__R_FIRST };

typedef struct ysig__ring { int32_t cap, head, n, reserved_; ysig_value* buf; } ysig__ring;

typedef struct ysig__nd {
    double                p[YSIG_NP];
    double                k[8];        /* derived: affine's matrix, ...     */
    int64_t               kns[2];      /* derived durations                 */
    const ysig_op_vtable* vt;
    const char*           name;
    double*               data;
    void*                 st;
    ysig__ring            hist;
    int64_t               pass;        /* the evaluation it last ran in     */
    int64_t               sink_t;      /* a detect's hold clock: never back */
    /* v0.7.0: of the calls in R since the last barrier that reach this
     * node, the greatest key (kt, kc: time and class, 0 a feed, 1 a frame
     * or a settle, 2 a state call or an arm) and the newest input time
     * (nt): N(F) is nt's largest over F, a late call one below kt */
    int64_t               kt, nt;
    uint64_t              ko;          /* the greatest key's call order     */
    uint64_t              dseq;        /* a detect: the newest call number of
                                        * a disarm of it in R since the last
                                        * barrier; an arm of it made before
                                        * that call, after it in R, is
                                        * cancelled (the cancel by key)     */
    int32_t               kc;
    int32_t               n_data;
    uint16_t              op;
    int16_t               in[4];
    uint8_t               n_in, lanes, flags, reserved_;
} ysig__nd;

/* A source's last time per device: order holds within one device, not
 * across two mice on one any-device source. Eight devices; a ninth takes
 * the slot of the least recently seen one (counted: order_evicted), so a
 * busy device keeps its slot. */
#define YSIG__SRC_DEVS 8
typedef struct ysig__src_st {
    int64_t  last_t[YSIG__SRC_DEVS];
    uint64_t used[YSIG__SRC_DEVS];
    uint32_t dev[YSIG__SRC_DEVS];
    int      n, reserved_;
    uint64_t clock;
} ysig__src_st;
typedef struct ysig__int_st {
    int64_t n[4];
    double  acc[4], x[4];
    int64_t t_prev;
    int     has, recentered;
    double  org[4];   /* the count grid's origin after ysig_recenter()    */
} ysig__int_st;
typedef struct ysig__lp_st { double y1[4], y2[4]; int64_t t_prev; int init, reserved_; } ysig__lp_st;
typedef struct ysig__det_st {
    int64_t    t_arm, t_start, t_hit;
    int        armed, ready, holding, state, fired, reserved_;
    ysig_value v_start, v_hit;
} ysig__det_st;
typedef struct ysig__latch_st { int latched, prev; } ysig__latch_st;

/* v0.6.0, an arm in the past: checkpoints of the graph's state after
 * frames and resets, the calls since the oldest, the ring slots those
 * calls wrote (their old values), and the hits they queued. Indices are
 * absolute counts; a slot is the index modulo the capacity. */
#define YSIG__FPW_ 8   /* a footprint's words: 512 nodes */
typedef struct ysig__ck {
    uint64_t call, undo, hit;   /* the logs' heads when it was taken     */
    uint64_t der;               /* v0.7.0: the derived log's head        */
    uint64_t hseq, dseq;        /* v0.7.0: the last sequence given then: a
                                 * queued entry at or below it was made
                                 * before, so a run again keeps it       */
    int64_t  cover;             /* t_cover then                          */
    uint64_t row_h;             /* the rows' hash                        */
    char*    img;               /* the state image                       */
    int      bar;               /* taken at a barrier: nothing before it
                                 * matters to an arm after it            */
    int      reserved_;
} ysig__ck;
/* A logged call: its time in the reference order (an input's own; a
 * state call's group time, else the input time at its call), and the
 * sources of the first 64 that took a sample and kept it (srcm_all: more,
 * or a sample no source dropped: every taker). */
typedef struct ysig__call {
    ysig_entry e;
    int64_t    tr;
    uint64_t   srcm;
    int        srcm_all;
    int        wp;          /* a waiting group's call, placed in an open gap
                             * (rule 3): not of that gap's group            */
    int        wnode;       /* wp: the waiting arm's detect, -1 - it when
                             * cancelled                                    */
    int        reserved_;
    uint64_t   wseq;        /* wp: the waiting arm's call number            */
    uint64_t   cseq;        /* this call's number (a disarm's cancel order) */
    uint64_t   gid;         /* its group's arm's call number (0: an input,
                             * a barrier, a state call in no group)        */
    int64_t    n_at;        /* the input time at the call (rule 3's test)   */
    /* v0.7.0: the key's call order (a group's: its arm's), and a frame's
     * bound channels as made (late_frames counts a frame once: lf) */
    uint64_t   ko, chh;
    int        lf, kcls;    /* kcls: the key's class                       */
    int        cx, reserved2_;   /* an arm a disarm placed before it cancels
                                  * (counted once)                         */
} ysig__call;
/* A row or a derived event held until it is final, with the call (log
 * index) that made it. */
typedef struct ysig__rbuf { ysig_row r; uint64_t idx; } ysig__rbuf;
typedef struct ysig__dbuf { yin_event e; uint64_t idx; } ysig__dbuf;
typedef struct ysig__undo { ysig_value* at; ysig_value old; } ysig__undo;
/* given: handed out (a run again matches it, else counts it voided);
 * the queue's not-given hits are the run again's to make again */
typedef struct ysig__hl { uint64_t seq; ysig_hit h; int given, reserved_; } ysig__hl;
typedef struct ysig__dl { uint64_t seq; yin_event e; int given, reserved_; } ysig__dl;
typedef struct ysig__cu {
    int         K, C, W, nfv, nslot, n_ck, ck0, replaying, full;
    uint64_t    slot_next;      /* frames with values logged (a slot is it
                                 * modulo nslot)                          */
    size_t      img_size;
    ysig__ck*   ck;
    ysig__call* calls;
    uint64_t    c_tail, c_head;
    ysig__undo* undo;
    uint64_t    u_tail, u_head;
    ysig__hl*   hl;             /* C entries, so is tmp                  */
    uint64_t    h_tail, h_head;
    ysig__hl*   tmp;
    float*      pool;           /* nslot frames' values, nfv each        */
    uint64_t*   qseq;           /* each queued hit's sequence (64 + C) */
    uint64_t*   qhl;            /* its log index (given marks it there) */
    struct ysig__call* blk;     /* a past group's block (YSIG__BLK)       */
    uint64_t    next_seq;       /* the last given                        */
    uint64_t    row_h;
    /* v0.7.0: derived events as hits (at-once mode, a graph with an emit):
     * the log since the oldest checkpoint, DC entries, and each queued
     * event's sequence and log index (64 + C) */
    ysig__dl*   dl;
    ysig__dl*   tmpd;
    uint64_t    d_tail, d_head, next_dseq;
    uint64_t*   dseq;
    uint64_t*   dqdl;
    int         DC, n_tmpd, d_stale;
    uint64_t    c_pos;          /* a run again: the next call's index    */
    int         n_tmp;          /* a run again: hits handed out to match */
    uint64_t    arm_pos;        /* a run again: the new arm's index      */
    int         arm_node, arm_live;   /* its detect; its hits now        */
    int         grp_dis[16], n_grp_dis;   /* the detects its group disarms */
    /* the state calls since the last input, barrier or arm (the group an
     * arm would close): from grp_from; grp_bad: one was not logged */
    int         grp_open, grp_bad;
    int         placing;        /* logging a waiting group's calls: not the
                                 * open gap's                             */
    int         at_bar;         /* the next checkpoint is a barrier's    */
    uint64_t    grp_from;
    uint64_t    cur;            /* the live call's log index (or UINT64_MAX) */
    uint64_t    srcm;           /* the feed being logged: its sources    */
    int         srcm_all;
    int         rows_final, derived_final, R;   /* R: room in each buffer */
    ysig__rbuf* rb;
    ysig__dbuf* db;
    uint64_t    rb_tail, rb_head, db_tail, db_head;
    /* v0.7.0, LATE INPUT */
    int64_t     span_ns;        /* desc.catchup_ns                       */
    ysig__call* mv;             /* C calls: those a late call moves      */
    uint64_t    late_f[YSIG__FPW_];   /* a run again for late calls: the
                                 * nodes they reach (superseded hits)    */
    int64_t     late_t;
    int         late_on, late_in;   /* late_in: an input among the late
                                     * calls of the run again           */
    /* ysig_feed_n(): the run again waits for the batch's end */
    int         defer, def_k;
    uint64_t    def_p;
    int64_t     def_t;
    int64_t     n_lf;           /* a run again: frames late_frames counts */
} ysig__cu;

/* v0.6.0: the arms that wait for input past their time (rule 3) and the
 * calls of their groups; the disarms of the open gap. Live state only: a
 * run again does not change it (no image copies it). */
#define YSIG__WAIT_CAP 32
#define YSIG__WAIT_CALLS 64
#define YSIG__GAP_DIS 16
/* a past group's block: its calls, and the waiting groups they let in */
#define YSIG__BLK (2 * YSIG__WAIT_CALLS + YSIG__WAIT_CAP + 1)
#define YSIG__FPW YSIG__FPW_
/* xf: the footprint of its group's calls that stayed where made (no
 * window, or not logged): still the group's footprint for rule 3 */
typedef struct ysig__wa { int node; int64_t t; uint64_t seq; int c0, nc; uint64_t xf[YSIG__FPW]; } ysig__wa;
/* Exactness (docs/sig.md): an arm in R and its group's footprint, for
 * late reports (condition 3) and for later groups placed between it and
 * its L (condition 2 (b)) */
typedef struct ysig__ux {
    int       node;
    int64_t   t;
    uint64_t  gid, l_seq;  /* l_seq: L's call number (0: none)          */
    uint64_t* F;           /* dw words of the pool, fixed per slot       */
} ysig__ux;
#define YSIG__UX 32
/* A group placed while a trigger lets waiting groups in (a cascade) */
typedef struct ysig__ucg {
    uint64_t  gid;
    int64_t   t, n_at;
    uint64_t *F, *Fa;      /* dw words of the pool each                  */
    int       cancelled, inside, ncalls, placed, first, last, dis_only;
} ysig__ucg;
typedef struct ysig__ucc { uint64_t gid; int64_t t; int kind, node; } ysig__ucc;
#define YSIG__UCG 33       /* the waiting arms, and the one going in      */
#define YSIG__UCC 96
/* the footprints' pool: dw words each (the graph's nodes, not 512) */
#define YSIG__UFP (YSIG__UX + 2 * YSIG__UCG)
typedef struct ysig__wt {
    int        n, nc, n_dis, gap_n, fired;   /* fired: one went in this gap */
    uint64_t   gap_f[YSIG__FPW];            /* v0.7.0: the open gap's footprint */
    int64_t    nfg;                         /* v0.7.0: N(F(G)) of the arm made now */
    uint64_t   seq;                         /* calls made, numbered      */
    ysig__wa   a[YSIG__WAIT_CAP];
    ysig_entry calls[YSIG__WAIT_CALLS];
    uint64_t   call_seq[YSIG__WAIT_CALLS];
    int64_t    call_n[YSIG__WAIT_CALLS];    /* the input time at each call  */
    int        dis_node[YSIG__GAP_DIS];
    uint64_t   dis_seq[YSIG__GAP_DIS];
    /* exactness: arms counted once; arms in R for condition 3 (lr) and
     * open for condition 2 (b) (op); the cascade a trigger lets in */
    uint64_t   unv[64];
    int        n_unv, unv_at;
    ysig__ux   lr[YSIG__UX];
    int        n_lr;
    ysig__ux*  op;          /* with a window: catchup_calls of them      */
    int        n_op, cap_op;
    int        cas_kind, cas_depth, n_cg, n_cc, cas_stack[YSIG__UCG], cas_top;
    int        cas_dis;     /* condition 4: a disarm (the open gap's, or of
                             * a group let in) since the last arm or input
                             * comes before the cascade                  */
    int        cas_in;      /* condition 1: the cascade goes in after the
                             * open gap's own calls (an arm's let-in)     */
    int        gap4_wp;     /* a let-in group's disarm after the last arm
                             * in the open gap                           */
    int        n_pin;       /* groups a state call let in after the open
                             * gap's own calls: inside if an arm closes it */
    uint64_t   pin[8];
    int64_t    cas_t;
    uint64_t   cas_f[YSIG__FPW];
    int        cas_all;
    ysig__ucg  cg[YSIG__UCG];
    ysig__ucc  cc[YSIG__UCC];
} ysig__wt;
struct ysig_graph;
static void ysig__before_input(struct ysig_graph* g, int64_t t, const uint64_t* f);
static void ysig__before_state(struct ysig_graph* g, int node, int64_t t);
static void ysig__before_barrier(struct ysig_graph* g);
static int ysig__feed_fp(const struct ysig_graph* g, const yin_event* e, uint64_t* f, uint64_t* mask, int* all);

/* The logs ran full (or a reset): no checkpoint to run again from. */
static void ysig__cu_clear(ysig__cu* cu, uint64_t call) {
    cu->n_ck = 0;
    cu->c_tail = call;
    cu->u_tail = cu->u_head;
    cu->h_tail = cu->h_head;
    cu->d_tail = cu->d_head;
}
static void ysig__cu_full(ysig__cu* cu) {
    ysig__cu_clear(cu, cu->replaying ? cu->c_pos : cu->c_head);
    cu->full = 1;
}
static void ysig__undo_note(ysig_graph* g, ysig_value* at) {
    ysig__cu* cu = g->cu;
    ysig__undo* u;
    if (!cu || !cu->n_ck) return;
    if (cu->u_head - cu->u_tail >= (uint64_t)cu->W) { ysig__cu_full(cu); return; }
    u = &cu->undo[cu->u_head++ % (uint64_t)cu->W];
    u->at = at;
    u->old = *at;
}
static void ysig__hl_add(ysig__cu* cu, uint64_t seq, const ysig_hit* h) {
    ysig__hl* l;
    if (!cu->n_ck) return;
    if (cu->h_head - cu->h_tail >= (uint64_t)cu->C) { ysig__cu_full(cu); return; }
    l = &cu->hl[cu->h_head++ % (uint64_t)cu->C];
    l->seq = seq;
    l->h = *h;
    l->given = !seq;
}
static void ysig__dl_add(ysig__cu* cu, uint64_t seq, const yin_event* e) {
    ysig__dl* l;
    if (!cu->n_ck || !cu->DC) return;
    if (cu->d_head - cu->d_tail >= (uint64_t)cu->DC) { ysig__cu_full(cu); return; }
    l = &cu->dl[cu->d_head++ % (uint64_t)cu->DC];
    l->seq = seq;
    l->e = *e;
    l->given = !seq;
}
/* Handed out: the log entry, while the window still has it, is the
 * caller's (a stale index finds another sequence there) */
static void ysig__hl_give(ysig__cu* cu, uint64_t idx, uint64_t seq) {
    if (idx >= cu->h_tail && idx < cu->h_head && cu->hl[idx % (uint64_t)cu->C].seq == seq) cu->hl[idx % (uint64_t)cu->C].given = 1;
}
static void ysig__dl_give(ysig__cu* cu, uint64_t idx, uint64_t seq) {
    if (cu->DC && idx >= cu->d_tail && idx < cu->d_head && cu->dl[idx % (uint64_t)cu->DC].seq == seq) cu->dl[idx % (uint64_t)cu->DC].given = 1;
}
/* A run again: a hit the caller already had (one of the handed-out hits
 * of the old history, not yet matched) is found again; it is not queued
 * twice. Hits are the same when node, t and the whole value are (a hit
 * found at another pass keeps its t_found). */
static const ysig__hl* ysig__hit_seen(ysig__cu* cu, const ysig_hit* h) {
    int i;
    for (i = 0; i < cu->n_tmp; i++) {
        ysig__hl* l = &cu->tmp[i];
        if (l->seq != UINT64_MAX && l->h.node == h->node && l->h.t == h->t && !memcmp(&l->h.value, &h->value, sizeof h->value)) {
            l->seq = UINT64_MAX;
            return l;
        }
    }
    return NULL;
}
/* An emit's events are a sequence, not a set (a collector takes them in
 * order): a run again gives a handed-out event again only as the next
 * of its node's; another one there, a new one before it too, makes the
 * caller's sequence differ from R's (stale) */
static const ysig__dl* ysig__der_seen(ysig__cu* cu, const yin_event* e) {
    int i;
    for (i = 0; i < cu->n_tmpd; i++) {
        ysig__dl* l = &cu->tmpd[i];
        if (l->seq == UINT64_MAX || l->e.device != e->device) continue;
        if (memcmp(&l->e, e, sizeof *e)) { cu->d_stale = 1; return NULL; }
        l->seq = UINT64_MAX;
        return l;
    }
    return NULL;
}
/* v0.7.0: the queues hold 64, and the window's room with one: a caller
 * that holds hits or events past a horizon keeps them in sig.h */
#define YSIG__QCAP(g) (YSIG_QUEUE_CAP + ((g)->cu ? (g)->cu->C : 0))
typedef struct ysig__peak_st { double best; int has, prev, seen, reserved_; } ysig__peak_st;
typedef struct ysig__silent_st { int64_t last, t_reset; } ysig__silent_st;
typedef struct ysig__noise_st { uint64_t s; double y[4]; int64_t t_prev; int init, reserved_; } ysig__noise_st;

/* --- the op table -------------------------------------------------------- */

#define YSIG__F64(n, lo, hi, d, u, doc) { n, "f64", lo, hi, d, u, doc, NULL }
#define YSIG__BIG 1e12

static const ysig_param ysig__p_none[] = {
    { "op", "enum", 0, 30, 0, "", "the node's operation",
      "none=0,source=1,role=2,integrate=3,lowpass=4,velocity=5,speed=6,delay=7,deadzone=8,affine=9,scale=10,"
      "offset=11,clamp=12,select=13,join=14,sum=15,mean=16,diff=17,sines=18,noise=19,channel=20,bind=21,"
      "detect=22,record=23,emit=24,mux=25,latch=26,distance=27,peak=28,all=29,silent=30" }
};
static const ysig_param ysig__p_source[] = {
    { "kind", "enum", 0, 15, 0, "", "the event's kind (ysp/input.h)",
      "keyboard=0,mouse=1,touch=2,pen=3,gamepad=4,box=5,eye=6,sync=7,user=8" },
    { "control", "u32", 0, 4294967295.0, 0, "", "the event's control: an axis (YIN_AXIS_*), a button", NULL },
    { "device", "u32", 0, 4294967295.0, 0, "", "the device; 0 = any", NULL },
    { "fields", "u32", 0, 15, 0, "", "lanes from x (1), y (2), value (4), aux (8), in that order; 0 = x, y, value, "
      "aux by lanes, or value alone for 1 lane", NULL },
    { "tier", "enum", 0, 4, 0, "", "the source table's tier, for events with stamp 0",
      "unknown=0,tier_1=1,tier_2=2,tier_3=3,sim=4" },
    { "type", "enum", 0, 4, 3, "", "the event's type; 0 = sample", "press=1,release=2,sample=3,proximity=4" },
};
static const ysig_param ysig__p_role[] = {
    { "role", "i32", 0, 255, 0, "", "the index of the role in desc.roles", NULL },
};
static const ysig_param ysig__p_integrate[] = {
    YSIG__F64("gain.x", -1e9, 1e9, 1, "", "per count (DELTA) or per unit-second (rate); 0 = 1"),
    YSIG__F64("gain.y", -1e9, 1e9, 1, "", "lane 2's"),
    YSIG__F64("gain.z", -1e9, 1e9, 1, "", "lane 3's"),
    YSIG__F64("gain.w", -1e9, 1e9, 1, "", "lane 4's"),
    YSIG__F64("origin.x", -YSIG__BIG, YSIG__BIG, 0, "", "the value at reset"),
    YSIG__F64("origin.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("origin.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's"),
    YSIG__F64("origin.w", -YSIG__BIG, YSIG__BIG, 0, "", "lane 4's"),
    YSIG__F64("low.x", -YSIG__BIG, YSIG__BIG, 0, "", "lower bound; low and high both 0 = no bound"),
    YSIG__F64("low.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("low.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's"),
    YSIG__F64("low.w", -YSIG__BIG, YSIG__BIG, 0, "", "lane 4's"),
    YSIG__F64("high.x", -YSIG__BIG, YSIG__BIG, 0, "", "upper bound"),
    YSIG__F64("high.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("high.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's"),
    YSIG__F64("high.w", -YSIG__BIG, YSIG__BIG, 0, "", "lane 4's"),
};
static const ysig_param ysig__p_lowpass[] = {
    YSIG__F64("cutoff", 1e-6, 1e6, 0, "Hz", "the -3 dB frequency of each first-order section"),
    { "order", "i32", 0, 2, 1, "", "1 or 2 (two sections); 0 = 1", NULL },
    YSIG__F64("rate", 0, 1e7, 0, "Hz", "the source's sample rate, when known: the cutoff must be under half of it"),
};
static const ysig_param ysig__p_velocity[] = {
    YSIG__F64("lag", 1e-6, 10, 0, "s", "the difference is over at least this"),
    { "capacity", "i32", 0, 1048576, 256, "", "samples kept for the lag; 0 = 256", NULL },
};
static const ysig_param ysig__p_delay[] = {
    YSIG__F64("delay", 0, 100, 0, "s", "the input as it was this long ago"),
    { "capacity", "i32", 0, 1048576, 1024, "", "samples kept; 0 = 1024", NULL },
};
static const ysig_param ysig__p_deadzone[] = {
    YSIG__F64("radius", 0, YSIG__BIG, 0, "", "values within it give 0; past it the norm shrinks by it"),
};
static const ysig_param ysig__p_affine[] = {
    YSIG__F64("m.xx", -YSIG__BIG, YSIG__BIG, 0, "", "the matrix, row-major; all four 0 = the identity"),
    YSIG__F64("m.xy", -YSIG__BIG, YSIG__BIG, 0, "", "row 1, column 2"),
    YSIG__F64("m.yx", -YSIG__BIG, YSIG__BIG, 0, "", "row 2, column 1"),
    YSIG__F64("m.yy", -YSIG__BIG, YSIG__BIG, 0, "", "row 2, column 2"),
    YSIG__F64("pre.x", -YSIG__BIG, YSIG__BIG, 0, "", "subtracted before the matrix: the rotation's center"),
    YSIG__F64("pre.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("post.x", -YSIG__BIG, YSIG__BIG, 0, "", "added after it"),
    YSIG__F64("post.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("angle", -36000, 36000, 0, "deg", "a rotation after the matrix, counterclockwise with y up; plus "
              "input 2"),
};
static const ysig_param ysig__p_scale[] = {
    YSIG__F64("k.x", -YSIG__BIG, YSIG__BIG, 1, "", "the factor; 0 = 1"),
    YSIG__F64("k.y", -YSIG__BIG, YSIG__BIG, 1, "", "lane 2's"),
    YSIG__F64("k.z", -YSIG__BIG, YSIG__BIG, 1, "", "lane 3's"),
    YSIG__F64("k.w", -YSIG__BIG, YSIG__BIG, 1, "", "lane 4's"),
};
static const ysig_param ysig__p_offset[] = {
    YSIG__F64("d.x", -YSIG__BIG, YSIG__BIG, 0, "", "added"),
    YSIG__F64("d.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("d.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's"),
    YSIG__F64("d.w", -YSIG__BIG, YSIG__BIG, 0, "", "lane 4's"),
};
static const ysig_param ysig__p_clamp[] = {
    YSIG__F64("low.x", -YSIG__BIG, YSIG__BIG, 0, "", "low and high both 0 = no bound"),
    YSIG__F64("low.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("low.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's"),
    YSIG__F64("low.w", -YSIG__BIG, YSIG__BIG, 0, "", "lane 4's"),
    YSIG__F64("high.x", -YSIG__BIG, YSIG__BIG, 0, "", "upper bound"),
    YSIG__F64("high.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("high.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's"),
    YSIG__F64("high.w", -YSIG__BIG, YSIG__BIG, 0, "", "lane 4's"),
};
static const ysig_param ysig__p_select[] = {
    { "lane.x", "i32", 0, 3, 0, "", "the input lane of output lane 1", NULL },
    { "lane.y", "i32", 0, 3, 0, "", "of output lane 2", NULL },
    { "lane.z", "i32", 0, 3, 0, "", "of output lane 3", NULL },
    { "lane.w", "i32", 0, 3, 0, "", "of output lane 4", NULL },
};
static const ysig_param ysig__p_sum[] = {
    YSIG__F64("w.0", -YSIG__BIG, YSIG__BIG, 1, "", "input 1's weight; 0 = 1"),
    YSIG__F64("w.1", -YSIG__BIG, YSIG__BIG, 1, "", "input 2's"),
    YSIG__F64("w.2", -YSIG__BIG, YSIG__BIG, 1, "", "input 3's"),
    YSIG__F64("w.3", -YSIG__BIG, YSIG__BIG, 1, "", "input 4's"),
};
static const ysig_param ysig__p_sines[] = {
    { "n", "i32", 1, 64, 1, "", "components per lane; data holds lanes x n x (amplitude, Hz, phase rad)", NULL },
    YSIG__F64("ramp", 0, 1e6, 0, "s", "the amplitude rises from 0 to 1 over this from base time 0; 0 = none"),
};
static const ysig_param ysig__p_noise[] = {
    YSIG__F64("seed", 0, 9007199254740992.0, 0, "", "the generator's seed, set again at each reset"),
    YSIG__F64("sd", 0, YSIG__BIG, 1, "", "the standard deviation before the filter; 0 = 1"),
    YSIG__F64("cutoff", 0, 1e6, 0, "Hz", "a first-order low-pass per frame; 0 = white"),
};
static const ysig_param ysig__p_channel[] = {
    { "channel", "i32", 0, 65535, 0, "", "the timeline channel (ytl_values()) the frame gives", NULL },
};
static const ysig_param ysig__p_bind[] = {
    { "channel", "i32", 0, 65535, 0, "", "desc.channels[channel + lane] := lane, as float, at each frame", NULL },
};
static const ysig_param ysig__p_detect[] = {
    { "test", "enum", 0, 3, 0, "", "the test", "ge=0,le=1,in_rect=2,out_rect=3" },
    { "lane", "i32", -1, 3, 0, "", "ge and le: the lane, or -1 for the norm of value - center", NULL },
    YSIG__F64("level", -YSIG__BIG, YSIG__BIG, 0, "", "ge and le: the threshold"),
    YSIG__F64("hysteresis", 0, YSIG__BIG, 0, "", "ready again when the test fails by this much"),
    YSIG__F64("hold", 0, 1e6, 0, "s", "the test true for this long; 0 = at once"),
    { "at", "enum", 0, 1, 0, "", "test each sample, or each frame's held value", "sample=0,frame=1" },
    { "start", "enum", 0, 1, 0, "", "armed at reset, or by ysig_arm()", "reset=0,arm=1" },
    { "repeat", "enum", 0, 1, 0, "", "one hit per arm, or every crossing", "once=0,every=1" },
    YSIG__F64("a.x", -YSIG__BIG, YSIG__BIG, 0, "", "the center (norm), or the rectangle's low x"),
    YSIG__F64("a.y", -YSIG__BIG, YSIG__BIG, 0, "", "the center's y, or the rectangle's low y"),
    YSIG__F64("b.x", -YSIG__BIG, YSIG__BIG, 0, "", "the center's z, or the rectangle's high x"),
    YSIG__F64("b.y", -YSIG__BIG, YSIG__BIG, 0, "", "the center's w, or the rectangle's high y"),
    { "entry", "enum", 0, 2, 0, "", "armed while its test is true: a hold counts from the arm, or waits for the test "
      "to fail and come true again; now: with no hold, a hit at the arm (frame rate: at the next onset)",
      "arm=0,fresh=1,now=2" },
    { "out", "enum", 0, 1, 0, "", "the output: the test's state, or 1 from a hit until ready again (one-hit: until the "
      "next arm or reset)", "test=0,hit=1" },
};
static const ysig_param ysig__p_record[] = {
    { "per", "enum", 0, 1, 0, "", "a row per sample of the input, or per frame", "sample=0,frame=1" },
};
static const ysig_param ysig__p_emit[] = {
    { "kind", "i32", 0, 7, 0, "", "the derived event's kind is YIN_KIND_USER + this", NULL },
    { "control", "u32", 0, 4294967295.0, 0, "", "the derived event's control", NULL },
};
static const ysig_param ysig__p_distance[] = {
    YSIG__F64("point.x", -YSIG__BIG, YSIG__BIG, 0, "", "the point, when no second input gives it"),
    YSIG__F64("point.y", -YSIG__BIG, YSIG__BIG, 0, "", "lane 2's"),
    YSIG__F64("point.z", -YSIG__BIG, YSIG__BIG, 0, "", "lane 3's (a 3-lane value)"),
};
static const ysig_param ysig__p_peak[] = {
    { "lane", "i32", -1, 3, 0, "", "the lane, or -1 for the norm", NULL },
};
static const ysig_param ysig__p_silent[] = {
    YSIG__F64("after", 1e-6, 1e6, 0, "s", "no sample for this long, judged at the poll"),
};

typedef struct ysig__opinfo {
    const char*       name;
    uint8_t           min_in, max_in, rule, kind;
    const ysig_param* params;
    int               n_params;
} ysig__opinfo;

#define YSIG__N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const ysig__opinfo ysig__ops[YSIG_OP_COUNT] = {
    { "none", 0, 0, YSIG__R_SINK, 0, ysig__p_none, 1 },
    { "source", 0, 0, YSIG__R_SOURCE, 0, ysig__p_source, YSIG__N(ysig__p_source) },
    { "role", 0, 0, YSIG__R_ROLE, YSIG__K_PURE, ysig__p_role, YSIG__N(ysig__p_role) },
    { "integrate", 1, 1, YSIG__R_SAME, 0, ysig__p_integrate, YSIG__N(ysig__p_integrate) },
    { "lowpass", 1, 1, YSIG__R_SAME, YSIG__K_VIEW, ysig__p_lowpass, YSIG__N(ysig__p_lowpass) },
    { "velocity", 1, 1, YSIG__R_SAME, YSIG__K_VIEW, ysig__p_velocity, YSIG__N(ysig__p_velocity) },
    { "speed", 1, 1, YSIG__R_ONE, YSIG__K_VIEW, ysig__p_velocity, YSIG__N(ysig__p_velocity) },
    { "delay", 1, 1, YSIG__R_SAME, YSIG__K_VIEW, ysig__p_delay, YSIG__N(ysig__p_delay) },
    { "deadzone", 1, 1, YSIG__R_SAME, YSIG__K_PURE, ysig__p_deadzone, YSIG__N(ysig__p_deadzone) },
    { "affine", 1, 2, YSIG__R_TWO, YSIG__K_PURE, ysig__p_affine, YSIG__N(ysig__p_affine) },
    { "scale", 1, 2, YSIG__R_SAME, YSIG__K_PURE, ysig__p_scale, YSIG__N(ysig__p_scale) },
    { "offset", 1, 1, YSIG__R_SAME, YSIG__K_PURE, ysig__p_offset, YSIG__N(ysig__p_offset) },
    { "clamp", 1, 1, YSIG__R_SAME, YSIG__K_PURE, ysig__p_clamp, YSIG__N(ysig__p_clamp) },
    { "select", 1, 1, YSIG__R_SELECT, YSIG__K_PURE, ysig__p_select, YSIG__N(ysig__p_select) },
    { "join", 1, 4, YSIG__R_JOIN, YSIG__K_PURE, NULL, 0 },
    { "sum", 1, 4, YSIG__R_SAME, YSIG__K_PURE, ysig__p_sum, YSIG__N(ysig__p_sum) },
    { "mean", 1, 4, YSIG__R_SAME, YSIG__K_PURE, ysig__p_sum, YSIG__N(ysig__p_sum) },
    { "diff", 2, 2, YSIG__R_SAME, YSIG__K_PURE, NULL, 0 },
    { "sines", 0, 0, YSIG__R_GEN, YSIG__K_GEN, ysig__p_sines, YSIG__N(ysig__p_sines) },
    { "noise", 0, 0, YSIG__R_GEN, YSIG__K_GEN, ysig__p_noise, YSIG__N(ysig__p_noise) },
    { "channel", 0, 0, YSIG__R_GEN, YSIG__K_GEN, ysig__p_channel, YSIG__N(ysig__p_channel) },
    { "bind", 1, 1, YSIG__R_SINK, YSIG__K_SINK, ysig__p_bind, YSIG__N(ysig__p_bind) },
    { "detect", 1, 1, YSIG__R_ONE, YSIG__K_SINK, ysig__p_detect, YSIG__N(ysig__p_detect) },
    { "record", 1, 1, YSIG__R_SINK, YSIG__K_SINK, ysig__p_record, YSIG__N(ysig__p_record) },
    { "emit", 1, 1, YSIG__R_SINK, YSIG__K_SINK, ysig__p_emit, YSIG__N(ysig__p_emit) },
    { "mux", 3, 3, YSIG__R_MUX, YSIG__K_PURE, NULL, 0 },
    { "latch", 2, 2, YSIG__R_FIRST, 0, NULL, 0 },
    { "distance", 1, 2, YSIG__R_ONE, YSIG__K_PURE, ysig__p_distance, YSIG__N(ysig__p_distance) },
    { "peak", 1, 2, YSIG__R_ONE, 0, ysig__p_peak, YSIG__N(ysig__p_peak) },
    { "all", 2, 4, YSIG__R_ONE, YSIG__K_PURE, NULL, 0 },
    { "silent", 1, 1, YSIG__R_ONE, YSIG__K_VIEW, ysig__p_silent, YSIG__N(ysig__p_silent) },
};

YSIG_API const char* ysig_version(void) { return YSIG_VERSION_STRING; }

YSIG_API const ysig_param* ysig_op_params(int op, int* n) {
    if (op < 0 || op >= YSIG_OP_COUNT) { if (n) *n = 0; return NULL; }
    if (n) *n = ysig__ops[op].n_params;
    return ysig__ops[op].params;
}

YSIG_API const char* ysig_op_name(int op) {
    return op >= 0 && op < YSIG_OP_COUNT ? ysig__ops[op].name : NULL;
}

/* --- analysis: the combined graph, from a desc ----------------------------- */

/* Node ids: the experiment's 0..n-1, then each adapter's in turn. The
 * evaluation order puts the adapters first: a role reads its adapter's last
 * node. */
typedef struct ysig__an {
    int      n, n_exp, n_src, sched_total, n_frame;
    int32_t  base[16];
    const ysig_node* node[YSIG_MAX_NODES];
    int16_t  in[YSIG_MAX_NODES][4];
    uint8_t  n_in[YSIG_MAX_NODES], lanes[YSIG_MAX_NODES], flags[YSIG_MAX_NODES];
    int16_t  src1[YSIG_MAX_NODES];        /* -1 none, -2 two or more     */
    int32_t  hist[YSIG_MAX_NODES];
    int16_t  order[YSIG_MAX_NODES];
} ysig__an;

static bool ysig__fail(char* err, size_t size, const char* fmt, ...) {
    va_list ap;
    if (err && size) {
        va_start(ap, fmt);
        vsnprintf(err, size, fmt, ap);
        va_end(ap);
    }
    return false;
}

static const char* ysig__nm(const ysig_node* n) { return n && n->name ? n->name : "(unnamed)"; }

static int ysig__type_lanes(int type) {
    switch (type) {
    case YSIG_TYPE_POSITION2D: case YSIG_TYPE_POSITION2D_RELATIVE: case YSIG_TYPE_GAZE: return 2;
    case YSIG_TYPE_POSITION3D: return 3;
    case YSIG_TYPE_SCALAR: return 1;
    default: return 0;
    }
}

static int ysig__popcount4(unsigned m) { return (int)((m & 1u) + ((m >> 1) & 1u) + ((m >> 2) & 1u) + ((m >> 3) & 1u)); }

static const ysig_op_vtable* ysig__vt(const ysig_desc* d, int op) {
    int i = op - YSIG_OP_CUSTOM;
    if (op < YSIG_OP_CUSTOM || !d->custom || i >= d->n_custom) return NULL;
    return d->custom[i];
}

static const ysig_param* ysig__table(const ysig_desc* d, int op, int* n) {
    const ysig_op_vtable* vt;
    if (op < YSIG_OP_COUNT) { *n = ysig__ops[op].n_params; return ysig__ops[op].params; }
    vt = ysig__vt(d, op);
    *n = vt ? vt->n_params : 0;
    return vt ? vt->params : NULL;
}

/* A value against its row: range, and whole numbers for the integer and
 * enum types. */
static bool ysig__param_ok(const ysig_param* r, double v) {
    if (!(v >= r->min && v <= r->max)) return false;
    if (r->type && r->type[0] != 'f' && v != floor(v)) return false;
    return true;
}

static bool ysig__role_idx(const ysig_desc* d, const char* name) {
    int i;
    for (i = 0; i < d->n_roles; i++)
        if (d->roles[i].name && name && !strcmp(d->roles[i].name, name)) return true;
    return false;
}

/* A frame-only sink is in no sample's list. */
static int ysig__frame_only(const ysig_node* nd) {
    return nd->op == YSIG_OP_BIND || (nd->op == YSIG_OP_RECORD && nd->p[0] == 1) ||
           (nd->op == YSIG_OP_DETECT && nd->p[5] == 1);
}

static int ysig__in_frame(const ysig__an* a, int id) {
    const ysig_node* nd = a->node[id];
    switch (nd->op) {
    case YSIG_OP_BIND: case YSIG_OP_DETECT: return 1;
    case YSIG_OP_RECORD: return nd->p[0] == 1;
    case YSIG_OP_EMIT: return (a->flags[a->in[id][0]] & YSIG__F_FRAME) != 0;
    default: return (a->flags[id] & YSIG__F_FRAME) && !(a->flags[id] & YSIG__F_SINK);
    }
}

/* The schedules, counted (pointers NULL) or written: per source, every
 * node it reaches in evaluation order; per frame, the frame list. */
static void ysig__sched(ysig__an* a, int16_t* src_ids, int32_t* src_off, int16_t* sched, int16_t* frame) {
    uint8_t reach[YSIG_MAX_NODES];
    int k, j, q, ns = 0, total = 0, nf = 0;
    for (k = 0; k < a->n; k++) {
        int s = a->order[k];
        if (a->node[s]->op != YSIG_OP_SOURCE) continue;
        if (src_ids) { src_ids[ns] = (int16_t)s; src_off[ns] = total; }
        ns++;
        memset(reach, 0, (size_t)a->n);
        reach[s] = 1;
        for (j = k + 1; j < a->n; j++) {
            int id = a->order[j];
            for (q = 0; q < a->n_in[id]; q++)
                if (reach[a->in[id][q]]) { reach[id] = 1; break; }
            if (!reach[id] || ysig__frame_only(a->node[id])) continue;
            if (sched) sched[total] = (int16_t)id;
            total++;
        }
    }
    if (src_off) src_off[ns] = total;
    for (k = 0; k < a->n; k++) {
        int id = a->order[k];
        if (!ysig__in_frame(a, id)) continue;
        if (frame) frame[nf] = (int16_t)id;
        nf++;
    }
    a->n_src = ns;
    a->sched_total = total;
    a->n_frame = nf;
}

static size_t ysig__layout(const ysig_desc* d, const ysig__an* a, ysig_graph* g, char* base);

static bool ysig__analyze(const ysig_desc* d, ysig__an* a, char* err, size_t es) {
    int i, j, k, part, total;
    memset(a->n_in, 0, sizeof a->n_in);
    if (!d) return ysig__fail(err, es, "ysig: no desc");
    if (!d->nodes || d->n_nodes < 1) return ysig__fail(err, es, "ysig: no nodes");
    if (d->n_roles < 0 || d->n_adapters < 0 || d->n_custom < 0 || d->n_channels < 0)
        return ysig__fail(err, es, "ysig: a negative count");
    if (d->n_adapters > 16) return ysig__fail(err, es, "ysig: %d adapters; at most 16", d->n_adapters);
    if ((d->n_roles && !d->roles) || (d->n_adapters && !d->adapters) || (d->n_custom && !d->custom) ||
        (d->n_channels && !d->channels))
        return ysig__fail(err, es, "ysig: a count without its array");
    if (d->n_roles > 256) return ysig__fail(err, es, "ysig: %d roles; at most 256", d->n_roles);
    if (d->catchup_frames < 0 || d->catchup_frames > 8 || d->catchup_calls < 0 || d->catchup_calls > (1 << 20) ||
        d->catchup_writes < 0 || d->catchup_writes > (1 << 24))
        return ysig__fail(err, es, "ysig: catchup_frames %d (0 to 8), catchup_calls %d (0 to 2^20), catchup_writes %d (0 to 2^24)",
                          d->catchup_frames, d->catchup_calls, d->catchup_writes);
    if (d->catchup_rows < 0 || d->catchup_rows > (1 << 22))
        return ysig__fail(err, es, "ysig: catchup_rows %d (0 to 2^22)", d->catchup_rows);
    if (d->catchup_ns < 0 || d->catchup_ns > (int64_t)3600 * 1000000000)
        return ysig__fail(err, es, "ysig: catchup_ns %lld (0 to an hour)", (long long)d->catchup_ns);
    total = d->n_nodes;
    for (i = 0; i < d->n_adapters; i++) {
        if (!d->adapters[i].nodes || d->adapters[i].n_nodes < 1)
            return ysig__fail(err, es, "ysig: adapter %d has no nodes", i);
        if (d->adapters[i].n_nodes > YSIG_MAX_NODES) return ysig__fail(err, es, "ysig: adapter %d too long", i);
        total += d->adapters[i].n_nodes;
        if (total > YSIG_MAX_NODES) break;
    }
    if (d->n_nodes > YSIG_MAX_NODES || total > YSIG_MAX_NODES)
        return ysig__fail(err, es, "ysig: more than %d nodes", YSIG_MAX_NODES);
    for (i = 0; i < d->n_custom; i++) {
        const ysig_op_vtable* vt = d->custom[i];
        if (!vt || vt->version != YSIG_OP_ABI || !vt->name)
            return ysig__fail(err, es, "ysig: custom op %d: no vtable, or ABI %u (want %d)", i, vt ? vt->version : 0u,
                              YSIG_OP_ABI);
        if ((vt->n_params && !vt->params) || vt->n_params < 0 || vt->n_params > YSIG_NP || vt->state_size > (1u << 24) ||
            vt->lanes_out > 4)
            return ysig__fail(err, es, "ysig: custom op %s: bad table, state or lanes", vt->name);
        for (j = 0; j < 4; j++)
            if (vt->lanes_in[j] > 4) return ysig__fail(err, es, "ysig: custom op %s: lanes_in > 4", vt->name);
    }
    a->n = total;
    a->n_exp = d->n_nodes;
    /* ids and inputs; the experiment's part is part -1 */
    {
        int id = d->n_nodes;
        for (part = 0; part < d->n_adapters; part++) {
            a->base[part] = id;
            id += d->adapters[part].n_nodes;
        }
    }
    for (part = -1; part < d->n_adapters; part++) {
        const ysig_node* arr = part < 0 ? d->nodes : d->adapters[part].nodes;
        int cnt = part < 0 ? d->n_nodes : d->adapters[part].n_nodes, base = part < 0 ? 0 : a->base[part];
        for (i = 0; i < cnt; i++) {
            const ysig_node* nd = &arr[i];
            int id = base + i, op = nd->op, mn, mx, gap = 0;
            const ysig_param* rows;
            int nr;
            a->node[id] = nd;
            if (op == YSIG_OP_NONE || (op >= YSIG_OP_COUNT && !ysig__vt(d, op)))
                return ysig__fail(err, es, "ysig: node %d (%s): unknown op %d", id, ysig__nm(nd), op);
            if (op == YSIG_OP_ROLE && part >= 0)
                return ysig__fail(err, es, "ysig: node %d (%s): a role inside an adapter", id, ysig__nm(nd));
            for (k = 0; k < 4; k++) {
                int v = nd->in[k];
                if (v < 0) { if (v != -1) return ysig__fail(err, es, "ysig: node %d: input %d is %d", id, k, v); gap = 1; continue; }
                if (gap) return ysig__fail(err, es, "ysig: node %d (%s): input %d after a -1", id, ysig__nm(nd), k);
                if (v >= i)
                    return ysig__fail(err, es, "ysig: node %d (%s): input %d is node %d, not before it (a cycle)", id,
                                      ysig__nm(nd), k, v);
                a->in[id][k] = (int16_t)(base + v);
                a->n_in[id]++;
            }
            for (k = a->n_in[id]; k < 4; k++) a->in[id][k] = -1;
            if (op < YSIG_OP_COUNT) { mn = ysig__ops[op].min_in; mx = ysig__ops[op].max_in; }
            else { mn = 0; mx = 4; }
            if (a->n_in[id] < mn || a->n_in[id] > mx)
                return ysig__fail(err, es, "ysig: node %d (%s): %s takes %d to %d inputs, has %d", id, ysig__nm(nd),
                                  op < YSIG_OP_COUNT ? ysig__ops[op].name : "custom", mn, mx, a->n_in[id]);
            rows = ysig__table(d, op, &nr);
            for (k = 0; k < YSIG_NP; k++) {
                if (!isfinite(nd->p[k])) return ysig__fail(err, es, "ysig: node %d (%s): p[%d] not finite", id, ysig__nm(nd), k);
                if (k < nr && !ysig__param_ok(&rows[k], nd->p[k]))
                    return ysig__fail(err, es, "ysig: node %d (%s): %s = %g out of [%g, %g]", id, ysig__nm(nd), rows[k].name,
                                      nd->p[k], rows[k].min, rows[k].max);
            }
            if (nd->n_data < 0 || (nd->n_data && !nd->data) || nd->n_data > 4096)
                return ysig__fail(err, es, "ysig: node %d (%s): bad data", id, ysig__nm(nd));
            if (nd->n_data && op != YSIG_OP_SINES && op < YSIG_OP_COUNT)
                return ysig__fail(err, es, "ysig: node %d (%s): data on an op that reads none", id, ysig__nm(nd));
            for (k = 0; k < nd->n_data; k++)
                if (!isfinite(nd->data[k])) return ysig__fail(err, es, "ysig: node %d: data[%d] not finite", id, k);
            if (nd->history < 0 || nd->history > YSIG_MAX_HISTORY)
                return ysig__fail(err, es, "ysig: node %d (%s): history %d", id, ysig__nm(nd), (int)nd->history);
            if (nd->lanes > 4) return ysig__fail(err, es, "ysig: node %d (%s): %d lanes", id, ysig__nm(nd), nd->lanes);
        }
    }
    /* the order: adapters, then the experiment */
    k = 0;
    for (part = 0; part < d->n_adapters; part++)
        for (i = 0; i < d->adapters[part].n_nodes; i++) a->order[k++] = (int16_t)(a->base[part] + i);
    for (i = 0; i < d->n_nodes; i++) a->order[k++] = (int16_t)i;
    /* roles resolve to their adapter's last node */
    for (i = 0; i < d->n_roles; i++) {
        const ysig_role* r = &d->roles[i];
        if (!r->name || r->type == YSIG_TYPE_NONE || r->type >= YSIG_TYPE_COUNT || r->units >= YSIG_UNITS_COUNT)
            return ysig__fail(err, es, "ysig: role %d: no name, or a bad type or units", i);
        for (j = 0; j < i; j++)
            if (!strcmp(d->roles[j].name, r->name)) return ysig__fail(err, es, "ysig: role %s twice", r->name);
    }
    for (part = 0; part < d->n_adapters; part++) {
        const ysig_adapter* ad = &d->adapters[part];
        if (!ad->role || !ysig__role_idx(d, ad->role))
            return ysig__fail(err, es, "ysig: adapter %d is for no declared role (%s)", part, ad->role ? ad->role : "NULL");
        for (j = 0; j < part; j++)
            if (!strcmp(d->adapters[j].role, ad->role)) return ysig__fail(err, es, "ysig: two adapters for role %s", ad->role);
    }
    for (i = 0; i < d->n_nodes; i++) {
        const ysig_node* nd = &d->nodes[i];
        const ysig_role* r;
        const ysig_adapter* ad = NULL;
        int ri;
        if (nd->op != YSIG_OP_ROLE) continue;
        ri = (int)nd->p[0];
        if (ri < 0 || ri >= d->n_roles) return ysig__fail(err, es, "ysig: node %d (%s): role %d not declared", i, ysig__nm(nd), ri);
        r = &d->roles[ri];
        for (part = 0; part < d->n_adapters; part++)
            if (!strcmp(d->adapters[part].role, r->name)) { ad = &d->adapters[part]; break; }
        if (!ad) return ysig__fail(err, es, "ysig: role %s has no adapter", r->name);
        if (ad->units != r->units)
            return ysig__fail(err, es, "ysig: role %s: the adapter's units (%d) are not the role's (%d)", r->name, ad->units,
                              r->units);
        if (ad->type != r->type &&
            !(r->type == YSIG_TYPE_POSITION2D && ad->type == YSIG_TYPE_POSITION2D_RELATIVE && r->allow_relative))
            return ysig__fail(err, es, "ysig: role %s: the adapter's type (%d) is not the role's (%d)%s", r->name, ad->type,
                              r->type, r->type == YSIG_TYPE_POSITION2D && ad->type == YSIG_TYPE_POSITION2D_RELATIVE
                                           ? "; a relative device needs allow_relative" : "");
        a->in[i][0] = (int16_t)(a->base[part] + ad->n_nodes - 1);
        a->n_in[i] = 1;
    }
    /* lanes and flags, in evaluation order */
    a->n_src = a->sched_total = a->n_frame = 0;
    for (k = 0; k < total; k++) {
        int id = a->order[k], op = a->node[id]->op, lanes = 0, in0 = a->n_in[id] ? a->lanes[a->in[id][0]] : 0;
        const ysig_node* nd = a->node[id];
        const ysig_op_vtable* vt = op >= YSIG_OP_COUNT ? ysig__vt(d, op) : NULL;
        int rule = vt ? -1 : ysig__ops[op].rule, kind = vt ? 0 : ysig__ops[op].kind;
        uint8_t fl = 0;
        int16_t s1 = -1;
        for (j = 0; j < a->n_in[id]; j++) {
            int u = a->in[id][j];
            /* a detect's output is its test: other sinks have no output */
            if ((a->flags[u] & YSIG__F_SINK) && a->lanes[u] == 0)
                return ysig__fail(err, es, "ysig: node %d (%s): input %d is a sink", id, ysig__nm(nd), j);
            if (a->flags[u] & YSIG__F_FRAME) fl |= YSIG__F_FRAME;
            if (a->src1[u] == -2 || (a->src1[u] >= 0 && s1 >= 0 && a->src1[u] != s1)) s1 = -2;
            else if (a->src1[u] >= 0 && s1 == -1) s1 = a->src1[u];
        }
        switch (rule) {
        case YSIG__R_SOURCE: {
            unsigned m = (unsigned)nd->p[3];
            lanes = m ? ysig__popcount4(m) : (nd->lanes ? nd->lanes : 2);
            s1 = (int16_t)id;
            break;
        }
        case YSIG__R_ROLE: {
            int want = ysig__type_lanes(d->roles[(int)nd->p[0]].type);
            if (in0 != want)
                return ysig__fail(err, es, "ysig: node %d (%s): the adapter gives %d lanes, the role's type %d", id,
                                  ysig__nm(nd), in0, want);
            lanes = want;
            break;
        }
        case YSIG__R_SAME:
            lanes = in0;
            for (j = 1; j < a->n_in[id]; j++) {
                int lj = a->lanes[a->in[id][j]];
                if (op == YSIG_OP_SCALE ? lj != 1 : lj != in0)
                    return ysig__fail(err, es, "ysig: node %d (%s): input %d has %d lanes, %s", id, ysig__nm(nd), j, lj,
                                      op == YSIG_OP_SCALE ? "the factor takes 1" : "input 1 has a different count");
            }
            break;
        case YSIG__R_ONE: lanes = 1; break;
        case YSIG__R_FIRST: lanes = in0; break;
        case YSIG__R_MUX:
            if (a->lanes[a->in[id][1]] != in0)
                return ysig__fail(err, es, "ysig: node %d (%s): mux takes a and b of the same lanes; a has %d, b %d", id,
                                  ysig__nm(nd), in0, a->lanes[a->in[id][1]]);
            lanes = in0;
            break;
        case YSIG__R_TWO:
            if (in0 != 2 || (a->n_in[id] > 1 && a->lanes[a->in[id][1]] != 1))
                return ysig__fail(err, es, "ysig: node %d (%s): affine takes 2 lanes and a 1-lane angle", id, ysig__nm(nd));
            lanes = 2;
            break;
        case YSIG__R_JOIN:
            for (j = 0; j < a->n_in[id]; j++) lanes += a->lanes[a->in[id][j]];
            if (lanes > 4) return ysig__fail(err, es, "ysig: node %d (%s): join gives %d lanes; at most 4", id, ysig__nm(nd), lanes);
            break;
        case YSIG__R_SELECT:
            lanes = nd->lanes;
            if (lanes < 1) return ysig__fail(err, es, "ysig: node %d (%s): select needs lanes", id, ysig__nm(nd));
            for (j = 0; j < lanes; j++)
                if ((int)nd->p[j] >= in0)
                    return ysig__fail(err, es, "ysig: node %d (%s): lane %d of a %d-lane input", id, ysig__nm(nd), (int)nd->p[j], in0);
            break;
        case YSIG__R_GEN:
            lanes = nd->lanes ? nd->lanes : 1;
            break;
        case YSIG__R_SINK: lanes = 0; break;
        default: /* custom */
            for (j = 0; j < a->n_in[id]; j++)
                if (vt->lanes_in[j] && vt->lanes_in[j] != a->lanes[a->in[id][j]])
                    return ysig__fail(err, es, "ysig: node %d (%s): %s's input %d takes %d lanes", id, ysig__nm(nd), vt->name, j,
                                      vt->lanes_in[j]);
            lanes = vt->lanes_out ? vt->lanes_out : a->n_in[id] ? in0 : nd->lanes;
            if (lanes < 1) return ysig__fail(err, es, "ysig: node %d (%s): no lanes", id, ysig__nm(nd));
            if (!a->n_in[id]) { kind = YSIG__K_GEN; if (!vt->frame) return ysig__fail(err, es, "ysig: %s: no frame()", vt->name); }
            else if (!vt->sample) return ysig__fail(err, es, "ysig: %s: no sample()", vt->name);
            break;
        }
        if (nd->lanes && lanes && nd->lanes != lanes)
            return ysig__fail(err, es, "ysig: node %d (%s): lanes %d, the op gives %d", id, ysig__nm(nd), nd->lanes, lanes);
        if (rule != YSIG__R_SINK && (lanes < 1 || lanes > 4))
            return ysig__fail(err, es, "ysig: node %d (%s): %d lanes", id, ysig__nm(nd), lanes);
        /* the op's own rules */
        switch (op) {
        case YSIG_OP_INTEGRATE:
            for (j = 0; j < lanes; j++) {
                double lo = nd->p[8 + j], hi = nd->p[12 + j], gn = nd->p[j];
                if ((lo != 0 || hi != 0) && !(lo < hi))
                    return ysig__fail(err, es, "ysig: node %d (%s): bound %d: low %g not below high %g", id, ysig__nm(nd), j, lo, hi);
                if ((lo != 0 || hi != 0) && gn < 0)
                    return ysig__fail(err, es, "ysig: node %d (%s): a bounded lane needs a positive gain (flip with affine)",
                                      id, ysig__nm(nd));
            }
            /* counts: a raw mouse's DELTA samples only (a box's channel 1
             * is not a count) */
            if (a->node[a->in[id][0]]->op == YSIG_OP_SOURCE && (uint32_t)a->node[a->in[id][0]]->p[1] == YIN_AXIS_DELTA &&
                (int)a->node[a->in[id][0]]->p[0] == YIN_KIND_MOUSE)
                fl |= YSIG__F_COUNTS;
            else fl |= YSIG__F_FRAME | YSIG__F_VIEW;
            break;
        case YSIG_OP_LOWPASS:
            if (nd->p[2] > 0 && !(nd->p[0] < nd->p[2] / 2))
                return ysig__fail(err, es, "ysig: node %d (%s): cutoff %g Hz at %g Hz sampling (Nyquist)", id, ysig__nm(nd),
                                  nd->p[0], nd->p[2]);
            break;
        case YSIG_OP_CLAMP:
            for (j = 0; j < lanes; j++)
                if ((nd->p[j] != 0 || nd->p[4 + j] != 0) && nd->p[j] > nd->p[4 + j])
                    return ysig__fail(err, es, "ysig: node %d (%s): clamp %d: low above high", id, ysig__nm(nd), j);
            break;
        case YSIG_OP_SINES:
            if (lanes > 2) return ysig__fail(err, es, "ysig: node %d (%s): sines gives 1 or 2 lanes", id, ysig__nm(nd));
            if (nd->n_data != lanes * 3 * (int)nd->p[0])
                return ysig__fail(err, es, "ysig: node %d (%s): data has %d values; %d lanes x %d x 3 = %d", id, ysig__nm(nd),
                                  nd->n_data, lanes, (int)nd->p[0], lanes * 3 * (int)nd->p[0]);
            break;
        case YSIG_OP_CHANNEL:
            if (lanes != 1) return ysig__fail(err, es, "ysig: node %d (%s): channel gives 1 lane", id, ysig__nm(nd));
            break;
        case YSIG_OP_BIND:
            if ((int)nd->p[0] + in0 > d->n_channels)
                return ysig__fail(err, es, "ysig: node %d (%s): channels %d..%d, desc has %d", id, ysig__nm(nd), (int)nd->p[0],
                                  (int)nd->p[0] + in0 - 1, d->n_channels);
            break;
        case YSIG_OP_DETECT:
            if ((int)nd->p[1] >= in0)
                return ysig__fail(err, es, "ysig: node %d (%s): lane %d of %d", id, ysig__nm(nd), (int)nd->p[1], in0);
            if (((int)nd->p[0] == YSIG_TEST_IN_RECT || (int)nd->p[0] == YSIG_TEST_OUT_RECT) && in0 < 2)
                return ysig__fail(err, es, "ysig: node %d (%s): a rectangle needs 2 lanes", id, ysig__nm(nd));
            if ((int)nd->p[0] >= YSIG_TEST_IN_RECT && (nd->p[8] > nd->p[10] || nd->p[9] > nd->p[11]))
                return ysig__fail(err, es, "ysig: node %d (%s): the rectangle's low corner is above its high", id, ysig__nm(nd));
            /* its output can change at a frame: a frame-rate test, or a
             * hit found by a frame (a hold), so its users run at frames */
            if ((int)nd->p[5] == 1 || (int)nd->p[13] == YSIG_OUT_HIT) fl |= YSIG__F_FRAME;
            break;
        case YSIG_OP_DISTANCE:
            if (in0 != 2 && in0 != 3)
                return ysig__fail(err, es, "ysig: node %d (%s): distance takes 2 or 3 lanes, has %d", id, ysig__nm(nd), in0);
            if (a->n_in[id] > 1 && a->lanes[a->in[id][1]] != in0)
                return ysig__fail(err, es, "ysig: node %d (%s): the point has %d lanes, the value %d", id, ysig__nm(nd),
                                  a->lanes[a->in[id][1]], in0);
            break;
        case YSIG_OP_PEAK:
            if ((int)nd->p[0] >= in0)
                return ysig__fail(err, es, "ysig: node %d (%s): lane %d of %d", id, ysig__nm(nd), (int)nd->p[0], in0);
            break;
        default: break;
        }
        if (kind & YSIG__K_GEN) fl |= YSIG__F_GEN | YSIG__F_FRAME;
        if (kind & YSIG__K_SINK) fl |= YSIG__F_SINK;
        if (kind & YSIG__K_VIEW) fl |= YSIG__F_FRAME | YSIG__F_VIEW;
        if (s1 == -2) fl |= YSIG__F_MULTI;
        if (s1 == -1 && rule != YSIG__R_SOURCE) fl |= YSIG__F_NOSRC;
        /* these keep state from sample to sample: with no source upstream
         * they would never run (a frame-rate form is not built) */
        if (s1 == -1 && (op == YSIG_OP_INTEGRATE || op == YSIG_OP_LOWPASS || op == YSIG_OP_VELOCITY ||
                         op == YSIG_OP_SPEED || op == YSIG_OP_DELAY || op == YSIG_OP_PEAK || op == YSIG_OP_SILENT))
            return ysig__fail(err, es, "ysig: node %d (%s): %s keeps state per sample and has no source upstream", id,
                              ysig__nm(nd), ysig__ops[op].name);
        if (op == YSIG_OP_MEAN) {
            double ws = 0;
            for (j = 0; j < a->n_in[id]; j++) ws += nd->p[j] != 0 ? nd->p[j] : 1;
            if (ws == 0) return ysig__fail(err, es, "ysig: node %d (%s): mean's weights sum to 0", id, ysig__nm(nd));
        }
        a->lanes[id] = (uint8_t)lanes;
        a->flags[id] = fl;
        a->src1[id] = s1;
        a->hist[id] = nd->history;
    }
    /* history: a stateless node after two sources is computed from its
     * inputs at the time asked, so they keep the rings */
    for (k = total - 1; k >= 0; k--) {
        int id = a->order[k], op = a->node[id]->op;
        if (!a->hist[id]) continue;
        /* also one source and a generator (or another frame-updated
         * input): its ring would hold sample times only and miss the
         * generator's frames */
        /* silent too: its past value comes from when its input arrived */
        if ((a->flags[id] & (YSIG__F_MULTI | YSIG__F_FRAME)) && op < YSIG_OP_COUNT &&
            ((ysig__ops[op].kind & YSIG__K_PURE) || op == YSIG_OP_SILENT)) {
            a->flags[id] |= YSIG__F_AT;
            for (j = 0; j < a->n_in[id]; j++) {
                int u = a->in[id][j];
                if (a->hist[u] < a->hist[id]) a->hist[u] = a->hist[id];
            }
            a->hist[id] = 0;
        }
    }
    ysig__sched(a, NULL, NULL, NULL, NULL);
    /* the promise: a graph the check takes has an arena size */
    if (!ysig__layout(d, a, NULL, NULL))
        return ysig__fail(err, es, "ysig: the graph needs more memory than size_t can count (history rings)");
    (void)i;
    return true;
}

/* --- layout ------------------------------------------------------------------ */

/* A block at the next 16 bytes; on a size_t wrap (32-bit hosts and big
 * rings) *off becomes SIZE_MAX and stays there. */
static size_t ysig__al(size_t* off, size_t size) {
    size_t at;
    if (*off > SIZE_MAX - 15u) { *off = SIZE_MAX; return 0; }
    at = (*off + 15u) & ~(size_t)15u;
    if (size > SIZE_MAX - at) { *off = SIZE_MAX; return 0; }
    *off = at + size;
    return at;
}

static int ysig__ring_cap(const ysig_node* nd) {
    int c = (int)nd->p[1];
    if (nd->op == YSIG_OP_DELAY) return c ? c : 1024;
    return c ? c : 256;
}

static size_t ysig__state_size(const ysig_desc* d, const ysig_node* nd) {
    const ysig_op_vtable* vt;
    switch (nd->op) {
    case YSIG_OP_SOURCE: return sizeof(ysig__src_st);
    case YSIG_OP_INTEGRATE: return sizeof(ysig__int_st);
    case YSIG_OP_LOWPASS: return sizeof(ysig__lp_st);
    case YSIG_OP_VELOCITY: case YSIG_OP_SPEED: case YSIG_OP_DELAY:
        return sizeof(ysig__ring) + 16 + (size_t)ysig__ring_cap(nd) * sizeof(ysig_value);
    case YSIG_OP_DETECT: return sizeof(ysig__det_st);
    case YSIG_OP_NOISE: return sizeof(ysig__noise_st);
    case YSIG_OP_LATCH: return sizeof(ysig__latch_st);
    case YSIG_OP_PEAK: return sizeof(ysig__peak_st);
    case YSIG_OP_SILENT: return sizeof(ysig__silent_st);
    default:
        vt = nd->op >= YSIG_OP_CUSTOM ? ysig__vt(d, nd->op) : NULL;
        return vt ? vt->state_size : 0;
    }
}

/* Every block of the arena, measured (g NULL) or placed. */
static size_t ysig__layout(const ysig_desc* d, const ysig__an* a, ysig_graph* g, char* base) {
    size_t off = 0, at;
    int i, n = a->n;
    at = ysig__al(&off, sizeof(ysig__nd) * (size_t)n);
    if (g) g->nd = (ysig__nd*)(void*)(base + at);
    at = ysig__al(&off, sizeof(ysig_value) * (size_t)n);
    if (g) g->val = (ysig_value*)(void*)(base + at);
    at = ysig__al(&off, sizeof(int16_t) * (size_t)n);
    if (g) g->order = (int16_t*)(void*)(base + at);
    at = ysig__al(&off, sizeof(int16_t) * (size_t)(a->n_src + 1));
    if (g) g->src_ids = (int16_t*)(void*)(base + at);
    at = ysig__al(&off, sizeof(int32_t) * (size_t)(a->n_src + 1));
    if (g) g->src_off = (int32_t*)(void*)(base + at);
    at = ysig__al(&off, sizeof(int16_t) * (size_t)(a->sched_total + 1));
    if (g) g->sched = (int16_t*)(void*)(base + at);
    at = ysig__al(&off, sizeof(int16_t) * (size_t)(a->n_frame + 1));
    if (g) g->frame = (int16_t*)(void*)(base + at);
    /* a run again can queue the window's new hits at once (the caller
     * drains after the arm): room for C more with a window */
    at = ysig__al(&off, sizeof(ysig_hit) * (size_t)(YSIG_QUEUE_CAP + (d->catchup_frames > 0 ? (d->catchup_calls ? d->catchup_calls : 1024) : 0)));
    if (g) g->hitq = (ysig_hit*)(void*)(base + at);
    at = ysig__al(&off, sizeof(yin_event) * (size_t)(YSIG_QUEUE_CAP + (d->catchup_frames > 0 ? (d->catchup_calls ? d->catchup_calls : 1024) : 0)));
    if (g) g->derq = (yin_event*)(void*)(base + at);
    /* v0.6.0: the downstream sets (footprints) and the waiting arms */
    at = ysig__al(&off, sizeof(uint64_t) * (size_t)n * (size_t)((n + 63) / 64));
    if (g) { g->down = (uint64_t*)(void*)(base + at); g->dw = (n + 63) / 64; }
    at = ysig__al(&off, sizeof(ysig__wt));
    if (g) g->wt = (ysig__wt*)(void*)(base + at);
    at = ysig__al(&off, sizeof(uint64_t) * (size_t)YSIG__UFP * (size_t)((n + 63) / 64));
    if (g) {
        uint64_t* q = (uint64_t*)(void*)(base + at);
        int dw = (n + 63) / 64, j;
        for (j = 0; j < YSIG__UX; j++, q += dw) g->wt->lr[j].F = q;
        for (j = 0; j < YSIG__UCG; j++, q += 2 * dw) { g->wt->cg[j].F = q; g->wt->cg[j].Fa = q + dw; }
    }
    for (i = 0; i < n; i++) {
        const ysig_node* nd = a->node[i];
        size_t s = ysig__state_size(d, nd);
        if (s) {
            at = ysig__al(&off, s);
            if (g) g->nd[i].st = base + at;
        }
        if (nd->n_data) {
            at = ysig__al(&off, sizeof(double) * (size_t)nd->n_data);
            if (g) g->nd[i].data = (double*)(void*)(base + at);
        }
        if (a->hist[i]) {
            at = ysig__al(&off, sizeof(ysig_value) * (size_t)a->hist[i]);
            if (g) {
                g->nd[i].hist.buf = (ysig_value*)(void*)(base + at);
                g->nd[i].hist.cap = a->hist[i];
            }
        }
    }
    if (d->catchup_frames > 0) {
        /* v0.6.0: the window of an arm in the past (AN ARM IN THE PAST) */
        int K = d->catchup_frames, C = d->catchup_calls ? d->catchup_calls : 1024;
        int W = d->catchup_writes ? d->catchup_writes : 8192, nfv = 0, k;
        size_t img = 0;
        ysig__cu* cu = NULL;
        int DC = 0;
#define YSIG__A8(x) (((size_t)(x) + 7u) & ~(size_t)7u)
        img += YSIG__A8(sizeof(ysig_graph)) + YSIG__A8(sizeof(ysig__nd) * (size_t)n) + YSIG__A8(sizeof(ysig_value) * (size_t)n);
        for (i = 0; i < n; i++) {
            const ysig_node* nd = a->node[i];
            int ring = nd->op == YSIG_OP_VELOCITY || nd->op == YSIG_OP_SPEED || nd->op == YSIG_OP_DELAY;
            img += YSIG__A8(ring ? sizeof(ysig__ring) : ysig__state_size(d, nd)) + YSIG__A8(sizeof(double) * (size_t)nd->n_data);
            if (nd->op == YSIG_OP_CHANNEL && (int)nd->p[0] + 1 > nfv) nfv = (int)nd->p[0] + 1;
            /* a call makes at most one event an emit: the derived log does
             * not run full before the call log (its full is the window's) */
            if (nd->op == YSIG_OP_EMIT && !d->derived_final) DC += C;
        }
        /* v0.7.0: no queue in an image: a run again keeps the live queue's
         * entries made before its checkpoint and makes the rest again */
#undef YSIG__A8
        at = ysig__al(&off, sizeof(ysig__cu));
        if (g) {
            cu = (ysig__cu*)(void*)(base + at);
            g->cu = cu;
            cu->K = K;
            cu->C = C;
            cu->W = W;
            cu->nfv = nfv;
            cu->nslot = K + 1;
            cu->img_size = img;
            cu->DC = DC;
        }
        at = ysig__al(&off, sizeof(ysig__ck) * (size_t)K);
        if (cu) cu->ck = (ysig__ck*)(void*)(base + at);
        for (k = 0; k < K; k++) {
            at = ysig__al(&off, img);
            if (cu) cu->ck[k].img = base + at;
        }
        at = ysig__al(&off, sizeof(ysig__call) * (size_t)C);
        if (cu) cu->calls = (ysig__call*)(void*)(base + at);
        at = ysig__al(&off, sizeof(ysig__undo) * (size_t)W);
        if (cu) cu->undo = (ysig__undo*)(void*)(base + at);
        at = ysig__al(&off, sizeof(ysig__hl) * (size_t)C);
        if (cu) cu->hl = (ysig__hl*)(void*)(base + at);
        at = ysig__al(&off, sizeof(ysig__hl) * (size_t)C);
        if (cu) cu->tmp = (ysig__hl*)(void*)(base + at);
        if (nfv) {
            at = ysig__al(&off, sizeof(float) * (size_t)(K + 1) * (size_t)nfv);
            if (cu) cu->pool = (float*)(void*)(base + at);
        }
        at = ysig__al(&off, sizeof(uint64_t) * (size_t)(YSIG_QUEUE_CAP + C));
        if (cu) cu->qseq = (uint64_t*)(void*)(base + at);
        at = ysig__al(&off, sizeof(uint64_t) * (size_t)(YSIG_QUEUE_CAP + C));
        if (cu) cu->qhl = (uint64_t*)(void*)(base + at);
        at = ysig__al(&off, sizeof(uint64_t) * (size_t)(YSIG_QUEUE_CAP + C));
        if (cu) cu->dseq = (uint64_t*)(void*)(base + at);
        at = ysig__al(&off, sizeof(uint64_t) * (size_t)(YSIG_QUEUE_CAP + C));
        if (cu) cu->dqdl = (uint64_t*)(void*)(base + at);
        if (DC) {
            at = ysig__al(&off, sizeof(ysig__dl) * (size_t)DC);
            if (cu) cu->dl = (ysig__dl*)(void*)(base + at);
            at = ysig__al(&off, sizeof(ysig__dl) * (size_t)DC);
            if (cu) cu->tmpd = (ysig__dl*)(void*)(base + at);
        }
        at = ysig__al(&off, sizeof(ysig__call) * YSIG__BLK);
        if (cu) cu->blk = (ysig__call*)(void*)(base + at);
        at = ysig__al(&off, sizeof(ysig__call) * (size_t)C);
        if (cu) {
            cu->mv = (ysig__call*)(void*)(base + at);
            cu->span_ns = d->catchup_ns ? d->catchup_ns : 50000000;
            cu->def_k = -1;
        }
        {
            /* the open groups of exactness (2 (b)): each has its L in the
             * log, so a group per logged call at most */
            int dw = (n + 63) / 64, j;
            size_t at2 = ysig__al(&off, sizeof(ysig__ux) * (size_t)C);
            at = ysig__al(&off, sizeof(uint64_t) * (size_t)C * (size_t)dw);
            if (cu) {
                g->wt->op = (ysig__ux*)(void*)(base + at2);
                g->wt->cap_op = C;
                for (j = 0; j < C; j++) g->wt->op[j].F = (uint64_t*)(void*)(base + at) + (size_t)j * (size_t)dw;
            }
        }
        if (d->rows_final || d->derived_final) {
            int R = d->catchup_rows ? d->catchup_rows : 4 * C;
            if (cu) { cu->R = R; cu->rows_final = d->rows_final; cu->derived_final = d->derived_final; }
            if (d->rows_final) {
                at = ysig__al(&off, sizeof(ysig__rbuf) * (size_t)R);
                if (cu) cu->rb = (ysig__rbuf*)(void*)(base + at);
            }
            if (d->derived_final) {
                at = ysig__al(&off, sizeof(ysig__dbuf) * (size_t)R);
                if (cu) cu->db = (ysig__dbuf*)(void*)(base + at);
            }
        }
    }
    return off > SIZE_MAX - 16 ? 0 : off + 16;   /* 0: more than size_t holds */
}

/* The analysis is about 14 KB on the stack: no allocation, no static. */
YSIG_API bool ysig_check(const ysig_desc* d, char* err, size_t err_size) {
    ysig__an a;
    if (err && err_size) err[0] = 0;
    return ysig__analyze(d, &a, err, err_size);
}

YSIG_API size_t ysig_arena_size(const ysig_desc* d) {
    ysig__an a;
    if (!ysig__analyze(d, &a, NULL, 0)) return 0;
    return ysig__layout(d, &a, NULL, NULL);
}

YSIG_API const char* ysig_error(const ysig_graph* g) { return g ? g->err : "ysig: no graph"; }

/* --- evaluation -------------------------------------------------------------- */

/* The digest a 64-bit word at a time (a byte at a time was 18% of the
 * bench's time; docs/sig.md, "Cost"), each step through splitmix64's
 * finalizer: a plain xor-multiply carries a flipped sign bit to bit 63
 * only, so two sign flips (or -0 for +0) cancelled. Words, not bytes: the
 * same digest on either byte order. */
static void ysig__digest_word(ysig_graph* g, uint64_t w) {
    uint64_t z = (g->st.digest ^ w) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    g->st.digest = z ^ (z >> 31);
}
static void ysig__digest_value(ysig_graph* g, const ysig_value* v) {
    int k;
    ysig__digest_word(g, (uint64_t)v->t);
    ysig__digest_word(g, (uint64_t)v->t_oldest);
    ysig__digest_word(g, (uint64_t)v->tier | ((uint64_t)v->lanes << 8));
    for (k = 0; k < v->lanes && k < 4; k++) {
        uint64_t w;
        memcpy(&w, &v->v[k], sizeof w);
        ysig__digest_word(g, w);
    }
}

static int64_t ysig__ns(double s) { return (int64_t)llround(s * 1e9); }

/* Times near INT64_MIN or INT64_MAX: a difference or a sum saturates
 * instead of overflowing (undefined in C). */
static int64_t ysig__sub(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b) return INT64_MAX;
    if (b > 0 && a < INT64_MIN + b) return INT64_MIN;
    return a - b;
}
static int64_t ysig__add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
    return a + b;
}

/* Derived constants from p[]: after open and after ysig_set(). */
static void ysig__prepare(ysig_graph* g, int i) {
    ysig__nd* n = &g->nd[i];
    double a;
    switch (n->op) {
    case YSIG_OP_AFFINE: {
        double m[4], c, s;
        int k;
        for (k = 0; k < 4; k++) m[k] = n->p[k];
        if (m[0] == 0 && m[1] == 0 && m[2] == 0 && m[3] == 0) { m[0] = 1; m[3] = 1; }
        a = n->p[8];
        if (a != 0) {
            double r = a * YSIG__PI / 180;
            c = cos(r);
            s = sin(r);
            n->k[0] = c * m[0] - s * m[2];
            n->k[1] = c * m[1] - s * m[3];
            n->k[2] = s * m[0] + c * m[2];
            n->k[3] = s * m[1] + c * m[3];
        } else {
            for (k = 0; k < 4; k++) n->k[k] = m[k];
        }
        for (k = 0; k < 4; k++) n->k[4 + k] = m[k];   /* before the angle, for an angle input */
        break;
    }
    case YSIG_OP_VELOCITY: case YSIG_OP_SPEED: n->kns[0] = ysig__ns(n->p[0]); break;
    case YSIG_OP_DELAY: n->kns[0] = ysig__ns(n->p[0]); break;
    case YSIG_OP_DETECT: n->kns[0] = ysig__ns(n->p[4]); break;
    case YSIG_OP_SILENT: n->kns[0] = ysig__ns(n->p[0]); break;
    default: break;
    }
}

static void ysig__undo_note(ysig_graph* g, ysig_value* at);
static void ysig__ring_push(ysig_graph* g, ysig__ring* r, const ysig_value* v) {
    int at;
    /* never true (ysig__ring_cap gives 256 or 1024 for 0), but MSVC can
     * prove n == cap == 0 on an inlined path and calls the mod an error */
    if (r->cap <= 0) return;
    at = (r->head + r->n) % r->cap;
    if (r->n == r->cap) { r->head = (r->head + 1) % r->cap; at = (r->head + r->n - 1) % r->cap; }
    else r->n++;
    ysig__undo_note(g, &r->buf[at]);
    r->buf[at] = *v;
}
static const ysig_value* ysig__ring_at(const ysig__ring* r, int k) { return &r->buf[(r->head + k) % r->cap]; }

/* The newest entry with t <= limit (rings in time order), or -1. */
static int ysig__ring_find(const ysig__ring* r, int64_t limit) {
    int lo = 0, hi = r->n - 1, best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (ysig__ring_at(r, mid)->t <= limit) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

static void ysig__hist_push(ysig_graph* g, ysig__nd* n, const ysig_value* v) {
    ysig__ring* r = &n->hist;
    if (!r->cap) return;
    if (r->n) {
        ysig_value* last = &r->buf[(r->head + r->n - 1) % r->cap];
        if (v->t < last->t) { g->st.history_skipped++; return; }
        if (v->t == last->t) { ysig__undo_note(g, last); *last = *v; return; }
    }
    ysig__ring_push(g, r, v);
}

/* A value whose t may go back (a peak's, on a late report): the ring keeps
 * time order on the later of its t and the ring's last. */
static void ysig__hist_push_later(ysig_graph* g, ysig__nd* n, const ysig_value* v) {
    ysig__ring* r = &n->hist;
    ysig_value c;
    if (!r->cap) return;
    c = *v;
    if (r->n) {
        int64_t last = r->buf[(r->head + r->n - 1) % r->cap].t;
        if (c.t < last) c.t = last;
    }
    ysig__hist_push(g, n, &c);
}

/* The generic part of a value from its inputs. t, source, device and
 * unc_us come from the input that changed in this evaluation (bit j of
 * `changed`), the newest of them when several did: the report that made
 * the value, also when another source's held part is newer in time, and
 * after a delay the delayed sample's. With none changed (a value at a
 * past time), from the newest input. t_oldest is the oldest input's; the
 * tier the worst. */
static void ysig__meta(const ysig__nd* n, const ysig_value* const* in, unsigned changed, ysig_value* o) {
    int j, pick = -1;
    if (!n->n_in) return;
    o->t_oldest = in[0]->t_oldest;
    o->src_oldest = in[0]->src_oldest;
    o->tier = in[0]->tier;
    for (j = 0; j < n->n_in; j++) {
        if (j) {
            if (in[j]->t_oldest < o->t_oldest) { o->t_oldest = in[j]->t_oldest; o->src_oldest = in[j]->src_oldest; }
            o->tier = ysig_tier_worst(o->tier, in[j]->tier);
        }
        if (changed && !((changed >> j) & 1u)) continue;
        if (pick < 0 || in[j]->t > in[pick]->t) pick = j;
    }
    o->t = in[pick]->t;
    o->src = in[pick]->src;
    o->device = in[pick]->device;
    o->unc_us = in[pick]->unc_us;
}

/* r = sum of the nonzero terms m[c] * d[c], after post when nonzero: an
 * exact zero adds nothing, so a mirror or a selection stays exact, the
 * sign of zero included. */
static double ysig__row(double post, const double* m, const double* dd) {
    double acc = 0;
    int has = 0, c;
    if (post != 0) { acc = post; has = 1; }
    for (c = 0; c < 2; c++)
        if (m[c] != 0) {
            double t = m[c] * dd[c];
            acc = has ? acc + t : t;
            has = 1;
        }
    return has ? acc : 0.0;
}

/* "Nonzero" for a selector, trigger or gate; NaN is not (a missing value
 * must not flip a selection or arm a latch). */
static int ysig__truth(double v) { return v > 0 || v < 0; }

/* The stateless ops: out from in, nothing else read. */
static void ysig__pure(const ysig__nd* n, const ysig_value* const* in, ysig_value* o) {
    int k, j, L = n->lanes;
    const double* x = in[0]->v;
    switch (n->op) {
    case YSIG_OP_ROLE:
        for (k = 0; k < L; k++) o->v[k] = x[k];
        break;
    case YSIG_OP_DEADZONE: {
        double r2 = 0, r, R = n->p[0];
        for (k = 0; k < L; k++) r2 += x[k] * x[k];
        r = sqrt(r2);
        for (k = 0; k < L; k++) o->v[k] = r <= R ? 0.0 : x[k] * ((r - R) / r);
        break;
    }
    case YSIG_OP_AFFINE: {
        double dd[2], m[4];
        const double* mm = n->k;
        dd[0] = x[0] - n->p[4];
        dd[1] = x[1] - n->p[5];
        if (n->n_in > 1) {
            double a = n->p[8] + in[1]->v[0];
            if (a != 0) {
                double r = a * YSIG__PI / 180, c = cos(r), s = sin(r);
                const double* b = n->k + 4;
                m[0] = c * b[0] - s * b[2];
                m[1] = c * b[1] - s * b[3];
                m[2] = s * b[0] + c * b[2];
                m[3] = s * b[1] + c * b[3];
            } else {
                for (k = 0; k < 4; k++) m[k] = n->k[4 + k];
            }
            mm = m;
        }
        o->v[0] = ysig__row(n->p[6], mm, dd);
        o->v[1] = ysig__row(n->p[7], mm + 2, dd);
        break;
    }
    case YSIG_OP_SCALE:
        for (k = 0; k < L; k++) {
            double s = n->p[k] != 0 ? n->p[k] : 1;
            o->v[k] = s == 1 ? x[k] : x[k] * s;
            if (n->n_in > 1) o->v[k] = o->v[k] * in[1]->v[0];
        }
        break;
    case YSIG_OP_OFFSET:
        for (k = 0; k < L; k++) o->v[k] = x[k] + n->p[k];
        break;
    case YSIG_OP_CLAMP:
        for (k = 0; k < L; k++) {
            double v = x[k], lo = n->p[k], hi = n->p[4 + k];
            if (lo != 0 || hi != 0) v = v < lo ? lo : v > hi ? hi : v;
            o->v[k] = v;
        }
        break;
    case YSIG_OP_SELECT:
        for (k = 0; k < L; k++) o->v[k] = x[(int)n->p[k]];
        break;
    case YSIG_OP_JOIN: {
        int at = 0;
        for (j = 0; j < n->n_in; j++)
            for (k = 0; k < in[j]->lanes; k++) o->v[at++] = in[j]->v[k];
        break;
    }
    case YSIG_OP_SUM:
    case YSIG_OP_MEAN: {
        double ws = 0;
        for (k = 0; k < L; k++) {
            double s = 0;
            for (j = 0; j < n->n_in; j++) {
                double w = n->p[j] != 0 ? n->p[j] : 1;
                s = j == 0 ? (w == 1 ? in[j]->v[k] : w * in[j]->v[k]) : s + (w == 1 ? in[j]->v[k] : w * in[j]->v[k]);
            }
            o->v[k] = s;
        }
        if (n->op == YSIG_OP_MEAN) {
            for (j = 0; j < n->n_in; j++) ws += n->p[j] != 0 ? n->p[j] : 1;
            for (k = 0; k < L; k++) o->v[k] = o->v[k] / ws;
        }
        break;
    }
    case YSIG_OP_DIFF:
        for (k = 0; k < L; k++) o->v[k] = x[k] - in[1]->v[k];
        break;
    case YSIG_OP_MUX: {
        /* the chosen input's value, time and sources: a sample of the
         * other input changes nothing here */
        const ysig_value* c = ysig__truth(in[2]->v[0]) ? in[0] : in[1];
        for (k = 0; k < L; k++) o->v[k] = c->v[k];
        o->t = c->t;
        o->t_oldest = c->t_oldest;
        o->device = c->device;
        o->src = c->src;
        o->src_oldest = c->src_oldest;
        o->tier = c->tier;
        o->unc_us = c->unc_us;
        break;
    }
    case YSIG_OP_DISTANCE: {
        const double* q = n->n_in > 1 ? in[1]->v : n->p;
        double s2 = 0, dd;
        for (k = 0; k < in[0]->lanes; k++) { dd = x[k] - q[k]; s2 += dd * dd; }
        o->v[0] = sqrt(s2);
        break;
    }
    case YSIG_OP_ALL: {
        int ok = 1;
        for (j = 0; j < n->n_in; j++) ok &= ysig__truth(in[j]->v[0]);
        o->v[0] = ok ? 1.0 : 0.0;
        break;
    }
    default: break;
    }
}

static void ysig__hit(ysig_graph* g, int node, int64_t t, int64_t t_found, const ysig_value* v) {
    ysig_hit* h;
    g->st.hits++;
    ysig__digest_word(g, (uint64_t)(uint32_t)node);
    ysig__digest_word(g, (uint64_t)t);
    ysig__digest_value(g, v);
    if (g->cu && g->cu->replaying) {
        ysig_hit x;
        memset(&x, 0, sizeof x);
        x.node = node;
        x.t = t;
        x.t_found = t_found;
        x.value = *v;
        /* handed out before the arm: logged as handed out (seq 0) */
        if (!(g->cu->arm_live && node == g->cu->arm_node)) {
            /* logged as the caller has it (its t_found: where a later arm
             * goes is after the call that found it) */
            const ysig__hl* seen = ysig__hit_seen(g->cu, &x);
            if (seen) { ysig__hl_add(g->cu, 0, &seen->h); return; }
        }
    }
    /* 64 queued, and the window's room (finding 6: the caller drains after
     * the arm; v0.7.0: a caller holds hits past its horizon) */
    if (g->n_hit >= YSIG__QCAP(g)) { g->st.hits_lost++; return; }
    h = &g->hitq[g->n_hit++];
    memset(h, 0, sizeof *h);
    h->node = node;
    h->t = t;
    h->t_found = t_found;
    h->value = *v;
    if (g->cu) {
        g->cu->qseq[g->n_hit - 1] = ++g->cu->next_seq;
        g->cu->qhl[g->n_hit - 1] = g->cu->h_head;
        ysig__hl_add(g->cu, g->cu->next_seq, h);
    }
}

/* A 64-bit mix for the window's hashes of rows and derived events. */
static uint64_t ysig__hmix(uint64_t h, uint64_t w) {
    h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}
static uint64_t ysig__mix_value(uint64_t h, const ysig_value* v) {
    int k;
    /* every column a row or a derived event carries, not the lanes only */
    h = ysig__hmix(h, (uint64_t)v->t);
    h = ysig__hmix(h, (uint64_t)v->t_oldest);
    h = ysig__hmix(h, (uint64_t)v->device | ((uint64_t)v->src << 32) | ((uint64_t)v->src_oldest << 48));
    h = ysig__hmix(h, (uint64_t)v->tier | ((uint64_t)v->lanes << 8) | ((uint64_t)v->unc_us << 16));
    for (k = 0; k < YSIG_LANES; k++) {
        uint64_t b;
        memcpy(&b, &v->v[k], sizeof b);
        h = ysig__hmix(h, b);
    }
    return h;
}

/* The detect's test on one value; with hyst, the test moved back by the
 * hysteresis (for "ready again"). */
static int ysig__test(const ysig__nd* n, const ysig_value* v, double hyst) {
    int test = (int)n->p[0], lane = (int)n->p[1];
    double q, x = v->v[0], y = v->v[1];
    if (test == YSIG_TEST_IN_RECT || test == YSIG_TEST_OUT_RECT) {
        int in = x >= n->p[8] + hyst && x <= n->p[10] - hyst && y >= n->p[9] + hyst && y <= n->p[11] - hyst;
        int out = x < n->p[8] - hyst || x > n->p[10] + hyst || y < n->p[9] - hyst || y > n->p[11] + hyst;
        return test == YSIG_TEST_IN_RECT ? in : out;
    }
    if (lane >= 0) q = v->v[lane];
    else {
        double s = 0, dd;
        int k;
        for (k = 0; k < v->lanes; k++) { dd = v->v[k] - n->p[8 + k]; s += dd * dd; }
        q = sqrt(s);
    }
    return test == YSIG_TEST_GE ? q >= n->p[2] + hyst : q <= n->p[2] - hyst;
}

/* Returns 1 when the arm itself is a hit (entry now). v: the input's
 * value at the arm's time. */
static int ysig__det_arm(ysig_graph* g, int i, int64_t t, const ysig_value* v) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    int r = ysig__test(n, v, 0);
    s->armed = 1;
    s->t_arm = t;
    s->state = r;
    s->fired = 0;
    if ((int)n->p[13] == YSIG_OUT_HIT) g->val[i].v[0] = 0.0;
    if (n->kns[0] > 0 && n->p[12] != 1) {
        /* a hold counts from the arm when the test is true then (entry
         * arm, the default: a hand on the start at a trial's start; entry
         * now is the same with a hold) */
        s->ready = 1;
        s->holding = r;
        s->t_start = t;
        s->v_start = *v;
    } else if (n->kns[0] > 0) {
        /* entry fresh: true at the arm waits for a fresh entry */
        s->ready = !r;
        s->holding = 0;
    } else if (n->p[12] == 2) {
        /* entry now (D35 (f)): a test true at the arm is a hit then, as a
         * cursor already in the target starts tracking. A frame-rate
         * detect armed between frames tests at the next onset instead,
         * since its hits are at onsets */
        s->ready = 1;
        s->holding = 0;
        /* an input with no sample since its reset (tier UNKNOWN) has no
         * held value to test: the first value after the arm is tested,
         * ready, as a crossing (a hit if it meets the test) */
        if (r && n->p[5] != 1 && v->tier != YIN_TIER_UNKNOWN) {
            ysig__hit(g, i, t, t, v);
            s->fired = 1;
            s->t_hit = t;
            s->v_hit = *v;
            s->ready = 0;
            if (n->p[7] == 0) s->armed = 0;
            return 1;
        }
    } else {
        /* past the test at arm: held until it re-arms (CROSSINGS) */
        s->ready = !r;
        s->holding = 0;
    }
    return 0;
}

/* A hold met by time `t`: the held value was true from t_start on. */
static int ysig__det_hold_done(ysig_graph* g, int i, int64_t t) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    if (!s->holding || t < ysig__add(s->t_start, n->kns[0])) return 0;
    ysig__hit(g, i, ysig__add(s->t_start, n->kns[0]), t, &s->v_start);
    s->fired = 1;
    s->t_hit = ysig__add(s->t_start, n->kns[0]);
    s->v_hit = s->v_start;
    s->holding = 0;
    s->ready = 0;
    if (n->p[7] == 0) { s->armed = 0; return 1; }
    return 0;
}

/* A detect output's sources: those of the input value given. */
static void ysig__det_prov(ysig_value* o, const ysig_value* v) {
    o->t_oldest = v->t_oldest;
    o->device = v->device;
    o->src = v->src;
    o->src_oldest = v->src_oldest;
    o->tier = v->tier;
    o->unc_us = v->unc_us;
}

/* A detect's output after a value or a frame: its test, or whether a hit
 * stands. rise: a hit output's rise in this evaluation. */
static void ysig__det_out(ysig_graph* g, int i, const ysig_value* v, int64_t tc, int rise) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    ysig_value* o = &g->val[i];
    int hit_out = (int)n->p[13] == YSIG_OUT_HIT;
    o->v[0] = (hit_out ? s->fired : s->state) ? 1.0 : 0.0;
    /* the rise is at the hit's time, with the sources of the input that
     * met the test, however the hit was found (a sample, a frame, a
     * settle, an arm): a past read after the hit says 1. A later
     * evaluation of a standing hit carries its own time and input, as
     * any value does, so a hold downstream completes from samples and
     * the ring takes the entry */
    o->t = rise ? s->t_hit : tc;
    ysig__det_prov(o, rise ? &s->v_hit : v);
}

/* One value for a detect: t is its time (a hit's time), tc the detect's
 * own clock, which never goes back (ysig__sink_t): a hold starts and ends
 * on tc. */
static void ysig__det_eval(ysig_graph* g, int i, const ysig_value* v, int64_t t, int64_t tc) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    int r = ysig__test(n, v, 0);
    s->state = r;
    if (!s->armed) return;
    if (t < s->t_arm) {
        /* before the arm: no hit and no start, but a failing value breaks
         * a hold whatever its t (a late report of a hand that left), and
         * makes any detect ready again, as it would before the arm (O3: a
         * fresh entry's failure) */
        if (!r) {
            s->holding = 0;
            if (!ysig__test(n, v, -n->p[3])) { s->ready = 1; s->fired = 0; }
        }
        return;
    }
    if (n->kns[0] > 0) {
        if (ysig__det_hold_done(g, i, tc)) return;
        if (r) {
            if (!s->holding && s->ready) {
                s->holding = 1;
                s->t_start = tc;
                s->v_start = *v;
            }
        } else {
            s->holding = 0;
            if (!ysig__test(n, v, -n->p[3])) { s->ready = 1; s->fired = 0; }
        }
        return;
    }
    if (r && s->ready) {
        ysig__hit(g, i, t, t, v);
        s->fired = 1;
        s->t_hit = t;
        s->v_hit = *v;
        s->ready = 0;
        if (n->p[7] == 0) s->armed = 0;
    } else if (!s->ready && !ysig__test(n, v, -n->p[3])) {
        /* ready again once the test fails with the level moved back */
        s->ready = 1;
        s->fired = 0;
    }
}

static void ysig__det_value(ysig_graph* g, int i, const ysig_value* v, int64_t t, int64_t tc) {
    ysig__det_st* s = (ysig__det_st*)g->nd[i].st;
    int was = s->fired;
    ysig__det_eval(g, i, v, t, tc);
    ysig__det_out(g, i, v, tc, s->fired && !was && (int)g->nd[i].p[13] == YSIG_OUT_HIT);
}

/* A sample-rate detect at a frame's onset: a still hand completes its hold. */
static void ysig__det_frame(ysig_graph* g, int i, int64_t onset, const ysig_value* v) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    int was = s->fired;
    if (!s->armed || n->kns[0] <= 0 || onset < s->t_arm) return;
    (void)ysig__det_hold_done(g, i, onset);
    if ((int)n->p[13] == YSIG_OUT_HIT && s->fired && !was) ysig__det_out(g, i, v, onset, 1);
}

/* A detect's hold clock never goes back: a value whose t went back (a
 * report from another source that arrived late) keeps the detect's last
 * time, counted. Emit and record carry the true t (ysp/input.h: times need
 * not rise in arrival order). */
static int64_t ysig__sink_t(ysig_graph* g, ysig__nd* n, int64_t t) {
    if (t < n->sink_t) { g->st.time_clamped++; return n->sink_t; }
    n->sink_t = t;
    return t;
}

/* v0.6.0, final-only rows and derived events: held with the call that
 * made them until no arm can change them, that is until that call is
 * older than the oldest the window can run again. */
static uint64_t ysig__final_limit(const ysig__cu* cu) {
    return cu->n_ck ? cu->ck[cu->ck0].call : UINT64_MAX;
}
static void ysig__late_run(ysig_graph* g);
static void ysig__out_final(ysig_graph* g) {
    ysig__cu* cu = g->cu;
    uint64_t lim;
    /* every API call ends here: the late calls it placed run again once */
    if (cu && cu->def_k >= 0 && !cu->defer && !g->busy) ysig__late_run(g);
    if (!cu || (!cu->rows_final && !cu->derived_final)) return;
    lim = ysig__final_limit(cu);
    while (cu->rb && cu->rb_tail < cu->rb_head && cu->rb[cu->rb_tail % (uint64_t)cu->R].idx < lim) {
        ysig_row r = cu->rb[cu->rb_tail++ % (uint64_t)cu->R].r;
        if (g->on_row) { g->busy++; g->on_row(g->user, &r); g->busy--; }
    }
    /* final events go to the queue as it has room; the rest wait, final,
     * for the next ysig_derived() (H8: a barrier can make hundreds final) */
    while (cu->db && cu->db_tail < cu->db_head && cu->db[cu->db_tail % (uint64_t)cu->R].idx < lim && g->n_der < YSIG__QCAP(g))
        g->derq[g->n_der++] = cu->db[cu->db_tail++ % (uint64_t)cu->R].e;
}
/* A full buffer: the checkpoints at or before its oldest entry's call
 * go, so that entry is final (a shorter window, still exact). */
static void ysig__buf_room(ysig_graph* g, uint64_t idx0) {
    ysig__cu* cu = g->cu;
    while (cu->n_ck && cu->ck[cu->ck0].call <= idx0) {
        /* a log ran full: an arm whose place this takes falls back as
         * after the other logs (arm_past_full, not far: H10) */
        cu->full = 1;
        cu->ck0 = (cu->ck0 + 1) % cu->K;
        cu->n_ck--;
        if (cu->n_ck) {
            const ysig__ck* k = &cu->ck[cu->ck0];
            cu->c_tail = k->call;
            cu->u_tail = k->undo;
            cu->h_tail = k->hit;
            cu->d_tail = k->der;
        } else {
            ysig__cu_clear(cu, cu->replaying ? cu->c_pos : cu->c_head);
        }
    }
    ysig__out_final(g);
}
static uint64_t ysig__buf_idx(const ysig__cu* cu) {
    if (cu->replaying) return cu->c_pos - 1;
    /* a call the log did not keep: final once what is before it is */
    return cu->cur != UINT64_MAX ? cu->cur : 0;
}
static void ysig__rb_push(ysig_graph* g, const ysig_row* r) {
    ysig__cu* cu = g->cu;
    ysig__rbuf* b;
    if (!cu->rb) return;
    if (cu->rb_head - cu->rb_tail >= (uint64_t)cu->R) ysig__buf_room(g, cu->rb[cu->rb_tail % (uint64_t)cu->R].idx);
    if (cu->rb_head - cu->rb_tail >= (uint64_t)cu->R) {
        /* every checkpoint went, and on_row took the rest: at once */
        if (g->on_row) { g->busy++; g->on_row(g->user, r); g->busy--; }
        return;
    }
    b = &cu->rb[cu->rb_head++ % (uint64_t)cu->R];
    b->r = *r;
    b->idx = ysig__buf_idx(cu);
}
static void ysig__db_push(ysig_graph* g, const yin_event* e) {
    ysig__cu* cu = g->cu;
    ysig__dbuf* b;
    if (!cu->db) return;
    if (cu->db_head - cu->db_tail >= (uint64_t)cu->R) ysig__buf_room(g, cu->db[cu->db_tail % (uint64_t)cu->R].idx);
    if (cu->db_head - cu->db_tail >= (uint64_t)cu->R) {
        if (g->n_der >= YSIG__QCAP(g)) g->st.derived_lost++;
        else g->derq[g->n_der++] = *e;
        return;
    }
    b = &cu->db[cu->db_head++ % (uint64_t)cu->R];
    b->e = *e;
    b->idx = ysig__buf_idx(cu);
}

static void ysig__emit(ysig_graph* g, int i, const ysig_value* v) {
    ysig__nd* n = &g->nd[i];
    yin_event* e;
    yin_event held;
    int k;
    ysig__digest_word(g, (uint64_t)(uint32_t)i);
    ysig__digest_value(g, v);
    e = &held;
    memset(e, 0, sizeof *e);
    e->t = v->t;
    e->kind = (uint8_t)(YIN_KIND_USER + (int)n->p[0]);
    e->type = (uint8_t)YIN_SAMPLE;
    e->device = (uint32_t)i;
    e->control = (uint32_t)n->p[1];
    e->stamp = v->tier;
    e->unc_us = v->unc_us;
    e->code = (uint32_t)v->src | ((uint32_t)v->src_oldest << 16);
    for (k = 0; k < v->lanes && k < 4; k++) {
        float f = (float)v->v[k];
        if (k == 0) e->x = f;
        else if (k == 1) e->y = f;
        else if (k == 2) e->value = f;
        else e->aux = f;
    }
    if (g->cu && g->cu->derived_final) { ysig__db_push(g, &held); return; }
    /* v0.7.0: as a hit: one the caller already had that a run again makes
     * again is not queued twice */
    if (g->cu && g->cu->replaying) {
        const ysig__dl* seen = ysig__der_seen(g->cu, &held);
        if (seen) { ysig__dl_add(g->cu, 0, &held); return; }
    }
    if (g->n_der >= YSIG__QCAP(g)) { g->st.derived_lost++; return; }
    g->derq[g->n_der] = held;
    if (g->cu) {
        g->cu->dseq[g->n_der] = ++g->cu->next_dseq;
        g->cu->dqdl[g->n_der] = g->cu->d_head;
        ysig__dl_add(g->cu, g->cu->next_dseq, &held);
    }
    g->n_der++;
}

static void ysig__row_out(ysig_graph* g, int i, int per_frame, const ysig_value* v) {
    ysig_row r;
    memset(&r, 0, sizeof r);
    r.name = g->nd[i].name;
    r.node = i;
    r.per_frame = per_frame;
    r.frame = per_frame ? g->frame_index : 0;
    r.onset = per_frame ? g->onset : 0;
    r.value = *v;
    g->st.rows++;
    ysig__digest_word(g, (uint64_t)(uint32_t)i);
    ysig__digest_word(g, (uint64_t)r.frame);
    ysig__digest_value(g, &r.value);
    if (g->cu) {
        g->cu->row_h = ysig__hmix(ysig__hmix(ysig__hmix(g->cu->row_h, (uint64_t)(uint32_t)i | ((uint64_t)per_frame << 32)), (uint64_t)r.frame), (uint64_t)r.onset);
        g->cu->row_h = ysig__mix_value(g->cu->row_h, &r.value);
        if (g->cu->rows_final) { ysig__rb_push(g, &r); return; }
        if (g->cu->replaying) return;   /* a run again: on_row had it */
    }
    if (g->on_row) { g->busy++; g->on_row(g->user, &r); g->busy--; }
}

/* Integrate, DELTA: one axis, d more counts held inside [lo, hi] (the last
 * grid point inside, so a move back leaves the edge at once; yrsp_cursor's
 * rule in double). */
static int64_t ysig__kq(double q) { return q > 4e18 ? (int64_t)4e18 : q < -4e18 ? -(int64_t)4e18 : (int64_t)q; }
static double ysig__counts_axis(int64_t* n, double o, double gn, double lo, double hi, int64_t d) {
    double p;
    int i;
    *n += d;
    p = o + gn * (double)*n;
    if (lo == 0 && hi == 0) return p;
    if (p > hi) {
        int64_t k = ysig__kq((hi - o) / gn);
        for (i = 0; i < 4 && o + gn * (double)k > hi; i++) k--;
        for (i = 0; i < 4 && o + gn * (double)(k + 1) <= hi; i++) k++;
        *n = k;
    }
    if (o + gn * (double)*n < lo) {
        int64_t k = ysig__kq((lo - o) / gn);
        for (i = 0; i < 4 && o + gn * (double)k < lo; i++) k++;
        for (i = 0; i < 4 && o + gn * (double)(k - 1) >= lo; i++) k--;
        *n = k;
    }
    p = o + gn * (double)*n;
    return p < lo ? lo : p > hi ? hi : p;
}

static double ysig__gain(const ysig__nd* n, int k) { return n->p[k] != 0 ? n->p[k] : 1.0; }

static double ysig__bound(const ysig__nd* n, int k, double v) {
    double lo = n->p[8 + k], hi = n->p[12 + k];
    if (lo == 0 && hi == 0) return v;
    return v < lo ? lo : v > hi ? hi : v;
}

/* splitmix64 and a Gaussian by Box-Muller: the same numbers on every host */
static uint64_t ysig__mix(uint64_t* s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double ysig__gauss(uint64_t* s) {
    double u1 = ((double)(ysig__mix(s) >> 11) + 1.0) / 9007199254740993.0;
    double u2 = (double)(ysig__mix(s) >> 11) / 9007199254740992.0;
    return sqrt(-2.0 * log(u1)) * cos(2 * YSIG__PI * u2);
}

/* The velocity's lookup: the newest kept value at least lag before t, by
 * bisection (the ring is in time order). A linear scan from the newest cost
 * 73% of a 32-node graph's time at 16 kHz into one node (docs/sig.md). */
static const ysig_value* ysig__lagged(const ysig__ring* r, int64_t t, int64_t lag) {
    int f = ysig__ring_find(r, ysig__sub(t, lag));
    return f < 0 ? NULL : ysig__ring_at(r, f);
}

static void ysig__velocity_out(const ysig__nd* n, const ysig_value* x, const ysig_value* q, int64_t t, ysig_value* o) {
    double dt = (double)ysig__sub(t, q->t) / 1e9;
    int k;
    if (n->op == YSIG_OP_SPEED) {
        double s = 0;
        for (k = 0; k < x->lanes; k++) s += (x->v[k] - q->v[k]) * (x->v[k] - q->v[k]);
        o->v[0] = sqrt(s) / dt;
    } else {
        for (k = 0; k < x->lanes; k++) o->v[k] = (x->v[k] - q->v[k]) / dt;
    }
}

/* Run node i: mode 0 a sample, 1 a frame (at onset t), 2 the reset. */
static void ysig__run(ysig_graph* g, int i, int mode, int64_t t) {
    ysig__nd* n = &g->nd[i];
    ysig_value* o = &g->val[i];
    const ysig_value* in[4] = { NULL, NULL, NULL, NULL };
    int k, j;
    unsigned changed = 0;
    for (j = 0; j < n->n_in; j++) {
        in[j] = &g->val[n->in[j]];
        if (g->nd[n->in[j]].pass == g->pass) changed |= 1u << j;
    }
    /* the input that changed names a sample's report; a frame takes the
     * newest input, so a held part newer than a view keeps its time */
    if (mode != 0) changed = 0;
    n->pass = g->pass;
    if (n->flags & YSIG__F_SINK) {
        const ysig_value* x = in[0];
        switch (n->op) {
        case YSIG_OP_BIND:
            if (mode == 1)
                for (k = 0; k < x->lanes; k++) {
                    float c = (float)x->v[k];
                    g->channels[(int)n->p[0] + k] = c;
                    uint32_t cw;
                    memcpy(&cw, &c, sizeof cw);
                    ysig__digest_word(g, cw);
                }
            break;
        case YSIG_OP_RECORD:
            if (mode == 0 && n->p[0] == 0) ysig__row_out(g, i, 0, x);
            else if (mode == 1 && n->p[0] == 1) ysig__row_out(g, i, 1, x);
            break;
        case YSIG_OP_EMIT:
            if (mode != 2) ysig__emit(g, i, x);
            break;
        case YSIG_OP_DETECT:
            if (mode == 2) break;
            if (mode == 1) {
                int at_frame = n->p[5] == 1, view = (g->nd[n->in[0]].flags & YSIG__F_FRAME) != 0;
                if (at_frame || view) {
                    /* a frame-rate test is at the onset; a sample-rate one
                     * on a frame-updated input, at the value's time. A
                     * hold's clock there is the time the frame knows input
                     * up to, while the test is true: a still input's value
                     * keeps its old t, and the hold would never end */
                    ysig_value v = *x;
                    if (at_frame) v.t = t;
                    else if (n->kns[0] > 0 && g->t_known > v.t && ysig__test(n, x, 0)) v.t = g->t_known;
                    ysig__det_value(g, i, &v, v.t, ysig__sink_t(g, n, v.t));
                } else {
                    ysig__det_frame(g, i, t, x);
                }
            } else {
                ysig__det_value(g, i, x, x->t, ysig__sink_t(g, n, x->t));
            }
            if (n->hist.cap) ysig__hist_push(g, n, &g->val[i]);
            break;
        default: break;
        }
        return;
    }
    if (n->op >= YSIG_OP_CUSTOM) {
        if (n->n_in) {
            ysig__meta(n, in, changed, o);
            if (mode == 2) {
                if (n->vt->reset) { g->busy++; n->vt->reset(n->st, n->p, t); g->busy--; }
                for (k = 0; k < n->lanes; k++) o->v[k] = 0;
            } else {
                /* the op takes its inputs as one array: gathered, since
                 * the nodes need not be neighbors in val[] */
                ysig_value iv[4];
                for (j = 0; j < n->n_in; j++) iv[j] = *in[j];
                g->busy++;
                n->vt->sample(n->st, n->p, iv, n->n_in, o);
                g->busy--;
            }
        } else {
            if (mode == 2) {
                if (n->vt->reset) { g->busy++; n->vt->reset(n->st, n->p, t); g->busy--; }
                for (k = 0; k < n->lanes; k++) o->v[k] = 0;
                o->t = o->t_oldest = t;
            }
            else if (mode == 1) {
                o->t = o->t_oldest = t;
                o->tier = YIN_TIER_SIM;
                o->src = o->src_oldest = (uint16_t)i;
                g->busy++;
                n->vt->frame(n->st, n->p, t, o);
                g->busy--;
            }
        }
        o->lanes = n->lanes;
        if (mode != 1 || (n->flags & YSIG__F_NOSRC)) ysig__hist_push(g, n, o);
        return;
    }
    switch (n->op) {
    case YSIG_OP_SOURCE: break; /* ysig_feed() writes it */
    case YSIG_OP_INTEGRATE: {
        ysig__int_st* s = (ysig__int_st*)n->st;
        const ysig_value* x = in[0];
        ysig__meta(n, in, changed, o);
        if (mode == 2) {
            memset(s, 0, sizeof *s);
            for (k = 0; k < n->lanes; k++) { s->acc[k] = n->p[4 + k]; o->v[k] = n->p[4 + k]; }
            s->t_prev = t;
            break;
        }
        if (n->flags & YSIG__F_COUNTS) {
            if (mode != 0) break;
            for (k = 0; k < n->lanes; k++) {
                double v = x->v[k];
                int64_t dc;
                /* raw counts are whole; NaN or a count no mouse sends is no move */
                if (!(v > -1e9 && v < 1e9)) v = 0;
                dc = (int64_t)(v < 0 ? v - 0.5 : v + 0.5);
                o->v[k] = ysig__counts_axis(&s->n[k], s->recentered ? s->org[k] : n->p[4 + k], ysig__gain(n, k), n->p[8 + k],
                                            n->p[12 + k], dc);
            }
        } else {
            /* rate: the held input over the time since the last value; a
             * view runs only to the time input is known (t_known) */
            int64_t now = mode == 1 ? g->t_known : x->t;
            double dt = s->has && now > s->t_prev ? (double)ysig__sub(now, s->t_prev) / 1e9 : 0;
            for (k = 0; k < n->lanes; k++) {
                double v = ysig__bound(n, k, s->acc[k] + ysig__gain(n, k) * s->x[k] * dt);
                o->v[k] = v;
                if (mode == 0) { s->acc[k] = v; s->x[k] = x->v[k]; }
            }
            if (mode == 0) { s->t_prev = now > s->t_prev || !s->has ? now : s->t_prev; s->has = 1; }
            else o->t = now > o->t ? now : o->t;
        }
        break;
    }
    case YSIG_OP_LOWPASS: {
        ysig__lp_st* s = (ysig__lp_st*)n->st;
        const ysig_value* x = in[0];
        int order2 = n->p[1] == 2;
        ysig__meta(n, in, changed, o);
        if (mode == 2) {
            s->init = 0;
            for (k = 0; k < n->lanes; k++) o->v[k] = x->v[k];
            break;
        }
        if (!s->init) {
            if (mode == 1) break;
            for (k = 0; k < n->lanes; k++) { s->y1[k] = s->y2[k] = x->v[k]; o->v[k] = x->v[k]; }
            s->t_prev = x->t;
            s->init = 1;
            break;
        }
        {
            int64_t now = mode == 1 ? g->t_known : x->t;
            double dt = now > s->t_prev ? (double)ysig__sub(now, s->t_prev) / 1e9 : 0;
            double a = 1 - exp(-2 * YSIG__PI * n->p[0] * dt);
            for (k = 0; k < n->lanes; k++) {
                double y1 = s->y1[k] + a * (x->v[k] - s->y1[k]);
                double y2 = s->y2[k] + a * (y1 - s->y2[k]);
                o->v[k] = order2 ? y2 : y1;
                if (mode == 0) { s->y1[k] = y1; s->y2[k] = y2; }
            }
            if (mode == 0 && now > s->t_prev) s->t_prev = now;
            if (mode == 1 && now > o->t) o->t = now;
        }
        break;
    }
    case YSIG_OP_VELOCITY:
    case YSIG_OP_SPEED: {
        ysig__ring* r = (ysig__ring*)n->st;
        const ysig_value* x = in[0];
        const ysig_value* q;
        ysig__meta(n, in, changed, o);
        if (mode == 2) {
            r->n = r->head = 0;
            for (k = 0; k < n->lanes; k++) o->v[k] = 0;
            break;
        }
        if (mode == 0) {
            /* kept in time order: a value older than the newest (two
             * sources out of order) is used, not kept */
            if (!r->n || x->t >= ysig__ring_at(r, r->n - 1)->t) {
                if (r->n == r->cap && ysig__sub(x->t, ysig__ring_at(r, 0)->t) < n->kns[0]) g->st.ring_short++;
                ysig__ring_push(g, r, x);
            }
            q = ysig__lagged(r, x->t, n->kns[0]);
            if (q) ysig__velocity_out(n, x, q, x->t, o);
        } else {
            /* at a frame: still only when the input was silent for the
             * lag up to the time input is known (the poll), never from the
             * gap to a planned onset; then the difference over the lag is
             * 0. Otherwise the last sample's value stands. */
            int64_t last = r->n ? ysig__ring_at(r, r->n - 1)->t : x->t;
            if (ysig__sub(g->t_known, last) < n->kns[0]) break;
            for (k = 0; k < n->lanes; k++) o->v[k] = 0;
            o->t = g->t_known;
        }
        break;
    }
    case YSIG_OP_DELAY: {
        ysig__ring* r = (ysig__ring*)n->st;
        const ysig_value* x = in[0];
        int64_t want = ysig__sub(mode == 1 ? t : x->t, n->kns[0]);
        int f;
        if (mode == 2) {
            r->n = r->head = 0;
            ysig__ring_push(g, r, x);
            *o = *x;
            break;
        }
        if (mode == 0 && (!r->n || x->t >= ysig__ring_at(r, r->n - 1)->t)) ysig__ring_push(g, r, x);
        /* a view reads no later than the time input is known */
        if (mode == 1 && want > g->t_known) want = g->t_known;
        f = ysig__ring_find(r, want);
        if (f < 0) {
            if (r->n == r->cap) g->st.ring_short++;
            f = 0;
        }
        *o = *ysig__ring_at(r, f);
        break;
    }
    case YSIG_OP_LATCH: {
        ysig__latch_st* s = (ysig__latch_st*)n->st;
        const ysig_value* x = in[0];
        int r = ysig__truth(in[1]->v[0]);
        if (mode == 2) {
            s->latched = 0;
            s->prev = r;
            *o = *x;
            break;
        }
        if (!s->latched) {
            if (r && !s->prev) {
                /* a detect's hit on this input names the value and time
                 * the data records, also a hold that a frame completed */
                const ysig__nd* tr = &g->nd[n->in[1]];
                s->latched = 1;
                if (tr->op == YSIG_OP_DETECT && (int)tr->p[13] == YSIG_OUT_HIT && tr->in[0] == n->in[0] &&
                    ((const ysig__det_st*)tr->st)->fired) {
                    *o = ((const ysig__det_st*)tr->st)->v_hit;
                    o->t = ((const ysig__det_st*)tr->st)->t_hit;
                } else {
                    *o = *x;
                }
                /* a frame's capture has no sample's ring entry */
                if (mode == 1) ysig__hist_push(g, n, o);
            } else {
                *o = *x;
            }
        }
        s->prev = r;
        break;
    }
    case YSIG_OP_PEAK: {
        ysig__peak_st* s = (ysig__peak_st*)n->st;
        const ysig_value* x = in[0];
        int gate = n->n_in > 1 ? ysig__truth(in[1]->v[0]) : 1, rise, lane = (int)n->p[0], moved = 0;
        double q = 0;
        if (mode == 2) {
            s->has = s->seen = 0;
            s->best = 0;
            s->prev = gate;
            ysig__meta(n, in, 0, o);
            o->v[0] = 0;
            break;
        }
        if (changed & 1u) s->seen = 1;
        rise = gate && !s->prev;
        s->prev = gate;
        if (rise) s->has = 0;
        /* a sample's pass only: a frame's held or view value is no sample.
         * A rise before any sample has nothing to start from */
        if (gate && (rise || mode == 0) && s->seen) {
            if (lane >= 0) q = x->v[lane];
            else {
                for (k = 0; k < x->lanes; k++) q += x->v[k] * x->v[k];
                q = sqrt(q);
            }
            if (q == q && (!s->has || q > s->best)) {
                s->best = q;
                s->has = 1;
                *o = *x;
                o->v[0] = q;
                moved = 1;
            }
        }
        o->lanes = n->lanes;
        /* a late larger peak must not be skipped; a gate's rise at a frame
         * restarts it, though a frame's value does not otherwise enter */
        if (mode == 0 || moved) ysig__hist_push_later(g, n, o);
        return;
    }
    case YSIG_OP_SILENT: {
        ysig__silent_st* s = (ysig__silent_st*)n->st;
        ysig__meta(n, in, changed, o);
        if (mode == 2) {
            s->last = s->t_reset = t;
            o->v[0] = 0;
            break;
        }
        if (mode == 0) {
            if (in[0]->t > s->last) s->last = in[0]->t;
            o->v[0] = 0;
            break;
        }
        /* judged at the poll, as the views are; the last sample's time
         * and the reset's, whichever is later */
        if (ysig__sub(g->t_known, s->last) >= n->kns[0]) {
            o->v[0] = 1;
            o->t = g->t_known;
        } else {
            o->v[0] = 0;
        }
        break;
    }
    case YSIG_OP_SINES:
        if (mode == 2) { for (k = 0; k < n->lanes; k++) o->v[k] = 0; o->t = o->t_oldest = t; break; }
        if (mode == 1) {
            double tt = (double)g->base / 1e9, A = 1;
            int nc = (int)n->p[0], c;
            if (n->p[1] > 0) A = tt <= 0 ? 0 : tt < n->p[1] ? tt / n->p[1] : 1;
            for (k = 0; k < n->lanes; k++) {
                const double* q = n->data + k * nc * 3;
                double s = 0;
                for (c = 0; c < nc; c++) s += q[3 * c] * cos(2 * YSIG__PI * q[3 * c + 1] * tt + q[3 * c + 2]);
                o->v[k] = n->p[1] > 0 ? A * s : s;
            }
            o->t = o->t_oldest = t;
            o->tier = YIN_TIER_SIM;
            o->src = o->src_oldest = (uint16_t)i;
        }
        break;
    case YSIG_OP_NOISE: {
        ysig__noise_st* s = (ysig__noise_st*)n->st;
        if (mode == 2) {
            memset(s, 0, sizeof *s);
            s->s = (uint64_t)n->p[0];
            for (k = 0; k < n->lanes; k++) o->v[k] = 0;
            o->t = o->t_oldest = t;
            break;
        }
        if (mode == 1) {
            double sd = n->p[1] != 0 ? n->p[1] : 1, a = 1;
            if (n->p[2] > 0 && s->init) {
                double dt = t > s->t_prev ? (double)ysig__sub(t, s->t_prev) / 1e9 : 0;
                a = 1 - exp(-2 * YSIG__PI * n->p[2] * dt);
            }
            for (k = 0; k < n->lanes; k++) {
                double z = ysig__gauss(&s->s);
                s->y[k] = s->init && n->p[2] > 0 ? s->y[k] + a * (z - s->y[k]) : z;
                o->v[k] = sd * s->y[k];
            }
            s->init = 1;
            s->t_prev = t;
            o->t = o->t_oldest = t;
            o->tier = YIN_TIER_SIM;
            o->src = o->src_oldest = (uint16_t)i;
        }
        break;
    }
    case YSIG_OP_CHANNEL:
        if (mode == 2) { o->v[0] = 0; o->t = o->t_oldest = t; break; }
        if (mode == 1) {
            int c = (int)n->p[0];
            if (g->fvals && c < g->n_fvals) o->v[0] = (double)g->fvals[c];
            o->t = o->t_oldest = t;
            o->tier = YIN_TIER_SIM;
            o->src = o->src_oldest = (uint16_t)i;
        }
        break;
    default: /* the stateless ops */
        ysig__meta(n, in, changed, o);
        ysig__pure(n, in, o);
        break;
    }
    o->lanes = n->lanes;
    /* rings hold values at sample times: a frame's value (a view at the
     * onset) would make the samples after it look late */
    if (mode != 1 || (n->flags & YSIG__F_NOSRC)) ysig__hist_push(g, n, o);
}

/* --- API ------------------------------------------------------------------- */

static void ysig__journal(ysig_graph* g, const ysig_entry* e) {
    if (g->cu && g->cu->replaying) return;
    if (g->journal) { g->busy++; g->journal(g->user, e); g->busy--; }
}

static void ysig__gen_meta(ysig_value* o, int i, int64_t t) {
    o->t = o->t_oldest = t;
    o->tier = YIN_TIER_SIM;
    o->src = o->src_oldest = (uint16_t)i;
    o->device = 0;
}

/* exp_only: the experiment's nodes only (ysig_reset_trial(): D19). An
 * adapter's nodes keep their state and values, which the experiment's
 * nodes then read. */
static void ysig__settle(ysig_graph* g, int from, int64_t t);

/* v0.7.0: a call of key (t, cls) went into R reaching the nodes of f
 * (NULL: every node). Maxima only, so the order calls note in does not
 * matter, and an image keeps them with the nodes. */
static void ysig__key_note(ysig_graph* g, const uint64_t* f, int64_t t, int cls) {
    int i;
    for (i = 0; i < g->n; i++) {
        ysig__nd* n;
        if (f && !((f[i >> 6] >> (i & 63)) & 1u)) continue;
        n = &g->nd[i];
        if (t > n->kt || (t == n->kt && (cls > n->kc || (cls == n->kc && g->ko > n->ko)))) { n->kt = t; n->kc = cls; n->ko = g->ko; }
        if (cls < 2 && t > n->nt) n->nt = t;
    }
}
static const uint64_t* ysig__down(const ysig_graph* g, int node) {
    return &g->down[(size_t)node * (size_t)g->dw];
}
/* N(F): the newest input time of R that reaches F (NULL: every node) */
static int64_t ysig__nf(const ysig_graph* g, const uint64_t* f) {
    int64_t m = INT64_MIN;
    int i;
    for (i = 0; i < g->n; i++)
        if ((!f || ((f[i >> 6] >> (i & 63)) & 1u)) && g->nd[i].nt > m) m = g->nd[i].nt;
    return m == INT64_MIN ? g->t_cover : m;
}
/* A call of key (t, cls), made now, is late: R holds a call that reaches
 * a node of f with a greater key (the same class at one time: made
 * earlier, so smaller) */
static int ysig__is_late(const ysig_graph* g, const uint64_t* f, int64_t t, int cls, uint64_t ord) {
    int i;
    for (i = 0; i < g->n; i++) {
        const ysig__nd* n = &g->nd[i];
        if (f && !((f[i >> 6] >> (i & 63)) & 1u)) continue;
        if (n->kt > t || (n->kt == t && (n->kc > cls || (n->kc == cls && n->ko > ord)))) return 1;
    }
    return 0;
}
/* A barrier at t: R starts again (N(F) is t after a reset; a flush
 * changes no N(F), only what a later call can go before) */
static void ysig__key_bar(ysig_graph* g, int64_t t, int reset) {
    int i;
    for (i = 0; i < g->n; i++) {
        g->nd[i].kt = t;
        g->nd[i].kc = 0;
        g->nd[i].ko = 0;
        g->nd[i].dseq = 0;
        if (reset) g->nd[i].nt = t;
    }
    g->bar_t = t;
}

static void ysig__fp_or(const ysig_graph* g, uint64_t* f, int node);
static void ysig__reset(ysig_graph* g, int64_t t, int exp_only) {
    int k;
    g->n_hit = g->n_der = 0;
    g->pass++;
    g->t_known = t;
    for (k = 0; k < g->n; k++) {
        int id = g->order[k];
        ysig__nd* n = &g->nd[id];
        ysig_value* o = &g->val[id];
        if (exp_only && id >= g->n_exp) continue;
        n->hist.n = n->hist.head = 0;
        n->sink_t = INT64_MIN;
        if (n->op == YSIG_OP_SOURCE) {
            /* no sample yet: no value, tier UNKNOWN, an empty ring */
            memset(n->st, 0, sizeof(ysig__src_st));
            memset(o, 0, sizeof *o);
            ysig__gen_meta(o, id, t);
            o->tier = YIN_TIER_UNKNOWN;
            o->lanes = n->lanes;
            continue;
        }
        if (!(n->flags & YSIG__F_SINK) && !n->n_in) ysig__gen_meta(o, id, t);
        if (n->op == YSIG_OP_DETECT) {
            memset(n->st, 0, sizeof(ysig__det_st));
            memset(o, 0, sizeof *o);
            ysig__gen_meta(o, id, t);
            o->lanes = 1;
            if (n->p[6] == 0) ysig__det_arm(g, id, t, &g->val[n->in[0]]);
            /* a hit output is 0 at the reset; an arm that was a hit (entry
             * now) rises after the loop, so its users see the rise */
            o->v[0] = (int)n->p[13] != YSIG_OUT_HIT && ((ysig__det_st*)n->st)->state ? 1.0 : 0.0;
            ysig__hist_push(g, n, o);
            continue;
        }
        ysig__run(g, id, 2, t);
    }
    for (k = 0; k < g->n; k++) {
        /* an entry-now hit at the reset: the rise as ysig_arm() gives it,
         * after every node has its reset value, so a latch freezes and a
         * peak's gate restarts */
        int id = g->order[k];
        ysig__nd* n = &g->nd[id];
        if (exp_only && id >= g->n_exp) continue;
        if (n->op != YSIG_OP_DETECT || n->p[6] != 0 || (int)n->p[13] != YSIG_OUT_HIT || !((ysig__det_st*)n->st)->fired) continue;
        g->val[id].v[0] = 1.0;
        g->val[id].t = t;
        ysig__hist_push(g, n, &g->val[id]);
        ysig__settle(g, id, t);
    }
}

static void ysig__do_reset(ysig_graph* g, int64_t t, int exp_only);
static void ysig__ck_take(ysig_graph* g);
static int ysig__log(ysig_graph* g, const ysig_entry* e);

/* A call that changes the graph, made from a callback (on_row, the
 * journal, a custom op) during another call: it would run inside that
 * call but be logged and journaled after it, so a run again and a replay
 * would run it in another order. Refused, counted. */
static int ysig__reentered(ysig_graph* g, const char* fn) {
    if (!g->busy) return 0;
    g->st.call_reentry++;
    snprintf(g->err, sizeof g->err, "%s: called from a callback during another call", fn);
    return 1;
}

YSIG_API bool ysig_open(ysig_graph* g, const ysig_desc* d) {
    ysig__an a;
    size_t need;
    char* base;
    int i, k;
    if (!g) return false;
    memset(g, 0, sizeof *g);
    if (!ysig__analyze(d, &a, g->err, sizeof g->err)) return false;
    need = ysig__layout(d, &a, NULL, NULL);
    if (!need) {
        snprintf(g->err, sizeof g->err, "ysig_open: the graph needs more memory than size_t can count");
        return false;
    }
    if (!d->arena || d->arena_size < need) {
        snprintf(g->err, sizeof g->err, "ysig_open: the arena has %lu bytes; the graph needs %lu (ysig_arena_size())",
                 (unsigned long)d->arena_size, (unsigned long)need);
        return false;
    }
    base = (char*)d->arena;
    if (((uintptr_t)base & 7u) != 0) {
        snprintf(g->err, sizeof g->err, "ysig_open: the arena is not 8-byte aligned");
        return false;
    }
    memset(base, 0, need);
    ysig__layout(d, &a, g, base);
    g->n = a.n;
    g->n_exp = a.n_exp;
    g->n_adapters = d->n_adapters;
    for (i = 0; i < d->n_adapters; i++) {
        g->adapter_base[i] = a.base[i];
        g->adapter_n[i] = d->adapters[i].n_nodes;
        g->adapter_type[i] = d->adapters[i].type;
    }
    for (i = 0; i < a.n; i++) {
        ysig__nd* n = &g->nd[i];
        const ysig_node* s = a.node[i];
        memcpy(n->p, s->p, sizeof n->p);
        n->op = s->op;
        for (k = 0; k < 4; k++) n->in[k] = a.in[i][k];
        n->n_in = a.n_in[i];
        n->lanes = a.lanes[i];
        n->flags = a.flags[i];
        n->name = s->name;
        n->vt = s->op >= YSIG_OP_CUSTOM ? ysig__vt(d, s->op) : NULL;
        n->n_data = s->n_data;
        if (s->n_data) memcpy(n->data, s->data, sizeof(double) * (size_t)s->n_data);
        if (s->op == YSIG_OP_VELOCITY || s->op == YSIG_OP_SPEED || s->op == YSIG_OP_DELAY) {
            ysig__ring* r = (ysig__ring*)n->st;
            r->cap = ysig__ring_cap(s);
            r->buf = (ysig_value*)(void*)((char*)n->st + ((sizeof(ysig__ring) + 15u) & ~(size_t)15u));
        }
        ysig__prepare(g, i);
    }
    for (k = 0; k < a.n; k++) g->order[k] = a.order[k];
    /* each node's downstream set, users last in order, so a node's set is
     * whole when its inputs take it */
    for (k = a.n - 1; k >= 0; k--) {
        int id = g->order[k], j, w;
        uint64_t* dn = g->down + (size_t)id * (size_t)g->dw;
        dn[id >> 6] |= 1ull << (id & 63);
        for (j = 0; j < g->nd[id].n_in; j++) {
            uint64_t* di = g->down + (size_t)g->nd[id].in[j] * (size_t)g->dw;
            for (w = 0; w < g->dw; w++) di[w] |= dn[w];
        }
    }
    ysig__sched(&a, g->src_ids, g->src_off, g->sched, g->frame);
    g->n_src = a.n_src;
    g->n_frame = a.n_frame;
    g->channels = d->channels;
    g->n_channels = d->n_channels;
    g->on_row = d->on_row;
    g->journal = d->journal;
    g->user = d->user;
    for (i = 0; i < a.n; i++) {
        if (g->nd[i].flags & YSIG__F_VIEW) g->has_view = 1;
        if (g->nd[i].op == YSIG_OP_CHANNEL && (int)g->nd[i].p[0] + 1 > g->ch_hi) g->ch_hi = (int)g->nd[i].p[0] + 1;
    }
    g->t_fed = INT64_MIN;
    g->ok = 1;
    ysig__reset(g, 0, 0);
    g->st.digest = 14695981039346656037ull;
    g->t_cover = INT64_MIN;
    ysig__ck_take(g);   /* with a window, its first checkpoint */
    return true;
}

YSIG_API void ysig_reset(ysig_graph* g, int64_t t_ns) {
    ysig_entry e;
    if (!g || !g->ok || ysig__reentered(g, "ysig_reset")) return;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_RESET;
    e.t = t_ns;
    ysig__journal(g, &e);
    g->wt->seq++;
    g->ko = g->wt->seq;
    ysig__before_barrier(g);
    ysig__log(g, &e);
    ysig__do_reset(g, t_ns, 0);
    ysig__out_final(g);
}

YSIG_API void ysig_reset_trial(ysig_graph* g, int64_t t_ns) {
    ysig_entry e;
    if (!g || !g->ok || ysig__reentered(g, "ysig_reset_trial")) return;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_RESET_TRIAL;
    e.t = t_ns;
    ysig__journal(g, &e);
    g->wt->seq++;
    g->ko = g->wt->seq;
    ysig__before_barrier(g);
    ysig__log(g, &e);
    ysig__do_reset(g, t_ns, 1);
    ysig__out_final(g);
}

/* A role's value in the role's units, back through the adapter's scale and
 * offset nodes to its integrator, in that integrator's units. *id is the
 * integrator; 0 and the reason in g->err when the chain cannot be inverted. */
static int ysig__recenter_at(ysig_graph* g, int role, double xy[2], int* id) {
    int a = -1, cur = -1, i, bad = -1, k;
    for (i = 0; i < g->n_exp; i++)
        if (g->nd[i].op == YSIG_OP_ROLE && (int)g->nd[i].p[0] == role) { cur = g->nd[i].in[0]; break; }
    if (cur < 0) {
        snprintf(g->err, sizeof g->err, "ysig_recenter: no node reads role %d", role);
        return 0;
    }
    for (i = 0; i < g->n_adapters; i++)
        if (cur >= g->adapter_base[i] && cur < g->adapter_base[i] + g->adapter_n[i]) a = i;
    if (a < 0 || g->adapter_type[a] != YSIG_TYPE_POSITION2D_RELATIVE) {
        snprintf(g->err, sizeof g->err, "ysig_recenter: role %d's adapter is not relative (position2d_relative): "
                 "only a relative role can be placed", role);
        return 0;
    }
    for (;;) {
        const ysig__nd* n = &g->nd[cur];
        if (n->op == YSIG_OP_INTEGRATE) break;
        if (n->op == YSIG_OP_SCALE && n->n_in == 1) {
            for (k = 0; k < 2; k++) xy[k] /= n->p[k] != 0 ? n->p[k] : 1.0;
        } else if (n->op == YSIG_OP_OFFSET) {
            for (k = 0; k < 2; k++) xy[k] -= n->p[k];
        } else if (n->op != YSIG_OP_SOURCE && bad < 0) {
            bad = cur;
        }
        if (!n->n_in || n->in[0] < g->adapter_base[a] || n->in[0] >= g->adapter_base[a] + g->adapter_n[a]) {
            snprintf(g->err, sizeof g->err, "ysig_recenter: role %d's adapter has no integrator", role);
            return 0;
        }
        cur = n->in[0];
    }
    if (bad >= 0) {
        snprintf(g->err, sizeof g->err, "ysig_recenter: role %d's adapter maps its integrator through a %s; recenter needs "
                 "a scale or an offset", role, ysig_op_name(g->nd[bad].op));
        return 0;
    }
    if (!isfinite(xy[0]) || !isfinite(xy[1])) {
        snprintf(g->err, sizeof g->err, "ysig_recenter: the point is not finite after the adapter's scale");
        return 0;
    }
    *id = cur;
    return 1;
}

static void ysig__do_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns);
static int ysig__log(ysig_graph* g, const ysig_entry* e);

YSIG_API bool ysig_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns) {
    ysig_entry e;
    double xy[2];
    int id = -1, k;
    ysig__nd* n;
    if (!g || !g->ok || ysig__reentered(g, "ysig_recenter")) return false;
    if (role < 0 || !isfinite(x) || !isfinite(y)) {
        snprintf(g->err, sizeof g->err, "ysig_recenter: a bad role or a point that is not finite");
        return false;
    }
    xy[0] = x;
    xy[1] = y;
    if (!ysig__recenter_at(g, role, xy, &id)) return false;
    n = &g->nd[id];
    /* outside the integrator's bounds the next count would jump to the edge */
    for (k = 0; k < 2; k++)
        if ((n->p[8 + k] != 0 || n->p[12 + k] != 0) && (!(xy[k] >= n->p[8 + k]) || !(xy[k] <= n->p[12 + k]))) {
            snprintf(g->err, sizeof g->err, "ysig_recenter: lane %d of the point is outside the integrator's bounds", k);
            return false;
        }
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_RECENTER;
    e.node = role;
    e.value = x;
    e.y = y;
    e.t = t_ns;
    ysig__journal(g, &e);
    ysig__before_state(g, id, ysig__nf(g, ysig__down(g, id)));
    g->wt->seq++;
    g->ko = g->wt->seq;
    g->wt->gap_n++;
    ysig__fp_or(g, g->wt->gap_f, id);
    ysig__log(g, &e);
    g->tk = ysig__nf(g, ysig__down(g, id));
    ysig__do_recenter(g, role, x, y, t_ns);
    ysig__out_final(g);
    return true;
}

/* The checks passed (ysig_recenter()): the same chain again, deterministic. */
static void ysig__do_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns) {
    uint8_t reach[YSIG_MAX_NODES];
    double xy[2];
    int id = -1, k, pos = 0;
    ysig__nd* n;
    ysig__int_st* s;
    ysig_value* o;
    xy[0] = x;
    xy[1] = y;
    if (!ysig__recenter_at(g, role, xy, &id)) return;
    n = &g->nd[id];
    s = (ysig__int_st*)n->st;
    o = &g->val[id];
    for (k = 0; k < 2; k++) {
        if (n->flags & YSIG__F_COUNTS) { s->n[k] = 0; s->org[k] = xy[k]; }
        else s->acc[k] = xy[k];
        o->v[k] = xy[k];
    }
    s->recentered = 1;
    ysig__key_note(g, ysig__down(g, id), g->tk, 2);
    s->t_prev = t_ns;                /* a rate integrator: no jump by the time since its last sample */
    o->t = o->t_oldest = t_ns;
    n->pass = ++g->pass;
    ysig__hist_push(g, n, o);        /* a new entry: the history before t_ns stands */
    /* only what the jump would spike restarts (a filter, a difference, a
     * delay); the stateless nodes take the new value; anything else keeps
     * its state and output, and what it feeds is not touched */
    memset(reach, 0, (size_t)g->n);
    reach[id] = 1;
    for (k = 0; k < g->n; k++) if (g->order[k] == id) pos = k;
    for (k = pos + 1; k < g->n; k++) {
        int nid = g->order[k], j, hit = 0;
        ysig__nd* m = &g->nd[nid];
        for (j = 0; j < m->n_in; j++) if (reach[m->in[j]]) hit = 1;
        if (!hit) continue;
        if (m->op == YSIG_OP_LATCH) {
            if (((ysig__latch_st*)m->st)->latched) continue;
            g->val[nid] = g->val[m->in[0]];
            ysig__hist_push(g, m, &g->val[nid]);
        } else if (m->op == YSIG_OP_LOWPASS || m->op == YSIG_OP_VELOCITY || m->op == YSIG_OP_SPEED || m->op == YSIG_OP_DELAY ||
                   (m->op < YSIG_OP_COUNT && (ysig__ops[m->op].kind & YSIG__K_PURE))) {
            ysig__run(g, nid, 2, t_ns);
        } else {
            continue;
        }
        reach[nid] = 1;
    }
}

/* A value for p[param] of node n that the op allows after open. */
static bool ysig__set_ok(const ysig_graph* g, const ysig__nd* n, int param, double v) {
    double q[YSIG_NP];
    int k, L = n->lanes;
    memcpy(q, n->p, sizeof q);
    q[param] = v;
    switch (n->op) {
    case YSIG_OP_ROLE: return false;
    case YSIG_OP_SOURCE: return param != 3 && param != 0 && param != 1;   /* the lanes; integrate's counts mode */
    case YSIG_OP_MEAN: {
        double ws = 0;
        for (k = 0; k < n->n_in; k++) ws += q[k] != 0 ? q[k] : 1;
        return ws != 0;
    }
    case YSIG_OP_VELOCITY: case YSIG_OP_SPEED: case YSIG_OP_DELAY: return param != 1;
    case YSIG_OP_SINES: return param != 0;
    case YSIG_OP_RECORD: return false;
    case YSIG_OP_CHANNEL:
        /* up to the highest channel a channel node read at the open: the
         * window keeps a frame's values up to it, so a run again reads
         * what the frame gave (with or without a window, one rule) */
        return (int)v < g->ch_hi;
    case YSIG_OP_DETECT:
        if (param == 5 || param == 13) return false;   /* the schedules depend on them */
        if ((int)q[1] >= g->val[n->in[0]].lanes) return false;
        if ((int)q[0] >= YSIG_TEST_IN_RECT && (g->val[n->in[0]].lanes < 2 || q[8] > q[10] || q[9] > q[11])) return false;
        return true;
    case YSIG_OP_SELECT: return (int)v < g->val[n->in[0]].lanes;
    case YSIG_OP_PEAK: return (int)v < g->val[n->in[0]].lanes;
    case YSIG_OP_BIND: return (int)v + g->val[n->in[0]].lanes <= g->n_channels;
    case YSIG_OP_LOWPASS: return !(q[2] > 0) || q[0] < q[2] / 2;
    case YSIG_OP_INTEGRATE:
        for (k = 0; k < L; k++)
            if ((q[8 + k] != 0 || q[12 + k] != 0) && (!(q[8 + k] < q[12 + k]) || q[k] < 0)) return false;
        return true;
    case YSIG_OP_CLAMP:
        for (k = 0; k < L; k++)
            if ((q[k] != 0 || q[4 + k] != 0) && q[k] > q[4 + k]) return false;
        return true;
    default: return true;
    }
}

/* After ysig_set() on a stateless node: it and its stateless users take
 * the new parameters from their inputs' held values now, not at the next
 * sample, so a frame-rate test or a bind before then reads the new map
 * (G17: a block's new transform with a still hand). No ring entry: a ring
 * holds values at sample times, and no sample came. */
static void ysig__recompute(ysig_graph* g, int from) {
    uint8_t reach[YSIG_MAX_NODES];
    int k, j, pos = -1;
    /* the newest input that reaches the set at its place in R (v0.7.0),
     * not the newest event fed: input that does not reach the set commutes
     * with it, so it must not move its entry; a run again computes it at
     * the set's place, as a graph with no window does at its call */
    int64_t ts = ysig__nf(g, ysig__down(g, from));
    for (k = 0; k < g->n; k++) if (g->order[k] == from) pos = k;
    if (pos < 0) return;
    memset(reach, 0, (size_t)g->n);
    for (k = pos; k < g->n; k++) {
        int nid = g->order[k], hit = nid == from, l;
        ysig__nd* m = &g->nd[nid];
        const ysig_value* in[4] = { NULL, NULL, NULL, NULL };
        ysig_value nv;
        if (m->op >= YSIG_OP_COUNT || !(ysig__ops[m->op].kind & YSIG__K_PURE) || !m->n_in) continue;
        for (j = 0; j < m->n_in; j++) {
            in[j] = &g->val[m->in[j]];
            if (reach[m->in[j]]) hit = 1;
        }
        if (!hit) continue;
        /* only the lanes change: no sample came, so t, sources and tier
         * stay the input's that made the value */
        nv = g->val[nid];
        ysig__meta(m, in, 0, &nv);
        ysig__pure(m, in, &nv);
        for (l = 0; l < YSIG_LANES; l++) g->val[nid].v[l] = nv.v[l];
        if (m->hist.cap && !(m->flags & YSIG__F_AT)) {
            /* a past read at or after the set's time gives the new value,
             * one before it the old; a node computed from its inputs at a
             * past time (F_AT) uses the parameters it has now */
            ysig_value e = g->val[nid];
            if (m->hist.n) {
                int64_t last = m->hist.buf[(m->hist.head + m->hist.n - 1) % m->hist.cap].t;
                if (ts < last) ts = last;
            }
            e.t = ts;
            ysig__hist_push(g, m, &e);
        }
        reach[nid] = 1;
    }
}

static void ysig__do_set(ysig_graph* g, int node, int param, double v) {
    ysig__key_note(g, ysig__down(g, node), g->tk, 2);
    g->nd[node].p[param] = v;
    ysig__prepare(g, node);
    ysig__recompute(g, node);
}

YSIG_API bool ysig_set(ysig_graph* g, int node, int param, double v) {
    const ysig_param* rows;
    ysig__nd* n;
    ysig_entry e;
    int nr;
    if (!g || !g->ok || node < 0 || node >= g->n || param < 0 || param >= YSIG_NP || !isfinite(v)) return false;
    if (ysig__reentered(g, "ysig_set")) return false;
    n = &g->nd[node];
    if (n->vt) { rows = n->vt->params; nr = n->vt->n_params; }
    else rows = ysig_op_params(n->op, &nr);
    if (param >= nr || !ysig__param_ok(&rows[param], v) || !ysig__set_ok(g, n, param, v)) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_SET;
    e.node = node;
    e.param = param;
    e.value = v;
    ysig__journal(g, &e);
    ysig__before_state(g, node, ysig__nf(g, ysig__down(g, node)));
    g->wt->seq++;
    g->ko = g->wt->seq;
    g->wt->gap_n++;
    ysig__fp_or(g, g->wt->gap_f, node);
    ysig__log(g, &e);
    g->tk = ysig__nf(g, ysig__down(g, node));
    ysig__do_set(g, node, param, v);
    ysig__out_final(g);
    return true;
}

YSIG_API bool ysig_set_data(ysig_graph* g, int node, int index, double v) {
    ysig_entry e;
    if (!g || !g->ok || node < 0 || node >= g->n || index < 0 || index >= g->nd[node].n_data || !isfinite(v)) return false;
    if (ysig__reentered(g, "ysig_set_data")) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_SET_DATA;
    e.node = node;
    e.param = index;
    e.value = v;
    ysig__journal(g, &e);
    ysig__before_state(g, node, ysig__nf(g, ysig__down(g, node)));
    g->wt->seq++;
    g->ko = g->wt->seq;
    g->wt->gap_n++;
    ysig__fp_or(g, g->wt->gap_f, node);
    ysig__log(g, &e);
    g->tk = ysig__nf(g, ysig__down(g, node));
    ysig__key_note(g, ysig__down(g, node), g->tk, 2);
    g->nd[node].data[index] = v;
    ysig__out_final(g);
    return true;
}

/* A stamp more than YSIG_FUTURE_NS past the poll is a bad one: it must not
 * make its source drop every later sample or hide a silence. A stamp
 * within that of the poll changes nothing. */
static void ysig__lower_late(ysig_graph* g, int64_t poll) {
    int64_t lim = ysig__add(poll, YSIG_FUTURE_NS);
    int i, d;
    for (i = 0; i < g->n; i++) {
        ysig__nd* n = &g->nd[i];
        if (n->op == YSIG_OP_SOURCE) {
            ysig__src_st* s = (ysig__src_st*)n->st;
            for (d = 0; d < s->n; d++)
                if (s->last_t[d] > lim) s->last_t[d] = poll;
        } else if (n->op == YSIG_OP_SILENT) {
            ysig__silent_st* s = (ysig__silent_st*)n->st;
            if (s->last > lim) s->last = poll;
        }
    }
}

static int ysig__source_takes(const ysig__nd* n, const yin_event* e) {
    uint32_t type = n->p[5] != 0 ? (uint32_t)n->p[5] : (uint32_t)YIN_SAMPLE;
    return e->kind == (uint8_t)n->p[0] && e->type == type && e->control == (uint32_t)n->p[1] &&
           (n->p[2] == 0 || e->device == (uint32_t)n->p[2]);
}

static int ysig__do_feed(ysig_graph* g, const yin_event* e) {
    int i, k, taken = 0, journaled = 0;
    g->st.fed++;
    for (i = 0; i < g->n_src; i++) {
        int id = g->src_ids[i];
        ysig__nd* n = &g->nd[id];
        ysig__src_st* s = (ysig__src_st*)n->st;
        ysig_value* o = &g->val[id];
        unsigned m = (unsigned)n->p[3];
        int L = n->lanes, at = 0;
        float f[4];
        if (!ysig__source_takes(n, e)) continue;
        /* the newest time a source saw bounds the poll from below
         * (ysig_frame()); only those events are journaled */
        if (e->t > g->t_fed) g->t_fed = e->t;
        if (e->t > g->t_cover) g->t_cover = e->t;
        if (!journaled && g->journal) {
            ysig_entry j;
            memset(&j, 0, sizeof j);
            j.kind = YSIG_J_FEED;
            j.event = *e;
            ysig__journal(g, &j);
            journaled = 1;
        }
        /* within one device of a source times rise (ysp/input.h); an older
         * sample is counted and dropped. Raw mouse counts are never
         * dropped: a sum does not depend on the order of its terms. */
        {
            int d, counts = (int)n->p[0] == YIN_KIND_MOUSE && (uint32_t)n->p[1] == YIN_AXIS_DELTA;
            for (d = 0; d < s->n && s->dev[d] != e->device; d++) { }
            if (d == s->n) {
                if (s->n < YSIG__SRC_DEVS) d = s->n++;
                else {
                    int q;
                    for (d = 0, q = 1; q < YSIG__SRC_DEVS; q++)
                        if (s->used[q] < s->used[d]) d = q;
                    g->st.order_evicted++;
                }
                s->dev[d] = e->device;
                s->last_t[d] = e->t;
            } else if (e->t < s->last_t[d] && !counts) {
                g->st.dropped_order++;
                continue;
            }
            s->used[d] = ++s->clock;
            if (e->t > s->last_t[d]) s->last_t[d] = e->t;
        }
        f[0] = e->x; f[1] = e->y; f[2] = e->value; f[3] = e->aux;
        if (!m) {
            if (L == 1) o->v[0] = (double)e->value;
            else for (k = 0; k < L; k++) o->v[k] = (double)f[k];
        } else {
            for (k = 0; k < 4; k++)
                if (m & (1u << k)) o->v[at++] = (double)f[k];
        }
        o->t = o->t_oldest = e->t;
        o->device = e->device;
        o->tier = e->stamp ? e->stamp : (uint8_t)n->p[4];
        o->src = o->src_oldest = (uint16_t)id;
        o->unc_us = e->unc_us;
        o->lanes = (uint8_t)L;
        ysig__hist_push(g, n, o);
        ysig__key_note(g, ysig__down(g, id), e->t, 0);
        n->pass = ++g->pass;   /* this sample's evaluation: the source changed */
        for (k = g->src_off[i]; k < g->src_off[i + 1]; k++) ysig__run(g, g->sched[k], 0, e->t);
        taken++;
    }
    g->st.taken += taken;
    return taken;
}

static int ysig__log(ysig_graph* g, const ysig_entry* e);
static int ysig__late_fix(ysig_graph* g, uint64_t a);
static void ysig__late_nolog(ysig_graph* g);

YSIG_API int ysig_feed(ysig_graph* g, const yin_event* e) {
    ysig_entry j;
    int i, takers = 0, r, all, late = 0, logged = 0;
    uint64_t f[YSIG__FPW], mask;
    if (!g || !g->ok || !e || ysig__reentered(g, "ysig_feed")) return -1;
    for (i = 0; i < g->n_src; i++)
        if (ysig__source_takes(&g->nd[g->src_ids[i]], e)) takers = 1;
    if (takers) {
        /* an input call: the waiting arms it passes go first. Only the
         * sources that keep it (an older sample of its device is dropped)
         * change nodes: its footprint */
        for (i = 0; i < g->dw; i++) f[i] = 0;
        /* one every source drops is no input call: it changes no node, so
         * it neither ends a gap nor goes into the log (its count stands) */
        if (!ysig__feed_fp(g, e, f, &mask, &all)) takers = 0;
    }
    if (takers && e->t < g->bar_t) {
        /* v0.7.0: before the last barrier: R does not have it either */
        if (g->journal) {
            memset(&j, 0, sizeof j);
            j.kind = YSIG_J_FEED;
            j.event = *e;
            ysig__journal(g, &j);
        }
        /* not in R: not one of R's calls fed (fed counts R's) */
        g->st.late_barrier++;
        return 0;
    }
    if (takers) {
        g->wt->seq++;
        g->ko = g->wt->seq;
        ysig__before_input(g, e->t, f);
        /* after the gap's end: its disarms are in R now */
        late = ysig__is_late(g, f, e->t, 0, g->ko);
        if (g->cu) {
            /* as the journal: only an event a source takes changes
             * anything (but the count of calls), so a replay of the
             * journal fills the window as the live calls did */
            memset(&j, 0, sizeof j);
            j.kind = YSIG_J_FEED;
            j.event = *e;
            g->cu->srcm = mask;
            g->cu->srcm_all = all;
            logged = ysig__log(g, &j);
            g->cu->srcm = 0;
            g->cu->srcm_all = 0;
        }
    }
    g->ko = g->wt->seq;
    r = ysig__do_feed(g, e);
    if (late) {
        /* made at the end of R: to its place (a run again), or counted */
        if (logged) (void)ysig__late_fix(g, g->cu->c_head - 1);
        else ysig__late_nolog(g);
    }
    ysig__out_final(g);
    return r;
}

static void ysig__ck_take(ysig_graph* g);

static void ysig__do_frame(ysig_graph* g, const ysig_frame_in* f) {
    int k;
    g->frame_index = f->index;
    g->onset = f->onset_ns;
    if (f->onset_ns > g->t_cover) g->t_cover = f->onset_ns;
    g->base = f->base_ns;
    g->fvals = f->values;
    g->n_fvals = f->n_values;
    /* views use input up to the given poll, never the gap to a planned
     * onset. An event stamped past the poll is counted and moves nothing:
     * one bad stamp must not make every later view run to the onset (a
     * view whose last sample is past the poll stands) */
    if (f->has_poll) {
        if (g->t_fed > f->poll_ns) {
            int64_t ex = ysig__sub(g->t_fed, f->poll_ns);
            g->st.late_poll++;
            if (ex > g->st.late_poll_max_ns) g->st.late_poll_max_ns = ex;
            if (ex > YSIG_FUTURE_NS) ysig__lower_late(g, f->poll_ns);
        }
        g->t_known = f->poll_ns < f->onset_ns ? f->poll_ns : f->onset_ns;
    } else {
        g->t_known = f->onset_ns;   /* no view reads it (refused above) */
    }
    ysig__digest_word(g, (uint64_t)f->index);
    ysig__digest_word(g, (uint64_t)f->onset_ns);
    ysig__key_note(g, NULL, f->onset_ns, 1);
    g->pass++;
    for (k = 0; k < g->n_frame; k++) ysig__run(g, g->frame[k], 1, f->onset_ns);
    g->fvals = NULL;
    g->n_fvals = 0;
    if (g->cu && g->channels && g->n_channels > 0) {
        /* late_frames: a frame's bound channels as made, and as a run
         * again for a late call makes them (counted once a frame) */
        ysig__cu* cu = g->cu;
        uint64_t h = 1469598103934665603ull;
        const unsigned char* b = (const unsigned char*)g->channels;
        size_t i, nb = sizeof(float) * (size_t)g->n_channels;
        for (i = 0; i < nb; i++) h = (h ^ b[i]) * 1099511628211ull;
        if (!cu->replaying) {
            if (cu->cur != UINT64_MAX && cu->cur >= cu->c_tail && cu->cur < cu->c_head) cu->calls[cu->cur % (uint64_t)cu->C].chh = h;
        } else if (cu->c_pos > cu->c_tail) {
            ysig__call* c = &cu->calls[(cu->c_pos - 1) % (uint64_t)cu->C];
            if (c->e.kind == YSIG_J_FRAME) {
                if (cu->late_on && c->chh != h && !c->lf) { cu->n_lf++; c->lf = 1; }
                c->chh = h;
            }
        }
    }
    ysig__ck_take(g);   /* the window's checkpoint after each frame */
}

YSIG_API int ysig_frame(ysig_graph* g, const ysig_frame_in* f) {
    ysig_entry e;
    int late, logged;
    if (!g || !g->ok || !f || f->n_values < 0 || (f->n_values && !f->values) || ysig__reentered(g, "ysig_frame")) return -1;
    /* a graph with a view needs the poll's time: none is a configuration
     * error, refused on every call whatever the data */
    if (g->has_view && !f->has_poll) {
        snprintf(g->err, sizeof g->err, "ysig_frame: no poll (has_poll false) and the graph has a velocity, speed, delay, "
                 "lowpass, rate integrate or silent: give the time input was polled up to");
        return -1;
    }
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_FRAME;
    e.frame = *f;
    ysig__journal(g, &e);
    g->wt->seq++;
    g->ko = g->wt->seq;
    /* v0.7.0: a frame is at its onset in R; one after a feed later than
     * it (the frame loop fed past the onset first) is late */
    ysig__before_input(g, f->onset_ns, NULL);
    /* one before the last barrier goes right after it (every call since
     * has a greater key); a frame is not dropped: its pass draws */
    late = ysig__is_late(g, NULL, f->onset_ns, 1, g->ko);
    logged = ysig__log(g, &e);
    g->ko = g->wt->seq;
    ysig__do_frame(g, f);
    if (late) {
        if (logged) (void)ysig__late_fix(g, g->cu->c_head - 1);
        else ysig__late_nolog(g);
    }
    ysig__out_final(g);
    return 0;
}

/* A change of a detect's output outside a pass (an arm's fall or rise, a
 * hold a settle or an arm completes, an entry-now hit at a reset): a value
 * of the detect at t, which every user takes as a sample's value (the
 * semantics, D): in order, each node a changed input reaches runs as in a
 * sample's pass. A node that runs only at frames takes it at its next
 * frame, as any sample's change. */
static void ysig__settle(ysig_graph* g, int from, int64_t t) {
    int k, pos = 0;
    g->nd[from].pass = ++g->pass;
    for (k = 0; k < g->n; k++) if (g->order[k] == from) { pos = k; break; }
    for (k = pos + 1; k < g->n; k++) {
        int nid = g->order[k], j, hit = 0;
        ysig__nd* m = &g->nd[nid];
        if ((m->flags & YSIG__F_NOSRC) || m->op == YSIG_OP_BIND || (m->op == YSIG_OP_RECORD && m->p[0] == 1) ||
            (m->op == YSIG_OP_DETECT && m->p[5] == 1))
            continue;
        for (j = 0; j < m->n_in; j++) if (g->nd[m->in[j]].pass == g->pass) hit = 1;
        if (hit) ysig__run(g, nid, 0, t);
    }
}

/* --- an arm in the past: the window (v0.6.0) ------------------------------- */

/* A node's state that a checkpoint copies: a ring op's slots are not
 * copied (the undo log restores the ones written since). */
static size_t ysig__st_bytes(const ysig__nd* n) {
    switch (n->op) {
    case YSIG_OP_SOURCE: return sizeof(ysig__src_st);
    case YSIG_OP_INTEGRATE: return sizeof(ysig__int_st);
    case YSIG_OP_LOWPASS: return sizeof(ysig__lp_st);
    case YSIG_OP_VELOCITY: case YSIG_OP_SPEED: case YSIG_OP_DELAY: return sizeof(ysig__ring);
    case YSIG_OP_DETECT: return sizeof(ysig__det_st);
    case YSIG_OP_NOISE: return sizeof(ysig__noise_st);
    case YSIG_OP_LATCH: return sizeof(ysig__latch_st);
    case YSIG_OP_PEAK: return sizeof(ysig__peak_st);
    case YSIG_OP_SILENT: return sizeof(ysig__silent_st);
    default: return n->vt ? n->vt->state_size : 0;
    }
}

static char* ysig__put(char* p, const void* src, size_t n) {
    if (n) memcpy(p, src, n);
    return p + ((n + 7u) & ~(size_t)7u);
}
static const char* ysig__get(void* dst, const char* p, size_t n) {
    if (n) memcpy(dst, p, n);
    return p + ((n + 7u) & ~(size_t)7u);
}

/* The graph's state: the handle, the nodes (parameters, ring heads), the
 * values, each node's state, data, and both queues. */
static void ysig__ck_save(ysig_graph* g, char* p) {
    int i;
    p = ysig__put(p, g, sizeof *g);
    p = ysig__put(p, g->nd, sizeof(ysig__nd) * (size_t)g->n);
    p = ysig__put(p, g->val, sizeof(ysig_value) * (size_t)g->n);
    for (i = 0; i < g->n; i++) p = ysig__put(p, g->nd[i].st, ysig__st_bytes(&g->nd[i]));
    for (i = 0; i < g->n; i++) p = ysig__put(p, g->nd[i].data, sizeof(double) * (size_t)g->nd[i].n_data);
    (void)p;
}
static void ysig__ck_load(ysig_graph* g, const char* p) {
    int i, busy = g->busy;
    p = ysig__get(g, p, sizeof *g);   /* the same pointers, the window's too */
    g->busy = busy;                   /* the call running now, not then */
    p = ysig__get(g->nd, p, sizeof(ysig__nd) * (size_t)g->n);
    p = ysig__get(g->val, p, sizeof(ysig_value) * (size_t)g->n);
    for (i = 0; i < g->n; i++) p = ysig__get(g->nd[i].st, p, ysig__st_bytes(&g->nd[i]));
    for (i = 0; i < g->n; i++) p = ysig__get(g->nd[i].data, p, sizeof(double) * (size_t)g->nd[i].n_data);
    (void)p;
}

/* After a frame or a reset: the oldest of K goes, and the logs before the
 * next oldest with it. */
static void ysig__ck_take(ysig_graph* g) {
    ysig__cu* cu = g->cu;
    ysig__ck* k;
    uint64_t call;
    if (!cu) return;
    call = cu->replaying ? cu->c_pos : cu->c_head;
    /* v0.7.0: spaced in input time, catchup_ns / 8 apart (a barrier's
     * always), so the images do not grow with the display's rate */
    if (cu->n_ck && !cu->at_bar && g->t_cover - cu->ck[(cu->ck0 + cu->n_ck - 1) % cu->K].cover < cu->span_ns / 8) return;
    if (cu->n_ck == cu->K) {
        cu->ck0 = (cu->ck0 + 1) % cu->K;
        cu->n_ck--;
        k = &cu->ck[cu->ck0];
        cu->c_tail = k->call;
        cu->u_tail = k->undo;
        cu->h_tail = k->hit;
        cu->d_tail = k->der;
    }
    if (!cu->n_ck) ysig__cu_clear(cu, call);
    k = &cu->ck[(cu->ck0 + cu->n_ck) % cu->K];
    cu->n_ck++;
    if (cu->n_ck == cu->K) cu->full = 0;   /* the window is whole again */
    k->call = call;
    k->undo = cu->u_head;
    k->hit = cu->h_head;
    k->der = cu->d_head;
    k->hseq = cu->next_seq;
    k->dseq = cu->next_dseq;
    k->cover = g->t_cover;
    k->bar = cu->at_bar;
    cu->at_bar = 0;
    k->row_h = cu->row_h;
    ysig__ck_save(g, k->img);
    /* the oldest goes when the next oldest is catchup_ns behind the newest
     * already: the window keeps catchup_ns of input time, not more */
    while (cu->n_ck > 2 && cu->ck[(cu->ck0 + 1) % cu->K].cover != INT64_MIN && g->t_cover - cu->ck[(cu->ck0 + 1) % cu->K].cover >= cu->span_ns) {
        cu->ck0 = (cu->ck0 + 1) % cu->K;
        cu->n_ck--;
        k = &cu->ck[cu->ck0];
        cu->c_tail = k->call;
        cu->u_tail = k->undo;
        cu->h_tail = k->hit;
        cu->d_tail = k->der;
    }
}

static int ysig__input_t(const ysig_entry* e, int64_t* t);
static int ysig__is_state(int kind) {
    return kind == YSIG_J_SET || kind == YSIG_J_SET_DATA || kind == YSIG_J_DISARM || kind == YSIG_J_RECENTER;
}
static int ysig__recenter_node(ysig_graph* g, int role);
static int ysig__log(ysig_graph* g, const ysig_entry* e) {
    ysig__cu* cu = g->cu;
    ysig__call* c;
    int st = ysig__is_state(e->kind);
    if (!cu || cu->replaying) return 0;
    cu->cur = UINT64_MAX;
    if (!st && !cu->placing) cu->grp_open = cu->grp_bad = 0;
    if (!cu->n_ck || cu->c_head - cu->c_tail >= (uint64_t)cu->C) {
        if (cu->n_ck) ysig__cu_full(cu);
        if (st && !cu->placing) { cu->grp_bad = 1; cu->grp_open = 1; }
        return 0;
    }
    if (st && !cu->placing && !cu->grp_open) { cu->grp_open = 1; cu->grp_from = cu->c_head; }
    cu->cur = cu->c_head;
    c = &cu->calls[cu->c_head++ % (uint64_t)cu->C];
    c->e = *e;
    /* its time in the reference order: an input's own, an arm's t, a
     * state call's input time now (its group's, if an arm closes one) */
    if (!ysig__input_t(e, &c->tr)) c->tr = e->kind == YSIG_J_ARM || e->kind == YSIG_J_DISARM_AT ? e->t : g->t_cover;
    /* v0.7.0: a set, set_data or recenter outside a group is at N(F(c)),
     * the newest input that reaches it; a plain disarm at N (the ruling) */
    if (e->kind == YSIG_J_SET || e->kind == YSIG_J_SET_DATA) c->tr = ysig__nf(g, ysig__down(g, e->node));
    if (e->kind == YSIG_J_RECENTER) c->tr = ysig__nf(g, ysig__down(g, ysig__recenter_node(g, e->node)));
    if (e->kind == YSIG_J_DISARM) c->e.t = c->tr;   /* a disarm acts at its time */
    c->lf = 0;
    c->cx = 0;
    c->chh = 0;
    c->srcm = cu->srcm;
    c->srcm_all = cu->srcm_all;
    c->wp = 0;
    c->wnode = 0;
    c->wseq = 0;
    c->gid = 0;
    c->cseq = g->wt->seq;
    /* rule 3's time of the call when it goes in (H9): its key time at its
     * call (v0.7.0: N(F(c)) for a set, set_data, recenter; N for the rest) */
    c->n_at = e->kind == YSIG_J_SET || e->kind == YSIG_J_SET_DATA || e->kind == YSIG_J_RECENTER ? c->tr : g->t_cover;
    if (e->kind == YSIG_J_FRAME) {
        /* the caller's values may change: the channels a node reads */
        int m = e->frame.values ? (e->frame.n_values < cu->nfv ? e->frame.n_values : cu->nfv) : 0;
        if (m > 0) {
            float* q = cu->pool + (size_t)(cu->slot_next++ % (uint64_t)cu->nslot) * (size_t)cu->nfv;
            memcpy(q, e->frame.values, sizeof(float) * (size_t)m);
            c->e.frame.values = q;
        } else {
            c->e.frame.values = NULL;
        }
        c->e.frame.n_values = m;
    }
    return 1;
}

/* A call's input time: a sample's, a frame's onset, a settle's. */
static int ysig__input_t(const ysig_entry* e, int64_t* t) {
    if (e->kind == YSIG_J_FEED) { *t = e->event.t; return 1; }
    if (e->kind == YSIG_J_FRAME) { *t = e->frame.onset_ns; return 1; }
    if (e->kind == YSIG_J_SETTLE) { *t = e->t; return 1; }
    return 0;
}

static void ysig__do_reset(ysig_graph* g, int64_t t, int exp_only) {
    ysig__reset(g, t, exp_only);
    g->t_cover = t;   /* the input time after a reset is its time */
    ysig__key_bar(g, t, 1);
    /* a reset is a barrier: an arm after it is never placed before it */
    if (g->cu) {
        ysig__cu_clear(g->cu, g->cu->replaying ? g->cu->c_pos : g->cu->c_head);
        g->cu->at_bar = 1;
    }
    ysig__ck_take(g);
}

/* An arm with the newest held value: 0.4.1's arm, and the arm placed
 * where no input later than t has come. */
static void ysig__settle_det(ysig_graph* g, int i, int64_t t, int any);
/* An arm at t_ns; ts: the time input is known to (t_ns for an arm in the
 * past, whose later input is fed; the newest input for one ahead of it). */
static void ysig__arm_plain(ysig_graph* g, int node, int64_t t_ns, int64_t ts) {
    ysig__nd* n = &g->nd[node];
    int hit_out = (int)n->p[13] == YSIG_OUT_HIT;
    int fell, rose;
    /* a hold met by the arm's time is the earlier arm's hit, also when no
     * input has found it yet: the re-arm ends that arm after it, and a run
     * again that places the arm before the input that found it keeps it.
     * Any detect: one on a view or at frame rate too (finding 2). Only up
     * to what input supports: an arm ahead of the newest input does not
     * credit unfed time (finding 4) */
    ysig__settle_det(g, node, ts < t_ns ? ts : t_ns, 1);
    ysig__key_note(g, ysig__down(g, node), t_ns, 2);
    fell = hit_out && g->val[node].v[0] != 0;
    rose = ysig__det_arm(g, node, t_ns, &g->val[n->in[0]]) && hit_out;
    if (fell) {
        /* the fall has a time, so a past read after it agrees; its
         * sources are the input's held value's */
        g->val[node].t = t_ns;
        ysig__det_prov(&g->val[node], &g->val[n->in[0]]);
        ysig__hist_push(g, n, &g->val[node]);
        ysig__settle(g, node, t_ns);
    }
    if (rose) {
        /* entry now: the output rises at the arm, after any fall, so
         * its users see a rise now (a latch freezes, a gate restarts) */
        g->val[node].v[0] = 1.0;
        g->val[node].t = t_ns;
        ysig__det_prov(&g->val[node], &g->val[n->in[0]]);   /* the input that met the test */
        ysig__hist_push(g, n, &g->val[node]);
        ysig__settle(g, node, t_ns);
    }
}

/* The arm logged at window index `at` (UINT64_MAX: none) is made now; ts:
 * the time input is known to. In a run again, the new arm's hits start
 * here: the hold met by its time is the earlier arm's; what the detect
 * finds from it on is its own, handed out again even when the earlier
 * arm's equal hit was (that one counts as superseded); a later logged arm
 * of the detect starts its own, which the caller had */
static void ysig__arm_made(ysig_graph* g, int node, int64_t t, int64_t ts, uint64_t at) {
    ysig__cu* cu = g->cu;
    /* v0.7.0, the cancel by key: a disarm made after this arm's call that
     * R puts before it ends what the caller armed it for: not made. Only a
     * run again has R's order: at R's end such a disarm is the later key
     * (the arm is late and moves before it, or with no window late_off) */
    if (cu && cu->replaying && g->nd[node].dseq > g->ko) return;
    if (cu && cu->replaying && node == cu->arm_node) {
        ysig__settle_det(g, node, ts < t ? ts : t, 1);
        cu->arm_live = at == cu->arm_pos;
    }
    ysig__arm_plain(g, node, t, ts);
}

/* An arm at t, logged at window index `at`. At its place input up to t
 * is in (an input later than t exists, or t is the input time), so the
 * earlier arm's hold is met up to t (rule 1 of the arm). param 2: an arm
 * that had no room to wait, made on the input known then (not on unfed
 * time) */
static void ysig__arm_do(ysig_graph* g, int node, int64_t t, uint64_t at, int param) {
    ysig__arm_made(g, node, t, param == 2 && t > g->t_cover ? g->t_cover : t, at);
}

/* A disarm at t ends its detect's arm at t: a hold met by then is that
 * arm's hit first, as at an arm (the exit a hit makes disarms the control
 * at the hit's time) */
static void ysig__do_disarm(ysig_graph* g, int node, int64_t t) {
    ysig__det_st* s = (ysig__det_st*)g->nd[node].st;
    ysig__settle_det(g, node, t, 1);
    s->armed = s->holding = 0;
    ysig__key_note(g, ysig__down(g, node), t, 2);
    if (g->cs > g->nd[node].dseq) g->nd[node].dseq = g->cs;
}

/* A detect's hold met by t, input fed up to t: its hit. ysig_settle()
 * does sample-rate detects only (a frame-rate detect tests at onsets, one
 * on a frame-updated input at its frames); an arm (any) ends every kind's
 * earlier arm at its time. */
static void ysig__settle_det(ysig_graph* g, int i, int64_t t, int any) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s;
    int was;
    if (n->op != YSIG_OP_DETECT || n->kns[0] <= 0) return;
    if (!any && (n->p[5] == 1 || (g->nd[n->in[0]].flags & YSIG__F_FRAME))) return;
    s = (ysig__det_st*)n->st;
    if (!s->armed || !s->holding) return;
    was = s->fired;
    (void)ysig__det_hold_done(g, i, t);
    if ((int)n->p[13] == YSIG_OUT_HIT && s->fired && !was) {
        g->val[i].v[0] = 1.0;
        g->val[i].t = s->t_hit;
        ysig__det_prov(&g->val[i], &s->v_hit);
        ysig__hist_push(g, n, &g->val[i]);
        ysig__settle(g, i, s->t_hit);
    }
}

static void ysig__do_settle(ysig_graph* g, int64_t t) {
    int k;
    for (k = 0; k < g->n; k++) ysig__settle_det(g, g->order[k], t, 0);
    if (t > g->t_cover) g->t_cover = t;
    ysig__key_note(g, NULL, t, 1);
}

static void ysig__do_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns);
static void ysig__do_set(ysig_graph* g, int node, int param, double v);
static int ysig__do_feed(ysig_graph* g, const yin_event* e);
static void ysig__do_flush(ysig_graph* g);

static void ysig__apply(ysig_graph* g, const ysig_entry* e) {
    switch (e->kind) {
    case YSIG_J_RESET: ysig__do_reset(g, e->t, 0); break;
    case YSIG_J_RESET_TRIAL: ysig__do_reset(g, e->t, 1); break;
    case YSIG_J_RECENTER: ysig__do_recenter(g, e->node, e->value, e->y, e->t); break;
    case YSIG_J_SET: ysig__do_set(g, e->node, e->param, e->value); break;
    case YSIG_J_SET_DATA: g->nd[e->node].data[e->param] = e->value; ysig__key_note(g, ysig__down(g, e->node), g->tk, 2); break;
    case YSIG_J_ARM: ysig__arm_do(g, e->node, e->t, g->cu ? g->cu->c_pos - 1 : UINT64_MAX, e->param); break;
    case YSIG_J_DISARM: case YSIG_J_DISARM_AT: ysig__do_disarm(g, e->node, e->t); break;
    case YSIG_J_FEED: (void)ysig__do_feed(g, &e->event); break;
    case YSIG_J_FRAME: ysig__do_frame(g, &e->frame); break;
    case YSIG_J_SETTLE: ysig__do_settle(g, e->t); break;
    case YSIG_J_FLUSH: ysig__do_flush(g); break;
    default: break;
    }
}

/* The detects whose arm a past group ends at its time: its arm's, and
 * those it disarms. Their hits later than that time were the ended arms':
 * a caller acting in time order took none (superseded, not retracted). */
static int ysig__grp_ends(const ysig__cu* cu, int node, int hit_node) {
    int k;
    if (hit_node == node) return 1;
    for (k = 0; k < cu->n_grp_dis; k++) if (cu->grp_dis[k] == hit_node) return 1;
    return 0;
}

/* v0.7.0: a logged call's key (LATE INPUT, "The key of a call"): its
 * time tr, its class, its call order (a group's calls: their arm's) */
static int ysig__kcls(int kind) {
    return kind == YSIG_J_FEED ? 0 : kind == YSIG_J_FRAME || kind == YSIG_J_SETTLE ? 1 : 2;
}
static uint64_t ysig__call_ko(const ysig__call* c) { return c->gid ? c->gid : c->cseq; }
static int ysig__call_gt(const ysig__call* c, int64_t t, int cls, uint64_t ord) {
    int k = ysig__kcls(c->e.kind);
    uint64_t o = ysig__call_ko(c);
    return c->tr > t || (c->tr == t && (k > cls || (k == cls && o > ord)));
}

/* From checkpoint k: its state, the ring slots written since put back,
 * then every call since, the new arm among them. Hits the caller already
 * had are not queued again, but for the armed detect's from the new arm
 * on (arm_live); one the history no longer gives is counted (superseded
 * or retracted). Rows and derived events are not handed out again; a change
 * in them is counted. */
static void ysig__rerun(ysig_graph* g, int kslot, int node, int64_t t) {
    ysig__cu* cu = g->cu;
    ysig__ck* k = &cu->ck[kslot];
    ysig_stats keep = g->st;
    uint64_t s, end = cu->c_head, rh = cu->row_h;
    int n_hit, n_der, q, j, i, stale = 0;
    /* the hits and events since the checkpoint that the caller has: a run
     * again matches them, and counts those it does not give again */
    cu->n_tmp = 0;
    for (s = k->hit; s < cu->h_head; s++) {
        const ysig__hl* l = &cu->hl[s % (uint64_t)cu->C];
        if (l->given) cu->tmp[cu->n_tmp++] = *l;
    }
    cu->n_tmpd = 0;
    cu->d_stale = 0;
    for (s = k->der; cu->DC && s < cu->d_head; s++) {
        const ysig__dl* l = &cu->dl[s % (uint64_t)cu->DC];
        if (l->given) cu->tmpd[cu->n_tmpd++] = *l;
    }
    /* queued, not handed out: those made before the checkpoint stay; the
     * run again makes the rest again (or not), with no count */
    for (q = j = 0; q < g->n_hit; q++)
        if (cu->qseq[q] <= k->hseq) { g->hitq[j] = g->hitq[q]; cu->qseq[j] = cu->qseq[q]; cu->qhl[j] = cu->qhl[q]; j++; }
    n_hit = j;
    /* final-only: the queue as the caller left it; final events the run
     * again hands out go after it, as room allows (H11) */
    if (!cu->derived_final) {
        for (q = j = 0; q < g->n_der; q++)
            if (cu->dseq[q] <= k->dseq) { g->derq[j] = g->derq[q]; cu->dseq[j] = cu->dseq[q]; cu->dqdl[j] = cu->dqdl[q]; j++; }
        g->n_der = j;
    }
    n_der = g->n_der;
    while (cu->u_head > k->undo) {
        const ysig__undo* u = &cu->undo[--cu->u_head % (uint64_t)cu->W];
        *u->at = u->old;
    }
    ysig__ck_load(g, k->img);
    g->n_hit = n_hit;
    g->n_der = n_der;
    /* held rows and events from the checkpoint's call on are made again */
    while (cu->rb && cu->rb_head > cu->rb_tail && cu->rb[(cu->rb_head - 1) % (uint64_t)cu->R].idx >= k->call) cu->rb_head--;
    while (cu->db && cu->db_head > cu->db_tail && cu->db[(cu->db_head - 1) % (uint64_t)cu->R].idx >= k->call) cu->db_head--;
    cu->h_head = k->hit;
    cu->d_head = k->der;
    cu->row_h = k->row_h;
    cu->n_ck = (kslot - cu->ck0 + cu->K) % cu->K + 1;   /* the newer ones were of the old history */
    cu->replaying = 1;
    for (s = k->call; s < end; s++) {
        cu->c_pos = s + 1;
        g->tk = cu->calls[s % (uint64_t)cu->C].tr;
        g->ko = ysig__call_ko(&cu->calls[s % (uint64_t)cu->C]);
        g->cs = cu->calls[s % (uint64_t)cu->C].cseq;
        ysig__apply(g, &cu->calls[s % (uint64_t)cu->C].e);
    }
    cu->replaying = 0;
    /* a hit handed out that the new history does not give. The armed
     * detect's own after the arm's time were its earlier arm's, which a
     * caller re-arming at t took none of (superseded; or an equal one
     * handed out again as the new arm's). Any other the caller may have
     * taken: its own at or before t, and another detect's at any time,
     * since the caller drains them together and cannot tell a voided one
     * from one that stands (retracted, which the caller must watch) */
    for (i = 0; i < cu->n_tmp; i++)
        if (cu->tmp[i].seq != UINT64_MAX) {
            int hn = cu->tmp[i].h.node;
            /* a late input voids it with no caller action (the ruling of
             * 2026-11-15); a late disarm, as any disarm, as in 0.6.x */
            if (cu->late_on && cu->late_in) keep.late_hits_voided++;   /* whatever its time */
            else if (cu->tmp[i].h.t > t && ysig__grp_ends(cu, node, hn)) keep.arm_hits_superseded++;
            else keep.arm_hits_retracted++;
        }
    /* the arms' own counts stand, and the calls made (the window keeps
     * only the events a source took); the rest is the run again's */
    g->st.fed = keep.fed;
    g->st.dropped_order = keep.dropped_order;   /* a dropped feed is not logged */
    g->st.arm_caught_up = keep.arm_caught_up;
    g->st.arm_past_off = keep.arm_past_off;
    g->st.arm_past_far = keep.arm_past_far;
    g->st.arm_past_full = keep.arm_past_full;
    g->st.arm_hits_retracted = keep.arm_hits_retracted;
    g->st.arm_hits_superseded = keep.arm_hits_superseded;
    g->st.arm_reentry = keep.arm_reentry;
    g->st.call_reentry = keep.call_reentry;
    g->st.arm_past_pinned = keep.arm_past_pinned;
    g->st.arm_ahead_cancelled = keep.arm_ahead_cancelled;
    g->st.arm_unverified = keep.arm_unverified;
    g->st.late_far = keep.late_far;
    g->st.late_full = keep.late_full;
    g->st.late_barrier = keep.late_barrier;
    g->st.late_off = keep.late_off;
    g->st.late_unverified = keep.late_unverified;
    g->st.late_hits_voided = keep.late_hits_voided;
    g->st.late_frames = keep.late_frames + cu->n_lf;
    cu->n_lf = 0;
    /* final-only rows (events) were not handed out yet: none is stale */
    g->st.arm_rows_stale = keep.arm_rows_stale + (!cu->rows_final && cu->row_h != rh);
    /* v0.7.0: an event handed out that the new history does not give as
     * it was, at its place in its node's sequence (the new one is queued);
     * not one the caller did not have */
    stale = cu->d_stale;
    for (i = 0; i < cu->n_tmpd; i++) if (cu->tmpd[i].seq != UINT64_MAX) stale = 1;
    g->st.arm_derived_stale = keep.arm_derived_stale + stale;
    cu->n_tmp = 0;
    cu->n_tmpd = 0;
}

/* --- the reference order (docs/sig.md, "An arm in the past: the semantics") */

/* Footprints: the nodes a call can change, as bit sets of dw words. */
static void ysig__fp_or(const ysig_graph* g, uint64_t* f, int node) {
    int w;
    if (node < 0 || node >= g->n) return;
    for (w = 0; w < g->dw; w++) f[w] |= g->down[(size_t)node * (size_t)g->dw + (size_t)w];
}
static int ysig__fp_meets(const ysig_graph* g, const uint64_t* a, const uint64_t* b) {
    int w;
    for (w = 0; w < g->dw; w++) if (a[w] & b[w]) return 1;
    return 0;
}
static void ysig__fp_all(const ysig_graph* g, uint64_t* f) {
    int w;
    for (w = 0; w < g->dw; w++) f[w] = ~0ull;
}
/* A feed: the sources that take it and keep it (an older sample of its
 * device is dropped and changes no node); mask: of the first 64. */
static int ysig__feed_fp(const ysig_graph* g, const yin_event* e, uint64_t* f, uint64_t* mask, int* all) {
    int i, d, n = 0;
    *mask = 0;
    *all = 0;
    for (i = 0; i < g->n_src; i++) {
        const ysig__nd* nd = &g->nd[g->src_ids[i]];
        const ysig__src_st* s = (const ysig__src_st*)nd->st;
        int counts = (int)nd->p[0] == YIN_KIND_MOUSE && (uint32_t)nd->p[1] == YIN_AXIS_DELTA;
        if (!ysig__source_takes(nd, e)) continue;
        for (d = 0; d < s->n && s->dev[d] != e->device; d++) { }
        if (d < s->n && e->t < s->last_t[d] && !counts) continue;
        if (f) ysig__fp_or(g, f, g->src_ids[i]);
        if (i < 64) *mask |= 1ull << i;
        else *all = 1;
        n++;
    }
    return n;
}
/* A role's integrator: what ysig_recenter() moves. */
static int ysig__recenter_node(ysig_graph* g, int role) {
    double xy[2] = { 0, 0 };
    char keep[sizeof g->err];
    int id = -1;
    memcpy(keep, g->err, sizeof keep);
    if (!ysig__recenter_at(g, role, xy, &id)) id = -1;
    memcpy(g->err, keep, sizeof keep);
    return id;
}
/* A call's footprint; 2 for a barrier. */
static int ysig__fp_entry(ysig_graph* g, const ysig__call* c, uint64_t* f) {
    const ysig_entry* e = &c->e;
    int i, w;
    for (w = 0; w < g->dw; w++) f[w] = 0;
    switch (e->kind) {
    case YSIG_J_RESET: case YSIG_J_RESET_TRIAL: case YSIG_J_FLUSH: return 2;
    case YSIG_J_FRAME: case YSIG_J_SETTLE: ysig__fp_all(g, f); return 1;
    case YSIG_J_FEED:
        for (i = 0; i < g->n_src; i++)
            if ((i < 64 && (c->srcm >> i) & 1u) || (i >= 64 && c->srcm_all && ysig__source_takes(&g->nd[g->src_ids[i]], &e->event)))
                ysig__fp_or(g, f, g->src_ids[i]);
        return 1;
    case YSIG_J_RECENTER: ysig__fp_or(g, f, ysig__recenter_node(g, e->node)); return 1;
    default: ysig__fp_or(g, f, e->node); return 1;
    }
}

/* A logged state call's time: its group's (a disarm acts at it) */
static void ysig__set_tr(ysig__call* c, int64_t t) {
    c->tr = t;
    if (c->e.kind == YSIG_J_DISARM) c->e.t = t;
}


static void ysig__wait_fp(ysig_graph* g, const ysig__wa* a, uint64_t* f);
static int ysig__input_t(const ysig_entry* e, int64_t* t);
/* Exactness (docs/sig.md, "Exactness"): the arms whose group is outside
 * the shapes whose result is verified, each counted once. */
static void ysig__unv(ysig_graph* g, uint64_t gid) {
    ysig__wt* w = g->wt;
    int i;
    if (!gid) return;
    for (i = 0; i < w->n_unv; i++) if (w->unv[i] == gid) return;
    w->unv[w->unv_at] = gid;
    w->unv_at = (w->unv_at + 1) % 64;
    if (w->n_unv < 64) w->n_unv++;
    g->st.arm_unverified++;
}
static int ysig__fp_any(const ysig_graph* g, const uint64_t* f) {
    int w;
    for (w = 0; w < g->dw; w++) if (f[w]) return 1;
    return 0;
}
/* Condition 3: the newest arm of each detect in R, with its group's
 * footprint; an input earlier than its time that meets it, later in R, is
 * a late report (one at its time is not: the frame at an onset that the
 * arms at that onset came before) */
static void ysig__ux_arm(ysig_graph* g, int node, int64_t t, uint64_t gid, const uint64_t* F) {
    ysig__wt* w = g->wt;
    int i;
    for (i = 0; i < w->n_lr && w->lr[i].node != node; i++) { }
    if (i == w->n_lr) {
        if (w->n_lr == YSIG__UX) {
            /* no room: one goes, its late reports not seen (counted). Slots
             * keep their footprint storage: entries move by swapping */
            ysig__unv(g, w->lr[0].gid);
            i = 0;
        } else {
            i = w->n_lr++;
        }
    }
    w->lr[i].node = node;
    w->lr[i].t = t;
    w->lr[i].gid = gid;
    w->lr[i].l_seq = 0;
    memcpy(w->lr[i].F, F, sizeof(uint64_t) * (size_t)g->dw);
}
static void ysig__ux_disarm(ysig_graph* g, int node) {
    ysig__wt* w = g->wt;
    int i;
    for (i = 0; i < w->n_lr; i++)
        if (w->lr[i].node == node) {
            ysig__ux x = w->lr[i];
            w->lr[i] = w->lr[w->n_lr - 1];
            w->lr[--w->n_lr] = x;
            return;
        }
}
static void ysig__ux_clear(ysig_graph* g) {
    g->wt->n_lr = 0;
    g->wt->n_op = 0;
}
/* A logged call's footprint for the state-call tests (a recenter: its
 * integrator's) */
static void ysig__ent_fp(ysig_graph* g, int kind, int node, uint64_t* f) {
    int w;
    for (w = 0; w < g->dw; w++) f[w] = 0;
    ysig__fp_or(g, f, kind == YSIG_J_RECENTER ? ysig__recenter_node(g, node) : node);
}

/* The cascade a trigger lets in (rule 3), at the end of the log: the
 * groups and their calls in the order they go into R, tested when the
 * trigger is done (conditions 1 and 2) */
enum { YSIG__CAS_NONE, YSIG__CAS_INPUT, YSIG__CAS_STATE, YSIG__CAS_ARM, YSIG__CAS_BARRIER };
static void ysig__cas_begin(ysig_graph* g, int kind, int64_t t, const uint64_t* f) {
    ysig__wt* w = g->wt;
    int k;
    w->cas_kind = kind;
    w->cas_t = t;
    w->cas_all = !f;
    for (k = 0; k < g->dw; k++) w->cas_f[k] = f ? f[k] : ~0ull;
    w->n_cg = w->n_cc = w->cas_top = 0;
}
static int ysig__cas_group(ysig_graph* g, const ysig__wa* a, const ysig_entry* calls, int nc, int with_arm) {
    ysig__wt* w = g->wt;
    ysig__ucg* c;
    int i;
    if (!w->cas_kind) return -1;
    if (w->n_cg == YSIG__UCG) { ysig__unv(g, a->seq); return -1; }
    c = &w->cg[w->n_cg];
    c->gid = a->seq;
    c->t = a->t;
    c->n_at = g->t_cover;
    ysig__wait_fp(g, a, c->F);
    for (i = 0; i < g->dw; i++) c->Fa[i] = 0;
    c->cancelled = !(with_arm && a->node >= 0);
    if (!c->cancelled) ysig__fp_or(g, c->Fa, a->node);
    c->dis_only = 1;
    for (i = 0; i < nc; i++) {
        if (calls[i].kind == YSIG_J_DISARM) ysig__fp_or(g, c->Fa, calls[i].node);
        else c->dis_only = 0;
    }
    /* condition 1: let in between another group's calls */
    if (w->cas_top > 0) {
        const ysig__ucg* p = &w->cg[w->cas_stack[w->cas_top - 1]];
        c->inside = p->placed > 0 && p->placed < p->ncalls + !p->cancelled;
    } else {
        c->inside = 0;
    }
    c->ncalls = nc;
    c->placed = 0;
    c->first = c->last = w->n_cc;
    if (w->cas_top < YSIG__UCG) w->cas_stack[w->cas_top++] = w->n_cg;
    return w->n_cg++;
}
static void ysig__cas_call(ysig_graph* g, int ci, uint64_t gid, int64_t t, int kind, int node) {
    ysig__wt* w = g->wt;
    if (ci < 0) return;
    if (w->n_cc == YSIG__UCC) { ysig__unv(g, gid); return; }
    w->cc[w->n_cc].gid = gid;
    w->cc[w->n_cc].t = t;
    w->cc[w->n_cc].kind = kind;
    w->cc[w->n_cc].node = node;
    w->n_cc++;
    w->cg[ci].placed++;
    w->cg[ci].last = w->n_cc - 1;
}
static void ysig__cas_pop(ysig_graph* g, int ci) {
    if (ci >= 0 && g->wt->cas_top > 0) g->wt->cas_top--;
}
static void ysig__op_add(ysig_graph* g, uint64_t gid, int64_t t, const uint64_t* F, uint64_t l_seq, int node);
/* The trigger is done: each group placed tested against conditions 1 and
 * 2; L is the trigger when it is an input that meets the group and is
 * later than its time (it let it in, or let in the arm that did) */
static void ysig__cas_end(ysig_graph* g, uint64_t l_seq) {
    ysig__wt* w = g->wt;
    int i, j, k;
    for (i = 0; i < w->n_cg; i++) {
        ysig__ucg* c = &w->cg[i];
        int bad = c->inside || w->cas_in;
        if (c->cancelled && !c->ncalls) continue;   /* nothing of it goes into R */
        if (!bad && !c->cancelled) {
            /* condition 4: a disarm of another group, or of no group, in
             * the gap G's arm closes (back to the last arm or input) */
            for (j = c->last - 1; j >= 0; j--) {
                if (w->cc[j].gid == c->gid) continue;
                if (w->cc[j].kind == YSIG_J_ARM) break;
                if (w->cc[j].kind == YSIG_J_DISARM) { bad = 1; break; }
            }
            if (j < 0 && w->cas_dis) bad = 1;
        }
        /* 2 (c): a cancelled group with a disarm, unless N(G) is its time */
        if (c->cancelled && ysig__fp_any(g, c->Fa) && c->n_at != c->t) bad = 1;
        if (!bad && c->n_at < c->t) {
            if (w->cas_kind != YSIG__CAS_INPUT || w->cas_t <= c->t || c->cancelled ||
                (!w->cas_all && (!ysig__fp_meets(g, w->cas_f, c->F) || !ysig__fp_meets(g, w->cas_f, c->Fa))) || !ysig__fp_any(g, c->Fa)) bad = 1;
            /* (b): a later group's state call that meets G, but one of a
             * group of an arm and disarms only, later, that waits at its
             * place, and that the input lets in too (no input between) */
            for (j = c->last + 1; !bad && j < w->n_cc; j++) {
                const ysig__ucc* x = &w->cc[j];
                uint64_t fx[YSIG__FPW];
                if (x->gid == c->gid) continue;
                ysig__ent_fp(g, x->kind, x->node, fx);
                if (!ysig__fp_meets(g, fx, c->F)) continue;
                for (k = 0; k < w->n_cg && w->cg[k].gid != x->gid; k++) { }
                if ((x->kind == YSIG_J_ARM || x->kind == YSIG_J_DISARM) && k < w->n_cg && !w->cg[k].cancelled && w->cg[k].dis_only &&
                    (w->cg[k].t > c->t || (w->cg[k].t == c->t && x->gid > c->gid)) && w->cg[k].n_at < w->cg[k].t &&
                    (w->cas_all || ysig__fp_meets(g, w->cas_f, w->cg[k].F)) && w->cas_t > w->cg[k].t) continue;
                bad = 1;
            }
            if (!bad && g->cu && l_seq) {
                int an = -1;
                for (j = 0; j < w->n_cc; j++) if (w->cc[j].gid == c->gid && w->cc[j].kind == YSIG_J_ARM) an = w->cc[j].node;
                ysig__op_add(g, c->gid, c->t, c->F, l_seq, an);
            }
        }
        if (bad) ysig__unv(g, c->gid);
    }
    /* after the cascade: a let-in disarm after its last arm is in the gap */
    for (j = w->n_cc - 1; j >= 0; j--) {
        if (w->cc[j].kind == YSIG_J_ARM) { w->gap4_wp = 0; break; }
        if (w->cc[j].kind == YSIG_J_DISARM) { w->gap4_wp = 1; break; }
    }
    /* a state call's let-ins after the gap's own calls: inside the group
     * an arm makes of the gap (condition 1), decided when the gap ends */
    if (w->cas_kind == YSIG__CAS_STATE && w->gap_n > 0)
        for (i = 0; i < w->n_cg; i++)
            if (!(w->cg[i].cancelled && !w->cg[i].ncalls) && w->n_pin < 8) w->pin[w->n_pin++] = w->cg[i].gid;
    w->cas_dis = w->cas_in = 0;
    w->cas_kind = YSIG__CAS_NONE;
    w->n_cg = w->n_cc = w->cas_top = 0;
}

/* Condition 2 in the log (a window): a group placed with N(G) < t is open
 * until its L; a later group placed between them whose state call meets
 * it makes it unverified (2 (b)) */
static void ysig__op_add(ysig_graph* g, uint64_t gid, int64_t t, const uint64_t* F, uint64_t l_seq, int node) {
    ysig__wt* w = g->wt;
    int i;
    if (!w->cap_op) return;
    if (w->n_op == w->cap_op && g->cu) {
        /* full: those whose L the log no longer holds go (no group can be
         * placed before L any more) */
        ysig__cu* cu = g->cu;
        for (i = 0; i < w->n_op; ) {
            uint64_t q;
            for (q = cu->c_tail; q < cu->c_head && cu->calls[q % (uint64_t)cu->C].cseq != w->op[i].l_seq; q++) { }
            if (q == cu->c_head && w->op[i].l_seq) {
                ysig__ux x = w->op[i];
                w->op[i] = w->op[w->n_op - 1];
                w->op[--w->n_op] = x;
            } else {
                i++;
            }
        }
    }
    if (w->n_op == w->cap_op) {
        /* still no room: one is no longer watched (counted) */
        ysig__unv(g, w->op[0].gid);
        i = 0;
    } else {
        i = w->n_op++;
    }
    w->op[i].node = node;   /* its arm's detect (2 (b): what its arm lets in) */
    w->op[i].t = t;
    w->op[i].gid = gid;
    w->op[i].l_seq = l_seq;
    memcpy(w->op[i].F, F, sizeof(uint64_t) * (size_t)g->dw);
}

/* Waiting arms (rule 3). */
static void ysig__wait_drop(ysig__wt* w, int k) {
    int c0 = w->a[k].c0, nc = w->a[k].nc, i;
    if (nc) {
        memmove(&w->calls[c0], &w->calls[c0 + nc], sizeof(ysig_entry) * (size_t)(w->nc - c0 - nc));
        memmove(&w->call_seq[c0], &w->call_seq[c0 + nc], sizeof(uint64_t) * (size_t)(w->nc - c0 - nc));
        memmove(&w->call_n[c0], &w->call_n[c0 + nc], sizeof(int64_t) * (size_t)(w->nc - c0 - nc));
        w->nc -= nc;
        for (i = 0; i < w->n; i++) if (w->a[i].c0 > c0) w->a[i].c0 -= nc;
    }
    memmove(&w->a[k], &w->a[k + 1], sizeof(ysig__wa) * (size_t)(w->n - k - 1));
    w->n--;
}
/* A disarm of `node` at time t, call `seq`: it cancels the waiting arms
 * of node at or after its time, made before it (one with a later time
 * goes after the arm: the arm went in before it, rule 3). */
static void ysig__wait_cancel(ysig_graph* g, int node, int64_t t, uint64_t seq) {
    ysig__wt* w = g->wt;
    int k;
    for (k = 0; k < w->n; k++)
        if (w->a[k].node == node && w->a[k].t > t && w->a[k].seq < seq) {
            w->a[k].node = -1 - node;   /* its group's other calls still go in */
            g->st.arm_ahead_cancelled++;
        }
}
/* The open gap's disarms go into R at time t (their group's, or the
 * input time at the gap's end): they cancel, and (made 1) take effect now
 * (S1: a disarm takes effect at the end of its gap). made 0: a run again
 * made them from the log already. */
static void ysig__do_disarm(ysig_graph* g, int node, int64_t t);
static void ysig__gap_disarms(ysig_graph* g, int64_t t, int made) {
    ysig__wt* w = g->wt;
    int k;
    for (k = 0; k < w->n_dis; k++) {
        g->cs = w->dis_seq[k];
        if (made) ysig__do_disarm(g, w->dis_node[k], t);
        ysig__wait_cancel(g, w->dis_node[k], t, w->dis_seq[k]);
        ysig__ux_disarm(g, w->dis_node[k]);
    }
    w->n_dis = 0;
    w->gap_n = 0;
    memset(w->gap_f, 0, sizeof w->gap_f);
    w->fired = 0;
}
static void ysig__apply(ysig_graph* g, const ysig_entry* e);
static void ysig__arm_made(ysig_graph* g, int node, int64_t t, int64_t ts, uint64_t at);
/* Waiting arm k goes into the reference order now, at the end: its
 * group's calls at its time, then the arm (with_arm 0: cancelled). */
static void ysig__wait_fire(ysig_graph* g, int64_t t, const uint64_t* f, uint64_t incl);
static void ysig__state_fp(ysig_graph* g, const ysig_entry* e, uint64_t* f);
/* Waiting arm k goes into the reference order now, at the end: its
 * group's calls at its time, then the arm (with_arm 0: cancelled). A call
 * of it first lets in what it lets in at its own call's input time (rule
 * 3: its trigger takes effect when it goes into R, H9). In an open gap the
 * logged calls are marked (wp): they are not that gap's group. */
/* v0.7.0, T1's closure: the waiting group with call number seq goes in
 * now; first, right before it, every waiting group that meets it with a
 * smaller key (time, call order), smallest first, each with its own
 * closure. Returns its index again (the list changed), or -1 when one
 * of them let it in already. */
static int ysig__wait_closure(ysig_graph* g, uint64_t seq, int blk, int* nb);
static int ysig__late_place(ysig_graph* g, uint64_t a, uint64_t b, uint64_t lo, int64_t t, int cls, uint64_t ord);
static void ysig__late_nolog(ysig_graph* g);
static void ysig__c4(ysig_graph* g, uint64_t gid, int64_t t);
static void ysig__wait_place(ysig_graph* g, int k, int with_arm) {
    ysig__wt* w;
    ysig__wa a;
    uint64_t fl[YSIG__FPW], h0 = g->cu ? g->cu->c_head : 0;
    int late, all_logged = 1;
    if (with_arm && (k = ysig__wait_closure(g, g->wt->a[k].seq, 0, NULL)) < 0) return;
    w = g->wt;
    a = w->a[k];
    /* W1: R already holds a call that meets the group with a greater key
     * (a state call at its time made after its arm): a late call */
    ysig__wait_fp(g, &a, fl);
    late = with_arm && ysig__is_late(g, fl, a.t, 2, a.seq);
    ysig_entry calls[YSIG__WAIT_CALLS];
    uint64_t cseq[YSIG__WAIT_CALLS];
    int64_t cn[YSIG__WAIT_CALLS];
    int i, nc = a.nc;
    int ci;
    for (i = 0; i < nc; i++) { calls[i] = w->calls[a.c0 + i]; cseq[i] = w->call_seq[a.c0 + i]; cn[i] = w->call_n[a.c0 + i]; }
    ci = ysig__cas_group(g, &a, calls, nc, with_arm);
    ysig__wait_drop(w, k);
    for (i = 0; i < nc; i++) {
        ysig_entry e = calls[i];
        uint64_t f[YSIG__FPW];
        ysig__state_fp(g, &e, f);
        ysig__wait_fire(g, cn[i], f, 0);
        if (e.kind == YSIG_J_DISARM) e.t = a.t;
        ysig__cas_call(g, ci, a.seq, a.t, e.kind, e.node);
        if (g->cu) {
            g->cu->srcm = 0;
            g->cu->srcm_all = 0;
            g->cu->placing++;
            if (!ysig__log(g, &e)) all_logged = 0;
            else {
                ysig__call* c = &g->cu->calls[(g->cu->c_head - 1) % (uint64_t)g->cu->C];
                ysig__set_tr(c, a.t);
                c->wp = 1; c->wnode = a.node; c->wseq = a.seq; c->cseq = cseq[i]; c->n_at = cn[i]; c->gid = a.seq;
            }
            g->cu->placing--;
        }
        g->tk = a.t;
        g->ko = a.seq;
        g->cs = cseq[i];
        ysig__apply(g, &e);
        if (e.kind == YSIG_J_DISARM) ysig__wait_cancel(g, e.node, a.t, cseq[i]);
    }
    if (with_arm && a.node >= 0) {
        ysig_entry e;
        int logged = 0;
        uint64_t f[YSIG__FPW];
        int w2;
        /* the arm lets in at its time now, as it goes in (H12, S3): an
         * equal time's earlier call before it, a later one after it (H16) */
        for (w2 = 0; w2 < g->dw; w2++) f[w2] = 0;
        ysig__fp_or(g, f, a.node);
        ysig__wait_fire(g, a.t, f, a.seq);
        memset(&e, 0, sizeof e);
        e.kind = YSIG_J_ARM;
        e.node = a.node;
        e.t = a.t;
        if (g->cu) {
            g->cu->placing++;
            logged = ysig__log(g, &e);
            g->cu->placing--;
        }
        if (logged) {
            ysig__call* c = &g->cu->calls[(g->cu->c_head - 1) % (uint64_t)g->cu->C];
            c->wp = 1; c->wnode = a.node; c->wseq = a.seq; c->gid = a.seq;
        }
        ysig__cas_call(g, ci, a.seq, a.t, YSIG_J_ARM, a.node);
        g->ko = a.seq;
        ysig__arm_made(g, a.node, a.t, a.t, logged ? g->cu->c_head - 1 : UINT64_MAX);
        if (g->cu && !g->cu->replaying) ysig__c4(g, a.seq, a.t);
        if (g->t_cover < a.t) {
            /* waits at its place: a late report counts (condition 3) */
            uint64_t F[YSIG__FPW];
            ysig__wait_fp(g, &a, F);
            ysig__ux_arm(g, a.node, a.t, a.seq, F);
        }
        ysig__cas_pop(g, ci);
        ci = -1;
        if (late) {
            /* to its place (with the groups let in before it here), the
             * run again at the API call's end */
            if (g->cu && logged && all_logged && !g->cu->replaying) (void)ysig__late_place(g, h0, g->cu->c_head, g->cu->c_tail, a.t, 2, a.seq);
            else ysig__late_nolog(g);
        }
        ysig__wait_fire(g, a.t, f, UINT64_MAX);
    } else if (a.node >= 0) {
        g->st.arm_ahead_cancelled++;
    }
    ysig__cas_pop(g, ci);
    w->fired = 1;
}
static void ysig__wait_fp(ysig_graph* g, const ysig__wa* a, uint64_t* f) {
    int i, w;
    for (w = 0; w < g->dw; w++) f[w] = 0;
    /* a cancelled arm's group: its other calls' footprints only */
    if (a->node >= 0) ysig__fp_or(g, f, a->node);
    for (w = 0; w < g->dw; w++) f[w] |= a->xf[w];
    for (i = 0; i < a->nc; i++) {
        const ysig_entry* e = &g->wt->calls[a->c0 + i];
        ysig__fp_or(g, f, e->kind == YSIG_J_RECENTER ? ysig__recenter_node(g, e->node) : e->node);
    }
}
/* Before an input call at t with footprint f (NULL: every node): the
 * open gap's disarms are made at the input time now, and every waiting
 * arm at an earlier time whose footprint it meets goes in first, by
 * (time, call order). */
/* Rule 3: the waiting groups a call at time t with footprint f (NULL:
 * every node) lets in: earlier than t (incl above 0: an arm call, S3, at
 * or before t; at t only one made before call incl, H16: rule 2 puts an
 * equal time's later call after the arm), meeting f, by (time, call
 * order). */
static void ysig__state_fp(ysig_graph* g, const ysig_entry* e, uint64_t* f) {
    int w;
    for (w = 0; w < g->dw; w++) f[w] = 0;
    ysig__fp_or(g, f, e->kind == YSIG_J_RECENTER ? ysig__recenter_node(g, e->node) : e->node);
}
static void ysig__wait_fire(ysig_graph* g, int64_t t, const uint64_t* f, uint64_t incl) {
    ysig__wt* w = g->wt;
    for (;;) {
        int k, best = -1;
        for (k = 0; k < w->n; k++) {
            uint64_t fa[YSIG__FPW];
            if (incl ? w->a[k].t > t || (w->a[k].t == t && w->a[k].seq > incl) : w->a[k].t >= t) continue;
            if (f) {
                ysig__wait_fp(g, &w->a[k], fa);
                if (!ysig__fp_meets(g, fa, f)) continue;
            }
            if (best < 0 || w->a[k].t < w->a[best].t || (w->a[k].t == w->a[best].t && w->a[k].seq < w->a[best].seq)) best = k;
        }
        if (best < 0) break;
        ysig__wait_place(g, best, 1);
    }
}
/* The open gap's disarms take effect at its end (S1): in the log they go
 * after the gap's other calls, so a run again makes them there too (the
 * reviewer's 2192: a recenter after a disarm in one group; the run again
 * met the hold before the recenter) */
static void ysig__dis_last(ysig_graph* g) {
    ysig__cu* cu = g->cu;
    uint64_t s, wr, from;
    ysig__call* tmp;
    int n = 0, i;
    if (!cu || cu->replaying || !cu->grp_open || cu->grp_bad || !g->wt->n_dis) return;
    from = cu->grp_from;
    tmp = cu->blk;
    for (s = from, wr = from; s < cu->c_head; s++) {
        ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (c->e.kind == YSIG_J_DISARM && !c->wp) { if (n < YSIG__BLK) tmp[n++] = *c; continue; }
        if (wr != s) cu->calls[wr % (uint64_t)cu->C] = *c;
        wr++;
    }
    for (i = 0; i < n; i++) cu->calls[(wr + (uint64_t)i) % (uint64_t)cu->C] = tmp[i];
}
/* v0.7.0 (T3): the open gap ends in no group. Each of its state calls is
 * in R at its own key (a set, set_data or recenter at N(F(c)) at its call,
 * a plain disarm at N): one that a call already in R must follow is a
 * late call, placed before it (the run again at the API call's end) */
static int ysig__late_place(ysig_graph* g, uint64_t a, uint64_t b, uint64_t lo, int64_t t, int cls, uint64_t ord);
static void ysig__gap_late(ysig_graph* g) {
    ysig__cu* cu = g->cu;
    uint64_t seqs[YSIG__WAIT_CALLS], from, s;
    int n = 0, i;
    if (!cu || cu->replaying || !cu->grp_open || cu->grp_bad) return;
    from = cu->grp_from;
    for (s = from; s < cu->c_head && n < YSIG__WAIT_CALLS; s++)
        if (!cu->calls[s % (uint64_t)cu->C].wp) seqs[n++] = cu->calls[s % (uint64_t)cu->C].cseq;
    for (i = 0; i < n; i++) {
        const ysig__call* c;
        for (s = from; s < cu->c_head && cu->calls[s % (uint64_t)cu->C].cseq != seqs[i]; s++) { }
        if (s == cu->c_head) continue;
        c = &cu->calls[s % (uint64_t)cu->C];
        /* the calls before the gap; the gap's own earlier ones are placed */
        (void)ysig__late_place(g, s, s + 1, cu->c_tail, c->tr, 2, ysig__call_ko(c));
        /* a placed call can move the gap's start down */
        for (s = cu->c_tail; s < cu->c_head && cu->calls[s % (uint64_t)cu->C].cseq != seqs[0]; s++) { }
        if (s < from) from = s;
    }
}
static void ysig__before_input(ysig_graph* g, int64_t t, const uint64_t* f) {
    ysig__wt* w = g->wt;
    /* the gap ends with no arm: its disarms are in no group, its let-ins
     * in no group's block (condition 4 below; condition 1 not) */
    int dis = w->n_dis > 0 || w->gap4_wp;
    w->n_pin = 0;
    ysig__dis_last(g);
    ysig__gap_disarms(g, g->t_cover, 1);
    ysig__gap_late(g);
    /* v0.7.0: a late report of an arm in R (0.6.x's condition 3) goes to
     * its place by key now (ysig_feed()), or falls back counted */
    if (w->n) {
        ysig__cas_begin(g, YSIG__CAS_INPUT, t, f);
        w->cas_dis = dis;
        ysig__wait_fire(g, t, f, 0);
        ysig__cas_end(g, g->wt->seq);   /* L: this input, logged next */
    }
    w->gap4_wp = 0;   /* the input ends the gap */
}
/* Before a state call (set, set_data, disarm, recenter): it is at the input
 * time now, so a waiting group at an earlier time whose footprint its own
 * meets goes in first (rule 3: a disarm later than the arm goes after it) */
static void ysig__before_state(ysig_graph* g, int node, int64_t t) {
    uint64_t f[YSIG__FPW];
    int w;
    if (!g->wt->n || node < 0) return;
    /* the gap opens at this call: what it lets in goes into the gap (an
     * ahead group takes it back with the call, H9) */
    if (g->cu && !g->cu->grp_open && !g->cu->replaying && g->cu->n_ck) {
        g->cu->grp_open = 1;
        g->cu->grp_from = g->cu->c_head;
    }
    for (w = 0; w < g->dw; w++) f[w] = 0;
    ysig__fp_or(g, f, node);
    /* v0.7.0: at the call's key time (N(F(c)); a plain disarm's N) */
    ysig__cas_begin(g, YSIG__CAS_STATE, t, f);
    g->wt->cas_dis = g->wt->n_dis > 0 || g->wt->gap4_wp;
    ysig__wait_fire(g, t, f, 0);
    ysig__cas_end(g, 0);
}
/* Before a barrier: every waiting arm is cancelled; its group's other
 * calls go in first. */
static void ysig__before_barrier(ysig_graph* g) {
    ysig__wt* w = g->wt;
    int dis = w->n_dis > 0 || w->gap4_wp;
    w->n_pin = 0;
    ysig__dis_last(g);
    ysig__gap_disarms(g, g->t_cover, 1);
    ysig__gap_late(g);
    ysig__late_run(g);   /* now: the barrier clears the log */
    if (w->n) { ysig__cas_begin(g, YSIG__CAS_BARRIER, g->t_cover, NULL); w->cas_dis = dis; }
    while (w->n) {
        int k, best = 0;
        for (k = 1; k < w->n; k++)
            if (w->a[k].t < w->a[best].t || (w->a[k].t == w->a[best].t && w->a[k].seq < w->a[best].seq)) best = k;
        ysig__wait_place(g, best, 0);
    }
    if (w->cas_kind) ysig__cas_end(g, 0);
    w->gap4_wp = 0;
    ysig__ux_clear(g);   /* a barrier: no late report of an arm before it */
}

/* The newest checkpoint at or before log index pos (at the log's start
 * only if no input before the oldest checkpoint is later than t), or -1. */
static int ysig__ck_at(const ysig__cu* cu, uint64_t pos, int64_t t) {
    int i;
    for (i = cu->n_ck - 1; i >= 0; i--) {
        const ysig__ck* k = &cu->ck[(cu->ck0 + i) % cu->K];
        if (k->call <= pos && (pos > cu->c_tail || k->cover <= t || k->bar)) return (cu->ck0 + i) % cu->K;
    }
    return -1;
}

/* v0.7.0: a call of key (t, cls, ord) with footprint F must go before a
 * call older than checkpoint k (R holds, before k, a call that meets F
 * with a greater key): the image's nodes keep those keys */
static int ysig__ck_late(const ysig_graph* g, int kslot, const uint64_t* F, int64_t t, int cls, uint64_t ord) {
    const ysig__ck* k = &g->cu->ck[kslot];
    const ysig__nd* nd = (const ysig__nd*)(const void*)(k->img + ((sizeof(ysig_graph) + 7u) & ~(size_t)7u));
    int i;
    if (k->bar) return 0;
    for (i = 0; i < g->n; i++) {
        const ysig__nd* n = &nd[i];
        if (F && !((F[i >> 6] >> (i & 63)) & 1u)) continue;
        if (n->kt > t || (n->kt == t && (n->kc > cls || (n->kc == cls && n->ko > ord)))) return 1;
    }
    return 0;
}

/* The unit goes to its place: the calls from p0 that must follow it
 * (they reach F with a greater key, or follow one that moves and share a
 * node with it: the reviewer's L2) move after it, in their order; the
 * rest stay before it. A group moves whole. */
static uint64_t ysig__late_move_k(ysig_graph* g, uint64_t a, uint64_t p0, const uint64_t* F, int64_t t, int cls, uint64_t ord) {
    ysig__cu* cu = g->cu;
    const uint64_t C = (uint64_t)cu->C;
    int w, any = 0;
    uint64_t Mf[YSIG__FPW], fu[YSIG__FPW], f[YSIG__FPW], s = p0, wr = p0, q, nm = 0, i, at;
    for (w = 0; w < g->dw; w++) Mf[w] = 0;
    while (s < a) {
        const ysig__call* c = &cu->calls[s % C];
        uint64_t u1 = s + 1;
        int move;
        (void)ysig__fp_entry(g, c, fu);
        if (c->gid)
            for (; u1 < a && cu->calls[u1 % C].gid == c->gid; u1++) {
                (void)ysig__fp_entry(g, &cu->calls[u1 % C], f);
                for (w = 0; w < g->dw; w++) fu[w] |= f[w];
            }
        move = (ysig__call_gt(c, t, cls, ord) && ysig__fp_meets(g, fu, F)) || (any && ysig__fp_meets(g, fu, Mf));
        if (move) {
            for (q = s; q < u1; q++) cu->mv[nm++] = cu->calls[q % C];
            for (w = 0; w < g->dw; w++) Mf[w] |= fu[w];
            any = 1;
        } else {
            for (q = s; q < u1; q++, wr++) if (wr != q) cu->calls[wr % C] = cu->calls[q % C];
        }
        s = u1;
    }
    at = wr;
    for (q = a; q < cu->c_head; q++, wr++) cu->calls[wr % C] = cu->calls[q % C];
    for (i = 0; i < nm; i++, wr++) cu->calls[wr % C] = cu->mv[i];
    return at;
}
static void ysig__rerun(ysig_graph* g, int kslot, int node, int64_t t);
static void ysig__cx_count(ysig_graph* g, uint64_t a, int node, uint64_t dseq);
/* A run again for the late calls placed since the last (one for a batch,
 * or for the calls one API call placed): from the newest checkpoint at or
 * before the earliest place */
static void ysig__late_run(ysig_graph* g) {
    ysig__cu* cu = g->cu;
    int k;
    if (!cu || cu->def_k < 0 || cu->defer) return;
    cu->def_k = -1;
    k = ysig__ck_at(cu, cu->def_p, INT64_MAX);
    if (k < 0) {
        /* the logs ran full since the call was placed */
        g->st.late_full++;
        return;
    }
    cu->late_on = 1;
    cu->arm_node = -1;
    cu->arm_live = 0;
    cu->arm_pos = UINT64_MAX;
    ysig__rerun(g, k, -1, cu->late_t);
    cu->late_on = cu->late_in = 0;
    cu->n_grp_dis = 0;
}
/* v0.7.0: the calls of R a disarm of node with call number dseq cancels,
 * from log index a on (an arm of node made before it, after it in R):
 * each counted once (arm_ahead_cancelled), the run again does not make
 * them (ysig__arm_made()) */
static void ysig__cx_count(ysig_graph* g, uint64_t a, int node, uint64_t dseq) {
    ysig__cu* cu = g->cu;
    uint64_t s;
    for (s = a; s < cu->c_head; s++) {
        ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (c->e.kind == YSIG_J_ARM && c->e.node == node && ysig__call_ko(c) < dseq && !c->cx) {
            c->cx = 1;
            g->st.arm_ahead_cancelled++;
        }
    }
}
/* v0.7.0: the unit [a, b) of the log (a call, or a group's calls, which
 * share one key t, cls, ord), made at the end of R, goes to its place: the
 * calls logged before it (from lo on) that must follow it move after it
 * (L2), and the window runs again (now, or at the API call's end: def_*).
 * A call before the window that must follow it: late_far; the logs or the
 * block full: late_full. 1: placed (or nothing to move). */
static void ysig__late_unv(ysig_graph* g, uint64_t at, uint64_t n, const uint64_t* F, int64_t t, int cls);
static int ysig__late_place(ysig_graph* g, uint64_t a, uint64_t b, uint64_t lo, int64_t t, int cls, uint64_t ord) {
    ysig__cu* cu = g->cu;
    const uint64_t C = (uint64_t)cu->C;
    uint64_t F[YSIG__FPW], f[YSIG__FPW], s, p0 = UINT64_MAX, n = b - a, i, at;
    int kslot, w;
    if (lo < cu->c_tail) lo = cu->c_tail;
    for (w = 0; w < g->dw; w++) F[w] = 0;
    for (s = a; s < b; s++) {
        (void)ysig__fp_entry(g, &cu->calls[s % C], f);
        for (w = 0; w < g->dw; w++) F[w] |= f[w];
    }
    if (!cu->n_ck || ysig__ck_late(g, cu->ck0, F, t, cls, ord)) { g->st.late_far++; return 0; }
    for (s = lo; s < a; s++) {
        const ysig__call* c = &cu->calls[s % C];
        if (ysig__fp_entry(g, c, f) == 2 || !ysig__call_gt(c, t, cls, ord) || !ysig__fp_meets(g, f, F)) continue;
        while (s > lo && c->gid && cu->calls[(s - 1) % C].gid == c->gid) s--;
        p0 = s;
        break;
    }
    if (p0 == UINT64_MAX) return 1;
    kslot = ysig__ck_at(cu, p0, INT64_MAX);
    if (kslot < 0) { g->st.late_far++; return 0; }
    if (b != cu->c_head) {
        /* to the log's end first, the calls after it down */
        if (n > YSIG__BLK) { g->st.late_full++; return 0; }
        for (i = 0; i < n; i++) cu->blk[i] = cu->calls[(a + i) % C];
        for (s = b; s < cu->c_head; s++) cu->calls[(s - n) % C] = cu->calls[s % C];
        for (i = 0; i < n; i++) cu->calls[(cu->c_head - n + i) % C] = cu->blk[i];
        a = cu->c_head - n;
    }
    at = ysig__late_move_k(g, a, p0, F, t, cls, ord);
    ysig__late_unv(g, at, n, F, t, cls);
    if (cls < 2) {
        /* a late input before a disarm in no group with no input after it:
         * in R that disarm's gap is open again (it takes effect at its
         * gap's end, S1), where the header made it at the input's call.
         * Not verified */
        uint64_t dis = UINT64_MAX;
        int64_t ti;
        for (s = at + n; s < cu->c_head; s++) {
            const ysig__call* c = &cu->calls[s % C];
            if (c->e.kind == YSIG_J_DISARM && !c->gid) dis = s;
            else if (dis != UINT64_MAX && ysig__input_t(&c->e, &ti)) dis = UINT64_MAX;   /* its gap ends there */
        }
        if (dis != UINT64_MAX) g->st.late_unverified++;
    }
    if (cu->def_k < 0) { cu->n_grp_dis = 0; cu->late_in = 0; }
    for (s = at; s < at + n; s++) {
        const ysig__call* c = &cu->calls[s % C];
        int64_t ti;
        if (c->e.kind == YSIG_J_DISARM || c->e.kind == YSIG_J_DISARM_AT) {
            ysig__cx_count(g, at + n, c->e.node, c->cseq);
            if (cu->n_grp_dis < 16) cu->grp_dis[cu->n_grp_dis++] = c->e.node;
        }
        if (ysig__input_t(&c->e, &ti)) cu->late_in = 1;
    }
    if (cu->def_k < 0) {
        for (w = 0; w < g->dw; w++) cu->late_f[w] = 0;
        cu->late_t = t;
        cu->def_p = p0;
        cu->def_k = kslot;
    } else {
        if (p0 < cu->def_p) cu->def_p = p0;
        if (t < cu->late_t) cu->late_t = t;
    }
    for (w = 0; w < g->dw; w++) cu->late_f[w] |= F[w];
    return 1;
}
/* v0.7.0, "Exactness for late calls", conditions 2 to 4: a late unit
 * placed at [at, at + n) of the log with footprint F, key time t, class
 * cls. For each open group G (placed waiting at its place, its L in the
 * log) whose span, from its calls to L, holds the unit and whose footprint
 * the unit meets: an input later than G's time is G's L now (2): 2 (a)
 * and 2 (b) again with it (it meets G's arm; no state call of another
 * group, or of none, meets G between them); any other is counted (3, 4:
 * a state call there, or a shape the header does not order). Counted
 * once a late call (late_unverified). */
static void ysig__late_unv(ysig_graph* g, uint64_t at, uint64_t n, const uint64_t* F, int64_t t, int cls) {
    ysig__cu* cu = g->cu;
    ysig__wt* w = g->wt;
    const uint64_t C = (uint64_t)cu->C;
    int i;
    for (i = 0; i < w->n_op; i++) {
        ysig__ux* o = &w->op[i];
        uint64_t s, zl = UINT64_MAX, li = UINT64_MAX, fa[YSIG__FPW], f[YSIG__FPW];
        int ok, q;
        if (!o->l_seq || !ysig__fp_meets(g, F, o->F)) continue;
        for (s = cu->c_tail; s < cu->c_head; s++) {
            const ysig__call* c = &cu->calls[s % C];
            if (s >= at && s < at + n) continue;
            if (c->gid == o->gid) zl = s;
            if (c->cseq == o->l_seq && !c->wp && li == UINT64_MAX) li = s;
        }
        if (zl == UINT64_MAX || li == UINT64_MAX || at < zl || at > li) continue;
        ok = 0;
        if (cls < 2 && t > o->t && o->node >= 0) {
            for (q = 0; q < g->dw; q++) fa[q] = 0;
            ysig__fp_or(g, fa, o->node);
            ok = ysig__fp_meets(g, F, fa);
            for (s = zl + 1; ok && s < at; s++) {
                const ysig__call* c = &cu->calls[s % C];
                int64_t ti;
                if (ysig__input_t(&c->e, &ti) || c->gid == o->gid) continue;
                if (ysig__fp_entry(g, c, f) == 1 && ysig__fp_meets(g, f, o->F)) ok = 0;
            }
            if (ok) { o->l_seq = cu->calls[at % C].cseq; continue; }
        }
        g->st.late_unverified++;
        return;
    }
}
/* A late call logged at a (its unit to the log's end), made at the end:
 * to its place in R; the run again at the API call's end. */
static int ysig__late_fix(ysig_graph* g, uint64_t a) {
    ysig__cu* cu = g->cu;
    const ysig__call* c0 = &cu->calls[a % (uint64_t)cu->C];
    return ysig__late_place(g, a, cu->c_head, cu->c_tail, c0->tr, ysig__kcls(c0->e.kind), ysig__call_ko(c0));
}
/* v0.7.0, condition 4 in R's order by key: R is defined up to the order
 * of calls that do not meet, and a graph with no window joins a disarm
 * to the gap an arm closes whatever their footprints. So the gap of the
 * group with key (t, a group, gid) is read in the order by key over every
 * logged call: the call with the greatest key below the group's among the
 * inputs, arms, disarms and disarm_at (no gap holds one); when it is a
 * disarm of another group or of none, at another time, G is counted. */
static void ysig__unv(ysig_graph* g, uint64_t gid);
static void ysig__c4(ysig_graph* g, uint64_t gid, int64_t t) {
    ysig__cu* cu = g->cu;
    const ysig__call* best = NULL;
    uint64_t s;
    if (!cu) return;
    for (s = cu->c_tail; s < cu->c_head; s++) {
        const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        int64_t ti;
        int k = c->e.kind;
        if (c->gid == gid || ysig__call_gt(c, t, 2, gid)) continue;
        if (!ysig__input_t(&c->e, &ti) && k != YSIG_J_ARM && k != YSIG_J_DISARM && k != YSIG_J_DISARM_AT) continue;
        if (!best || ysig__call_gt(c, best->tr, ysig__kcls(best->e.kind), ysig__call_ko(best)) ||
            (c->tr == best->tr && ysig__kcls(k) == ysig__kcls(best->e.kind) && ysig__call_ko(c) == ysig__call_ko(best) && c->cseq > best->cseq))
            best = c;
    }
    if (best && best->e.kind == YSIG_J_DISARM && best->tr != t) ysig__unv(g, gid);
}

/* A late call that the window could not log */
static void ysig__late_nolog(ysig_graph* g) {
    if (!g->cu) g->st.late_off++;
    else if (g->cu->full) g->st.late_full++;
    else g->st.late_far++;
}

/* The waiting groups placed in an open gap [from, end) go back to
 * waiting, by their call order (an ahead group takes them back, H9; a
 * past group lets them in again at its place, H13). */
static void ysig__gap_unplace(ysig_graph* g, uint64_t from, uint64_t end) {
    ysig__wt* w = g->wt;
    ysig__cu* cu = g->cu;
    uint64_t u, s;
    int i;
    for (u = from; u < end; u++) {
        const ysig__call* c0 = &cu->calls[u % (uint64_t)cu->C];
        ysig__wa* a;
        if (!c0->wp) continue;
        for (i = 0; i < w->n && w->a[i].seq != c0->wseq; i++) { }
        if (i < w->n) continue;   /* this one is back already */
        a = &w->a[w->n++];
        a->node = c0->wnode >= 0 ? -1 - c0->wnode : c0->wnode;   /* its arm below makes it live */
        a->t = c0->tr;
        a->seq = c0->wseq;
        a->c0 = w->nc;
        a->nc = 0;
        memset(a->xf, 0, sizeof a->xf);
        /* its calls, in order (another one let in by a call of it may
         * sit between them) */
        for (s = u; s < end; s++) {
            const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
            if (!c->wp || c->wseq != a->seq) continue;
            if (c->e.kind == YSIG_J_ARM) { a->node = c->e.node; continue; }
            w->calls[w->nc] = c->e;
            w->call_seq[w->nc] = c->cseq;
            w->call_n[w->nc] = c->n_at;
            w->nc++;
            a->nc++;
        }
    }
}

/* Waiting group k goes into a past group's block (H13), as
 * ysig__wait_place() puts it at the end of the log: what its calls let
 * in first, its calls at its time, then its arm. The run again makes
 * them. */
static void ysig__wait_fire_blk(ysig_graph* g, int64_t t, const uint64_t* f, uint64_t incl, int* nb);
static void ysig__wait_blk(ysig_graph* g, int k, int* nb) {
    ysig__wt* w;
    ysig__wa a;
    if ((k = ysig__wait_closure(g, g->wt->a[k].seq, 1, nb)) < 0) return;
    w = g->wt;
    a = w->a[k];
    ysig_entry calls[YSIG__WAIT_CALLS];
    uint64_t cseq[YSIG__WAIT_CALLS];
    int64_t cn[YSIG__WAIT_CALLS];
    int i, nc = a.nc;
    ysig__call* c;
    for (i = 0; i < nc; i++) { calls[i] = w->calls[a.c0 + i]; cseq[i] = w->call_seq[a.c0 + i]; cn[i] = w->call_n[a.c0 + i]; }
    ysig__wait_drop(w, k);
    for (i = 0; i < nc && *nb < YSIG__BLK - 1; i++) {
        uint64_t f[YSIG__FPW];
        ysig__state_fp(g, &calls[i], f);
        ysig__wait_fire_blk(g, cn[i], f, 0, nb);
        c = &g->cu->blk[(*nb)++];
        memset(c, 0, sizeof *c);
        c->e = calls[i];
        if (c->e.kind == YSIG_J_DISARM) c->e.t = a.t;
        c->tr = a.t;
        c->wp = 1; c->wnode = a.node; c->wseq = a.seq; c->cseq = cseq[i]; c->n_at = cn[i]; c->gid = a.seq;
        if (c->e.kind == YSIG_J_DISARM) ysig__wait_cancel(g, c->e.node, a.t, cseq[i]);
    }
    if (a.node >= 0 && *nb < YSIG__BLK - 1) {
        uint64_t f[YSIG__FPW];
        int w2;
        for (w2 = 0; w2 < g->dw; w2++) f[w2] = 0;
        ysig__fp_or(g, f, a.node);
        ysig__wait_fire_blk(g, a.t, f, a.seq, nb);
        c = &g->cu->blk[(*nb)++];
        memset(c, 0, sizeof *c);
        c->e.kind = YSIG_J_ARM;
        c->e.node = a.node;
        c->e.t = a.t;
        c->tr = a.t;
        c->wp = 1; c->wnode = a.node; c->wseq = a.seq; c->gid = a.seq;
        ysig__wait_fire_blk(g, a.t, f, UINT64_MAX, nb);
    }
}
static void ysig__wait_fire_blk(ysig_graph* g, int64_t t, const uint64_t* f, uint64_t incl, int* nb) {
    ysig__wt* w = g->wt;
    for (;;) {
        int k, best = -1;
        for (k = 0; k < w->n; k++) {
            uint64_t fa[YSIG__FPW];
            if (incl ? w->a[k].t > t || (w->a[k].t == t && w->a[k].seq > incl) : w->a[k].t >= t) continue;
            ysig__wait_fp(g, &w->a[k], fa);
            if (!ysig__fp_meets(g, fa, f)) continue;
            if (best < 0 || w->a[k].t < w->a[best].t || (w->a[k].t == w->a[best].t && w->a[k].seq < w->a[best].seq)) best = k;
        }
        if (best < 0) break;
        ysig__wait_blk(g, best, nb);
    }
}

static int ysig__wait_closure(ysig_graph* g, uint64_t seq, int blk, int* nb) {
    ysig__wt* w = g->wt;
    for (;;) {
        uint64_t fk[YSIG__FPW], fa[YSIG__FPW];
        int k, j, best = -1;
        for (k = 0; k < w->n && w->a[k].seq != seq; k++) { }
        if (k == w->n) return -1;   /* a group of its closure let it in */
        ysig__wait_fp(g, &w->a[k], fk);
        for (j = 0; j < w->n; j++) {
            if (j == k || w->a[j].t > w->a[k].t || (w->a[j].t == w->a[k].t && w->a[j].seq > w->a[k].seq)) continue;
            ysig__wait_fp(g, &w->a[j], fa);
            if (!ysig__fp_meets(g, fa, fk)) continue;
            if (best < 0 || w->a[j].t < w->a[best].t || (w->a[j].t == w->a[best].t && w->a[j].seq < w->a[best].seq)) best = j;
        }
        if (best < 0) return k;
        if (blk) ysig__wait_blk(g, best, nb);
        else ysig__wait_place(g, best, 1);
    }
}

/* An ahead group (t later than the input time): its arm waits (rule 3),
 * the group whole with it (H9): its state calls, made already, are taken
 * out of the log and undone by a run again from the newest checkpoint, and
 * a waiting group that one of them let in goes back to waiting (it goes in
 * again when that call does). Where they cannot be (no window, or a call
 * the log did not keep) the sets and recenters stay where made, counted
 * (arm_past_off, arm_past_full); the disarms, not made yet, wait (S1). */
static void ysig__arm_wait(ysig_graph* g, int node, int64_t t, uint64_t seq) {
    ysig__wt* w = g->wt;
    ysig__cu* cu = g->cu;
    uint64_t from = cu && cu->grp_open && !cu->grp_bad ? cu->grp_from : (cu ? cu->c_head : 0), s;
    int nc = 0, nw = 0, nb = 0, kslot = -1;
    int stays;
    uint64_t lastw = 0;
    /* S1: the gap's disarms take effect at its end, so in the waiting
     * group they go after its other calls (the oracle's case 3578 of seed
     * 31415: a disarm made before the group's recenters; 2192 for a past
     * group) */
    ysig__dis_last(g);
    if (cu && w->n) {
        /* a graph with no window makes this group's sets, set_data and
         * recenters at once, before the waiting groups they meet with a
         * smaller key go in (R makes them with this group, after those):
         * those groups are not verified (the oracle's B, reason 4) */
        uint64_t fs[YSIG__FPW], fx[YSIG__FPW], fa[YSIG__FPW];
        int q, any = 0;
        for (q = 0; q < g->dw; q++) fs[q] = 0;
        if (cu->grp_bad) { for (q = 0; q < g->dw; q++) fs[q] = w->gap_f[q]; any = 1; }
        else
            for (s = from; s < cu->c_head; s++) {
                const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
                if (c->wp || c->e.kind == YSIG_J_DISARM || c->e.kind == YSIG_J_DISARM_AT) continue;
                (void)ysig__fp_entry(g, c, fx);
                for (q = 0; q < g->dw; q++) fs[q] |= fx[q];
                any = 1;
            }
        if (any)
            for (q = 0; q < w->n; q++) {
                if (w->a[q].t > t) continue;
                ysig__wait_fp(g, &w->a[q], fa);
                if (ysig__fp_meets(g, fa, fs)) ysig__unv(g, w->a[q].seq);
            }
        /* the other way round: a waiting group H with a greater key whose
         * sets the graph with no window made already, before this group,
         * which R makes first */
        for (q = 0; q < g->dw; q++) fs[q] = w->gap_f[q];
        ysig__fp_or(g, fs, node);
        for (q = 0; q < w->n; q++) {
            int i2;
            if (w->a[q].t < t || (w->a[q].t == t && w->a[q].seq < seq)) continue;
            for (i2 = 0; i2 < g->dw; i2++) fa[i2] = w->a[q].xf[i2];
            for (i2 = 0; i2 < w->a[q].nc; i2++) {
                const ysig_entry* x = &w->calls[w->a[q].c0 + i2];
                if (x->kind == YSIG_J_DISARM) continue;
                ysig__fp_or(g, fa, x->kind == YSIG_J_RECENTER ? ysig__recenter_node(g, x->node) : x->node);
            }
            if (ysig__fp_meets(g, fa, fs)) { ysig__unv(g, seq); break; }
        }
    }
    if (cu)
        for (s = from; s < cu->c_head; s++) {
            const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
            if (!c->wp) nc++;
            else {
                if (c->e.kind != YSIG_J_ARM) nw++;
                if (c->wseq != lastw) { nb++; lastw = c->wseq; }   /* at most the blocks */
            }
        }
    stays = w->gap_n > 0 && (!cu || cu->grp_bad || nc == 0);
    if (!stays && cu && cu->c_head > from) {
        kslot = ysig__ck_at(cu, from, INT64_MAX);
        if (kslot < 0) stays = 1;
    }
    if (w->n + 1 + (stays ? 0 : nb) > YSIG__WAIT_CAP || w->nc + (stays ? 0 : nc + nw) + w->n_dis > YSIG__WAIT_CALLS) {
        /* no room to wait: made at once on the input known, counted */
        ysig_entry e;
        int logged = 0;
        g->st.arm_past_full++;
        memset(&e, 0, sizeof e);
        e.kind = YSIG_J_ARM;
        e.node = node;
        e.t = t;
        e.param = 2;
        if (cu) logged = ysig__log(g, &e);
        ysig__gap_disarms(g, t, 1);
        ysig__arm_made(g, node, t, g->t_cover, logged ? cu->c_head - 1 : UINT64_MAX);
        return;
    }
    if (stays) {
        /* the sets stay where made; the disarms, not made yet, wait (S1) */
        if (cu) g->st.arm_past_full++;
        else g->st.arm_past_off++;
        nc = 0;
    }
    if (!stays && cu && cu->c_head > from) ysig__gap_unplace(g, from, cu->c_head);
    w->a[w->n].node = node;
    w->a[w->n].t = t;
    w->a[w->n].seq = seq;
    w->a[w->n].c0 = w->nc;
    w->a[w->n].nc = 0;
    {
        int q;
        for (q = 0; q < YSIG__FPW; q++) w->a[w->n].xf[q] = stays ? w->gap_f[q] : 0;
    }
    for (s = from; cu && !stays && s < cu->c_head; s++) {
        const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (c->wp) continue;
        w->calls[w->nc] = c->e;
        w->call_seq[w->nc] = c->cseq;
        w->call_n[w->nc] = c->n_at;
        w->nc++;
        w->a[w->n].nc++;
    }
    if (stays) {
        /* calls that stay: the disarms alone wait with the arm */
        int d;
        for (d = 0; d < w->n_dis && w->nc < YSIG__WAIT_CALLS; d++) {
            memset(&w->calls[w->nc], 0, sizeof w->calls[w->nc]);
            w->calls[w->nc].kind = YSIG_J_DISARM;
            w->calls[w->nc].node = w->dis_node[d];
            w->call_seq[w->nc] = w->dis_seq[d];
            w->call_n[w->nc] = g->t_cover;
            w->nc++;
            w->a[w->n].nc++;
        }
    }
    w->n_dis = 0;
    w->gap_n = 0;
    memset(w->gap_f, 0, sizeof w->gap_f);
    w->fired = 0;
    w->n++;
    if (!stays && cu && cu->c_head > from) {
        /* undo: the log without them, run again from the newest checkpoint */
        cu->c_head = from;
        cu->grp_open = 0;
        cu->arm_node = -1;
        cu->arm_live = 0;
        cu->arm_pos = UINT64_MAX;
        ysig__rerun(g, kslot, -1, t);
    }
}

/* Exactness for a past group placed at pos (its block: nb calls, its own
 * and the waiting groups they let in), in the log before the run again
 * (docs/sig.md, "Exactness"). First the open groups it goes between and
 * their L (condition 2 (b)), then each group of the block: condition 1,
 * and condition 2 with N(G) from the checkpoint the run starts at. */
/* 2 (b): the order in which L lets in the groups at one time that wait
 * for it (rule 3, H16). st: when each one's calls go in; pos: when its
 * arm does (-1: not yet, -2: its calls in, its arm not yet). */
#define YSIG__LSIM 32
typedef struct ysig__lsim {
    const ysig_graph* g;
    int               n, at;
    uint64_t          seq[YSIG__LSIM];
    int               node[YSIG__LSIM], pos[YSIG__LSIM], st[YSIG__LSIM];
    const uint64_t*   F[YSIG__LSIM];
} ysig__lsim;
static void ysig__lsim_place(ysig__lsim* m, int k) {
    uint64_t fa[YSIG__FPW];
    int w, pass;
    m->pos[k] = -2;
    m->st[k] = m->at++;
    for (w = 0; w < m->g->dw; w++) fa[w] = 0;
    if (m->node[k] >= 0) ysig__fp_or(m->g, fa, m->node[k]);
    for (pass = 0; pass < 2; pass++) {
        /* pass 0: before the arm, those earlier by call order that meet it;
         * pass 1: after it, all that meet it */
        if (pass == 1) m->pos[k] = m->at++;
        if (m->node[k] < 0) continue;   /* no arm: it lets nothing in */
        for (;;) {
            int i, b = -1;
            for (i = 0; i < m->n; i++)
                if (m->pos[i] == -1 && (pass || m->seq[i] < m->seq[k]) && ysig__fp_meets(m->g, m->F[i], fa) && (b < 0 || m->seq[i] < m->seq[b])) b = i;
            if (b < 0) break;
            ysig__lsim_place(m, b);
        }
    }
}
static int ysig__unv_x_ok(ysig_graph* g, uint64_t xi, uint64_t gid, int64_t t, int64_t n_x, uint64_t li) {
    /* the exception of 2 (b): the call is of a group H that holds its arm
     * and disarms only, later than G by (time, call order), that waits at
     * its place too, and whose own L is G's: L lets H in, and no input
     * between H and L does (the reviewer's 489) */
    ysig__cu* cu = g->cu;
    const ysig__call* x = &cu->calls[xi % (uint64_t)cu->C];
    const ysig__call* l = &cu->calls[li % (uint64_t)cu->C];
    uint64_t s, fh[YSIG__FPW], f[YSIG__FPW], hl = xi;
    int64_t th = INT64_MIN, ti;
    int w, armed = 0, hn = -1;
    if (!x->gid || (x->e.kind != YSIG_J_ARM && x->e.kind != YSIG_J_DISARM)) return 0;
    for (w = 0; w < g->dw; w++) fh[w] = 0;
    for (s = cu->c_tail; s < cu->c_head; s++) {
        const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (c->gid != x->gid) continue;
        if (c->e.kind != YSIG_J_ARM && c->e.kind != YSIG_J_DISARM) return 0;
        if (c->e.kind == YSIG_J_ARM) { armed = 1; th = c->tr; hn = c->e.node; }
        if (s > hl) hl = s;
        (void)ysig__fp_entry(g, c, f);
        for (w = 0; w < g->dw; w++) fh[w] |= f[w];
    }
    if (!armed || !(th > t || (th == t && x->gid > gid)) || n_x >= th || l->tr <= th || hl >= li) return 0;
    if (ysig__fp_entry(g, l, f) != 1 || !ysig__fp_meets(g, f, fh)) return 0;
    for (s = hl + 1; s < li; s++) {
        const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (ysig__input_t(&c->e, &ti) && ti > th && ysig__fp_entry(g, c, f) == 1 && ysig__fp_meets(g, f, fh)) return 0;
    }
    if (th == t) {
        /* R's order at L among the groups at G's time that wait for it: L
         * lets them in by call order, and each arm lets in, right before
         * it, those earlier by call order that meet it, and right after
         * it, all that meet it (rule 3, H16). A group K made before G can
         * so let H in before G (1061: K armed the detect H disarms; H's
         * disarm of G's detect then cancelled G). Not exact unless G's arm
         * goes in before H's calls. */
        const ysig__wt* wt = g->wt;
        ysig__lsim m;
        int i;
        m.n = 0;
        m.g = g;
        m.at = 0;
        for (i = 0; i < wt->n_op; i++) {
            const ysig__ux* o = &wt->op[i];
            if (o->l_seq != l->cseq || o->t != t || o->gid == x->gid) continue;
            if (m.n == YSIG__LSIM) return 0;   /* too many to order here */
            m.seq[m.n] = o->gid; m.node[m.n] = o->node; m.F[m.n] = o->F; m.n++;
        }
        if (m.n == YSIG__LSIM) return 0;
        m.seq[m.n] = x->gid; m.node[m.n] = hn; m.F[m.n] = fh; m.n++;
        for (i = 0; i < m.n; i++) m.pos[i] = -1;
        for (;;) {
            int b = -1;
            for (i = 0; i < m.n; i++) if (m.pos[i] == -1 && (b < 0 || m.seq[i] < m.seq[b])) b = i;
            if (b < 0) break;
            ysig__lsim_place(&m, b);
        }
        for (i = 0; i < m.n - 1 && m.seq[i] != gid; i++) { }
        if (m.seq[i] != gid || m.pos[i] > m.st[m.n - 1]) return 0;   /* H's disarm would go in first */
    }
    return 1;
}
static int ysig__unv_spans(ysig_graph* g, uint64_t first, uint64_t last, uint64_t gid) {
    /* condition 1: another group with calls before first and after last */
    ysig__cu* cu = g->cu;
    uint64_t ids[64], s;
    int n = 0, i;
    for (s = last + 1; s < cu->c_head; s++) {
        uint64_t x = cu->calls[s % (uint64_t)cu->C].gid;
        if (!x || x == gid) continue;
        for (i = 0; i < n && ids[i] != x; i++) { }
        if (i == n) { if (n == 64) return 1; ids[n++] = x; }
    }
    for (s = cu->c_tail; s < first && n; s++) {
        uint64_t x = cu->calls[s % (uint64_t)cu->C].gid;
        if (!x) continue;
        for (i = 0; i < n; i++) if (ids[i] == x) return 1;
    }
    return 0;
}
static void ysig__unv_place(ysig_graph* g, int kslot, uint64_t pos, int nb) {
    ysig__cu* cu = g->cu;
    ysig__wt* w = g->wt;
    const ysig__ck* k = &cu->ck[kslot];
    uint64_t end = pos + (uint64_t)nb, s;
    int64_t N = k->cover, ti;
    int i;
    for (s = k->call; s < pos; s++) {
        const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (c->e.kind == YSIG_J_RESET || c->e.kind == YSIG_J_RESET_TRIAL) N = c->e.t;
        else if (ysig__input_t(&c->e, &ti) && ti > N) N = ti;
    }
    /* each group of the block, in order */
    for (s = pos; s < end; s++) {
        const ysig__call* c0 = &cu->calls[s % (uint64_t)cu->C];
        uint64_t gid = c0->gid, first = s, last = s, F[YSIG__FPW], Fa[YSIG__FPW], f[YSIG__FPW], u;
        int64_t t = c0->tr;
        int armed = -1, bad, j;
        if (!gid) continue;
        for (u = pos; u < s && cu->calls[u % (uint64_t)cu->C].gid != gid; u++) { }
        if (u < s) continue;   /* done with this group */
        for (j = 0; j < g->dw; j++) F[j] = Fa[j] = 0;
        for (u = s; u < end; u++) {
            const ysig__call* c = &cu->calls[u % (uint64_t)cu->C];
            if (c->gid != gid) continue;
            last = u;
            (void)ysig__fp_entry(g, c, f);
            for (j = 0; j < g->dw; j++) F[j] |= f[j];
            if (c->e.kind == YSIG_J_ARM) { armed = c->e.node; t = c->tr; ysig__fp_or(g, Fa, c->e.node); }
            if (c->e.kind == YSIG_J_DISARM) ysig__fp_or(g, Fa, c->e.node);
        }
        {
            /* v0.7.0: N(G) in R's order by key, which puts every input
             * later than t after G, whatever its footprint: t when an input
             * at t is in R, else before t (G waits at its place) */
            N = t - 1;
            if (k->cover == t) N = t;
            for (u = k->call; u < cu->c_head && N < t; u++) {
                const ysig__call* c = &cu->calls[u % (uint64_t)cu->C];
                if (ysig__input_t(&c->e, &ti) && ti == t) N = t;
            }
        }
        bad = ysig__unv_spans(g, first, last, gid);
        /* 2 (c) at any N(G): a cancelled group's disarm, unless N(G) is t */
        if (!bad && armed < 0 && ysig__fp_any(g, Fa) && N != t) bad = 1;
        if (!bad && armed >= 0) {
            /* condition 4: a disarm of another group, or of no group, in
             * the gap G's arm closes: back from its arm to the last input,
             * barrier or arm */
            for (u = last; u > cu->c_tail; u--) {
                const ysig__call* c = &cu->calls[(u - 1) % (uint64_t)cu->C];
                if (c->gid == gid) continue;
                if (ysig__input_t(&c->e, &ti) || ysig__fp_entry(g, c, f) == 2 || c->e.kind == YSIG_J_ARM) break;
                if (c->e.kind == YSIG_J_DISARM) { bad = 1; break; }
            }
        }
        if (!bad && N < t) {
            /* waits at its place: L in R, meeting the arm, no barrier and
             * no other state call meeting G before it but the exception */
            uint64_t li = UINT64_MAX;
            int64_t nx = N;
            if (armed < 0) bad = 1;   /* (c), or (a) with no disarm */
            for (u = last + 1; !bad && u < cu->c_head; u++) {
                const ysig__call* c = &cu->calls[u % (uint64_t)cu->C];
                int kf = ysig__fp_entry(g, c, f);
                if (kf == 2) { bad = 1; break; }
                if (ysig__input_t(&c->e, &ti)) {
                    if (ti > t && ysig__fp_meets(g, f, F)) { li = u; break; }
                    if (ti > nx) nx = ti;
                }
            }
            /* v0.7.0: a past group by its footprint has its L in R (the
             * input that made it past); pending groups no longer arise */
            if (!bad && li == UINT64_MAX) bad = 1;
            if (!bad && li != UINT64_MAX) {
                const ysig__call* l = &cu->calls[li % (uint64_t)cu->C];
                (void)ysig__fp_entry(g, l, f);
                if (!ysig__fp_meets(g, f, Fa)) bad = 1;
                nx = N;
                for (u = last + 1; !bad && u < li; u++) {
                    const ysig__call* x = &cu->calls[u % (uint64_t)cu->C];
                    if (ysig__input_t(&x->e, &ti)) { if (ti > nx) nx = ti; continue; }
                    if (x->gid == gid || ysig__fp_entry(g, x, f) != 1 || !ysig__fp_meets(g, f, F)) continue;
                    if (!ysig__unv_x_ok(g, u, gid, t, nx, li)) bad = 1;
                }
                if (!bad) ysig__op_add(g, gid, t, F, l->cseq, armed);
            }
        }
        if (bad) ysig__unv(g, gid);
        if (armed >= 0 && N < t) ysig__ux_arm(g, armed, t, gid, F);   /* condition 3: while it waits */
    }
    /* the open groups this block goes between (2 (b)), after the block's
     * own are open too: one of them can link a group K to an H placed
     * earlier (a chain K, J, H) */
    for (i = 0; i < w->n_op; ) {
        ysig__ux* o = &w->op[i];
        uint64_t zl = UINT64_MAX, li = UINT64_MAX;
        int drop = 0;
        for (s = pos; s < end && cu->calls[s % (uint64_t)cu->C].gid != o->gid; s++) { }
        if (s < end) { i++; continue; }   /* the block's own */
        if (!o->l_seq) {
            /* pending (L not in R yet): a past group placed after it whose
             * call meets it is a call of a group R makes wait (counted;
             * its exception needs L, 2 (b)) */
            for (s = cu->c_tail; s < pos && cu->calls[s % (uint64_t)cu->C].gid != o->gid; s++) { }
            if (s < pos)
                for (s = pos; s < end && !drop; s++) {
                    uint64_t fx[YSIG__FPW];
                    const ysig__call* x = &cu->calls[s % (uint64_t)cu->C];
                    if (!ysig__input_t(&x->e, &ti) && ysig__fp_entry(g, x, fx) == 1 && ysig__fp_meets(g, fx, o->F)) { ysig__unv(g, o->gid); drop = 1; }
                }
            if (drop) { ysig__ux x = w->op[i]; w->op[i] = w->op[w->n_op - 1]; w->op[--w->n_op] = x; } else i++;
            continue;
        }
        for (s = cu->c_tail; s < cu->c_head; s++) {
            const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
            if (s >= pos && s < end) continue;
            if (c->gid == o->gid) zl = s;
            if (c->cseq == o->l_seq && !c->wp && li == UINT64_MAX) li = s;
        }
        if (zl == UINT64_MAX || li == UINT64_MAX) drop = 1;
        else if (zl < pos && li >= end) {
            /* every call between G and L, not the block's alone: the
             * block can be a group K that links an earlier H to G's
             * order at L (1061) */
            int64_t nx = cu->n_ck ? cu->ck[cu->ck0].cover : INT64_MAX;
            uint64_t u0 = cu->n_ck ? cu->ck[cu->ck0].call : UINT64_MAX;
            if (u0 > zl) nx = INT64_MAX;   /* N unknown there: not exact */
            for (s = u0 < zl ? u0 : zl; s < li && !drop; s++) {
                const ysig__call* x = &cu->calls[s % (uint64_t)cu->C];
                uint64_t fx[YSIG__FPW];
                int in = ysig__input_t(&x->e, &ti);
                if (s > zl && !in && ysig__fp_entry(g, x, fx) == 1 && ysig__fp_meets(g, fx, o->F) &&
                    !ysig__unv_x_ok(g, s, o->gid, o->t, s >= pos && s < end ? N : nx, li)) {
                    ysig__unv(g, o->gid);
                    drop = 1;
                }
                if (x->e.kind == YSIG_J_RESET || x->e.kind == YSIG_J_RESET_TRIAL) nx = x->e.t;
                else if (in && ti > nx && nx != INT64_MAX) nx = ti;
            }
        }
        if (drop) {
            ysig__ux x = w->op[i];
            w->op[i] = w->op[w->n_op - 1];
            w->op[--w->n_op] = x;
        } else {
            i++;
        }
    }
}

/* A past group with a window (rule 2): it goes as one block after the
 * last logged call that must come before it; the window runs again from
 * the newest checkpoint at or before that place. The waiting groups its
 * calls let in at their calls stay where they went in (wp). */
static void ysig__arm_past(ysig_graph* g, ysig_entry* e, uint64_t seq) {
    ysig__cu* cu = g->cu;
    int node = e->node, kslot, pinned = 0, logged, ng = 0;
    int64_t t = e->t;
    uint64_t F[YSIG__FPW], f[YSIG__FPW], pf[YSIG__FPW];
    uint64_t from = cu->grp_open && !cu->grp_bad ? cu->grp_from : cu->c_head, pos, s, end, q;
    int w;
    for (w = 0; w < g->dw; w++) F[w] = 0;
    ysig__fp_or(g, F, node);
    for (s = from; s < cu->c_head; s++) {
        if (cu->calls[s % (uint64_t)cu->C].wp) continue;
        (void)ysig__fp_entry(g, &cu->calls[s % (uint64_t)cu->C], f);
        for (w = 0; w < g->dw; w++) F[w] |= f[w];
        ng++;
    }
    /* v0.7.0, the place by key (time t, a group, its arm's call order):
     * before the first logged call that meets F with a greater key; the
     * calls that must follow the group move after it (L2). Its own calls
     * alone set it: a waiting group placed in the gap goes in again with
     * the call that lets it in at the place (H13) */
    pos = UINT64_MAX;
    for (s = cu->c_tail; s < from; s++) {
        const ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
        if (ysig__fp_entry(g, c, f) == 2 || !ysig__call_gt(c, t, 2, seq) || !ysig__fp_meets(g, f, F)) continue;
        while (s > cu->c_tail && c->gid && cu->calls[(s - 1) % (uint64_t)cu->C].gid == c->gid) s--;
        pos = s;
        break;
    }
    /* a call before the window that must follow the group (the oldest
     * image's keys): its place is before the window (arm_past_far below).
     * v0.7.0 never pins: the calls that must follow it move */
    if (!cu->n_ck || ysig__ck_late(g, cu->ck0, F, t, 2, seq)) pinned = 1;
    /* nothing logged must follow it but the gap's let-ins: made where it
     * is. With no checkpoint the log is not kept (a clear inside a run
     * again leaves its calls after that one): nothing is known (1414) */
    if (cu->n_ck && !cu->grp_bad && cu->c_head - cu->c_tail < (uint64_t)cu->C && pos == UINT64_MAX) {
        int nothing = 1;
        for (s = from; s < cu->c_head && nothing; s++)
            if (cu->calls[s % (uint64_t)cu->C].wp) nothing = 0;
        if (nothing && !pinned) {
            ysig__dis_last(g);
            logged = ysig__log(g, e);
            for (s = from; s < cu->c_head; s++)
                if (!cu->calls[s % (uint64_t)cu->C].wp) {
                    ysig__set_tr(&cu->calls[s % (uint64_t)cu->C], t);
                    cu->calls[s % (uint64_t)cu->C].gid = seq;
                    (void)ysig__fp_entry(g, &cu->calls[s % (uint64_t)cu->C], f);
                    ysig__key_note(g, f, t, 2);
                }
            ysig__gap_disarms(g, t, 1);
            ysig__arm_made(g, node, t, t, logged ? cu->c_head - 1 : UINT64_MAX);
            return;
        }
    }
    if (pos == UINT64_MAX) pos = from;
    kslot = cu->grp_bad || pinned ? -1 : ysig__ck_at(cu, pos, INT64_MAX);
    if (kslot < 0 || cu->c_head - cu->c_tail >= (uint64_t)cu->C || ng > YSIG__WAIT_CALLS) {
        if (cu->full || kslot >= 0 || cu->grp_bad || ng > YSIG__WAIT_CALLS) g->st.arm_past_full++;
        else g->st.arm_past_far++;
        ysig__dis_last(g);
        logged = ysig__log(g, e);
        for (s = from; s < cu->c_head; s++) if (!cu->calls[s % (uint64_t)cu->C].wp) ysig__set_tr(&cu->calls[s % (uint64_t)cu->C], t);
        ysig__gap_disarms(g, t, 1);
        ysig__arm_made(g, node, t, t, logged ? cu->c_head - 1 : UINT64_MAX);
        return;
    }
    ysig__dis_last(g);
    logged = ysig__log(g, e);
    end = cu->c_head;
    {
        /* the gap's calls, out of [from, end), as one block: the waiting
         * groups placed in the gap go back to waiting, and each of its own
         * calls lets in at t what it meets, right before it (H13). At t no
         * group can come in that N did not let in, so the block is no
         * longer than the gap. */
        ysig__call* blk = cu->blk;
        int nb = 0;
        ysig__gap_unplace(g, from, end);
        for (s = from; s < end && nb < YSIG__BLK - 1; s++) {
            ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
            uint64_t fs[YSIG__FPW];
            if (c->wp) continue;
            ysig__state_fp(g, &c->e, fs);
            ysig__wait_fire_blk(g, t, fs, c->e.kind == YSIG_J_ARM ? UINT64_MAX : 0, &nb);
            ysig__set_tr(c, t);
            c->gid = seq;
            blk[nb++] = *c;
            if (c->e.kind == YSIG_J_DISARM) ysig__wait_cancel(g, c->e.node, t, c->cseq);
        }
        /* the rest of the gap's disarms cancel at t (the run again makes
         * them, from the log) */
        ysig__gap_disarms(g, t, 0);
        for (w = 0; w < nb; w++) cu->calls[(from + (uint64_t)w) % (uint64_t)cu->C] = blk[w];
        cu->c_head = from + (uint64_t)nb;
        for (w = 0; w < g->dw; w++) pf[w] = F[w];
        for (q = from; q < cu->c_head; q++) {
            (void)ysig__fp_entry(g, &cu->calls[q % (uint64_t)cu->C], f);
            for (w = 0; w < g->dw; w++) pf[w] |= f[w];
        }
        pos = pos < from ? ysig__late_move_k(g, from, pos, pf, t, 2, seq) : from;
        /* the cancel by key (W3): an arm in R after the block, made before
         * a disarm of the block */
        for (w = 0; w < nb; w++)
            if (blk[w].e.kind == YSIG_J_DISARM) ysig__cx_count(g, pos + (uint64_t)nb, blk[w].e.node, blk[w].cseq);
        cu->arm_pos = pos + (uint64_t)nb - 1;
        cu->n_grp_dis = 0;
        for (w = 0; w < nb && cu->n_grp_dis < 16; w++)
            if (blk[w].e.kind == YSIG_J_DISARM && !blk[w].wp) cu->grp_dis[cu->n_grp_dis++] = blk[w].e.node;
        ysig__unv_place(g, kslot, pos, nb);
    }
    cu->arm_node = node;
    cu->arm_live = 0;
    ysig__rerun(g, kslot, node, t);
    cu->n_grp_dis = 0;
    cu->arm_live = 0;
    cu->arm_node = -1;
    g->st.arm_caught_up++;
}

static void ysig__arm_now(ysig_graph* g, ysig_entry* ep, uint64_t seq);
YSIG_API bool ysig_arm(ysig_graph* g, int node, int64_t t_ns) {
    ysig_entry e;
    ysig__cu* cu;
    uint64_t seq;
    int64_t nfg;
    if (!g || !g->ok || node < 0 || node >= g->n || g->nd[node].op != YSIG_OP_DETECT) return false;
    if (g->busy) {
        /* from a callback (on_row, a custom op, the journal) inside a call
         * that runs nodes: a run again there would run that call's rest
         * twice (finding 8). Refused, counted */
        g->st.arm_reentry++;
        snprintf(g->err, sizeof g->err, "ysig_arm: called from a callback during another call");
        return false;
    }
    g->busy++;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_ARM;
    e.node = node;
    e.t = t_ns;
    ysig__journal(g, &e);
    cu = g->cu;
    {
        /* v0.7.0: the class is by the group's footprint (its arm's and
         * its gap's calls): input of another device that does not reach
         * the group makes it neither past nor present */
        uint64_t fg[YSIG__FPW];
        int w;
        for (w = 0; w < g->dw; w++) fg[w] = g->wt->gap_f[w];
        ysig__fp_or(g, fg, node);
        nfg = ysig__nf(g, fg);
        g->wt->nfg = nfg;
    }
    if (g->wt->n && t_ns <= nfg) {
        /* rule 3 (S2): a present or past arm is at its t; a waiting group
         * at or before it (S3) that it meets goes in first. An ahead arm
         * lets in when its group goes in (H12) */
        uint64_t f[YSIG__FPW];
        int w;
        /* a past arm: what it lets in goes into its gap, so the group's
         * place takes it in again there (H13's principle; residual 1650:
         * placed at the end it pinned the group after later input) */
        if (cu && t_ns < nfg && !cu->grp_open && !cu->replaying && cu->n_ck) {
            cu->grp_open = 1;
            cu->grp_from = cu->c_head;
        }
        for (w = 0; w < g->dw; w++) f[w] = 0;
        ysig__fp_or(g, f, node);
        ysig__cas_begin(g, YSIG__CAS_ARM, t_ns, f);
        /* what the arm lets in goes after its gap's own calls (inside its
         * group's block, condition 1) and after the gap's disarms (4) */
        g->wt->cas_in = g->wt->gap_n > 0;
        g->wt->cas_dis = g->wt->n_dis > 0 || g->wt->gap4_wp;
        ysig__wait_fire(g, t_ns, f, UINT64_MAX);
        ysig__cas_end(g, 0);
    }
    seq = ++g->wt->seq;
    g->ko = seq;
    if (t_ns > nfg) {
        ysig__arm_wait(g, node, t_ns, seq);
    } else {
        int64_t cu0 = g->st.arm_caught_up;
        if (t_ns == nfg || !cu) ysig__arm_now(g, &e, seq);
        else ysig__arm_past(g, &e, seq);
        ysig__c4(g, seq, t_ns);
        if (g->st.arm_caught_up == cu0) {
            /* at the end of R: the gap's let-ins are inside its block (1),
             * and a let-in group's disarm after the last let-in arm lies
             * in the gap this arm closes (4). A run again tests them at
             * the group's place instead */
            int k;
            for (k = 0; k < g->wt->n_pin; k++) ysig__unv(g, g->wt->pin[k]);
            if (g->wt->gap4_wp) ysig__unv(g, seq);
        }
    }
    if (cu) cu->grp_open = cu->grp_bad = 0;
    g->wt->gap_n = 0;
    memset(g->wt->gap_f, 0, sizeof g->wt->gap_f);
    g->wt->n_pin = 0;
    g->wt->gap4_wp = 0;
    g->busy--;
    ysig__out_final(g);
    return true;
}
/* A present arm (no input later than t), or a past one with no window: at
 * the end of R */
static void ysig__arm_now(ysig_graph* g, ysig_entry* ep, uint64_t seq) {
    ysig__cu* cu = g->cu;
    ysig_entry e = *ep;
    int node = e.node;
    int64_t t_ns = e.t;
    {
        /* present (no input later than t), or past with no window */
        int logged = 0;
        if (t_ns < g->wt->nfg) g->st.arm_past_off++;
        if (cu) {
            uint64_t s, from = cu->grp_open && !cu->grp_bad ? cu->grp_from : cu->c_head;
            ysig__dis_last(g);
            logged = ysig__log(g, &e);
            for (s = from; s < cu->c_head; s++) {
                ysig__call* c = &cu->calls[s % (uint64_t)cu->C];
                uint64_t f[YSIG__FPW];
                if (c->wp) continue;
                ysig__set_tr(c, t_ns);
                c->gid = seq;
                /* v0.7.0: in R the gap's calls have the group's key now */
                (void)ysig__fp_entry(g, c, f);
                ysig__key_note(g, f, t_ns, 2);
            }
        }
        ysig__gap_disarms(g, t_ns, 1);
        ysig__arm_made(g, node, t_ns, t_ns, logged ? cu->c_head - 1 : UINT64_MAX);
        /* present: N(G) is t, exact by condition 2; no late report counts */
    }
}

YSIG_API int ysig_settle(ysig_graph* g, int64_t t_ns) {
    ysig_entry e;
    int late, logged;
    if (!g || !g->ok || ysig__reentered(g, "ysig_settle")) return -1;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_SETTLE;
    e.t = t_ns;
    ysig__journal(g, &e);
    g->wt->seq++;
    g->ko = g->wt->seq;
    ysig__before_input(g, t_ns, NULL);
    late = ysig__is_late(g, NULL, t_ns, 1, g->ko);
    logged = ysig__log(g, &e);
    g->ko = g->wt->seq;
    ysig__do_settle(g, t_ns);
    if (late) {
        if (logged) (void)ysig__late_fix(g, g->cu->c_head - 1);
        else ysig__late_nolog(g);
    }
    ysig__out_final(g);
    return 0;
}

YSIG_API bool ysig_disarm(ysig_graph* g, int node) {
    ysig_entry e;
    ysig__wt* w;
    if (!g || !g->ok || node < 0 || node >= g->n || g->nd[node].op != YSIG_OP_DETECT) return false;
    if (ysig__reentered(g, "ysig_disarm")) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_DISARM;
    e.node = node;
    ysig__journal(g, &e);
    ysig__before_state(g, node, g->t_cover);   /* a plain disarm at N (the ruling) */
    w = g->wt;
    w->seq++;
    g->ko = w->seq;
    w->gap_n++;
    ysig__fp_or(g, w->gap_f, node);
    /* it cancels waiting arms of node at its time: known when its gap
     * closes (its group's time, or the input time) */
    e.t = g->t_cover;
    ysig__log(g, &e);
    if (w->n_dis < YSIG__GAP_DIS) {
        /* it takes effect at the end of its gap (S1) */
        w->dis_node[w->n_dis] = node;
        w->dis_seq[w->n_dis] = w->seq;
        w->n_dis++;
    } else {
        g->cs = w->seq;
        ysig__do_disarm(g, node, g->t_cover);
        ysig__wait_cancel(g, node, g->t_cover, w->seq);
        ysig__ux_disarm(g, node);
    }
    ysig__out_final(g);
    return true;
}

/* v0.7.0: a disarm with the caller's time, a state call of time t in no
 * group (LATE INPUT, "ysig_disarm_at()"). The open gap ends here, in no
 * group, as at an input; the waiting groups earlier than t that meet the
 * detect go in first (rule 3); a waiting arm of it later than t, made
 * before, is cancelled; one already in R with a greater key is cancelled
 * by the run again that places this call (the cancel by key). */
YSIG_API bool ysig_disarm_at(ysig_graph* g, int node, int64_t t_ns) {
    ysig_entry e;
    ysig__wt* w;
    uint64_t f[YSIG__FPW];
    int k, late, logged = 0;
    if (!g || !g->ok || node < 0 || node >= g->n || g->nd[node].op != YSIG_OP_DETECT) return false;
    if (ysig__reentered(g, "ysig_disarm_at")) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_DISARM_AT;
    e.node = node;
    e.t = t_ns;
    ysig__journal(g, &e);
    w = g->wt;
    for (k = 0; k < g->dw; k++) f[k] = 0;
    ysig__fp_or(g, f, node);
    /* the gap ends in no group (its disarms at the input time) */
    w->n_pin = 0;
    ysig__dis_last(g);
    ysig__gap_disarms(g, g->t_cover, 1);
    ysig__gap_late(g);
    if (w->n) {
        ysig__cas_begin(g, YSIG__CAS_STATE, t_ns, f);
        w->cas_dis = w->gap4_wp;
        ysig__wait_fire(g, t_ns, f, 0);
        ysig__cas_end(g, 0);
    }
    w->gap4_wp = 0;
    w->seq++;
    g->ko = w->seq;
    ysig__wait_cancel(g, node, t_ns, w->seq);
    late = ysig__is_late(g, f, t_ns, 2, w->seq);
    if (g->cu) {
        g->cu->srcm = 0;
        g->cu->srcm_all = 0;
        logged = ysig__log(g, &e);
    }
    g->tk = t_ns;
    g->cs = w->seq;
    ysig__do_disarm(g, node, t_ns);
    ysig__ux_disarm(g, node);
    if (late) {
        if (logged) (void)ysig__late_fix(g, g->cu->c_head - 1);
        else ysig__late_nolog(g);
    }
    ysig__out_final(g);
    return true;
}

/* v0.7.0: ysig_feed() of each event, the window run again once at the end
 * for the late ones ("The batch"). */
YSIG_API int ysig_feed_n(ysig_graph* g, const yin_event* e, int n) {
    int i, r, taken = 0;
    if (!g || !g->ok || n < 0 || (n && !e) || ysig__reentered(g, "ysig_feed_n")) return -1;
    if (g->cu) g->cu->defer++;
    for (i = 0; i < n; i++) {
        r = ysig_feed(g, &e[i]);
        if (r > 0) taken += r;
    }
    if (g->cu) g->cu->defer--;
    ysig__out_final(g);
    return taken;
}

static void ysig__do_flush(ysig_graph* g) {
    ysig__key_bar(g, g->t_cover, 0);
    if (g->cu) {
        ysig__cu_clear(g->cu, g->cu->replaying ? g->cu->c_pos : g->cu->c_head);
        g->cu->at_bar = 1;
    }
    ysig__ck_take(g);
}

YSIG_API bool ysig_flush(ysig_graph* g) {
    ysig_entry e;
    if (!g || !g->ok || ysig__reentered(g, "ysig_flush")) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_FLUSH;
    ysig__journal(g, &e);
    g->wt->seq++;
    g->ko = g->wt->seq;
    ysig__before_barrier(g);
    ysig__log(g, &e);
    ysig__do_flush(g);
    ysig__out_final(g);
    return true;
}

YSIG_API int ysig_detected_to(ysig_graph* g, ysig_hit* out, int cap, int64_t t_ns) {
    ysig__cu* cu;
    int q, j = 0, n = 0;
    if (!g || !g->ok || cap < 0 || (cap && !out)) return -1;
    cu = g->cu;
    for (q = 0; q < g->n_hit; q++) {
        if (n < cap && g->hitq[q].t <= t_ns) {
            out[n++] = g->hitq[q];
            /* handed out: a run again matches it, or counts it voided */
            if (cu) ysig__hl_give(cu, cu->qhl[q], cu->qseq[q]);
            continue;
        }
        if (j != q) {
            g->hitq[j] = g->hitq[q];
            if (cu) { cu->qseq[j] = cu->qseq[q]; cu->qhl[j] = cu->qhl[q]; }
        }
        j++;
    }
    g->n_hit = j;
    return n;
}
YSIG_API int ysig_detected(ysig_graph* g, ysig_hit* out, int cap) {
    return ysig_detected_to(g, out, cap, INT64_MAX);
}

YSIG_API int ysig_derived_to(ysig_graph* g, yin_event* out, int cap, int64_t t_ns) {
    ysig__cu* cu;
    int q, j = 0, n = 0;
    if (!g || !g->ok || cap < 0 || (cap && !out)) return -1;
    cu = g->cu;
    for (q = 0; q < g->n_der; q++) {
        if (n < cap && g->derq[q].t <= t_ns) {
            out[n++] = g->derq[q];
            if (cu && !cu->derived_final) ysig__dl_give(cu, cu->dqdl[q], cu->dseq[q]);
            continue;
        }
        if (j != q) {
            g->derq[j] = g->derq[q];
            if (cu) { cu->dseq[j] = cu->dseq[q]; cu->dqdl[j] = cu->dqdl[q]; }
        }
        j++;
    }
    g->n_der = j;
    /* final events that waited for room come into the queue now */
    if (cu && cu->derived_final && !g->busy) ysig__out_final(g);
    return n;
}
YSIG_API int ysig_derived(ysig_graph* g, yin_event* out, int cap) {
    return ysig_derived_to(g, out, cap, INT64_MAX);
}

YSIG_API const ysig_value* ysig_value_of(const ysig_graph* g, int node) {
    if (!g || !g->ok || node < 0 || node >= g->n) return NULL;
    return &g->val[node];
}

YSIG_API int ysig_value_at(const ysig_graph* g, int node, int64_t t_ns, ysig_value* out) {
    const ysig__nd* n;
    if (!g || !g->ok || node < 0 || node >= g->n || !out) return -1;
    n = &g->nd[node];
    if (n->hist.cap) {
        int f;
        if (!n->hist.n) { *out = g->val[node]; return 0; }
        f = ysig__ring_find(&n->hist, t_ns);
        *out = *ysig__ring_at(&n->hist, f < 0 ? 0 : f);
        return f < 0 ? 0 : 1;
    }
    if (n->op == YSIG_OP_SILENT && (n->flags & YSIG__F_AT)) {
        /* judged at t from when its input last arrived, at most at the
         * time the graph knows input up to (the views' rule) */
        const ysig__silent_st* s = (const ysig__silent_st*)n->st;
        int64_t at = t_ns < g->t_known ? t_ns : g->t_known, base;
        ysig_value v;
        int r = ysig_value_at(g, n->in[0], at, &v);
        if (r < 0) return -1;
        /* a sample later than t is the ring's oldest: none had come by t */
        base = v.t <= at && v.t > s->t_reset ? v.t : s->t_reset;
        *out = v;
        if (ysig__sub(at, base) >= n->kns[0]) { out->v[0] = 1; out->t = at; }
        else out->v[0] = 0;
        out->lanes = 1;
        return r;
    }
    if (n->flags & YSIG__F_AT) {
        ysig_value iv[4];
        const ysig_value* in[4];
        int j, rc = 1, rj[4];
        for (j = 0; j < n->n_in; j++) {
            rj[j] = ysig_value_at(g, n->in[j], t_ns, &iv[j]);
            if (rj[j] < 0) return -1;
            if (rj[j] < rc) rc = rj[j];
            in[j] = &iv[j];
        }
        /* a mux's answer depends on the selector and the chosen input: an
         * input not chosen that is older than its ring does not matter */
        if (n->op == YSIG_OP_MUX) {
            int c = ysig__truth(iv[2].v[0]) ? 0 : 1;
            rc = rj[2] < rj[c] ? rj[2] : rj[c];
        }
        memset(out, 0, sizeof *out);
        ysig__meta(n, in, 0, out);
        ysig__pure(n, in, out);
        out->lanes = n->lanes;
        return rc;
    }
    return -1;
}

YSIG_API int ysig_adapter_node(const ysig_graph* g, int adapter, int i) {
    if (!g || !g->ok || adapter < 0 || adapter >= g->n_adapters || i < 0 || i >= g->adapter_n[adapter]) return -1;
    return g->adapter_base[adapter] + i;
}

YSIG_API void ysig_get_stats(const ysig_graph* g, ysig_stats* out) {
    if (!out) return;
    if (!g) { memset(out, 0, sizeof *out); return; }
    *out = g->st;
}

YSIG_API int ysig_replay(ysig_graph* g, const ysig_entry* e, int n) {
    int i;
    if (!g || !g->ok || n < 0 || (n && !e) || ysig__reentered(g, "ysig_replay")) return -1;
    for (i = 0; i < n; i++) {
        bool ok = true;
        switch (e[i].kind) {
        case YSIG_J_RESET: ysig_reset(g, e[i].t); break;
        case YSIG_J_RESET_TRIAL: ysig_reset_trial(g, e[i].t); break;
        case YSIG_J_RECENTER: ok = ysig_recenter(g, e[i].node, e[i].value, e[i].y, e[i].t); break;
        case YSIG_J_SET: ok = ysig_set(g, e[i].node, e[i].param, e[i].value); break;
        case YSIG_J_SET_DATA: ok = ysig_set_data(g, e[i].node, e[i].param, e[i].value); break;
        case YSIG_J_ARM: ok = ysig_arm(g, e[i].node, e[i].t); break;
        case YSIG_J_DISARM: ok = ysig_disarm(g, e[i].node); break;
        case YSIG_J_DISARM_AT: ok = ysig_disarm_at(g, e[i].node, e[i].t); break;
        case YSIG_J_FEED: ok = ysig_feed(g, &e[i].event) >= 0; break;
        case YSIG_J_FRAME: ok = ysig_frame(g, &e[i].frame) == 0; break;
        case YSIG_J_SETTLE: ok = ysig_settle(g, e[i].t) == 0; break;
        case YSIG_J_FLUSH: ok = ysig_flush(g); break;
        default: ok = false; break;
        }
        if (!ok) return -1;
    }
    return n;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_SIG_IMPLEMENTATION */

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
