/* ysp/sig.h - v0.2.0 - public domain single-header signal path
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
 *   A stamp later than the poll is believed only up to the poll: at a
 *   frame with a poll, every time kept from a stamp and later than it
 *   comes down to it (counted as a late poll): a source's order slots,
 *   rings, a detect's hold clock and hold start, a filter's and a rate
 *   integrator's previous time, a silent's last. So one far-future stamp
 *   stops its effects one frame later, but what its own sample's pass
 *   did stands (a rate integral's step over that dt, a hold it ended). A
 *   frame with no poll has no such recovery: the rings and the hold clocks
 *   keep the stamp until a reset (an adapter's until ysig_reset()).
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
 *   frame that knows the time is past it, so a still hand that sends no
 *   samples completes a hold (found at the frame's known time). A hold
 *   armed while its test is true counts
 *   from the arm (p12 entry arm, 0: a hand already on the start), or
 *   waits for the test to fail and come true again (entry fresh, 1).
 *   In a frame's pass a hold's clock is the time the frame knows input
 *   up to, min(poll, onset), while the test is true: a still input (also a
 *   mux or a latch of it) keeps its old t, and the hold would never end.
 *   So a hold completes at a poll, up to a frame after the onset that would
 *   end a hold on samples. A hit is at start + hold on that clock. A
 *   frame-rate detect without a hold tests at the onset. p6: armed at
 *   reset (0) or by ysig_arm().
 *   p7: one hit per arm (0) or every crossing. A detect at frame rate
 *   (p5) tests the held value at each onset. Hits queue (64, the rest
 *   counted) until ysig_detected().
 *   A detect's output may feed a mux, an all, a latch or a peak's gate. It
 *   is the test's state by default (p13 out test); with out hit (1) it is
 *   1 from a hit until the detect is ready again, and until the next arm
 *   or reset for a one-hit or a disarmed detect, so it rises at a hold's
 *   completion, which may be in a frame's pass. Its rise is at the hit's
 *   time, also in its ring. ysig_arm() takes it to 0 for its users at
 *   once, so the next hit is a new rise.
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
 *   time since its last sample. It refuses an absolute adapter, an adapter
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
 *   v0.2.0 - the ops mux, latch, distance, peak, all and silent (D17,
 *          D18, the paper test); detect's output as an input, and
 *          detect's `out` row (hit); ysig_reset_trial() and ysig_recenter()
 *          (D19, journaled); the C++20 enum-float comparisons fixed.
 *   v0.1.0 - first version: the ops above, the schedules, detectors,
 *          derived events, records, journal and replay, roles and
 *          adapters, custom ops, parameter tables, and a name for
 *          each p[] index (YSIG_<OP>_<ROW>).
 *
 *   STATUS: v0.2.0, 2026-11-09, Linux desktop (Debian 12, GCC 12.2):
 *   tests/adapt/sig_test.c (the new ops beyond closed forms: late reports,
 *   a planned onset ahead of the poll, past reads, resets, extreme times,
 *   replay), the compile checks (C99, C11, C++17, C++20), the fuzz target
 *   under ASan and UBSan (20 million inputs, the new ops and calls, valid
 *   adapter graphs), 183 of 183 mutants killed, and trial_visuomotor and
 *   trial_bimanual
 *   byte-identical to v0.1.0's CI data. Four raw mice at 8 kHz into 32
 *   nodes: 146 ns a sample and 0.30 us a ysig_frame() at the median on a
 *   performance core of the i5-14500, no allocator call (docs/sig.md,
 *   "Cost"). Not run: MSVC, macOS, a window, a real device.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_SIG_H_INCLUDED
#define YSP_SIG_H_INCLUDED

#define YSIG_VERSION_MAJOR 0
#define YSIG_VERSION_MINOR 2
#define YSIG_VERSION_PATCH 0
#define YSIG_VERSION_STRING "0.2.0"

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
    YSIG_J_RESET_TRIAL, YSIG_J_RECENTER
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
 * units, at time t_ns; its integrator continues from there on the count grid
 * (ROLES AND ADAPTERS). false with the reason in ysig_error(). */
YSIG_API bool ysig_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns);

/* p[param] of node := v, checked by the op's table; between trials. param
 * is the row's index, by name YSIG_<OP>_<ROW>. A parameter that sizes the
 * arena (a ring's capacity, sines' count) is refused. State is kept. */
YSIG_API bool ysig_set(ysig_graph* g, int node, int param, double v);
YSIG_API bool ysig_set_data(ysig_graph* g, int node, int index, double v);

/* Every input event. Returns how many sources took it (0 or more), -1 for
 * a bad argument. */
YSIG_API int ysig_feed(ysig_graph* g, const yin_event* e);

/* Once per frame, after the frame's input was fed. 0, or -1. */
YSIG_API int ysig_frame(ysig_graph* g, const ysig_frame_in* f);

/* Detects: arm from t (DETECTORS), or stop. */
YSIG_API bool ysig_arm(ysig_graph* g, int node, int64_t t_ns);
YSIG_API bool ysig_disarm(ysig_graph* g, int node);

/* The queues since the last call: hits, derived events. */
YSIG_API int ysig_detected(ysig_graph* g, ysig_hit* out, int cap);
YSIG_API int ysig_derived(ysig_graph* g, yin_event* out, int cap);

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
    { "entry", "enum", 0, 1, 0, "", "a hold armed while its test is true: counts from the arm, or waits for the test "
      "to fail and come true again", "arm=0,fresh=1" },
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
    at = ysig__al(&off, sizeof(ysig_hit) * YSIG_QUEUE_CAP);
    if (g) g->hitq = (ysig_hit*)(void*)(base + at);
    at = ysig__al(&off, sizeof(yin_event) * YSIG_QUEUE_CAP);
    if (g) g->derq = (yin_event*)(void*)(base + at);
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

static void ysig__ring_push(ysig__ring* r, const ysig_value* v) {
    int at = (r->head + r->n) % r->cap;
    if (r->n == r->cap) { r->head = (r->head + 1) % r->cap; at = (r->head + r->n - 1) % r->cap; }
    else r->n++;
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
        if (v->t == last->t) { *last = *v; return; }
    }
    ysig__ring_push(r, v);
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
    if (g->n_hit >= YSIG_QUEUE_CAP) { g->st.hits_lost++; return; }
    h = &g->hitq[g->n_hit++];
    memset(h, 0, sizeof *h);
    h->node = node;
    h->t = t;
    h->t_found = t_found;
    h->value = *v;
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

static void ysig__det_arm(ysig_graph* g, int i, int64_t t) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    const ysig_value* v = &g->val[n->in[0]];
    int r = ysig__test(n, v, 0);
    s->armed = 1;
    s->t_arm = t;
    s->state = r;
    s->fired = 0;
    if ((int)n->p[13] == YSIG_OUT_HIT) g->val[i].v[0] = 0.0;
    if (n->kns[0] > 0 && n->p[12] == 0) {
        /* a hold counts from the arm when the test is true then (entry
         * arm, the default: a hand on the start at a trial's start) */
        s->ready = 1;
        s->holding = r;
        s->t_start = t;
        s->v_start = *v;
    } else if (n->kns[0] > 0) {
        /* entry fresh: true at the arm waits for a fresh entry */
        s->ready = !r;
        s->holding = 0;
    } else {
        /* past the test at arm: held until it re-arms (CROSSINGS) */
        s->ready = !r;
        s->holding = 0;
    }
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

/* A detect's output after a value or a frame: its test, or whether a hit
 * stands, with the time and sources of the value it tested. */
static void ysig__det_out(ysig_graph* g, int i, const ysig_value* v, int64_t tc) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    ysig_value* o = &g->val[i];
    int hit_out = (int)n->p[13] == YSIG_OUT_HIT;
    o->v[0] = (hit_out ? s->fired : s->state) ? 1.0 : 0.0;
    o->t = tc;
    o->t_oldest = v->t_oldest;
    o->device = v->device;
    o->src = v->src;
    o->src_oldest = v->src_oldest;
    o->tier = v->tier;
    o->unc_us = v->unc_us;
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
        /* before the arm: no hit and no start, but a hold's failing value
         * breaks it whatever its t (a late report of a hand that left) */
        if (n->kns[0] > 0 && !r) {
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
    /* the rise of a hit output is at the hit: a past read after it says 1 */
    ysig__det_out(g, i, v, s->fired && !was && (int)g->nd[i].p[13] == YSIG_OUT_HIT ? s->t_hit : tc);
}

/* A sample-rate detect at a frame: a still hand completes its hold. tk is
 * the time the frame knows input up to. */
static void ysig__det_frame(ysig_graph* g, int i, int64_t tk, const ysig_value* v) {
    ysig__nd* n = &g->nd[i];
    ysig__det_st* s = (ysig__det_st*)n->st;
    int was = s->fired;
    if (!s->armed || n->kns[0] <= 0 || tk < s->t_arm) return;
    (void)ysig__det_hold_done(g, i, tk);
    if ((int)n->p[13] == YSIG_OUT_HIT && s->fired && !was) ysig__det_out(g, i, v, s->t_hit);
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

static void ysig__emit(ysig_graph* g, int i, const ysig_value* v) {
    ysig__nd* n = &g->nd[i];
    yin_event* e;
    int k;
    ysig__digest_word(g, (uint64_t)(uint32_t)i);
    ysig__digest_value(g, v);
    if (g->n_der >= YSIG_QUEUE_CAP) { g->st.derived_lost++; return; }
    e = &g->derq[g->n_der++];
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
    if (g->on_row) g->on_row(g->user, &r);
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
                     * on a view, at the view's time. A hold's clock in a
                     * frame's pass is the time the frame knows input up
                     * to, while the test is true: a still input's value
                     * keeps its old t, and the hold would never end */
                    ysig_value v = *x;
                    int holding = n->kns[0] > 0 && ysig__test(n, x, 0);
                    if (at_frame) v.t = holding ? g->t_known : t;
                    else if (holding && g->t_known > v.t) v.t = g->t_known;
                    ysig__det_value(g, i, &v, v.t, ysig__sink_t(g, n, v.t));
                } else {
                    ysig__det_frame(g, i, g->t_known, x);
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
                if (n->vt->reset) n->vt->reset(n->st, n->p, t);
                for (k = 0; k < n->lanes; k++) o->v[k] = 0;
            } else {
                /* the op takes its inputs as one array: gathered, since
                 * the nodes need not be neighbors in val[] */
                ysig_value iv[4];
                for (j = 0; j < n->n_in; j++) iv[j] = *in[j];
                n->vt->sample(n->st, n->p, iv, n->n_in, o);
            }
        } else {
            if (mode == 2) {
                if (n->vt->reset) n->vt->reset(n->st, n->p, t);
                for (k = 0; k < n->lanes; k++) o->v[k] = 0;
                o->t = o->t_oldest = t;
            }
            else if (mode == 1) {
                o->t = o->t_oldest = t;
                o->tier = YIN_TIER_SIM;
                o->src = o->src_oldest = (uint16_t)i;
                n->vt->frame(n->st, n->p, t, o);
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
                ysig__ring_push(r, x);
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
            ysig__ring_push(r, x);
            *o = *x;
            break;
        }
        if (mode == 0 && (!r->n || x->t >= ysig__ring_at(r, r->n - 1)->t)) ysig__ring_push(r, x);
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
    if (g->journal) g->journal(g->user, e);
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
            if (n->p[6] == 0) ysig__det_arm(g, id, t);
            o->v[0] = (int)n->p[13] != YSIG_OUT_HIT && ((ysig__det_st*)n->st)->state ? 1.0 : 0.0;
            ysig__hist_push(g, n, o);
            continue;
        }
        ysig__run(g, id, 2, t);
    }
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
    ysig__sched(&a, g->src_ids, g->src_off, g->sched, g->frame);
    g->n_src = a.n_src;
    g->n_frame = a.n_frame;
    g->channels = d->channels;
    g->n_channels = d->n_channels;
    g->on_row = d->on_row;
    g->journal = d->journal;
    g->user = d->user;
    for (i = 0; i < a.n; i++)
        if (g->nd[i].flags & YSIG__F_VIEW) g->has_view = 1;
    g->t_fed = INT64_MIN;
    g->ok = 1;
    ysig__reset(g, 0, 0);
    g->st.digest = 14695981039346656037ull;
    return true;
}

YSIG_API void ysig_reset(ysig_graph* g, int64_t t_ns) {
    ysig_entry e;
    if (!g || !g->ok) return;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_RESET;
    e.t = t_ns;
    ysig__journal(g, &e);
    ysig__reset(g, t_ns, 0);
}

YSIG_API void ysig_reset_trial(ysig_graph* g, int64_t t_ns) {
    ysig_entry e;
    if (!g || !g->ok) return;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_RESET_TRIAL;
    e.t = t_ns;
    ysig__journal(g, &e);
    ysig__reset(g, t_ns, 1);
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

YSIG_API bool ysig_recenter(ysig_graph* g, int role, double x, double y, int64_t t_ns) {
    uint8_t reach[YSIG_MAX_NODES];
    ysig_entry e;
    double xy[2];
    int id = -1, k, pos = 0;
    ysig__nd* n;
    ysig__int_st* s;
    ysig_value* o;
    if (!g || !g->ok) return false;
    if (role < 0 || !isfinite(x) || !isfinite(y)) {
        snprintf(g->err, sizeof g->err, "ysig_recenter: a bad role or a point that is not finite");
        return false;
    }
    xy[0] = x;
    xy[1] = y;
    if (!ysig__recenter_at(g, role, xy, &id)) return false;
    n = &g->nd[id];
    s = (ysig__int_st*)n->st;
    o = &g->val[id];
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
    for (k = 0; k < 2; k++) {
        if (n->flags & YSIG__F_COUNTS) { s->n[k] = 0; s->org[k] = xy[k]; }
        else s->acc[k] = xy[k];
        o->v[k] = xy[k];
    }
    s->recentered = 1;
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
    return true;
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

YSIG_API bool ysig_set(ysig_graph* g, int node, int param, double v) {
    const ysig_param* rows;
    ysig__nd* n;
    ysig_entry e;
    int nr;
    if (!g || !g->ok || node < 0 || node >= g->n || param < 0 || param >= YSIG_NP || !isfinite(v)) return false;
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
    n->p[param] = v;
    ysig__prepare(g, node);
    return true;
}

YSIG_API bool ysig_set_data(ysig_graph* g, int node, int index, double v) {
    ysig_entry e;
    if (!g || !g->ok || node < 0 || node >= g->n || index < 0 || index >= g->nd[node].n_data || !isfinite(v)) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_SET_DATA;
    e.node = node;
    e.param = index;
    e.value = v;
    ysig__journal(g, &e);
    g->nd[node].data[index] = v;
    return true;
}

static void ysig__ring_lower(ysig__ring* r, int64_t poll) {
    int k;
    for (k = r->n - 1; k >= 0; k--) {
        ysig_value* v = &r->buf[(r->head + k) % r->cap];
        if (v->t <= poll) break;     /* in time order: the rest are earlier */
        v->t = poll;
    }
}

/* A stamp later than the poll is believed only up to the poll: one bad
 * stamp must not make its source drop every later sample, its rings skip
 * them, a hold or a filter wait for its time, or hide a silence. Every
 * time kept from a stamp comes down to it. */
static void ysig__lower_late(ysig_graph* g, int64_t poll) {
    int i, d;
    for (i = 0; i < g->n; i++) {
        ysig__nd* n = &g->nd[i];
        ysig__ring_lower(&n->hist, poll);
        if (n->sink_t > poll) n->sink_t = poll;
        switch (n->op) {
        case YSIG_OP_SOURCE: {
            ysig__src_st* s = (ysig__src_st*)n->st;
            for (d = 0; d < s->n; d++)
                if (s->last_t[d] > poll) s->last_t[d] = poll;
            break;
        }
        case YSIG_OP_SILENT: {
            ysig__silent_st* s = (ysig__silent_st*)n->st;
            if (s->last > poll) s->last = poll;
            break;
        }
        case YSIG_OP_VELOCITY: case YSIG_OP_SPEED: case YSIG_OP_DELAY:
            ysig__ring_lower((ysig__ring*)n->st, poll);
            break;
        case YSIG_OP_LOWPASS: {
            ysig__lp_st* s = (ysig__lp_st*)n->st;
            if (s->t_prev > poll) s->t_prev = poll;
            break;
        }
        case YSIG_OP_INTEGRATE: {
            ysig__int_st* s = (ysig__int_st*)n->st;
            if (s->t_prev > poll) s->t_prev = poll;
            break;
        }
        case YSIG_OP_DETECT: {
            ysig__det_st* s = (ysig__det_st*)n->st;
            if (s->holding && s->t_start > poll) s->t_start = poll;
            break;
        }
        default: break;
        }
    }
}

static int ysig__source_takes(const ysig__nd* n, const yin_event* e) {
    uint32_t type = n->p[5] != 0 ? (uint32_t)n->p[5] : (uint32_t)YIN_SAMPLE;
    return e->kind == (uint8_t)n->p[0] && e->type == type && e->control == (uint32_t)n->p[1] &&
           (n->p[2] == 0 || e->device == (uint32_t)n->p[2]);
}

YSIG_API int ysig_feed(ysig_graph* g, const yin_event* e) {
    int i, k, taken = 0, journaled = 0;
    if (!g || !g->ok || !e) return -1;
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
        n->pass = ++g->pass;   /* this sample's evaluation: the source changed */
        for (k = g->src_off[i]; k < g->src_off[i + 1]; k++) ysig__run(g, g->sched[k], 0, e->t);
        taken++;
    }
    g->st.taken += taken;
    return taken;
}

YSIG_API int ysig_frame(ysig_graph* g, const ysig_frame_in* f) {
    ysig_entry e;
    int k;
    if (!g || !g->ok || !f || f->n_values < 0 || (f->n_values && !f->values)) return -1;
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
    g->frame_index = f->index;
    g->onset = f->onset_ns;
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
            ysig__lower_late(g, f->poll_ns);
        }
        g->t_known = f->poll_ns < f->onset_ns ? f->poll_ns : f->onset_ns;
    } else {
        g->t_known = f->onset_ns;   /* no view reads it (refused above) */
    }
    ysig__digest_word(g, (uint64_t)f->index);
    ysig__digest_word(g, (uint64_t)f->onset_ns);
    g->pass++;
    for (k = 0; k < g->n_frame; k++) ysig__run(g, g->frame[k], 1, f->onset_ns);
    g->fvals = NULL;
    g->n_fvals = 0;
    return 0;
}

/* A hit output that fell at an arm: its users see the 0 now, or the next
 * hit in the next sample's pass would be no rise for a gate or a trigger. */
static void ysig__settle(ysig_graph* g, int from) {
    uint8_t reach[YSIG_MAX_NODES];
    int k, pos = 0;
    memset(reach, 0, (size_t)g->n);
    reach[from] = 1;
    for (k = 0; k < g->n; k++) if (g->order[k] == from) pos = k;
    for (k = pos + 1; k < g->n; k++) {
        int nid = g->order[k], j, hit = 0;
        ysig__nd* m = &g->nd[nid];
        for (j = 0; j < m->n_in; j++) if (reach[m->in[j]]) hit = 1;
        if (!hit || m->op >= YSIG_OP_COUNT) continue;
        if (m->op != YSIG_OP_LATCH && m->op != YSIG_OP_PEAK && !(ysig__ops[m->op].kind & YSIG__K_PURE)) continue;
        ysig__run(g, nid, 1, g->onset);
        reach[nid] = 1;
    }
}

YSIG_API bool ysig_arm(ysig_graph* g, int node, int64_t t_ns) {
    ysig_entry e;
    if (!g || !g->ok || node < 0 || node >= g->n || g->nd[node].op != YSIG_OP_DETECT) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_ARM;
    e.node = node;
    e.t = t_ns;
    ysig__journal(g, &e);
    {
        int fell = (int)g->nd[node].p[13] == YSIG_OUT_HIT && g->val[node].v[0] != 0;
        ysig__det_arm(g, node, t_ns);
        if (fell) ysig__settle(g, node);
    }
    return true;
}

YSIG_API bool ysig_disarm(ysig_graph* g, int node) {
    ysig_entry e;
    ysig__det_st* s;
    if (!g || !g->ok || node < 0 || node >= g->n || g->nd[node].op != YSIG_OP_DETECT) return false;
    memset(&e, 0, sizeof e);
    e.kind = YSIG_J_DISARM;
    e.node = node;
    ysig__journal(g, &e);
    s = (ysig__det_st*)g->nd[node].st;
    s->armed = s->holding = 0;
    return true;
}

YSIG_API int ysig_detected(ysig_graph* g, ysig_hit* out, int cap) {
    int n;
    if (!g || !g->ok || cap < 0 || (cap && !out)) return -1;
    n = g->n_hit < cap ? g->n_hit : cap;
    if (n) memcpy(out, g->hitq, sizeof *out * (size_t)n);
    if (n < g->n_hit) memmove(g->hitq, g->hitq + n, sizeof *out * (size_t)(g->n_hit - n));
    g->n_hit -= n;
    return n;
}

YSIG_API int ysig_derived(ysig_graph* g, yin_event* out, int cap) {
    int n;
    if (!g || !g->ok || cap < 0 || (cap && !out)) return -1;
    n = g->n_der < cap ? g->n_der : cap;
    if (n) memcpy(out, g->derq, sizeof *out * (size_t)n);
    if (n < g->n_der) memmove(g->derq, g->derq + n, sizeof *out * (size_t)(g->n_der - n));
    g->n_der -= n;
    return n;
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
    if (!g || !g->ok || n < 0 || (n && !e)) return -1;
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
        case YSIG_J_FEED: ok = ysig_feed(g, &e[i].event) >= 0; break;
        case YSIG_J_FRAME: ok = ysig_frame(g, &e[i].frame) == 0; break;
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
