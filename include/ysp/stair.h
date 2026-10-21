/* ysp/stair.h - v0.3.0 - public domain single-header adaptive staircase library
 *
 *   Nonparametric adaptive procedures for psychophysics: transformed and
 *   weighted up/down staircases (Wetherill & Levitt 1965; Levitt 1971;
 *   Kaernbach 1991) and accelerated stochastic approximation (Kesten 1958;
 *   Robbins & Monro 1951). One handle is one staircase; interleave several
 *   by owning several handles.
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no OS calls, no threads, no heap. Needs nothing but libm.
 *   Not a transport header, so it does not include ysp/rt.h.
 *
 *   Targets every platform the compiler does. C99 is the floor: it builds as
 *   C99, C11 and C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_STAIR_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *       #define YSP_STAIR_IMPLEMENTATION
 *       #include "ysp/stair.h"
 *
 *       yst_desc d = { 0 };             // unset fields are 0: the defaults
 *       d.start          = 0.5;         // starting level, caller's units
 *       d.n_up           = 1;           // 1-up ...
 *       d.n_down         = 3;           // ... 3-down: converges on 79.4%
 *       d.step_type      = YST_STEP_LOG;
 *       d.steps[0]       = 0.3;         // log10 units: a factor of 2
 *       d.steps[1]       = 0.15;        // halved after the first reversal
 *       d.steps[2]       = 0.075;       // and again after the second
 *       d.n_steps        = 3;
 *       d.min            = 0.001;
 *       d.max            = 1.0;
 *       d.stop_reversals = 10;
 *
 *       static yst_stair s;             // about 28 KB; see MEMORY AND THREADS
 *       if (!yst_open(&s, &d)) { fputs(yst_error(&s), stderr); return 1; }
 *
 *       while (!yst_done(&s)) {
 *           double level = yst_next(&s);
 *           int correct  = run_trial(level);          // 1 = correct, 0 = not
 *           if (yst_update(&s, level, correct) < 0) { fputs(yst_error(&s), stderr); break; }
 *       }
 *       double thr = yst_estimate(&s, YST_EST_REVERSALS);
 *       yst_close(&s);
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   A staircase is a rule that maps the trial history to the next level.
 *   The level is a double in the caller's units (contrast, dB, degrees,
 *   log10 of any of those). The header does not know what the units mean;
 *   it only knows that a CORRECT response (or a detection, a "yes") makes
 *   the task HARDER, which by default means a SMALLER level. Set
 *   desc.harder_is_up when a larger level is the harder one (a delay to
 *   detect, a noise amplitude, a masker contrast).
 *
 *   THE LOOP
 *     yst_next(s)              the level the rule proposes for this trial
 *     yst_update(s, x, r)      what was actually shown, and the outcome
 *     yst_done(s)              has a stop criterion been met
 *     yst_estimate(s, how)     the threshold from the history so far
 *
 *   next() and update() are separate on purpose. The level you can show is
 *   not always the level the rule asked for: a display quantizes contrast,
 *   a device clips, an interleaved design shows a different staircase's
 *   trial. Pass the level you SHOWED to update(); the rule steps from the
 *   level it PROPOSED, so the two need not agree, and the history records
 *   both. update() with a level the rule did not propose does not reset
 *   anything.
 *
 *   DIRECTION
 *     "Down" and "up" in this manual are directions of the LEVEL VALUE, not
 *     of difficulty: down is a smaller number. yst_trial.direction is the
 *     sign of the change the rule made to its own level after that trial
 *     (-1, +1, or 0 for no step). With the default harder_is_up = false a
 *     correct response steps down; with harder_is_up = true it steps up, and
 *     then desc.step_down_scale still scales the HARDER step, which is the
 *     one a correct response takes. Reversals do not care: they are a change
 *     of sign either way.
 *
 *   RULES (desc.rule)
 *     YST_RULE_UPDOWN  (default)
 *       Transformed up/down: after n_down consecutive correct responses the
 *       level steps down (harder); after n_up consecutive incorrect
 *       responses it steps up (easier). Every response resets the OTHER
 *       direction's counter, so the runs are of consecutive responses, and
 *       every step resets both. That is where this header and Palamedes'
 *       PAL_AMUD part: PAL_AMUD keeps the opposite counter across a
 *       response that causes no step, so on a 2-up-2-down track the sequence
 *       wrong, right, wrong steps up at its third response in PAL_AMUD, which
 *       has counted two wrongs, and here only at the next wrong, because the
 *       right reset the count. The two agree whenever n_up or n_down is 1,
 *       which is every 1-up-N-down and N-up-1-down rule; see STATUS.
 *       1-up-1-down converges on 50%,
 *       1-up-2-down on 70.7%, 1-up-3-down on 79.4%, 1-up-4-down on 84.1%
 *       (Levitt 1971, Table 1; the value is 0.5^(1/n_down) for n_up = 1).
 *       Weighted up/down (Kaernbach 1991): when step_down_scale != 1 the
 *       down step is step * step_down_scale. With n_up = n_down = 1 the
 *       procedure converges on p = 1 / (1 + step_down_scale), so a
 *       target p is reached with step_down_scale = (1 - p) / p;
 *       yst_weighted_scale(p) computes it. Unlike the transformed rule,
 *       the weighted rule converges on any p in (0, 1) with one response
 *       per step.
 *     YST_RULE_ASA
 *       Accelerated stochastic approximation (Kesten 1958). After each
 *       trial the level moves TOWARD THE HARDER END by c / m_k *
 *       (r - p_target), where r is the response (0 or 1) and c is
 *       desc.steps[0]. So with the default harder_is_up = false the level
 *       changes by -c / m_k * (r - p_target), and a correct response at a
 *       target below 1 lowers it. m_k is 1 for the first two trials and
 *       thereafter 1 + the number of reversals so far, COUNTING a reversal
 *       on this trial, so the step shrinks from the reversal on rather than
 *       from the trial after it. There is no schedule: n_steps must be 1
 *       and step_down_scale must be 0 or 1. Converges on desc.target_p,
 *       which is required for this rule and must be in (0, 1). Use
 *       YST_STEP_LIN or YST_STEP_LOG; a log step moves log10(level).
 *
 *   STEPS (desc.step_type, desc.steps[], desc.n_steps)
 *     YST_STEP_LIN   level += step, level -= step
 *     YST_STEP_LOG   log10(level) += step, i.e. level *= 10^step. The
 *                      level must stay positive; open() rejects a start,
 *                      min or max <= 0 under this type, and update()
 *                      returns YST_ERR_ARG for a shown level <= 0.
 *     YST_STEP_DB    20*log10(level) += step, i.e. level *= 10^(step/20).
 *                      Same positivity rule.
 *     steps[] is the step schedule: steps[0] until the first reversal,
 *     steps[1] until the second, and so on; after n_steps - 1 reversals the
 *     last entry stays in force. The trial that CAUSES a reversal already
 *     steps by the new size, which is PsychoPy's StairHandler order and
 *     what yst_trial.step_index records. This is PsychoPy's StairHandler
 *     semantic and Palamedes' PAL_AMUD stepSizeUp/Down with a schedule on
 *     top. One entry is a fixed step. The schedule counts reversals, not
 *     trials, so a run of correct responses at the start does not consume
 *     it. Every entry up to n_steps must be finite and > 0.
 *     YST_MAX_STEPS bounds n_steps (16).
 *
 *   INITIAL RULE (desc.initial_rule)
 *     Until the first reversal, step after every response as 1-up-1-down
 *     would, regardless of n_up and n_down. This gets a start far from
 *     threshold to threshold in fewer trials, at the cost of a first
 *     reversal that is not part of the equilibrium. PsychoPy's
 *     applyInitialRule (default true there); off by default here, because
 *     it changes which reversals count, and the estimate section says how.
 *
 *   LIMITS (desc.min, desc.max)
 *     A proposed level is clamped to [min, max] when both are nonzero (or
 *     when desc.use_limits is set; see the desc). The clamp is on the
 *     rule's own level, so it does not accumulate: a staircase held at max
 *     leaves max on the first step away from it. desc.start is clamped the
 *     same way. A clamped step is still a step: YST_EVENT_STEP is
 *     reported, the direction changes, and the schedule advances on a
 *     reversal, even when the level did not move. A proposal sitting
 *     exactly on min or max for desc.stop_at_limit consecutive trials ends
 *     the staircase with YST_STOP_LIMIT, because a staircase stuck at max
 *     is an observer who cannot do the task, not a threshold. The count is
 *     of trials proposed at a limit, whether a clamp put the level there or
 *     the rule simply has not stepped away yet.
 *
 *   STOPPING (desc.stop_reversals, desc.stop_trials)
 *     Either or both. The staircase is done when the reversal count reaches
 *     stop_reversals, or the trial count reaches stop_trials, whichever is
 *     first; 0 disables a criterion, and open() rejects both zero.
 *     yst_stop_reason() says which fired. When more than one criterion
 *     comes true on the same trial the reason reported is the first of
 *     REVERSALS, TRIALS, LIMIT, FULL, and it never changes afterward.
 *     Trials the caller submits after done() is true are accepted and
 *     recorded; done() just keeps saying yes. YST_MAX_TRIALS (1024) is
 *     the hard ceiling: the staircase is done with YST_STOP_FULL once
 *     that many trials are recorded, and update() returns YST_ERR_FULL
 *     past it. YST_MAX_REVERSALS bounds stop_reversals, and
 *     YST_MAX_TRIALS bounds stop_trials, so open() rejects a criterion
 *     the handle could not reach. Reversals past YST_MAX_REVERSALS are
 *     still counted but their levels are not recorded.
 *
 *   REVERSALS
 *     A reversal is a change in the direction of the step. The level at
 *     which the direction changed (the last level before the turn, as the
 *     rule PROPOSED it) is what the reversal records, which is the
 *     convention of Levitt, Palamedes and PsychoPy. The first step has no
 *     direction to reverse from; a clamp at a limit that would have stepped
 *     past it counts as a step in the limit's direction.
 *
 *   ESTIMATES (yst_estimate)
 *     YST_EST_REVERSALS      mean of the last desc.est_reversals reversal
 *                              levels (default, est_reversals = 0: all
 *                              reversals after the step schedule was
 *                              exhausted, and excluding the first reversal
 *                              when the initial rule was on). Even counts
 *                              pair peaks with troughs; an odd count biases
 *                              toward one. Levitt's recommendation, and the
 *                              default. A nonzero est_reversals is a plain
 *                              "last N reversals" with neither exclusion.
 *     YST_EST_TRIALS         mean of the levels of the last est_trials
 *                              trials (default: every trial after the
 *                              schedule was exhausted).
 *     YST_EST_MEDIAN_REV     median of the same reversals as above, for a
 *                              staircase with an outlier reversal. An even
 *                              count averages the two middle levels.
 *     YST_EST_LAST           the level the rule would propose next, which
 *                              is yst_next(); what ASA reports.
 *     YST_EST_REVERSALS and YST_EST_MEDIAN_REV summarize the rule's own
 *     track, so they use the PROPOSED level at each turn. YST_EST_TRIALS
 *     summarizes what the observer saw, so it uses the SHOWN level the
 *     caller passed to update(). The two agree unless the caller showed
 *     something other than the proposal.
 *     Means are taken in the step's units: a LOG or DB staircase averages
 *     log levels and returns the antilog (the geometric mean). The estimate
 *     is a summary of the track, not a fit; for a fit with a slope and
 *     confidence intervals, replay the history through ysp/quest.h or fit
 *     it offline.
 *
 *   ---------------------------------------------------------------------
 *   RETURN VALUES AND ERRORS
 *   ---------------------------------------------------------------------
 *   yst_open() and yst_open_saved() return bool and fill yst_error() on
 *   failure; they clear the message on entry. yst_save() takes a const
 *   handle, as yqst_save() does, so it returns its code and writes no
 *   message. update() returns 0, or a positive YST_EVENT_* mask
 *   for a step or a reversal, or a negative YST_ERR_*, and on a failure it
 *   also writes yst_error() (a null handle has nowhere to write).
 *   yst_strerror() names a code. The queries return a value and never write
 *   the message. yst_close() releases nothing (the handle owns no
 *   resource); it zeroes the handle, and is safe on a zeroed handle, after
 *   a failed open and a second time.
 *
 *     YST_ERR_ARG      null handle, response not 0 or 1, level not finite,
 *                        or a level <= 0 under YST_STEP_LOG / _DB
 *     YST_ERR_CLOSED   the handle was never opened
 *     YST_ERR_FULL     YST_MAX_TRIALS trials recorded
 *
 *   A closed handle answers every query without touching it: next()
 *   and estimate() give NaN, estimate_count() and n_trials() give 0,
 *   history() gives NULL, done() and is_open() give false, stop_reason()
 *   gives YST_STOP_NONE, reversal_level() gives NaN and
 *   reversal_trial() gives -1. So does a null handle.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   The handle holds everything inline: the full trial history
 *   (YST_MAX_TRIALS levels and responses), the reversal list
 *   (YST_MAX_REVERSALS) and the counters. About 28 KB at the defaults
 *   (28168 bytes on a 64-bit ABI).
 *   Nothing allocates, ever. Define YST_MAX_TRIALS or
 *   YST_MAX_REVERSALS before the include to resize both the handle and
 *   the ceilings. A handle is not thread-safe; one thread drives one
 *   staircase, and interleaved staircases on one thread are just several
 *   handles. Every function is deterministic: the same desc and the same
 *   history give the same proposals, so a saved history (yst_history())
 *   replayed through yst_update() restores the state exactly. A snapshot
 *   (SNAPSHOTS) restores it without the replay.
 *
 *   ---------------------------------------------------------------------
 *   SNAPSHOTS
 *   ---------------------------------------------------------------------
 *   A staircase can be saved at any trial and resumed later, after a crash
 *   or a break, in the form ysp/quest.h, ysp/aep.h and ysp/trials.h use:
 *
 *       size_t n = yst_save_size(&s);
 *       yst_save(&s, buf, n);                     // write buf to disk
 *       ...
 *       yst_close(&s);
 *       if (!yst_open_saved(&s, &desc, buf, n))   // s resumes where it was
 *           fputs(yst_error(&s), stderr);
 *
 *   What is saved: the desc's numbers as open() resolved them, the
 *   counters, the rule's level, the history and the recorded reversals. A
 *   staircase has no pending proposal (yst_next() changes nothing), so a
 *   cut anywhere resumes to the bit: the next proposal, every estimate and
 *   every later update are the uninterrupted session's.
 *
 *   yst_open_saved() is yst_open() and then a restore. It takes the desc
 *   so that a resume proves it runs the same staircase: the desc must agree
 *   with the snapshot on every number after open() folds the defaults
 *   (n_up 0 and 1 agree, so do step_down_scale 0 and 1, and use_limits is
 *   compared as "limits in force"). A mismatch fails the load and names the
 *   first field that differs.
 *
 *   LAYOUT, format 1. Every integer is little-endian two's complement,
 *   written and read one byte at a time, so a snapshot moves between
 *   compilers and platforms; an f64 is the IEEE 754 bit pattern as a u64.
 *     magic     4 bytes "YSTR", then u32 format (1)
 *     desc      f64 start, i32 rule, n_up, n_down, step_type, n_steps,
 *               n_steps f64 steps, f64 step_down_scale, target_p, min,
 *               max, u8 use_limits, harder_is_up, initial_rule, i32
 *               stop_reversals, stop_trials, stop_at_limit, est_reversals,
 *               est_trials
 *     state     i32 n_trials, n_reversals, run_correct, run_incorrect,
 *               direction, step_index, at_limit_run, stop; f64 level (in
 *               step units: log10 or dB under LOG and DB)
 *     history   n_trials x (f64 proposed, f64 shown, u8 response, u8
 *               reversal, i8 direction, u8 step_index)
 *     reversals i32 R, the reversals recorded (n_reversals, at most
 *               YST_MAX_REVERSALS), then R x (f64 level, i32 trial)
 *   143 bytes with one step, 8 more a step, 20 a trial and 12 a reversal:
 *   2,383 bytes after 100 trials with 20 reversals and one step.
 *   Every number is range-checked as it is read, so a wrong, truncated or
 *   corrupt snapshot fails the load with a message and leaves the handle
 *   closed. A snapshot with more trials than this build's YST_MAX_TRIALS,
 *   or saved by a build whose YST_MAX_REVERSALS recorded a different
 *   number of reversals, is refused and the message says so. Nothing else
 *   is checked: a snapshot edited to hold other in-range numbers loads, and
 *   the session continues from them. The format number changes when this
 *   layout does.
 *
 *   ---------------------------------------------------------------------
 *   PARAMETERS
 *   ---------------------------------------------------------------------
 *   yst_params() lists every yst_desc field that an experiment definition
 *   or a designer sets (rig_spec principle 8): its name, type, inclusive
 *   range, the value a zero field means, unit and a line of text. The
 *   shape is the one ysp/quest.h and ysp/aep.h use:
 *     name    the desc field; "a.b" is member b of a, and "a[].b" is
 *             member b of each element of the array a
 *     type    "i32", "f64", "bool", "enum", or an array of one of them
 *             ("f64[]"), whose length another row gives (steps has
 *             n_steps entries)
 *     def     what a zero field means when every other field is at its
 *             default; a field without a default (a required one) has 0,
 *             which can be outside [min, max], and its doc starts with
 *             "required"
 *     values  for an "enum", "name=value" pairs joined by ',' in value
 *             order, the names the C enum's suffix in lower case
 *             ("lin=0,log=1,db=2"); NULL for every other type
 *   The table is static and the same for every handle.
 *
 *   ---------------------------------------------------------------------
 *   SIMULATION
 *   ---------------------------------------------------------------------
 *   yst_simulate_response(p_correct, u) returns 1 when u < p_correct and
 *   0 otherwise; u is a uniform variate in [0, 1) that the caller draws.
 *   The header has no random generator, so an example's seed policy is the
 *   example's. examples/stair/sim.c runs a 3-down-1-up against a Weibull
 *   observer and prints the track and the estimate against the truth.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link but libm (-lm on Linux when not using a C++ driver).
 *   Define YST_API to override the default `extern` linkage; it is on the
 *   definitions too, so -DYST_API=static gives one translation unit a
 *   private copy.
 *
 *       cc -O2 -Iinclude -o stair_sim examples/stair/sim.c -lm
 *       cl /O2 /Iinclude examples\stair\sim.c
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - snapshots: yst_save_size(), yst_save() and yst_open_saved(),
 *          in the form of ysp/quest.h's, so a resumed session no longer
 *          replays its updates (gap G12 of docs/experiment_format.md). And
 *          yst_params(), the table of the desc's fields for a loader's key
 *          check and a designer's panel (gap G6). Adds API; changes none.
 *          No rule changed.
 *   v0.2.0 - the repository conventions (README.md, Conventions). Breaks
 *          the API: yst_close() is new (it zeroes the handle; every open
 *          has a close), and a failed yst_update() now writes yst_error()
 *          as well as returning the code. The USAGE block zeroes the desc
 *          and sets fields, so it builds as C99 and C++17. No rule changed.
 *   v0.1.2 - documentation only. STATUS records the replay against
 *          Palamedes' PAL_AMUD, now done. The rule's counter sentence said
 *          "the counters reset on every step and on every change of
 *          direction", which could be read two ways; it now says what the
 *          code does (the opposite counter resets on every response, both on
 *          every step) and where that differs from PAL_AMUD. No code changed.
 *   v0.1.1 - version macros (YST_VERSION_MAJOR / MINOR / PATCH / STRING)
 *          and yst_version(), so a program can log which header it was
 *          built from. No behavior changed.
 *   v0.1 - first implementation. Defining YSP_STAIR_IMPLEMENTATION now
 *          compiles the library instead of failing. The manual gained the
 *          detail a specification could leave open and an implementation
 *          cannot: which step size the reversal trial itself takes (the new
 *          one, as in PsychoPy), the sign convention of YST_RULE_ASA and
 *          what its m_k counts, which recorded level each estimator
 *          averages, the order the stop criteria are tested in, and the
 *          rest of the descs yst_open() rejects. YST_EVENT_STEP now
 *          means "the rule stepped", not "the value changed", so a step
 *          held by a limit still reports one.
 *   v0.0 - specification. Declarations and the manual, no implementation.
 *
 *   STATUS: v0.3.0. See the end of this section for v0.3.0. Built and run on Linux (WSL2) with gcc 11.4 as C11, C99 and
 *   C++17 under -Wall -Wextra -Wpedantic -Wshadow -Werror, and once under
 *   -fsanitize=address,undefined -fno-sanitize-recover=all with no
 *   diagnostic. Also built and run on Windows 11 with MSVC 14.51 under
 *   /W4 /WX, both in its default (pre-C11) C dialect and as /std:c++17;
 *   every number the test prints is identical on the two compilers.
 *   tests/adapt/stair_test.c, at YST_MAX_TRIALS 128 and
 *   YST_MAX_REVERSALS 32, checks hand-derived tracks for 1-up-1-down,
 *   1-up-2-down, 1-up-3-down and 2-up-1-down (proposals, reversal indices,
 *   reversal levels), the step schedule, the initial rule, LIN / LOG / DB steps, the
 *   limits and every stop reason, the weighted rule, ASA, all four
 *   estimators, YST_ERR_FULL at YST_MAX_TRIALS, and every desc
 *   yst_open() is documented to reject. A simulated Weibull observer over
 *   204000 pooled trials puts the empirical proportion correct within 0.007
 *   of yst_convergence_p for 1-up-2-down (0.7105 against 0.7071),
 *   1-up-3-down (0.7970 against 0.7937) and a weighted 1-up-1-down (0.7565
 *   against 0.7500), against a 0.01 tolerance. All three sit ABOVE the
 *   target, which is the finite-step bias of the rule and not noise; shrink
 *   the step to shrink it.
 *   Replayed against Palamedes' PAL_AMUD through the MEX binding in MATLAB
 *   R2023a (tests/compare/compare_stair_palamedes.m), on fixed 80-trial
 *   response sequences: 1-up-2-down, 1-up-3-down, 2-up-1-down, a reversal
 *   stop, a weighted 1-up-1-down, limits, log10 steps and a caller-driven
 *   step schedule are identical, level for level and reversal for reversal.
 *   PAL_AMUD always applies the initial rule, so those descs set
 *   initial_rule. Two differences are documented rather than papered over.
 *   PAL_AMUD keeps the opposite-direction counter when a response causes no
 *   step, where this header resets it on every response; the two agree
 *   whenever n_up or n_down is 1 (see RULES). And a caller who changes
 *   Palamedes' step after the reversing update gives the reversal trial the
 *   OLD step, where this header, like PsychoPy, gives it the new one.
 *   PsychoPy's StairHandler is replayed too, through the Python binding
 *   (tests/compare/compare_stair_psychopy.py): on seven configurations
 *   (1-up-2-down and 1-up-3-down and 2-up-1-down, step schedules, the
 *   initial rule on and off, limits, linear, dB and log steps) no proposal
 *   differs on any trial, the reversal indices and levels are the same, and
 *   the estimates agree, the log and dB cases to 1e-15 because PsychoPy
 *   multiplies where this header adds in log units.
 *   v0.2.0 on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, C and C++17, and
 *   MinGW-w64 gcc -Werror): the test adds yst_close() on a zeroed handle,
 *   after a failed open and twice, and the messages a failed update leaves.
 *   v0.3.0 on 2026-10-21 (Windows 11, MSVC 19.44 /W4 /WX as C and C++17,
 *   MinGW-w64 gcc 16.1 -Werror, WSL2 gcc 11.4 under
 *   -fsanitize=address,undefined, emcc): the test checks every row of
 *   yst_params() against the desc field it names (by offsetof), each enum's
 *   values against the C enum, and each zero field against def, both as
 *   open() resolves it and as a 40-trial run. Snapshots cut at up to nine
 *   points in four configurations (LOG with a schedule, limits and the
 *   initial rule; weighted dB; ASA; a run past YST_MAX_TRIALS with more
 *   reversals than YST_MAX_REVERSALS) and resumed into a handle filled
 *   with garbage give the uninterrupted run's history, reversals, counters,
 *   estimates and final snapshot, byte for byte. Every truncation, a
 *   trailing byte, a wrong magic or format, crafted counters, history and
 *   reversals are refused with the handle closed; each of the 5,208
 *   single-bit flips of a 651-byte snapshot loads or is refused, none
 *   crashes. 16 of 16 mutants killed (tests/mutate/stair.toml).
 *   tests/fuzz/adapt_fuzz.c: see docs/adapt.md for its run.
 *   What is NOT done here: nothing here is a timing measurement. No
 *   estimator is a fit and none carries a confidence interval. A
 *   -DYST_API=static build needs a translation unit that calls every
 *   function, or -Wno-unused-function.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_STAIR_H_INCLUDED
#define YSP_STAIR_H_INCLUDED

/* The version of this header, for a log line or a compile-time check. The
 * string is the three numbers, and the test asserts that it stays so. */
#define YST_VERSION_MAJOR  0
#define YST_VERSION_MINOR  3
#define YST_VERSION_PATCH  0
#define YST_VERSION_STRING "0.3.0"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YST_API
#define YST_API extern
#endif

/* Handle capacities. Both are compile-time so the handle has no heap and a
 * fixed size; raise them before the include for a long session. */
#ifndef YST_MAX_TRIALS
#define YST_MAX_TRIALS 1024
#endif
#ifndef YST_MAX_REVERSALS
#define YST_MAX_REVERSALS 256
#endif
#define YST_MAX_STEPS 16

/* --- codes ------------------------------------------------------------- */

#define YST_OK          0
#define YST_ERR_ARG    (-1)  /* null handle, bad response, bad level      */
#define YST_ERR_CLOSED (-2)  /* yst_open() has not succeeded on this handle */
#define YST_ERR_FULL   (-3)  /* YST_MAX_TRIALS reached                    */

/* Bits in a positive yst_update() return. */
#define YST_EVENT_STEP     1  /* the rule stepped (a limit may have held
                                 * the level where it was)                  */
#define YST_EVENT_REVERSAL 2  /* the step direction changed                */
#define YST_EVENT_DONE     4  /* a stop criterion fired on this trial      */

/* Static description of a YST_ERR_* code ("ok" for values >= 0). */
YST_API const char* yst_strerror(int code);

/* YST_VERSION_STRING, as compiled into the implementation. Beside the macro,
 * which is what the CALLER was compiled against, it tells a program that links
 * a prebuilt implementation which one it got. */
YST_API const char* yst_version(void);

/* --- description ------------------------------------------------------- */

typedef enum yst_rule {
    YST_RULE_UPDOWN = 0, /* transformed / weighted up-down (default)     */
    YST_RULE_ASA         /* accelerated stochastic approximation         */
} yst_rule;

typedef enum yst_step_type {
    YST_STEP_LIN = 0,    /* level += step                                 */
    YST_STEP_LOG,        /* log10(level) += step                          */
    YST_STEP_DB          /* 20 log10(level) += step                       */
} yst_step_type;

typedef enum yst_estimator {
    YST_EST_REVERSALS = 0, /* mean of the counted reversal levels         */
    YST_EST_TRIALS,        /* mean of the counted trial levels            */
    YST_EST_MEDIAN_REV,    /* median of the counted reversal levels       */
    YST_EST_LAST           /* the next proposed level                     */
} yst_estimator;

typedef enum yst_stop {
    YST_STOP_NONE = 0,   /* still running                                 */
    YST_STOP_REVERSALS,  /* desc.stop_reversals reached                   */
    YST_STOP_TRIALS,     /* desc.stop_trials reached                      */
    YST_STOP_LIMIT,      /* pinned at min or max for stop_at_limit trials */
    YST_STOP_FULL        /* YST_MAX_TRIALS reached                      */
} yst_stop;

/* Staircase description. Zero-initialize it and set only what you need.
 * Required: start, at least one of stop_reversals / stop_trials, and
 * steps[0] with n_steps >= 1. Under YST_RULE_ASA also target_p. */
typedef struct yst_desc {
    double     start;           /* first proposed level, caller's units     */
    yst_rule rule;            /* YST_RULE_UPDOWN (0) or ASA             */
    int        n_up;            /* incorrect responses per up step; 0 = 1   */
    int        n_down;          /* correct responses per down step; 0 = 1   */
    yst_step_type step_type;  /* LIN (0), LOG or DB                       */
    double     steps[YST_MAX_STEPS]; /* step schedule, one per reversal    */
    int        n_steps;         /* entries in steps[]; 1 = fixed step       */
    double     step_down_scale; /* harder ("down") step = step * this; 0 = 1
                                 * (equal). != 1 makes a weighted staircase */
    double     target_p;        /* ASA only: the proportion to converge on  */
    double     min, max;        /* proposal clamp; both 0 = no limits unless
                                 * use_limits is set (a real 0 bound)       */
    bool       use_limits;      /* honor min/max even when both are 0       */
    bool       harder_is_up;    /* a correct response RAISES the level      */
    bool       initial_rule;    /* 1-up-1-down until the first reversal     */
    int        stop_reversals;  /* stop after this many reversals; 0 = off  */
    int        stop_trials;     /* stop after this many trials; 0 = off     */
    int        stop_at_limit;   /* stop after this many consecutive trials
                                 * pinned at min or max; 0 = never          */
    int        est_reversals;   /* reversals YST_EST_REVERSALS averages;
                                 * 0 = all after the schedule ran out       */
    int        est_trials;      /* trials YST_EST_TRIALS averages; 0 = all
                                 * after the schedule ran out               */
} yst_desc;

/* --- handle ------------------------------------------------------------ */

/* One recorded trial. yst_history() hands out the array. */
typedef struct yst_trial {
    double  proposed;   /* what yst_next() returned before this trial     */
    double  shown;      /* what the caller passed to yst_update()         */
    uint8_t response;   /* 0 or 1                                           */
    uint8_t reversal;   /* 1 when this trial's step reversed direction      */
    int8_t  direction;  /* step taken after this trial: -1 down, +1 up, 0   */
    uint8_t step_index; /* index into desc.steps in force for this trial    */
} yst_trial;

/* Staircase handle. The caller allocates it and treats every field as
 * opaque. Must be zeroed or previously opened before yst_open(). Sized by
 * YST_MAX_TRIALS and YST_MAX_REVERSALS; see MEMORY AND THREADS. */
typedef struct yst_stair {
    yst_desc  desc;
    double      level;          /* next proposal, in step units (log for
                                 * LOG/DB) so a step is an add              */
    int         n_trials;
    int         n_reversals;
    int         run_correct;    /* consecutive correct since the last step  */
    int         run_incorrect;
    int         direction;      /* last step direction; 0 before the first  */
    int         step_index;
    int         at_limit_run;   /* consecutive trials pinned at a limit     */
    yst_stop  stop;
    bool        open;
    yst_trial history[YST_MAX_TRIALS];
    double      reversals[YST_MAX_REVERSALS]; /* level at each reversal,
                                                 * caller's units          */
    int         reversal_trial[YST_MAX_REVERSALS]; /* trial index of each */
    char        error[256];
} yst_stair;

/* --- lifecycle --------------------------------------------------------- */

/* Validate `desc` and reset `s` to its first trial. Returns false and fills
 * yst_error() on a bad desc: a null handle or desc, no stop criterion, no
 * step (n_steps < 1) or n_steps > YST_MAX_STEPS, a step that is not finite
 * and positive, a non-finite start, LOG/DB with a non-positive start, min or
 * max, min > max with limits on, a negative n_up, n_down, step_down_scale,
 * stop_* or est_*, stop_reversals > YST_MAX_REVERSALS, stop_trials >
 * YST_MAX_TRIALS, an out-of-range rule or step_type, ASA without a
 * target_p in (0, 1), and ASA with n_steps != 1 or step_down_scale not 0 or
 * 1. Clears the message on entry. */
YST_API bool yst_open(yst_stair* s, const yst_desc* desc);

/* Zero the handle. It owns nothing, so this releases nothing; every open
 * has a close. Safe on a zeroed handle, after a failed open and twice. */
YST_API void yst_close(yst_stair* s);

/* The message of the last failed yst_open() or yst_update() on this handle
 * ("" if none). */
YST_API const char* yst_error(const yst_stair* s);

/* True after a successful yst_open(). */
YST_API bool yst_is_open(const yst_stair* s);

/* --- the loop ---------------------------------------------------------- */

/* The level the rule proposes for the next trial, in the caller's units and
 * inside [min, max] when limits are on. Calling it twice without an update
 * returns the same value. NaN on a closed handle. */
YST_API double yst_next(const yst_stair* s);

/* Record a trial shown at `level` with `response` (1 = correct / detected,
 * 0 = incorrect / missed) and advance the rule. Returns a YST_EVENT_*
 * mask (0 when nothing but the counters moved) or a negative YST_ERR_*,
 * with the reason in yst_error(). `level` is recorded but does not alter the rule's own level; see MODEL. */
YST_API int yst_update(yst_stair* s, double level, int response);

/* True once a stop criterion has fired. Stays true; later updates are still
 * recorded. */
YST_API bool yst_done(const yst_stair* s);

/* Which criterion ended the staircase, YST_STOP_NONE while running. */
YST_API yst_stop yst_stop_reason(const yst_stair* s);

/* --- estimates --------------------------------------------------------- */

/* Threshold estimate per `how`; see ESTIMATES. NaN when the history has
 * nothing to average (no counted reversal yet). Cheap; call it every trial
 * if you like. */
YST_API double yst_estimate(const yst_stair* s, yst_estimator how);

/* Number of reversals / trials the YST_EST_REVERSALS, YST_EST_TRIALS or
 * YST_EST_MEDIAN_REV window would average right now; 1 for
 * YST_EST_LAST. Zero means the estimate is NaN. Log these with the
 * estimate. */
YST_API int yst_estimate_count(const yst_stair* s, yst_estimator how);

/* step_down_scale that makes a 1-up-1-down weighted staircase converge on
 * `target_p`: (1 - p) / p. Kaernbach 1991. NaN outside (0, 1). */
YST_API double yst_weighted_scale(double target_p);

/* The proportion correct a transformed n_up / n_down staircase converges
 * on, with step_down_scale applied (0 means 1). For n_up = 1 this is
 * (1 / (1 + scale))^(1 / n_down); for n_down = 1 it is
 * 1 - (scale / (1 + scale))^(1 / n_up). NaN when the closed form does not
 * apply (n_up > 1 with n_down > 1); such rules exist but are not
 * tabulated. */
YST_API double yst_convergence_p(int n_up, int n_down, double step_down_scale);

/* --- history ----------------------------------------------------------- */

YST_API int yst_n_trials(const yst_stair* s);
YST_API int yst_n_reversals(const yst_stair* s);

/* The recorded trials, oldest first. `*n` receives the count. The pointer
 * is into the handle and stays valid until the handle is reopened. NULL
 * with *n = 0 on a closed handle. */
YST_API const yst_trial* yst_history(const yst_stair* s, int* n);

/* Level (caller's units) and trial index of reversal `i`, or NaN / -1. */
YST_API double yst_reversal_level(const yst_stair* s, int i);
YST_API int    yst_reversal_trial(const yst_stair* s, int i);

/* --- snapshot ---------------------------------------------------------- */

/* Save a staircase and resume it later without replaying it.
 * yst_save_size() is the bytes yst_save() writes (0 on a handle that is not
 * open); yst_save() writes them and returns the count, or YST_ERR_ARG when
 * `buf` is NULL or `cap` is too small, or YST_ERR_CLOSED. It writes no
 * message (the handle is const). yst_open_saved() opens `s` from `desc`
 * exactly as yst_open() would, checks that the desc agrees with the
 * snapshot, and puts the counters, the level, the history and the
 * reversals back. On any failure it returns false, leaves `s` closed, and
 * says why in yst_error(). SNAPSHOTS in the manual gives the layout. */
YST_API size_t yst_save_size(const yst_stair* s);
YST_API int    yst_save(const yst_stair* s, void* buf, size_t cap);
YST_API bool   yst_open_saved(yst_stair* s, const yst_desc* desc, const void* buf, size_t len);

/* --- parameters -------------------------------------------------------- */

/* One settable yst_desc field. PARAMETERS in the manual gives the rules;
 * ysp/quest.h and ysp/aep.h have tables of the same shape. */
typedef struct yst_param {
    const char* name;    /* the desc field; "a.b" a member, "a[]" an array   */
    const char* type;    /* "i32", "f64", "bool", "enum", "f64[]"            */
    double      min, max;/* inclusive                                        */
    double      def;     /* the value a zero field means                     */
    const char* unit;    /* "" for the caller's units or a count             */
    const char* doc;
    const char* values;  /* "enum": "name=value,..." in value order; NULL    */
} yst_param;

/* The table of desc fields a definition or a designer sets; *n gets the
 * count. Static: the pointer stays valid for the life of the program. */
YST_API const yst_param* yst_params(int* n);

/* --- simulation -------------------------------------------------------- */

/* 1 when u < p_correct, else 0. `u` is a uniform variate in [0, 1) drawn by
 * the caller; the header has no generator. */
YST_API int yst_simulate_response(double p_correct, double u);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_STAIR_H_INCLUDED */

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_STAIR_IMPLEMENTATION
#ifndef YSP_STAIR_IMPLEMENTATION_GUARD
#define YSP_STAIR_IMPLEMENTATION_GUARD

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef NAN
    #define YST__NAN ((double)NAN)
#else
    /* No NAN macro means a pre-C99 library; inf - inf is the portable
     * spelling and this branch never compiles on the supported toolchains. */
    #define YST__NAN (HUGE_VAL - HUGE_VAL)
#endif

/* isfinite() is C99 and MSVC's default C dialect does not declare it, so the
 * test is written in comparisons a NaN loses on its own. */
static bool yst__finite(double x) {
    return x > -HUGE_VAL && x < HUGE_VAL;
}

/* --- unit conversion ---------------------------------------------------
 * The handle keeps the level in STEP units so every rule is an addition.
 * These two are the only places the caller's units are entered or left. */

static double yst__to_step(const yst_stair* s, double v) {
    switch (s->desc.step_type) {
    case YST_STEP_LOG: return log10(v);
    case YST_STEP_DB:  return 20.0 * log10(v);
    default:             return v;
    }
}

static double yst__from_step(const yst_stair* s, double v) {
    switch (s->desc.step_type) {
    case YST_STEP_LOG: return pow(10.0, v);
    case YST_STEP_DB:  return pow(10.0, v / 20.0);
    default:             return v;
    }
}

/* yst_open() normalizes desc.use_limits to mean "limits are in force", so
 * the two-nonzero-bounds rule is decided once instead of at every clamp. */
static double yst__lo(const yst_stair* s) { return yst__to_step(s, s->desc.min); }
static double yst__hi(const yst_stair* s) { return yst__to_step(s, s->desc.max); }

static double yst__clamp(const yst_stair* s, double v) {
    double lo, hi;
    if (!s->desc.use_limits) return v;
    lo = yst__lo(s);
    hi = yst__hi(s);
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool yst__at_limit(const yst_stair* s, double v) {
    if (!s->desc.use_limits) return false;
    return v <= yst__lo(s) || v >= yst__hi(s);
}

/* --- codes ------------------------------------------------------------- */

YST_API const char* yst_strerror(int code) {
    switch (code) {
    case YST_ERR_ARG:    return "invalid argument";
    case YST_ERR_CLOSED: return "staircase is not open";
    case YST_ERR_FULL:   return "trial history is full";
    default:               return code >= 0 ? "ok" : "unknown error";
    }
}

YST_API const char* yst_version(void) { return YST_VERSION_STRING; }

/* --- lifecycle --------------------------------------------------------- */

/* A failed update leaves a message of the same form open() does, so a
 * caller logs one string whichever call failed. */
static int yst__fail(yst_stair* s, int rc, const char* what) {
    snprintf(s->error, sizeof(s->error), "ysp_stair: %s: %s", what, yst_strerror(rc));
    return rc;
}

#define YST__FAIL(s, msg) \
    do { snprintf((s)->error, sizeof((s)->error), "ysp_stair: %s", (msg)); \
         return false; } while (0)

YST_API bool yst_open(yst_stair* s, const yst_desc* desc) {
    bool limits;
    int i;

    if (!s) return false;
    memset(s, 0, sizeof(*s));
    if (!desc) YST__FAIL(s, "yst_open: null desc");

    if (desc->rule != YST_RULE_UPDOWN && desc->rule != YST_RULE_ASA)
        YST__FAIL(s, "desc.rule is not a yst_rule");
    if (desc->step_type != YST_STEP_LIN && desc->step_type != YST_STEP_LOG &&
        desc->step_type != YST_STEP_DB)
        YST__FAIL(s, "desc.step_type is not a yst_step_type");

    if (!yst__finite(desc->start)) YST__FAIL(s, "desc.start is not finite");
    if (desc->n_steps < 1) YST__FAIL(s, "desc.n_steps must be at least 1");
    if (desc->n_steps > YST_MAX_STEPS)
        YST__FAIL(s, "desc.n_steps is above YST_MAX_STEPS");
    for (i = 0; i < desc->n_steps; i++) {
        if (!yst__finite(desc->steps[i]) || desc->steps[i] <= 0.0)
            YST__FAIL(s, "every desc.steps entry must be finite and positive");
    }
    if (desc->n_up < 0 || desc->n_down < 0)
        YST__FAIL(s, "desc.n_up and desc.n_down must not be negative");
    if (!yst__finite(desc->step_down_scale) || desc->step_down_scale < 0.0)
        YST__FAIL(s, "desc.step_down_scale must be finite and not negative");
    if (desc->stop_reversals < 0 || desc->stop_trials < 0 ||
        desc->stop_at_limit < 0 || desc->est_reversals < 0 || desc->est_trials < 0)
        YST__FAIL(s, "desc counts must not be negative");
    if (desc->stop_reversals == 0 && desc->stop_trials == 0)
        YST__FAIL(s, "desc needs stop_reversals or stop_trials");
    if (desc->stop_reversals > YST_MAX_REVERSALS)
        YST__FAIL(s, "desc.stop_reversals is above YST_MAX_REVERSALS");
    if (desc->stop_trials > YST_MAX_TRIALS)
        YST__FAIL(s, "desc.stop_trials is above YST_MAX_TRIALS");

    limits = desc->use_limits || (desc->min != 0.0 && desc->max != 0.0);
    if (limits) {
        if (!yst__finite(desc->min) || !yst__finite(desc->max))
            YST__FAIL(s, "desc.min and desc.max must be finite");
        if (desc->min > desc->max) YST__FAIL(s, "desc.min is above desc.max");
    }
    if (desc->step_type != YST_STEP_LIN) {
        if (desc->start <= 0.0)
            YST__FAIL(s, "desc.start must be positive under a LOG or DB step");
        if (limits && (desc->min <= 0.0 || desc->max <= 0.0))
            YST__FAIL(s, "desc.min and desc.max must be positive under a LOG or DB step");
    }

    if (desc->rule == YST_RULE_ASA) {
        if (!yst__finite(desc->target_p) || desc->target_p <= 0.0 || desc->target_p >= 1.0)
            YST__FAIL(s, "YST_RULE_ASA needs desc.target_p in (0, 1)");
        if (desc->n_steps != 1)
            YST__FAIL(s, "YST_RULE_ASA has no step schedule: desc.n_steps must be 1");
        if (desc->step_down_scale != 0.0 && desc->step_down_scale != 1.0)
            YST__FAIL(s, "YST_RULE_ASA needs desc.step_down_scale 0 or 1");
    }

    s->desc = *desc;
    /* Fold the "0 means default" fields once so the rules read plain values. */
    if (s->desc.n_up <= 0) s->desc.n_up = 1;
    if (s->desc.n_down <= 0) s->desc.n_down = 1;
    if (s->desc.step_down_scale <= 0.0) s->desc.step_down_scale = 1.0;
    s->desc.use_limits = limits;

    s->level = yst__clamp(s, yst__to_step(s, desc->start));
    s->stop = YST_STOP_NONE;
    s->open = true;
    s->error[0] = '\0';
    return true;
}

YST_API void yst_close(yst_stair* s) {
    if (s) memset(s, 0, sizeof(*s));
}

YST_API const char* yst_error(const yst_stair* s) {
    return s ? s->error : "ysp_stair: null staircase handle";
}

YST_API bool yst_is_open(const yst_stair* s) {
    return s != NULL && s->open;
}

/* --- the loop ---------------------------------------------------------- */

YST_API double yst_next(const yst_stair* s) {
    if (!s || !s->open) return YST__NAN;
    return yst__from_step(s, s->level);
}

YST_API int yst_update(yst_stair* s, double level, int response) {
    double proposed, step = 0.0, delta;
    int ti, dir, harder, events;
    bool reversal;

    if (!s) return YST_ERR_ARG;
    if (!s->open) return yst__fail(s, YST_ERR_CLOSED, "yst_update");
    if (response != 0 && response != 1)
        return yst__fail(s, YST_ERR_ARG, "yst_update: response is not 0 or 1");
    if (!yst__finite(level))
        return yst__fail(s, YST_ERR_ARG, "yst_update: level is not finite");
    /* A LOG or DB history is averaged as a logarithm, so a non-positive
     * shown level would poison the estimate rather than fail here. */
    if (s->desc.step_type != YST_STEP_LIN && level <= 0.0)
        return yst__fail(s, YST_ERR_ARG, "yst_update: level must be positive under a LOG or DB step");
    if (s->n_trials >= YST_MAX_TRIALS) {
        if (s->stop == YST_STOP_NONE) s->stop = YST_STOP_FULL;
        return yst__fail(s, YST_ERR_FULL, "yst_update");
    }

    ti = s->n_trials;
    proposed = yst__from_step(s, s->level);
    s->at_limit_run = yst__at_limit(s, s->level) ? s->at_limit_run + 1 : 0;

    dir = 0;
    delta = 0.0;
    harder = 0;

    if (s->desc.rule == YST_RULE_ASA) {
        double m;
        /* The move is toward the harder end; see RULES. */
        delta = (double)response - s->desc.target_p;
        dir = (delta > 0.0) ? 1 : -1;               /* harder, in difficulty */
        if (!s->desc.harder_is_up) dir = -dir;      /* ... now in level units */
        reversal = (s->direction != 0 && dir != s->direction);
        if (reversal) {
            if (s->n_reversals < YST_MAX_REVERSALS) {
                s->reversals[s->n_reversals] = proposed;
                s->reversal_trial[s->n_reversals] = ti;
            }
            s->n_reversals++;
        }
        m = (ti < 2) ? 1.0 : (double)(1 + s->n_reversals);
        delta = s->desc.steps[0] / m * fabs(delta) * (double)dir;
    } else {
        int n_up = s->desc.n_up, n_down = s->desc.n_down;

        if (response) { s->run_correct++; s->run_incorrect = 0; }
        else          { s->run_incorrect++; s->run_correct = 0; }

        if (s->desc.initial_rule && s->n_reversals == 0) { n_up = 1; n_down = 1; }

        if (s->run_correct >= n_down) harder = 1;
        else if (s->run_incorrect >= n_up) harder = -1;

        reversal = false;
        if (harder != 0) {
            s->run_correct = 0;
            s->run_incorrect = 0;
            dir = s->desc.harder_is_up ? harder : -harder;
            reversal = (s->direction != 0 && dir != s->direction);
            if (reversal) {
                if (s->n_reversals < YST_MAX_REVERSALS) {
                    s->reversals[s->n_reversals] = proposed;
                    s->reversal_trial[s->n_reversals] = ti;
                }
                s->n_reversals++;
                /* PsychoPy's order: the reversal trial already steps by the
                 * schedule's next entry. */
                s->step_index = (s->n_reversals < s->desc.n_steps)
                              ? s->n_reversals : s->desc.n_steps - 1;
            }
            step = s->desc.steps[s->step_index];
            if (harder > 0) step *= s->desc.step_down_scale;
            delta = (double)dir * step;
        }
    }

    if (dir != 0) {
        s->level = yst__clamp(s, s->level + delta);
        s->direction = dir;
    }

    s->history[ti].proposed   = proposed;
    s->history[ti].shown      = level;
    s->history[ti].response   = (uint8_t)response;
    s->history[ti].reversal   = (uint8_t)(reversal ? 1 : 0);
    s->history[ti].direction  = (int8_t)dir;
    s->history[ti].step_index = (uint8_t)s->step_index;
    s->n_trials = ti + 1;

    events = 0;
    if (dir != 0) events |= YST_EVENT_STEP;
    if (reversal) events |= YST_EVENT_REVERSAL;

    if (s->stop == YST_STOP_NONE) {
        if (s->desc.stop_reversals > 0 && s->n_reversals >= s->desc.stop_reversals)
            s->stop = YST_STOP_REVERSALS;
        else if (s->desc.stop_trials > 0 && s->n_trials >= s->desc.stop_trials)
            s->stop = YST_STOP_TRIALS;
        else if (s->desc.stop_at_limit > 0 && s->at_limit_run >= s->desc.stop_at_limit)
            s->stop = YST_STOP_LIMIT;
        else if (s->n_trials >= YST_MAX_TRIALS)
            s->stop = YST_STOP_FULL;
        if (s->stop != YST_STOP_NONE) events |= YST_EVENT_DONE;
    }
    return events;
}

YST_API bool yst_done(const yst_stair* s) {
    return s != NULL && s->open && s->stop != YST_STOP_NONE;
}

YST_API yst_stop yst_stop_reason(const yst_stair* s) {
    return (s && s->open) ? s->stop : YST_STOP_NONE;
}

/* --- estimates --------------------------------------------------------- */

/* The reversal window [*first, *first + n). Only recorded reversals are in
 * it, so a run past YST_MAX_REVERSALS estimates from what it kept. */
static int yst__rev_window(const yst_stair* s, int* first) {
    int n = s->n_reversals;
    int f;
    if (n > YST_MAX_REVERSALS) n = YST_MAX_REVERSALS;
    if (s->desc.est_reversals > 0) {
        f = n - s->desc.est_reversals;
        if (f < 0) f = 0;
    } else {
        f = s->desc.n_steps - 1;
        if (s->desc.initial_rule && f < 1) f = 1;
        if (f > n) f = n;
    }
    *first = f;
    return n - f;
}

static int yst__trial_window(const yst_stair* s, int* first) {
    int n = s->n_trials;
    int f;
    if (s->desc.est_trials > 0) {
        f = n - s->desc.est_trials;
        if (f < 0) f = 0;
    } else {
        int last = s->desc.n_steps - 1;
        f = 0;
        while (f < n && (int)s->history[f].step_index < last) f++;
    }
    *first = f;
    return n - f;
}

/* k-th smallest of v[0..n), by rank counting. No buffer and no sort, which
 * keeps the header's "nothing allocates, ever" true for the median too; n is
 * at most YST_MAX_REVERSALS. */
static double yst__kth(const double* v, int n, int k) {
    int i, j;
    for (i = 0; i < n; i++) {
        int less = 0, equal = 0;
        for (j = 0; j < n; j++) {
            if (v[j] < v[i]) less++;
            else if (v[j] <= v[i]) equal++;
        }
        if (k >= less && k < less + equal) return v[i];
    }
    return v[0];
}

YST_API double yst_estimate(const yst_stair* s, yst_estimator how) {
    int first = 0, n = 0, i;
    double sum;

    if (!s || !s->open) return YST__NAN;

    switch (how) {
    case YST_EST_LAST:
        return yst__from_step(s, s->level);

    case YST_EST_TRIALS:
        n = yst__trial_window(s, &first);
        if (n <= 0) return YST__NAN;
        sum = 0.0;
        for (i = first; i < first + n; i++)
            sum += yst__to_step(s, s->history[i].shown);
        return yst__from_step(s, sum / (double)n);

    case YST_EST_REVERSALS:
        n = yst__rev_window(s, &first);
        if (n <= 0) return YST__NAN;
        sum = 0.0;
        for (i = first; i < first + n; i++)
            sum += yst__to_step(s, s->reversals[i]);
        return yst__from_step(s, sum / (double)n);

    case YST_EST_MEDIAN_REV:
        n = yst__rev_window(s, &first);
        if (n <= 0) return YST__NAN;
        if (n % 2 == 1) {
            sum = yst__to_step(s, yst__kth(s->reversals + first, n, n / 2));
        } else {
            sum = 0.5 * (yst__to_step(s, yst__kth(s->reversals + first, n, n / 2 - 1)) +
                         yst__to_step(s, yst__kth(s->reversals + first, n, n / 2)));
        }
        return yst__from_step(s, sum);

    default:
        return YST__NAN;
    }
}

YST_API int yst_estimate_count(const yst_stair* s, yst_estimator how) {
    int first = 0;
    if (!s || !s->open) return 0;
    switch (how) {
    case YST_EST_REVERSALS:
    case YST_EST_MEDIAN_REV: return yst__rev_window(s, &first);
    case YST_EST_TRIALS:     return yst__trial_window(s, &first);
    case YST_EST_LAST:       return 1;
    default:                   return 0;
    }
}

YST_API double yst_weighted_scale(double target_p) {
    if (!yst__finite(target_p) || target_p <= 0.0 || target_p >= 1.0)
        return YST__NAN;
    return (1.0 - target_p) / target_p;
}

YST_API double yst_convergence_p(int n_up, int n_down, double step_down_scale) {
    double s = step_down_scale;
    if (n_up <= 0) n_up = 1;
    if (n_down <= 0) n_down = 1;
    if (!yst__finite(s) || s < 0.0) return YST__NAN;
    if (s == 0.0) s = 1.0;
    /* Levitt's equilibrium: the weighted probability of a harder step equals
     * that of an easier one. Only the two single-sided families close. */
    if (n_up == 1) return pow(1.0 / (1.0 + s), 1.0 / (double)n_down);
    if (n_down == 1) return 1.0 - pow(s / (1.0 + s), 1.0 / (double)n_up);
    return YST__NAN;
}

/* --- history ----------------------------------------------------------- */

YST_API int yst_n_trials(const yst_stair* s) {
    return (s && s->open) ? s->n_trials : 0;
}

YST_API int yst_n_reversals(const yst_stair* s) {
    return (s && s->open) ? s->n_reversals : 0;
}

YST_API const yst_trial* yst_history(const yst_stair* s, int* n) {
    if (!s || !s->open) {
        if (n) *n = 0;
        return NULL;
    }
    if (n) *n = s->n_trials;
    return s->history;
}

YST_API double yst_reversal_level(const yst_stair* s, int i) {
    if (!s || !s->open || i < 0 || i >= s->n_reversals || i >= YST_MAX_REVERSALS)
        return YST__NAN;
    return s->reversals[i];
}

YST_API int yst_reversal_trial(const yst_stair* s, int i) {
    if (!s || !s->open || i < 0 || i >= s->n_reversals || i >= YST_MAX_REVERSALS)
        return -1;
    return s->reversal_trial[i];
}

/* --- simulation -------------------------------------------------------- */

YST_API int yst_simulate_response(double p_correct, double u) {
    return (u < p_correct) ? 1 : 0;
}

/* --- snapshot ----------------------------------------------------------
 * In the shape of ysp/quest.h's: one writer does three jobs (count, write,
 * compare), which is how yst_open_saved() checks the desc against the
 * snapshot without a second description of the layout that could drift
 * from the first. */

#define YST__SNAP_FORMAT 1u

typedef struct yst__w {
    unsigned char*       out;
    const unsigned char* cmp;
    size_t               pos;
    size_t               cap;
    const char*          diff;   /* compare: the first field that differs */
} yst__w;

static void yst__put(yst__w* w, uint64_t v, int nbytes, const char* name) {
    int i;
    unsigned char b;
    for (i = 0; i < nbytes; i++) {
        b = (unsigned char)((v >> (8 * i)) & 0xffu);
        if (w->out) {
            if (w->pos < w->cap) w->out[w->pos] = b;
        } else if (w->cmp && !w->diff) {
            if (w->pos >= w->cap || w->cmp[w->pos] != b) w->diff = name;
        }
        w->pos++;
    }
}

static void yst__put_i32(yst__w* w, int v, const char* name) {
    yst__put(w, (uint64_t)(uint32_t)v, 4, name);
}

static void yst__put_f64(yst__w* w, double v, const char* name) {
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    yst__put(w, u, 8, name);
}

/* The desc as open() resolved it, so two descs that open the same
 * staircase compare equal. */
static void yst__put_desc(yst__w* w, const yst_desc* d) {
    int i;
    yst__put_f64(w, d->start, "start");
    yst__put_i32(w, (int)d->rule, "rule");
    yst__put_i32(w, d->n_up, "n_up");
    yst__put_i32(w, d->n_down, "n_down");
    yst__put_i32(w, (int)d->step_type, "step_type");
    yst__put_i32(w, d->n_steps, "n_steps");
    for (i = 0; i < d->n_steps; i++) yst__put_f64(w, d->steps[i], "steps");
    yst__put_f64(w, d->step_down_scale, "step_down_scale");
    yst__put_f64(w, d->target_p, "target_p");
    yst__put_f64(w, d->min, "min");
    yst__put_f64(w, d->max, "max");
    yst__put(w, d->use_limits ? 1u : 0u, 1, "use_limits");
    yst__put(w, d->harder_is_up ? 1u : 0u, 1, "harder_is_up");
    yst__put(w, d->initial_rule ? 1u : 0u, 1, "initial_rule");
    yst__put_i32(w, d->stop_reversals, "stop_reversals");
    yst__put_i32(w, d->stop_trials, "stop_trials");
    yst__put_i32(w, d->stop_at_limit, "stop_at_limit");
    yst__put_i32(w, d->est_reversals, "est_reversals");
    yst__put_i32(w, d->est_trials, "est_trials");
}

static int yst__n_recorded(const yst_stair* s) {
    return s->n_reversals < YST_MAX_REVERSALS ? s->n_reversals : YST_MAX_REVERSALS;
}

static void yst__put_all(yst__w* w, const yst_stair* s) {
    int i, rec = yst__n_recorded(s);
    yst__put(w, 'Y', 1, "magic");
    yst__put(w, 'S', 1, "magic");
    yst__put(w, 'T', 1, "magic");
    yst__put(w, 'R', 1, "magic");
    yst__put(w, YST__SNAP_FORMAT, 4, "format");
    yst__put_desc(w, &s->desc);
    yst__put_i32(w, s->n_trials, "n_trials");
    yst__put_i32(w, s->n_reversals, "n_reversals");
    yst__put_i32(w, s->run_correct, "run_correct");
    yst__put_i32(w, s->run_incorrect, "run_incorrect");
    yst__put_i32(w, s->direction, "direction");
    yst__put_i32(w, s->step_index, "step_index");
    yst__put_i32(w, s->at_limit_run, "at_limit_run");
    yst__put_i32(w, (int)s->stop, "stop");
    yst__put_f64(w, s->level, "level");
    for (i = 0; i < s->n_trials; i++) {
        const yst_trial* t = &s->history[i];
        yst__put_f64(w, t->proposed, "history.proposed");
        yst__put_f64(w, t->shown, "history.shown");
        yst__put(w, t->response, 1, "history.response");
        yst__put(w, t->reversal, 1, "history.reversal");
        yst__put(w, (uint64_t)(uint8_t)t->direction, 1, "history.direction");
        yst__put(w, t->step_index, 1, "history.step_index");
    }
    yst__put_i32(w, rec, "reversals.n");
    for (i = 0; i < rec; i++) {
        yst__put_f64(w, s->reversals[i], "reversals.level");
        yst__put_i32(w, s->reversal_trial[i], "reversals.trial");
    }
}

YST_API size_t yst_save_size(const yst_stair* s) {
    yst__w w;
    if (!s || !s->open) return 0;
    memset(&w, 0, sizeof(w));
    yst__put_all(&w, s);
    return w.pos;
}

YST_API int yst_save(const yst_stair* s, void* buf, size_t cap) {
    yst__w w;
    size_t need;
    if (!s) return YST_ERR_ARG;
    if (!s->open) return YST_ERR_CLOSED;
    need = yst_save_size(s);
    if (!buf || cap < need || need > 0x7fffffffu) return YST_ERR_ARG;
    memset(&w, 0, sizeof(w));
    w.out = (unsigned char*)buf;
    w.cap = cap;
    yst__put_all(&w, s);
    return (int)w.pos;
}

typedef struct yst__r {
    const unsigned char* in;
    size_t               pos;
    size_t               len;
    bool                 bad;
} yst__r;

static uint64_t yst__get(yst__r* r, int nbytes) {
    uint64_t v = 0;
    int i;
    if (r->bad || r->len - r->pos < (size_t)nbytes) {
        r->bad = true;
        return 0;
    }
    for (i = 0; i < nbytes; i++) v |= (uint64_t)r->in[r->pos + (size_t)i] << (8 * i);
    r->pos += (size_t)nbytes;
    return v;
}

static int yst__get_i32(yst__r* r) { return (int)(int32_t)(uint32_t)yst__get(r, 4); }

static double yst__get_f64(yst__r* r) {
    uint64_t u = yst__get(r, 8);
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

/* Leave the handle closed and say why: a load either resumes the session or
 * leaves nothing half-restored behind. */
static bool yst__load_fail(yst_stair* s, const char* why) {
    char msg[sizeof(s->error)];
    snprintf(msg, sizeof(msg), "ysp_stair: yst_open_saved: %s", why);
    yst_close(s);
    memcpy(s->error, msg, sizeof(msg));
    return false;
}

YST_API bool yst_open_saved(yst_stair* s, const yst_desc* desc, const void* buf, size_t len) {
    const unsigned char* in = (const unsigned char*)buf;
    yst__w w;
    yst__r r;
    int i, v, n, rec;
    bool logs;

    if (!s) return false;
    if (!yst_open(s, desc)) return false;
    if (!in) return yst__load_fail(s, "null snapshot");
    if (len < 8 || in[0] != 'Y' || in[1] != 'S' || in[2] != 'T' || in[3] != 'R')
        return yst__load_fail(s, "not a ysp_stair snapshot");
    memset(&r, 0, sizeof(r));
    r.in = in;
    r.len = len;
    r.pos = 4;
    if ((uint32_t)yst__get(&r, 4) != YST__SNAP_FORMAT)
        return yst__load_fail(s, "the snapshot's format is not this header's");

    memset(&w, 0, sizeof(w));
    w.cmp = in;
    w.cap = len;
    w.pos = r.pos;
    yst__put_desc(&w, &s->desc);
    if (w.diff) {
        char why[96];
        snprintf(why, sizeof(why), "desc.%s does not match the snapshot", w.diff);
        return yst__load_fail(s, why);
    }
    r.pos = w.pos;

    n                = yst__get_i32(&r);
    s->n_reversals   = yst__get_i32(&r);
    s->run_correct   = yst__get_i32(&r);
    s->run_incorrect = yst__get_i32(&r);
    s->direction     = yst__get_i32(&r);
    s->step_index    = yst__get_i32(&r);
    s->at_limit_run  = yst__get_i32(&r);
    v                = yst__get_i32(&r);
    s->level         = yst__get_f64(&r);
    if (!r.bad && n > YST_MAX_TRIALS)
        return yst__load_fail(s, "the snapshot has more trials than this build's YST_MAX_TRIALS");
    /* A reversal is at most one a trial, and a run or a pinned stretch is
     * at most the whole history. */
    if (r.bad || n < 0 || s->n_reversals < 0 || s->n_reversals > n ||
        s->run_correct < 0 || s->run_correct > n ||
        s->run_incorrect < 0 || s->run_incorrect > n ||
        s->direction < -1 || s->direction > 1 ||
        s->step_index < 0 || s->step_index >= s->desc.n_steps ||
        s->at_limit_run < 0 || s->at_limit_run > n ||
        v < (int)YST_STOP_NONE || v > (int)YST_STOP_FULL || !yst__finite(s->level))
        return yst__load_fail(s, "the snapshot's counters are corrupt or truncated");
    s->n_trials = n;
    s->stop = (yst_stop)v;

    /* update() refuses a shown level <= 0 under LOG and DB, so a history
     * that holds one was not written by update(). */
    logs = s->desc.step_type != YST_STEP_LIN;
    for (i = 0; i < n && !r.bad; i++) {
        yst_trial* t = &s->history[i];
        int resp, rev, dir, si;
        t->proposed = yst__get_f64(&r);
        t->shown    = yst__get_f64(&r);
        resp = (int)yst__get(&r, 1);
        rev  = (int)yst__get(&r, 1);
        dir  = (int)(int8_t)(uint8_t)yst__get(&r, 1);
        si   = (int)yst__get(&r, 1);
        if (!yst__finite(t->proposed) || !yst__finite(t->shown) ||
            (logs && !(t->shown > 0.0)) || resp > 1 || rev > 1 ||
            dir < -1 || dir > 1 || si >= s->desc.n_steps)
            r.bad = true;
        t->response   = (uint8_t)resp;
        t->reversal   = (uint8_t)rev;
        t->direction  = (int8_t)dir;
        t->step_index = (uint8_t)si;
    }
    if (r.bad) return yst__load_fail(s, "the snapshot's history is corrupt or truncated");

    rec = yst__get_i32(&r);
    if (!r.bad && rec != yst__n_recorded(s))
        return yst__load_fail(s, "the snapshot records another number of reversals "
                                 "(a build with another YST_MAX_REVERSALS, or corrupt)");
    for (i = 0; i < rec && !r.bad; i++) {
        s->reversals[i] = yst__get_f64(&r);
        s->reversal_trial[i] = yst__get_i32(&r);
        if (!yst__finite(s->reversals[i]) || s->reversal_trial[i] < 0 ||
            s->reversal_trial[i] >= n)
            r.bad = true;
    }
    if (r.bad) return yst__load_fail(s, "the snapshot's reversals are corrupt or truncated");
    if (r.pos != r.len) return yst__load_fail(s, "bytes after the snapshot's end");
    s->error[0] = '\0';
    return true;
}

/* --- parameters -------------------------------------------------------- */

/* The ranges are what yst_open() accepts. */
static const yst_param yst__params[] = {
    { "start", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "first proposed level, in the caller's units; > 0 under a LOG or DB step", NULL },
    { "rule", "enum", 0, 1, 0, "",
      "transformed or weighted up/down, or accelerated stochastic approximation",
      "updown=0,asa=1" },
    { "n_up", "i32", 0, INT_MAX, 1, "",
      "consecutive incorrect responses per easier step", NULL },
    { "n_down", "i32", 0, INT_MAX, 1, "",
      "consecutive correct responses per harder step", NULL },
    { "step_type", "enum", 0, 2, 0, "",
      "a step adds to the level, to its log10, or to 20 log10 (dB)",
      "lin=0,log=1,db=2" },
    { "steps", "f64[]", 0, DBL_MAX, 0, "",
      "required: the step schedule, n_steps entries, each finite and > 0; entry k "
      "holds from reversal k on, in the step's units", NULL },
    { "n_steps", "i32", 1, YST_MAX_STEPS, 0, "",
      "required: the entries in steps; 1 = a fixed step", NULL },
    { "step_down_scale", "f64", 0, DBL_MAX, 1, "",
      "the harder step is the step times this; not 1 makes a weighted staircase", NULL },
    { "target_p", "f64", 0, 1, 0, "",
      "required under the ASA rule: the proportion correct to converge on, in (0, 1)", NULL },
    { "min", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "lower clamp on the proposal; min and max both 0 = no limits", NULL },
    { "max", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "upper clamp on the proposal", NULL },
    { "use_limits", "bool", 0, 1, 0, "",
      "clamp to min and max even when both are 0", NULL },
    { "harder_is_up", "bool", 0, 1, 0, "",
      "a correct response raises the level", NULL },
    { "initial_rule", "bool", 0, 1, 0, "",
      "1-up-1-down until the first reversal", NULL },
    { "stop_reversals", "i32", 0, YST_MAX_REVERSALS, 0, "",
      "stop after this many reversals; 0 = off (set this or stop_trials)", NULL },
    { "stop_trials", "i32", 0, YST_MAX_TRIALS, 0, "",
      "stop after this many trials; 0 = off (set this or stop_reversals)", NULL },
    { "stop_at_limit", "i32", 0, INT_MAX, 0, "",
      "stop after this many consecutive proposals at min or max; 0 = never", NULL },
    { "est_reversals", "i32", 0, INT_MAX, 0, "",
      "reversals the reversal estimates average; 0 = all after the schedule ran out", NULL },
    { "est_trials", "i32", 0, INT_MAX, 0, "",
      "trials the trial estimate averages; 0 = all after the schedule ran out", NULL },
};

YST_API const yst_param* yst_params(int* n) {
    if (n) *n = (int)(sizeof yst__params / sizeof yst__params[0]);
    return yst__params;
}

#undef YST__FAIL

#endif /* YSP_STAIR_IMPLEMENTATION_GUARD */
#endif /* YSP_STAIR_IMPLEMENTATION */

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
