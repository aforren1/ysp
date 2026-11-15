/* sig_arm_ref.c - a reference oracle for ysp/sig.h's arm in the past,
 * written from docs/sig.md, "An arm in the past: the semantics" (v2-lower
 * 09ecfb2, amended in b6676f4) and the graph's other rules in sig.md. It
 * does not read the header's internals.
 *
 * Late voids (the coordinator's ruling of 2026-11-15; the builder's case
 *   763 of seed 20261115): a hit handed out that a late call voids counts
 *   in late_hits_voided, a guard stop. hand_check() used to fail any void
 *   with no arm since the last check. In late modes, with a late call since
 *   the last check, the voids must now rise late_hits_voided (no arm since)
 *   or the three counts together (arms too). Compile with
 *   -DSIG_HAS_LATE_VOIDED for a header with the count; without it the
 *   check takes the pre-ruling text (superseded + retracted) and counts
 *   those checks. a_counts() puts late_hits_voided on the fallback side.
 *
 * Tally fix (the builder's case 14 of seed 20261115): push() read A's
 *   counts at the failing call itself, without the late counts; it now
 *   uses a_counts(), as the B-cannot tally and the end checks do. Counted:
 *   arm_unverified and late_unverified; the fallbacks the guard stops on
 *   (pinned, far, full, off, retracted, rows and derived stale, late_far,
 *   late_full). Not counted: late_barrier and late_frames (the guard does
 *   not stop on them) and late_off (no window; not compared).
 *
 * The 9834173 text (SIG_ORACLE_LATE=4; 3 keeps the 7a17176 draft):
 *   - The cancel by key (U1, U5): a disarm cancels an arm of its detect
 *     made before it, waiting or already in R, exactly when its key is
 *     smaller than the arm's group key (time earlier than tau; at one time
 *     the arm, made before, is the smaller). An arm already in R is taken
 *     out of R (cancel_by_disarm). Tally: let-in arms cancelled by key.
 *   - Everything else as mode 3. The disarm_at() cases are hand-derived
 *     in sig_disarm_at_cases.c (the 0.6.1 B cannot make that call).
 *
 * The 7a17176 text (SIG_ORACLE_LATE=3; 2 keeps the 25837ee draft):
 *   - Key: (time; class: a feed, then a frame or a settle, then a state
 *     call or a group; call order, a group's by its arm). Late: R holds a
 *     meeting call with a greater key. A plain ysig_disarm() at N (the
 *     coordinator's ruling on open question 3); other state calls outside
 *     a group at N(F(c)) at their call. SIG_ORACLE_KEY3=1 uses this key
 *     for the order comparison in 0.6.x mode.
 *   - T1: a call that lets a group in lets in, before it, every waiting
 *     group that meets it with a smaller key, transitively
 *     (place_waiting_at; group.by_closure).
 *   - Tallies: closure let-ins; late state calls (T3); inputs late only
 *     because a plain disarm sits at N; plain disarms that cancel an arm
 *     at their own tau (rule 3's "at or before"); late state calls inside
 *     a waiting group's span that meet it (no retest in the text); disarms
 *     at or before tau of a group the closure let in, made after its arm.
 *   - Not generated: ysig_disarm_at() (the 0.6.1 header B cannot make it).
 *   - SIG_ORACLE_NOLATEGEN=1 turns off the late-mode generator shapes.
 *
 * The 25837ee text (R as a time order over meeting pairs):
 *   - SIG_ORACLE_ORDERCMP=1 (any mode): at each case's end, every pair of
 *     R's calls whose footprints meet and whose order is not the time
 *     order's (key: time, an input first, call order; a group by its arm's
 *     call; a state call outside a group at N(F(c)) at its call), by class.
 *     Cases with a fallback are left out: after one, the oracle's R holds
 *     the header's order (a fallen-back group at the end), not R's.
 *   - SIG_ORACLE_LATE=2: late mode by the 25837ee text. R is sorted by that
 *     key in each barrier segment after every call (r_sort(); a group's
 *     calls keep their block order); which calls R holds (lets in,
 *     cancels) is decided as before, with N(F) classes. An input before
 *     the last barrier's time is dropped (input_late_barrier). B gets the
 *     sorted R. l_track() counts late inputs that become a waiting group's
 *     L, and those that miss F_a. SIG_ORACLE_LATE=1 keeps the d906e19 mode.
 *
 * Tally fixes after the coordinator's check (seeds 779001, 779007, 780002):
 *   - A failure that end_checks() finds read no A count (g_fail_ci -1, the
 *     counts 0), so it printed "no count". A's counts are now kept after
 *     every call (g_unv_at, g_oth_at), and an end-check failure reads them
 *     at the call that handed out the first wrong row or derived event
 *     (rowrec.at, derrec.at: the call that delivered it; for final-only
 *     rows, the call at which the row became final and was handed out). A
 *     row or event of R that A never handed out, and the end checks of
 *     hits (an R hit A did not hand out; the extra hits' total), have no
 *     such call: they read the case's last call, an end-of-case read that
 *     accepts a count made after the caller had the wrong hit. The summary
 *     line counts those cases.
 *   - The B-cannot tally accepted arm_unverified only. It now accepts a
 *     fallback the guard stops on as well (pinned, far, full, off,
 *     retracted, rows and derived stale): "Counts and fallbacks" says a
 *     fallback is not R's result. It also reports how many B-cannot cases
 *     had such a count by the call where B was found unable to run R.
 *
 * Late-input mode (SIG_ORACLE_LATE=1), from docs/sig.md at v2-late
 * d906e19, "Late input (proposed for 0.7.0; not built)". Without the
 * variable the oracle is unchanged and checks the 0.6.x header as before.
 *   - Generator: device 2 delivers 1 to 20 ms late (in its own order), and
 *     half the frames have an onset 1 to 2 ms before the newest input fed
 *     (the poll after the onset).
 *   - R: rule 4 as amended (late_input()): an input goes right after the
 *     last call of R that must come before it (a barrier, or a call at or
 *     before its time that meets it; after a group's whole block); the
 *     waiting groups it lets in go right before it, there. Frames go at
 *     R's end. Classes and a state call's time per footprint: N(F) (nf()),
 *     a reset setting it to its t, a flush keeping it (the text leaves the
 *     flush open, finding L11). Pins only before the window.
 *   - B: as before, the graph with no window given R's calls in R's order
 *     (the 0.6.x header with a zeroed window); it places nothing.
 *   - A: none (no 0.7.0 header). A's counts still mark the arms that fall
 *     back (an approximation of the window); nothing is compared to A.
 *   - Tallies (the "late" summary line): late inputs; the text's counts
 *     (input_late_far: the place before the catchup_frames-th newest frame
 *     or barrier; input_late_barrier; condition 2; input_late_frames);
 *     R out of time order for a node (an input, or a past group's arm,
 *     after a later input that meets it), with the call it had to follow,
 *     and how many such cases nothing counts; B-cannot cases counted by the
 *     text (input counts, or the group predicate); late inputs that become
 *     a waiting group's L, and how many miss F_a.
 *   - SIG_ORACLE_LATEDBG=<case> prints each out-of-order placement;
 *     SIG_ORACLE_DEBUG7=1 prints the reason-7 inputs (equal times flagged).
 *
 * Change after db38205 (the coordinator's check of case 2201, seed 777001):
 *   - The "other count" of a wrong case that arm_unverified does not count
 *     includes arm_past_pinned. sig.md's "Counts and fallbacks" lists
 *     pinned with far, full, off, retracted and the stale counts as the
 *     fallbacks the guard stops on; the tally left it out, so a pinned
 *     wrong case printed as one with none.
 *
 * Changes for the text at 085bbb6 (b22dccb, f903891, 617971a, 816d958),
 * and why:
 *   - H12: an ahead arm lets in at its tau when its group goes in (it used
 *     the smaller of tau and N at its call); a past group's calls let in
 *     at the group's tau (they used N at their call). trig_time() has it.
 *   - S3 at equal times: an arm lets in a waiting group with its own tau
 *     only when that group's arm call is earlier; one made later goes in
 *     right after the arm, not before it (g_pw_lo/hi, g_pw_after). The
 *     header puts it before (H16).
 *   - A group an earlier one's call let in during the same batch is not
 *     put in twice (place_waiting_at).
 *   - Hits are compared as values are, lanes below `lanes` only
 *     (SIG_ORACLE_HITMEM=1: the whole struct, as before).
 *   - Voided-hit bookkeeping is sticky (the o2 review: a copy matched
 *     again and voided again was counted twice).
 *   - B cannot run R (new reasons): 7, an input at or before the tau of an
 *     arm waiting in B's graph that meets it (R has it after the arm); 8, a
 *     disarm of a group whose arm was cancelled, going in at a tau later
 *     than B's input time, on a detect with a hold (S1: R meets the hold up
 *     to tau; B's graph only up to its N).
 *   - SIG_ORACLE_PRED=1 evaluates the arm_unverified predicate (P1, P2;
 *     see pred_check) and counts what it flags in B-cannot, wrong and
 *     right cases, and per made group.
 *
 * Changes for the text at c2c5459 (167616c), and why:
 *   - S3: an arm call lets in a waiting group it meets whose tau is at or
 *     before its time (the smaller of its t and N at its call).
 *   - H9: a state call's letting in takes effect when the call goes into
 *     R. A call in an open gap goes in at the gap's end (in no group: the
 *     waiting groups it lets in go right before it) or at its group's arm
 *     (right before the call where the call goes into R: a present group's
 *     at R's end, a past group's at its place in R, by the ruling on
 *     "stays where it went in"); an ahead group's calls when the group goes
 *     in, each letting in what it meets then. Compares are skipped while a gap is open: its
 *     calls' letting in is not decided yet.
 *   - The out-of-window pin (8594b73) cannot be decided exactly from the
 *     public side: where the window starts depends on its log and ring
 *     writes, which no public read gives (the logs can run full at any
 *     call). The oracle accepts a pin count when an input after the last
 *     barrier and before the place is later than tau and lies before the
 *     catchup_frames-th newest frame or barrier of R (older than anything
 *     the window could hold).
 *   - S6 (count ambiguity): arm_caught_up is required only when a call
 *     after the place shares a node with the group (one that shares none
 *     commutes: the group may be made where it is), and a pinned group may
 *     count only as pinned.
 *   - SIG_ORACLE_H9=1 sets aside cases where a call of an ahead group, its
 *     arm included, would let a waiting group in at its own call (the
 *     shapes of H9 and H12; c2c5459 still lets one in at an ahead arm's
 *     call). SIG_ORACLE_EMIT=n prints case n as a public-API C program.
 *
 * Changes for the text at f2f0e0d (321ac2b, 8594b73, ca9bc48), and why:
 *   - S1: a disarm takes effect at the end of its gap. In a group block the
 *     group's other calls come first, then its disarms, then the arm; a
 *     disarm in no group moves to the end of its gap. In a group that B's
 *     graph sees ahead it waits with the arm (B's waiting footprint has the
 *     disarm's too); no case is set aside for it any more.
 *   - H2: a disarm's cancel is decided when it goes into R: a past or
 *     present group's at its arm, a waiting group's when the group goes
 *     in (and before the next waiting group of the same batch), a disarm
 *     in no group at its gap's end.
 *   - S2: an arm call's time for rule 3's test is the smaller of t and N.
 *   - 8594b73: an input before the window's oldest checkpoint later than
 *     tau pins whatever its footprint (see above: not decidable from the
 *     public side); a pin that rule 2 gives must still be counted.
 *   - ca9bc48: superseded is a hit later than tau of a detect the group
 *     arms or disarms.
 *   - arm_caught_up: placed before calls already made that change a node
 *     (a feed every source drops changes none).
 *   - Voided hits under several arms since the last check: the total must
 *     agree and the superseded ones lie in the range those arms allow.
 *   - The generator gives past groups state calls one time in eight
 *     (SIG_ORACLE_PAST_GROUPS=1: as often as other groups), as the player
 *     measured none; note 7 says what B cannot run.
 *   - Tried and dropped: reordering calls in B (see b_step).
 *
 * Changes for the amended text (b6676f4), and why:
 *   - Rule 3's trigger: before each state call, a waiting group whose tau is
 *     earlier than the call's time and whose footprint meets the call's goes
 *     in first. A state call's time for the test is N at its call; an arm
 *     call's is the smaller of its t and N (spec gap S2: at N, a past arm
 *     with an earlier time would let a waiting arm of its detect in first,
 *     against E; an ahead arm waits itself). B's tracking of its own graph's
 *     waiting uses the same times.
 *   - A disarm cancels a waiting arm of its detect made before it when the
 *     disarm's time is at or before that arm's tau (was: earlier than), and
 *     the cancelled group's footprint is the union of its other calls'.
 *     The cancel is decided when the disarm goes into R (decision on H2):
 *     a group's disarms when the group goes in (a waiting group's when it
 *     is let in), a disarm in no group at its gap's end.
 *   - Feed footprints: the sources that take the event and do not drop it
 *     (already so).
 *   - Rule 2 may split an earlier group's block (already so).
 *   - Final-only rows and events: R's order per node (already compared per
 *     node).
 *   - A disarm first meets its detect's hold up to its time: shared code
 *     (no-window disarms run in B); sig_arm_spec_cases.c cases 18 and 19.
 *   - Ahead fallbacks: an ahead arm that A counts in arm_past_full is a
 *     fallback. With room, its group's state calls stay where made and the
 *     arm waits alone (the log did not keep them); with no room (32 arms or
 *     64 calls waiting) the arm is made at once on the input known, which
 *     B's no-window graph cannot do: such cases are counted, not compared.
 *     B's graph is no-window, so it makes a group's state calls at once and
 *     waits only the arm: B tracks that (note 5 below).
 *   - Voided hits are classified over the arms made since the last check
 *     that ran (a check is skipped while B's graph waits an arm): a hit
 *     voided then is superseded when it is one of those groups' detect's and
 *     later than that group's tau.
 *   - New check switches (SIG_ORACLE_SKIP) to see what a class hides, and
 *     shape switches k (disarms in groups) and q (samples a source drops).
 *
 * A: the graph with a window, called as a caller calls it.
 * B: the same graph with no window, given the reference sequence R that
 *    this file builds from A's calls (below), through the public API only.
 *
 * Building R (the spec's rules, applied call by call, online with A):
 *   - Each call has a time: a feed its event's t, a frame its onset, a
 *     settle and an arm their t, a reset its t, a flush N; a state call in
 *     a group the group's time; any other state call N at its call. N is
 *     the input time: a reset's t after a reset, else the largest input
 *     call's time since.
 *   - An input call is a frame, a settle, or a feed that at least one
 *     source takes and does not drop. B finds the drops itself: a model
 *     of each source's per-device last time (8 devices, least recently
 *     seen evicted), cleared by a reset (a reset_trial clears only the
 *     experiment's sources), raw mouse counts never dropped. A feed that
 *     is not an input call is put at the end of R and restricts nothing.
 *   - Footprints come from the graph's description: D(n) is n and every
 *     node that reads a node in D(n) (inputs 0..3, a mux's selector; a
 *     role reads its adapter's last node). A feed's footprint is the union
 *     of D(s) over the sources that take it and do not drop it; a frame or
 *     a settle is every node; arm, disarm, set, set_data: D(node);
 *     recenter: D(the role's adapter's integrator).
 *   - Groups: a gap is a run of state calls with no input call or barrier
 *     between; it is cut after each arm; a group is the state calls from
 *     the gap's start (or after its previous arm) through an arm. Past
 *     (tau < N at the arm's call), present (=), ahead (>).
 *   - Rule 1: input calls, barriers, state calls in no group and present
 *     groups go at the end of R. A state call goes in at its call as one
 *     in no group; when an arm closes its group, the group's calls move
 *     with the arm.
 *   - Rule 2: a past group goes in as one block right after the last
 *     entry of R that is a barrier, or whose time is at or before tau and
 *     whose footprint meets the group's. It is pinned when an input entry
 *     after the last barrier and before that place has a time later than
 *     tau and a footprint that meets the group's.
 *   - Rule 3: an ahead group waits; before an input call whose time is
 *     later than tau and whose footprint meets the group's, every such
 *     waiting group goes in, by (tau, call order). A disarm(d) cancels each
 *     waiting arm of d whose tau is later than the disarm's time (decided
 *     when the disarm's time is known: at its group's arm, or at the gap's
 *     end); a barrier cancels every waiting arm, and the other calls of
 *     the waiting groups go in right before it. A cancelled arm's other
 *     calls keep waiting by the rule.
 *   - A past group that A reports as a fallback (arm_past_far, _full) goes
 *     in at its call positions (the end of R), as the spec says. A far
 *     fallback whose rule-2 place is after R's last frame or barrier (the
 *     newest checkpoint) is a failure: the window reaches it.
 *
 * Checked:
 *   - After every call of A: B has run R so far (rebuilt from the start
 *     when a past group moved calls B had run). A and B must agree on every
 *     node's value (ysig_value_of), every node's ring at recent input
 *     times (ysig_value_at), the bound channels, and every count but the
 *     window's own (arm_*, call_reentry, derived_lost) and the digest.
 *   - Per arm call, spec mode: a pinned group counted in arm_past_pinned
 *     (or a far or full fallback), no pinned count otherwise; the waiting
 *     arms cancelled equal arm_ahead_cancelled.
 *   - At the end: every hit of R (a fresh no-window run of the final R) is
 *     among A's; each extra hit A handed out is counted: superseded when
 *     it is a hit of a past group's detect, later than that group's tau,
 *     handed out before that group's arm; retracted otherwise. Rows and
 *     derived events per node: equal to R's, or counted stale (at once);
 *     in final-only mode exactly R's, and no stale count.
 *
 * What B shares with A (no-window code that B cannot test; the spec says
 * each acts the same with or without a window):
 *   1. The arm's own action at its place: the old hold met up to tau, the
 *      test and the entry rule (arm, fresh, now), the fall and the rise.
 *   2. The propagation of an arm's fall and rise through D (latches,
 *      peaks, delay, velocity, lowpass, custom ops, records, emits,
 *      detects). sig_arm_spec_cases.c checks 1 and 2 against values
 *      derived by hand from the spec.
 *   3. Waiting: B gives an ahead group right before the input call that
 *      places it; the no-window graph waits that one call and makes it
 *      there. B relies on the graph's footprint test to agree that the call
 *      meets the group.
 *   4. Grouping: B's adjacent state calls form groups in B's graph too;
 *      with no window a past group is made at its call position
 *      (arm_past_off) and a present one at once, so the order is R's.
 *   5. When B gives a past group at a place where B's input time is below
 *      tau (only disjoint input later than tau came before it in A), B's
 *      graph sees it ahead and waits. Compares are skipped until R gives
 *      an input call that meets it. If a state call that meets the group
 *      comes first, the spec's rule 3 ("a state call in no group made
 *      while a group waits has the time N < tau") does not hold in B: the
 *      case is counted as "B cannot run R" and not compared further.
 * Rows and derived events are compared per node (the spec lets calls with
 * disjoint footprints change the order of different nodes' rows).
 *
 * Spec mode: compile with -DSIG_ARM_REF_SPEC=1 against a header that has
 * ysig_flush(), desc.rows_final, desc.derived_final, desc.catchup_rows and
 * ysig_stats.arm_past_pinned, arm_ahead_cancelled. Without it (0.6.0 as
 * built) the oracle makes no flush, no final-only rows, and does not check
 * those counts.
 *
 *     gcc -std=c99 -O1 -Iinclude -DSIG_ARM_REF_SPEC=1 -o sig_arm_ref sig_arm_ref.c -lm
 *     SIG_ORACLE_CASES=20000 SIG_ORACLE_SEED=1 ./sig_arm_ref [show]
 * Prints "FAIL case N: reason" for each wrong case; exit 1 when any.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#ifndef SIG_ARM_REF_SPEC
#define SIG_ARM_REF_SPEC 0
#endif

#define MS 1000000LL
#define US 1000LL
#define MAXC 1400
#define MAXN 40
#define NCH 8
#define MAXW 64

/* ---- calls --------------------------------------------------------------- */

enum { C_FEED, C_FRAME, C_SETTLE, C_ARM, C_DISARM, C_SET, C_RECENTER, C_RESET, C_RESET_TRIAL, C_FLUSH, C_DRAIN };
typedef struct {
    int kind;
    int node, param;          /* ARM, DISARM, SET (param), RECENTER (role) */
    int ekind, ectl, dev;     /* FEED */
    int64_t t;                /* FEED, FRAME onset, SETTLE, ARM, RECENTER, RESET */
    int64_t poll;             /* FRAME */
    int chbits;               /* FRAME channel values */
    double x, y, v;
} call;

static call g_c[MAXC];
static int g_nc;

/* ---- the graph ----------------------------------------------------------- */

static ysig_node g_n[MAXN], g_ad[2];
static ysig_role g_role;
static ysig_adapter g_adp;
static int g_nn, g_rel, g_ntot;               /* experiment nodes; with the adapter's */
static int g_in[MAXN][4];                     /* combined graph's inputs (-1 none) */
static uint64_t g_D[MAXN];                    /* downstream sets */
static int g_dets[8], g_ndet, g_chnode, g_integ;
static int g_K, g_C, g_W, g_final_rows, g_final_der;
static uint64_t g_all;

/* node positions */
enum { N_S1, N_S2, N_SANY, N_SUM, N_SPEED, N_AFF, N_D1, N_D2, N_LATCH, N_D3, N_ALL, N_PEAK, N_MUX, N_REC_MUX,
       N_REC_ALL, N_EMIT_LATCH, N_DELAY, N_D4, N_REC_PEAK, N_LP, N_VEL, N_CUST, N_LATCH_DELAY, N_REC_CUST, N_CHAN,
       N_D5, N_BIND, N_PRESS, N_POFF, N_D6, N_REC_POFF, N_D7, N_EMIT_DELAY, N_REC_LP, N_EXP };

/* a custom op: a running sum of its input's lane 0 */
typedef struct { double s; } cust_st;
static void cust_reset(void* st, const double* p, int64_t t) { (void)p; (void)t; ((cust_st*)st)->s = 0; }
static void cust_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    cust_st* s = (cust_st*)st;
    (void)p; (void)n_in;
    s->s += in[0].v[0];
    out->v[0] = s->s;
}
static const ysig_op_vtable g_cust = { YSIG_OP_ABI, "runsum", { 0, 0, 0, 0 }, 0, sizeof(cust_st), NULL, 0, cust_reset, cust_sample, NULL };
static const ysig_op_vtable* const g_custs[1] = { &g_cust };

static uint64_t g_rs = 88172645463325252ull;
static uint32_t rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 11); }
static double rndf(void) { return (rnd() & 0xffffff) / (double)0x1000000; }
static int chance(int per1000) { return (int)(rnd() % 1000) < per1000; }
/* SIG_ORACLE_OFF: letters for shapes left out, to check the oracle itself:
 * p past arms, a ahead arms, g a group's state calls, l late and dropped
 * reports, r resets and reset_trial, x a callback's arm, s state calls in
 * no group, o the pressure pen's input (a stream no armed path but D6's
 * reads), m the relative-mouse graph, k disarms in groups, q samples a source drops, d users of hit outputs other than
 * latches, peaks and stateless nodes, 1 arms of other detects than D1, h holds (0.6.0's no-window arm meets
 * a hold only up to N, so B carries difference 5 at a past arm's place) */
static int off(char c) { const char* o = getenv("SIG_ORACLE_OFF"); return o && strchr(o, c) != NULL; }
/* SIG_ORACLE_SKIP: checks left out, to see what a class hides: P pinned
 * counts, C cancel counts, V voided-hit classes, R final-only rows, D
 * final-only derived events */
static int skip(char c) { const char* o = getenv("SIG_ORACLE_SKIP"); return o && strchr(o, c) != NULL; }

static void det_params(ysig_node* n, int one_lane) {
    int test = (int)(rnd() % (one_lane ? 2 : 4));
    n->p[YSIG_DETECT_TEST] = test;
    n->p[YSIG_DETECT_LANE] = one_lane ? 0 : test >= 2 ? -1 : (int)(rnd() % 3) - 1;
    n->p[YSIG_DETECT_LEVEL] = one_lane && rnd() % 2 ? 0.5 : 1 + rnd() % 6;
    n->p[YSIG_DETECT_A_X] = -3; n->p[YSIG_DETECT_A_Y] = -3; n->p[YSIG_DETECT_B_X] = 3; n->p[YSIG_DETECT_B_Y] = 3;
    if (n->p[YSIG_DETECT_LANE] < 0 && test < 2) { n->p[YSIG_DETECT_A_X] = 0; n->p[YSIG_DETECT_A_Y] = 0; }
    n->p[YSIG_DETECT_HYSTERESIS] = (rnd() & 1) ? 0.5 : 0;
    n->p[YSIG_DETECT_HOLD] = (rnd() % 3) ? (rnd() % 8) * 0.002 : 0;
    if (off('h')) n->p[YSIG_DETECT_HOLD] = 0;
    n->p[YSIG_DETECT_AT] = rnd() % 6 == 0 ? 1 : 0;
    n->p[YSIG_DETECT_START] = (rnd() % 3) ? 1 : 0;
    n->p[YSIG_DETECT_REPEAT] = rnd() & 1;
    n->p[YSIG_DETECT_ENTRY] = rnd() % 3;
    n->p[YSIG_DETECT_OUT] = (rnd() % 4) ? YSIG_OUT_HIT : YSIG_OUT_TEST;
    n->history = 64;
}

static ysig_node src(int kind, int ctl, int dev, int lanes) {
    ysig_node n = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n.lanes = (uint8_t)lanes; n.p[YSIG_SOURCE_KIND] = kind; n.p[YSIG_SOURCE_CONTROL] = ctl; n.p[YSIG_SOURCE_DEVICE] = dev;
    n.history = 1024;
    return n;
}

static void make_graph(void) {
    static const int ins[] = { N_S1, N_SANY, N_SUM, N_AFF, N_SPEED };
    int i, j, in;
    memset(g_n, 0, sizeof g_n);
    g_rel = rnd() % 4 == 0 && !off('m');
    if (g_rel) {
        memset(&g_role, 0, sizeof g_role);
        g_role.name = "hand"; g_role.type = YSIG_TYPE_POSITION2D; g_role.units = YSIG_UNITS_DISPLAY_PX; g_role.allow_relative = true;
        g_ad[0] = src(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
        g_ad[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        g_ad[1].p[0] = g_ad[1].p[1] = 1; g_ad[1].history = 1024;
        memset(&g_adp, 0, sizeof g_adp);
        g_adp.role = "hand"; g_adp.name = "mouse"; g_adp.type = YSIG_TYPE_POSITION2D_RELATIVE; g_adp.units = YSIG_UNITS_DISPLAY_PX;
        g_adp.nodes = g_ad; g_adp.n_nodes = 2;
        g_n[N_S1] = ysig_node_of(YSIG_OP_ROLE, -1, -1); g_n[N_S1].history = 1024;
        g_n[N_S2] = ysig_node_of(YSIG_OP_OFFSET, N_S1, -1); g_n[N_S2].p[0] = 1; g_n[N_S2].history = 1024;
        g_n[N_SANY] = ysig_node_of(YSIG_OP_AFFINE, N_S1, -1); g_n[N_SANY].p[YSIG_AFFINE_ANGLE] = 10; g_n[N_SANY].history = 1024;
    } else {
        g_n[N_S1] = src(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        g_n[N_S2] = src(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
        g_n[N_SANY] = src(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    }
    g_n[N_SUM] = ysig_node_of(YSIG_OP_SUM, N_S1, N_S2); g_n[N_SUM].history = 1024;
    g_n[N_SPEED] = ysig_node_of(YSIG_OP_SPEED, N_SANY, -1); g_n[N_SPEED].p[0] = 0.004; g_n[N_SPEED].history = 1024;
    g_n[N_AFF] = ysig_node_of(YSIG_OP_AFFINE, N_S1, -1); g_n[N_AFF].p[YSIG_AFFINE_ANGLE] = 30; g_n[N_AFF].history = 1024;
    g_n[N_MUX] = ysig_node_of(YSIG_OP_MUX, N_S1, N_S2); g_n[N_MUX].in[2] = N_D1; g_n[N_MUX].history = 64;
    in = ins[rnd() % 5];
    g_n[N_D1] = ysig_node_of(YSIG_OP_DETECT, in, -1); det_params(&g_n[N_D1], in == N_SPEED);
    in = ins[rnd() % 5];
    g_n[N_D2] = ysig_node_of(YSIG_OP_DETECT, in, -1); det_params(&g_n[N_D2], in == N_SPEED);
    g_n[N_LATCH] = ysig_node_of(YSIG_OP_LATCH, g_n[N_D1].in[0] == N_SPEED ? N_SANY : g_n[N_D1].in[0], N_D1); g_n[N_LATCH].history = 64;
    g_n[N_D3] = ysig_node_of(YSIG_OP_DETECT, N_LATCH, -1); det_params(&g_n[N_D3], 0);
    g_n[N_ALL] = ysig_node_of(YSIG_OP_ALL, N_D1, N_D2); g_n[N_ALL].history = 64;
    g_n[N_PEAK] = ysig_node_of(YSIG_OP_PEAK, N_SANY, N_D2); g_n[N_PEAK].history = 64;
    g_n[N_REC_MUX] = ysig_node_of(YSIG_OP_RECORD, N_MUX, -1);
    g_n[N_REC_ALL] = ysig_node_of(YSIG_OP_RECORD, N_ALL, -1); g_n[N_REC_ALL].p[0] = rnd() & 1;
    g_n[N_EMIT_LATCH] = ysig_node_of(YSIG_OP_EMIT, N_LATCH, -1);
    g_n[N_DELAY] = ysig_node_of(YSIG_OP_DELAY, N_D1, -1); g_n[N_DELAY].p[0] = (rnd() & 1) ? 0 : 0.003; g_n[N_DELAY].history = 64;
    /* D4: on all(D1, D2), or on mux(pen 1, pen 2, D1) (r12: a hit of another
     * detect that a re-arm of D1 changes) */
    if (rnd() & 1) { g_n[N_D4] = ysig_node_of(YSIG_OP_DETECT, N_ALL, -1); det_params(&g_n[N_D4], 1); }
    else { g_n[N_D4] = ysig_node_of(YSIG_OP_DETECT, N_MUX, -1); det_params(&g_n[N_D4], 0); }
    g_n[N_REC_PEAK] = ysig_node_of(YSIG_OP_RECORD, N_PEAK, -1);
    g_n[N_LP] = ysig_node_of(YSIG_OP_LOWPASS, N_D1, -1); g_n[N_LP].p[0] = 20; g_n[N_LP].history = 64;
    g_n[N_VEL] = ysig_node_of(YSIG_OP_VELOCITY, N_D1, -1); g_n[N_VEL].p[0] = 0.002; g_n[N_VEL].history = 64;
    g_n[N_CUST] = ysig_node_of(YSIG_OP_CUSTOM, N_D1, -1); g_n[N_CUST].history = 64;
    g_n[N_LATCH_DELAY] = ysig_node_of(YSIG_OP_LATCH, N_SANY, N_DELAY); g_n[N_LATCH_DELAY].history = 64;
    g_n[N_REC_CUST] = ysig_node_of(YSIG_OP_RECORD, N_CUST, -1);
    g_n[N_CHAN] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); g_n[N_CHAN].p[0] = rnd() % 2; g_n[N_CHAN].history = 64;
    g_n[N_D5] = ysig_node_of(YSIG_OP_DETECT, N_CHAN, -1); det_params(&g_n[N_D5], 1); g_n[N_D5].p[YSIG_DETECT_AT] = 1;
    g_n[N_BIND] = ysig_node_of(YSIG_OP_BIND, N_D1, -1); g_n[N_BIND].p[0] = 4;
    g_n[N_PRESS] = src(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
    g_n[N_POFF] = ysig_node_of(YSIG_OP_OFFSET, N_PRESS, -1); g_n[N_POFF].history = 256;
    g_n[N_D6] = ysig_node_of(YSIG_OP_DETECT, N_POFF, -1); det_params(&g_n[N_D6], 1);
    g_n[N_REC_POFF] = ysig_node_of(YSIG_OP_RECORD, N_POFF, -1);
    g_n[N_D7] = ysig_node_of(YSIG_OP_DETECT, N_D1, -1); det_params(&g_n[N_D7], 1);
    g_n[N_EMIT_DELAY] = ysig_node_of(YSIG_OP_EMIT, N_DELAY, -1); g_n[N_EMIT_DELAY].p[0] = 1;
    g_n[N_REC_LP] = ysig_node_of(YSIG_OP_RECORD, N_LP, -1);
    if (off('d')) {
        /* only latches, peaks and stateless nodes after a hit output (0.6.0's
         * reach of an arm's fall and rise): the others read input instead */
        g_n[N_D3].in[0] = N_SUM; det_params(&g_n[N_D3], 0);
        g_n[N_REC_MUX].in[0] = N_AFF;
        g_n[N_REC_ALL].in[0] = N_SUM;
        g_n[N_EMIT_LATCH].in[0] = N_SUM;
        g_n[N_DELAY].in[0] = N_SANY;
        g_n[N_D4].in[0] = N_SPEED; det_params(&g_n[N_D4], 1);
        g_n[N_REC_PEAK].in[0] = N_SUM;
        g_n[N_LP].in[0] = N_SANY;
        g_n[N_VEL].in[0] = N_SANY;
        g_n[N_CUST].in[0] = N_SPEED;
        g_n[N_D7].in[0] = N_SPEED;
    }
    g_nn = N_EXP;
    g_ntot = g_nn + (g_rel ? 2 : 0);
    g_chnode = N_CHAN;
    g_ndet = 0;
    g_dets[g_ndet++] = N_D1; g_dets[g_ndet++] = N_D2; g_dets[g_ndet++] = N_D3; g_dets[g_ndet++] = N_D4;
    g_dets[g_ndet++] = N_D5; g_dets[g_ndet++] = N_D6; g_dets[g_ndet++] = N_D7;
    /* the combined graph's inputs, for the downstream sets */
    for (i = 0; i < g_ntot; i++) for (j = 0; j < 4; j++) g_in[i][j] = -1;
    for (i = 0; i < g_nn; i++) for (j = 0; j < 4; j++) g_in[i][j] = g_n[i].in[j];
    for (i = 0; i < g_nn; i++)   /* ysig_node_of leaves in[2..3] -1; a mux's selector is in[2] */
        for (j = 0; j < 4; j++) if (g_in[i][j] >= g_nn) g_in[i][j] = -1;
    if (g_rel) {
        g_in[N_S1][0] = g_nn + 1;              /* a role reads its adapter's last node */
        g_in[g_nn + 1][0] = g_nn;              /* integrate reads the mouse source */
        g_integ = g_nn + 1;
    }
    g_all = g_ntot >= 64 ? ~0ull : ((1ull << g_ntot) - 1);
    for (i = 0; i < g_ntot; i++) g_D[i] = 1ull << i;
    for (;;) {
        int changed = 0;
        for (i = 0; i < g_ntot; i++)
            for (j = 0; j < 4; j++) {
                int m = g_in[i][j];
                if (m < 0) continue;
                /* i reads m: every set containing m contains i */
                {
                    int s;
                    for (s = 0; s < g_ntot; s++)
                        if ((g_D[s] >> m & 1) && !(g_D[s] >> i & 1)) { g_D[s] |= 1ull << i; changed = 1; }
                }
            }
        if (!changed) break;
    }
}

/* ---- A and B ------------------------------------------------------------- */

static uint64_t g_ar_a[(24u << 20) / 8], g_ar_b[(24u << 20) / 8];
static float g_ch_a[NCH], g_ch_b[NCH];
static ysig_graph g_a, g_b;

typedef struct { int node; int64_t t; ysig_row r; int at; } rowrec;   /* at: A's call that handed it out */
static int g_cur_ci;   /* the call being made (A's rows and events record it) */
typedef struct { rowrec* v; int n, cap; } rowlist;
static rowlist g_rows_a, g_rows_b, *g_rows;
static int g_reenter;   /* A's on_row tries an arm (refused, not in R) */
static void on_row(void* u, const ysig_row* r) {
    rowlist* l = g_rows;
    (void)u;
    if (l->n < l->cap) { l->v[l->n].node = r->node; l->v[l->n].r = *r; l->v[l->n].r.name = NULL; l->v[l->n].at = g_cur_ci; }
    l->n++;
    if (g_reenter && l == &g_rows_a) { g_reenter = 0; (void)ysig_arm(&g_a, N_D1, r->value.t); }
}

static int open_graph(ysig_graph* g, uint64_t* ar, float* ch, int window) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = g_n; d.n_nodes = g_nn; d.channels = ch; d.n_channels = NCH; d.arena = ar;
    d.on_row = on_row;
    d.custom = g_custs; d.n_custom = 1;
    if (window) {
        d.catchup_frames = g_K; d.catchup_calls = g_C; d.catchup_writes = g_W;
#if SIG_ARM_REF_SPEC
        d.rows_final = g_final_rows != 0;
        d.derived_final = g_final_der != 0;
#endif
    }
    if (g_rel) { d.roles = &g_role; d.n_roles = 1; d.adapters = &g_adp; d.n_adapters = 1; }
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_ar_a) {
        char e[256];
        e[0] = 0;
        ysig_check(&d, e, sizeof e);
        fprintf(stderr, "sig_arm_ref: arena %zu: %s\n", d.arena_size, e);
        return 0;
    }
    if (!ysig_open(g, &d)) { fprintf(stderr, "sig_arm_ref: open: %s\n", ysig_error(g)); return 0; }
    {
        /* the open's own reset can queue an entry-now hit; the case's
         * first call, a reset, clears it in R: not a hit of R */
        ysig_hit h[64];
        yin_event e[64];
        while (ysig_detected(g, h, 64) > 0) { }
        while (ysig_derived(g, e, 64) > 0) { }
    }
    return 1;
}

typedef struct { ysig_hit h; int at; } hitrec;   /* at: A's call index of the drain */
static hitrec g_ha[8192], g_hb[8192], g_hr[8192];
static int g_nha, g_nhb, g_nhr;   /* g_hr: the hits of R so far (B's run) */
typedef struct { yin_event e; int at; } derrec;   /* at: A's call that handed it out */
static derrec g_da[8192], g_db[8192];
static int g_nda, g_ndb;

static void drain(ysig_graph* g, hitrec* hr, int* nh, derrec* dr, int* nd, int at, int hits_too) {
    ysig_hit h[64];
    yin_event e[64];
    int k, m;
    if (hits_too)
        while ((m = ysig_detected(g, h, 64)) > 0)
            for (k = 0; k < m; k++) if (hr && *nh < 8192) { hr[*nh].h = h[k]; hr[*nh].at = at; (*nh)++; }
    while ((m = ysig_derived(g, e, 64)) > 0)
        for (k = 0; k < m; k++) if (dr && *nd < 8192) { dr[*nd].e = e[k]; dr[*nd].at = g_cur_ci; (*nd)++; }
}

static void do_call(ysig_graph* g, const call* c) {
    yin_event e;
    ysig_frame_in f;
    float v[NCH];
    int k;
    switch (c->kind) {
    case C_FEED:
        memset(&e, 0, sizeof e);
        e.t = c->t; e.type = YIN_SAMPLE; e.kind = (uint8_t)c->ekind; e.control = (uint32_t)c->ectl; e.device = (uint32_t)c->dev;
        e.x = (float)c->x; e.y = (float)c->y; e.value = (float)c->v; e.stamp = YIN_TIER_2;
        ysig_feed(g, &e);
        break;
    case C_FRAME:
        memset(&f, 0, sizeof f);
        for (k = 0; k < NCH; k++) v[k] = (float)((c->chbits >> k) & 1);
        f.index = c->t / MS; f.onset_ns = c->t; f.poll_ns = c->poll; f.has_poll = true; f.values = v; f.n_values = NCH;
        ysig_frame(g, &f);
        break;
    case C_SETTLE: ysig_settle(g, c->t); break;
    case C_ARM: ysig_arm(g, c->node, c->t); break;
    case C_DISARM: ysig_disarm(g, c->node); break;
    case C_SET: ysig_set(g, c->node, c->param, c->v); break;
    case C_RECENTER: ysig_recenter(g, 0, c->x, c->y, c->t); break;
    case C_RESET: ysig_reset(g, c->t); break;
    case C_RESET_TRIAL: ysig_reset_trial(g, c->t); break;
#if SIG_ARM_REF_SPEC
    case C_FLUSH: (void)ysig_flush(g); break;
#endif
    default: break;
    }
}

/* ---- the source model (drops) --------------------------------------------- */

typedef struct { int n; uint32_t dev[8]; int64_t last[8]; uint64_t used[8], clock; } srcm;
static srcm g_sm[MAXN];

static int src_takes(int s, const call* c) {
    const ysig_node* n = s < g_nn ? &g_n[s] : &g_ad[s - g_nn];
    if (n->op != YSIG_OP_SOURCE) return 0;
    return c->ekind == (int)n->p[YSIG_SOURCE_KIND] && c->ectl == (int)n->p[YSIG_SOURCE_CONTROL] &&
           (n->p[YSIG_SOURCE_DEVICE] == 0 || c->dev == (int)n->p[YSIG_SOURCE_DEVICE]);
}
/* the footprint of a feed: D(s) over the sources that take it and do not
 * drop it (the model advances) */
static uint64_t feed_fp(const call* c) {
    uint64_t fp = 0;
    int s;
    for (s = 0; s < g_ntot; s++) {
        srcm* m = &g_sm[s];
        int d, counts = c->ekind == YIN_KIND_MOUSE && c->ectl == YIN_AXIS_DELTA;
        if (!src_takes(s, c)) continue;
        for (d = 0; d < m->n && m->dev[d] != (uint32_t)c->dev; d++) { }
        if (d == m->n) {
            if (m->n < 8) d = m->n++;
            else { int q; for (d = 0, q = 1; q < 8; q++) if (m->used[q] < m->used[d]) d = q; }
            m->dev[d] = (uint32_t)c->dev;
            m->last[d] = c->t;
        } else if (c->t < m->last[d] && !counts) {
            continue;   /* dropped */
        }
        m->used[d] = ++m->clock;
        if (c->t > m->last[d]) m->last[d] = c->t;
        fp |= g_D[s];
    }
    return fp;
}
static void src_reset(int exp_only) {
    int s;
    for (s = 0; s < g_ntot; s++) if (!exp_only || s < g_nn) memset(&g_sm[s], 0, sizeof g_sm[s]);
}

/* ---- R ------------------------------------------------------------------- */

enum { E_INPUT = 1, E_BARRIER = 2, E_STATE = 4, E_NEUTRAL = 8, E_DROPPED = 16 };
typedef struct { int ci, flags, grp; int64_t time; uint64_t fp; } rent;
static rent g_R[MAXC * 2];
static int g_nR;

typedef struct {
    int calls[16], n, arm_ci, node, cancelled, cls;   /* cls 0 past 1 present 2 ahead */
    int64_t tau;
    uint64_t fp;
    int pinned, fallback, rerun, p1_done, pflag, wait_r, by_closure;
} group;
static group g_gr[MAXC];
static int g_ngr;
static int g_wait[MAXC], g_nwait;            /* waiting groups */
static int g_pend[64], g_npend;              /* the open gap's state calls since its last arm */
static int64_t g_N;
static int g_cancelled;                      /* arms cancelled so far (rule 3) */
static int g_call_grp[MAXC];                 /* the group of each call, -1 */
static uint64_t g_call_fp[MAXC];
static int64_t g_call_time[MAXC];
static int g_call_flags[MAXC];
static int64_t g_call_N[MAXC];          /* N at the call (rule 3's test for a state call) */
static int64_t g_call_nf[MAXC];         /* N(F(c)) at the call (25837ee: a state call's time outside a group) */

static uint64_t state_fp(const call* c) {
    switch (c->kind) {
    case C_ARM: case C_DISARM: case C_SET: return g_D[c->node];
    case C_RECENTER: return g_rel ? g_D[g_integ] : 0;
    default: return 0;
    }
}

static int g_r_moved;   /* an insertion before R's end this step */
static void r_insert(int at, const rent* e) {
    if (at < g_nR) g_r_moved = 1;
    memmove(&g_R[at + 1], &g_R[at], sizeof(rent) * (size_t)(g_nR - at));
    g_R[at] = *e;
    g_nR++;
}
static void r_remove_ci(int ci) {
    int i;
    for (i = g_nR - 1; i >= 0; i--)
        if (g_R[i].ci == ci) { memmove(&g_R[i], &g_R[i + 1], sizeof(rent) * (size_t)(g_nR - i - 1)); g_nR--; return; }
}
/* ---- late input (d906e19, "Late input", proposed for 0.7.0) ------------- */
static int g_late;            /* SIG_ORACLE_LATE=1: R by that text */
static int g_noA;             /* late mode without A's checks (no SIG_ORACLE_LATE_A) */
static char g_call_late[MAXC];   /* (late modes) the call was a late call: an input or a state call placed before calls already in R */
static struct {
    long late, far, barrier, frames, cond2, l_new, l_new_nfa, ooo_in, ooo_grp, by_frame, by_fb, by_other, closure, late_state, late_by_disarm, cancel_eq, late_state_span, closure_escape, cancel_letin;
} g_lc;                       /* per case: what the text counts, and what it leaves out */
/* N(F): after the last barrier, the latest time of an input in R that meets
 * F (a frame or a settle meets every F); a reset sets it to its t; a flush
 * keeps it (the text says "after the last barrier"; it does not say what a
 * flush leaves: taken as N does, finding L8) */
static int64_t nf(uint64_t fp) {
    int i;
    int64_t n = INT64_MIN;
    for (i = g_nR - 1; i >= 0; i--) {
        const rent* e = &g_R[i];
        if (e->flags & E_BARRIER) {
            int kd = g_c[e->ci].kind;
            if (kd == C_RESET || kd == C_RESET_TRIAL) { if (g_c[e->ci].t > n) n = g_c[e->ci].t; break; }
            continue;
        }
        if ((e->flags & E_INPUT) && (e->fp & fp) && e->time > n) n = e->time;
    }
    return n;
}
static rent mk_ent(int ci, int grp, int64_t time) {
    rent e;
    e.ci = ci; e.grp = grp; e.time = time; e.fp = g_call_fp[ci]; e.flags = g_call_flags[ci];
    return e;
}

/* a disarm's cancel of waiting arms of its node with tau later than t */
/* (b6676f4) a disarm whose time is at or before tau cancels the waiting
 * arms of its detect made before it; the group's other calls keep waiting
 * with the union of their own footprints */
static void cancel_by_disarm(int node, int64_t t, int disarm_ci) {
    int w, k;
    if (g_late == 4) {
        /* (9834173, U1 and U5) the cancel by key: a disarm cancels an arm of
         * its detect made before it, waiting or already let in, exactly when
         * its key is smaller than the arm's group key: time earlier than
         * tau (at one time the arm, made before, has the smaller key) */
        int gi;
        for (gi = 0; gi < g_ngr; gi++) {
            group* G = &g_gr[gi];
            int waits = 0, q;
            if (G->cancelled || G->node != node || G->arm_ci > disarm_ci || !(t < G->tau)) continue;
            for (q = 0; q < g_nwait; q++) if (g_wait[q] == gi) waits = 1;
            if (!waits) {
                int in = 0;
                for (q = 0; q < g_nR; q++) if (g_R[q].ci == G->arm_ci) in = 1;
                if (!in) continue;   /* not made (never let in): nothing in R */
                r_remove_ci(G->arm_ci);
                g_r_moved = 1;
                g_lc.cancel_letin++;
            }
            G->cancelled = 1;
            g_cancelled++;
            G->fp = 0;
            for (k = 0; k < G->n; k++) if (G->calls[k] != G->arm_ci) G->fp |= g_call_fp[G->calls[k]];
        }
        return;
    }
    if (g_late == 3) {
        /* a group the T1 closure let in before input on its footprint
         * passed its tau: a disarm at or before tau, made after its arm,
         * would have cancelled it had it still waited (finding U5) */
        int gi;
        for (gi = 0; gi < g_ngr; gi++) {
            const group* G = &g_gr[gi];
            if (G->by_closure && !G->cancelled && G->node == node && t <= G->tau && G->arm_ci < disarm_ci) { g_lc.closure_escape++; break; }
        }
    }
    for (w = 0; w < g_nwait; w++) {
        group* G = &g_gr[g_wait[w]];
        if (G->cancelled || G->node != node || t > G->tau || G->arm_ci > disarm_ci) continue;
        if (g_late == 3 && t == G->tau) g_lc.cancel_eq++;   /* rule 3 cancels at t = tau; the key order puts the arm first */
        G->cancelled = 1;
        g_cancelled++;
        if (getenv("SIG_ORACLE_DEBUG")) printf("  R: disarm call %d (time %.3f) cancels group %d (arm call %d, node %d, tau %.3f)\n", disarm_ci, t / 1e6, (int)(G - g_gr), G->arm_ci, G->node, G->tau / 1e6);
        G->fp = 0;
        for (k = 0; k < G->n; k++) if (G->calls[k] != G->arm_ci) G->fp |= g_call_fp[G->calls[k]];
    }
}
/* the open gap ends (an input call or a barrier): its trailing state calls
 * are in no group; their disarms are at N */
static void call_triggers(int ci, int64_t t, int incl, int pos_before);
static void gap_end(void) {
    int i;
    /* (H9) the gap's calls go into R now, in no group: their triggers */
    for (i = 0; i < g_npend; i++) if (g_c[g_pend[i]].kind != C_DISARM) call_triggers(g_pend[i], g_late ? g_call_time[g_pend[i]] : g_N, 0, -1);
    for (i = 0; i < g_npend; i++)
        if (g_c[g_pend[i]].kind == C_DISARM) {
            /* (S1) it takes effect at the end of its gap: after the gap's
             * other calls */
            rent e;
            int64_t td = g_late ? g_call_time[g_pend[i]] : g_N;
            r_remove_ci(g_pend[i]);
            e = mk_ent(g_pend[i], -1, td);
            g_R[g_nR++] = e;
            call_triggers(g_pend[i], td, 0, g_nR - 1);
            cancel_by_disarm(g_c[g_pend[i]].node, td, g_pend[i]);
        }
    g_npend = 0;
}
/* (f2f0e0d text) superseded: a hit later than tau of a detect that the
 * group arms or disarms */
static int group_touches(const group* G, int node) {
    int k;
    if (G->node == node) return 1;
    for (k = 0; k < G->n; k++) if (g_c[G->calls[k]].kind == C_DISARM && g_c[G->calls[k]].node == node) return 1;
    return 0;
}
/* the cancels of a group's disarms, at their time (the group's tau), when
 * the group goes into R */
static void group_disarms(int gi) {
    group* G = &g_gr[gi];
    int k;
    for (k = 0; k < G->n; k++)
        if (G->calls[k] != G->arm_ci && g_c[G->calls[k]].kind == C_DISARM) cancel_by_disarm(g_c[G->calls[k]].node, G->tau, G->calls[k]);
}
static int cmp_wait(const void* x, const void* y) {
    const group* a = &g_gr[*(const int*)x];
    const group* b = &g_gr[*(const int*)y];
    if (a->tau != b->tau) return a->tau < b->tau ? -1 : 1;
    return a->arm_ci - b->arm_ci;
}
static int put_group_at(int gi, int with_arm, int pos) {
    group* G = &g_gr[gi];
    int i;
    for (i = 0; i < G->n; i++) {
        rent e;
        if (G->calls[i] == G->arm_ci && !with_arm) continue;
        e = mk_ent(G->calls[i], gi, G->tau);
        r_insert(pos++, &e);
    }
    return pos;
}
static void put_group_end(int gi, int with_arm) { (void)put_group_at(gi, with_arm, g_nR); }
/* rule 3: the waiting groups that a call with time t and footprint fp lets
 * in, by (tau, call order), put in at pos (R's end for an input). incl
 * (S3): an arm call lets in one whose tau is at or before t */
static void call_triggers(int ci, int64_t t, int incl, int pos_before);
static int r_index_ci(int ci);
/* a group call's time for rule 3's test (085bbb6 text): a state call's is
 * N at its call, in a past group the group's tau; an arm's the smaller of
 * its t and N, an ahead arm's its tau (H12: it lets in when its group goes
 * in, at its tau) */
static int64_t trig_time(const group* G, int ci) {
    int64_t tn = g_call_N[ci];
    if (G->cls == 0 && !getenv("SIG_ORACLE_PASTN")) return G->tau;
    if (ci == G->arm_ci) return G->cls == 2 ? G->tau : (G->tau < tn ? G->tau : tn);
    return tn;
}
/* S3: at equal times the earlier call first. An arm call lets in a
 * waiting group at its own time only when that group's arm call is
 * earlier; one made later goes in right after the arm (g_pw_lo/hi bound
 * the arm calls of the equal-time ones; -1 none) */
static int g_pw_lo = -1, g_pw_hi = -1;
static char g_pw_after[MAXC];   /* groups let in right after an arm (per case) */
static void place_waiting_at(int64_t t, uint64_t fp, int incl, int pos) {
    int sel[MAXC], ns = 0, w, k;
    for (w = 0; w < g_nwait; w++) {
        group* G = &g_gr[g_wait[w]];
        int eq = incl && G->tau == t && (g_pw_hi < 0 || G->arm_ci < g_pw_hi) && (g_pw_lo < 0 || G->arm_ci > g_pw_lo);
        if (g_pw_lo >= 0 && !eq) continue;   /* the second pass takes the later equal ones only */
        if ((G->tau < t || eq) && (G->fp & fp)) sel[ns++] = g_wait[w];
    }
    g_pw_lo = g_pw_hi = -1;
    if (!ns) return;
    if (g_late >= 3) {
        /* (7a17176, T1) a call that lets W in lets in, right before it,
         * every waiting group that meets W with a smaller key, transitively */
        int grew = 1;
        while (grew) {
            grew = 0;
            for (w = 0; w < g_nwait; w++) {
                const group* V = &g_gr[g_wait[w]];
                int in = 0, j;
                for (j = 0; j < ns; j++) if (sel[j] == g_wait[w]) in = 1;
                if (in || V->cancelled) continue;
                for (j = 0; j < ns; j++) {
                    const group* W = &g_gr[sel[j]];
                    if ((V->fp & W->fp) && (V->tau < W->tau || (V->tau == W->tau && V->arm_ci < W->arm_ci))) { g_gr[g_wait[w]].by_closure = 1; sel[ns++] = g_wait[w]; grew = 1; g_lc.closure++; break; }
                }
            }
        }
    }
    qsort(sel, (size_t)ns, sizeof(int), cmp_wait);
    for (k = 0; k < ns; k++) {
        int q;
        /* one an earlier one's call let in is in already */
        for (q = 0; q < g_nwait && g_wait[q] != sel[k]; q++) { }
        if (q == g_nwait) continue;
        pos = put_group_at(sel[k], !g_gr[sel[k]].cancelled, pos);
        for (w = q = 0; w < g_nwait; w++) if (g_wait[w] != sel[k]) g_wait[q++] = g_wait[w];
        g_nwait = q;
        /* (H9 text, c2c5459) the group's calls go into R now: their own
         * triggers, each before its call (time: N at the call, an arm's the
         * smaller of its t and that N, at or before) */
        {
            const group* G = &g_gr[sel[k]];
            int c2, last = -1;
            for (c2 = 0; c2 < G->n; c2++) {
                int ci = G->calls[c2], arm = ci == G->arm_ci;
                if (arm && G->cancelled) continue;
                call_triggers(ci, trig_time(G, ci), arm, -1);
            }
            for (c2 = 0; c2 < G->n; c2++) { int r = r_index_ci(G->calls[c2]); if (r < g_nR && r > last) last = r; }
            if (last >= 0) pos = last + 1;
            /* past the equal-time ones its arm let in after it (S3) */
            while (pos < g_nR && g_R[pos].grp >= 0 && g_pw_after[g_R[pos].grp]) pos++;
        }
        /* (H2 decision) a disarm's cancel is decided when it goes into R */
        group_disarms(sel[k]);
    }
}
static void place_waiting(int64_t t, uint64_t fp) { place_waiting_at(t, fp, 0, g_nR); }
static int r_index_ci(int ci) {
    int i;
    for (i = g_nR - 1; i >= 0; i--) if (g_R[i].ci == ci) return i;
    return g_nR;
}
/* (H9 decision) a state call's rule-3 trigger takes effect when the call
 * itself goes into R: the waiting groups it lets in go in right before
 * it. Calls in no group go in at their gap's end (their time N); a present
 * group's calls at its arm; nothing of an ahead group before it goes in. */
static void call_triggers(int ci, int64_t t, int incl, int pos_before) {
    int after = pos_before < 0;
    if (pos_before < 0) pos_before = r_index_ci(ci);
    if (incl && g_c[ci].kind == C_ARM && !getenv("SIG_ORACLE_EQBEFORE")) {
        int r;
        g_pw_hi = ci;
        place_waiting_at(t, g_call_fp[ci], incl, pos_before);
        r = r_index_ci(ci);
        if (r < g_nR) {
            int w, k2;
            char was[MAXC];
            for (w = 0; w < g_nwait; w++) was[w] = 0;
            {
                int nw0 = g_nwait, ids[MAXC];
                for (w = 0; w < nw0; w++) ids[w] = g_wait[w];
                g_pw_lo = ci;
                place_waiting_at(t, g_call_fp[ci], incl, r + 1);
                /* the ones that went in: no longer waiting */
                for (w = 0; w < nw0; w++) {
                    for (k2 = 0; k2 < g_nwait && g_wait[k2] != ids[w]; k2++) { }
                    if (k2 == g_nwait) g_pw_after[ids[w]] = 1;
                }
            }
            (void)was;
        }
        (void)after;
        return;
    }
    place_waiting_at(t, g_call_fp[ci], incl, pos_before);
}
/* a group's calls go into R: their triggers. A state call's time for the
 * test is N at its call, the arm's the smaller of its t and N (S2), at or
 * before for an arm (S3). The waiting groups go in right before the call
 * where the call goes into R: a past group's at its place (the
 * coordinator's ruling on rule 3, after c2c5459) */
static void group_triggers(int gi, int at_end) {
    group* G = &g_gr[gi];
    int k;
    for (k = 0; k < G->n; k++) {
        int ci = G->calls[k], arm = ci == G->arm_ci;
        call_triggers(ci, trig_time(G, ci), arm, at_end ? g_nR : -1);
    }
}

/* Rule 2: the place of a past group; pinned is set */
static int g_may_pin;   /* (8594b73) an input of any footprint, later than tau, before the place */
static int rule2_place(const group* G, int* pinned) {
    int p, i, lb = -1;
    for (p = g_nR; p > 0; p--) {
        const rent* e = &g_R[p - 1];
        if (e->flags & E_BARRIER) break;
        if ((e->fp & G->fp) && e->time <= G->tau && !(e->flags & E_NEUTRAL)) break;
    }
    for (i = 0; i < p; i++) if (g_R[i].flags & E_BARRIER) lb = i;
    *pinned = 0;
    g_may_pin = 0;
    {
        /* an extra pin (8594b73) is accepted only for an input older than
         * anything the window could hold: before the catchup_frames-th
         * newest frame or barrier of R (the window's oldest checkpoint is at
         * most that far back) */
        int seen = 0, bound = -1;
        for (i = g_nR - 1; i >= 0; i--)
            if (g_c[g_R[i].ci].kind == C_FRAME || (g_R[i].flags & E_BARRIER)) { if (++seen == g_K) { bound = i; break; } }
        for (i = lb + 1; i < p && i < bound; i++) if ((g_R[i].flags & E_INPUT) && g_R[i].time > G->tau) g_may_pin = 1;
    }
    for (i = lb + 1; i < p; i++)
        if ((g_R[i].flags & E_INPUT) && g_R[i].time > G->tau && (g_R[i].fp & G->fp)) {
            if (getenv("SIG_ORACLE_DEBUG") && !*pinned) printf("  pin: R[%d] call %d (time %.3f) before place %d after R[%d] call %d kind %d time %.3f\n", i, g_R[i].ci, g_R[i].time / 1e6, p, p - 1, p > 0 ? g_R[p - 1].ci : -1, p > 0 ? g_c[g_R[p - 1].ci].kind : -1, p > 0 ? g_R[p - 1].time / 1e6 : 0.0);
            *pinned = 1;
        }
    return p;
}
static int last_ckpt_pos(void) {
    int i;
    for (i = g_nR - 1; i >= 0; i--) if (g_c[g_R[i].ci].kind == C_FRAME || (g_R[i].flags & E_BARRIER)) return i;
    return -1;
}

/* ---- the case ------------------------------------------------------------ */

static const char* g_why;
static char g_whybuf[256];
static long g_case;
static int g_b_cannot, g_bw_n, g_cannot_why, g_h9;
static int64_t g_bw_tau[MAXC];
static uint64_t g_bw_fp[MAXC];
static int g_bw_grp[MAXC];
static int g_bran[MAXC * 2], g_nbran;   /* the R entries B has run, by call index */
static int64_t g_bN;
static int64_t g_probe[16];
static int g_nprobe;

static void fail(const char* fmt, const char* a, long b) {
    if (g_why || g_noA) return;
    snprintf(g_whybuf, sizeof g_whybuf, fmt, a, b);
    g_why = g_whybuf;
}

/* give B one entry of R (B drains after each call: B never loses a hit) */
static void b_issue(const rent* e) {
    const call* c = &g_c[e->ci];
    int w, k;
    /* note 5: an arm that B's no-window graph sees ahead (B's N below tau)
     * waits there, alone (b6676f4: with no window a group's state calls
     * stay where made); it goes in before the first input later than tau,
     * or state call at N later than tau, that meets its footprint */
    int bw_new = SIG_ARM_REF_SPEC && c->kind == C_ARM && g_bN < e->time;
    if (c->kind == C_DRAIN) return;
    if (e->flags & E_DROPPED) return;   /* (late_barrier) not in R: B never gets it */
    /* a disarm of a group that B's graph sees ahead is made at once there
     * (a no-window graph cannot take back a state call), meeting the hold
     * only up to B's input time, where R meets it up to tau: B cannot give
     * R's result (spec gap S1) */
    /* (S1 decision) a disarm takes effect at the end of its gap: in a group
     * that B's graph sees ahead it waits with the arm and meets the hold up
     * to tau, as R has it */
    /* an arm that waits in B's graph too goes in with the others by (tau,
     * call order), as R has them: no conflict */
    int dis_waits = 0;
    if (SIG_ARM_REF_SPEC && c->kind == C_DISARM && e->grp >= 0 && !g_gr[e->grp].cancelled && !getenv("SIG_ORACLE_NODISWAIT")) {
        /* (S1) a disarm whose gap in B ends with its group's arm, which B's
         * graph sees ahead: it waits with the arm and goes in with it */
        int q, me = -1, arm = -1, inp = 0;
        for (q = 0; q < g_nR; q++) { if (&g_R[q] == e) me = q; if (me >= 0 && g_R[q].ci == g_gr[e->grp].arm_ci) { arm = q; break; } }
        if (me >= 0 && arm > me) {
            for (q = me + 1; q < arm; q++) if (g_R[q].flags & (E_INPUT | E_BARRIER)) inp = 1;
            if (!inp && g_bN < g_gr[e->grp].tau) dis_waits = 1;
        }
    }
    if (c->kind == C_DISARM && getenv("SIG_ORACLE_DISDBG") && atol(getenv("SIG_ORACLE_DISDBG")) == g_case)
        printf("  B disarm call %d node %d grp %d (cls %d cancelled %d tau %.3f) R time %.3f, B's N %.3f, waits with its arm %d, B waiting %d\n", e->ci, c->node, e->grp,
               e->grp >= 0 ? g_gr[e->grp].cls : -1, e->grp >= 0 ? g_gr[e->grp].cancelled : -1, e->grp >= 0 ? g_gr[e->grp].tau / 1e6 : 0.0, e->time / 1e6, g_bN / 1e6, dis_waits, g_bw_n);
    if (g_bw_n && (e->flags & (E_STATE | E_BARRIER)) && !bw_new && !dis_waits) {
        for (w = k = 0; w < g_bw_n; w++) {
            int meets = (e->fp & g_bw_fp[w]) != 0;
            int64_t tt = c->kind == C_ARM && e->time < g_bN ? e->time : g_bN;   /* the trigger's time (S2) */
            if (e->flags & E_BARRIER) {   /* B's graph would cancel it */
                if (!g_b_cannot) {
                    g_cannot_why = 1;
                    if (getenv("SIG_ORACLE_DEBUG") || getenv("SIG_ORACLE_DEBUG8")) printf("  case %ld B cannot: barrier call %d while an arm at %.3f waits (B's N %.3f), grp %d arm call %d cls %d cancelled %d wait_r %d p1_done %d pflag %d\n", g_case, e->ci, g_bw_tau[w] / 1e6, g_bN / 1e6,
                        g_bw_grp[w], g_gr[g_bw_grp[w]].arm_ci, g_gr[g_bw_grp[w]].cls, g_gr[g_bw_grp[w]].cancelled, g_gr[g_bw_grp[w]].wait_r, g_gr[g_bw_grp[w]].p1_done, g_gr[g_bw_grp[w]].pflag);
                }
                g_b_cannot = 1;
                continue;
            }
            if (meets && tt > g_bw_tau[w]) continue;                     /* let in first, as R has it */
            if (meets) {                                                  /* R has it first; B's graph would not */
                if (!g_b_cannot) {
                    g_cannot_why = c->kind == C_ARM ? 2 : e->grp >= 0 ? (g_gr[e->grp].cls == 0 ? 3 : 4) : 5;
                    if (getenv("SIG_ORACLE_DEBUG"))
                        printf("  B cannot: call %d (kind %d node %d t %.3f, group %d cls %d) at B's N %.3f meets an arm waiting at %.3f\n", e->ci, c->kind, c->node, e->time / 1e6,
                               e->grp, e->grp >= 0 ? g_gr[e->grp].cls : -1, g_bN / 1e6, g_bw_tau[w] / 1e6);
                }
                g_b_cannot = 1;
            }
            g_bw_tau[k] = g_bw_tau[w]; g_bw_fp[k] = g_bw_fp[w]; g_bw_grp[k] = g_bw_grp[w]; k++;
        }
        g_bw_n = k;
    }
    /* a disarm of a group whose arm a barrier cancelled goes in at the
     * group's tau, later than B's input time: B's graph makes it at its N
     * and meets a hold only up to there (S1 has R meet it up to tau) */
    if (c->kind == C_DISARM && e->grp >= 0 && (getenv("SIG_ORACLE_R8OLD") ? e->time > g_bN : e->time != g_bN) && !bw_new && !g_b_cannot) {
        int w2, waits = 0;
        for (w2 = 0; w2 < g_bw_n; w2++) if (g_bw_grp[w2] == e->grp) waits = 1;
        if (!waits && g_gr[e->grp].cancelled && g_n[c->node].p[YSIG_DETECT_HOLD] > 0) {
            g_b_cannot = 1; g_cannot_why = 8;
            if (getenv("SIG_ORACLE_DEBUG8")) printf("  case %ld reason 8: disarm call %d grp %d (arm call %d cls %d tau %.3f pflag %d p1_done %d fb %d) B's N %.3f\n", g_case, e->ci, e->grp, g_gr[e->grp].arm_ci, g_gr[e->grp].cls, g_gr[e->grp].tau / 1e6, g_gr[e->grp].pflag, g_gr[e->grp].p1_done, g_gr[e->grp].fallback, g_bN / 1e6);
        }
    }
    /* a state call in no group right before a group's calls (no input or
     * barrier between): B's graph puts it in that group's gap, and times a
     * disarm by the group (R: N at its call, S1) */
    if (c->kind == C_ARM && e->grp >= 0 && !g_b_cannot && !getenv("SIG_ORACLE_NOR9")) {
        /* the gap this arm closes in B's graph: back to the last input,
         * barrier or arm. A disarm there of another group (or of none)
         * takes this arm's time in B's graph (S1) */
        int me = (int)(e - g_R), q;
        for (q = me - 1; q >= 0; q--) {
            const rent* x = &g_R[q];
            if ((x->flags & (E_INPUT | E_BARRIER)) || g_c[x->ci].kind == C_ARM) break;
            if (g_c[x->ci].kind == C_DISARM && x->grp != e->grp && x->time != e->time) { g_b_cannot = 1; g_cannot_why = 9; break; }
        }
    }
    g_rows = &g_rows_b;
    do_call(&g_b, c);
    drain(&g_b, g_hr, &g_nhr, NULL, NULL, 0, 1);
    if (bw_new && g_bw_n < MAXC) {
        /* what waits in B's graph: the arm, and its group's disarms (S1) */
        uint64_t f = e->fp;
        if (e->grp >= 0) {
            const group* G = &g_gr[e->grp];
            int q;
            for (q = 0; q < G->n; q++) if (g_c[G->calls[q]].kind == C_DISARM) f |= g_call_fp[G->calls[q]];
        }
        g_bw_tau[g_bw_n] = e->time; g_bw_fp[g_bw_n] = f; g_bw_grp[g_bw_n] = e->grp; g_bw_n++;
    }
    if ((e->flags & E_INPUT) && !g_b_cannot)
        /* an input at or before a waiting arm's tau that meets it: R has it
         * after the arm (a late report, taken as before the arm, O3); B's
         * graph takes it before the arm is made, and the arm's entry then
         * reads it (the o1 shape) */
        for (w = 0; w < g_bw_n; w++)
            if (e->time <= g_bw_tau[w] && (e->fp & g_bw_fp[w])) {
                g_b_cannot = 1; g_cannot_why = 7;
                if (getenv("SIG_ORACLE_DEBUG7")) printf("  case %ld reason 7: input call %d t %.3f, waiting group %d (arm call %d, tau %.3f), B's N %.3f%s\n", g_case, e->ci, e->time / 1e6, g_bw_grp[w],
                                                        g_bw_grp[w] >= 0 ? g_gr[g_bw_grp[w]].arm_ci : -1, g_bw_tau[w] / 1e6, g_bN / 1e6, e->time == g_bw_tau[w] ? " (equal times)" : "");
                break;
            }
    if (e->flags & E_INPUT) {
        /* B's graph lets in the waiting arms this input meets. One that stays
         * while a later one (in R) that shares a node with it goes in would
         * reverse their order: R has the earlier first (the H14 review: arm31
         * waiting on node 6's output, arm6 let in first by an input that does
         * not reach node 31) */
        for (w = 0; w < g_bw_n; w++) {
            int in_w = e->time > g_bw_tau[w] && (e->fp & g_bw_fp[w]);
            int w2;
            if (in_w) continue;
            for (w2 = w + 1; w2 < g_bw_n; w2++)
                if (e->time > g_bw_tau[w2] && (e->fp & g_bw_fp[w2]) && (g_bw_fp[w] & g_bw_fp[w2]) && !g_b_cannot) {
                    g_b_cannot = 1; g_cannot_why = 6;
                    if (getenv("SIG_ORACLE_DEBUG6")) {
                        const group* a = &g_gr[g_bw_grp[w]]; const group* b = &g_gr[g_bw_grp[w2]];
                        printf("  case %ld reason 6: input call %d t %.3f; stays grp %d (arm call %d cls %d tau %.3f fb %d), goes grp %d (arm call %d cls %d tau %.3f fb %d), B's N %.3f\n",
                               g_case, e->ci, e->time / 1e6, g_bw_grp[w], a->arm_ci, a->cls, a->tau / 1e6, a->fallback, g_bw_grp[w2], b->arm_ci, b->cls, b->tau / 1e6, b->fallback, g_bN / 1e6);
                    }
                }
        }
        if (e->time > g_bN) g_bN = e->time;
        for (w = k = 0; w < g_bw_n; w++)
            if (!(e->time > g_bw_tau[w] && (e->fp & g_bw_fp[w]))) { g_bw_tau[k] = g_bw_tau[w]; g_bw_fp[k] = g_bw_fp[w]; g_bw_grp[k] = g_bw_grp[w]; k++; }
        g_bw_n = k;
    }
    if (c->kind == C_RESET || c->kind == C_RESET_TRIAL) { g_bN = c->t; g_bw_n = 0; }
}

/* B gives R's entries in R's order. (Tried and dropped, both: moving a
 * later input that shares no node with what it passes in front of a group
 * B's graph would wait, and moving such a group behind such an input. Each
 * compared no more cases and made false failures, rings read differently
 * by ysig_value_at(); not understood, so not kept.) */
static char g_bdone[MAXC], g_bseen[MAXC];
static void b_step(int i) {
    b_issue(&g_R[i]);
}

static void b_rebuild(void) {
    int i;
    g_rows_b.n = 0;
    g_bw_n = 0;
    g_nhr = 0;
    if (!open_graph(&g_b, g_ar_b, g_ch_b, 0)) exit(2);
    memset(g_ch_b, 0, sizeof g_ch_b);
    memset(g_bdone, 0, sizeof g_bdone);
    memset(g_bseen, 0, sizeof g_bseen);
    g_bN = INT64_MIN;
    g_nbran = 0;
    for (i = 0; i < g_nR; i++) { b_step(i); g_bran[g_nbran++] = g_R[i].ci; }
}
/* B to R so far: append when R extends what B ran, else run R again */
static void b_sync(void) {
    int i;
    if (g_nbran <= g_nR) {
        for (i = 0; i < g_nbran && g_bran[i] == g_R[i].ci; i++) { }
        if (i == g_nbran) {
            for (; i < g_nR; i++) { b_step(i); g_bran[g_nbran++] = g_R[i].ci; }
            return;
        }
    }
    b_rebuild();
}

static int hit_eq(const ysig_hit* x, const ysig_hit* y);
/* the hits A handed out that R so far does not have (match R's hits to
 * A's latest copies: an earlier equal copy was an earlier arm's) */
static char g_void_prev[8192];
static int voided(char* out) {
    static char used[8192];
    int i, j, n = 0;
    /* a copy once voided stays voided: R's later equal hit (another pass of
     * the same event, say) matches a newer copy, not it (the o2 review: a
     * copy re-matched and voided again was counted twice) */
    memset(used, 0, (size_t)g_nha);
    for (i = 0; i < g_nha; i++) if (g_void_prev[i]) used[i] = 2;
    for (j = 0; j < g_nhr; j++)
        for (i = g_nha - 1; i >= 0; i--) if (!used[i] && hit_eq(&g_ha[i].h, &g_hr[j].h)) { used[i] = 1; break; }
    for (i = 0; i < g_nha; i++) { out[i] = used[i] != 1; n += out[i]; }
    return n;
}

/* two values the same as far as the public struct defines them: lanes
 * beyond `lanes` carry nothing (a peak's reset leaves its lane 1 as it was;
 * the H15 candidate was this) */
static int value_ne(const ysig_value* a, const ysig_value* b) {
    int k;
    if (a->lanes != b->lanes || a->t != b->t || a->t_oldest != b->t_oldest || a->device != b->device || a->src != b->src ||
        a->src_oldest != b->src_oldest || a->tier != b->tier || a->unc_us != b->unc_us) return 1;
    for (k = 0; k < a->lanes && k < YSIG_LANES; k++) if (memcmp(&a->v[k], &b->v[k], sizeof(double))) return 1;
    return 0;
}
static const char* cmp_values(void) {
    int i, k;
    ysig_stats sa, sb;
    for (i = 0; i < g_ntot; i++) {
        const ysig_value* va = ysig_value_of(&g_a, i);
        const ysig_value* vb = ysig_value_of(&g_b, i);
        if (value_ne(va, vb)) {
            if (getenv("SIG_ORACLE_DEBUG")) printf("  node %d op %d: A (%g,%g) lanes %d t %.3f t_old %.3f dev %u tier %d src %d unc %d | B (%g,%g) lanes %d t %.3f t_old %.3f dev %u tier %d src %d unc %d\n", i, i < g_nn ? g_n[i].op : -1,
                                                   va->v[0], va->v[1], va->lanes, va->t / 1e6, va->t_oldest / 1e6, va->device, va->tier, va->src, va->unc_us,
                                                   vb->v[0], vb->v[1], vb->lanes, vb->t / 1e6, vb->t_oldest / 1e6, vb->device, vb->tier, vb->src, vb->unc_us);
            return "a node's value";
        }
        for (k = 0; k < g_nprobe; k++) {
            ysig_value xa, xb;
            int ra = ysig_value_at(&g_a, i, g_probe[k], &xa), rb = ysig_value_at(&g_b, i, g_probe[k], &xb);
            if (ra != rb || (ra >= 0 && value_ne(&xa, &xb))) {
                if (getenv("SIG_ORACLE_DEBUG"))
                    printf("  ring of node %d at %.3f: A rc %d (%g,%g) t %.3f | B rc %d (%g,%g) t %.3f\n", i, g_probe[k] / 1e6, ra, xa.v[0], xa.v[1], xa.t / 1e6, rb, xb.v[0], xb.v[1], xb.t / 1e6);
                return "a ring read (ysig_value_at)";
            }
        }
    }
    if (memcmp(g_ch_a, g_ch_b, sizeof g_ch_a)) return "a bound channel";
    ysig_get_stats(&g_a, &sa);
    ysig_get_stats(&g_b, &sb);
#define CK(f) if (sa.f != sb.f) { if (getenv("SIG_ORACLE_DEBUG")) printf("  stats." #f ": A %lld B %lld\n", (long long)sa.f, (long long)sb.f); return "stats." #f; }
    CK(fed); CK(taken); CK(dropped_order); CK(hits_lost); CK(rows); CK(hits); CK(ring_short); if (getenv("SIG_ORACLE_HSKCHECK")) CK(history_skipped);
    CK(order_evicted); CK(late_poll); CK(late_poll_max_ns); CK(time_clamped);
#undef CK
    return NULL;
}

static void add_probe(int64_t t) {
    int k;
    for (k = 0; k < g_nprobe; k++) if (g_probe[k] == t) return;
    if (g_nprobe < 16) g_probe[g_nprobe++] = t;
    else { memmove(g_probe, g_probe + 1, sizeof(int64_t) * 15); g_probe[15] = t; }
}

/* After call ci (B has run R so far): a hit A handed out at this call must
 * be one of R's; a hit handed out before that R no longer has was voided by
 * this call, which must be a past arm, counted: superseded when its own
 * detect's and later than tau, retracted otherwise. */
static ysig_stats g_chk_st;   /* A's counts at the last hand_check that ran */
static long g_lv_old;         /* voids after a late call checked by the pre-ruling text (no late_hits_voided) */
static int g_chk_ci;          /* its call */
static void hand_check(int ci, int mark, const ysig_stats* s0, const ysig_stats* s1) {
    static char now[8192];
    const call* c = &g_c[ci];
    long esup = 0, eret = 0, fresh = 0;
    int i, k, arms = 0;
    (void)s0;
    if (g_why || g_bw_n || g_b_cannot) return;   /* checked at the next call that can */
    /* the arms made since the last check: a hit voided by one of them is
     * superseded when it is that group's detect's and later than its tau */
    for (k = 0; k < g_ngr; k++) if (g_gr[k].arm_ci > g_chk_ci && g_gr[k].arm_ci <= ci) arms++;
    voided(now);
    for (i = 0; i < g_nha; i++) {
        int sup = 0;
        if (!now[i] || g_void_prev[i]) continue;
        if (i >= mark) {
            fresh++;
            if (getenv("SIG_ORACLE_DEBUG")) {
                int q;
                printf("  fresh at call %d: hit node %d t %.3f found %.3f v (%g,%g); R's of the node:", ci, g_ha[i].h.node, g_ha[i].h.t / 1e6, g_ha[i].h.t_found / 1e6, g_ha[i].h.value.v[0], g_ha[i].h.value.v[1]);
                for (q = 0; q < g_nhr; q++) if (g_hr[q].h.node == g_ha[i].h.node) {
                    const ysig_value* x = &g_hr[q].h.value; const ysig_value* y = &g_ha[i].h.value; int l;
                    printf(" %.3f/%.3f", g_hr[q].h.t / 1e6, g_hr[q].h.t_found / 1e6);
                    if (g_hr[q].h.t == g_ha[i].h.t) {
                        printf(" [R lanes %d t %lld to %lld src %d/%d tier %d dev %u unc %d v", x->lanes, (long long)x->t, (long long)x->t_oldest, x->src, x->src_oldest, x->tier, x->device, x->unc_us);
                        for (l = 0; l < YSIG_LANES; l++) printf(" %.17g", x->v[l]);
                        printf(" | A lanes %d t %lld to %lld src %d/%d tier %d dev %u unc %d v", y->lanes, (long long)y->t, (long long)y->t_oldest, y->src, y->src_oldest, y->tier, y->device, y->unc_us);
                        for (l = 0; l < YSIG_LANES; l++) printf(" %.17g", y->v[l]);
                        printf("]");
                    }
                }
                printf("\n    A's of the node:");
                for (q = 0; q < g_nha; q++) if (g_ha[q].h.node == g_ha[i].h.node) printf(" %.3f/%.3f@%d%s", g_ha[q].h.t / 1e6, g_ha[q].h.t_found / 1e6, g_ha[q].at, now[q] ? "v" : "");
                printf("\n");
            }
            continue;
        }
        for (k = 0; k < g_ngr && !sup; k++)
            if (g_gr[k].arm_ci > g_chk_ci && g_gr[k].arm_ci <= ci && g_gr[k].cls == 0 && group_touches(&g_gr[k], g_ha[i].h.node) && g_ha[i].h.t > g_gr[k].tau) sup = 1;
        if (sup) esup++; else eret++;
        if (getenv("SIG_ORACLE_DEBUG")) printf("  voided at call %d: hit node %d t %.3f found %.3f\n", ci, g_ha[i].h.node, g_ha[i].h.t / 1e6, g_ha[i].h.t_found / 1e6);
    }
    if (g_nha > 0) memcpy(g_void_prev, now, (size_t)g_nha);
    if (fresh) {
        snprintf(g_whybuf, sizeof g_whybuf, "a hit handed out (at a %s) that R does not have",
                 c->kind == C_ARM ? (g_gr[g_call_grp[ci]].cls == 0 ? "past arm" : g_gr[g_call_grp[ci]].cls == 1 ? "present arm" : "ahead arm")
                 : c->kind == C_DRAIN ? "drain" : "drain before a barrier or a full queue");
        g_why = g_whybuf;
        return;
    }
    {
        /* (late modes) the late calls since the last check */
        int lates = 0;
        long dlate = 0;
        for (k = g_chk_ci + 1; k <= ci && k < (int)sizeof g_call_late; k++) if (k >= 0 && g_call_late[k]) lates++;
#ifdef SIG_HAS_LATE_VOIDED
        dlate = (long)(s1->late_hits_voided - g_chk_st.late_hits_voided);
#endif
        if (g_late && lates && (esup || eret || dlate)) {
            /* (the coordinator's ruling, 2026-11-15) a hit handed out that a
             * late call voids counts in late_hits_voided, whatever its time;
             * arm_hits_superseded and _retracted stay for voids an arm or a
             * disarm causes. With arms since the last check too, only the
             * total can be checked. */
            long dsup = s1->arm_hits_superseded - g_chk_st.arm_hits_superseded, dret = s1->arm_hits_retracted - g_chk_st.arm_hits_retracted;
#ifdef SIG_HAS_LATE_VOIDED
            int ok = arms ? dsup + dret + dlate == esup + eret : (dlate == esup + eret && dsup + dret == 0);
#else
            int ok = dsup + dret == esup + eret;   /* (a header before the count: the text before the ruling) */
            g_lv_old++;
#endif
            if (!ok && !skip('V')) {
                snprintf(g_whybuf, sizeof g_whybuf, "hits voided after a late call: late_hits_voided +%ld, superseded +%ld, retracted +%ld; voided %ld (arms since %d)",
                         dlate, dsup, dret, esup + eret, arms);
                g_why = g_whybuf;
                return;
            }
            g_chk_st = *s1;
            g_chk_ci = ci;
            return;
        }
    }
    if (!arms && (esup || eret)) {
        g_why = "a hit handed out earlier voided with no arm made since";
        return;
    }
    {
        /* with one arm since the last check each voided hit's class is
         * known; with several (checks skipped while an arm waited in B) a
         * hit that one of them may supersede may have been voided by another
         * first: the total must agree, the superseded ones within range */
        long dsup = s1->arm_hits_superseded - g_chk_st.arm_hits_superseded, dret = s1->arm_hits_retracted - g_chk_st.arm_hits_retracted;
        long smin = arms == 1 ? esup : 0;
        if (!skip('V') && (dsup + dret != esup + eret || dsup < smin || dsup > esup)) {
            snprintf(g_whybuf, sizeof g_whybuf, "voided hits: superseded +%ld retracted +%ld, the spec +%ld and +%ld",
                     dsup, dret, esup, eret);
            g_why = g_whybuf;
            return;
        }
    }
    g_chk_st = *s1;
    g_chk_ci = ci;
}


/* ---- the proposed boundary (arm_unverified) ------------------------------- *
 * SIG_ORACLE_PRED: evaluate the predicate proposed for sig.md on what the
 * header knows (footprints, call times, N, R, the waiting groups):
 *  P1, a past group G placed at p by rule 2 where the input time of R before
 *      p (since the last barrier) is below tau: exact only when R has, after
 *      G, an input later than tau that meets F(G), and no call between G and
 *      it is a barrier or a state call that meets F(G). Checked when G is
 *      placed, and again whenever a later call goes in before R's end.
 *  P2, a feed that meets F(G) of a group G made since the last barrier whose
 *      arm call came before the feed, with a time before tau (a late report
 *      for an armed detect). */
static int g_pred, g_pred_why;
/* for the predicate's refinement: the input time at a group's place, and
 * the R index of the first input after it later than its tau that meets
 * its footprint (g_nR: none) */
static int64_t p1_np(int gi, int* lastp) {
    int i, first = -1, last = -1;
    int64_t np = INT64_MIN;
    for (i = 0; i < g_nR; i++) {
        if ((g_R[i].flags & E_BARRIER) && first < 0) {
            int kd = g_c[g_R[i].ci].kind;
            if (kd == C_RESET || kd == C_RESET_TRIAL) np = g_c[g_R[i].ci].t;
        }
        if (g_R[i].grp == gi) { if (first < 0) first = i; last = i; }
        else if (first < 0 && (g_R[i].flags & E_INPUT) && g_R[i].time > np) np = g_R[i].time;
    }
    *lastp = last;
    return np;
}
static int p1_let(int gi, int from) {
    int k;
    for (k = from; k < g_nR; k++) {
        if (g_R[k].flags & E_BARRIER) return g_nR;
        if ((g_R[k].flags & E_INPUT) && g_R[k].time > g_gr[gi].tau && (g_R[k].fp & g_gr[gi].fp)) return k;
    }
    return g_nR;
}
/* the exception's group: its arm alone (SIG_ORACLE_P1ARMONLY=1), or its
 * arm and disarms (a disarm waits with its arm, S1); x one of them */
static int g_exc_dis_case;   /* the extension (a disarm of a waiting later group) was taken this case */
static int p1_arm_dis(int og, int xci) {
    int q;
    if (getenv("SIG_ORACLE_P1ARMONLY")) return g_gr[og].n == 1 && g_c[xci].kind == C_ARM;
    for (q = 0; q < g_gr[og].n; q++) { int kd = g_c[g_gr[og].calls[q]].kind; if (kd != C_ARM && kd != C_DISARM) return 0; }
    return g_c[xci].kind == C_ARM || g_c[xci].kind == C_DISARM;
}
static int p1_bad(int gi) {
    const group* G = &g_gr[gi];
    int i, lb = -1, first = -1, last = -1, k;
    int64_t np = INT64_MIN;
    for (i = 0; i < g_nR; i++) {
        if ((g_R[i].flags & E_BARRIER) && first < 0) {
            /* a reset sets the input time to its t; a flush keeps it */
            int kd = g_c[g_R[i].ci].kind;
            lb = i;
            if (kd == C_RESET || kd == C_RESET_TRIAL) np = g_c[g_R[i].ci].t;
        }
        if (g_R[i].grp == gi) { if (first < 0) first = i; last = i; }
        else if (first < 0 && (g_R[i].flags & E_INPUT) && g_R[i].time > np) np = g_R[i].time;
    }
    (void)lb;
    if (first < 0) return -1;             /* not in R yet (waits), or cancelled */
    /* placed inside another group's block (between its calls): with no
     * window, calls group by their gaps, so the block's leading calls join
     * this group's gap */
    if (first > 0 && last + 1 < g_nR && g_R[first - 1].grp >= 0 && g_R[first - 1].grp != gi) {
        int og = g_R[first - 1].grp;
        for (k = last + 1; k < g_nR; k++) if (g_R[k].grp == og) return 7;
    }
    /* (4) right after a call in no group (the gap that the next input
     * closes): a graph with no window puts that call in G's gap */
    if (!getenv("SIG_ORACLE_NOC4")) {
        /* (4) a disarm of another group or of none in the gap that G's arm
         * closes with no window (back to the last input, barrier or arm) */
        int ar = -1;
        for (k = first; k <= last; k++) if (g_R[k].ci == G->arm_ci) ar = k;
        if (ar >= 0)
            for (k = ar - 1; k >= 0; k--) {
                const rent* x = &g_R[k];
                if ((x->flags & (E_INPUT | E_BARRIER)) || g_c[x->ci].kind == C_ARM) break;
                if (g_c[x->ci].kind == C_DISARM && x->grp != gi) return 8;
            }
    }
    /* (2c, any N(G)) a cancelled group's disarm acts at tau, which a graph
     * with no window cannot give unless tau is the input time */
    if (G->cancelled && np != G->tau && !getenv("SIG_ORACLE_C2COLD"))
        for (k = 0; k < G->n; k++) if (g_c[G->calls[k]].kind == C_DISARM) return 5;
    if (np >= G->tau) return 0;           /* the input time at its place is past tau: made at once */
    g_gr[gi].wait_r = 1;                  /* it waits at its place in R */
    if (G->cancelled)                     /* its disarm acts at tau, past the input time (S1) */
        for (k = 0; k < G->n; k++) if (g_c[G->calls[k]].kind == C_DISARM) return 5;
    for (k = last + 1; k < g_nR; k++) {
        const rent* x = &g_R[k];
        if (x->flags & E_BARRIER) return 1;
        if ((x->flags & E_INPUT) && x->time > G->tau && (x->fp & G->fp)) {
            /* it must meet the arm's own footprint (and its group's
             * disarms'): with no window the group's other calls are made
             * where they are, and only these keep the arm waiting */
            uint64_t fa = G->cancelled ? 0 : g_D[G->node];
            int q;
            for (q = 0; q < G->n; q++) if (g_c[G->calls[q]].kind == C_DISARM) fa |= g_call_fp[G->calls[q]];
            return (x->fp & fa) || !fa ? 0 : 6;
        }
        if ((x->flags & E_STATE) && x->grp != gi && (x->fp & G->fp)) {
            /* another arm alone that waits at its place too and that the
             * same input lets in, after this one: B's graph lets both in
             * there, in this order (SIG_ORACLE_P1STRICT=1: not taken) */
            int og = x->grp, ol;
            if (og >= 0 && !getenv("SIG_ORACLE_P1STRICT") && !g_gr[og].cancelled && p1_arm_dis(og, x->ci) &&
                (g_gr[og].tau > G->tau || (g_gr[og].tau == G->tau && g_gr[og].arm_ci > G->arm_ci)) &&
                p1_np(og, &ol) < g_gr[og].tau && ol >= k && p1_let(og, ol + 1) == p1_let(gi, last + 1) && p1_let(gi, last + 1) < g_nR) {
                if (g_c[x->ci].kind == C_DISARM) g_exc_dis_case = 1;
                continue;
            }
            return 2;
        }
    }
    for (k = first; k <= last; k++) if (g_R[k].grp != gi && (g_R[k].flags & E_STATE) && (g_R[k].fp & G->fp)) return 2;
    return 3;                             /* no input yet that lets it in */
}
static long g_pg_n, g_pg_f, g_pc_n[3], g_pc_f[3];   /* groups made (not fallen back), flagged; by class */
static void pred_check(int ci) {
    const call* c = &g_c[ci];
    int k, lastbar = -1;
    if (!getenv("SIG_ORACLE_PRED")) return;
    for (k = 0; k < g_nc && k < ci; k++) if (g_c[k].kind == C_RESET || g_c[k].kind == C_RESET_TRIAL || g_c[k].kind == C_FLUSH) lastbar = k;
    for (k = 0; k < g_ngr; k++) {
        group* G = &g_gr[k];
        int r;
        if (G->pflag) continue;   /* a fallen-back group too: its other calls still go into R */
        if (!g_r_moved && G->p1_done) continue;
        r = p1_bad(k);
        /* SIG_ORACLE_LAZY=1: a group that waits in R with no input yet to
         * let it in is decided later, when one comes (or never) */
        if (r == 3 && getenv("SIG_ORACLE_LAZY")) r = -1;
        if (getenv("SIG_ORACLE_P1DBG") && atol(getenv("SIG_ORACLE_P1DBG")) == g_case)
            printf("  P1 step %d grp %d (arm call %d cls %d tau %.3f fp %llx node %d) -> %d\n", ci, k, G->arm_ci, G->cls, G->tau / 1e6, (unsigned long long)G->fp, G->node, r);
        if (r >= 0) G->p1_done = 1;
        if (r > 0) { G->pflag = 1; if (!g_pred) { g_pred = 1; g_pred_why = r; } }
    }
    if (c->kind == C_FEED && (g_call_flags[ci] & E_INPUT) && !getenv("SIG_ORACLE_NOP2"))
        for (k = 0; k < g_ngr; k++) {
            group* G = &g_gr[k];
            if (G->arm_ci < lastbar || G->arm_ci > ci || G->cancelled || G->pflag || G->fallback) continue;
            if ((getenv("SIG_ORACLE_P2STRICT") ? c->t < G->tau : c->t <= G->tau) && (g_call_fp[ci] & G->fp) && r_index_ci(G->arm_ci) < g_nR && (G->wait_r || getenv("SIG_ORACLE_P2ALL"))) {
                G->pflag = 1;
                if (!g_pred) { g_pred = 1; g_pred_why = 4; }
            }
        }
}
static void pred_tally(void) {
    int k;
    if (getenv("SIG_ORACLE_LAZY"))   /* still waiting at the end: counted then */
        for (k = 0; k < g_ngr; k++) if (!g_gr[k].pflag && g_gr[k].wait_r && !g_gr[k].p1_done) g_gr[k].pflag = 1;
    for (k = 0; k < g_ngr; k++) if (!g_gr[k].fallback) {
        g_pg_n++; g_pc_n[g_gr[k].cls]++;
        if (g_gr[k].pflag) { g_pg_f++; g_pc_f[g_gr[k].cls]++; }
    }
}

/* (late text) rule 4 as amended: an input goes right after the last call
 * of R that must come before it: a barrier, or a call at or before its time
 * whose footprint meets its own (a group's calls are one block). Waiting
 * groups it lets in go right before it. Counts: input_late_barrier (time
 * before the last barrier's), input_late_far (place before the window: the
 * catchup_frames-th newest frame or barrier of R, as for pins),
 * input_late_frames (frames after the place), condition 2 (a group already
 * let in after the place that the input would let in), and what the text
 * leaves out: an input of R before the place, later than it and meeting
 * it (R not in time order for a node; findings L1, L2) */
static int64_t p1_np(int gi, int* lastp);
static int p1_let(int gi, int from);
static int late_window_pos(void) {
    int i, seen = 0;
    for (i = g_nR - 1; i >= 0; i--)
        if (g_c[g_R[i].ci].kind == C_FRAME || (g_R[i].flags & E_BARRIER)) { if (++seen == g_K) return i; }
    return -1;
}
static void late_input(int ci, uint64_t fp) {
    const call* c = &g_c[ci];
    int pos, i, lb = -1, anchor_ci = -1, late = 0;
    int64_t t = c->t;
    for (pos = g_nR; pos > 0; pos--) {
        const rent* e = &g_R[pos - 1];
        if (e->flags & E_BARRIER) break;
        if (!(e->flags & E_NEUTRAL) && e->time <= t && (e->fp & fp)) {
            /* a group's calls are one block: after the whole block */
            if (e->grp >= 0) { int q = pos; while (q < g_nR && g_R[q].grp == e->grp) q++; pos = q; }
            break;
        }
    }
    for (i = pos; i < g_nR; i++) if (!(g_R[i].flags & E_NEUTRAL) && (g_R[i].fp & fp) && g_R[i].time > t) late = 1;
    for (i = 0; i < pos; i++) if (g_R[i].flags & E_BARRIER) lb = i;
    if (late) {
        g_lc.late++;
        if (lb >= 0 && g_R[lb].time > t) g_lc.barrier++;
        else if (pos <= late_window_pos()) g_lc.far++;
        for (i = pos; i < g_nR; i++) if (g_c[g_R[i].ci].kind == C_FRAME) { g_lc.frames++; break; }
        /* condition 2: a group already in R after the place that this input,
         * later than its tau and meeting it, would let in */
        for (i = pos; i < g_nR; i++) if (g_R[i].grp >= 0 && g_gr[g_R[i].grp].cls == 2 && g_gr[g_R[i].grp].tau < t && (g_R[i].fp & fp)) { g_lc.cond2++; break; }
    }
    /* what the text leaves out: R keeps an input later than t that meets
     * this one before its place (after the last barrier) */
    for (i = lb + 1; i < pos; i++)
        if ((g_R[i].flags & E_INPUT) && g_R[i].time > t && (g_R[i].fp & fp)) {
            g_lc.ooo_in++;
            {
                const rent* b4 = pos > 0 ? &g_R[pos - 1] : NULL;
                if (b4 && g_c[b4->ci].kind == C_FRAME) g_lc.by_frame++;
                else if (b4 && b4->grp >= 0 && g_gr[b4->grp].fallback) g_lc.by_fb++;
                else g_lc.by_other++;
            }
            if (getenv("SIG_ORACLE_LATEDBG") && atol(getenv("SIG_ORACLE_LATEDBG")) == g_case)
                printf("  late input call %d t %.3f placed at %d of %d after call %d (kind %d t %.3f); R[%d] call %d kind %d t %.3f before it meets it\n", ci, t / 1e6, pos, g_nR,
                       pos > 0 ? g_R[pos - 1].ci : -1, pos > 0 ? g_c[g_R[pos - 1].ci].kind : -1, pos > 0 ? g_R[pos - 1].time / 1e6 : 0.0, i, g_R[i].ci, g_c[g_R[i].ci].kind, g_R[i].time / 1e6);
            break;
        }
    if (pos < g_nR) anchor_ci = g_R[pos].ci;
    place_waiting_at(t, fp, 0, pos);
    if (anchor_ci >= 0) {
        for (pos = 0; pos < g_nR && g_R[pos].ci != anchor_ci; pos++) { }
        /* the waiting groups went in right before the anchor; the input goes after them */
    } else pos = g_nR;
    {
        rent e = mk_ent(ci, -1, t);
        r_insert(pos, &e);
    }
    /* condition 3: the input becomes G's L. The text retests 2 (b) only;
     * 2 (a) needs the new L to meet F_a(G) too (finding L5) */
    if (pos < g_nR - 1) {
        int gi;
        for (gi = 0; gi < g_ngr; gi++) {
            const group* G = &g_gr[gi];
            int last;
            if (G->fallback || G->cls != 0 || p1_np(gi, &last) >= G->tau || last < 0 || last >= pos) continue;
            if (p1_let(gi, last + 1) == pos) {
                uint64_t fa = G->cancelled ? 0 : g_D[G->node];
                int q;
                for (q = 0; q < G->n; q++) if (g_c[G->calls[q]].kind == C_DISARM) fa |= g_call_fp[G->calls[q]];
                g_lc.l_new++;
                if (fa && !(fp & fa)) g_lc.l_new_nfa++;
            }
        }
    }
    if (t > g_N) g_N = t;
    add_probe(t);
}

/* (25837ee) R as a time order over meeting pairs: key (time, an input
 * first, call order); a group's calls one block at its tau, keyed by its
 * arm; a state call outside a group at N(F(c)) at its call. */
typedef struct { int64_t t; int rank; int order, sub; } okey;
static okey r_key(const rent* e) {
    okey k;
    const call* c = &g_c[e->ci];
    k.sub = e->ci;
    if (e->grp >= 0 && !(g_late >= 3 || getenv("SIG_ORACLE_KEY3"))) { k.t = g_gr[e->grp].tau; k.rank = 1; k.order = g_gr[e->grp].arm_ci; return k; }
    if (g_late >= 3 || getenv("SIG_ORACLE_KEY3")) {
        /* (7a17176) classes at one time: a feed, then a frame or a settle,
         * then a state call or a group; a plain disarm at N (the ruling) */
        /* a feed no source keeps (T6: older than its device's last) is not
         * in R: it stays at its call position, N at its call, so a graph
         * that gets R's calls drops it as the header did */
        if (e->flags & E_NEUTRAL) { k.t = g_call_N[e->ci]; k.rank = 0; k.order = e->ci; return k; }
        if (e->flags & E_INPUT) { k.t = c->t; k.rank = (c->kind == C_FRAME || c->kind == C_SETTLE) ? 1 : 0; k.order = e->ci; return k; }
        if (e->grp >= 0) { k.t = g_gr[e->grp].tau; k.rank = 2; k.order = g_gr[e->grp].arm_ci; return k; }
        k.t = c->kind == C_DISARM ? g_call_N[e->ci] : g_call_nf[e->ci]; k.rank = 2; k.order = e->ci;
        return k;
    }
    if (e->flags & E_NEUTRAL) { k.t = g_late >= 2 ? g_call_N[e->ci] : c->t; k.rank = 0; k.order = e->ci; return k; }
    if (e->flags & E_INPUT) { k.t = c->t; k.rank = 0; k.order = e->ci; return k; }
    k.t = g_call_nf[e->ci]; k.rank = 1; k.order = e->ci;
    return k;
}
static int okey_lt(okey a, okey b) {
    if (a.t != b.t) return a.t < b.t;
    if (a.rank != b.rank) return a.rank < b.rank;
    if (a.order != b.order) return a.order < b.order;
    return a.sub < b.sub;
}
/* SIG_ORACLE_ORDERCMP=1: at each case's end, every meeting pair of R (in
 * one barrier segment) whose order is not the time order's, by class */
static long g_oc[8];   /* per case: 0 frame or settle, 1 feed, 2 input at a group's tau, 3 input after a group, 4 group after a later input, 5 state call outside a group, 6 group and group, 7 other */
static void order_cmp(void) {
    int i, j;
    memset(g_oc, 0, sizeof g_oc);
    for (i = 0; i < g_nR; i++) {
        const rent* a = &g_R[i];
        if (a->flags & (E_BARRIER | E_NEUTRAL)) continue;
        if (a->grp >= 0 && g_gr[a->grp].fallback) continue;   /* a fallen-back group is the header's, not R's */
        for (j = i + 1; j < g_nR; j++) {
            const rent* b = &g_R[j];
            okey ka, kb;
            if (b->flags & E_BARRIER) break;
            if ((b->flags & E_NEUTRAL) || !(a->fp & b->fp)) continue;
            if (b->grp >= 0 && g_gr[b->grp].fallback) continue;
            if (a->grp >= 0 && a->grp == b->grp) continue;
            ka = r_key(a); kb = r_key(b);
            if (!okey_lt(kb, ka)) continue;
            /* b comes after a in R, the time order puts it first */
            {
                int ai = (a->flags & E_INPUT) != 0, bi = (b->flags & E_INPUT) != 0, cls;
                int bk = g_c[b->ci].kind;
                if (ai && bi) cls = (bk == C_FRAME || bk == C_SETTLE || g_c[a->ci].kind == C_FRAME || g_c[a->ci].kind == C_SETTLE) ? 0 : 1;
                else if (bi && a->grp >= 0) cls = kb.t == ka.t ? 2 : 3;
                else if (ai && b->grp >= 0) cls = 4;
                else if ((a->grp < 0 && !ai) || (b->grp < 0 && !bi)) cls = 5;
                else if (a->grp >= 0 && b->grp >= 0) cls = 6;
                else cls = 7;
                g_oc[cls]++;
                if (getenv("SIG_ORACLE_OCDBG") && atol(getenv("SIG_ORACLE_OCDBG")) == g_case && g_oc[cls] <= 3)
                    printf("  class %d: R[%d] call %d kind %d grp %d (cls %d pinned %d) key %.3f/%d before R[%d] call %d kind %d grp %d (cls %d) key %.3f/%d\n", cls, i, a->ci, g_c[a->ci].kind, a->grp,
                           a->grp >= 0 ? g_gr[a->grp].cls : -1, a->grp >= 0 ? g_gr[a->grp].pinned : -1, ka.t / 1e6, ka.rank, j, b->ci, g_c[b->ci].kind, b->grp, b->grp >= 0 ? g_gr[b->grp].cls : -1, kb.t / 1e6, kb.rank);
            }
        }
    }
}

/* (25837ee, SIG_ORACLE_LATE=2) R as a time order: after each call, each
 * barrier segment of R is sorted stably by (time, an input first, call
 * order; a group by its arm), a group's calls kept in their block order.
 * Which calls R holds (lets in, cancels) is decided as before, with the
 * per-footprint classes. Returns 1 when the order changed. */
static int r_sort(void) {
    static rent tmp[MAXC * 2];
    static okey key[MAXC * 2];
    int i, a = 0, changed = 0;
    for (i = 0; i <= g_nR; i++) {
        if (i == g_nR || (g_R[i].flags & E_BARRIER)) {
            /* stable insertion sort of [a, i) */
            int n = i - a, x, y;
            for (x = 0; x < n; x++) { tmp[x] = g_R[a + x]; key[x] = r_key(&tmp[x]); key[x].sub = x; }
            for (x = 1; x < n; x++) {
                rent e = tmp[x];
                okey k = key[x];
                for (y = x - 1; y >= 0 && okey_lt(k, key[y]); y--) { tmp[y + 1] = tmp[y]; key[y + 1] = key[y]; }
                tmp[y + 1] = e; key[y + 1] = k;
            }
            for (x = 0; x < n; x++) { if (g_R[a + x].ci != tmp[x].ci) changed = 1; g_R[a + x] = tmp[x]; }
            a = i + 1;
        }
    }
    return changed;
}
static int g_lci[MAXC];   /* per group: the call that is its L (-1 none) */
static int g_late_state_ci = -1;   /* (T3) the late state call of this step */
static void l_track(int ci) {
    int gi;
    for (gi = 0; gi < g_ngr; gi++) {
        const group* G = &g_gr[gi];
        int last, li, lci;
        if (G->fallback || p1_np(gi, &last) >= G->tau || last < 0) { g_lci[gi] = -1; continue; }
        li = p1_let(gi, last + 1);
        lci = li < g_nR ? g_R[li].ci : -1;
        if (lci == ci && g_lci[gi] != ci && g_lci[gi] >= 0) {
            /* a late input became G's L (condition 2): 2 (a) needs it to meet F_a */
            uint64_t fa = G->cancelled ? 0 : g_D[G->node];
            int q;
            for (q = 0; q < G->n; q++) if (g_c[G->calls[q]].kind == C_DISARM) fa |= g_call_fp[G->calls[q]];
            g_lc.l_new++;
            if (fa && !(g_call_fp[ci] & fa)) g_lc.l_new_nfa++;
        }
        g_lci[gi] = lci;
    }
}
static void late_input2(int ci, uint64_t fp) {
    const call* c = &g_c[ci];
    int i, lb = -1, pos;
    int64_t t = c->t;
    for (i = 0; i < g_nR; i++) if (g_R[i].flags & E_BARRIER) lb = i;
    if (lb >= 0 && g_R[lb].time > t) {
        /* (L10 answer) before the last barrier's time: dropped, in R too */
        g_lc.barrier++; g_lc.late++;
        g_call_flags[ci] = E_NEUTRAL | E_DROPPED; g_call_fp[ci] = 0;
        g_R[g_nR++] = mk_ent(ci, -1, t);
        return;
    }
    if (g_late >= 3) {
        rent tmp = mk_ent(ci, -1, t);
        okey kc;
        int by_other = 0, by_dis = 0;
        tmp.fp = fp; tmp.flags = E_INPUT;
        kc = r_key(&tmp);
        for (i = lb + 1; i < g_nR; i++)
            if (!(g_R[i].flags & E_NEUTRAL) && (g_R[i].fp & fp) && okey_lt(kc, r_key(&g_R[i]))) {
                if (g_R[i].grp < 0 && g_c[g_R[i].ci].kind == C_DISARM && r_key(&g_R[i]).t > t && g_call_nf[g_R[i].ci] <= t) by_dis = 1; else by_other = 1;
            }
        if (by_dis || by_other) { g_lc.late++; g_call_late[ci] = 1; }
        if (by_dis && !by_other) g_lc.late_by_disarm++;   /* late only because a plain disarm sits at N, past N(F) */
    } else
    for (i = lb + 1; i < g_nR; i++) if (!(g_R[i].flags & E_NEUTRAL) && (g_R[i].fp & fp) && r_key(&g_R[i]).t > t) { g_lc.late++; g_call_late[ci] = 1; break; }
    place_waiting(t, fp);
    g_R[g_nR++] = mk_ent(ci, -1, t);
    r_sort();
    for (pos = 0; pos < g_nR && g_R[pos].ci != ci; pos++) { }
    if (pos < g_nR - 1) {
        g_r_moved = 1;
        if (pos <= late_window_pos()) g_lc.far++;
        for (i = pos + 1; i < g_nR; i++) if (g_c[g_R[i].ci].kind == C_FRAME) { g_lc.frames++; break; }
    }
    if (t > g_N) g_N = t;
    add_probe(t);
}

/* one call: A makes it, R takes it, B follows, compare */
static void step(int ci) {
    call* c = &g_c[ci];
    ysig_stats s0, s1;
    int k;
    g_call_grp[ci] = -1;
    g_call_fp[ci] = 0;
    g_call_flags[ci] = 0;
    memset(g_pw_after, 0, sizeof g_pw_after);
    int mark = g_nha;
    g_call_N[ci] = g_N;
    ysig_get_stats(&g_a, &s0);
    if (c->kind == C_DRAIN) {
        drain(&g_a, g_ha, &g_nha, g_da, &g_nda, ci, 1);
        if (!g_noA) hand_check(ci, mark, &s0, &s0);
        return;
    }
    /* a reset clears the queues: a caller drains before a barrier */
    if (c->kind == C_RESET || c->kind == C_RESET_TRIAL || c->kind == C_FLUSH) drain(&g_a, g_ha, &g_nha, g_da, &g_nda, ci, 1);
    g_rows = &g_rows_a;
    if (c->kind == C_FEED && chance(3) && !off('x')) g_reenter = 1;
    do_call(&g_a, c);
    g_reenter = 0;
    drain(&g_a, NULL, NULL, g_da, &g_nda, ci, 0);   /* derived events at once; hits at drains */
    /* after an arm the caller takes what it queued (a past arm's hits
     * handed out again): the arm's voided hits are classified with them */
    if (c->kind == C_ARM) drain(&g_a, g_ha, &g_nha, NULL, NULL, ci, 1);
    if (g_a.n_hit > 40) drain(&g_a, g_ha, &g_nha, NULL, NULL, ci, 1);   /* a caller drains before 64 */
    ysig_get_stats(&g_a, &s1);
    switch (c->kind) {
    case C_FEED: {
        uint64_t fp = feed_fp(c);
        g_call_time[ci] = c->t;
        if (!fp) {
            g_call_flags[ci] = E_NEUTRAL;
            g_R[g_nR++] = mk_ent(ci, -1, c->t);
            break;
        }
        g_call_fp[ci] = fp;
        g_call_flags[ci] = E_INPUT;
        gap_end();
        if (g_late >= 2) { late_input2(ci, fp); break; }
        if (g_late) { late_input(ci, fp); break; }
        place_waiting(c->t, fp);
        g_R[g_nR++] = mk_ent(ci, -1, c->t);
        if (c->t > g_N) g_N = c->t;
        add_probe(c->t);
        break;
    }
    case C_FRAME: case C_SETTLE:
        g_call_fp[ci] = g_all;
        g_call_flags[ci] = E_INPUT;
        g_call_time[ci] = c->t;
        gap_end();
        place_waiting(c->t, g_all);
        g_R[g_nR++] = mk_ent(ci, -1, c->t);
        if (c->t > g_N) g_N = c->t;
        add_probe(c->t);
        break;
    case C_RESET: case C_RESET_TRIAL: case C_FLUSH: {
        int w;
        g_call_flags[ci] = E_BARRIER;
        gap_end();
        /* waiting arms cancelled; their groups' other calls go in first */
        qsort(g_wait, (size_t)g_nwait, sizeof(int), cmp_wait);
        for (w = 0; w < g_nwait; w++) {
            group* G = &g_gr[g_wait[w]];
            if (!G->cancelled) { G->cancelled = 1; g_cancelled++; }
            put_group_end(g_wait[w], 0);
        }
        g_nwait = 0;
        g_call_time[ci] = c->kind == C_FLUSH ? g_N : c->t;
        g_R[g_nR++] = mk_ent(ci, -1, g_call_time[ci]);
        if (c->kind != C_FLUSH) {
            g_N = c->t;
            src_reset(c->kind == C_RESET_TRIAL);
        }
        break;
    }
    case C_SET: case C_DISARM: case C_RECENTER:
        g_call_fp[ci] = state_fp(c);
        g_call_nf[ci] = nf(g_call_fp[ci]);
        /* rule 3's trigger: when the call goes into R (H9), not now */
        g_call_flags[ci] = E_STATE;
        g_call_time[ci] = g_late ? nf(g_call_fp[ci]) : g_N;
        if (g_late >= 3 && c->kind == C_DISARM) g_call_time[ci] = g_N;   /* (the coordinator's ruling) plain ysig_disarm() at N */
        if (g_late && !(g_late >= 3 && c->kind == C_DISARM)) g_call_N[ci] = g_call_time[ci];
        if (g_late >= 3) {
            /* (T3) late when R holds a meeting call with a greater key */
            int q, lb = -1;
            okey kc;
            rent tmp = mk_ent(ci, -1, g_call_time[ci]);
            tmp.fp = g_call_fp[ci]; tmp.flags = E_STATE;
            kc = r_key(&tmp);
            for (q = 0; q < g_nR; q++) if (g_R[q].flags & E_BARRIER) lb = q;
            for (q = lb + 1; q < g_nR; q++) if (!(g_R[q].flags & E_NEUTRAL) && (g_R[q].fp & g_call_fp[ci]) && okey_lt(kc, r_key(&g_R[q]))) { g_lc.late_state++; g_late_state_ci = ci; g_call_late[ci] = 1; break; }
        }
        g_R[g_nR++] = mk_ent(ci, -1, g_call_time[ci]);
        if (g_npend < 15) g_pend[g_npend++] = ci;
        else { gap_end(); g_pend[g_npend++] = ci; }   /* (a gap longer than a group holds: the rest in no group) */
        break;
    case C_ARM: {
        group* G = &g_gr[g_ngr];
        int gi = g_ngr++, p, pin;
        memset(G, 0, sizeof *G);
        g_call_fp[ci] = state_fp(c);
        /* the arm call's trigger (S2, S3): when its group goes into R */
        g_call_flags[ci] = E_STATE;
        g_call_time[ci] = c->t;
        G->tau = c->t;
        G->node = c->node;
        G->arm_ci = ci;
        /* (f2f0e0d text, S1) a disarm takes effect at its group's arm: in
         * the block, the group's other state calls, then its disarms, then
         * the arm */
        for (k = 0; k < g_npend; k++) { r_remove_ci(g_pend[k]); if (g_c[g_pend[k]].kind != C_DISARM) G->calls[G->n++] = g_pend[k]; }
        for (k = 0; k < g_npend; k++) if (g_c[g_pend[k]].kind == C_DISARM) G->calls[G->n++] = g_pend[k];
        G->calls[G->n++] = ci;
        for (k = 0; k < G->n; k++) { G->fp |= g_call_fp[G->calls[k]]; g_call_grp[G->calls[k]] = gi; g_call_time[G->calls[k]] = G->tau; }
        g_npend = 0;
        if (s1.arm_reentry > s0.arm_reentry) break;   /* (not generated as a top call) */
        {
            /* (late text) past, present, ahead by N(F(G)) at the arm's call */
            int64_t nn = g_late ? nf(G->fp) : g_N;
            G->cls = G->tau < nn ? 0 : G->tau == nn ? 1 : 2;
            if (g_late) g_call_N[ci] = nf(g_call_fp[ci]);   /* the arm's own time for rule 3's test: N(F) at its call */
        }
        if (G->cls == 0) {
            p = rule2_place(G, &pin);
            if (g_late && pin) {
                const rent* b4 = p > 0 ? &g_R[p - 1] : NULL;
                g_lc.ooo_grp++;
                if (b4 && g_c[b4->ci].kind == C_FRAME) g_lc.by_frame++;
                else if (b4 && b4->grp >= 0 && g_gr[b4->grp].fallback) g_lc.by_fb++;
                else g_lc.by_other++;
                if (!g_may_pin) pin = 0;
            }   /* the text pins only before the window (finding L1) */
            G->pinned = pin;
            if (s1.arm_past_far > s0.arm_past_far || s1.arm_past_full > s0.arm_past_full) {
                G->fallback = 1;
                if (getenv("SIG_ORACLE_DEBUG") && s1.arm_past_far > s0.arm_past_far)
                    printf("  far: call %d tau %.3f place %d (after call %d) newest checkpoint R[%d] (call %d); window frames %d calls %d writes %d; full so far %lld\n", ci, G->tau / 1e6, p,
                           p > 0 ? g_R[p - 1].ci : -1, last_ckpt_pos(), last_ckpt_pos() >= 0 ? g_R[last_ckpt_pos()].ci : -1, g_K, g_C, g_W, (long long)s1.arm_past_full);
                if (s1.arm_past_far > s0.arm_past_far && p > last_ckpt_pos()) fail("%sarm_past_far for a group whose place is after R's newest checkpoint%.0ld", "", 0L);
                put_group_end(gi, 1);
                group_triggers(gi, 0);
                group_disarms(gi);
            } else {
                /* calls after the place that share a node with the group: one
                 * that shares none commutes with it (a feed every source drops
                 * changes none), so the group may be made where it is and
                 * nothing need run again (spec ambiguity S6: arm_caught_up
                 * counts "placed before calls already made") */
                G->rerun = 0;
                for (k = p; k < g_nR; k++) if (!(g_R[k].flags & E_NEUTRAL) && (g_R[k].fp & G->fp)) G->rerun = 1;
                /* arm_caught_up: a past group placed before calls already made */
                if (G->rerun && s1.arm_caught_up == s0.arm_caught_up && s1.arm_past_off == s0.arm_past_off
#if SIG_ARM_REF_SPEC
                    && s1.arm_past_pinned == s0.arm_past_pinned   /* (S6) a pinned group may count only as pinned */
#endif
                    )
                    fail("%sa past group placed before calls already made, made at its call with no count", "", ci);
                for (k = 0; k < G->n; k++) { rent e = mk_ent(G->calls[k], gi, G->tau); r_insert(p + k, &e); }
                group_triggers(gi, 0);   /* (ruling) right before the call, at the group's place */
                group_disarms(gi);
            }
#if SIG_ARM_REF_SPEC
            if (getenv("SIG_ORACLE_DEBUG")) printf("  past group %d call %d node %d tau %.3f: place %d of %d (after call %d), pinned %d, A far +%lld full +%lld pinned +%lld caught_up +%lld\n", gi, ci, G->node, G->tau / 1e6, p, g_nR, p > 0 ? g_R[p - 1].ci : -1, pin,
                (long long)(s1.arm_past_far - s0.arm_past_far), (long long)(s1.arm_past_full - s0.arm_past_full), (long long)(s1.arm_past_pinned - s0.arm_past_pinned), (long long)(s1.arm_caught_up - s0.arm_caught_up));
            if (0 && pin) printf("  pinned group %d at call %d tau %.3f place %d of %d; A pinned +%lld caught_up +%lld far +%lld full +%lld\n", gi, ci, G->tau / 1e6, p, g_nR,
                (long long)(s1.arm_past_pinned - s0.arm_past_pinned), (long long)(s1.arm_caught_up - s0.arm_caught_up), (long long)(s1.arm_past_far - s0.arm_past_far), (long long)(s1.arm_past_full - s0.arm_past_full));
            if (pin && s1.arm_past_pinned == s0.arm_past_pinned && !G->fallback && !skip('P')) fail("%sa pinned group not counted in arm_past_pinned", "", ci);
            /* (8594b73) before the window's oldest checkpoint the graph keeps no
             * footprint: an input there later than tau pins whatever its
             * footprint. Where the window starts is not public (its logs can
             * run full), so a pin count is accepted when some input after the
             * last barrier and before the place is later than tau */
            if (!pin && !g_may_pin && s1.arm_past_pinned != s0.arm_past_pinned && !skip('P')) fail("%sarm_past_pinned for a group that is not pinned", "", ci);
#endif
        } else if (G->cls == 1) {
            put_group_end(gi, 1);
            group_triggers(gi, 0);
            group_disarms(gi);
        } else if (s1.arm_past_full > s0.arm_past_full) {
            /* (b6676f4) an ahead group the window cannot keep: with room,
             * its state calls stay where made and the arm waits alone; with
             * no room (32 arms, 64 calls waiting) the arm is made at once on
             * the input known, which B's no-window graph cannot do */
            int nw = 0, w;
            for (w = 0; w < g_nwait; w++) nw += g_gr[g_wait[w]].n;
            G->fallback = 1;
            if (g_nwait >= 32 || nw + G->n > 64) { g_b_cannot = 2; break; }
            {
                int keep = 0;
                for (k = 0; k < G->n - 1; k++) {
                    rent e;
                    if (g_c[G->calls[k]].kind == C_DISARM) { G->calls[keep++] = G->calls[k]; continue; }   /* waits with the arm */
                    e = mk_ent(G->calls[k], -1, g_N);
                    g_call_grp[G->calls[k]] = -1;
                    g_R[g_nR++] = e;
                }
                G->calls[keep++] = ci;
                G->n = keep;
                G->fp = 0;
                for (k = 0; k < G->n; k++) G->fp |= g_call_fp[G->calls[k]];
            }
            g_wait[g_nwait++] = gi;
        } else {
            /* SIG_ORACLE_H9=1: set aside cases where a call of this ahead
             * group would have let a waiting group in at its own call (the
             * reviewer's H9 shape, f2f0e0d makes those calls at once) */
            if (getenv("SIG_ORACLE_H9")) {
                int w, q;
                for (q = 0; q < G->n; q++)   /* the arm too: f2f0e0d's S2 lets an ahead arm in at N */
                    for (w = 0; w < g_nwait; w++)
                        if ((g_gr[g_wait[w]].tau < g_N || (q == G->n - 1 && g_gr[g_wait[w]].tau == g_N)) && (g_gr[g_wait[w]].fp & g_call_fp[G->calls[q]])) g_h9 = 1;
            }
            g_wait[g_nwait++] = gi;
        }
        break;
    }
    default: break;
    }
#if SIG_ARM_REF_SPEC
    if (getenv("SIG_ORACLE_DEBUG") && s1.arm_ahead_cancelled != g_cancelled) {
        int w;
        printf("  cancel counts after call %d (kind %d node %d): A %lld R %d; waiting:", ci, c->kind, c->node, (long long)s1.arm_ahead_cancelled, g_cancelled);
        for (w = 0; w < g_nwait; w++) printf(" [g%d arm call %d node %d tau %.3f%s]", g_wait[w], g_gr[g_wait[w]].arm_ci, g_gr[g_wait[w]].node, g_gr[g_wait[w]].tau / 1e6, g_gr[g_wait[w]].cancelled ? " cancelled" : "");
        printf(" N %.3f\n", g_N / 1e6);
    }
    /* while a gap is open, its calls' letting in (and the cancels it
     * brings) is not in R yet; a read then gives it at N (617971a), as the
     * values, which are not compared then either: checked at the gap's end */
    if (s1.arm_ahead_cancelled != g_cancelled && !skip('C') && (!g_npend || getenv("SIG_ORACLE_CANCELOPEN"))) fail("%sarm_ahead_cancelled differs from rule 3's cancels", "", ci);
#endif
    if (g_nR > MAXC * 2 - 32) { fail("%sR too long", "", ci); return; }
    if (g_late >= 2 && r_sort()) g_r_moved = 1;
    if (g_late >= 2) l_track(ci);
    if (g_late >= 3 && g_late_state_ci >= 0) {
        /* a late state call placed in a waiting group's span (G to L(G))
         * that meets F(G): 2 (b) fails for G, and the text's conditions for
         * late calls do not retest it (finding U2) */
        int pos, gi;
        for (pos = 0; pos < g_nR && g_R[pos].ci != g_late_state_ci; pos++) { }
        for (gi = 0; gi < g_ngr && pos < g_nR; gi++) {
            const group* G = &g_gr[gi];
            int last, li;
            if (G->fallback || p1_np(gi, &last) >= G->tau || last < 0 || last >= pos) continue;
            li = p1_let(gi, last + 1);
            if (li > pos && (g_call_fp[g_late_state_ci] & G->fp)) { g_lc.late_state_span++; break; }
        }
        g_late_state_ci = -1;
    }
    pred_check(ci);
    g_r_moved = 0;
    if (getenv("SIG_ORACLE_HSK") && atol(getenv("SIG_ORACLE_HSK")) == g_case) {
        int q;
        printf("  step %d kind %d node %d t %.3f; A %p B %p; R:", ci, c->kind, c->node, c->t / 1e6, (void*)&g_a, (void*)&g_b);
        for (q = g_nR > 40 && !getenv("SIG_ORACLE_FULLR") ? g_nR - 40 : 0; q < g_nR; q++) printf(" %d%s", g_R[q].ci, g_R[q].grp >= 0 ? "g" : "");
        printf("\n");
    }
    b_sync();
    if (!g_noA) hand_check(ci, mark, &s0, &s1);
    /* while a gap is open its state calls' rule-3 triggers are not decided
     * (H9 decision: they take effect when the calls go into R, at the gap's
     * end or its group's arm); a read then can see either, so no compare */
    if (!g_noA && !g_why && !g_bw_n && !g_b_cannot && !g_npend) {
        const char* w = cmp_values();
        if (w) {
            snprintf(g_whybuf, sizeof g_whybuf, "after a %s call: %s",
                     c->kind == C_ARM ? (g_gr[g_call_grp[ci]].cls == 0 ? "past arm" : g_gr[g_call_grp[ci]].cls == 1 ? "present arm" : "ahead arm")
                     : c->kind == C_FEED ? "feed" : c->kind == C_FRAME ? "frame" : c->kind == C_SETTLE ? "settle"
                     : c->kind == C_SET ? "set" : c->kind == C_DISARM ? "disarm" : c->kind == C_RECENTER ? "recenter" : "barrier", w);
            g_why = g_whybuf;
        }
    }
}

/* ---- generation (online: an arm may take a handed-out hit's time) --------- */

static int64_t g_t, g_onset, g_last[4], g_newest;
static double g_x, g_y;
static int g_drains;

static int g_fail_ci = -1;   /* the call at which the case first failed */
static long long g_unv_fail, g_other_fail;   /* A's arm_unverified, and its fallback (pinned included), retracted and stale counts, then */
static long long g_unv_at[MAXC], g_oth_at[MAXC];   /* A's arm_unverified, and its guard-stop fallback counts, after each call */
static int g_bc_ci = -1;                            /* the call at which B was first found unable to run R */
static void a_counts(long long* unv, long long* oth) {
    ysig_stats st;
    ysig_get_stats(&g_a, &st);
#ifdef SIG_HAS_UNV
    *unv = st.arm_unverified;
#else
    *unv = 0;
#endif
    *oth = st.arm_past_pinned + st.arm_past_far + st.arm_past_full + st.arm_past_off + st.arm_hits_retracted + st.arm_rows_stale + st.arm_derived_stale;
#ifdef SIG_HAS_LATE
    /* (0.7.0) late_unverified, and the late fallbacks the guard stops on */
    *unv += st.late_unverified;
    *oth += st.late_far + st.late_full;
#ifdef SIG_HAS_LATE_VOIDED
    *oth += st.late_hits_voided;   /* (the 2026-11-15 ruling) a guard stop */
#endif
#endif
}
static void push(const call* c) {
    int b4 = g_b_cannot;
    if (g_nc >= MAXC - 4) return;
    g_c[g_nc] = *c;
    g_cur_ci = g_nc;
    step(g_nc);
    a_counts(&g_unv_at[g_nc], &g_oth_at[g_nc]);
    if (g_b_cannot && !b4 && g_bc_ci < 0) g_bc_ci = g_nc;
    if (g_why && g_fail_ci < 0) {
        g_fail_ci = g_nc;
#ifdef SIG_HAS_UNV
        /* one reading for every tally: a_counts() (arm_unverified and, with
         * a 0.7.0 header, late_unverified; the guard-stop fallbacks, with
         * late_far and late_full; not late_barrier, late_frames, late_off) */
        a_counts(&g_unv_fail, &g_other_fail);
#endif
    }
    g_nc++;
}
static int64_t g_last_onset = -1;
static void gen_frame(void) {
    call c;
    memset(&c, 0, sizeof c);
    c.kind = C_FRAME; c.t = g_onset; c.poll = g_onset - (int64_t)(rnd() % 3) * 300 * US; c.chbits = (int)(rnd() & 0xff);
    if (c.poll < g_newest - 2 * MS) c.poll = g_newest;   /* a poll near the input (no bad stamps) */
    if (g_late && !getenv("SIG_ORACLE_NOLATEGEN") && !off('l') && rnd() % 2 && g_newest - 2 * MS > g_last_onset) {
        /* (late mode) the poll after the onset: input up to the poll was fed
         * before the frame call, some of it later than the onset */
        c.t = g_newest - (int64_t)(1 + rnd() % 2) * MS;
        c.poll = g_newest;
    }
    if (g_late && c.t <= g_last_onset) c.t = g_last_onset + MS;
    g_last_onset = c.t;
    push(&c);
    if (c.t > g_newest) g_newest = c.t;
    g_onset += 16 * MS + (int64_t)(rnd() % 3) * US * 500;
    memset(&c, 0, sizeof c);
    c.kind = C_DRAIN;
    if (rnd() % 4) push(&c);
}
static void gen_feed(int dense) {
    call c;
    memset(&c, 0, sizeof c);
    c.kind = C_FEED;
    if (rnd() % 7 == 0 && !off('o')) {
        /* the pressure pen: input no armed path reads but D6's, late reports too */
        c.ekind = YIN_KIND_PEN; c.ectl = YIN_AXIS_PEN_PRESSURE; c.dev = 3; c.v = rndf() * 6;
        c.t = rnd() % 3 == 0 && !off('l') && !off('q') ? g_t - (int64_t)(rnd() % 8000) * US : g_t;
    } else if (g_rel) {
        c.ekind = YIN_KIND_MOUSE; c.ectl = YIN_AXIS_DELTA; c.dev = 1;
        c.x = (double)((int)(rnd() % 7) - 3); c.y = (double)((int)(rnd() % 7) - 3);
        c.t = g_t;
    } else {
        if (dense) { g_x = g_x > 0 ? -1 : 9; }
        else if (rnd() % 4 == 0) { g_x = (rndf() - 0.5) * 16; g_y = (rndf() - 0.5) * 16; }
        else { g_x += (rndf() - 0.5) * 3; g_y += (rndf() - 0.5) * 3; }
        c.ekind = YIN_KIND_PEN; c.ectl = YIN_AXIS_POSITION; c.dev = 1 + (int)(rnd() % 2); c.x = g_x; c.y = g_y;
        c.t = g_t;
        if (off('l')) { }
        else if (rnd() % 12 == 0 && !off('q')) c.t = g_last[c.dev] - (int64_t)(rnd() % 3000) * US;                 /* older than its device's last: dropped */
        else if (rnd() % 8 == 0) { int o = 3 - c.dev; c.dev = o; c.t = g_last[o] + (g_t - g_last[o]) / 2; }   /* a late report of the other device */
        else if (g_late && !getenv("SIG_ORACLE_NOLATEGEN") && !off('l') && c.dev == 2 && rnd() % 2) {
            /* (late mode) device 2 delivers 1 to 20 ms late, in its own order */
            int64_t tl = g_t - (int64_t)(1 + rnd() % 20) * MS;
            c.t = tl > g_last[2] ? tl : g_last[2];
        }
        if (c.t > g_last[c.dev]) g_last[c.dev] = c.t;
    }
    if (c.t < 0) c.t = 0;
    push(&c);
    if (c.t > g_newest) g_newest = c.t;
}

static void gen_group(void) {
    call c;
    int m, j, sh = (int)(rnd() % 6), d = off('1') ? N_D1 : g_dets[rnd() % (unsigned)g_ndet];
    int64_t tau;
    /* tau: past, an input's time, a handed-out hit's time, present, ahead */
    if (sh == 0 || sh == 1) tau = g_newest - (int64_t)(rnd() % 30) * MS;
    else if (sh == 2) { int q = (int)(rnd() % (unsigned)(g_nc ? g_nc : 1)); tau = g_c[q].kind == C_FEED || g_c[q].kind == C_FRAME ? g_c[q].t : g_newest - MS; }
    else if (sh == 3 && g_nha) { tau = g_ha[g_nha - 1].h.t; if (!off('1')) d = g_ha[g_nha - 1].h.node; }
    else if (sh == 4) tau = g_N;
    else tau = g_newest + (int64_t)(rnd() % 20) * MS + (int64_t)(rnd() % 1000) * US;
    if (off('p') && tau < g_N) tau = g_N;
    if (off('a') && tau > g_N) tau = g_N;
    if (tau < 0) tau = 0;
    /* the group's state calls first (format 3.12: sets, disarms, recenters, then the arm) */
    /* a group's state calls: as the player makes them, with present and
     * ahead groups (format 3.12: recenter, then the until's arm; a state's
     * exit disarms a waiting arm); with past groups rarely (the player
     * measured none; B often cannot run them, see note 7) */
    m = rnd() % 3 == 0 && !off('g') && (tau >= g_N || rnd() % 8 == 0 || getenv("SIG_ORACLE_PAST_GROUPS")) ? 1 + (int)(rnd() % 3) : 0;
    for (j = 0; j < m; j++) {
        memset(&c, 0, sizeof c);
        switch (rnd() % 5) {
        case 0: c.kind = C_SET; c.node = g_dets[rnd() % (unsigned)g_ndet]; c.param = YSIG_DETECT_LEVEL; c.v = 0.5 + rnd() % 6; break;
        case 1:
            if (off('k')) { c.kind = C_SET; c.node = N_AFF; c.param = YSIG_AFFINE_ANGLE; c.v = (rndf() - 0.5) * 90; break; }
            c.kind = C_DISARM; c.node = rnd() % 2 ? d : g_dets[rnd() % (unsigned)g_ndet]; break;
        case 2:
            if (g_rel) { c.kind = C_RECENTER; c.t = tau < g_N ? tau : g_N; c.x = (rndf() - 0.5) * 10; c.y = (rndf() - 0.5) * 10; }
            else { c.kind = C_SET; c.node = N_AFF; c.param = YSIG_AFFINE_ANGLE; c.v = (rndf() - 0.5) * 90; }
            break;
        case 3: c.kind = C_SET; c.node = g_chnode; c.param = YSIG_CHANNEL_CHANNEL; c.v = (double)(rnd() % (unsigned)(g_n[g_chnode].p[0] + 1)); break;
        default: c.kind = C_SET; c.node = N_POFF; c.param = 0; c.v = (rndf() - 0.5) * 4; break;
        }
        push(&c);
    }
    m = 1 + (int)(rnd() % 3);
    for (j = 0; j < m; j++) {
        memset(&c, 0, sizeof c);
        c.kind = C_ARM; c.node = j && !off('1') ? g_dets[rnd() % (unsigned)g_ndet] : d; c.t = tau;
        push(&c);
    }
    if (rnd() & 1) { memset(&c, 0, sizeof c); c.kind = C_DRAIN; push(&c); }
    /* an ahead arm, then (E) a later arm of the same detect with an earlier time, or a disarm of the waiting arm */
    if (sh == 5 && rnd() % 3 == 0 && !off('a') && !off('s')) {
        gen_feed(0);
        memset(&c, 0, sizeof c);
        if (rnd() & 1) { c.kind = C_ARM; c.node = d; c.t = tau - (int64_t)(1 + rnd() % 15) * MS; if (c.t < 0) c.t = 0; if (off('p') && c.t < g_N) c.t = g_N; }
        else { c.kind = C_DISARM; c.node = d; }
        push(&c);
    }
}

/* ---- the end checks ------------------------------------------------------ */

static int hit_eq(const ysig_hit* x, const ysig_hit* y) {
    if (getenv("SIG_ORACLE_HITMEM")) return x->node == y->node && x->t == y->t && !memcmp(&x->value, &y->value, sizeof x->value);
    return x->node == y->node && x->t == y->t && !value_ne(&x->value, &y->value);
}
static int rows_node_eq(const rowlist* a, const rowlist* b, int node) {
    int i = 0, j = 0;
    for (;;) {
        while (i < a->n && i < a->cap && a->v[i].node != node) i++;
        while (j < b->n && j < b->cap && b->v[j].node != node) j++;
        if (i >= a->n || i >= a->cap || j >= b->n || j >= b->cap) return (i >= a->n || i >= a->cap) == (j >= b->n || j >= b->cap);
        if (memcmp(&a->v[i].r.value, &b->v[j].r.value, sizeof(ysig_value)) || a->v[i].r.frame != b->v[j].r.frame ||
            a->v[i].r.onset != b->v[j].r.onset || a->v[i].r.per_frame != b->v[j].r.per_frame) return 0;
        i++; j++;
    }
}
static int der_node_eq(int node) {
    int i = 0, j = 0;
    for (;;) {
        while (i < g_nda && (int)g_da[i].e.device != node) i++;
        while (j < g_ndb && (int)g_db[j].e.device != node) j++;
        if (i >= g_nda || j >= g_ndb) return (i >= g_nda) == (j >= g_ndb);
        if (memcmp(&g_da[i].e, &g_db[j].e, sizeof(yin_event))) return 0;
        i++; j++;
    }
}

/* A failure found at the case's end: the call at which A handed out the
 * first wrong row (or event), so A's counts are read there, not at the end
 * (a count made after the caller had the wrong row does not cover it).
 * A row A never handed out has no such call: the case's last call. */
static int g_end_fail_ci = -1;
static int g_end_fail_kind;   /* 0 at a call, 1 an end check with a call, 2 an end check read at the end */
static int rows_fail_at(void) {
    int q, best = -1;
    for (q = 0; q < g_ntot; q++) {
        int i = 0, j = 0, at = -1;
        for (;;) {
            while (i < g_rows_a.n && i < g_rows_a.cap && g_rows_a.v[i].node != q) i++;
            while (j < g_rows_b.n && j < g_rows_b.cap && g_rows_b.v[j].node != q) j++;
            if (i >= g_rows_a.n || i >= g_rows_a.cap) { if (!(j >= g_rows_b.n || j >= g_rows_b.cap)) at = g_nc - 1; break; }
            if (j >= g_rows_b.n || j >= g_rows_b.cap) { at = g_rows_a.v[i].at; break; }
            if (memcmp(&g_rows_a.v[i].r.value, &g_rows_b.v[j].r.value, sizeof(ysig_value)) || g_rows_a.v[i].r.frame != g_rows_b.v[j].r.frame ||
                g_rows_a.v[i].r.onset != g_rows_b.v[j].r.onset || g_rows_a.v[i].r.per_frame != g_rows_b.v[j].r.per_frame) { at = g_rows_a.v[i].at; break; }
            i++; j++;
        }
        if (at >= 0 && (best < 0 || at < best)) best = at;
    }
    return best;
}
static int der_fail_at(void) {
    int q, best = -1;
    for (q = 0; q < g_ntot; q++) {
        int i = 0, j = 0, at = -1;
        for (;;) {
            while (i < g_nda && (int)g_da[i].e.device != q) i++;
            while (j < g_ndb && (int)g_db[j].e.device != q) j++;
            if (i >= g_nda) { if (j < g_ndb) at = g_nc - 1; break; }
            if (j >= g_ndb || memcmp(&g_da[i].e, &g_db[j].e, sizeof(yin_event))) { at = g_da[i].at; break; }
            i++; j++;
        }
        if (at >= 0 && (best < 0 || at < best)) best = at;
    }
    return best;
}
static void end_checks(void) {
    static char used[8192];
    ysig_stats sa;
    long ex_sup = 0, ex_ret = 0;
    int i, j, rd = 0, dd = 0;
    /* a fresh no-window run of the final R: its hits, rows and events */
    g_rows_b.n = 0;
    if (!open_graph(&g_b, g_ar_b, g_ch_b, 0)) exit(2);
    g_rows = &g_rows_b;
    g_nhb = g_ndb = 0;
    for (i = 0; i < g_nR; i++) {
        if (g_c[g_R[i].ci].kind == C_DRAIN) continue;
        g_rows = &g_rows_b;
        do_call(&g_b, &g_c[g_R[i].ci]);
        drain(&g_b, g_hb, &g_nhb, g_db, &g_ndb, i, 1);
    }
    ysig_get_stats(&g_a, &sa);
    /* every hit of R is among A's (match A's last copies: earlier equal
     * ones were an earlier arm's) */
    memset(used, 0, sizeof used);
    for (j = 0; j < g_nhb && !g_why; j++) {
        for (i = g_nha - 1; i >= 0; i--) if (!used[i] && hit_eq(&g_ha[i].h, &g_hb[j].h)) { used[i] = 1; break; }
        if (i < 0) fail("%sa hit of R that A did not hand out (node %ld)", "", g_hb[j].h.node);
    }
    /* each extra: superseded when a hit of a past group's detect later
     * than its tau, handed out before its arm; else retracted */
    for (i = 0; i < g_nha && !g_why; i++) {
        int sup = 0, k;
        if (used[i]) continue;
        for (k = 0; k < g_ngr && !sup; k++) {
            const group* G = &g_gr[k];
            if (G->cls == 0 && !G->fallback && group_touches(G, g_ha[i].h.node) && g_ha[i].h.t > G->tau && g_ha[i].at < G->arm_ci) sup = 1;
        }
        if (sup) ex_sup++; else ex_ret++;
        if (getenv("SIG_ORACLE_DEBUG")) {
            int q;
            printf("  extra: node %d t %.6f found %.6f v (%g,%g) at call %d; B has:", g_ha[i].h.node, g_ha[i].h.t / 1e6, g_ha[i].h.t_found / 1e6,
                   g_ha[i].h.value.v[0], g_ha[i].h.value.v[1], g_ha[i].at);
            for (q = 0; q < g_nhb; q++) if (g_hb[q].h.node == g_ha[i].h.node && llabs(g_hb[q].h.t - g_ha[i].h.t) < 5 * MS)
                printf(" t %.6f found %.6f v (%g,%g) tier %d dev %u;", g_hb[q].h.t / 1e6, g_hb[q].h.t_found / 1e6, g_hb[q].h.value.v[0], g_hb[q].h.value.v[1], g_hb[q].h.value.tier, g_hb[q].h.value.device);
            printf("\n  A's: tier %d dev %u src %u t_old %.6f\n", g_ha[i].h.value.tier, g_ha[i].h.value.device, g_ha[i].h.value.src, g_ha[i].h.value.t_oldest / 1e6);
        }
    }
    /* each arm's voided hits were classified at its call; here the total */
    if (!g_why && !skip('V') && sa.arm_hits_superseded + sa.arm_hits_retracted != ex_sup + ex_ret) {
        snprintf(g_whybuf, sizeof g_whybuf, "extra hits %ld, counted superseded %lld + retracted %lld",
                 ex_sup + ex_ret, (long long)sa.arm_hits_superseded, (long long)sa.arm_hits_retracted);
        g_why = g_whybuf;
    }
    for (i = 0; i < g_ntot; i++) if (!rows_node_eq(&g_rows_a, &g_rows_b, i)) rd = 1;
    for (i = 0; i < g_ntot; i++) if (!der_node_eq(i)) dd = 1;
    if (!g_why) {
        if (g_final_rows && skip('R')) { }
        else if (g_final_rows) {
            if (rd && getenv("SIG_ORACLE_DEBUG")) {
                int q, ra_ = 0, rb_ = 0;
                for (q = 0; q < g_ntot; q++) if (!rows_node_eq(&g_rows_a, &g_rows_b, q)) {
                    int k2, c2 = 0;
                    printf("   rows of node %d differ; A:", q);
                    for (k2 = 0; k2 < g_rows_a.n && k2 < g_rows_a.cap && c2 < 400; k2++) if (g_rows_a.v[k2].node == q) { ra_++; c2++; }
                    c2 = 0;
                    for (k2 = 0; k2 < g_rows_b.n && k2 < g_rows_b.cap && c2 < 400; k2++) if (g_rows_b.v[k2].node == q) { rb_++; c2++; }
                    printf(" %d rows, B %d rows\n", ra_, rb_);
                    ra_ = rb_ = 0;
                }
                printf("   A rows %d B rows %d; A stats.rows %lld\n", g_rows_a.n, g_rows_b.n, (long long)sa.rows);
            }
            if (rd) { g_why = "rows (final only) not R's"; g_end_fail_ci = rows_fail_at(); } else if (sa.arm_rows_stale) g_why = "arm_rows_stale in final-only mode"; }
        else if (rd && !sa.arm_rows_stale) { g_why = "rows changed, not counted stale"; g_end_fail_ci = rows_fail_at(); }
    }
    if (!g_why) {
        if (getenv("SIG_ORACLE_DEBUG") && dd) {
            int q, na6 = 0, nb6 = 0;
            for (q = 0; q < g_nda; q++) na6++;
            for (q = 0; q < g_ndb; q++) nb6++;
            printf("  derived: A %d B %d; A derived_lost %lld B %lld; final %d\n", na6, nb6, (long long)sa.derived_lost, (long long)g_b.st.derived_lost, g_final_der);
            for (q = 0; q < g_ntot; q++) if (!der_node_eq(q)) {
                int i2 = 0, j2 = 0, k2, na_ = 0, nb_ = 0, ia[8192], ib[8192], m2;
                for (k2 = 0; k2 < g_nda; k2++) if ((int)g_da[k2].e.device == q) ia[na_++] = k2;
                for (k2 = 0; k2 < g_ndb; k2++) if ((int)g_db[k2].e.device == q) ib[nb_++] = k2;
                for (m2 = 0; m2 < na_ && m2 < nb_ && !memcmp(&g_da[ia[m2]].e, &g_db[ib[m2]].e, sizeof(yin_event)); m2++) { }
                printf("   node %d: A %d events, B %d; first difference at %d: A %s t %.3f x %g | B %s t %.3f x %g\n", q, na_, nb_, m2,
                       m2 < na_ ? "" : "(none)", m2 < na_ ? g_da[ia[m2]].e.t / 1e6 : 0.0, m2 < na_ ? g_da[ia[m2]].e.x : 0.0,
                       m2 < nb_ ? "" : "(none)", m2 < nb_ ? g_db[ib[m2]].e.t / 1e6 : 0.0, m2 < nb_ ? g_db[ib[m2]].e.x : 0.0);
                if (getenv("SIG_ORACLE_DEBUG3")) {
                    for (k2 = m2 - 2; k2 < m2 + 30; k2++) if (k2 >= 0) printf("     %d: A %s%.3f/%g  B %s%.3f/%g\n", k2, k2 < na_ ? "" : "-", k2 < na_ ? g_da[ia[k2]].e.t / 1e6 : 0.0, k2 < na_ ? g_da[ia[k2]].e.x : 0.0,
                                                                 k2 < nb_ ? "" : "-", k2 < nb_ ? g_db[ib[k2]].e.t / 1e6 : 0.0, k2 < nb_ ? g_db[ib[k2]].e.x : 0.0);
                }
                printf("   node %d:", q);
                for (k2 = 0; k2 < g_nda && i2 < 12; k2++) if ((int)g_da[k2].e.device == q) { printf(" A(%.3f,%g)", g_da[k2].e.t / 1e6, g_da[k2].e.x); i2++; }
                printf("\n   node %d:", q);
                for (k2 = 0; k2 < g_ndb && j2 < 12; k2++) if ((int)g_db[k2].e.device == q) { printf(" B(%.3f,%g)", g_db[k2].e.t / 1e6, g_db[k2].e.x); j2++; }
                printf("\n");
            }
        }
        if (g_final_der && skip('D')) { }
        else if (g_final_der) { if (dd) { g_why = "derived events (final only) not R's"; g_end_fail_ci = der_fail_at(); } else if (sa.arm_derived_stale) g_why = "arm_derived_stale in final-only mode"; }
        else if (dd && !sa.arm_derived_stale) { g_why = "derived events changed, not counted stale"; g_end_fail_ci = der_fail_at(); }
    }
}


/* SIG_ORACLE_EMIT=n: print case n as a C program that makes A's calls on
 * the same graph and window with the public API only (a repro for the
 * header's maintainer; the expected result is the oracle's) */
static void emit_node(const char* arr, int i, const ysig_node* n) {
    int k;
    printf("    %s[%d] = ysig_node_of(%d, %d, %d); %s[%d].in[2] = %d; %s[%d].in[3] = %d; %s[%d].lanes = %d; %s[%d].history = %d;\n", arr, i, n->op, n->in[0], n->in[1],
           arr, i, n->in[2], arr, i, n->in[3], arr, i, n->lanes, arr, i, (int)n->history);
    for (k = 0; k < YSIG_NP; k++) if (n->p[k] != 0) printf("    %s[%d].p[%d] = %.17g;\n", arr, i, k, n->p[k]);
}
static void emit_case(void) {
    int i;
    printf("/* generated by sig_arm_ref.c (SIG_ORACLE_EMIT): case %ld */\n", g_case);
    printf("#include <stdio.h>\n#include <string.h>\n#define YSP_SIG_IMPLEMENTATION\n#include \"ysp/sig.h\"\n");
    printf("typedef struct { double s; } cust_st;\n");
    printf("static void cust_reset(void* st, const double* p, int64_t t) { (void)p; (void)t; ((cust_st*)st)->s = 0; }\n");
    printf("static void cust_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) { (void)p; (void)n_in; ((cust_st*)st)->s += in[0].v[0]; out->v[0] = ((cust_st*)st)->s; }\n");
    printf("static const ysig_op_vtable cust = { YSIG_OP_ABI, \"runsum\", { 0, 0, 0, 0 }, 0, sizeof(cust_st), NULL, 0, cust_reset, cust_sample, NULL };\n");
    printf("static const ysig_op_vtable* const custs[1] = { &cust };\n");
    printf("static uint64_t ar[(24u << 20) / 8];\nstatic ysig_graph g;\nstatic float ch[8];\n");
    printf("static int rows_n[64], der_n[64];\n");
    printf("static void on_row(void* u, const ysig_row* r) { (void)u; if (r->node >= 0 && r->node < 64) rows_n[r->node]++; }\n");
    printf("static void feed(int64_t t, int kind, int ctl, int dev, double x, double y, double v) { yin_event e; memset(&e, 0, sizeof e); e.t = t; e.type = YIN_SAMPLE; e.kind = (uint8_t)kind; e.control = (uint32_t)ctl; e.device = (uint32_t)dev; e.x = (float)x; e.y = (float)y; e.value = (float)v; e.stamp = YIN_TIER_2; ysig_feed(&g, &e); }\n");
    printf("static void frame(int64_t o, int64_t poll, int bits) { float v[8]; int k; ysig_frame_in f; for (k = 0; k < 8; k++) v[k] = (float)((bits >> k) & 1); memset(&f, 0, sizeof f); f.index = o / 1000000; f.onset_ns = o; f.poll_ns = poll; f.has_poll = true; f.values = v; f.n_values = 8; ysig_frame(&g, &f); }\n");
    printf("static void drain(void) { ysig_hit h[64]; yin_event e[64]; int m, k; while (ysig_detected(&g, h, 64) > 0) { } while ((m = ysig_derived(&g, e, 64)) > 0) for (k = 0; k < m; k++) if (e[k].device < 64) der_n[e[k].device]++; }\n");
    printf("static void stats(int call) { ysig_stats s; ysig_get_stats(&g, &s); printf(\"after call %%d: caught_up %%lld far %%lld full %%lld pinned %%lld cancelled %%lld superseded %%lld retracted %%lld\\n\", call, (long long)s.arm_caught_up, (long long)s.arm_past_far, (long long)s.arm_past_full, (long long)s.arm_past_pinned, (long long)s.arm_ahead_cancelled, (long long)s.arm_hits_superseded, (long long)s.arm_hits_retracted); }\n");
#ifdef SIG_HAS_UNV
    printf("static void unv(int call) { ysig_stats s; ysig_get_stats(&g, &s); printf(\"after call %%d: arm_unverified %%lld\\n\", call, (long long)s.arm_unverified); }\n");
#else
    printf("static void unv(int call) { (void)call; }\n");
#endif
    printf("int main(void) {\n    static ysig_node n[%d], ad[2];\n    ysig_role role; ysig_adapter adp; ysig_desc d;\n", g_nn);
    for (i = 0; i < g_nn; i++) emit_node("n", i, &g_n[i]);
    printf("    memset(&d, 0, sizeof d); d.nodes = n; d.n_nodes = %d; d.channels = ch; d.n_channels = 8; d.arena = ar; d.on_row = on_row; d.custom = custs; d.n_custom = 1;\n", g_nn);
    printf("    d.catchup_frames = %d; d.catchup_calls = %d; d.catchup_writes = %d;\n", g_K, g_C, g_W);
#if SIG_ARM_REF_SPEC
    printf("    d.rows_final = %d; d.derived_final = %d;\n", g_final_rows, g_final_der);
#endif
    if (g_rel) {
        emit_node("ad", 0, &g_ad[0]);
        emit_node("ad", 1, &g_ad[1]);
        printf("    memset(&role, 0, sizeof role); role.name = \"hand\"; role.type = YSIG_TYPE_POSITION2D; role.units = YSIG_UNITS_DISPLAY_PX; role.allow_relative = true;\n");
        printf("    memset(&adp, 0, sizeof adp); adp.role = \"hand\"; adp.name = \"mouse\"; adp.type = YSIG_TYPE_POSITION2D_RELATIVE; adp.units = YSIG_UNITS_DISPLAY_PX; adp.nodes = ad; adp.n_nodes = 2;\n");
        printf("    d.roles = &role; d.n_roles = 1; d.adapters = &adp; d.n_adapters = 1;\n");
    } else printf("    (void)role; (void)adp;\n");
    printf("    d.arena_size = ysig_arena_size(&d);\n    if (!d.arena_size || d.arena_size > sizeof ar || !ysig_open(&g, &d)) { printf(\"open\\n\"); return 2; }\n    drain();\n");
    for (i = 0; i < g_nc && (g_fail_ci < 0 || i <= g_fail_ci); i++) {
        const call* c = &g_c[i];
        printf("    /* %d */ ", i);
        switch (c->kind) {
        case C_FEED: printf("feed(%lld, %d, %d, %d, %.9g, %.9g, %.9g);", (long long)c->t, c->ekind, c->ectl, c->dev, (double)(float)c->x, (double)(float)c->y, (double)(float)c->v); break;
        case C_FRAME: printf("frame(%lld, %lld, %d);", (long long)c->t, (long long)c->poll, c->chbits); break;
        case C_SETTLE: printf("ysig_settle(&g, %lld);", (long long)c->t); break;
        case C_ARM: printf("ysig_arm(&g, %d, %lld); stats(%d); unv(%d);", c->node, (long long)c->t, i, i); break;
        case C_DISARM: printf("ysig_disarm(&g, %d);", c->node); break;
        case C_SET: printf("ysig_set(&g, %d, %d, %.17g);", c->node, c->param, c->v); break;
        case C_RECENTER: printf("ysig_recenter(&g, 0, %.17g, %.17g, %lld);", c->x, c->y, (long long)c->t); break;
        case C_RESET: printf("drain(); ysig_reset(&g, %lld);", (long long)c->t); break;
        case C_RESET_TRIAL: printf("drain(); ysig_reset_trial(&g, %lld);", (long long)c->t); break;
        case C_FLUSH: printf("drain(); ysig_flush(&g);"); break;
        case C_DRAIN: printf("drain();"); break;
        }
        printf(" drain();\n");   /* as the oracle's A: derived events after every call */
    }
    printf("    { int k; for (k = 0; k < %d; k++) { const ysig_value* v = ysig_value_of(&g, k); printf(\"node %%d: (%%.9g, %%.9g) t %%lld tier %%d src %%d\\n\", k, v->v[0], v->v[1], (long long)v->t, v->tier, v->src); } }\n", g_ntot);
    printf("    { ysig_stats s; ysig_get_stats(&g, &s); printf(\"stats: hits %%lld rows %%lld hits_lost %%lld history_skipped %%lld time_clamped %%lld\\n\", (long long)s.hits, (long long)s.rows, (long long)s.hits_lost, (long long)s.history_skipped, (long long)s.time_clamped); }\n");
    printf("    drain();\n    { int k; for (k = 0; k < 64; k++) { if (rows_n[k]) printf(\"rows of node %%d: %%d\\n\", k, rows_n[k]); if (der_n[k]) printf(\"derived events of node %%d: %%d\\n\", k, der_n[k]); } }\n");
    printf("    return 0;\n}\n");
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char** argv) {
    long cases = getenv("SIG_ORACLE_CASES") ? atol(getenv("SIG_ORACLE_CASES")) : 2000, wrong = 0, cannot = 0, fellback = 0, noroom = 0, disarm_ahead = 0, h9 = 0, pred_n[3] = { 0, 0, 0 }, pred_f[3] = { 0, 0, 0 }, pred_why[10] = { 0 }, pred_show = 0, cannot_why[10] = { 0 }, hu_n[3] = { 0, 0, 0 }, hu_f[3] = { 0, 0, 0 }, hu_show = 0, hu_late = 0, hu_other = 0, hu_bare = 0, hu_arms = 0, hu_cnt = 0, hu_bc_at = 0, hu_end_late = 0, exc_n[3] = { 0, 0, 0 }, exc_only[3] = { 0, 0, 0 }, lt_cases = 0, lt_far = 0, lt_bar = 0, lt_frames = 0, lt_cond2 = 0, lt_ooo = 0, lt_ooo_unc = 0, lt_bc = 0, lt_bc_f = 0, lt_inputs = 0, lt_show = 0, lt_byf = 0, lt_byfb = 0, lt_byo = 0, lt_lnew = 0, lt_lnfa = 0, lt_clo = 0, lt_lst = 0, lt_lbd = 0, lt_lbd_in = 0, lt_ceq = 0, lt_lss = 0, lt_esc = 0, lt_cli = 0, oc_cases[8] = { 0 }, oc_pairs[8] = { 0 }, oc_any = 0, oc_show = 0;
    g_late = getenv("SIG_ORACLE_LATE") ? atoi(getenv("SIG_ORACLE_LATE")) : 0;
    g_noA = g_late && !getenv("SIG_ORACLE_LATE_A");
    int show = argc > 1 ? atoi(argv[1]) : 0, w;
    const char* names[64];
    long counts[64];
    int nnames = 0;
    static rowrec ra[16384], rb[16384];
    g_rs = getenv("SIG_ORACLE_SEED") ? strtoull(getenv("SIG_ORACLE_SEED"), NULL, 10) : g_rs;
    if (!g_rs) g_rs = 1;
    g_rows_a.v = ra; g_rows_a.cap = 16384;
    g_rows_b.v = rb; g_rows_b.cap = 16384;
    for (g_case = 0; g_case < cases; g_case++) {
        call c;
        int nsteps, dense = rnd() % 8 == 0, s, fb = 0;
        make_graph();
        if (rnd() % 3 == 0) { g_K = 1 + (int)(rnd() % 8); g_C = 32 << (rnd() % 7); g_W = 256 << (rnd() % 8); }
        else { g_K = 4 + (int)(rnd() % 5); g_C = 4096; g_W = 1 << 16; }
        g_final_rows = SIG_ARM_REF_SPEC && rnd() % 2;
        g_final_der = SIG_ARM_REF_SPEC && rnd() % 3 == 0;
        if (!open_graph(&g_a, g_ar_a, g_ch_a, 1)) return 2;
        memset(g_ch_a, 0, sizeof g_ch_a);
        memset(&g_lc, 0, sizeof g_lc); memset(g_lci, 0xff, sizeof g_lci); memset(g_call_late, 0, sizeof g_call_late); g_bc_ci = -1; g_end_fail_kind = 0; g_exc_dis_case = 0; g_unv_fail = 0; g_other_fail = 0; g_why = NULL; g_b_cannot = 0; g_cannot_why = 0; g_h9 = 0; g_fail_ci = -1; g_pred = 0; g_pred_why = 0; g_r_moved = 0; g_bw_n = 0; g_nbran = 0;
        memset(g_bdone, 0, sizeof g_bdone); memset(g_bseen, 0, sizeof g_bseen);
        g_nc = g_nR = g_ngr = g_nwait = g_npend = 0; g_cancelled = 0; memset(g_pw_after, 0, sizeof g_pw_after);
        g_nha = g_nhb = g_nda = g_ndb = 0; g_nprobe = 0; g_nhr = 0;
        memset(g_void_prev, 0, sizeof g_void_prev);
        memset(&g_chk_st, 0, sizeof g_chk_st);
        g_chk_ci = -1;
        g_rows_a.n = g_rows_b.n = 0;
        memset(g_sm, 0, sizeof g_sm);
        g_t = 0; g_onset = 16 * MS; g_newest = 0; g_last_onset = -1; g_x = g_y = 0; g_drains = 0;
        memset(g_last, 0, sizeof g_last);
        g_N = 0;
        if (!open_graph(&g_b, g_ar_b, g_ch_b, 0)) return 2;
        memset(g_ch_b, 0, sizeof g_ch_b);
        g_bN = INT64_MIN;
        memset(&c, 0, sizeof c);
        c.kind = C_RESET; c.t = 0;
        push(&c);
        nsteps = 60 + (int)(rnd() % 260);
        for (s = 0; s < nsteps && g_nc < MAXC - 40 && !g_why; s++) {
            int r = (int)(rnd() % 1000);
            g_t += dense ? 500 * US : (rnd() % 4 == 0) ? 0 : (int64_t)(1 + rnd() % 2500) * US;
            while (g_t > g_onset && !g_why) gen_frame();
            gen_feed(dense);
            if (r < 110) gen_group();
            else if (r < 140) { memset(&c, 0, sizeof c); c.kind = C_DRAIN; push(&c); }
            else if (r < 146 && !off('r')) { memset(&c, 0, sizeof c); c.kind = (rnd() & 1) ? C_RESET : C_RESET_TRIAL; c.t = g_t; push(&c); }
            else if (r < 150 && SIG_ARM_REF_SPEC) { memset(&c, 0, sizeof c); c.kind = C_FLUSH; push(&c); }
            else if (r < 175) { memset(&c, 0, sizeof c); c.kind = C_SETTLE; c.t = g_t; push(&c); }
            else if (r < 190 && !off('s')) { memset(&c, 0, sizeof c); c.kind = C_SET; c.node = N_AFF; c.param = YSIG_AFFINE_ANGLE; c.v = (rndf() - 0.5) * 90; push(&c); }
            else if (r < 198 && !off('s')) { memset(&c, 0, sizeof c); c.kind = C_DISARM; c.node = g_dets[rnd() % (unsigned)g_ndet]; push(&c); }
            else if (r < 206 && g_rel && !off('s')) { memset(&c, 0, sizeof c); c.kind = C_RECENTER; c.t = g_N; c.x = (rndf() - 0.5) * 10; c.y = (rndf() - 0.5) * 10; push(&c); }
        }
        if (!g_why) {
            g_onset = g_onset > g_t ? g_onset : g_t + MS;
            gen_frame();
            if (SIG_ARM_REF_SPEC) { memset(&c, 0, sizeof c); c.kind = C_FLUSH; push(&c); }
            memset(&c, 0, sizeof c); c.kind = C_DRAIN; push(&c);
        }
        for (s = 0; s < g_ngr; s++) if (g_gr[s].fallback) fb = 1;
        if (fb) fellback++;
        if (g_h9) { h9++; continue; }
        if (g_b_cannot == 2 && !g_why) { noroom++; continue; }
        if (g_b_cannot == 3 && !g_why) { disarm_ahead++; continue; }
        if ((getenv("SIG_ORACLE_ORDERCMP") || g_late >= 2) && !fb) {   /* a case with a fallback holds the header's order after it, not R's */
            int q, any = 0;
            order_cmp();
            for (q = 0; q < 8; q++) if (g_oc[q]) { oc_cases[q]++; oc_pairs[q] += g_oc[q]; any = 1; }
            if (any) { oc_any++; if (oc_show++ < 6 || (getenv("SIG_ORACLE_OCALL") && g_oc[4])) printf("  order: case %ld differs from the time order (frame/settle %ld, feed %ld, input at tau %ld, input after group %ld, group after later input %ld, state call %ld, groups %ld, other %ld)\n", g_case, g_oc[0], g_oc[1], g_oc[2], g_oc[3], g_oc[4], g_oc[5], g_oc[6], g_oc[7]); }
        }
        if (g_noA) g_why = NULL;   /* no 0.7.0 header: A is not compared (SIG_ORACLE_LATE_A=1 compares it) */
        if (g_late) {
            int counted = g_lc.far || g_lc.barrier || g_lc.cond2 || fb;   /* fb: a group fell back (arm_past_far, _full): counted */
            if (g_lc.late) { lt_cases++; lt_inputs += g_lc.late; }
            if (g_lc.far) lt_far++;
            if (g_lc.barrier) lt_bar++;
            if (g_lc.frames) lt_frames++;
            if (g_lc.cond2) lt_cond2++;
            lt_lnew += g_lc.l_new; lt_lnfa += g_lc.l_new_nfa;
            if (g_lc.closure) lt_clo++;
            if (g_lc.late_state) lt_lst++;
            if (g_lc.late_by_disarm) { lt_lbd++; lt_lbd_in += g_lc.late_by_disarm; }
            if (g_lc.cancel_eq) lt_ceq++;
            if (g_lc.late_state_span) lt_lss++;
            if (g_lc.closure_escape) lt_esc++;
            if (g_lc.cancel_letin) lt_cli++;
            if (g_lc.ooo_in || g_lc.ooo_grp) {
                lt_ooo++;
                if (g_lc.by_frame) lt_byf++;
                if (g_lc.by_fb) lt_byfb++;
                if (g_lc.by_other) lt_byo++;
                if (!counted && !g_pred) { lt_ooo_unc++; if (lt_show++ < 5) printf("  late: case %ld keeps an input out of time order for a node, nothing counted (inputs %ld, groups %ld)\n", g_case, g_lc.ooo_in, g_lc.ooo_grp); }
            }
            if (g_b_cannot && !g_why) {
                lt_bc++;
                if (counted || g_pred) lt_bc_f++;
                else if (1) printf("  late: B cannot run R in case %ld (reason %d), the text counts nothing\n", g_case, g_cannot_why);
            }
        }
        if (g_b_cannot && !g_why) {
            if (getenv("SIG_ORACLE_EMIT") && atol(getenv("SIG_ORACLE_EMIT")) == g_case) { emit_case(); return 0; }
            cannot++;
            if (g_cannot_why < 10) cannot_why[g_cannot_why]++;
#ifdef SIG_HAS_UNV
            {
                /* (B-cannot fix) arm_unverified or a fallback the guard stops
                 * on (pinned, far, full, off, retracted, stale): a fallback is
                 * not R's result, so the case is outside the bound either way.
                 * Read at the case's end, and at the call where B was found
                 * unable to run R (stricter: reads after it are unchecked) */
                long long u, o;
                a_counts(&u, &o);
                hu_n[0]++;
                if (u || o) hu_f[0]++;
                else if (hu_show++ < 40) printf("  header missed a B-cannot case %ld (reason %d)\n", g_case, g_cannot_why);
                if (g_bc_ci >= 0 && (g_unv_at[g_bc_ci] || g_oth_at[g_bc_ci])) hu_bc_at++;
                else if (u || o) printf("  B-cannot case %ld (reason %d, found at call %d) counted only after that call\n", g_case, g_cannot_why, g_bc_ci);
            }
#endif
            if (g_exc_dis_case) { exc_n[0]++; if (!g_pred) exc_only[0]++; }
            if (getenv("SIG_ORACLE_PRED")) {
                pred_n[0]++;
                if (g_pred) { pred_f[0]++; if (g_pred_why < 10) pred_why[g_pred_why]++; }
                else if (pred_show++ < 5) printf("  predicate missed a B-cannot case %ld (reason %d)\n", g_case, g_cannot_why);
            }
            continue;
        }
        if (!g_why && !g_noA) {
            g_end_fail_ci = -1;
            end_checks();
            if (g_why && g_fail_ci < 0) {
                /* (end-check fix) a failure found at the end: A's counts at
                 * the call that handed out the first wrong row or event; a
                 * hit's failure (R's hit missing, or the extra hits' total)
                 * has no such call here: the case's last call, an
                 * end-of-case read (see the change note) */
                g_fail_ci = g_end_fail_ci >= 0 ? g_end_fail_ci : g_nc - 1;
                g_end_fail_kind = g_end_fail_ci >= 0 ? 1 : 2;
                if (g_fail_ci >= 0) { g_unv_fail = g_unv_at[g_fail_ci]; g_other_fail = g_oth_at[g_fail_ci]; }
            }
        }
#ifdef SIG_HAS_UNV
        {
            ysig_stats st; int k2, made = 0;
            ysig_get_stats(&g_a, &st);
            if (g_why) {
                hu_n[1]++;
                if (g_unv_fail) hu_f[1]++;
                else if (g_other_fail) hu_other++;
                else { hu_bare++; printf("  header missed a wrong case %ld, no other count (%s)\n", g_case, g_why); }
                if (g_end_fail_kind == 2) { hu_end_late++; printf("  wrong case %ld (%s) found at the end with no call to read A's counts at: read at the last call\n", g_case, g_why); }
            }
            else {
                for (k2 = 0; k2 < g_ngr; k2++) if (!g_gr[k2].fallback) made++;
                hu_n[2]++; if (st.arm_unverified) hu_f[2]++; hu_arms += made; hu_cnt += st.arm_unverified;
            }
        }
#endif
        if (getenv("SIG_ORACLE_PRED")) {
            int cls = g_why ? 1 : 2;
            pred_n[cls]++;
            if (cls == 2) pred_tally();
            if (g_exc_dis_case) { exc_n[cls]++; if (!g_pred) exc_only[cls]++; }
            if (g_pred) { pred_f[cls]++; if (g_pred_why < 10) pred_why[g_pred_why]++; }
            else if (cls == 1 && pred_show++ < 5) printf("  predicate missed a wrong case %ld (%s)\n", g_case, g_why);
        }
        if (getenv("SIG_ORACLE_EMIT") && atol(getenv("SIG_ORACLE_EMIT")) == g_case) { emit_case(); return 0; }
        if (g_why) {
            wrong++;
            printf("FAIL case %ld: %s\n", g_case, g_why);
            for (w = 0; w < nnames && strcmp(names[w], g_why); w++) { }
            if (w == nnames && nnames < 64) {
                static char pool[64][256];
                snprintf(pool[nnames], sizeof pool[nnames], "%s", g_why);
                names[nnames] = pool[nnames]; counts[nnames++] = 0;
            }
            if (w < 64) counts[w]++;
            if (getenv("SIG_ORACLE_DEBUG")) {
                int q;
                static const char* kn[] = { "feed", "frame", "settle", "arm", "disarm", "set", "recenter", "reset", "reset_trial", "flush", "drain" };
                for (q = 0; q < g_nc; q++)
                    printf("  A%3d %-8s node %2d dev %d t %8.3f x %6.2f grp %d\n", q, kn[g_c[q].kind], g_c[q].node, g_c[q].dev, g_c[q].t / 1e6, g_c[q].x, g_call_grp[q]);
                printf("  R:");
                for (q = 0; q < g_nR; q++) printf(" %d", g_R[q].ci);
                printf("\n");
                printf("  detects:");
                for (q = 0; q < g_ndet; q++) {
                    const ysig_node* dn = &g_n[g_dets[q]];
                    printf(" [%d in %d test %g lane %g lv %g hy %g hold %g at %g start %g rep %g entry %g out %g]", g_dets[q], dn->in[0], dn->p[0], dn->p[1], dn->p[2], dn->p[3], dn->p[4], dn->p[5], dn->p[6], dn->p[7], dn->p[12], dn->p[13]);
                }
                printf("\n");
            }
            if (show && wrong <= show) {
                int q;
                for (q = 0; q < g_ngr; q++)
                    printf("  group %d: node %d tau %.3f cls %d n %d pinned %d fallback %d cancelled %d\n", q, g_gr[q].node, g_gr[q].tau / 1e6, g_gr[q].cls,
                           g_gr[q].n, g_gr[q].pinned, g_gr[q].fallback, g_gr[q].cancelled);
            }
        }
    }
    printf("cases %ld, wrong %ld, with a fallback %ld; not compared: B cannot run R %ld, an ahead arm with no room %ld, a disarm in a group B sees ahead %ld; spec mode %d\n",
           cases, wrong, fellback, cannot, noroom, disarm_ahead, SIG_ARM_REF_SPEC);
    if (h9) printf("  set aside as H9-shaped (SIG_ORACLE_H9): %ld\n", h9);
    if (g_late) printf("  late voids checked by the pre-ruling text (a header without late_hits_voided): %ld\n", g_lv_old);
    if (g_late >= 3)
        printf(g_late == 4 ? "  late (9834173) also: let-in arms cancelled by key %ld cases;" : "  late (7a17176) also:", lt_cli), printf(" T1 closure let in a group in %ld cases; a late state call (T3) in %ld cases; inputs late only by a plain disarm at N: %ld in %ld cases; a plain disarm cancelling an arm at its own tau: %ld cases; a late state call inside a waiting group's span, meeting it: %ld cases; a disarm at or before tau of a group the closure let in, made after its arm (it escapes the cancel): %ld cases\n",
               lt_clo, lt_lst, lt_lbd_in, lt_lbd, lt_ceq, lt_lss, lt_esc);
    if (getenv("SIG_ORACLE_ORDERCMP") || g_late >= 2)
        printf("  order (25837ee time order against this R, cases with no fallback): %ld cases differ; by class, cases (pairs): frame or settle %ld (%ld), feed %ld (%ld), input at a group's tau %ld (%ld), input after a group %ld (%ld), group after a later input %ld (%ld), state call outside a group %ld (%ld), group and group %ld (%ld), other %ld (%ld)\n",
               oc_any, oc_cases[0], oc_pairs[0], oc_cases[1], oc_pairs[1], oc_cases[2], oc_pairs[2], oc_cases[3], oc_pairs[3], oc_cases[4], oc_pairs[4], oc_cases[5], oc_pairs[5], oc_cases[6], oc_pairs[6], oc_cases[7], oc_pairs[7]);
    if (g_late)
        printf(g_late == 4 ? "  late (9834173 text):" : g_late == 3 ? "  late (7a17176 text):" : g_late == 2 ? "  late (25837ee text):" : "  late (d906e19 text):"), printf(" cases with a late input %ld (%ld inputs); the text counts: input_late_far %ld, input_late_barrier %ld, condition 2 %ld, input_late_frames %ld cases;"
               " R not in time order for a node %ld cases (the call it must follow: a frame %ld, a fallen-back group %ld, other %ld), %ld of them with no count; B cannot run R %ld, counted by the text %ld; a late input became a waiting group's L %ld times, %ld of them not meeting F_a\n",
               lt_cases, lt_inputs, lt_far, lt_bar, lt_cond2, lt_frames, lt_ooo, lt_byf, lt_byfb, lt_byo, lt_ooo_unc, lt_bc, lt_bc_f, lt_lnew, lt_lnfa);
#ifdef SIG_HAS_UNV
    printf("  header arm_unverified: B-cannot %ld of %ld, wrong %ld of %ld at the failing call (%ld more only later), right %ld of %ld cases; %ld counts over %ld made groups in right cases; wrong unflagged: %ld with a fallback (pinned included), retracted or stale count, %ld with none\n",
           hu_f[0], hu_n[0], hu_f[1], hu_n[1], hu_late, hu_f[2], hu_n[2], hu_cnt, hu_arms, hu_other, hu_bare);
    printf("  B-cannot counted (arm_unverified or a guard-stop fallback) by the call B was found unable: %ld of %ld; wrong cases read at the end for want of a call: %ld\n", hu_bc_at, hu_n[0], hu_end_late);
#else
    (void)hu_n; (void)hu_f; (void)hu_show; (void)hu_late; (void)hu_other; (void)hu_bare; (void)hu_bc_at; (void)hu_end_late; (void)hu_arms; (void)hu_cnt;
#endif
    if (getenv("SIG_ORACLE_PRED")) {
        printf("  predicate: flags %ld of %ld B-cannot cases, %ld of %ld wrong, %ld of %ld right (P1 barrier %ld, P1 meeting state call %ld, P1 no input yet %ld, P2 late report %ld, P1 cancelled group's disarm %ld, P1 input meets the group but not its arm %ld, P1 inside another group's block %ld, P1 a disarm of another group or none in its arm's gap %ld)\n",
               pred_f[0], pred_n[0], pred_f[1], pred_n[1], pred_f[2], pred_n[2], pred_why[1], pred_why[2], pred_why[3], pred_why[4], pred_why[5], pred_why[6], pred_why[7], pred_why[8]);
        printf("  exception for a waiting later group's disarm taken: B-cannot %ld (%ld unflagged), wrong %ld (%ld unflagged), right %ld (%ld unflagged)\n", exc_n[0], exc_only[0], exc_n[1], exc_only[1], exc_n[2], exc_only[2]);
        printf("  predicate, arms in right cases: %ld of %ld made groups flagged (past %ld of %ld, present %ld of %ld, ahead %ld of %ld)\n", g_pg_f, g_pg_n,
               g_pc_f[0], g_pc_n[0], g_pc_f[1], g_pc_n[1], g_pc_f[2], g_pc_n[2]);
    }

    if (cannot)
        printf("  B cannot run R: a barrier while an arm waits in B %ld; a meeting call not past tau while it waits: an arm %ld, a past group's call %ld, a present or ahead group's call %ld, a call in no group %ld; two waiting arms let in out of R's order %ld; a late input at or before a waiting arm's tau %ld; a disarm of a cancelled group at a tau not B's N %ld; a disarm of another group or none in the gap an arm closes %ld\n",
               cannot_why[1], cannot_why[2], cannot_why[3], cannot_why[4], cannot_why[5], cannot_why[6], cannot_why[7], cannot_why[8], cannot_why[9]);
    for (w = 0; w < nnames; w++) printf("  %6ld  %s\n", counts[w], names[w]);
    return wrong ? 1 : 0;
}
