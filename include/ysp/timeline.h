/* ysp/timeline.h - v0.6.0 - public domain single-header timeline library
 *
 *   What a frame shows, as a function of when the frame appears. Events
 *   (onsets, offsets, value changes, triggers, annotations) at times on a
 *   chosen time base, keyframe tracks with easing on float channels
 *   (position, contrast, opacity, spatial frequency), and one call per frame
 *   that takes the PREDICTED ONSET of the frame being drawn, fills the
 *   channel values for it and reports which events landed on it, with the
 *   frame and the residual. It never reads a clock. Tweens can also write
 *   the caller's own fields (a stimulus's x, contrast or color), as GSAP's
 *   gsap.to(target, vars) does (TARGETS).
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no OS calls, no threads, no heap, no I/O. Needs nothing
 *   but the C standard library (<math.h> for the cosine and log easings,
 *   snprintf for the messages). It does not include ysp/rt.h: a time is an
 *   int64_t count of nanoseconds, the type and unit yrt_now_ns() returns,
 *   so the caller passes ysp/rt.h times in as they are.
 *
 *   Targets every platform the compiler does. C99 is the floor: it builds as
 *   C99, C11 and C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_TIMELINE_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *   One trial: a fixation point from 0 to 0.5 s, then a grating whose
 *   contrast ramps up over 100 ms with a raised cosine, holds, and ramps
 *   down to end at 1.2 s, and a trigger at the grating's onset
 *   (examples/timeline/trial.c runs this against a simulated 60 Hz
 *   display):
 *
 *       #define YSP_TIMELINE_IMPLEMENTATION
 *       #include "ysp/timeline.h"
 *
 *       enum { FIX_ON, GRATING_ON, CONTRAST, N_CHANNELS };   // the caller's map
 *       enum { TRIAL = 1 };                                  // a base number
 *
 *       static ytl_event storage[64];
 *       static ytl_key arena[64];                          // for sequences
 *       static ytl_timeline tl;                            // 64 KB
 *       ytl_desc d = { 0 };
 *       d.events = storage; d.event_capacity = 64; d.n_channels = N_CHANNELS;
 *       d.keys = arena; d.key_capacity = 64;
 *       if (!ytl_open(&tl, &d)) { fputs(ytl_error(&tl), stderr); return 1; }
 *
 *       ytl_event e = { 0 };                   // a MARK at 0 on the RT base
 *       e.base = TRIAL;
 *       e.kind = YTL_ONSET;   e.target = FIX_ON;
 *       ytl_add(&tl, &e);
 *       e.kind = YTL_OFFSET;  e.time_ns = YRT_MS(500);
 *       ytl_add(&tl, &e);
 *       e.kind = YTL_ONSET;   e.target = GRATING_ON;
 *       ytl_add(&tl, &e);
 *       e.kind = YTL_TRIGGER; e.code = 12;     // a TRIGGER's target is the caller's
 *       ytl_add(&tl, &e);
 *       e.kind = YTL_OFFSET;  e.code = 0;      e.time_ns = YRT_MS(1200);
 *       ytl_add(&tl, &e);
 *       static const ytl_key ramp[] = {  // time_ns, value, ease, 0, 0; outlives use
 *           { YRT_MS(500),   0.0f, YTL_EASE_COSINE, 0, 0 },
 *           { YRT_MS(600),   0.5f, YTL_EASE_LINEAR, 0, 0 },     // a hold
 *           { YRT_MS(1100),  0.5f, YTL_EASE_COSINE, 0, 0 },
 *           { YRT_MS(1200),  0.0f, YTL_EASE_LINEAR, 0, 0 },
 *       };
 *       if (ytl_set_keys(&tl, CONTRAST, TRIAL, ramp, 4) < 0) fputs(ytl_error(&tl), stderr);
 *
 *   The same trial as a sequence (SEQUENCES), which reads as the GSAP
 *   timeline gsap.timeline().set(fix, {visible: 1}).set(fix, {visible: 0},
 *   "+=0.5").set(grating, {visible: 1}, "<").to(grating, {contrast: 0.5,
 *   duration: 0.1, ease: "sine.inOut"}, "<").to(grating, {contrast: 0,
 *   duration: 0.1, ease: "sine.inOut"}, "+=0.5").set(grating, {visible: 0})
 *   does; its tweens use the key arena, desc.keys and desc.key_capacity:
 *
 *       ytl_tween_desc up = { 0 };
 *       up.to = 0.5f; up.duration_ns = YRT_MS(100); up.ease = YTL_EASE_COSINE;
 *       ytl_tween_desc down = up;
 *       down.to = 0.0f;
 *
 *       ytl_seq q = ytl_seq_on(&tl, TRIAL);           // cursor at 0
 *       ytl_on(&q, FIX_ON);
 *       ytl_wait(&q, YRT_MS(500));
 *       ytl_off(&q, FIX_ON);
 *       ytl_on(&q, GRATING_ON);
 *       ytl_trigger(&q, 12);
 *       ytl_to(&q, CONTRAST, &up);                     // the cursor stays
 *       ytl_wait(&q, YRT_MS(600));
 *       ytl_then(&q, CONTRAST, &down);                 // and waits for its end
 *       ytl_off(&q, GRATING_ON);
 *       if (q.err < 0) fputs(ytl_error(&tl), stderr);  // names call q.err_call
 *
 *       // Trial time 0 is the first frame of the trial.
 *       ytl_anchor(&tl, TRIAL, next_onset_ns, 0);
 *       for (;;) {
 *           ytl_frame f = { next_onset_ns, period_ns, frame_index };
 *           ytl_event fired[8];
 *           int n = ytl_evaluate(&tl, &f, fired, 8);
 *           const float* v = ytl_values(&tl);
 *           draw(v[FIX_ON], v[GRATING_ON], v[CONTRAST]);
 *           for (int i = 0; i < n && i < 8; i++)
 *               if (fired[i].kind == YTL_TRIGGER) arm_trigger(fired[i].code);
 *           flip_at(next_onset_ns);   // and log fired[] with the flip record
 *           ...
 *       }
 *       ytl_close(&tl);
 *
 *   A 10-minute sum-of-sines tracking target on the x channel, from a
 *   table of samples at 120 per second, and a 7.5 Hz square-wave flicker
 *   on the luminance channel for the same 10 minutes
 *   (examples/timeline/tracking.c runs both at 500 Hz):
 *
 *       static float xs[600 * 120 + 1];        // from the pack, or computed
 *       ytl_track path = { 0 };
 *       path.samples = xs; path.n_samples = 600 * 120 + 1;
 *       path.rate = 120; path.interp = YTL_INTERP_CUBIC;
 *       ytl_set_track(&tl, TARGET_X, TRIAL, &path);
 *
 *       static const ytl_key square[] = {     // on for half a cycle
 *           { 0,                  1.0f, YTL_EASE_STEP, 0, 0 },
 *           { YRT_S(1) * 2 / 30,  0.0f, YTL_EASE_STEP, 0, 0 },
 *       };
 *       ytl_track flicker = { 0 };
 *       flicker.keys = square; flicker.n_keys = 2;
 *       flicker.period_ns = YRT_S(1) * 4 / 30;   // 133.33 ms, 7.5 Hz
 *       flicker.repeats = 4500;                   // 10 minutes, then off
 *       ytl_set_track(&tl, LUMINANCE, TRIAL, &flicker);
 *
 *   A script's fade, from wherever contrast is now to 0 over 200 ms, and
 *   a moving dot sent somewhere else in mid-flight with no velocity step
 *   (GSAP: gsap.to(g, {contrast: 0, duration: 0.2, ease: "sine.inOut"})):
 *
 *       ytl_tween_desc fade = { 0 };
 *       fade.to = 0.0f; fade.duration_ns = YRT_MS(200); fade.ease = YTL_EASE_COSINE;
 *       ytl_tween(&tl, CONTRAST, TRIAL, &fade);
 *       ytl_tween_desc move = { 0 };
 *       move.to = 4.0f; move.duration_ns = YRT_MS(300); move.keep_velocity = true;
 *       ytl_tween(&tl, DOT_X, TRIAL, &move);
 *
 *   The trial's stimuli animated in place, with no channel numbers and no
 *   copy per frame (TARGETS): GSAP's gsap.to(dot, {x: 300, duration: 0.5,
 *   ease: "power2.out"}), a fade-in with gsap.from(), and a set at 0.5 s.
 *   The pools are the caller's (desc.targets, desc.tweens):
 *
 *       static ytl_target_slot targets[64];
 *       static ytl_tween_slot tweens[128];
 *       d.targets = targets; d.target_capacity = 64;   // before ytl_open()
 *       d.tweens = tweens;   d.tween_capacity = 128;
 *       ...
 *       ytl_seq s = ytl_seq_on(&tl, TRIAL);
 *       ytl_tween_to(&s, &dot.x, 300.0f, YRT_MS(500), YTL_EASE_CUBIC_OUT);
 *       ytl_tween_from(&s, &dot.contrast, 0.0f, YRT_MS(200), YTL_EASE_COSINE);
 *       ytl_at(&s, YRT_MS(500));
 *       ytl_tween_set(&s, &fix.visible, 0.0f);
 *       if (s.err < 0) fputs(ytl_error(&tl), stderr);
 *       // each ytl_evaluate() then writes dot.x, dot.contrast, fix.visible
 *
 *   These blocks build as C99 and as C++17. The op-table macros
 *   (SEQUENCES) are designated initializers: C99, or C++20.
 *
 *   The next trial clears the trial base and adds its own events, or keeps
 *   them and anchors the base again at its first frame: anchoring moves
 *   the base's time back to 0, which un-fires everything, so the same
 *   events run again (REWIND).
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   TIME
 *     Every time is an int64_t count of nanoseconds. The RT clock is the
 *     caller's monotonic clock, yrt_now_ns() when ysp/rt.h is in use; a
 *     frame's onset is on it. The header compares and subtracts times and
 *     never reads a clock. Times stay below 2^62 ns (146 years) in
 *     magnitude so that a sum of two cannot overflow. A time field the
 *     caller writes ends in _ns (event.time_ns, key.time_ns,
 *     track.period_ns, tween.duration_ns), so code that wrote seconds
 *     there fails to compile. Write a literal with YRT_S(), YRT_MS() or
 *     YRT_US(), which this header defines as ysp/rt.h does. ytl_ns(seconds)
 *     rounds a run-time double to the nearest nanosecond and clamps it,
 *     for tables in seconds.
 *
 *   TIME BASES (ytl_anchor, ytl_pause, ytl_resume, ytl_stop)
 *     An event or a track is on one of YTL_MAX_BASES (8) bases. Base 0,
 *     YTL_BASE_RT, is the RT clock itself and always runs (anchor, pause,
 *     resume and stop on it are YTL_ERR_ARG). Bases 1 to 7
 *     are the caller's: a trial clock, a movie clock, a block clock. Each
 *     is in one of three states:
 *       stopped   the state after open(). Nothing on it fires, rewinds or
 *                 writes a channel; its channels hold their values.
 *       running   ytl_anchor(tl, b, rt, bt): base time bt at RT time rt,
 *                 advancing at the base's rate num/den (RATE; 1/1 unless
 *                 ytl_rate() set another), so base time at RT time t is
 *                 bt + floor((t - rt) x num / den), bt + (t - rt) at rate
 *                 1. rt may be in the future.
 *       paused    ytl_pause(tl, b, rt): the base time at rt, frozen.
 *                 ytl_resume(tl, b, rt) runs it again from that time at
 *                 RT time rt. A paused base evaluates at its frozen time,
 *                 so its tracks hold and an event added at or before that
 *                 time still fires. A pause un-fires nothing, even when
 *                 the frozen time is before an event that a lead window
 *                 already fired (QUANTIZATION).
 *     ytl_stop(tl, b) stops a base. Anchoring a base again, from any
 *     state, is how a trial clock restarts and how a movie seeks: forward,
 *     the next evaluate fires the events the base passed over, late by
 *     their residuals; back, the events from the new time on are pending
 *     again (REWIND). ytl_skip() is the seek that does not fire what it
 *     passes over (SKIP). ytl_rate() sets the rate (RATE); anchor, skip,
 *     pause, resume, stop and clear keep it.
 *
 *   EVENTS (ytl_event, ytl_add, ytl_add_n)
 *     An event has a time on its base, a kind, and the caller's payload:
 *       YTL_MARK     reported only; an annotation, a coroutine to resume
 *       YTL_TRIGGER  reported only; `code` is the value for the port
 *       YTL_ONSET    channel `target` becomes 1 when it fires
 *       YTL_OFFSET   channel `target` becomes 0
 *       YTL_SET      channel `target` becomes `value`
 *       YTL_CALL_START, YTL_CALL_COMPLETE (5, 6)  reported; the header adds
 *                    them for a target tween's callbacks (TARGETS)
 *       YTL_KIND_USER (16) to 255: reported only, the caller's meaning
 *     A zeroed event is a MARK at time 0 on the RT base. `code`, `user` and
 *     (for the reported-only kinds) `target` are the caller's and come back
 *     unchanged in the report. Kinds 5 to 15 are the header's, and an add
 *     rejects them.
 *     ytl_add() copies the event into the storage the desc gave, pending
 *     (flags 0, frame -1, onset 0, residual 0), assigns it an id (0, 1, 2, ... in add order; ytl_open() restarts it) and
 *     returns the id. ytl_add_n() adds n events, all or none, and returns
 *     the first id; the others follow in order. The storage is kept sorted
 *     by base, then time, then id, so events at one time keep their add
 *     order. An add costs a binary search and a move of the events after
 *     it in the storage: appending sorted events to the last base in use
 *     moves nothing, so a table of thousands loads in linear time.
 *
 *   CHANNELS (ytl_values, desc.n_channels, desc.initial)
 *     The state of a frame is desc.n_channels floats (at most
 *     YTL_MAX_CHANNELS, 256 by default), starting at desc.initial[] (all
 *     0 when NULL). The caller decides what each one means. A channel is
 *     driven either by events (ONSET, OFFSET, SET naming it) or by a track
 *     (ytl_set_track, ytl_set_keys, ytl_tween), never both, and from
 *     one base only. The first event that names a free channel binds it to
 *     the event's base; an event on another base, or a track, is then
 *     YTL_ERR_BOUND until the channel's last event is gone. A track binds
 *     the channel to its base and replaces the channel's track whatever
 *     base it was on; an event naming it is YTL_ERR_BOUND until the
 *     track is removed (set_track with NULL, set_keys with n = 0).
 *     ytl_clear() of a base frees the channels bound to it. A channel a
 *     prune left with no event is free but keeps its value, and a clear
 *     does not touch it.
 *     An event-driven channel's value is the value of its fired or skipped
 *     (SKIP) event with the latest (time, id), or its initial value when
 *     there is none. So an event that fires late (LATE EVENTS) does not
 *     overwrite a later event that fired before it, a skip leaves the
 *     channel as playing through would, and a rewind recomputes the
 *     channel from the events still fired or skipped.
 *
 *   TRACKS (ytl_track, ytl_set_track, ytl_set_keys, ytl_tween,
 *           ytl_sample)
 *     A track drives a channel from a function of its base's time. It has
 *     one of two forms, and an optional repeat.
 *     KEYED: n_keys keys (ytl_key) sorted by time, equal times allowed
 *     (a jump). For movement and envelopes with few changes: a ramp, a
 *     fade, a path through a dozen points. The value at time t: the first
 *     key's value before it, the last key's value from it on, and between
 *     key i and key i+1 (t_i <= t < t_i+1, at equal times the later key)
 *     v_i + (v_i+1 - v_i) e(u) with key i's easing e at
 *     u = (t - t_i) / (t_i+1 - t_i):
 *       YTL_EASE_LINEAR  (0)   e = u
 *       YTL_EASE_STEP          e = 0: v_i until t_i+1
 *       YTL_EASE_COSINE        e = (1 - cos(pi u)) / 2, a raised cosine
 *       YTL_EASE_QUAD_IN       e = u^2, starts slow
 *       YTL_EASE_QUAD_OUT      e = 1 - (1 - u)^2, ends slow
 *       YTL_EASE_QUAD_IN_OUT   e = 2u^2 below u = 1/2, 1 - 2(1 - u)^2 from it
 *       YTL_EASE_CUBIC_IN      e = u^3                 (GSAP power2.in)
 *       YTL_EASE_CUBIC_OUT     e = 1 - (1 - u)^3       (GSAP power2.out)
 *       YTL_EASE_CUBIC_IN_OUT  e = 4u^3 below u = 1/2, 1 - 4(1 - u)^3 from it
 *       YTL_EASE_BEZIER        e = y where x = u on the cubic Bezier
 *                                curve from (0, 0) to (1, 1) with control
 *                                points (x1, y1) and (x2, y2), CSS's
 *                                cubic-bezier(), from track.curves[key.curve]
 *       YTL_EASE_LOG           not of that form: v_i (v_i+1 / v_i)^u, a
 *                                geometric sweep (spatial frequency, tone
 *                                frequency); both values must be > 0
 *     A curve's x1 and x2 are in [0, 1], so x rises with the curve
 *     parameter and u has one y. y1 and y2 may leave [0, 1], and then the
 *     value overshoots its keys; for a stimulus that is usually a confound.
 *     The y is found to 1e-12 in x. Every key's ease must be a
 *     ytl_ease, the last key's too, though the last key's is never used
 *     and the LOG and BEZIER checks skip it.
 *     SAMPLED: n_samples floats at track.rate samples per second (an
 *     integer, 1 to 1e9), sample i at base time t0_ns + i * 1e9 / rate,
 *     exactly, not rounded to a whole ns. For a trajectory that few keys
 *     cannot describe: a sum-of-sines tracking target, a recorded path, a
 *     noise sequence. A sample costs 4 bytes against a key's 16, there is
 *     no time column to search, and the lookup is integer arithmetic, so a
 *     table does not drift over hours. A rate that is not an integer
 *     (59.94) is a resampling job for the pack tool. A step in whole ns
 *     would drift: at 120 samples/s it is a third of a ns short per
 *     sample, and docs/timeline.md measures what that costs on a
 *     moving target. Before t0_ns the value is the first sample, from the last
 *     sample's time on the last sample, and between samples i and i+1
 *     by track.interp:
 *       YTL_INTERP_LINEAR (0)  v_i + (v_i+1 - v_i) u
 *       YTL_INTERP_STEP        v_i
 *       YTL_INTERP_CUBIC       Catmull-Rom through v_i-1 .. v_i+2, with
 *                                v_-1 = 2 v_0 - v_1 and v_n = 2 v_n-1 -
 *                                v_n-2 past the ends; passes through every
 *                                sample with a continuous velocity, and
 *                                with two samples is LINEAR
 *     with u the fraction of the way from sample i's time to sample
 *     i+1's. How fine a table needs to be is
 *     measured in docs/timeline.md: for a sum of sines up to 5 Hz,
 *     CUBIC at 120 samples/s stays within 7.4e-6 of the peak, 1.7e-5 in
 *     the first and last segment (0.29 MB for 10 minutes), and reaches
 *     float rounding at 500 samples/s in the interior; LINEAR
 *     needs 1000 samples/s for 5e-6, and its velocity steps at every
 *     sample. The pack tool computes the samples once, so the runtime does
 *     no trigonometry and the trajectory is the same bits everywhere.
 *     REPEAT: track.period_ns > 0 repeats the track every period from its
 *     start (the first key's time, or t0_ns): base time t reads the track at
 *     start + ((t - start) mod period). track.repeats cycles (0: forever),
 *     after which the track holds its value at start + period, read
 *     without repeating. Before the start, no repeat applies. A square-wave
 *     flicker is two STEP keys and a period; a key past start + period is
 *     never reached while the track repeats.
 *     ytl_set_track(tl, channel, base, &tr) copies the struct; the
 *     arrays it points at (keys, curves, samples) stay the caller's and
 *     must stay valid and unchanged until the channel's track is
 *     replaced, the base is cleared, or the handle is no longer used. A
 *     pack's table works as is. tr NULL removes the track and returns the
 *     channel to its initial value. ytl_set_keys(tl, channel, base,
 *     keys, n) is set_track with only keys and n (n = 0 removes).
 *     set_track rejects with YTL_ERR_ARG: neither form or both; a
 *     negative count; unsorted key times; a key ease that is not a
 *     ytl_ease; a non-finite key value, sample or curve y; a curve x
 *     outside [0, 1]; a BEZIER key whose curve index is not below
 *     n_curves, or a LOG key with a value <= 0 at either end (both but
 *     the last key); n_curves > 0 with curves NULL, and any curve in the
 *     table that is bad, used or not; keys set with n_keys 0, or samples
 *     with n_samples 0 (neither form); a rate outside 1 to 1e9; a last
 *     sample at or past 2^62 (its time rounded up to a whole ns); an
 *     interp that is not a ytl_interp; a period < 0 or >= 2^62;
 *     repeats < 0, or > 0 with period 0; a time at or past +-2^62. The
 *     check reads every key and sample once. A rejected call leaves the
 *     channel's track as it was.
 *     TWEENS: ytl_tween(tl, channel, base, &d) drives a channel from one
 *     value to another, with its keys in the handle, so the caller keeps no
 *     array alive. Every field of ytl_tween_desc may be left zero:
 *       to             the end value
 *       duration_ns    >= 0; 0 jumps at the start
 *       start_ns       the base time it starts, when start_set; otherwise
 *                      "now": the base time of the base's last evaluated
 *                      onset, unless an anchor, pause, resume or rate
 *                      change of a running base came after it, which set
 *                      "now" to the anchor's base time, the frozen time or
 *                      the base time at the change; 0 on a base never
 *                      evaluated or anchored
 *       delay_ns       added to the start either way, so delay_ns = d is
 *                      "d after now", and start_ns = t, start_set = true,
 *                      delay_ns = d is "d after t"
 *       from           the start value, when from_set; otherwise the value
 *                      the channel has at the start (below)
 *       ease, curve    a ytl_ease (0 = LINEAR), and for YTL_EASE_BEZIER
 *                      the curve, inline in the desc
 *       cycles         0 or 1: play once; n: n cycles; YTL_FOREVER. The
 *                      hold after the last cycle is as REPEAT gives it
 *       yoyo           a cycle goes from `from` to `to` and back, over
 *                      2 x duration, the return leg with the same ease
 *       keep_velocity  start at the velocity the channel has at the start
 *                      and come to rest at `to`: the shape is a cubic
 *                      Hermite segment, with no step in velocity, for a
 *                      moving target retargeted in mid-flight. With no
 *                      track at the start the velocity is 0, a smoothstep
 *     `from` is read only with from_set and `curve` only with BEZIER. The
 *     return leg of a yoyo applies the same ease again, from `to` to
 *     `from`; it is not GSAP's mirrored playback, so QUAD_IN is slow at
 *     the start of both legs.
 *     THE TAKEOVER. The tween waits until its start. Until then the
 *     channel's track runs on, as GSAP lets an overwritten tween run until
 *     the new one starts. At the start, the tween takes over the channel,
 *     and `from` (unless from_set) is the value the old track has at the
 *     start time, not at the frame that happens to see the start. So a
 *     tween posted just before a frame begins where the old motion would
 *     have been on that frame, with no repeated position, and the result
 *     does not depend on frame timing. With keep_velocity the start
 *     velocity is the old track's slope over the last microsecond before
 *     the start. A channel with no track starts from its value; a channel
 *     whose track is on another base stops that track at once, at its last
 *     value, because the two bases' times do not compare. A second tween
 *     before the first has started replaces it. ytl_set_track() on the
 *     channel cancels a waiting tween. A rewind does not undo a takeover:
 *     the takeover replaced the channel's track, which is not an event.
 *     Rejected with YTL_ERR_ARG: d NULL; a negative duration; a start,
 *     a delay, start + delay, or start + delay + duration (x 2 with yoyo)
 *     at or past +-2^62 (cycles do not count: a repeat folds time); a
 *     non-finite to, or from with from_set; an ease that is not a
 *     ytl_ease; a bad curve with BEZIER; LOG without from_set, or with
 *     from or to <= 0; cycles below YTL_FOREVER; more than one cycle, or
 *     yoyo, with duration 0; keep_velocity with an ease other than LINEAR,
 *     from_set, yoyo, more than one cycle (YTL_FOREVER included), or
 *     duration 0. A STEP tween is
 *     a STEP track (below). To await the end, add a MARK at start +
 *     duration (x 2 with yoyo, x cycles) on the same base.
 *     The arithmetic is double, stored as float. ytl_sample(&tr, t) is
 *     the same function without a handle, for a designer's preview, with
 *     no validation. A track writes its channel at every evaluate while
 *     its base runs or is paused, at the base time of the frame's onset:
 *     a ramp shows its value at the moment the frame appears. A STEP
 *     track is the exception. It is a sampled track with
 *     YTL_INTERP_STEP, or a keyed track (a tween included) whose keys
 *     but the last are all YTL_EASE_STEP. It changes value only at its
 *     key times, so it is read at the end of the frame's lead window, base
 *     time at (onset + lead x period), and each change lands on the frame
 *     an event at the same time would (QUANTIZATION). Under
 *     YTL_LEAD_NONE both are read at the onset. The reason is frame-locked flicker: a 7.5 Hz
 *     square wave on a 60 Hz display has its edges exactly on frame
 *     onsets, and an onset rounded to whole ns, or predicted with any
 *     noise, falls on either side of an edge. Read at the onset, a wave
 *     with exact edges showed the wrong state on 3000 of 36000 frames;
 *     read at the end of a half-period window, on none
 *     (docs/timeline.md). A jump inside a track that is not a STEP
 *     track (a STEP key among ramps, keys at equal times, the wrap of a
 *     repeat whose cycle does not end where it starts) is read at the
 *     onset; to quantize it like an event, put it on its own STEP track
 *     or make it an event. Each frame costs one key
 *     lookup (O(1) while time moves forward a key at a time, a binary
 *     search after a jump or a repeat's wrap) or one division, plus the
 *     easing: cos() for COSINE, exp() and log() for LOG, at most 68
 *     cubic evaluations for BEZIER (a few on ordinary curves).
 *
 *   EVALUATE (ytl_frame, ytl_evaluate)
 *     Once per frame, before drawing, with the frame's PREDICTED onset on
 *     the RT clock, the frame period and the caller's frame number:
 *       ytl_frame f = { onset_ns, period_ns, index };
 *       int n = ytl_evaluate(tl, &f, fired, cap);
 *     For each base that is not stopped, in base order: fire every event
 *     the frame's window reaches that has not fired, then write the base's
 *     tracks. An evaluate never un-fires an event; only an anchor does
 *     (REWIND). Returns the number of events fired.
 *     The first min(n, cap) of them, ordered by RT target time and then
 *     by id, are copied into fired[], landing fields
 *     included; the rest fired all the same and their records are in the
 *     storage (ytl_events). fired may be NULL with cap 0. Onsets must
 *     not decrease from one call to the next (YTL_ERR_ORDER); an equal
 *     onset is allowed and fires only what was added since. An evaluate
 *     changes nothing but the handle and the storage, and the same calls
 *     in the same order give the same bytes: that is REPLAY. An event's RT
 *     target is the RT time its base reaches it (ytl_rt_time) when the
 *     base runs, and onset - residual when it is paused; at rate 1 the two
 *     are equal. Across bases at different rates, onset - residual would
 *     order by lateness in base ns instead of by time.
 *
 *   QUANTIZATION (desc.lead, ytl_window)
 *     An event lands on the first evaluated frame whose window reaches its
 *     time: on its base, time <= base time at RT time (onset + lead x
 *     period), with lead x period truncated to whole nanoseconds. The lead
 *     is RT time at every base rate (RATE), so the event lands on the frame
 *     nearest to the RT time its base reaches it. ytl_window(tl, b, &f,
 *     &end) gives that window end without evaluating, for a scheduler that
 *     resumes waits by the same rule, or a header that places its own
 *     frames by it (ysp/video.h). desc.lead:
 *       0                the default, 0.5: the nearest frame, up to half a
 *                        period early. A frame-aligned time lands on its
 *                        frame however the onset was rounded or predicted.
 *       (0, 1)           that fraction of a period early at most.
 *       YTL_LEAD_NONE  never early: the first frame whose onset is at or
 *                        after the event.
 *     The default is half a frame because a predicted onset carries noise.
 *     With 2 us of noise on a 60 Hz display, never-early put about 12% of
 *     frame-aligned changes a frame late, and half a frame put none
 *     (docs/timeline.md). Use YTL_LEAD_NONE only when "not before"
 *     matters more than "on the right frame". A script's wait(dt) lands on
 *     the frame nearest its target under the default.
 *     ytl_lead(tl) gives the lead in use: 0.5 for a desc lead of 0, the
 *     desc's lead in (0, 1), and 0 for YTL_LEAD_NONE, so that a window
 *     ends lead x period after the onset in every case. A header that
 *     places its own frames by the same rule (ysp/video.h) reads it.
 *     Pass period 0 on a display with no fixed period (variable refresh),
 *     and lead does nothing. When it fires, the event records:
 *       frame     f.index of the frame it landed on
 *       onset     f.onset, the predicted onset of that frame
 *       residual  base time at f.onset minus the event's time: how late
 *                 (positive) or early (negative; never under
 *                 YTL_LEAD_NONE) the frame shows it, in base ns. On a
 *                 running base at rate 1 this equals onset minus the
 *                 event's RT time; at rate num/den the RT lateness is about
 *                 residual x den / num.
 *     The onset is a prediction. The flip record of the same frame number
 *     gives the measured one; the difference is the frame's error and
 *     applies to every event that landed on it.
 *     ytl_next_due(tl, &t) gives the earliest RT time of a pending event
 *     (not fired, not skipped) on a running base, for a display that can
 *     flip at a time, or for sleeping until there is something to do. An
 *     event that a slow base reaches only past 2^62 RT ns gives 2^62, a
 *     time no clock reaches.
 *
 *   LATE EVENTS
 *     A base's REACH is the furthest window end any evaluate has reached
 *     on it since it was last rewound (REWIND sets it to just before the
 *     anchor's time). An event added at or before the reach is late: it
 *     fires at the first evaluate whose window reaches it, normally the
 *     next one, with YTL_EV_LATE set and a residual that says how late.
 *     The flag is set when the event fires; a pending event's flags are
 *     0. A window can end behind the reach (a pause with a lead, an
 *     anchor whose RT time is after the onset); a late event past the
 *     window then waits for it.
 *     It is how a script's "now" lands: add the event at the current time
 *     on any base and the next frame carries it.
 *
 *   REWIND
 *     ytl_anchor(tl, b, rt, bt) with bt at or before the furthest base
 *     time of an onset evaluated on the base (or before the time of its
 *     last skip, SKIP) moves the base back: every fired or skipped event
 *     of the base at or after bt, the one exactly at bt included, becomes
 *     pending again. Its landing fields reset (frame -1, onset 0, residual
 *     0, flags 0), the base's event-driven channels are recomputed from
 *     the events still fired or skipped, and the events fire again when
 *     the base reaches them. The base's reach moves to just before bt.
 *     Running a trial's events twice is two anchors at base time 0.
 *     An anchor at a later bt rewinds nothing. Nothing else rewinds: an
 *     anchor at an earlier bt whose RT time is after the next onset puts
 *     that onset before bt, and the events from bt on wait for the base
 *     to reach them again, while the fired events before bt stay fired.
 *     A re-anchor that corrects a drift by moving a base back by a small
 *     amount re-fires what lies in that amount; to avoid it, correct
 *     forward only.
 *
 *   SKIP (ytl_skip)
 *     ytl_skip(tl, b, rt, bt) is a seek that does not fire what it
 *     passes over. It is ytl_anchor(tl, b, rt, bt), and then every
 *     event of the base before bt that is still pending is skipped: its
 *     flags become YTL_EV_SKIPPED, its frame stays -1, its onset
 *     becomes rt and its residual bt minus its time (how far before the
 *     seek's target it was). A skipped event never fires and is in no
 *     evaluate's report. The return value, the number skipped, is the
 *     report of the skip; the storage holds each one. After a skip no
 *     event of the base before bt is pending, and the event exactly at
 *     bt fires as usual on the frame that reaches it.
 *     Forward, an anchor fires on the next frame every event it passed
 *     over, late by its residual; a skip fires none. On a movie base with
 *     an annotation every second, a 30-minute seek fires 1800 events on
 *     one frame after an anchor and none after a skip.
 *     Back, a skip is the anchor's REWIND, and then the late events
 *     before bt still waiting for a window are skipped. A seek calls
 *     ytl_skip() whatever its direction; refusing a skip back would
 *     make every caller compare bt with a time it cannot read.
 *     What else it does:
 *       channels  a skipped ONSET, OFFSET or SET counts for its channel
 *                 as a fired one does (CHANNELS), so the channel shows
 *                 what playing through to bt would, with no report.
 *       tracks    nothing: a track is a function of base time, and the
 *                 next evaluate reads it at its onset's base time. A
 *                 tween waiting for a start before bt takes over at the
 *                 next evaluate, from the old track's value at its start,
 *                 as it would without the skip. "now" is bt, as after an
 *                 anchor.
 *       reach     when the base's reach is before bt, it moves to just
 *                 before bt, and so does the furthest base time of an
 *                 evaluated onset. So an event added later at a time
 *                 before bt is late (LATE EVENTS), and a later anchor back
 *                 to a time before bt makes the skipped events from there
 *                 pending again (REWIND).
 *       state     the base runs from bt at rt, as after an anchor. A seek
 *                 that stays paused is a skip and a ytl_pause() at the
 *                 same rt.
 *     A skip costs a binary search and a write per event it skips.
 *
 *   PEEK (ytl_peek)
 *     ytl_peek(tl, b, from_rt, to_rt, out, cap) lists the pending events
 *     of base b (or of every base, YTL_ALL_BASES) whose RT time is in
 *     [from_rt, to_rt), and changes nothing: no flag, landing, channel,
 *     reach or "now". An event's RT time is when its running base reaches
 *     its time, ytl_rt_time(): anchor_rt + ceil((time - anchor_bt) x den
 *     / num), anchor_rt + (time - anchor_bt) at rate 1. A
 *     paused or stopped base has no RT times and gives no event; a fired
 *     or skipped event is not pending. Returns how many events are in the
 *     window; the first min(that, cap) are copied into out[] ordered by
 *     RT time, then id, each with onset set to its RT time, residual 0,
 *     frame -1 and flags 0. So onset - residual is the RT time in a peek
 *     as in a report. out may be NULL with cap 0, to count.
 *     It is the look-ahead for sound. An evaluate fires an event on the
 *     frame that shows it, at most lead x period early, and ysp/audio.h
 *     needs an onset 43.5 to 54.5 ms before its time (docs/audio.md).
 *     The audio path peeks [last to_rt, now + its lead) each frame and
 *     schedules by id: half-open windows that follow each other neither
 *     overlap nor leave a gap. The event still fires on its frame, and
 *     its landing record is the frame's. An event added less than the
 *     audio lead before its time can be in no peek before its frame
 *     fires it; it is in that evaluate's report, and the caller plays it
 *     late from there. An anchor, skip, pause, resume or rate change moves
 *     the RT times, so peek again after one. The cost is a binary search per
 *     base and a read per event in the window.
 *
 *   RATE (ytl_rate, ytl_get_rate, ytl_rt_time, ytl_clamped)
 *     ytl_rate(tl, b, rt, num, den) runs base b (1 to 7) at num/den base
 *     ns per RT ns from RT time rt on: slow motion for a movie base at 1/2,
 *     a 24 to 23.976 fps conversion at 1000/1001. num and den are in
 *     [1, 2^31 - 1] and are stored reduced by their gcd, so equal rates
 *     are equal bytes. GSAP's movie.timeScale(0.5), anime.js's
 *     anim.speed = 0.5 and Motion's controls.speed = 0.5 are
 *       ytl_rate(&tl, MOVIE, next_onset, 1, 2);
 *     The JS calls read their ticker's clock; this header reads none, so
 *     the change point is an argument. Pass the predicted onset of the
 *     next frame you evaluate, or the RT time of a script's now().
 *     The arithmetic is exact, in integers:
 *       base time at RT t   bt(t) = anchor_bt + floor((t - anchor_rt) x
 *                           num / den)
 *       RT time of base T   rt(T) = anchor_rt + ceil((T - anchor_bt) x
 *                           den / num), ytl_rt_time()
 *     The two round in opposite directions, so the base has reached T at
 *     RT time t exactly when t >= rt(T): every rule that asks "has the
 *     base reached it" (the window, peek, next_due, the report's order)
 *     gives the same answer from either side. Each time is computed from
 *     the anchor, so a rate held for 10 hours is within 1 ns of the exact
 *     rational time, as after 1 s; a change takes one floor, so n changes
 *     leave the base behind the exact time by less than n + 1 ns, never
 *     ahead. Replay stays exact: integer operations only.
 *     What a change does:
 *       running   the base continues from its time at rt, bt(rt) under
 *                 the old rate, at the new rate: no jump, and no rewind.
 *                 It is not an anchor: an anchor at the last onset's base
 *                 time rewinds, and the event there would fire twice. The
 *                 reach and the fired events stay; when the next window
 *                 ends behind the reach (with lead 0.5 and rt at the last
 *                 onset, a drop of the rate by more than 3 times), nothing
 *                 un-fires and late events wait for it (LATE EVENTS).
 *                 "now" (TWEENS) becomes bt(rt). An rt before the last
 *                 onset or after the next one places the change there, as
 *                 an anchor places its rt.
 *       paused    the rate is kept for the resume; the frozen time and
 *                 "now" stay.
 *       stopped   the rate is kept for the next anchor.
 *       same rate nothing changes, not even "now".
 *     Anchor, skip, pause, resume, stop and clear keep the rate; open()
 *     sets 1/1 on every base. Base 0 is always 1/1.
 *     What runs at the rate: everything on the base is in base time, and
 *     the lead stays RT ns. A track is a function of base time, so it
 *     plays at the rate; a sampled track stays exact. A repeat runs at its
 *     rate x num/den in RT, so a flicker defined in display frames belongs
 *     on a base at rate 1. A tween's duration, delay and start are base
 *     time: a 200 ms tween at 1/2 takes 400 ms of RT, as a GSAP tween in a
 *     timeline with timeScale(0.5) does. keep_velocity takes the slope per
 *     base ns, so a change during a tween steps its RT velocity by the
 *     ratio of the rates. A residual is base ns.
 *     Refused with YTL_ERR_ARG: rate 0 (ytl_pause() is the one way to
 *     freeze a base; at 0 the RT time of a base time does not exist) and a
 *     negative rate (base time that goes back conflicts with "an evaluate
 *     never un-fires"; a reverse scrub is a series of ytl_skip() calls,
 *     each a recorded rewind).
 *     Range: times stay below 2^62 in magnitude (TIME). The products are
 *     split so that nothing overflows for any rate, and a base time at or
 *     past +-2^62 (rate 1000/1 held for 53 days, or an anchor near the
 *     limit) becomes +-2^62, which no event time equals, so every event
 *     still fires or waits as the exact time says. The clamp is never
 *     silent: ytl_clamped(tl, b) counts the evaluates and pauses that
 *     met one; ytl_base_time(), ytl_rt_time() and ytl_window()
 *     return false for such a time; ytl_rate() refuses a running base
 *     whose time at rt is out of range.
 *     Cost: at rate 1, one compare more per conversion than v0.3.0. A
 *     scaled base adds two 64-bit divisions per conversion, and an
 *     evaluate does two conversions per base (docs/timeline.md).
 *
 *   SEQUENCES (ytl_seq, ytl_op, desc.keys)
 *     A sequence is a cursor on one base, in base time, for writing a trial
 *     in the order it plays. ytl_seq_on(tl, b) puts the cursor at 0,
 *     ytl_seq_now(tl, b) at the base's "now" (TWEENS). Against GSAP:
 *       ytl_on, ytl_off, ytl_set_value   .set(): ONSET, OFFSET or SET
 *                                              of a channel at the cursor
 *       ytl_trigger, ytl_mark(q, code, user)  .call(): a TRIGGER or a
 *                                              MARK at the cursor
 *       ytl_wait(q, dt_ns), ytl_at(q, t_ns)  "+=dt" and an absolute
 *                                              position; q.t is the cursor,
 *                                              so a variable is a label
 *       ytl_to(q, ch, &d)                    .to(target, vars, "<"): a
 *                                              tween from the cursor plus
 *                                              d.delay_ns; the cursor stays;
 *                                              returns the tween's end
 *       ytl_then(q, ch, &d)                  .to(target, vars): ytl_to
 *                                              and a wait to its end
 *       ytl_keyframes(q, ch, v, dt, n, ease) keyframes: v[i] at the cursor
 *                                              + dt[i], rising, from >= 0
 *       ytl_on_n(q, chans, n, stagger)       stagger: ONSET on chans[i] at
 *                                              the cursor + i x stagger, all
 *                                              or none
 *     Nothing here changes timing: each call lowers to events and keys at
 *     absolute base times, and frame placement, sampling and replay are the
 *     core's.
 *     A STICKY ERROR, as in a stream: the first failing call stores its code
 *     in q.err and its index (calls counted from 0) in q.err_call, writes
 *     the timeline's message (ytl_error), and every later call does
 *     nothing. One check at the end of a sequence does. The
 *     builder is not atomic: the calls before the error stay. Clear the
 *     base after an error, as a trial does anyway, or cancel the sequence.
 *     ytl_seq_cancel(&q) deletes every event of the sequence's base with
 *     an id at or past the sequence's first and the blocks the sequence
 *     built, and returns how many events. Ids rise in add order; the
 *     storage is sorted by time, so it cannot be cut at a mark. So events
 *     that other code added to the same base after the sequence began go
 *     too. A block that an evaluate already took over is removed and its
 *     channel holds its value; the track or tween it replaced is not
 *     brought back. After a cancel the sequence does nothing more.
 *     TWEENS IN A SEQUENCE: all the tweens of one sequence on a channel
 *     become one keyed track in the KEY ARENA, a block of keys grown with
 *     each call: a key at the start and at the end of each segment (cycles
 *     and yoyo unrolled), and between segments the value holds. The block
 *     waits for its first start while the channel's old driver runs, and
 *     then takes over from the old driver's value (and with keep_velocity
 *     its slope) at that start, as ytl_tween() does. From then on it is an
 *     ordinary keyed track, a function of base time: re-anchoring a trial
 *     at 0 replays its tweens exactly. A later tween's `from`, when not set,
 *     is the value where the block ends. A sequence of one tween gives the
 *     same values, bit for bit, as ytl_tween() with the same desc.
 *     Rejected in a sequence with YTL_ERR_ARG, besides ytl_tween()'s own
 *     checks: start_set (the cursor is the start; two sources for it would
 *     be silent), YTL_FOREVER (a repeat folds a whole track, not a
 *     segment; use ytl_set_track with a repeat), a tween that starts
 *     before the end of the channel's previous one in the sequence (cutting
 *     an eased segment part way changes its shape), and keep_velocity on
 *     any but the channel's first tween in the sequence (its slope is the
 *     takeover's). ytl_keyframes takes no BEZIER (it has no curve).
 *     THE KEY ARENA is desc.keys, desc.key_capacity entries of 16 bytes,
 *     the caller's, used while the handle is. With none, a sequence tween
 *     is YTL_ERR_FULL; events need no arena. Each channel's block is a
 *     header slot, its keys and its curve slots, and the channel stores an
 *     offset, not a pointer. A block that is replaced (a new sequence on the
 *     channel, set_track, a tween, a clear of its base) is garbage until a
 *     call needs its room: then the arena is compacted in one pass, and
 *     offsets move with the blocks. An evaluate never compacts or allocates.
 *     A call that does not fit after a compaction is YTL_ERR_FULL and
 *     adds nothing: a track is never cut short. A tween on a block at the
 *     top of the arena grows it in place; on another block it copies the
 *     block to the top, so building several channels at once can need room
 *     for one more copy of the largest block.
 *     OP TABLES: a ytl_op is one call as data, 96 bytes, built with
 *       YTL_ON(ch) YTL_OFF(ch) YTL_SET_VALUE(ch, v) YTL_TRIGGER(code)
 *       YTL_MARK(code, user) YTL_WAIT(dt) YTL_AT(t)
 *       YTL_TO(ch, desc fields...) YTL_THEN(ch, desc fields...)
 *     which are designated initializers: C99, or C++20 in the fields' order
 *     (g++ wants -Wno-missing-field-initializers). A table can be static
 *     const, logged and compared. ytl_run(&q, ops, n) is all or none: it
 *     checks the whole table first, from the sequence's cursor and blocks,
 *     and on an error sets q.err and q.err_call (the op's index) and
 *     changes nothing; then it applies the ops as the calls would.
 *     ytl_check_ops(tl, b, ops, n, &bad) is that check alone, for a new
 *     sequence at 0, changing nothing: for a pack's table when it loads,
 *     not at trial 190. It checks each op's fields, binding (an op against
 *     the first earlier op naming its channel, else the channel's state:
 *     O(n^2) compares, once), the overlap rule, the event storage, and the
 *     arena with room for one more copy of the largest block, so a table
 *     that passes cannot run out of room part way. What it cannot know is
 *     the run-time takeover value.
 *
 *   TARGETS (ytl_anim, ytl_animate, ytl_tween_to, ytl_kill, desc.targets)
 *     A target tween writes a field of the caller: n floats (YTL_F32) or n
 *     int32_t (YTL_I32, rounded to nearest), n from 1 to 4, at an address
 *     the caller keeps valid while the tween lives. It is a call on a
 *     sequence (SEQUENCES), so the sequence's base, cursor, labels and
 *     sticky error apply, and it starts at the cursor + delay_ns; the
 *     cursor stays. Against GSAP:
 *       ytl_tween_to(q, &f, to, dur, ease)          gsap.to(): from the
 *                                                     field's value at start
 *       ytl_tween_from(q, &f, from, dur, ease)      gsap.from(): to the
 *                                                     value at start, showing
 *                                                     `from` until then
 *       ytl_tween_from_to(q, &f, from, to, dur, ease) gsap.fromTo()
 *       ytl_tween_set(q, &f, value)                 .set(): a STEP jump
 *       ytl_animate(q, &a)                          any of these by a desc:
 *                                                     n components, int32_t,
 *                                                     delay, cycles, yoyo,
 *                                                     keep_velocity,
 *                                                     immediate, overwrite,
 *                                                     callbacks
 *       ytl_animate_n(q, &a, stride, n)             stagger: field i at
 *                                                     a.target + i x stride
 *                                                     bytes starts i x
 *                                                     a.stagger_ns later
 *       q.prev_start, q.prev_end                    "<" and ">": the start
 *                                                     and end of what the
 *                                                     last call added
 *       ytl_kill(tl, &f), ytl_kill_tween(tl, id)    killTweensOf(), kill()
 *     The fields of ytl_anim mean what ytl_tween_desc's mean (TWEENS), with
 *     the same arithmetic, so a target and a channel tween given the same
 *     desc have the same values bit for bit; to_current is gsap.from()'s
 *     end value, the value at the start.
 *     THE MODEL. A target is an address; one target per address, bound to
 *     one base as a channel is (another base is YTL_ERR_BOUND, a different
 *     type or n YTL_ERR_ARG). Its tweens are kept in (start, id) order, and
 *     at base time t the tween with the latest (start, id) that has started
 *     drives the field: a later tween takes the field over from its start,
 *     and the earlier one drives it again if the base goes back before that
 *     start. So the field is a function of base time, and a rewind, a skip
 *     or a seek (REWIND, SKIP) gives the values of continuous play. A tween
 *     without from_set starts from the field's value at its start: the
 *     value of the tween before it there, or for the first the field's own
 *     value before the timeline wrote it (the rest value, read once). These
 *     start values are computed at the first evaluate that reaches the
 *     start, kept, and replayed by a rewind, as a channel tween's takeover
 *     is. A tween added later with an earlier start does not change the
 *     kept values of the tweens after it.
 *     WRITES. Each evaluate, after the events and channels, writes every
 *     target of a base that is not stopped, in one pass: the driving
 *     tween's value at the frame's onset, or, for a STEP tween (a set
 *     included), at the window end, so its change lands on the frame where
 *     an event at the same time lands (QUANTIZATION). Before the first
 *     tween starts the field is not written and stays the caller's, unless
 *     that tween has `immediate` (from and fromTo set it, GSAP's
 *     immediateRender), which shows its `from`. After the first write the
 *     timeline owns the field until its tweens are killed: every evaluate
 *     writes it, also after the last tween ends, and a rewind before the
 *     first start writes the rest value. A caller's write in between is
 *     overwritten at the next evaluate, not read.
 *     OVERWRITE. a.overwrite = YTL_OVERWRITE_NONE (0) keeps the field's
 *     other tweens, as above (a GSAP timeline with overwrite: false).
 *     YTL_OVERWRITE_ALL kills them at the call (GSAP's overwrite: true),
 *     on any base, so the target can change base; the new tween then
 *     starts from the field's value at its start, which is the last value
 *     written. It is for a script that retargets a field on a base that is
 *     never cleared, where kept tweens would fill the pool.
 *     KILL. ytl_kill(tl, &f) kills the field's tweens, ytl_kill_tween(tl,
 *     id) one tween. Their callback events go, the field keeps its value,
 *     and a target left with no tween is freed: the timeline stops writing
 *     the field, and the next tween on it reads its rest value again.
 *     ytl_clear() of a base and ytl_seq_cancel() kill the tweens of the
 *     base or the sequence. So clear the trial base between trials, and
 *     each trial starts from the values the caller set.
 *     CALLBACKS. on_start and on_complete (not with YTL_FOREVER) each add an
 *     event to the storage, YTL_CALL_START at the start or
 *     YTL_CALL_COMPLETE at the end, with code = the tween's id and user =
 *     a.user. The events land, skip and rewind as events do, and they are
 *     in the report. ytl_evaluate() calls the functions after it has
 *     written every field, in the order in which their events fired, as
 *     fn(tl, id, user). A callback may post and kill tweens and add
 *     events; what it posts runs from the next evaluate. ytl_evaluate()
 *     from a callback is YTL_ERR_ORDER. A kill in on_start skips the same
 *     frame's on_complete.
 *     REJECTED with YTL_ERR_ARG, besides ytl_tween()'s checks on component
 *     0 (TWEENS): a NULL target, n outside 0 to 4, a type that is not
 *     YTL_F32 or YTL_I32, an overwrite that is neither, a non-finite
 *     component of to (or from with from_set), LOG with a component <= 0 or
 *     with to_current, n < 1 for ytl_animate_n(), and a start, an end or a
 *     staggered start at or past +-2^62. YTL_FOREVER and cycles are allowed
 *     (a target tween is not a segment of a block). Op tables have no
 *     target form: a pack's table cannot hold an address.
 *     POOLS. desc.targets and desc.tweens are arrays of the caller, used
 *     while the handle is: a target is 40 bytes, a tween 128 (64-bit). A
 *     call that needs more targets, tweens or events (for its callbacks)
 *     than are free, counting what its overwrite frees, is YTL_ERR_FULL,
 *     adds nothing, and its message names the pool ("the tween pool is
 *     full (128)"). ytl_tween_count() gives the live tweens and targets.
 *     A target's tweens are found by a scan of the targets, at the call,
 *     not in the frame.
 *     COST. The pass reads each target and its driving tween: O(1) per
 *     target while time moves forward, a walk of the target's tweens after
 *     a rewind. Nothing is allocated. examples/timeline/bench_targets.c
 *     measures it against the value array and a binding copy
 *     (docs/timeline.md, "Targets").
 *
 *   REMOVING (ytl_remove, ytl_prune, ytl_clear)
 *     ytl_remove(tl, id) deletes one event; when it had fired and its
 *     channel showed its value, the channel is recomputed.
 *     ytl_prune(tl, base) deletes every fired or skipped event of a base
 *     (YTL_ALL_BASES for all): for a long session on the RT base, where
 *     a script adds waits forever and the caller has logged the reports.
 *     Channels keep their values; a channel left with no events is free,
 *     and a later rewind of the base recomputes from what remains.
 *     ytl_clear(tl, base) deletes every event of a base and the keys of
 *     every channel bound to it, returns those channels to their initial
 *     values and frees them. The base keeps its state and anchor. Ids are
 *     never reused before the next open().
 *
 *   REPLAY
 *     A timeline is a function of the calls made on it. The same open(),
 *     the same adds, anchors and keys, and the same frames (onset, period,
 *     index) produce the same values, the same reports and the same
 *     storage bytes, every time and on every thread. The storage is the
 *     record of where every event landed. Two builds agree on every event
 *     field bit for bit (integer arithmetic) and on track values to the
 *     libm in use: COSINE and LOG call cos(), exp() and log(), whose last
 *     bit may differ between C libraries. The other easings, BEZIER, and
 *     the sampled interpolations are +, -, * and / in double, exact under
 *     IEEE 754 unless the compiler contracts them into fused multiply-adds
 *     (-ffp-contract=fast, or /fp:fast). So a trajectory the pack tool
 *     computed is the same bits on every platform; the same trajectory
 *     computed at run time with sin() would not be.
 *
 *   WITH ysp/trials.h
 *     The trial handler picks the condition, the caller turns it into
 *     events and keys on a trial base, anchors the base at the trial's
 *     first frame, and the timeline runs it. Nothing links the two
 *     headers. At movie scale, the storage holds thousands of annotations
 *     on a movie base, anchored, paused and skipped with the player.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   The handle is about 64 KB at the default YTL_MAX_CHANNELS (256),
 *   almost all of it 248 bytes per channel: a copy of its track, a
 *   tween's three keys and curve, and a waiting tween's desc. An evaluate
 *   reads a byte per channel to
 *   find the tracked ones and touches only their records. Targets and
 *   their tweens live in the pools the desc gives (TARGETS). Define
 *   YTL_MAX_CHANNELS (1 to 65535) before the implementation and every
 *   include to change it. Events live in the storage the desc gives
 *   (desc.events, desc.event_capacity entries of 64 bytes), owned by the
 *   caller and used while the handle is. Keys, curves and samples are the
 *   caller's arrays. A sequence's tweens live in the key arena the desc
 *   gives (desc.keys, 16 bytes a slot). Nothing is allocated.
 *   A frame's cost is a binary search per base that is not stopped, the
 *   events that fire (each a copy, plus an insertion into the report),
 *   and one key lookup per keyed channel, which is O(1) while time moves
 *   forward a segment at a time. Nothing depends on the refresh rate:
 *   the period is an argument, times are whole nanoseconds, and a faster
 *   display only moves the key cache less per frame. The per-frame cost
 *   measured the same at 60, 500 and 1000 Hz (docs/timeline.md), so
 *   the share of the budget grows with the rate and nothing else does.
 *   examples/timeline/bench.c [refresh_hz] measures it.
 *   No function is safe to call on one handle from two threads at once.
 *   The intended use is the frame thread only.
 *
 *   ---------------------------------------------------------------------
 *   ERRORS AND CLOSE
 *   ---------------------------------------------------------------------
 *   ytl_open() clears the message, and a failed open leaves one. Every
 *   other call on the handle that returns a YTL_ERR_* code writes the
 *   message before it returns, in the frame loop too ("ysp_timeline:
 *   ytl_evaluate: call out of order"), and so does a sequence call that
 *   sets q.err ("ysp_timeline: sequence call 3: invalid argument"). The
 *   message is the last failure's; ytl_error() reads it. The write is an
 *   snprintf on the failure path only. A query that returns bool
 *   (ytl_base_time, ytl_rt_time, ytl_window, ytl_get_rate, ytl_next_due)
 *   answers false and writes nothing: false is an answer there, not a
 *   failure.
 *   ytl_close() ends the use of the handle. The handle owns nothing (the
 *   storage and the key arena are the caller's), so close marks it closed
 *   and keeps the message; calls after it return YTL_ERR_CLOSED. It is safe
 *   on a zeroed handle, after a failed open, and a second time.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Link libm on Linux (-lm). Define YTL_API to change the linkage of
 *   every function (for example `static`); with `static`, a translation
 *   unit that does not call every function needs -Wno-unused-function.
 *
 *   LICENSE: public domain / MIT-0, see end of file.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.6.0 - targets (TARGETS): tweens that write the caller's fields (a
 *          float, float[2..4] or int32_t), as GSAP's to(), from(),
 *          fromTo() and set(): ytl_tween_to(), ytl_tween_from(),
 *          ytl_tween_from_to(), ytl_tween_set(), ytl_animate() with a
 *          ytl_anim desc (delay, cycles, yoyo, keep_velocity, immediate,
 *          overwrite, on_start and on_complete), ytl_animate_n() for a
 *          stagger, ytl_kill(), ytl_kill_tween(), ytl_tween_count(), and
 *          the caller's pools desc.targets and desc.tweens. The latest
 *          started (start, id) drives a field, so a rewind or a seek
 *          replays it. Callbacks are events of the new kinds
 *          YTL_CALL_START and YTL_CALL_COMPLETE, called after every field
 *          is written. ytl_seq.prev_start and prev_end ("<" and ">").
 *          Four eases: YTL_EASE_QUAD_IN_OUT, CUBIC_IN, CUBIC_OUT and
 *          CUBIC_IN_OUT, for keys and channel tweens too, so 11 is now the
 *          first number that is not a ytl_ease. Nothing else changes: a
 *          timeline with no target gives the same values and bytes.
 *   v0.5.0 - the repository conventions (README.md). Breaks the API. Time
 *          fields the caller writes end in _ns: ytl_event.time_ns,
 *          ytl_key.time_ns, ytl_track.t0_ns and period_ns,
 *          ytl_tween_desc.duration_ns, start_ns and delay_ns, ytl_op.t_ns;
 *          time parameters are named _ns. YTL_S, YTL_MS, YTL_NS_PER_S and
 *          YTL_NS_PER_MS are gone: the header defines ysp/rt.h's YRT_S,
 *          YRT_MS, YRT_US, YRT_NS, YRT_TO_S and YRT_TO_MS under the same
 *          guard. ytl_close(). Every int call that fails writes
 *          ytl_error(), and so does a sequence call that sets q.err;
 *          ytl_peek(), ytl_clamped() and ytl_check_ops() take a non-const
 *          handle for it. The CHANGELOG moved to the end of this comment,
 *          and USAGE builds as C99 and C++17. Nothing else changes: the
 *          same calls give the same values, records and bytes.
 *   v0.4.0 - base rates: ytl_rate(tl, base, rt, num, den) runs a base at
 *          an exact rational rate from RT time rt on, with no jump and no
 *          rewind (GSAP's timeScale); ytl_get_rate(); ytl_rt_time(),
 *          the first RT time at which a running base reaches a base time.
 *          ytl_window(): the base time a frame's window reaches, the
 *          rule evaluate uses, for a scheduler or ysp/video.h. A time at
 *          or past +-2^62 is clamped only with a record: ytl_clamped()
 *          counts the evaluates and pauses that met one, and the queries
 *          refuse it (before, the sum could overflow). The report is
 *          ordered by when the base reaches each event, which is v0.3.0's
 *          onset - residual at rate 1. A timeline that sets no rate gives
 *          the same bytes as v0.3.0.
 *          Sequences: ytl_seq, a cursor on a base for building a trial
 *          in the order it plays, as a GSAP timeline is built (ytl_on,
 *          ytl_off, ytl_set_value, ytl_trigger, ytl_mark,
 *          ytl_wait, ytl_at, ytl_to, ytl_then, ytl_keyframes,
 *          ytl_on_n), with a sticky error; op tables (ytl_op, the
 *          YTL_ON() ... macros) applied all or none by ytl_run() and
 *          checked at load by ytl_check_ops(); ytl_seq_cancel(). A
 *          sequence's tweens on a channel become one keyed track in a key
 *          arena (desc.keys), so a channel takes any number of them and a
 *          rewind replays them. Every key of a track now reads its curve
 *          through one path (ytl_sample() is unchanged).
 *   v0.3.0 - ytl_skip(): a seek that marks the events it passes over
 *          YTL_EV_SKIPPED instead of firing them late on one frame, and
 *          returns how many; their channel values still apply. A skip
 *          back is an anchor's rewind. ytl_peek(): the pending events
 *          in an RT window, with their RT times, changing nothing, for
 *          the audio path's look-ahead. ytl_lead(): the lead in use.
 *          A rewind now makes skipped events pending again, and prune
 *          deletes skipped events with the fired ones. Nothing else
 *          changes: a timeline that never skips gives the same bytes as
 *          v0.2.0.
 *   v0.2.0 - tracks grow: ytl_track with a keyed or a SAMPLED form
 *          (floats at an integer rate, LINEAR, STEP or Catmull-Rom CUBIC,
 *          for long trajectories such as a sum-of-sines target), a REPEAT
 *          (period and count, for flicker and counterphase), the
 *          YTL_EASE_BEZIER easing (CSS cubic-bezier, from a per-track
 *          curve table), and ytl_tween() with a ytl_tween_desc: to,
 *          from, duration, start or delay, ease, cycles, yoyo, and
 *          keep_velocity (a Hermite takeover with no velocity step); a
 *          tween waits until its start while the old track runs on, and
 *          takes the old track's value there; its keys live in the
 *          handle. YTL_S() and YTL_MS() for literals. A STEP track is
 *          read at the end of the lead window, so its changes land where
 *          events at the same times land; other tracks are read at the
 *          onset. Breaking: the default lead is now half a frame (desc.lead
 *          0 means 0.5), and YTL_LEAD_NONE asks for the old never-early
 *          rule, because under onset noise never-early misplaced about
 *          12% of frame-aligned changes;
 *          ytl_sample() takes a ytl_track; ytl_key's reserved bytes
 *          are now reserved_ (1) and curve (2), so a key written as
 *          { time, value, ease } still compiles.
 *   v0.1.0 - first implementation: time bases with anchor, pause, resume
 *          and rewind (by anchor only); events in a caller-sized array;
 *          keyframe tracks with six easings; evaluate at the predicted
 *          onset with a lead window; late events by the base's reach;
 *          prune, remove and clear; the record of where every event
 *          landed.
 *
 *   STATUS: v0.6.0. Targets, tested by the writer of the implementation
 *   (tests/adapt/timeline_test_targets.h): a target tween against
 *   ytl_tween() on a channel over 400 random descs, the second taking over
 *   from the first, the same bits on all 28,000 frames; every ease against
 *   pow()-based formulas, keys too; to, from, fromTo, set, n = 3, int32_t,
 *   stagger both ways, the overwrite rules, kill, clear, cancel, ownership;
 *   callbacks (landing, order, a post and a kill from one, skip, rewind,
 *   ytl_evaluate() refused); every pool refusal with its message, all or
 *   none; sets landing with events at three leads (900 cases); a trial
 *   replayed after a rewind and sought into from 11 places, the same bits,
 *   and two handles the same bytes; and a model of TARGETS over 12 random
 *   runs of 500 calls with rewinds, skips, pauses and kills. 3 model
 *   mutations of TARGETS are caught. The earlier tests pass unchanged
 *   apart from the first number that is not an ease (11, was 7), with the
 *   same model statistics. MinGW gcc 16.1 (C11, C99 and C++17 compile
 *   checks) and MSVC 19.44 (C, C++17).
 *   v0.4.0. Rates and the window, tested by the writer of the
 *   implementation against an exact reference written apart from the
 *   header's arithmetic (32-bit limbs, itself checked against __int128):
 *   200,000 random cases of the mapping and its inverse to +-2^62; rate
 *   grids at 60 to 1000 Hz, six rates, three leads; the change point swept
 *   across a frame at 500 and 1000 Hz (208,000 landings); directed checks
 *   of every RATE rule and clamp; 12 more model runs with rate, window and
 *   rt_time ops; replay. The v0.3.0 test passes unchanged, its 20 model
 *   runs give the same statistics, and the examples print the same bytes
 *   but the version. 9 model mutations of RATE are caught; of 24 header
 *   mutations, 21 are caught and 3 cannot change a result
 *   (docs/timeline.md). MinGW gcc 16.1 (C99, C11, C++17), gcc 11.4
 *   under WSL2 (ASan, UBSan, -O3), MSVC 19.44 (C, C11, C++17). An
 *   evaluate with 7 bases at rate 1 costs 37 to 40 ns against v0.3.0's 26
 *   to 43; a scaled base adds about 4 to 5 ns.
 *   Sequences, also by the writer of the implementation: the rig_spec
 *   4.5.1 trial by calls and as a table (both ramps against a keyed
 *   reference on every frame, a re-anchor replaying them bit for bit);
 *   every ytl_run refusal with the handle, storage and arena unchanged
 *   by memcmp and the same code and index from ytl_check_ops; one
 *   sequence tween against ytl_tween() on 400 random descs, the same
 *   bits; cancel, on_n, the cross-base stop, the arena under clears; and
 *   600 random tables built by the calls, by ytl_run and by hand with
 *   ytl_add and ytl_set_track, compared on 160 frames. 19 of 19
 *   header mutations of the sequence code are caught. The op macros
 *   compile as C99 and as C++20 (tests/compile/timeline_ops.cpp). 32
 *   keyed channels sample within this machine's run-to-run spread of
 *   v0.3.0 (docs/timeline.md, "Sequences").
 *   v0.3.0. Skip, peek and lead: tests/adapt/timeline_test.c
 *   has directed checks for each (the 30-minute seek against an anchor,
 *   skip back against an anchor back, a waiting late event, the reach and
 *   rewind after a skip, prune and remove of skipped events, tracks and a
 *   waiting tween, peek's window edges, order, cap, bases and purity by
 *   memcmp of the whole handle, and a 22 s audio look-ahead at 60 Hz), and
 *   skip and peek are ops of 10 more seeds of the random model check.
 *   Unlike the v0.1 and v0.2 checks, these were written by the writer of
 *   the implementation. Ten model mutations of skip and peek are each
 *   caught; of 23 mutations of the header, 20 are caught and 3 cannot
 *   change a result (docs/timeline.md). The v0.2.0 test, unchanged,
 *   passes with the same model counts, and the examples print the same
 *   bytes but the version. Built and run with gcc 11.4 (WSL2: C99, C11,
 *   C++17, ASan and UBSan), MinGW-w64 gcc 16.1 (C99, C11, C++17), MSVC
 *   19.44 (default C, C11, C++17) and emcc 6.0.10 under node.
 *   timeline_bench at 60 Hz (gcc): trial 51 ns, movie 322 ns mean and
 *   599 ns p99, script 60 ns, tracks 246 ns: inside v0.2.0's ranges.
 *   v0.2.0: Built and run on Linux (WSL2) with gcc 11.4 as C99
 *   and C11 under -Wall -Wextra -Wpedantic -Wshadow -Werror at -O2 and
 *   -O3, and under -fsanitize=address,undefined -fno-sanitize-recover=all
 *   with no diagnostic; the compile checks also as C++17. On Windows 11
 *   with MSVC 19.44 under /W4 /WX, in its default C dialect and as
 *   /std:c++17. examples/timeline/trial.c and timeline_tracking.c print
 *   the same bytes on both compilers.
 *   tests/adapt/timeline_test.c was written from this manual by
 *   writers who did not read the implementation, with its own model, and
 *   checks: every rejected desc and argument; quantization on grids of 60,
 *   144, 240, 360, 500 and 1000 Hz at lead 0, 0.25 and 0.5 against a
 *   ceiling-division model with times on the window edges, and period 0;
 *   base states, pause with a lead, base 0 refusals; channel values by
 *   (time, id), binding and YTL_ERR_BOUND, add_n all or none; the
 *   report's order, cap and total; late events and the reach; anchor
 *   rewinds; remove, prune and clear; next_due. Tracks: every easing
 *   against its own formulas, BEZIER against a bisection solver on ten
 *   curves; sampled placement over 600k samples at rates 1, 7, 60, 120,
 *   999983 and 1e9 in integer arithmetic, LINEAR, STEP and CUBIC with the
 *   phantom ends; REPEAT folding and the hold; the 7.5 Hz flicker against
 *   the frame count at 60 to 500 Hz under onset noise; every
 *   set_track and tween rejection alone; tweens: every desc field and
 *   default, YTL_S and YTL_MS, "now" after evaluates, anchors, pauses
 *   and resumes, the takeover's from at the start for keyed, sampled,
 *   repeating, tween and Hermite old tracks, identical across seven frame
 *   grids, no repeated position on a retarget, keep_velocity's value and
 *   slope continuity against a Hermite model, cycles, FOREVER and yoyo,
 *   cancellation, replacement, the cross-base stop; a STEP track's change landing on
 *   the same frame as an event at the same time over 300 random cases;
 *   replay of the calls into two handles, storages compared with memcmp;
 *   and a random model check, 10 seeds x 800 steps with set_track and
 *   tweens that wait and take over, comparing the whole storage and every
 *   value after each step.
 *   Deliberate mutations of the model (lead off by one, rewind bounds, the
 *   LATE flag, an evaluate-time un-fire, repeated instead of phantom end
 *   samples, a whole-ns sample step, the hold a cycle late or folded, the
 *   Bezier parameter used as x, STEP tracks read at the onset, a tween's
 *   from read at the call, keep_velocity's slope ignored, the yoyo
 *   return leg missing) are each caught, and so is the first version's
 *   rule for "now". examples/timeline/tracking.c runs a 10-minute 5 Hz
 *   sum-of-sines target at 500 Hz within 1.73e-5 of its peak and a 7.5 Hz
 *   flicker with no wrong frame in 300500. examples/timeline/bench.c
 *   measures a frame's evaluate at 46-527 ns mean and under 1.6 us p99 on
 *   the workloads docs/timeline.md lists, the same at 60, 500 and
 *   1000 Hz; at 500 Hz the worst p99 is 0.04% of the 2 ms budget. Rare
 *   slow frames (up to 1.4 ms at 500 Hz) are the machine's, not the
 *   header's: two identical timelines timed in lockstep are slow on
 *   different frames (docs/timeline.md). What is NOT done: no run on
 *   macOS, clang, the newer gcc of CI or under emscripten (CI covers
 *   those); nothing here is a timing measurement of a display: the header
 *   computes, ysp/screen.h and the photodiode measure.
 *
 */
#ifndef YSP_TIMELINE_H_INCLUDED
#define YSP_TIMELINE_H_INCLUDED

/* The version of this header, for a log line or a compile-time check. */
#define YTL_VERSION_MAJOR 0
#define YTL_VERSION_MINOR 6
#define YTL_VERSION_PATCH 0
#define YTL_VERSION_STRING "0.6.0"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YTL_API
#define YTL_API extern
#endif

#ifndef YTL_MAX_CHANNELS
#define YTL_MAX_CHANNELS 256
#endif
#define YTL_MAX_BASES 8

/* A channel number is stored in an event's int32 target, and the handle
 * holds one record per channel, so the bound is about handle size. */
#if YTL_MAX_CHANNELS < 1 || YTL_MAX_CHANNELS > 65535
#error "YTL_MAX_CHANNELS must be in [1, 65535]"
#endif

/* --- codes ------------------------------------------------------------- */

#define YTL_ERR_ARG       (-1)
#define YTL_ERR_CLOSED    (-2)  /* the handle is not open                 */
#define YTL_ERR_ORDER     (-3)  /* an onset earlier than the last one     */
#define YTL_ERR_FULL      (-4)  /* the storage or the ids are used up     */
#define YTL_ERR_BOUND     (-5)  /* the channel is bound to another base,
                                   * or to the other kind of driver        */
#define YTL_ERR_NOT_FOUND (-6)  /* no event has that id                   */

#define YTL_BASE_RT     0     /* the RT clock; always running             */
#define YTL_ALL_BASES (-1)    /* ytl_prune, ytl_clear, ytl_events   */
#define YTL_LEAD_NONE (-1.0)  /* desc.lead: never early; 0 is the default,
                                 * half a frame                             */

/* Units, the same block as ysp/rt.h's under the same guard, so one spelling
 * works with or without it: .duration_ns = YRT_MS(200). Rounded to the
 * nearest ns; constant expressions, so they work in a static initializer.
 * For a run-time double, ytl_ns() also clamps. */
#ifndef YRT_S
#define YRT_NS(x)    ((int64_t)(x))
#define YRT_US(x)    ((int64_t)((x) * 1e3 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_MS(x)    ((int64_t)((x) * 1e6 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_S(x)     ((int64_t)((x) * 1e9 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_TO_MS(ns) ((double)(ns) / 1e6)
#define YRT_TO_S(ns)  ((double)(ns) / 1e9)
#endif

#define YTL_FOREVER (-1)      /* ytl_tween_desc.cycles                  */

/* Static description of a YTL_ERR_* code ("ok" for values >= 0). */
YTL_API const char* ytl_strerror(int code);

/* YTL_VERSION_STRING as compiled into the implementation. */
YTL_API const char* ytl_version(void);

/* Seconds to nanoseconds, rounded to nearest, ties away from zero, and
 * clamped to +-2^62. NaN gives 0. */
YTL_API int64_t ytl_ns(double seconds);

/* --- events ------------------------------------------------------------ */

typedef enum ytl_kind {
    YTL_MARK      = 0,   /* reported only                                */
    YTL_TRIGGER   = 1,   /* reported only; code is the port value        */
    YTL_ONSET     = 2,   /* channel target := 1                          */
    YTL_OFFSET    = 3,   /* channel target := 0                          */
    YTL_SET       = 4,   /* channel target := value                      */
    YTL_CALL_START    = 5, /* reported; a target tween's on_start (TARGETS):
                            * code is the tween id, user its user         */
    YTL_CALL_COMPLETE = 6, /* reported; a target tween's on_complete      */
    YTL_KIND_USER = 16   /* 16..255: reported only, the caller's meaning */
} ytl_kind;

/* Event flags, set by the header. */
#define YTL_EV_FIRED   1u /* it has landed on a frame                     */
#define YTL_EV_LATE    2u /* it was added after its window had passed     */
#define YTL_EV_SKIPPED 4u /* a ytl_skip() passed over it; never fires   */

/* One event. The caller fills time_ns, base, kind and its payload; the
 * header assigns id and writes the landing fields. 64 bytes. */
typedef struct ytl_event {
    int64_t  time_ns;   /* on its base                                      */
    int32_t  target;    /* the channel for ONSET, OFFSET and SET; the
                         * caller's for the other kinds                     */
    float    value;     /* SET's value; must be finite                      */
    int32_t  code;      /* the caller's: a trigger value, an annotation row */
    uint8_t  base;      /* 0 .. YTL_MAX_BASES-1                           */
    uint8_t  kind;      /* a ytl_kind                                     */
    uint16_t flags;     /* YTL_EV_*; ignored on add                       */
    uint64_t user;      /* the caller's: a pointer, a coroutine, a string id */
    int32_t  id;        /* assigned on add; ignored on add                  */
    int32_t  reserved_; /* zero                                             */
    /* landing, written when it fires; frame -1 while pending and when
     * skipped (SKIP), and the RT time in a ytl_peek() copy (PEEK) */
    int64_t  frame;     /* ytl_frame.index of the frame it landed on      */
    int64_t  onset;     /* that frame's predicted onset, RT ns              */
    int64_t  residual;  /* base time at onset minus time_ns, ns             */
} ytl_event;

/* --- tracks ------------------------------------------------------------ */

typedef enum ytl_ease {
    YTL_EASE_LINEAR   = 0,
    YTL_EASE_STEP     = 1,
    YTL_EASE_COSINE   = 2,
    YTL_EASE_QUAD_IN  = 3,
    YTL_EASE_QUAD_OUT = 4,
    YTL_EASE_LOG      = 5,
    YTL_EASE_BEZIER   = 6,   /* key.curve indexes the track's curves[]   */
    YTL_EASE_QUAD_IN_OUT  = 7,   /* v0.6.0; GSAP power1.inOut              */
    YTL_EASE_CUBIC_IN     = 8,   /* GSAP power2.in                         */
    YTL_EASE_CUBIC_OUT    = 9,   /* GSAP power2.out                        */
    YTL_EASE_CUBIC_IN_OUT = 10   /* GSAP power2.inOut                      */
} ytl_ease;

/* One key: the value at a time on the track's base, and the easing of the
 * segment that starts here. The last key's ease is not used. 16 bytes. */
typedef struct ytl_key {
    int64_t  time_ns;   /* on the track's base                              */
    float    value;     /* must be finite                                   */
    uint8_t  ease;      /* a ytl_ease                                     */
    uint8_t  reserved_;
    uint16_t curve;     /* YTL_EASE_BEZIER: index into the track's curves */
} ytl_key;

/* A cubic Bezier easing from (0, 0) to (1, 1) with control points (x1, y1)
 * and (x2, y2), as CSS cubic-bezier() takes them. x1 and x2 in [0, 1]. */
typedef struct ytl_curve {
    float x1, y1, x2, y2;
} ytl_curve;

/* How a sampled track reads between two samples. */
typedef enum ytl_interp {
    YTL_INTERP_LINEAR = 0,
    YTL_INTERP_STEP   = 1,   /* the sample at or before                   */
    YTL_INTERP_CUBIC  = 2    /* Catmull-Rom through the samples           */
} ytl_interp;

/* A track: keys at any times, or samples at a fixed step, and an optional
 * repeat (TRACKS). Exactly one of keys and samples is set. */
typedef struct ytl_track {
    const ytl_key*   keys;       /* keyed: n_keys keys sorted by time     */
    const ytl_curve* curves;     /* keyed: for YTL_EASE_BEZIER keys     */
    const float*       samples;    /* sampled: samples[i] at t0_ns + i/rate s */
    int64_t            t0_ns;      /* sampled: base time of samples[0]      */
    int64_t            rate;       /* sampled: samples per second, 1..1e9   */
    int64_t            period_ns;  /* > 0: repeat every period_ns           */
    int                n_keys;
    int                n_curves;
    int                n_samples;
    int                interp;     /* sampled: a ytl_interp               */
    int                repeats;    /* with period: cycles, 0 = forever      */
} ytl_track;

/* The value of a track at time t (TRACKS), or 0 when it has neither keys
 * nor samples. No validation: the track is assumed to be one that
 * ytl_set_track() accepts. */
YTL_API double ytl_sample(const ytl_track* tr, int64_t t_ns);

/* A tween (TWEENS). Every field may be zero: a zeroed desc is a linear
 * jump to 0 now. GSAP's gsap.to(g, {contrast: 0, duration: 0.2,
 * ease: "sine.inOut"}) is
 *     ytl_tween_desc d = { 0 };
 *     d.to = 0.0f; d.duration_ns = YRT_MS(200); d.ease = YTL_EASE_COSINE;
 *     ytl_tween(&tl, CONTRAST, TRIAL, &d);
 */
typedef struct ytl_tween_desc {
    float       to;
    float       from;           /* with from_set; else the value at start   */
    int64_t     duration_ns;    /* >= 0; 0 jumps                            */
    int64_t     start_ns;       /* base time, with start_set; else now      */
    int64_t     delay_ns;       /* added to start_ns, either way            */
    ytl_curve curve;          /* YTL_EASE_BEZIER only                   */
    int         ease;           /* a ytl_ease; 0 = LINEAR                 */
    int         cycles;         /* 0 or 1: once; n; YTL_FOREVER           */
    bool        from_set;
    bool        start_set;
    bool        yoyo;           /* a cycle goes to `to` and back            */
    bool        keep_velocity;  /* start at the track's velocity, end at rest */
} ytl_tween_desc;

/* --- targets (TARGETS) ------------------------------------------------- */

#define YTL_F32 0               /* ytl_anim.type: float components         */
#define YTL_I32 1               /* int32_t components, rounded to nearest  */
#define YTL_OVERWRITE_NONE 0    /* the latest start drives the field       */
#define YTL_OVERWRITE_ALL  1    /* kill the field's other tweens at the call */

struct ytl_timeline;

/* A target tween's callback: the timeline, the tween's id and anim.user.
 * ytl_evaluate() calls it after it writes every field (TARGETS). */
typedef void (*ytl_anim_fn)(struct ytl_timeline* tl, int id, void* user);

/* A tween of a field of the caller (TARGETS), GSAP's vars. Every field but
 * target may be zero: a zeroed desc with a target is a linear jump of one
 * float to 0 at the cursor. */
typedef struct ytl_anim {
    void*       target;         /* the field: n floats, or n int32_t        */
    int         type;           /* YTL_F32 (0) or YTL_I32                   */
    int         n;              /* components, 1 to 4; 0 is 1               */
    float       to[4];          /* the end value; unless to_current         */
    float       from[4];        /* with from_set; else the value at start   */
    int64_t     duration_ns;    /* >= 0; 0 jumps at the start               */
    int64_t     delay_ns;       /* after the sequence's cursor              */
    int64_t     stagger_ns;     /* ytl_animate_n(): target i starts i x this
                                 * later                                    */
    ytl_curve   curve;          /* YTL_EASE_BEZIER only                     */
    int         ease;           /* a ytl_ease; 0 = LINEAR                   */
    int         cycles;         /* 0 or 1: once; n; YTL_FOREVER             */
    int         overwrite;      /* YTL_OVERWRITE_NONE (0) or _ALL           */
    bool        from_set;
    bool        to_current;     /* GSAP from(): `to` is the value at start  */
    bool        yoyo;           /* a cycle goes to `to` and back            */
    bool        keep_velocity;  /* start at the field's velocity, end at rest */
    bool        immediate;      /* the target's first tween: show `from`
                                 * before the start (GSAP immediateRender) */
    ytl_anim_fn on_start;       /* each uses an event of the storage        */
    ytl_anim_fn on_complete;    /* not with YTL_FOREVER                     */
    void*       user;
} ytl_anim;

/* Private, 40 bytes: one target, a field the timeline writes. The caller
 * gives an array of them (desc.targets) and treats it as opaque. */
typedef struct ytl_target_slot {
    void*    ptr;
    float    rest[4];      /* the field before the first write              */
    int32_t  head;         /* its first tween in (start, id) order          */
    int32_t  cur;          /* the last tween started by the last evaluate's
                            * onset, or -1                                  */
    int32_t  n_tw;
    uint8_t  type, n, base, flags;
} ytl_target_slot;

/* Private, 128 bytes on 64-bit: one target tween (desc.tweens). The fields
 * an evaluate reads come first, in one cache line. */
typedef struct ytl_tween_slot {
    int64_t     start_ns;
    int64_t     duration_ns;
    float       from[4];
    float       to[4];
    int32_t     next;      /* the target's next tween, or the free list     */
    int32_t     cycles;
    uint8_t     ease;
    uint8_t     flags;
    uint8_t     due;       /* callbacks fired by this evaluate              */
    uint8_t     reserved_;
    int32_t     target;
    ytl_curve   curve;     /* BEZIER's curve, or keep_velocity's tangents   */
    int32_t     id;
    uint32_t    seq_id;
    int32_t     due_next;
    void*       user;
    ytl_anim_fn on_start;
    ytl_anim_fn on_complete;
} ytl_tween_slot;

/* --- description and handle ------------------------------------------- */

typedef struct ytl_desc {
    ytl_event* events;          /* storage, event_capacity entries; the
                                   * caller's, used while the handle is     */
    int          event_capacity;  /* >= 0                                   */
    int          n_channels;      /* 0 .. YTL_MAX_CHANNELS                */
    const float* initial;         /* n_channels initial values, copied at
                                   * open; NULL: all 0                      */
    double       lead;            /* the fraction of a period an event may
                                   * land early, (0, 1); 0: 0.5, the
                                   * nearest frame; YTL_LEAD_NONE: never
                                   * early. QUANTIZATION                    */
    ytl_key*   keys;            /* the key arena, key_capacity entries:
                                   * where a sequence keeps the keys of its
                                   * tweens (SEQUENCES); the caller's, used
                                   * while the handle is. NULL: none        */
    int          key_capacity;    /* >= 0                                   */
    ytl_target_slot* targets;     /* v0.6.0: the target pool, target_capacity
                                   * entries (TARGETS); the caller's, used
                                   * while the handle is. NULL: none        */
    int          target_capacity; /* >= 0                                   */
    ytl_tween_slot* tweens;       /* the target tween pool                  */
    int          tween_capacity;  /* >= 0                                   */
} ytl_desc;

/* The frame being drawn. */
typedef struct ytl_frame {
    int64_t onset;    /* predicted onset, RT ns; never decreasing           */
    int64_t period;   /* the frame period, ns, >= 0; 0 when there is none   */
    int64_t index;    /* the caller's frame number, recorded with landings  */
} ytl_frame;

/* Private. One time base. Every field an evaluate of a base at rate 1 reads
 * or writes is in the first 64 bytes; the rate's fields follow, read only
 * when `scaled` is set. */
typedef struct ytl__base {
    int64_t anchor_rt;     /* running: base time anchor_bt at RT anchor_rt  */
    int64_t anchor_bt;     /* paused: the frozen base time                  */
    int64_t last_hi;       /* the furthest window end evaluated (the reach) */
    int64_t last_now;      /* the furthest base time of an evaluated onset  */
    int64_t now_bt;        /* "now" for a tween: the last evaluated onset's
                            * base time, or what a later anchor, pause,
                            * resume or rate change set; not last_now, which
                            * only moves forward between rewinds          */
    int32_t begin;         /* first event of the base in the storage        */
    int32_t count;
    int32_t hi;            /* events (from begin) with time <= last_hi      */
    int32_t dirty;         /* lowest late event (from begin), or INT32_MAX  */
    uint8_t state;         /* 0 stopped, 1 running, 2 paused                */
    uint8_t evaluated;     /* last_hi is meaningful                         */
    uint8_t scaled;        /* the rate is not 1/1                           */
    int32_t clamped;       /* evaluates and pauses that clamped (RATE)      */
    int32_t num, den;      /* the rate, reduced by their gcd                */
    int64_t lim_n, lim_d;  /* (2^63 - 1 - 2^31) / num and / den: the quotient
                            * bounds that keep the scaled products from
                            * overflow (ytl__scale)                      */
} ytl__base;

/* Private. One channel's driver. */
typedef struct ytl__chan {
    ytl_track tr;          /* a copy; keys and curves are the caller's,
                              * unless `own` says they are ik and ic        */
    ytl_key   ik[3];       /* a tween's keys (3 with yoyo)                */
    ytl_curve ic;          /* a tween's curve, or its start slope         */
    ytl_tween_desc pend;   /* a tween waiting for its start, resolved:
                              * pend.start_ns is absolute, delay_ns 0     */
    int64_t  last_time;      /* (time, id) of the event the value is from   */
    int32_t  last_id;
    int32_t  seg;            /* the key before the last sample, -1 before
                              * the first                                   */
    int32_t  n_events;       /* events naming it                            */
    float    initial;
    uint8_t  base;           /* valid while n_events > 0 or tracked         */
    uint8_t  has_last;       /* last_time and last_id are set               */
    uint8_t  mark;           /* scratch: lost an event in this compaction   */
    uint8_t  own;            /* 1: a tween in ik and ic; 2: a block in the
                              * arena at aoff (SEQUENCES)                   */
    uint8_t  stepped;        /* only STEP segments: read at the lead window */
    uint8_t  has_track;      /* tr (or ik) holds a track to sample          */
    uint8_t  has_pend;       /* pend is waiting                             */
    uint8_t  pend_stepped;   /* pend will be a STEP track                   */
    uint8_t  pend_arena;     /* the waiting driver is the arena block at
                              * poff, not pend (pend.start_ns its start)   */
    /* SEQUENCES, after the fields an evaluate reads for every track */
    int32_t  aoff;           /* own == 2: the track's keys in the arena     */
    int32_t  poff;           /* pend_arena: the waiting block's keys        */
    uint32_t seq_id;         /* the sequence that built the arena block     */
} ytl__chan;

/* Handle. The caller allocates it and treats every field as opaque.
 * ytl_open() resets it, so it may be reused. */
typedef struct ytl_timeline {
    ytl_event* events;
    int32_t      n_events;
    int32_t      capacity;
    int32_t      next_id;
    int32_t      n_channels;
    double       lead;
    int64_t      last_onset;
    bool         has_onset;
    bool         open;
    ytl__base  bases[YTL_MAX_BASES];
    float        values[YTL_MAX_CHANNELS];   /* contiguous, for upload    */
    uint8_t      track_base[YTL_MAX_CHANNELS]; /* base + 1 of the channel's
                                                * track, 0 for none: the
                                                * evaluate scans this, not
                                                * the records              */
    ytl__chan  chans[YTL_MAX_CHANNELS];
    ytl_key*   arena;          /* desc.keys (SEQUENCES)                     */
    int32_t      arena_cap;
    int32_t      arena_top;      /* slots in use, garbage included            */
    uint32_t     seq_next;       /* the last sequence id handed out           */
    ytl_target_slot* tg;         /* desc.targets (TARGETS), dense: n_tg used  */
    int32_t      tg_cap;
    int32_t      n_tg;
    ytl_tween_slot* tw;          /* desc.tweens                               */
    int32_t      tw_cap;
    int32_t      n_tw;
    int32_t      tw_free;        /* the free list, through next               */
    int32_t      tw_next_id;
    int32_t      due_head;       /* tweens whose callbacks this evaluate fired */
    int32_t      due_tail;
    bool         in_calls;       /* ytl_evaluate() is calling the callbacks   */
    char         error[192];
} ytl_timeline;

/* --- sequences (SEQUENCES) --------------------------------------------- */

/* A cursor on one base, for building a trial in the order it plays, as a
 * GSAP timeline is built. Every call lowers to events and to one keyed
 * track per tweened channel, at absolute base times; the timing is the
 * core's. After the first failing call, `err` holds its code and
 * `err_call` its index, and every later call does nothing. */
typedef struct ytl_seq {
    ytl_timeline* tl;
    int64_t  t;          /* the cursor, base time; store it to use as a label */
    int      base;
    int      err;        /* 0, or the first error                            */
    int      err_call;   /* the index of the call (or ytl_run's op) that set
                          * err; -1 for the constructor                     */
    int      n_calls;
    int32_t  id0;        /* the first event id the sequence can have added   */
    uint32_t id;
    int64_t  prev_start; /* v0.6.0: the start and end of what the last call
                          * added (GSAP "<" and ">"); the cursor before any */
    int64_t  prev_end;
} ytl_seq;

/* An op of a sequence table, for ytl_run(). Zero is no op: a zeroed
 * entry is YTL_ERR_ARG. Build them with the YTL_ON() ... macros. */
enum {
    YTL_OP_ON = 1,     /* ch                                              */
    YTL_OP_OFF,        /* ch                                              */
    YTL_OP_SET_VALUE,  /* ch, value                                       */
    YTL_OP_TRIGGER,    /* code                                            */
    YTL_OP_MARK,       /* code, user                                      */
    YTL_OP_WAIT,       /* t: a duration                                   */
    YTL_OP_AT,         /* t: a base time                                  */
    YTL_OP_TO,         /* ch, tween                                       */
    YTL_OP_THEN        /* ch, tween                                       */
};

typedef struct ytl_op {
    int              op;
    int              ch;
    int64_t          t_ns;     /* WAIT: a duration; AT: a base time */
    int32_t          code;
    float            value;
    uint64_t         user;
    ytl_tween_desc tween;
} ytl_op;

/* Designated initializers in declaration order, so they also compile as
 * C++20 (where g++ wants -Wno-missing-field-initializers). */
#define YTL_ON(c)            { .op = YTL_OP_ON, .ch = (c) }
#define YTL_OFF(c)           { .op = YTL_OP_OFF, .ch = (c) }
#define YTL_SET_VALUE(c, v)  { .op = YTL_OP_SET_VALUE, .ch = (c), .value = (v) }
#define YTL_TRIGGER(k)       { .op = YTL_OP_TRIGGER, .code = (k) }
#define YTL_MARK(k, u)       { .op = YTL_OP_MARK, .code = (k), .user = (u) }
#define YTL_WAIT(dt)         { .op = YTL_OP_WAIT, .t_ns = (dt) }
#define YTL_AT(bt)           { .op = YTL_OP_AT, .t_ns = (bt) }
#define YTL_TO(c, ...)       { .op = YTL_OP_TO, .ch = (c), .tween = { __VA_ARGS__ } }
#define YTL_THEN(c, ...)     { .op = YTL_OP_THEN, .ch = (c), .tween = { __VA_ARGS__ } }
#define YTL_COUNT(a)         ((int)(sizeof(a) / sizeof((a)[0])))

/* --- lifecycle --------------------------------------------------------- */

/* Validate `desc` and reset the handle: no events, base 0 running and the
 * others stopped, channels at their initial values, the message cleared.
 * Returns false with ytl_error() set on a bad or NULL desc, and the handle
 * closed. */
YTL_API bool ytl_open(ytl_timeline* tl, const ytl_desc* desc);

/* End the use of the handle (ERRORS AND CLOSE). It owns nothing, so this
 * marks it closed and keeps the message. Safe on a zeroed handle, after a
 * failed open, and twice; NULL is a no-op. */
YTL_API void ytl_close(ytl_timeline* tl);

/* The message of the last failure ("" after a successful open). */
YTL_API const char* ytl_error(const ytl_timeline* tl);
YTL_API bool        ytl_is_open(const ytl_timeline* tl);

/* --- time bases -------------------------------------------------------- */

/* Run base `base` (1 .. YTL_MAX_BASES-1) with base time `base_time_ns` at
 * RT time `rt_ns`, from any state. A base time at or before the furthest
 * base time evaluated on it rewinds it (REWIND). Returns 0, YTL_ERR_ARG
 * (base 0 or out of range, |base_time_ns| >= 2^62) or YTL_ERR_CLOSED.
 * Pause, resume and stop also return YTL_ERR_ARG for base 0. */
YTL_API int ytl_anchor(ytl_timeline* tl, int base, int64_t rt_ns, int64_t base_time_ns);

/* Freeze a running base at its time at RT time `rt_ns`. YTL_ERR_ORDER
 * when the base is not running. */
YTL_API int ytl_pause(ytl_timeline* tl, int base, int64_t rt_ns);

/* Run a paused base again from its frozen time at RT time `rt_ns`.
 * YTL_ERR_ORDER when the base is not paused. */
YTL_API int ytl_resume(ytl_timeline* tl, int base, int64_t rt_ns);

/* Stop a base: nothing on it fires, rewinds or writes a channel until it is
 * anchored again. */
YTL_API int ytl_stop(ytl_timeline* tl, int base);

/* Seek base `base` to `base_time_ns` at RT time `rt_ns` without firing
 * what lies between (SKIP): ytl_anchor(), then every pending event of the
 * base before base_time_ns is marked YTL_EV_SKIPPED and never fires. Returns the
 * number skipped (>= 0), or what ytl_anchor() returns on an error, with
 * nothing changed. */
YTL_API int ytl_skip(ytl_timeline* tl, int base, int64_t rt_ns, int64_t base_time_ns);

/* The base's time at RT time `rt_ns` into *out, and true, when the base is
 * running or paused (a paused base gives its frozen time); false when it is
 * stopped or the arguments are bad. */
YTL_API bool ytl_base_time(const ytl_timeline* tl, int base, int64_t rt_ns, int64_t* out);

/* From RT time `rt_ns` on, run `base` (1 .. YTL_MAX_BASES-1) at num/den
 * base ns per RT ns (RATE). num and den are in [1, 2^31 - 1] and are stored
 * reduced by their gcd. A running base continues from its time there with
 * no jump and no rewind; a paused or stopped base keeps the rate for its
 * next resume or anchor; the rate it has already changes nothing. GSAP's
 * movie.timeScale(0.5) is ytl_rate(&tl, MOVIE, next_onset, 1, 2).
 * Returns 0, YTL_ERR_ARG (base 0 or out of range, num or den < 1, |rt_ns|
 * >= 2^62, or a running base whose time at rt_ns is at or past +-2^62) or
 * YTL_ERR_CLOSED. */
YTL_API int ytl_rate(ytl_timeline* tl, int base, int64_t rt_ns, int32_t num, int32_t den);

/* The base's rate, reduced, into *num and *den, and true; base 0 is 1/1, and
 * a stopped base has the rate it will run at. False on bad arguments. */
YTL_API bool ytl_get_rate(const ytl_timeline* tl, int base, int32_t* num, int32_t* den);

/* The first RT time at which the running base reaches base time `bt_ns`,
 * into *out, and true (RATE): the base has reached bt_ns at RT time t
 * exactly when t >= *out. False when the base is paused or stopped,
 * |bt_ns| >= 2^62, the result is at or past +-2^62, or the arguments are
 * bad. */
YTL_API bool ytl_rt_time(const ytl_timeline* tl, int base, int64_t bt_ns, int64_t* out);

/* The base time frame f's window reaches on `base`, into *end, and true
 * (QUANTIZATION): the base's time at RT time f->onset + lead x f->period,
 * truncated to whole ns, or the frozen time of a paused base. An event at
 * or before *end that has not fired lands on f. ytl_evaluate() uses the
 * same function. False when the base is stopped, f is NULL, its period is
 * negative, the result is at or past +-2^62, or the arguments are bad.
 * Changes nothing. */
YTL_API bool ytl_window(const ytl_timeline* tl, int base, const ytl_frame* f, int64_t* end);

/* How many evaluates and pauses of `base` found its time at or past
 * +-2^62 and used +-2^62 in its place (RATE); 0 while every time stays in
 * range. YTL_ERR_ARG for a bad base, YTL_ERR_CLOSED. Not const: a failure
 * writes the message. */
YTL_API int ytl_clamped(ytl_timeline* tl, int base);

/* --- events ------------------------------------------------------------ */

/* Add one event (EVENTS). Returns its id (>= 0) or YTL_ERR_ARG (bad base,
 * kind, channel or value), YTL_ERR_BOUND, YTL_ERR_FULL. */
YTL_API int ytl_add(ytl_timeline* tl, const ytl_event* e);

/* Add n events, all or none. Returns the first id (the others follow), or
 * an error as ytl_add() with nothing added. n = 0 returns the next id. */
YTL_API int ytl_add_n(ytl_timeline* tl, const ytl_event* e, int n);

/* Delete one event by id. Returns 0 or YTL_ERR_NOT_FOUND. */
YTL_API int ytl_remove(ytl_timeline* tl, int id);

/* Delete every fired or skipped event of `base` (or YTL_ALL_BASES).
 * Returns the number deleted. Channel values stay. */
YTL_API int ytl_prune(ytl_timeline* tl, int base);

/* Delete every event of `base` (or YTL_ALL_BASES) and the keys of the
 * channels bound to it; those channels return to their initial values.
 * Returns the number of events deleted. */
YTL_API int ytl_clear(ytl_timeline* tl, int base);

/* --- tracks ------------------------------------------------------------ */

/* Drive `channel` from keys[0..n) on `base`: ytl_set_track() with only
 * keys and n_keys set (no curves, no repeat); n = 0 removes the track. The
 * value changes at the next evaluate, or at once to the initial value when
 * the track is removed. */
YTL_API int ytl_set_keys(ytl_timeline* tl, int channel, int base,
                             const ytl_key* keys, int n);

/* Drive `channel` from `tr` on `base` (TRACKS), replacing any track it
 * had; tr NULL removes it. ytl_set_keys() is this with keys only. The
 * struct is copied; the arrays it points at stay the caller's. Returns 0,
 * YTL_ERR_ARG (TRACKS lists what is checked) or YTL_ERR_BOUND (the
 * channel has events). */
YTL_API int ytl_set_track(ytl_timeline* tl, int channel, int base,
                              const ytl_track* tr);

/* Tween `channel` on `base` as `d` says (TWEENS). The tween waits until its
 * start while the channel's track runs on, then takes over from the value
 * the track had there. Its keys live in the handle. Returns 0,
 * YTL_ERR_ARG (TWEENS lists what is checked) or YTL_ERR_BOUND (the
 * channel has events). */
YTL_API int ytl_tween(ytl_timeline* tl, int channel, int base,
                          const ytl_tween_desc* d);

/* --- sequences --------------------------------------------------------- */

/* A sequence on `base` with its cursor at base time 0, or at the base's
 * "now" (TWEENS). A closed handle or a bad base sets err (err_call -1). */
YTL_API ytl_seq ytl_seq_on(ytl_timeline* tl, int base);
YTL_API ytl_seq ytl_seq_now(ytl_timeline* tl, int base);

/* Move the cursor by dt_ns, or to base time t_ns. */
YTL_API void ytl_wait(ytl_seq* q, int64_t dt_ns);
YTL_API void ytl_at(ytl_seq* q, int64_t t_ns);

/* An event at the cursor: ONSET, OFFSET, SET, TRIGGER or MARK. */
YTL_API void ytl_on(ytl_seq* q, int ch);
YTL_API void ytl_off(ytl_seq* q, int ch);
YTL_API void ytl_set_value(ytl_seq* q, int ch, float value);
YTL_API void ytl_trigger(ytl_seq* q, int32_t code);
YTL_API void ytl_mark(ytl_seq* q, int32_t code, uint64_t user);

/* ONSET on chans[i] at the cursor + i x stagger_ns, all or none. */
YTL_API void ytl_on_n(ytl_seq* q, const int* chans, int n, int64_t stagger_ns);

/* A tween on `ch` from the cursor + d->delay_ns (SEQUENCES); the cursor
 * stays. Returns the end, start + duration_ns (x 2 with yoyo) x cycles, or the cursor
 * on an error. ytl_then() is ytl_to() and a wait to that end. */
YTL_API int64_t ytl_to(ytl_seq* q, int ch, const ytl_tween_desc* d);
YTL_API void    ytl_then(ytl_seq* q, int ch, const ytl_tween_desc* d);

/* Keys on `ch`: values[i] at the cursor + offsets_ns[i], offsets rising
 * from >= 0, each segment eased by `ease` (not BEZIER). Returns the last
 * key's time, or the cursor on an error; the cursor stays. */
YTL_API int64_t ytl_keyframes(ytl_seq* q, int ch, const float* values,
                                  const int64_t* offsets_ns, int n, int ease);

/* Apply ops[0..n) in order, all or none: the whole table is checked first
 * (ytl_check_ops, from the sequence's cursor and blocks), and on a failure
 * err and err_call (the op's index) are set and nothing changes. Returns 0
 * or the error. */
YTL_API int ytl_run(ytl_seq* q, const ytl_op* ops, int n);

/* Check ops[0..n) as ytl_run() on a new sequence on `base` would, and
 * change nothing but the message on a failure: for a pack's table, when it
 * loads. Returns 0, or the first error with *bad set to its op's index. */
YTL_API int ytl_check_ops(ytl_timeline* tl, int base, const ytl_op* ops,
                              int n, int* bad);

/* Undo what the sequence added: every event of its base with an id at or
 * past the sequence's first, and the arena blocks it built (SEQUENCES).
 * Returns the number of events deleted. The sequence does nothing more. */
YTL_API int ytl_seq_cancel(ytl_seq* q);

/* --- targets (TARGETS) ------------------------------------------------- */

/* A tween of a field of the caller, at the sequence's cursor plus
 * a->delay_ns; the cursor stays. Returns the tween's id (>= 0), or the
 * error, which is also the sequence's sticky error: YTL_ERR_ARG (TARGETS
 * lists what is checked), YTL_ERR_BOUND (the field is a target on another
 * base), YTL_ERR_FULL (the target or tween pool, or the event storage for
 * the callbacks; the message names it). */
YTL_API int ytl_animate(ytl_seq* q, const ytl_anim* a);

/* The same tween on n fields, field i at (char*)a->target + i x stride,
 * starting i x a->stagger_ns later. All or none. Returns the first id; the
 * others follow in order. */
YTL_API int ytl_animate_n(ytl_seq* q, const ytl_anim* a, size_t stride, int n);

/* One float, GSAP's to(), from() and fromTo(): ytl_animate() with to,
 * from or both set; from and from_to also set `immediate`. */
YTL_API int ytl_tween_to(ytl_seq* q, float* field, float to, int64_t duration_ns, int ease);
YTL_API int ytl_tween_from(ytl_seq* q, float* field, float from, int64_t duration_ns, int ease);
YTL_API int ytl_tween_from_to(ytl_seq* q, float* field, float from, float to,
                              int64_t duration_ns, int ease);

/* GSAP's set(): `field` becomes `value` at the cursor, on the frame where
 * an event at that time lands (a STEP tween of duration 0). */
YTL_API int ytl_tween_set(ytl_seq* q, float* field, float value);

/* Kill every tween of the target at `field` (GSAP's killTweensOf), or the
 * tween `id`: their callback events go, the field keeps its value, and a
 * target with no tween left is freed, so the timeline no longer writes the
 * field. ytl_kill() returns the number killed (0 when the field is not a
 * target); ytl_kill_tween() returns 0 or YTL_ERR_NOT_FOUND. */
YTL_API int ytl_kill(ytl_timeline* tl, const void* field);
YTL_API int ytl_kill_tween(ytl_timeline* tl, int id);

/* Live target tweens, and the targets in *n_targets when it is not NULL. */
YTL_API int ytl_tween_count(const ytl_timeline* tl, int* n_targets);

/* --- the frame --------------------------------------------------------- */

/* Evaluate the frame (EVALUATE). Returns the number of events that fired,
 * the first min(that, cap) copied into fired[] in RT target order, or
 * YTL_ERR_ARG (period < 0, cap < 0, fired NULL with cap > 0) or
 * YTL_ERR_ORDER (onset below the last one). */
YTL_API int ytl_evaluate(ytl_timeline* tl, const ytl_frame* f,
                             ytl_event* fired, int cap);

/* The earliest RT time of a pending event on a running base, and true; a
 * late event's time is in the past. False when nothing is pending. */
YTL_API bool ytl_next_due(const ytl_timeline* tl, int64_t* rt_ns);

/* The pending events of `base` (or YTL_ALL_BASES) whose RT time is in
 * [from_rt_ns, to_rt_ns), on running bases only, without changing anything
 * but the message on a failure (PEEK). Returns how many; the first
 * min(that, cap) are copied into out[] in RT time order, then id, with
 * onset = the RT time, residual 0 and frame -1. YTL_ERR_ARG for a bad base, cap < 0, out NULL with cap > 0,
 * from_rt_ns > to_rt_ns, or a bound at or past +-2^62. */
YTL_API int ytl_peek(ytl_timeline* tl, int base, int64_t from_rt_ns, int64_t to_rt_ns,
                         ytl_event* out, int cap);

/* The lead in use (QUANTIZATION): 0.5 for a desc lead of 0, 0 for
 * YTL_LEAD_NONE, else the desc's. A window ends lead x period, truncated
 * to whole ns, after the onset. 0 when the handle is not open. */
YTL_API double ytl_lead(const ytl_timeline* tl);

/* --- state and record -------------------------------------------------- */

/* The channel values, n_channels floats, into the handle. NULL when the
 * handle is not open. */
YTL_API const float* ytl_values(const ytl_timeline* tl);
YTL_API float        ytl_value(const ytl_timeline* tl, int channel);

/* The events of `base` (YTL_ALL_BASES: all, grouped by base), sorted by
 * time then id, with their landing fields: the record. The pointer is into
 * the storage and moves with the next add or delete. */
YTL_API const ytl_event* ytl_events(const ytl_timeline* tl, int base, int* n);

/* The event with this id, or NULL. A linear scan. */
YTL_API const ytl_event* ytl_find(const ytl_timeline* tl, int id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_TIMELINE_H_INCLUDED */

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_TIMELINE_IMPLEMENTATION
#ifndef YSP_TIMELINE_IMPLEMENTATION_GUARD
#define YSP_TIMELINE_IMPLEMENTATION_GUARD

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define YTL__STOPPED 0
#define YTL__RUNNING 1
#define YTL__PAUSED  2
#define YTL__NONE    INT32_MAX
#define YTL__LIMIT   (INT64_C(1) << 62)
#define YTL__NS_PER_S INT64_C(1000000000)
#define YTL__QMAX    (INT64_MAX - (INT64_C(1) << 31))   /* RATE: ytl__scale */

/* The rate-1 base time must inline into the evaluate (RATE). gcc does so
 * with `inline`; MSVC 19.44 at /O2 did not, even with __inline, and its
 * call cost about 4 ns per base per frame (docs/timeline.md). */
#if defined(_MSC_VER)
#define YTL__INLINE __forceinline
#else
#define YTL__INLINE inline
#endif

/* An event the base has passed: fired, or skipped by ytl_skip(). Every
 * rule that asks "is it still to come" asks this, so a skipped event is
 * never fired late and counts for its channel. */
#define YTL__DONE    (YTL_EV_FIRED | YTL_EV_SKIPPED)

/* A tween with keep_velocity: a cubic Hermite segment whose start tangent
 * is curves[0].x1 and whose end tangent is 0. Never accepted from a
 * caller's keys, which stop at YTL_EASE_BEZIER. */
#define YTL__EASE_HERMITE 255

/* The last public ytl_ease: every check of an ease reads this. */
#define YTL__EASE_LAST YTL_EASE_CUBIC_IN_OUT

/* isfinite() is C99 and MSVC's default C dialect does not declare it, so the
 * test is written in comparisons a NaN loses on its own. */
static bool ytl__finite(double x) {
    return x > -HUGE_VAL && x < HUGE_VAL;
}

YTL_API const char* ytl_strerror(int code) {
    switch (code) {
    case YTL_ERR_ARG:       return "invalid argument";
    case YTL_ERR_CLOSED:    return "timeline is not open";
    case YTL_ERR_ORDER:     return "call out of order";
    case YTL_ERR_FULL:      return "event storage is full";
    case YTL_ERR_BOUND:     return "channel is bound elsewhere";
    case YTL_ERR_NOT_FOUND: return "no event with that id";
    default:                  return code >= 0 ? "ok" : "unknown error";
    }
}

YTL_API const char* ytl_version(void) { return YTL_VERSION_STRING; }

YTL_API int64_t ytl_ns(double seconds) {
    double ns = seconds * 1e9;
    if (!(ns == ns)) return 0;
    if (ns >= (double)YTL__LIMIT) return YTL__LIMIT;
    if (ns <= -(double)YTL__LIMIT) return -YTL__LIMIT;
    return ns >= 0.0 ? (int64_t)(ns + 0.5) : -(int64_t)(-ns + 0.5);
}

static bool ytl__open_fail(ytl_timeline* tl, const char* fmt, ...) {
    va_list ap;
    int n;
    n = snprintf(tl->error, sizeof(tl->error), "ysp_timeline: ");
    if (n < 0) n = 0;
    va_start(ap, fmt);
    vsnprintf(tl->error + n, sizeof(tl->error) - (size_t)n, fmt, ap);
    va_end(ap);
    tl->open = false;
    return false;
}

/* The message of a failed call, written on the failure path only, so a
 * call that succeeds costs one compare. */
static int ytl__err(ytl_timeline* tl, int rc, const char* what) {
    if (rc < 0 && tl) snprintf(tl->error, sizeof(tl->error), "ysp_timeline: %s: %s", what, ytl_strerror(rc));
    return rc;
}

/* --- tracks ------------------------------------------------------------ */

/* A cubic Bezier coordinate with end points 0 and 1. */
static double ytl__bez(double p1, double p2, double s) {
    double r = 1.0 - s;
    return 3.0 * r * r * s * p1 + 3.0 * r * s * s * p2 + s * s * s;
}

/* The curve's y where its x is u. x(s) is monotone because x1 and x2 are in
 * [0, 1]. Newton from s = u converges in a few steps on ordinary curves;
 * bisection takes over when a step leaves [0, 1] or the slope vanishes. */
static double ytl__bezier(const ytl_curve* c, double u) {
    double s = u, x, d, r, lo = 0.0, hi = 1.0;
    int i;
    for (i = 0; i < 8; i++) {
        x = ytl__bez(c->x1, c->x2, s) - u;
        if (x > -1e-12 && x < 1e-12) return ytl__bez(c->y1, c->y2, s);
        r = 1.0 - s;
        d = 3.0 * r * r * c->x1 + 6.0 * r * s * (c->x2 - c->x1) + 3.0 * s * s * (1.0 - c->x2);
        if (d > -1e-9 && d < 1e-9) break;
        s -= x / d;
        if (!(s >= 0.0 && s <= 1.0)) break;
    }
    for (i = 0; i < 60; i++) {
        s = 0.5 * (lo + hi);
        x = ytl__bez(c->x1, c->x2, s) - u;
        if (x > -1e-12 && x < 1e-12) break;
        if (x < 0.0) lo = s;
        else hi = s;
    }
    return ytl__bez(c->y1, c->y2, s);
}

/* Curve i of a track's table, or, with curves NULL, of the curve slots
 * that follow an arena block's keys (SEQUENCES), copied out so that it is
 * never read through a ytl_key. A caller's track with a BEZIER key always
 * has a table (set_track checks it), so NULL is free to mean the arena. Only
 * the BEZIER and HERMITE cases look a curve up: threading the arena through
 * every key lookup measured 21 to 28% slower on 32 keyed channels
 * (docs/timeline.md). */
static ytl_curve ytl__curve(const ytl_curve* curves, const ytl_key* cslots, int i) {
    ytl_curve c;
    if (curves) c = curves[i];
    else memcpy(&c, &cslots[i], sizeof c);
    return c;
}

/* e(u) of the eases whose value is v0 + (v1 - v0) e(u). One function for
 * the keys and the target tweens, so the two give the same bits. */
static double ytl__shape(int ease, double u) {
    double r;
    switch (ease) {
    case YTL_EASE_COSINE:       return 0.5 - 0.5 * cos(3.14159265358979323846 * u);
    case YTL_EASE_QUAD_IN:      return u * u;
    case YTL_EASE_QUAD_OUT:     return 1.0 - (1.0 - u) * (1.0 - u);
    case YTL_EASE_QUAD_IN_OUT:
        r = 1.0 - u;
        return u < 0.5 ? 2.0 * u * u : 1.0 - 2.0 * r * r;
    case YTL_EASE_CUBIC_IN:     return u * u * u;
    case YTL_EASE_CUBIC_OUT:
        r = 1.0 - u;
        return 1.0 - r * r * r;
    case YTL_EASE_CUBIC_IN_OUT:
        r = 1.0 - u;
        return u < 0.5 ? 4.0 * u * u * u : 1.0 - 4.0 * r * r * r;
    default:                    return u;
    }
}

/* For HERMITE the curve's x1 is the start tangent. cslots is the end of
 * the keys, read only with curves NULL. */
static double ytl__ease(const ytl_key* a, const ytl_curve* curves, const ytl_key* cslots,
                          double v0, double v1, double u) {
    ytl_curve c;
    switch (a->ease) {
    case YTL_EASE_STEP:     return v0;
    case YTL_EASE_LOG:      return v0 * exp(u * log(v1 / v0));
    case YTL_EASE_BEZIER:
        c = ytl__curve(curves, cslots, a->curve);
        return v0 + (v1 - v0) * ytl__bezier(&c, u);
    case YTL__EASE_HERMITE: {
        double u2 = u * u, u3 = u2 * u;
        c = ytl__curve(curves, cslots, a->curve);
        return (2.0 * u3 - 3.0 * u2 + 1.0) * v0 + (u3 - 2.0 * u2 + u) * c.x1
               + (3.0 * u2 - 2.0 * u3) * v1;
    }
    default:                  return v0 + (v1 - v0) * ytl__shape(a->ease, u);
    }
}

/* The last key with time <= t, or -1. */
static int ytl__find_key(const ytl_key* keys, int n, int64_t t) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (keys[mid].time_ns <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo - 1;
}

static double ytl__keys_at(const ytl_key* keys, int n, const ytl_curve* curves,
                             int i, int64_t t) {
    const ytl_key* a;
    const ytl_key* b;
    if (i < 0) return keys[0].value;
    if (i >= n - 1) return keys[n - 1].value;
    a = &keys[i];
    b = &keys[i + 1];
    return ytl__ease(a, curves, keys + n, a->value, b->value,
                       (double)(t - a->time_ns) / (double)(b->time_ns - a->time_ns));
}

/* Sample k is at t0 + k * 1e9 / rate exactly. A step in whole ns would
 * not do: at 120 samples/s it is a third of a ns short, 24 us by the end of
 * 10 minutes, which measured as a tenfold larger error on a moving target.
 * Splitting d into seconds and ns keeps every product below 2^63 for
 * rate <= 1e9: a * rate <= 4.7e18 and b * rate < 1e18. */
static double ytl__samples_at(const ytl_track* tr, int64_t t) {
    const float* v = tr->samples;
    int n = tr->n_samples, i;
    int64_t d, a, b, k, rem;
    double u, p0, p1, p2, p3;
    if (t <= tr->t0_ns) return v[0];
    d = t - tr->t0_ns;
    a = d / YTL__NS_PER_S;
    b = d % YTL__NS_PER_S;
    k = a * tr->rate + (b * tr->rate) / YTL__NS_PER_S;
    if (k >= n - 1) return v[n - 1];
    i = (int)k;
    if (tr->interp == YTL_INTERP_STEP) return v[i];
    rem = (b * tr->rate) % YTL__NS_PER_S;
    u = (double)rem / (double)YTL__NS_PER_S;
    p1 = v[i];
    p2 = v[i + 1];
    if (tr->interp != YTL_INTERP_CUBIC) return p1 + (p2 - p1) * u;
    /* Past an end, a sample extrapolated in a line: repeating the end
     * sample instead flattens the slope there, which measured 3 times the
     * error in the end segments on a sum of sines. */
    p0 = i > 0 ? v[i - 1] : 2.0 * p1 - p2;
    p3 = i + 2 < n ? v[i + 2] : 2.0 * p2 - p1;
    return p1 + 0.5 * u * (p2 - p0 + u * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3
                                          + u * (3.0 * (p1 - p2) + p3 - p0)));
}

/* Fold t into the track's cycle. After the last cycle the track holds its
 * value at the end of a cycle, read without folding. */
static int64_t ytl__cycle(const ytl_track* tr, int64_t start, int64_t t) {
    int64_t q;
    if (tr->period_ns <= 0 || t <= start) return t;
    q = (t - start) / tr->period_ns;
    if (tr->repeats > 0 && q >= tr->repeats) return start + tr->period_ns;
    return start + (t - start) % tr->period_ns;
}

/* `keys` and `curves` come apart from tr because a tween's live in the
 * channel. `seg` caches the key segment: O(1) while time moves forward a
 * segment at a time, a binary search after a jump or a cycle's wrap. */
static double ytl__track_at(const ytl_track* tr, const ytl_key* keys,
                              const ytl_curve* curves, int64_t t, int32_t* seg) {
    if (keys) {
        int n = tr->n_keys, i;
        bool ok;
        t = ytl__cycle(tr, keys[0].time_ns, t);
        if (!seg) return ytl__keys_at(keys, n, curves, ytl__find_key(keys, n, t), t);
        i = *seg;
        ok = (i < 0 ? t < keys[0].time_ns : keys[i].time_ns <= t)
             && (i + 1 >= n || t < keys[i + 1].time_ns);
        if (!ok && i + 2 <= n && keys[i + 1].time_ns <= t
            && (i + 2 >= n || t < keys[i + 2].time_ns)) {
            i++;
            ok = true;
        }
        if (!ok) i = ytl__find_key(keys, n, t);
        *seg = i;
        return ytl__keys_at(keys, n, curves, i, t);
    }
    if (tr->samples && tr->n_samples > 0)
        return ytl__samples_at(tr, ytl__cycle(tr, tr->t0_ns, t));
    return 0.0;
}

YTL_API double ytl_sample(const ytl_track* tr, int64_t t) {
    if (!tr) return 0.0;
    return ytl__track_at(tr, tr->keys && tr->n_keys > 0 ? tr->keys : NULL,
                           tr->curves, t, NULL);
}

/* The keys of a channel's track: the caller's array, a tween's in the
 * record, or a block in the arena (with curves NULL: its curve slots
 * follow its keys). */
static const ytl_key* ytl__chan_keys(const ytl_timeline* tl, const ytl__chan* c,
                                         const ytl_curve** curves) {
    if (c->own == 2) {
        *curves = NULL;
        return tl->arena + c->aoff;
    }
    *curves = c->own ? &c->ic : c->tr.curves;
    return c->own ? c->ik : c->tr.keys;
}

static float ytl__sample_chan(const ytl_timeline* tl, ytl__chan* c, int64_t t) {
    const ytl_curve* curves;
    const ytl_key* keys = ytl__chan_keys(tl, c, &curves);
    return (float)ytl__track_at(&c->tr, keys, curves, t, &c->seg);
}

/* --- lifecycle --------------------------------------------------------- */

YTL_API bool ytl_open(ytl_timeline* tl, const ytl_desc* d) {
    int i;
    if (!tl) return false;
    memset(tl, 0, sizeof(*tl));
    if (!d) return ytl__open_fail(tl, "null desc");
    if (d->event_capacity < 0) return ytl__open_fail(tl, "desc.event_capacity is negative");
    if (d->event_capacity > 0 && !d->events)
        return ytl__open_fail(tl, "desc.events is NULL with event_capacity %d", d->event_capacity);
    if (d->key_capacity < 0) return ytl__open_fail(tl, "desc.key_capacity is negative");
    if (d->key_capacity > 0 && !d->keys)
        return ytl__open_fail(tl, "desc.keys is NULL with key_capacity %d", d->key_capacity);
    if (d->n_channels < 0 || d->n_channels > YTL_MAX_CHANNELS)
        return ytl__open_fail(tl, "desc.n_channels must be in [0, %d]", YTL_MAX_CHANNELS);
    if (d->target_capacity < 0 || (d->target_capacity > 0 && !d->targets))
        return ytl__open_fail(tl, "desc.targets is NULL or target_capacity is negative");
    if (d->tween_capacity < 0 || (d->tween_capacity > 0 && !d->tweens))
        return ytl__open_fail(tl, "desc.tweens is NULL or tween_capacity is negative");
    if (!(d->lead == YTL_LEAD_NONE || (d->lead >= 0.0 && d->lead < 1.0)))
        return ytl__open_fail(tl, "desc.lead must be in [0, 1) or YTL_LEAD_NONE");
    if (d->initial) {
        for (i = 0; i < d->n_channels; i++)
            if (!ytl__finite(d->initial[i]))
                return ytl__open_fail(tl, "desc.initial[%d] is not finite", i);
    }
    tl->events = d->events;
    tl->capacity = d->event_capacity;
    tl->arena = d->keys;
    tl->arena_cap = d->key_capacity;
    tl->n_channels = d->n_channels;
    tl->lead = d->lead == 0.0 ? 0.5 : d->lead == YTL_LEAD_NONE ? 0.0 : d->lead;
    for (i = 0; i < d->n_channels && i < YTL_MAX_CHANNELS; i++) {
        float v = d->initial ? d->initial[i] : 0.0f;
        tl->chans[i].initial = v;
        tl->chans[i].seg = -1;
        tl->values[i] = v;
    }
    for (i = 0; i < YTL_MAX_BASES; i++) {
        tl->bases[i].dirty = YTL__NONE;
        tl->bases[i].num = 1;
        tl->bases[i].den = 1;
        tl->bases[i].lim_n = YTL__QMAX;
        tl->bases[i].lim_d = YTL__QMAX;
    }
    tl->bases[YTL_BASE_RT].state = YTL__RUNNING;
    /* The pools are the caller's and may hold anything: every tween slot is
     * marked free, so a stale callback event can never name a live one. */
    tl->tg = d->targets;
    tl->tg_cap = d->target_capacity;
    tl->tw = d->tweens;
    tl->tw_cap = d->tween_capacity;
    tl->tw_free = d->tween_capacity > 0 ? 0 : -1;
    for (i = 0; i < d->tween_capacity; i++) {
        tl->tw[i].flags = 0;
        tl->tw[i].due = 0;
        tl->tw[i].next = i + 1 < d->tween_capacity ? i + 1 : -1;
    }
    tl->due_head = -1;
    tl->due_tail = -1;
    tl->open = true;
    return true;
}

YTL_API const char* ytl_error(const ytl_timeline* tl) {
    return tl ? tl->error : "ysp_timeline: null handle";
}

YTL_API bool ytl_is_open(const ytl_timeline* tl) { return tl && tl->open; }

/* The handle owns nothing, so there is nothing to release; the message
 * stays for a log written after the close. */
YTL_API void ytl_close(ytl_timeline* tl) {
    if (tl) tl->open = false;
}

/* --- time bases -------------------------------------------------------- */

static bool ytl__user_base(const ytl_timeline* tl, int base) {
    return tl && tl->open && base > YTL_BASE_RT && base < YTL_MAX_BASES;
}

/* floor(d * a / b) for a and b in [1, 2^31). With d = q b + r and
 * 0 <= r < b it is q a + floor(r a / b): r a < 2^62 always, and q is held
 * to [-lim - 1, lim] with lim = (2^63 - 1 - 2^31) / a, so neither q a nor
 * the sum can overflow. Past that bound the result is more than 2^63 - 2^32
 * from 0, and added to an anchor's time (below 2^62) it is out of range:
 * *sat is set. The bound is not 2^62 / a because the anchor's time can
 * bring a product beyond 2^62 back into range. C99's / truncates, so a
 * negative remainder moves q down by one: truncation would map d = -1 and
 * d = 1 both to 0 at rate 1/2, and base time would not be monotone. */
static int64_t ytl__scale(int64_t d, int64_t a, int64_t b, int64_t lim, bool* sat) {
    int64_t q = d / b, r = d % b;
    if (r < 0) {
        q--;
        r += b;
    }
    if (q > lim) {
        *sat = true;
        return YTL__LIMIT;
    }
    if (q < -lim - 1) {
        *sat = true;
        return -YTL__LIMIT;
    }
    return q * a + r * a / b;
}

/* a + x, or +-2^62 with *sat set when it is at or past +-2^62. With
 * |a| <= 2^62 neither test can overflow, for any x. No event time is that
 * far, so every comparison with an event's time keeps its exact answer. */
static YTL__INLINE int64_t ytl__add_clamp(int64_t a, int64_t x, bool* sat) {
    if (x >= 0 ? x >= YTL__LIMIT - a : x <= -YTL__LIMIT - a) {
        *sat = true;
        return x >= 0 ? YTL__LIMIT : -YTL__LIMIT;
    }
    return a + x;
}

/* Base time at RT time rt: anchor_bt + floor((rt - anchor_rt) num / den).
 * Base 0 has anchor (0, 0) and rate 1, which is the identity. At rate 1
 * this is v0.3.0's sum, so a timeline without a rate keeps its bytes; the
 * sum is checked because an anchor near 2^62 can carry it past int64. */
static int64_t ytl__bt_scaled(const ytl__base* b, int64_t rt, bool* sat) {
    bool s = false;
    int64_t x = ytl__scale(rt - b->anchor_rt, b->num, b->den, b->lim_n, &s);
    if (s) {
        *sat = true;
        return x;
    }
    return ytl__add_clamp(b->anchor_bt, x, sat);
}

/* Split from the scaled path so that compilers inline this part into the
 * evaluate: called out of line, it measured about 3 ns more per base per
 * frame at rate 1 than v0.3.0's inline sum (docs/timeline.md). */
static YTL__INLINE int64_t ytl__bt_s(const ytl__base* b, int64_t rt, bool* sat) {
    if (b->state == YTL__PAUSED) return b->anchor_bt;
    if (!b->scaled) return ytl__add_clamp(b->anchor_bt, rt - b->anchor_rt, sat);
    return ytl__bt_scaled(b, rt, sat);
}

/* The first RT time at which a running base reaches base time t:
 * anchor_rt + ceil((t - anchor_bt) den / num), written as a floor of the
 * negated difference. bt(x) >= t exactly when x >= this, which makes the
 * two mappings agree on every "has the base reached it". */
static int64_t ytl__rt_s(const ytl__base* b, int64_t t, bool* sat) {
    bool s = false;
    int64_t x;
    if (!b->scaled) return ytl__add_clamp(b->anchor_rt, t - b->anchor_bt, sat);
    x = ytl__scale(b->anchor_bt - t, b->den, b->num, b->lim_d, &s);
    if (s) {
        *sat = true;
        return -x;
    }
    return ytl__add_clamp(b->anchor_rt, -x, sat);
}

/* The lead in ns for a period: one expression, so ytl_window() and the
 * evaluate cannot disagree. */
static int64_t ytl__lead_ns(const ytl_timeline* tl, int64_t period) {
    return (int64_t)(tl->lead * (double)period);
}

static void ytl__recompute(ytl_timeline* tl, int bi, int only);
static bool ytl__drives(int kind);
static void ytl__apply(ytl_timeline* tl, const ytl_event* e);

/* Make the base's events from `from` (relative) to its window pending
 * again. Late events there were never fired and lose only the late mark. */
static void ytl__unfire_from(ytl_timeline* tl, int bi, int from) {
    ytl__base* b = &tl->bases[bi];
    bool rewound = false;
    int i;
    for (i = from; i < b->hi; i++) {
        ytl_event* e = &tl->events[b->begin + i];
        if (e->flags & YTL__DONE) {
            rewound = rewound || ytl__drives(e->kind);
            e->frame = -1;
            e->onset = 0;
            e->residual = 0;
        }
        e->flags = 0;
    }
    if (from < b->hi) b->hi = from;
    if (b->dirty != YTL__NONE && b->dirty >= b->hi) b->dirty = YTL__NONE;
    if (rewound) ytl__recompute(tl, bi, -1);
}

/* First index in [lo, hi) of the storage whose time is > t. */
static int ytl__upper(const ytl_event* ev, int lo, int hi, int64_t t) {
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ev[mid].time_ns <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int ytl__anchor(ytl_timeline* tl, int base, int64_t rt, int64_t base_time) {
    ytl__base* b;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__user_base(tl, base)) return YTL_ERR_ARG;
    if (base_time >= YTL__LIMIT || base_time <= -YTL__LIMIT) return YTL_ERR_ARG;
    b = &tl->bases[base];
    /* Moving the base back to base_time puts every event at or after it in
     * the base's future again, the one exactly at base_time included: that
     * is what makes a second anchor at 0 run a trial again. last_hi moves
     * to just before base_time to keep hi = upper(last_hi). */
    if (b->evaluated && base_time <= b->last_now) {
        int from = ytl__upper(tl->events, b->begin, b->begin + b->count,
                                base_time - 1) - b->begin;
        ytl__unfire_from(tl, base, from);
        b->last_hi = base_time - 1;
        b->last_now = base_time - 1;
    }
    b->state = YTL__RUNNING;
    b->anchor_rt = rt;
    b->anchor_bt = base_time;
    b->now_bt = base_time;
    return 0;
}

static int ytl__pause(ytl_timeline* tl, int base, int64_t rt) {
    ytl__base* b;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__user_base(tl, base)) return YTL_ERR_ARG;
    b = &tl->bases[base];
    if (b->state != YTL__RUNNING) return YTL_ERR_ORDER;
    {
        bool sat = false;
        b->anchor_bt = ytl__bt_s(b, rt, &sat);
        if (sat && b->clamped < INT32_MAX) b->clamped++;
    }
    b->anchor_rt = rt;
    b->state = YTL__PAUSED;
    b->now_bt = b->anchor_bt;
    return 0;
}

static int ytl__resume(ytl_timeline* tl, int base, int64_t rt) {
    ytl__base* b;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__user_base(tl, base)) return YTL_ERR_ARG;
    b = &tl->bases[base];
    if (b->state != YTL__PAUSED) return YTL_ERR_ORDER;
    b->anchor_rt = rt;
    b->state = YTL__RUNNING;
    b->now_bt = b->anchor_bt;
    return 0;
}

static int ytl__stop(ytl_timeline* tl, int base) {
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__user_base(tl, base)) return YTL_ERR_ARG;
    tl->bases[base].state = YTL__STOPPED;
    return 0;
}

static int ytl__skip(ytl_timeline* tl, int base, int64_t rt, int64_t base_time) {
    ytl__base* b;
    int rc, i, start, end, n = 0;
    /* A skip back must rewind as an anchor does, so it goes through the
     * anchor; the skip proper is only the marking below. */
    rc = ytl_anchor(tl, base, rt, base_time);
    if (rc < 0) return rc;
    b = &tl->bases[base];
    end = ytl__upper(tl->events, b->begin, b->begin + b->count, base_time - 1) - b->begin;
    /* Below min(dirty, hi) every event is done already. */
    start = b->dirty < b->hi ? b->dirty : b->hi;
    for (i = start; i < end; i++) {
        ytl_event* e = &tl->events[b->begin + i];
        if (e->flags & YTL__DONE) continue;
        e->flags = YTL_EV_SKIPPED;
        e->frame = -1;
        e->onset = rt;
        e->residual = base_time - e->time_ns;
        if (ytl__drives(e->kind)) ytl__apply(tl, e);
        n++;
    }
    /* The base has passed everything before base_time: an add there is
     * late, and an anchor back there must rewind. Both rules read these. */
    if (!b->evaluated || base_time - 1 > b->last_hi) {
        b->last_hi = base_time - 1;
        b->hi = end;
    }
    if (!b->evaluated || base_time - 1 > b->last_now) b->last_now = base_time - 1;
    b->evaluated = 1;
    /* Only [end, hi) can still hold a late event: the window of a frame
     * before the skip may have reached past base_time. */
    if (b->dirty != YTL__NONE) {
        int d = b->dirty > end ? b->dirty : end;
        while (d < b->hi && (tl->events[b->begin + d].flags & YTL__DONE)) d++;
        b->dirty = d < b->hi ? d : YTL__NONE;
    }
    return n;
}

YTL_API bool ytl_base_time(const ytl_timeline* tl, int base, int64_t rt, int64_t* out) {
    const ytl__base* b;
    bool sat = false;
    int64_t x;
    if (!tl || !tl->open || base < 0 || base >= YTL_MAX_BASES || !out) return false;
    b = &tl->bases[base];
    if (b->state == YTL__STOPPED) return false;
    x = ytl__bt_s(b, rt, &sat);
    /* A pause can have frozen a clamped time (ytl_clamped counts it). */
    if (sat || x >= YTL__LIMIT || x <= -YTL__LIMIT) return false;
    *out = x;
    return true;
}

static int ytl__rate(ytl_timeline* tl, int base, int64_t rt, int32_t num, int32_t den) {
    ytl__base* b;
    int32_t x, y, g;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__user_base(tl, base) || num < 1 || den < 1) return YTL_ERR_ARG;
    if (rt >= YTL__LIMIT || rt <= -YTL__LIMIT) return YTL_ERR_ARG;
    /* Reduced, so equal rates are equal bytes and 1/1 is the only rate 1. */
    for (x = num, y = den; y != 0; g = x % y, x = y, y = g) {}
    num /= x;
    den /= x;
    b = &tl->bases[base];
    if (num == b->num && den == b->den) return 0;
    /* Not through ytl_anchor(): at rt = the last onset its base time is
     * the furthest evaluated one, where an anchor rewinds and the event
     * there would fire twice. A rate changes the mapping, never the fired
     * set, as a pause and a resume do. */
    if (b->state == YTL__RUNNING) {
        bool sat = false;
        int64_t bt = ytl__bt_s(b, rt, &sat);
        if (sat || bt >= YTL__LIMIT || bt <= -YTL__LIMIT) return YTL_ERR_ARG;
        b->anchor_rt = rt;
        b->anchor_bt = bt;
        b->now_bt = bt;
    }
    b->num = num;
    b->den = den;
    b->lim_n = YTL__QMAX / num;
    b->lim_d = YTL__QMAX / den;
    b->scaled = (uint8_t)(num != den);
    return 0;
}

YTL_API bool ytl_get_rate(const ytl_timeline* tl, int base, int32_t* num, int32_t* den) {
    if (!tl || !tl->open || base < 0 || base >= YTL_MAX_BASES || !num || !den) return false;
    *num = tl->bases[base].num;
    *den = tl->bases[base].den;
    return true;
}

YTL_API bool ytl_rt_time(const ytl_timeline* tl, int base, int64_t bt, int64_t* out) {
    const ytl__base* b;
    bool sat = false;
    int64_t x;
    if (!tl || !tl->open || base < 0 || base >= YTL_MAX_BASES || !out) return false;
    if (bt >= YTL__LIMIT || bt <= -YTL__LIMIT) return false;
    b = &tl->bases[base];
    if (b->state != YTL__RUNNING) return false;
    x = ytl__rt_s(b, bt, &sat);
    if (sat) return false;
    *out = x;
    return true;
}

YTL_API bool ytl_window(const ytl_timeline* tl, int base, const ytl_frame* f, int64_t* end) {
    const ytl__base* b;
    bool sat = false;
    int64_t x;
    if (!tl || !tl->open || base < 0 || base >= YTL_MAX_BASES || !f || f->period < 0 || !end)
        return false;
    b = &tl->bases[base];
    if (b->state == YTL__STOPPED) return false;
    x = ytl__bt_s(b, f->onset + ytl__lead_ns(tl, f->period), &sat);
    if (sat || x >= YTL__LIMIT || x <= -YTL__LIMIT) return false;
    *end = x;
    return true;
}

static int ytl__clamped(ytl_timeline* tl, int base) {
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (base < 0 || base >= YTL_MAX_BASES) return YTL_ERR_ARG;
    return tl->bases[base].clamped;
}

/* --- channels ---------------------------------------------------------- */

static bool ytl__drives(int kind) {
    return kind == YTL_ONSET || kind == YTL_OFFSET || kind == YTL_SET;
}

static float ytl__kind_value(const ytl_event* e) {
    return e->kind == YTL_ONSET ? 1.0f : e->kind == YTL_OFFSET ? 0.0f : e->value;
}

/* Apply a fired event to its channel unless a later (time, id) already set
 * it: the value is a function of which events have fired, not of the order
 * they fired in. */
static void ytl__apply(ytl_timeline* tl, const ytl_event* e) {
    ytl__chan* c = &tl->chans[e->target];
    if (c->has_last && (e->time_ns < c->last_time
                        || (e->time_ns == c->last_time && e->id < c->last_id)))
        return;
    c->has_last = 1;
    c->last_time = e->time_ns;
    c->last_id = e->id;
    tl->values[e->target] = ytl__kind_value(e);
}

/* Recompute the event-driven channels of base `bi` (or only `only` when it
 * is >= 0) from the base's fired events, in time order. */
static void ytl__recompute(ytl_timeline* tl, int bi, int only) {
    const ytl__base* b = &tl->bases[bi];
    int i;
    for (i = 0; i < tl->n_channels; i++) {
        ytl__chan* c = &tl->chans[i];
        if (only >= 0 && i != only) continue;
        if (c->n_events > 0 && c->base == bi) {
            c->has_last = 0;
            tl->values[i] = c->initial;
        }
    }
    for (i = b->begin; i < b->begin + b->count; i++) {
        const ytl_event* e = &tl->events[i];
        if (!(e->flags & YTL__DONE) || !ytl__drives(e->kind)) continue;
        if (only >= 0 && e->target != only) continue;
        ytl__apply(tl, e);
    }
}

/* Called when a channel's last driver goes away. */
static void ytl__free_chan(ytl_timeline* tl, int ch, bool reset_value) {
    ytl__chan* c = &tl->chans[ch];
    c->has_last = 0;
    memset(&c->tr, 0, sizeof(c->tr));
    c->own = 0;
    c->has_track = 0;
    c->has_pend = 0;
    c->pend_arena = 0;
    c->seg = -1;
    tl->track_base[ch] = 0;
    if (reset_value) tl->values[ch] = c->initial;
}

/* --- events ------------------------------------------------------------ */

static int ytl__check(const ytl_timeline* tl, const ytl_event* e) {
    if (e->base >= YTL_MAX_BASES) return YTL_ERR_ARG;
    if (e->kind > YTL_SET && e->kind < YTL_KIND_USER) return YTL_ERR_ARG;
    if (e->time_ns >= YTL__LIMIT || e->time_ns <= -YTL__LIMIT) return YTL_ERR_ARG;
    if (ytl__drives(e->kind)) {
        const ytl__chan* c;
        if (e->target < 0 || e->target >= tl->n_channels) return YTL_ERR_ARG;
        if (e->kind == YTL_SET && !ytl__finite(e->value)) return YTL_ERR_ARG;
        c = &tl->chans[e->target];
        if (tl->track_base[e->target]) return YTL_ERR_BOUND;
        if (c->n_events > 0 && c->base != e->base) return YTL_ERR_BOUND;
    }
    return 0;
}

/* Insert a checked event whose channel count is already taken. */
static void ytl__insert(ytl_timeline* tl, const ytl_event* src) {
    ytl__base* b = &tl->bases[src->base];
    int end = b->begin + b->count;
    int pos = ytl__upper(tl->events, b->begin, end, src->time_ns);
    ytl_event* e;
    int i;
    if (pos < tl->n_events)
        memmove(&tl->events[pos + 1], &tl->events[pos],
                (size_t)(tl->n_events - pos) * sizeof(ytl_event));
    e = &tl->events[pos];
    *e = *src;
    e->id = tl->next_id++;
    e->flags = 0;
    e->reserved_ = 0;
    e->frame = -1;
    e->onset = 0;
    e->residual = 0;
    tl->n_events++;
    b->count++;
    for (i = src->base + 1; i < YTL_MAX_BASES; i++) tl->bases[i].begin++;
    if (b->evaluated && src->time_ns <= b->last_hi) {
        int rel = pos - b->begin;
        b->hi++;
        if (rel < b->dirty) b->dirty = rel;
    }
}

static int ytl__add_n(ytl_timeline* tl, const ytl_event* e, int n) {
    int i, rc = 0, first;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (n < 0 || (n > 0 && !e)) return YTL_ERR_ARG;
    if (n > tl->capacity - tl->n_events) return YTL_ERR_FULL;
    if (n > INT32_MAX - tl->next_id) return YTL_ERR_FULL;
    /* Take the channel counts as we check, so two events of the batch that
     * bind one channel to two bases are caught, and undo them on a refusal. */
    for (i = 0; i < n; i++) {
        rc = ytl__check(tl, &e[i]);
        if (rc < 0) break;
        if (ytl__drives(e[i].kind)) {
            ytl__chan* c = &tl->chans[e[i].target];
            if (c->n_events++ == 0) c->base = e[i].base;
        }
    }
    if (rc < 0) {
        while (i-- > 0) {
            if (ytl__drives(e[i].kind)) tl->chans[e[i].target].n_events--;
        }
        return rc;
    }
    first = tl->next_id;
    for (i = 0; i < n; i++) ytl__insert(tl, &e[i]);
    return first;
}

static int ytl__add(ytl_timeline* tl, const ytl_event* e) {
    if (!e) return tl && tl->open ? YTL_ERR_ARG : YTL_ERR_CLOSED;
    return ytl_add_n(tl, e, 1);
}

/* Delete the event at storage index k. Recomputes its channel when the
 * value came from it, and frees the channel when it was its last event. */
static void ytl__delete(ytl_timeline* tl, int k, bool recompute) {
    ytl_event e = tl->events[k];
    ytl__base* b = &tl->bases[e.base];
    int rel = k - b->begin, i;
    if (k + 1 < tl->n_events)
        memmove(&tl->events[k], &tl->events[k + 1],
                (size_t)(tl->n_events - k - 1) * sizeof(ytl_event));
    tl->n_events--;
    b->count--;
    for (i = e.base + 1; i < YTL_MAX_BASES; i++) tl->bases[i].begin--;
    if (rel < b->hi) b->hi--;
    if (b->dirty != YTL__NONE && rel < b->dirty) b->dirty--;
    if (b->dirty >= b->hi) b->dirty = YTL__NONE;
    if (ytl__drives(e.kind)) {
        ytl__chan* c = &tl->chans[e.target];
        bool was_shown = (e.flags & YTL__DONE) && c->has_last
                         && c->last_id == e.id;
        c->n_events--;
        if (c->n_events == 0) ytl__free_chan(tl, e.target, recompute && was_shown);
        else if (recompute && was_shown) ytl__recompute(tl, e.base, e.target);
    }
}

static int ytl__remove(ytl_timeline* tl, int id) {
    int k;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    for (k = 0; k < tl->n_events; k++) {
        if (tl->events[k].id == id) {
            ytl__delete(tl, k, true);
            return 0;
        }
    }
    return YTL_ERR_NOT_FOUND;
}

static bool ytl__base_arg(int base) {
    return base == YTL_ALL_BASES || (base >= 0 && base < YTL_MAX_BASES);
}

/* Delete the events of `base` (or all), only the fired ones when
 * `fired_only`, in one pass over the storage: one deletion at a time would
 * move the tail once per event, quadratic for a movie's annotations.
 * Channels left with no event are freed, back to their initial value when
 * `reset`. Returns the number deleted. */
static int ytl__compact(ytl_timeline* tl, int base, bool fired_only, bool reset, int32_t min_id) {
    int bi, j = 0, removed = 0, i;
    for (bi = 0; bi < YTL_MAX_BASES; bi++) {
        ytl__base* b = &tl->bases[bi];
        int new_hi = 0, new_dirty = YTL__NONE, kept = 0, rel;
        bool hit = base == YTL_ALL_BASES || base == bi;
        for (rel = 0; rel < b->count; rel++) {
            const ytl_event* e = &tl->events[b->begin + rel];
            if (hit && e->id >= min_id && (!fired_only || (e->flags & YTL__DONE))) {
                if (ytl__drives(e->kind)) {
                    tl->chans[e->target].n_events--;
                    tl->chans[e->target].mark = 1;
                }
                removed++;
                continue;
            }
            if (rel < b->hi) new_hi++;
            if (rel >= b->dirty && new_dirty == YTL__NONE) new_dirty = kept;
            if (j != b->begin + rel) tl->events[j] = *e;
            j++;
            kept++;
        }
        b->begin = j - kept;
        b->count = kept;
        b->hi = new_hi;
        b->dirty = new_dirty < new_hi ? new_dirty : YTL__NONE;
    }
    tl->n_events = j;
    for (i = 0; i < tl->n_channels; i++) {
        ytl__chan* c = &tl->chans[i];
        if (c->mark && c->n_events == 0) ytl__free_chan(tl, i, reset);
        c->mark = 0;
    }
    return removed;
}

static int ytl__prune(ytl_timeline* tl, int base) {
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__base_arg(base)) return YTL_ERR_ARG;
    return ytl__compact(tl, base, true, false, 0);
}

static void ytl__tg_clear(ytl_timeline* tl, int base);
static void ytl__tg_cancel(ytl_timeline* tl, uint32_t id);

static int ytl__clear(ytl_timeline* tl, int base) {
    int removed, i;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__base_arg(base)) return YTL_ERR_ARG;
    removed = ytl__compact(tl, base, false, true, 0);
    for (i = 0; i < tl->n_channels; i++) {
        ytl__chan* c = &tl->chans[i];
        if (tl->track_base[i] && (base == YTL_ALL_BASES || c->base == base))
            ytl__free_chan(tl, i, true);
    }
    ytl__tg_clear(tl, base);
    /* The base's arena blocks are garbage now. A call that needs their room
     * compacts the arena then; doing it here too changed nothing that a
     * test could see (docs/timeline.md). */
    return removed;
}

/* --- tracks ------------------------------------------------------------ */

static bool ytl__time_ok(int64_t t) {
    return t < YTL__LIMIT && t > -YTL__LIMIT;
}

static bool ytl__curve_ok(const ytl_curve* c) {
    return ytl__finite(c->y1) && ytl__finite(c->y2)
           && c->x1 >= 0.0f && c->x1 <= 1.0f && c->x2 >= 0.0f && c->x2 <= 1.0f;
}

/* Everything ytl__track_at() relies on: it does no check of its own. */
static int ytl__check_track(const ytl_track* tr) {
    bool has_keys, has_samples;
    int i;
    if (tr->n_keys < 0 || tr->n_samples < 0 || tr->n_curves < 0) return YTL_ERR_ARG;
    if (tr->n_curves > 0 && !tr->curves) return YTL_ERR_ARG;
    has_keys = tr->keys && tr->n_keys > 0;
    has_samples = tr->samples && tr->n_samples > 0;
    if (has_keys == has_samples) return YTL_ERR_ARG;
    if (tr->period_ns < 0 || tr->period_ns >= YTL__LIMIT || tr->repeats < 0) return YTL_ERR_ARG;
    if (tr->repeats > 0 && tr->period_ns == 0) return YTL_ERR_ARG;
    for (i = 0; i < tr->n_curves; i++)
        if (!ytl__curve_ok(&tr->curves[i])) return YTL_ERR_ARG;
    if (has_keys) {
        int n = tr->n_keys;
        for (i = 0; i < n; i++) {
            const ytl_key* k = &tr->keys[i];
            if (!ytl__finite(k->value)) return YTL_ERR_ARG;
            if (k->ease > YTL__EASE_LAST) return YTL_ERR_ARG;
            if (!ytl__time_ok(k->time_ns)) return YTL_ERR_ARG;
            if (i > 0 && k->time_ns < tr->keys[i - 1].time_ns) return YTL_ERR_ARG;
            if (i + 1 < n && k->ease == YTL_EASE_LOG
                && !(k->value > 0.0f && tr->keys[i + 1].value > 0.0f))
                return YTL_ERR_ARG;
            if (i + 1 < n && k->ease == YTL_EASE_BEZIER && k->curve >= tr->n_curves)
                return YTL_ERR_ARG;
        }
    } else {
        if (tr->rate < 1 || tr->rate > YTL__NS_PER_S || !ytl__time_ok(tr->t0_ns))
            return YTL_ERR_ARG;
        if (tr->interp < YTL_INTERP_LINEAR || tr->interp > YTL_INTERP_CUBIC) return YTL_ERR_ARG;
        {
            /* The last sample's time, rounded up, must be below 2^62. The
             * products stay below 2^63: (n-1)/rate < 2^31 s, and the
             * remainder times 1e9 is below 1e18. */
            int64_t m = (int64_t)(tr->n_samples - 1);
            int64_t rem = (m % tr->rate) * YTL__NS_PER_S;
            int64_t span = (m / tr->rate) * YTL__NS_PER_S
                           + rem / tr->rate + (rem % tr->rate != 0);
            if (span > YTL__LIMIT - 1 - tr->t0_ns) return YTL_ERR_ARG;
        }
        for (i = 0; i < tr->n_samples; i++)
            if (!ytl__finite(tr->samples[i])) return YTL_ERR_ARG;
    }
    return 0;
}

static int ytl__chan_arg(const ytl_timeline* tl, int channel, int base) {
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (channel < 0 || channel >= tl->n_channels) return YTL_ERR_ARG;
    if (base < 0 || base >= YTL_MAX_BASES) return YTL_ERR_ARG;
    if (tl->chans[channel].n_events > 0) return YTL_ERR_BOUND;
    return 0;
}

/* Called after c->tr, c->own and the tween keys are set. A STEP track
 * changes only at its key times, so it is read where an event would land
 * (QUANTIZATION); see TRACKS for why. */
static void ytl__bind_track(ytl_timeline* tl, int channel, int base) {
    ytl__chan* c = &tl->chans[channel];
    const ytl_key* k = c->own ? c->ik : c->tr.keys;
    int i;
    if (k) {
        c->stepped = 1;
        for (i = 0; i + 1 < c->tr.n_keys; i++)
            if (k[i].ease != YTL_EASE_STEP) c->stepped = 0;
    } else {
        c->stepped = c->tr.interp == YTL_INTERP_STEP;
    }
    c->seg = -1;
    c->base = (uint8_t)base;
    c->has_last = 0;
    c->has_track = 1;
    c->has_pend = 0;
    c->pend_arena = 0;
    tl->track_base[channel] = (uint8_t)(base + 1);
}

static int ytl__set_track(ytl_timeline* tl, int channel, int base,
                              const ytl_track* tr) {
    int rc = ytl__chan_arg(tl, channel, base);
    ytl__chan* c;
    if (rc < 0) return rc;
    c = &tl->chans[channel];
    if (!tr) {
        if (tl->track_base[channel]) ytl__free_chan(tl, channel, true);
        return 0;
    }
    rc = ytl__check_track(tr);
    if (rc < 0) return rc;
    c->tr = *tr;
    c->own = 0;
    ytl__bind_track(tl, channel, base);
    return 0;
}

static int ytl__set_keys(ytl_timeline* tl, int channel, int base,
                             const ytl_key* keys, int n) {
    ytl_track tr;
    if (n < 0 || (n > 0 && !keys)) return tl && tl->open ? YTL_ERR_ARG : YTL_ERR_CLOSED;
    if (n == 0) return ytl_set_track(tl, channel, base, NULL);
    memset(&tr, 0, sizeof(tr));
    tr.keys = keys;
    tr.n_keys = n;
    return ytl_set_track(tl, channel, base, &tr);
}

/* Resolve `d` against the base's now and check it. On success *out is the
 * desc with start absolute and delay 0. */
static int ytl__tween_check(const ytl_timeline* tl, int base,
                              const ytl_tween_desc* d, ytl_tween_desc* out) {
    const ytl__base* b = &tl->bases[base];
    int64_t start;
    bool once = d->cycles == 0 || d->cycles == 1;
    if (d->start_set) {
        if (!ytl__time_ok(d->start_ns)) return YTL_ERR_ARG;
        start = d->start_ns;
    } else {
        start = b->now_bt;
    }
    if (!ytl__time_ok(d->delay_ns)) return YTL_ERR_ARG;
    start += d->delay_ns;
    if (!ytl__time_ok(start) || d->duration_ns < 0 || d->duration_ns >= YTL__LIMIT)
        return YTL_ERR_ARG;
    if (!ytl__time_ok(start + (d->yoyo ? 2 : 1) * d->duration_ns)) return YTL_ERR_ARG;
    if (!ytl__finite(d->to) || (d->from_set && !ytl__finite(d->from))) return YTL_ERR_ARG;
    if (d->ease < 0 || d->ease > YTL__EASE_LAST) return YTL_ERR_ARG;
    if (d->ease == YTL_EASE_BEZIER && !ytl__curve_ok(&d->curve)) return YTL_ERR_ARG;
    if (d->ease == YTL_EASE_LOG
        && !(d->from_set && d->from > 0.0f && d->to > 0.0f))
        return YTL_ERR_ARG;
    if (d->cycles < YTL_FOREVER) return YTL_ERR_ARG;
    if (!once && d->duration_ns == 0) return YTL_ERR_ARG;
    if (d->yoyo && d->duration_ns == 0) return YTL_ERR_ARG;
    if (d->keep_velocity
        && (d->ease != YTL_EASE_LINEAR || d->from_set || d->yoyo || !once
            || d->duration_ns == 0))
        return YTL_ERR_ARG;
    *out = *d;
    out->start_ns = start;
    out->start_set = true;
    out->delay_ns = 0;
    return 0;
}

static int ytl__tween(ytl_timeline* tl, int channel, int base,
                          const ytl_tween_desc* d) {
    int rc = ytl__chan_arg(tl, channel, base);
    ytl_tween_desc r;
    ytl__chan* c;
    if (rc < 0) return rc;
    if (!d) return YTL_ERR_ARG;
    rc = ytl__tween_check(tl, base, d, &r);
    if (rc < 0) return rc;
    c = &tl->chans[channel];
    /* A track on another base cannot run on until a start on this one: the
     * two times do not compare. It stops here, at its last value. */
    if (c->has_track && c->base != base) {
        c->has_track = 0;
        c->own = 0;
        memset(&c->tr, 0, sizeof(c->tr));
    }
    c->pend = r;
    c->has_pend = 1;
    c->pend_arena = 0;
    c->pend_stepped = r.ease == YTL_EASE_STEP;
    c->base = (uint8_t)base;
    c->has_last = 0;
    tl->track_base[channel] = (uint8_t)(base + 1);
    return 0;
}

/* The pending tween takes over at its start. Its start value and slope
 * are the old track's at that base time, not at the frame that happens to
 * promote it, so the takeover does not depend on the frame timing: a
 * tween posted just before a frame starts from where the old track would
 * have been on that frame, with no repeated position. The slope is the
 * difference over the last microsecond before the start, which keeps a
 * jump at the start out of it. */
/* The value and the slope per ns of the channel's old driver at base time
 * s, for a takeover: the track's, or the channel's value with no track. */
static double ytl__take(const ytl_timeline* tl, int ch, int64_t s, bool want_slope, double* slope) {
    const ytl__chan* c = &tl->chans[ch];
    const ytl_curve* curves;
    const ytl_key* keys;
    double from = tl->values[ch];
    int32_t seg = -1;
    *slope = 0.0;
    if (!c->has_track) return from;
    keys = ytl__chan_keys(tl, c, &curves);
    from = ytl__track_at(&c->tr, keys, curves, s, &seg);
    if (want_slope) {
        seg = -1;
        *slope = (from - ytl__track_at(&c->tr, keys, curves, s - 1000, &seg)) / 1000.0;
    }
    return from;
}

static void ytl__promote_block(ytl_timeline* tl, int ch);

static void ytl__promote(ytl_timeline* tl, int ch) {
    ytl__chan* c = &tl->chans[ch];
    const ytl_tween_desc* p = &c->pend;
    int64_t s = p->start_ns, dur = p->duration_ns;
    double from, slope = 0.0;
    bool once = p->cycles == 0 || p->cycles == 1;
    if (c->pend_arena) {
        ytl__promote_block(tl, ch);
        return;
    }
    from = ytl__take(tl, ch, s, p->keep_velocity, &slope);
    if (p->from_set) from = p->from;
    memset(&c->tr, 0, sizeof(c->tr));
    memset(c->ik, 0, sizeof(c->ik));
    c->ik[0].time_ns = s;
    c->ik[0].value = (float)from;
    c->ik[0].ease = (uint8_t)p->ease;
    c->ik[1].time_ns = s + dur;
    c->ik[1].value = p->to;
    c->tr.n_keys = 2;
    if (p->yoyo) {
        c->ik[1].ease = (uint8_t)p->ease;
        c->ik[2].time_ns = s + 2 * dur;
        c->ik[2].value = (float)from;
        c->tr.n_keys = 3;
    }
    if (p->ease == YTL_EASE_BEZIER) {
        c->ic = p->curve;
        c->tr.n_curves = 1;
    }
    if (p->keep_velocity) {
        /* The Hermite start tangent, in value units over the whole tween. */
        c->ik[0].ease = YTL__EASE_HERMITE;
        memset(&c->ic, 0, sizeof(c->ic));
        c->ic.x1 = (float)(slope * (double)dur);
        c->tr.n_curves = 1;
    }
    if (!once) {
        c->tr.period_ns = p->yoyo ? 2 * dur : dur;
        c->tr.repeats = p->cycles == YTL_FOREVER ? 0 : p->cycles;
    }
    c->own = 1;
    c->has_track = 1;
    c->has_pend = 0;
    c->stepped = c->pend_stepped;
    c->seg = -1;
}

/* --- the key arena (SEQUENCES) ---------------------------------------- */

/* key.reserved_ in an arena block: the key's value is the takeover's,
 * written when the block takes over (ytl__promote_block). */
#define YTL__KEY_TAKE 1u

/* A curve is kept in a key slot (both 16 bytes) and copied in and out with
 * memcpy, so the arena is one array of one type. */
typedef char ytl__curve_fits_key[sizeof(ytl_curve) <= sizeof(ytl_key) ? 1 : -1];

/* Each block is a header slot, then its keys, then its curve slots. The
 * header holds the counts and the channel, so a compaction is one pass
 * over the arena with no table: a block is live when its channel's track
 * or waiting block is at its offset. */
static void ytl__blk_set(ytl_key* h, int ch, int32_t nk, int32_t nc) {
    memset(h, 0, sizeof(*h));
    h->time_ns = (int64_t)(((uint64_t)(uint32_t)nk << 32) | (uint32_t)nc);
    h->curve = (uint16_t)ch;
}

static int32_t ytl__blk_nk(const ytl_key* h) { return (int32_t)((uint64_t)h->time_ns >> 32); }
static int32_t ytl__blk_nc(const ytl_key* h) { return (int32_t)((uint64_t)h->time_ns & 0xFFFFFFFFu); }

static bool ytl__blk_live(const ytl_timeline* tl, int ch, int32_t off, bool* pend) {
    const ytl__chan* c;
    if (ch >= tl->n_channels) return false;
    c = &tl->chans[ch];
    *pend = c->has_pend && c->pend_arena && c->poff == off;
    return *pend || (c->has_track && c->own == 2 && c->aoff == off);
}

/* Slide the live blocks down over the garbage, in one pass, and move the
 * channels' offsets with them. Evaluate never calls it. */
static void ytl__arena_compact(ytl_timeline* tl) {
    int32_t p = 0, dst = 0;
    while (p < tl->arena_top) {
        const ytl_key* h = &tl->arena[p];
        int ch = h->curve;
        int32_t size = 1 + ytl__blk_nk(h) + ytl__blk_nc(h);
        bool pend;
        if (ytl__blk_live(tl, ch, p + 1, &pend)) {
            if (dst != p) memmove(&tl->arena[dst], &tl->arena[p], (size_t)size * sizeof(ytl_key));
            if (pend) tl->chans[ch].poff = dst + 1;
            else tl->chans[ch].aoff = dst + 1;
            dst += size;
        }
        p += size;
    }
    tl->arena_top = dst;
}

/* Slots in live blocks, headers included. */
static int64_t ytl__arena_live(const ytl_timeline* tl) {
    int64_t live = 0;
    int32_t p = 0;
    while (p < tl->arena_top) {
        const ytl_key* h = &tl->arena[p];
        int32_t size = 1 + ytl__blk_nk(h) + ytl__blk_nc(h);
        bool pend;
        if (ytl__blk_live(tl, h->curve, p + 1, &pend)) live += size;
        p += size;
    }
    return live;
}

/* Room for ak more keys and ac more curves in channel ch's block, whose
 * offset is *offp (offp NULL: a new block). A block at the top grows in
 * place; another is copied to the top, and the old copy is garbage until a
 * compaction. Returns the block's offset, or -1 when it does not fit even
 * after a compaction; nothing changes then: a block is never cut short. */
static int32_t ytl__blk_grow(ytl_timeline* tl, int ch, int32_t* offp, int32_t ak, int32_t ac) {
    int pass;
    for (pass = 0; pass < 2; pass++) {
        int32_t off = offp ? *offp : -1, nk = 0, nc = 0;
        int64_t need;
        bool top;
        if (off >= 0) {
            nk = ytl__blk_nk(&tl->arena[off - 1]);
            nc = ytl__blk_nc(&tl->arena[off - 1]);
        }
        top = off >= 0 && off + nk + nc == tl->arena_top;
        need = top ? (int64_t)ak + ac : 1 + (int64_t)nk + nc + ak + ac;
        if (tl->arena_top + need <= tl->arena_cap) {
            ytl_key* k;
            if (top) {
                k = &tl->arena[off];
                memmove(&k[nk + ak], &k[nk], (size_t)nc * sizeof(ytl_key));
            } else {
                int32_t at = tl->arena_top + 1;
                k = &tl->arena[at];
                if (off >= 0) {
                    memcpy(k, &tl->arena[off], (size_t)nk * sizeof(ytl_key));
                    memcpy(&k[nk + ak], &tl->arena[off + nk], (size_t)nc * sizeof(ytl_key));
                }
                off = at;
            }
            ytl__blk_set(&tl->arena[off - 1], ch, nk + ak, nc + ac);
            tl->arena_top = off + nk + ak + nc + ac;
            return off;
        }
        if (pass == 0) ytl__arena_compact(tl);
    }
    return -1;
}

/* A waiting block takes over at its start: every key marked TAKE gets the
 * old driver's value there, and a keep_velocity first segment gets the old
 * driver's slope as its Hermite tangent. After this the block is an
 * ordinary keyed track, a function of base time, so a rewind replays it. */
static void ytl__promote_block(ytl_timeline* tl, int ch) {
    ytl__chan* c = &tl->chans[ch];
    ytl_key* k = tl->arena + c->poff;
    int32_t nk = ytl__blk_nk(&k[-1]), nc = ytl__blk_nc(&k[-1]), i;
    bool kv = k[0].ease == YTL__EASE_HERMITE;
    double slope, from = ytl__take(tl, ch, k[0].time_ns, kv, &slope);
    for (i = 0; i < nk; i++) {
        if (k[i].reserved_ & YTL__KEY_TAKE) k[i].value = (float)from;
        k[i].reserved_ = 0;
    }
    if (kv && nk > 1) {
        ytl_curve h;
        memset(&h, 0, sizeof h);
        h.x1 = (float)(slope * (double)(k[1].time_ns - k[0].time_ns));
        memcpy(&k[nk + k[0].curve], &h, sizeof h);
    }
    memset(&c->tr, 0, sizeof(c->tr));
    c->tr.n_keys = nk;
    c->tr.n_curves = nc;
    c->aoff = c->poff;
    c->own = 2;
    c->has_track = 1;
    c->has_pend = 0;
    c->pend_arena = 0;
    c->stepped = c->pend_stepped;
    c->seg = -1;
}

/* --- sequences --------------------------------------------------------- */

/* The sticky error's message, in the timeline the sequence builds on. */
static void ytl__seq_msg(const ytl_seq* q) {
    if (!q->tl) return;
    if (q->err_call < 0)
        snprintf(q->tl->error, sizeof(q->tl->error), "ysp_timeline: sequence on base %d: %s",
                 q->base, ytl_strerror(q->err));
    else
        snprintf(q->tl->error, sizeof(q->tl->error), "ysp_timeline: sequence call %d: %s",
                 q->err_call, ytl_strerror(q->err));
}

static ytl_seq ytl__seq_make(ytl_timeline* tl, int base, bool at_now) {
    ytl_seq q;
    memset(&q, 0, sizeof q);
    q.tl = tl;
    q.base = base;
    q.err_call = -1;
    if (!tl || !tl->open) {
        q.err = YTL_ERR_CLOSED;
        ytl__seq_msg(&q);
    } else if (base < 0 || base >= YTL_MAX_BASES) {
        q.err = YTL_ERR_ARG;
        ytl__seq_msg(&q);
    } else {
        if (++tl->seq_next == 0) ++tl->seq_next;
        q.id = tl->seq_next;
        q.id0 = tl->next_id;
        if (at_now) q.t = base == YTL_BASE_RT ? (tl->has_onset ? tl->last_onset : 0) : tl->bases[base].now_bt;
        q.prev_start = q.t;
        q.prev_end = q.t;
    }
    return q;
}

YTL_API ytl_seq ytl_seq_on(ytl_timeline* tl, int base) { return ytl__seq_make(tl, base, false); }
YTL_API ytl_seq ytl_seq_now(ytl_timeline* tl, int base) { return ytl__seq_make(tl, base, true); }

/* Counts the call; false when the sequence has stopped. */
static bool ytl__seq_go(ytl_seq* q) {
    if (!q) return false;
    q->n_calls++;
    if (q->err < 0) return false;
    if (!q->tl || !q->tl->open) {
        q->err = YTL_ERR_CLOSED;
        q->err_call = q->n_calls - 1;
        ytl__seq_msg(q);
        return false;
    }
    return true;
}

static int ytl__seq_fail(ytl_seq* q, int rc) {
    if (rc < 0 && q->err >= 0) {
        q->err = rc;
        q->err_call = q->n_calls - 1;
        ytl__seq_msg(q);
    }
    return rc;
}

/* The block a sequence built on a channel, waiting or taken over, or NULL. */
static int32_t* ytl__seq_blk(ytl_timeline* tl, uint32_t id, int ch) {
    ytl__chan* c = &tl->chans[ch];
    if (id == 0 || c->seq_id != id) return NULL;
    if (c->has_pend && c->pend_arena) return &c->poff;
    if (c->has_track && c->own == 2) return &c->aoff;
    return NULL;
}

/* What a sequence tween adds, from the desc and the channel's block so far.
 * Pure: ytl_check_ops() uses it too. has_prev and prev_end describe the
 * block; *s, *end, *nk and *nc come back. */
static int ytl__seq_tw_plan(const ytl_timeline* tl, int base, int64_t cursor,
                              const ytl_tween_desc* d, bool has_prev, int64_t prev_end,
                              int64_t* s, int64_t* end, int32_t* nk, int32_t* nc) {
    ytl_tween_desc d2, r;
    int64_t n, cyc;
    int rc;
    if (!d) return YTL_ERR_ARG;
    /* The cursor is the start; a second source for it would be silent. A
     * repeat folds a whole track, so an endless one cannot be a segment. */
    if (d->start_set || d->cycles == YTL_FOREVER) return YTL_ERR_ARG;
    d2 = *d;
    d2.start_ns = cursor;
    d2.start_set = true;
    rc = ytl__tween_check(tl, base, &d2, &r);
    if (rc < 0) return rc;
    n = d->cycles > 1 ? d->cycles : 1;
    cyc = d->duration_ns * (d->yoyo ? 2 : 1);
    if (cyc > 0 && n > (YTL__LIMIT - 1 - r.start_ns) / cyc) return YTL_ERR_ARG;
    /* Cutting an eased segment part way changes its shape, so a tween may
     * not start inside the channel's last one. keep_velocity's slope is the
     * takeover's, which only the first segment has. */
    if (has_prev && (r.start_ns < prev_end || d->keep_velocity)) return YTL_ERR_ARG;
    if (n > (INT32_MAX / 4)) return YTL_ERR_FULL;
    *s = r.start_ns;
    *end = r.start_ns + n * cyc;
    *nk = (int32_t)(n * (d->yoyo ? 3 : 2));
    *nc = d->ease == YTL_EASE_BEZIER || d->keep_velocity ? 1 : 0;
    return 0;
}


/* Room in channel ch's block of sequence q for ak more keys and ac more
 * curves; a new block when q has none on ch. The caller writes the new
 * keys at k[*nk0 ..] and the new curves after all the keys, then calls
 * ytl__seq_fin(). */
static int ytl__seq_room(ytl_seq* q, int ch, int32_t ak, int32_t ac, int32_t* off,
                           int32_t* nk0, int32_t* nc0) {
    ytl_timeline* tl = q->tl;
    int32_t* offp = ytl__seq_blk(tl, q->id, ch);
    *nk0 = 0;
    *nc0 = 0;
    if (offp) {
        *nk0 = ytl__blk_nk(&tl->arena[*offp - 1]);
        *nc0 = ytl__blk_nc(&tl->arena[*offp - 1]);
    }
    *off = ytl__blk_grow(tl, ch, offp, ak, ac);
    return *off < 0 ? YTL_ERR_FULL : 0;
}

/* After the keys are written: a new block waits for its first key's time
 * in the channel's waiting slot, as a tween does; a grown one moves. */
static void ytl__seq_fin(ytl_seq* q, int ch, int32_t off, int32_t nk, int32_t nc) {
    ytl_timeline* tl = q->tl;
    ytl__chan* c = &tl->chans[ch];
    int32_t* offp = ytl__seq_blk(tl, q->id, ch);
    const ytl_key* k = &tl->arena[off];
    uint8_t st = 1;
    int32_t i;
    for (i = 0; i + 1 < nk; i++)
        if (k[i].ease != YTL_EASE_STEP) st = 0;
    if (!offp) {
        /* A track on another base cannot run on until a start on this one,
         * whose time does not compare: it stops here, as for ytl_tween().
         * A waiting tween is replaced. */
        if (c->has_track && c->base != q->base) {
            c->has_track = 0;
            c->own = 0;
            memset(&c->tr, 0, sizeof(c->tr));
        }
        memset(&c->pend, 0, sizeof(c->pend));
        c->pend.start_ns = k[0].time_ns;
        c->poff = off;
        c->has_pend = 1;
        c->pend_arena = 1;
        c->pend_stepped = st;
        c->base = (uint8_t)q->base;
        c->has_last = 0;
        c->seq_id = q->id;
        tl->track_base[ch] = (uint8_t)(q->base + 1);
    } else if (offp == &c->poff) {
        c->poff = off;
        c->pend_stepped = st;
    } else {
        c->aoff = off;
        c->tr.n_keys = nk;
        c->tr.n_curves = nc;
        c->stepped = st;
        c->seg = -1;
    }
}

/* The last key of q's block on ch, or NULL. */
static const ytl_key* ytl__seq_last(const ytl_timeline* tl, uint32_t id, int ch) {
    const ytl__chan* c = &tl->chans[ch];
    int32_t off;
    if (id == 0 || c->seq_id != id) return NULL;
    if (c->has_pend && c->pend_arena) off = c->poff;
    else if (c->has_track && c->own == 2) off = c->aoff;
    else return NULL;
    return &tl->arena[off + ytl__blk_nk(&tl->arena[off - 1]) - 1];
}

static int ytl__seq_tween(ytl_seq* q, int ch, const ytl_tween_desc* d, int64_t* end) {
    ytl_timeline* tl = q->tl;
    const ytl_key* last;
    ytl_key* k;
    int32_t nk, nc, nk0, nc0, off, per, j;
    int64_t s, n, cyc;
    float from = 0.0f;
    uint8_t take = YTL__KEY_TAKE;
    int rc = ytl__chan_arg(tl, ch, q->base);
    if (rc < 0) return rc;
    last = ytl__seq_last(tl, q->id, ch);
    rc = ytl__seq_tw_plan(tl, q->base, q->t, d, last != NULL, last ? last->time_ns : 0, &s, end, &nk, &nc);
    if (rc < 0) return rc;
    /* A later tween starts from where the block ends; the first one from
     * the old driver at its start, which only the takeover knows. */
    if (last) {
        from = last->value;
        take = (uint8_t)(last->reserved_ & YTL__KEY_TAKE);
    }
    if (d->from_set) {
        from = d->from;
        take = 0;
    }
    rc = ytl__seq_room(q, ch, nk, nc, &off, &nk0, &nc0);
    if (rc < 0) return rc;
    k = &tl->arena[off];
    /* The block's last key holds its value until this start. */
    if (nk0 > 0) k[nk0 - 1].ease = YTL_EASE_STEP;
    n = d->cycles > 1 ? d->cycles : 1;
    cyc = d->duration_ns * (d->yoyo ? 2 : 1);
    per = d->yoyo ? 3 : 2;
    /* The cycles are unrolled: a repeat folds a whole track, not a segment. */
    for (j = 0; j < (int32_t)n; j++) {
        ytl_key* a = &k[nk0 + j * per];
        int64_t t0 = s + j * cyc;
        memset(a, 0, (size_t)per * sizeof(*a));
        a[0].time_ns = t0;
        a[0].value = from;
        a[0].ease = (uint8_t)(d->keep_velocity ? YTL__EASE_HERMITE : d->ease);
        a[0].reserved_ = take;
        a[0].curve = (uint16_t)nc0;
        /* Every key carries the tween's ease, the ends of cycles too: their
         * zero-length segments are never read, but a STEP tween then stays
         * a STEP track, read where events land, as ytl_tween() makes it. */
        a[1].time_ns = t0 + d->duration_ns;
        a[1].value = d->to;
        a[1].ease = (uint8_t)d->ease;
        a[1].curve = (uint16_t)nc0;
        if (d->yoyo) {
            a[2].time_ns = t0 + 2 * d->duration_ns;
            a[2].value = from;
            a[2].ease = (uint8_t)d->ease;
            a[2].curve = (uint16_t)nc0;
            a[2].reserved_ = take;
        }
    }
    if (nc) {
        ytl_curve cv;
        memset(&cv, 0, sizeof cv);   /* keep_velocity: the tangent, at the takeover */
        if (d->ease == YTL_EASE_BEZIER) cv = d->curve;
        memcpy(&k[nk0 + nk + nc0], &cv, sizeof cv);
    }
    ytl__seq_fin(q, ch, off, nk0 + nk, nc0 + nc);
    q->prev_start = s;
    q->prev_end = *end;
    return 0;
}

static int ytl__seq_keys(ytl_seq* q, int ch, const float* v, const int64_t* dt, int n,
                           int ease, int64_t* end) {
    ytl_timeline* tl = q->tl;
    const ytl_key* last;
    ytl_key* k;
    int32_t nk0, nc0, off;
    int i, rc = ytl__chan_arg(tl, ch, q->base);
    if (rc < 0) return rc;
    if (n < 1 || !v || !dt || ease < YTL_EASE_LINEAR || ease > YTL__EASE_LAST
        || ease == YTL_EASE_BEZIER)
        return YTL_ERR_ARG;
    for (i = 0; i < n; i++) {
        if (dt[i] < 0 || dt[i] >= YTL__LIMIT || !ytl__time_ok(q->t + dt[i])) return YTL_ERR_ARG;
        if (i > 0 && dt[i] < dt[i - 1]) return YTL_ERR_ARG;
        if (!ytl__finite(v[i])) return YTL_ERR_ARG;
        if (ease == YTL_EASE_LOG && n > 1 && !(v[i] > 0.0f)) return YTL_ERR_ARG;
    }
    last = ytl__seq_last(tl, q->id, ch);
    if (last && q->t + dt[0] < last->time_ns) return YTL_ERR_ARG;
    if (n > INT32_MAX / 4) return YTL_ERR_FULL;
    rc = ytl__seq_room(q, ch, n, 0, &off, &nk0, &nc0);
    if (rc < 0) return rc;
    k = &tl->arena[off];
    if (nk0 > 0) k[nk0 - 1].ease = YTL_EASE_STEP;
    for (i = 0; i < n; i++) {
        ytl_key* a = &k[nk0 + i];
        memset(a, 0, sizeof(*a));
        a->time_ns = q->t + dt[i];
        a->value = v[i];
        a->ease = (uint8_t)(i + 1 < n ? ease : YTL_EASE_LINEAR);
    }
    ytl__seq_fin(q, ch, off, nk0 + n, nc0);
    *end = q->t + dt[n - 1];
    q->prev_start = q->t + dt[0];
    q->prev_end = *end;
    return 0;
}

static int ytl__seq_ev(ytl_seq* q, int kind, int ch, float value, int32_t code, uint64_t user) {
    ytl_event e;
    int rc;
    memset(&e, 0, sizeof e);
    e.time_ns = q->t;
    e.base = (uint8_t)q->base;
    e.kind = (uint8_t)kind;
    e.target = ch;
    e.value = value;
    e.code = code;
    e.user = user;
    rc = ytl_add(q->tl, &e);
    if (rc < 0) return rc;
    q->prev_start = q->t;
    q->prev_end = q->t;
    return 0;
}

static int ytl__seq_wait(ytl_seq* q, int64_t dt) {
    if (!ytl__time_ok(dt) || !ytl__time_ok(q->t + dt)) return YTL_ERR_ARG;
    q->t += dt;
    return 0;
}

static int ytl__seq_at(ytl_seq* q, int64_t t) {
    if (!ytl__time_ok(t)) return YTL_ERR_ARG;
    q->t = t;
    return 0;
}

static int ytl__seq_on_n(ytl_seq* q, const int* chans, int n, int64_t stagger) {
    ytl_timeline* tl = q->tl;
    int i;
    if (n < 0 || (n > 0 && !chans) || !ytl__time_ok(stagger)) return YTL_ERR_ARG;
    if (n == 0) return 0;
    if (stagger > 0 && stagger > (YTL__LIMIT - 1 - q->t) / (n > 1 ? n - 1 : 1)) return YTL_ERR_ARG;
    if (stagger < 0 && -stagger > (q->t + YTL__LIMIT - 1) / (n > 1 ? n - 1 : 1)) return YTL_ERR_ARG;
    /* Checked before the first add, so the call is all or none. */
    for (i = 0; i < n; i++) {
        int ch = chans[i];
        if (ch < 0 || ch >= tl->n_channels) return YTL_ERR_ARG;
        if (tl->track_base[ch]) return YTL_ERR_BOUND;
        if (tl->chans[ch].n_events > 0 && tl->chans[ch].base != q->base) return YTL_ERR_BOUND;
    }
    if (n > tl->capacity - tl->n_events || n > INT32_MAX - tl->next_id) return YTL_ERR_FULL;
    for (i = 0; i < n; i++) {
        int64_t t0 = q->t;
        q->t = t0 + (int64_t)i * stagger;
        (void)ytl__seq_ev(q, YTL_ONSET, chans[i], 0.0f, 0, 0);
        q->t = t0;
    }
    q->prev_start = stagger < 0 ? q->t + (int64_t)(n - 1) * stagger : q->t;
    q->prev_end = stagger < 0 ? q->t : q->t + (int64_t)(n - 1) * stagger;
    return 0;
}

YTL_API void ytl_wait(ytl_seq* q, int64_t dt) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_wait(q, dt));
}

YTL_API void ytl_at(ytl_seq* q, int64_t t) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_at(q, t));
}

YTL_API void ytl_on(ytl_seq* q, int ch) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_ev(q, YTL_ONSET, ch, 0.0f, 0, 0));
}

YTL_API void ytl_off(ytl_seq* q, int ch) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_ev(q, YTL_OFFSET, ch, 0.0f, 0, 0));
}

YTL_API void ytl_set_value(ytl_seq* q, int ch, float value) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_ev(q, YTL_SET, ch, value, 0, 0));
}

YTL_API void ytl_trigger(ytl_seq* q, int32_t code) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_ev(q, YTL_TRIGGER, 0, 0.0f, code, 0));
}

YTL_API void ytl_mark(ytl_seq* q, int32_t code, uint64_t user) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_ev(q, YTL_MARK, 0, 0.0f, code, user));
}

YTL_API void ytl_on_n(ytl_seq* q, const int* chans, int n, int64_t stagger) {
    if (ytl__seq_go(q)) ytl__seq_fail(q, ytl__seq_on_n(q, chans, n, stagger));
}

YTL_API int64_t ytl_to(ytl_seq* q, int ch, const ytl_tween_desc* d) {
    int64_t end = 0;
    if (!ytl__seq_go(q)) return q ? q->t : 0;
    if (ytl__seq_fail(q, ytl__seq_tween(q, ch, d, &end)) < 0) return q->t;
    return end;
}

YTL_API void ytl_then(ytl_seq* q, int ch, const ytl_tween_desc* d) {
    int64_t end = 0;
    if (!ytl__seq_go(q)) return;
    if (ytl__seq_fail(q, ytl__seq_tween(q, ch, d, &end)) == 0) q->t = end;
}

YTL_API int64_t ytl_keyframes(ytl_seq* q, int ch, const float* values,
                                  const int64_t* offsets, int n, int ease) {
    int64_t end = 0;
    if (!ytl__seq_go(q)) return q ? q->t : 0;
    if (ytl__seq_fail(q, ytl__seq_keys(q, ch, values, offsets, n, ease, &end)) < 0) return q->t;
    return end;
}

/* --- op tables --------------------------------------------------------- */

static bool ytl__op_names_ch(const ytl_op* o) {
    return o->op == YTL_OP_ON || o->op == YTL_OP_OFF || o->op == YTL_OP_SET_VALUE
           || o->op == YTL_OP_TO || o->op == YTL_OP_THEN;
}

static bool ytl__op_is_tween(const ytl_op* o) {
    return o->op == YTL_OP_TO || o->op == YTL_OP_THEN;
}

/* The end of a tween op that passed its checks, from the cursor. */
static int64_t ytl__op_end(const ytl_op* o, int64_t t) {
    const ytl_tween_desc* d = &o->tween;
    int64_t n = d->cycles > 1 ? d->cycles : 1;
    return t + d->delay_ns + n * d->duration_ns * (d->yoyo ? 2 : 1);
}

/* Binding, by a rule and not a table: op i is checked against the first
 * earlier op that names its channel, or, with none, against the channel's
 * state in the timeline. A table per channel would need YTL_MAX_CHANNELS
 * bytes on the stack (up to 64 KB) or a write in a const call; the rule
 * costs O(n^2) compares, once, when a table loads. */
static int ytl__op_bind(const ytl_timeline* tl, int base, const ytl_op* ops, int i) {
    const ytl_op* o = &ops[i];
    const ytl__chan* c = &tl->chans[o->ch];
    bool ev = !ytl__op_is_tween(o);
    int j;
    for (j = 0; j < i; j++) {
        if (!ytl__op_names_ch(&ops[j]) || ops[j].ch != o->ch) continue;
        return ev == !ytl__op_is_tween(&ops[j]) ? 0 : YTL_ERR_BOUND;
    }
    if (ev) {
        if (tl->track_base[o->ch]) return YTL_ERR_BOUND;
        if (c->n_events > 0 && c->base != base) return YTL_ERR_BOUND;
        return 0;
    }
    return c->n_events > 0 ? YTL_ERR_BOUND : 0;
}

/* The block of op i's channel before op i: the sequence's (id) and the
 * table's earlier tween ops on it. *size counts slots, header included. */
static void ytl__op_prev(const ytl_timeline* tl, uint32_t id, int64_t cursor, const ytl_op* ops,
                           int i, bool* has, int64_t* end, int64_t* size) {
    const ytl_key* last = ytl__seq_last(tl, id, ops[i].ch);
    int64_t t = cursor;
    int j;
    *has = last != NULL;
    *end = last ? last->time_ns : 0;
    *size = 0;
    if (last) {
        const ytl__chan* c = &tl->chans[ops[i].ch];
        int32_t off = c->has_pend && c->pend_arena ? c->poff : c->aoff;
        *size = 1 + ytl__blk_nk(&tl->arena[off - 1]) + ytl__blk_nc(&tl->arena[off - 1]);
    }
    for (j = 0; j < i; j++) {
        const ytl_op* o = &ops[j];
        if (o->op == YTL_OP_WAIT) t += o->t_ns;
        else if (o->op == YTL_OP_AT) t = o->t_ns;
        if (!ytl__op_is_tween(o)) continue;
        if (o->ch == ops[i].ch) {
            const ytl_tween_desc* d = &o->tween;
            int64_t n = d->cycles > 1 ? d->cycles : 1;
            *size += (*size == 0 ? 1 : 0) + n * (d->yoyo ? 3 : 2)
                     + (d->ease == YTL_EASE_BEZIER || d->keep_velocity ? 1 : 0);
            *end = ytl__op_end(o, t);
            *has = true;
        }
        if (o->op == YTL_OP_THEN) t = ytl__op_end(o, t);
    }
}

static int ytl__check_ops(const ytl_timeline* tl, int base, int64_t cursor, uint32_t id,
                            const ytl_op* ops, int n, int* bad) {
    int64_t t = cursor, n_ev = 0, slots = 0, maxblk = 0, live;
    int i, rc;
    *bad = -1;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (base < 0 || base >= YTL_MAX_BASES || n < 0 || (n > 0 && !ops)) return YTL_ERR_ARG;
    live = ytl__arena_live(tl);
    for (i = 0; i < n; i++) {
        const ytl_op* o = &ops[i];
        *bad = i;
        switch (o->op) {
        case YTL_OP_WAIT:
            if (!ytl__time_ok(o->t_ns) || !ytl__time_ok(t + o->t_ns)) return YTL_ERR_ARG;
            t += o->t_ns;
            break;
        case YTL_OP_AT:
            if (!ytl__time_ok(o->t_ns)) return YTL_ERR_ARG;
            t = o->t_ns;
            break;
        case YTL_OP_ON:
        case YTL_OP_OFF:
        case YTL_OP_SET_VALUE:
            if (o->ch < 0 || o->ch >= tl->n_channels) return YTL_ERR_ARG;
            if (o->op == YTL_OP_SET_VALUE && !ytl__finite(o->value)) return YTL_ERR_ARG;
            rc = ytl__op_bind(tl, base, ops, i);
            if (rc < 0) return rc;
            /* fall through */
        case YTL_OP_TRIGGER:
        case YTL_OP_MARK:
            n_ev++;
            if (n_ev > tl->capacity - tl->n_events || n_ev > INT32_MAX - tl->next_id) return YTL_ERR_FULL;
            break;
        case YTL_OP_TO:
        case YTL_OP_THEN: {
            bool has;
            int64_t prev_end, size, s, end;
            int32_t nk, nc;
            if (o->ch < 0 || o->ch >= tl->n_channels) return YTL_ERR_ARG;
            rc = ytl__op_bind(tl, base, ops, i);
            if (rc < 0) return rc;
            ytl__op_prev(tl, id, cursor, ops, i, &has, &prev_end, &size);
            rc = ytl__seq_tw_plan(tl, base, t, &o->tween, has, prev_end, &s, &end, &nk, &nc);
            if (rc < 0) return rc;
            slots += (size == 0 ? 1 : 0) + nk + nc;
            size += (size == 0 ? 1 : 0) + nk + nc;
            if (size > maxblk) maxblk = size;
            /* Room for every block the table adds, and one copy of the
             * largest, which a growing block not at the top needs. */
            if (live + slots + maxblk > tl->arena_cap) return YTL_ERR_FULL;
            if (o->op == YTL_OP_THEN) t = end;
            break;
        }
        default:
            return YTL_ERR_ARG;
        }
    }
    *bad = -1;
    return 0;
}

YTL_API int ytl_check_ops(ytl_timeline* tl, int base, const ytl_op* ops, int n, int* bad) {
    int b;
    int rc = ytl__check_ops(tl, base, 0, 0, ops, n, &b);
    if (bad) *bad = b;
    if (rc < 0 && tl)
        snprintf(tl->error, sizeof(tl->error), "ysp_timeline: ytl_check_ops: op %d: %s", b,
                 ytl_strerror(rc));
    return rc;
}

static int ytl__seq_op(ytl_seq* q, const ytl_op* o) {
    int64_t end = 0;
    int rc;
    switch (o->op) {
    case YTL_OP_ON:        return ytl__seq_ev(q, YTL_ONSET, o->ch, 0.0f, 0, 0);
    case YTL_OP_OFF:       return ytl__seq_ev(q, YTL_OFFSET, o->ch, 0.0f, 0, 0);
    case YTL_OP_SET_VALUE: return ytl__seq_ev(q, YTL_SET, o->ch, o->value, 0, 0);
    case YTL_OP_TRIGGER:   return ytl__seq_ev(q, YTL_TRIGGER, 0, 0.0f, o->code, 0);
    case YTL_OP_MARK:      return ytl__seq_ev(q, YTL_MARK, 0, 0.0f, o->code, o->user);
    case YTL_OP_WAIT:      return ytl__seq_wait(q, o->t_ns);
    case YTL_OP_AT:        return ytl__seq_at(q, o->t_ns);
    case YTL_OP_TO:        return ytl__seq_tween(q, o->ch, &o->tween, &end);
    case YTL_OP_THEN:
        rc = ytl__seq_tween(q, o->ch, &o->tween, &end);
        if (rc == 0) q->t = end;
        return rc;
    default:                 return YTL_ERR_ARG;
    }
}

YTL_API int ytl_run(ytl_seq* q, const ytl_op* ops, int n) {
    int i, rc, bad;
    if (!q) return YTL_ERR_ARG;
    if (!ytl__seq_go(q)) return q->err;
    rc = ytl__check_ops(q->tl, q->base, q->t, q->id, ops, n, &bad);
    if (rc < 0) {
        q->err = rc;
        q->err_call = bad;
        ytl__seq_msg(q);
        return rc;
    }
    for (i = 0; i < n; i++) {
        rc = ytl__seq_op(q, &ops[i]);
        if (rc < 0) {   /* the check makes this unreachable */
            q->err = rc;
            q->err_call = i;
            return rc;
        }
    }
    return 0;
}

YTL_API int ytl_seq_cancel(ytl_seq* q) {
    ytl_timeline* tl;
    int ch, removed;
    if (!q) return YTL_ERR_ARG;
    tl = q->tl;
    if (!tl || !tl->open) return ytl__err(tl, YTL_ERR_CLOSED, "ytl_seq_cancel");
    q->tl = NULL;
    if (q->id == 0) return 0;
    /* Ids rise in add order, so the sequence's events are those of its base
     * from id0 on; the storage is sorted by time, so it cannot be cut at a
     * mark, and one compaction pass deletes them instead. */
    removed = ytl__compact(tl, q->base, false, true, q->id0);
    ytl__recompute(tl, q->base, -1);
    for (ch = 0; ch < tl->n_channels; ch++) {
        ytl__chan* c = &tl->chans[ch];
        if (c->seq_id != q->id) continue;
        if (c->has_pend && c->pend_arena) {
            c->has_pend = 0;
            c->pend_arena = 0;
            if (!c->has_track) tl->track_base[ch] = 0;
        } else if (c->has_track && c->own == 2) {
            ytl__free_chan(tl, ch, false);
        }
        c->seq_id = 0;
    }
    if (tl->tw) ytl__tg_cancel(tl, q->id);
    return removed;
}

/* --- targets (TARGETS) ------------------------------------------------- */

#define YTL__TW_LIVE      0x01u
#define YTL__TW_YOYO      0x02u
#define YTL__TW_STEPPED   0x04u  /* read at the window end, as events land  */
#define YTL__TW_RESOLVED  0x08u  /* from (and to) hold the takeover's values */
#define YTL__TW_FROM_SET  0x10u
#define YTL__TW_TO_CUR    0x20u
#define YTL__TW_KV        0x40u  /* keep_velocity: Hermite, curve = tangents */
#define YTL__TW_IMMEDIATE 0x80u

#define YTL__TG_REST 1u          /* rest[] holds the field before any write */
#define YTL__TG_MARK 2u          /* scratch: overwritten by the call checking */

#define YTL__DUE_START    1u
#define YTL__DUE_COMPLETE 2u

/* keep_velocity's tangent of component i: a Hermite tween has no BEZIER
 * curve, so its curve holds the four tangents. */
static float ytl__tan(const ytl_curve* m, int i) {
    return i == 0 ? m->x1 : i == 1 ? m->y1 : i == 2 ? m->x2 : m->y2;
}

static void ytl__tan_set(ytl_curve* m, int i, float x) {
    if (i == 0) m->x1 = x;
    else if (i == 1) m->y1 = x;
    else if (i == 2) m->x2 = x;
    else m->y2 = x;
}

/* The tween's value at base time t, into v[0..n): the keys ytl_tween()
 * builds (from at s, to at s + d, from again at s + 2d with yoyo) and its
 * repeat, read as ytl__track_at() reads keys. Same expressions, so a target
 * and a channel tween with the same desc give the same bits; the eased
 * fraction is computed once for all components. */
static void ytl__tw_at(const ytl_tween_slot* w, int n, int64_t t, double* v) {
    int64_t s = w->start_ns, d = w->duration_ns, a;
    const float* v0;
    const float* v1;
    bool once = w->cycles == 0 || w->cycles == 1;
    bool yoyo = (w->flags & YTL__TW_YOYO) != 0;
    double u, f;
    int c;
    if (!once && t > s) {
        int64_t per = yoyo ? 2 * d : d, q = (t - s) / per;
        if (w->cycles > 0 && q >= w->cycles) t = s + per;
        else t = s + (t - s) % per;
    }
    if (t < s || (yoyo && t >= s + 2 * d)) {
        for (c = 0; c < n; c++) v[c] = w->from[c];
        return;
    }
    if (!yoyo && t >= s + d) {
        for (c = 0; c < n; c++) v[c] = w->to[c];
        return;
    }
    if (yoyo && t >= s + d) {
        v0 = w->to;
        v1 = w->from;
        a = s + d;
    } else {
        v0 = w->from;
        v1 = w->to;
        a = s;
    }
    u = (double)(t - a) / (double)d;
    switch (w->flags & YTL__TW_KV ? YTL__EASE_HERMITE : w->ease) {
    case YTL_EASE_STEP:
        for (c = 0; c < n; c++) v[c] = v0[c];
        return;
    case YTL_EASE_LOG:
        for (c = 0; c < n; c++) v[c] = (double)v0[c] * exp(u * log((double)v1[c] / (double)v0[c]));
        return;
    case YTL__EASE_HERMITE: {
        double u2 = u * u, u3 = u2 * u;
        for (c = 0; c < n; c++)
            v[c] = (2.0 * u3 - 3.0 * u2 + 1.0) * v0[c] + (u3 - 2.0 * u2 + u) * ytl__tan(&w->curve, c)
                   + (3.0 * u2 - 2.0 * u3) * v1[c];
        return;
    }
    case YTL_EASE_BEZIER: f = ytl__bezier(&w->curve, u); break;
    default:              f = ytl__shape(w->ease, u); break;
    }
    for (c = 0; c < n; c++) v[c] = (double)v0[c] + ((double)v1[c] - (double)v0[c]) * f;
}

/* The field's value before the timeline's first write, read once. */
static void ytl__tg_rest(ytl_target_slot* g) {
    int c;
    if (g->flags & YTL__TG_REST) return;
    for (c = 0; c < g->n; c++)
        g->rest[c] = g->type == YTL_I32 ? (float)((const int32_t*)g->ptr)[c]
                                        : ((const float*)g->ptr)[c];
    g->flags |= YTL__TG_REST;
}

static void ytl__tg_write(const ytl_target_slot* g, const double* v) {
    int c;
    if (g->type == YTL_I32) {
        for (c = 0; c < g->n; c++) {
            double r = floor(v[c] + 0.5);
            ((int32_t*)g->ptr)[c] = r <= -2147483648.0 ? INT32_MIN
                                    : r >= 2147483647.0 ? INT32_MAX : (int32_t)r;
        }
    } else {
        for (c = 0; c < g->n; c++) ((float*)g->ptr)[c] = (float)v[c];
    }
}

/* The takeover: tween wi's start value (and its end value with to_current)
 * is the field's value at its start under the tween before it in the
 * target's list, prev, or the rest value for the first. Computed once and
 * kept, so a rewind replays it, as a channel tween's takeover is kept. The
 * slope for keep_velocity is ytl__take()'s: over the last microsecond. */
static void ytl__tw_resolve(ytl_timeline* tl, ytl_target_slot* g, int32_t wi, int32_t prev) {
    ytl_tween_slot* w = &tl->tw[wi];
    double bv[4], b1[4];
    int c;
    if (w->flags & YTL__TW_RESOLVED) return;
    if (prev >= 0) {
        ytl__tw_at(&tl->tw[prev], g->n, w->start_ns, bv);
        if (w->flags & YTL__TW_KV) ytl__tw_at(&tl->tw[prev], g->n, w->start_ns - 1000, b1);
    } else {
        ytl__tg_rest(g);
        for (c = 0; c < g->n; c++) bv[c] = b1[c] = g->rest[c];
    }
    for (c = 0; c < g->n; c++) {
        if (!(w->flags & YTL__TW_FROM_SET)) w->from[c] = (float)bv[c];
        if (w->flags & YTL__TW_TO_CUR) w->to[c] = (float)bv[c];
        if (w->flags & YTL__TW_KV)
            ytl__tan_set(&w->curve, c, (float)((bv[c] - b1[c]) / 1000.0 * (double)w->duration_ns));
    }
    w->flags |= YTL__TW_RESOLVED;
}

/* One pass over the targets, after the events and channels: a target's
 * value is a function of its base's time and its tweens (the latest started
 * (start, id) drives it), so the pass needs nothing from the frame before
 * but the cursor, which only saves the walk. A STEP tween counts as started
 * when the window end reaches it, as an event lands. */
static void ytl__targets(ytl_timeline* tl, const int64_t* bnow, const int64_t* bhi) {
    ytl_tween_slot* tw = tl->tw;
    int32_t k;
    for (k = 0; k < tl->n_tg; k++) {
        ytl_target_slot* g = &tl->tg[k];
        int32_t c, nx, e, w, p;
        int64_t now, hi;
        double v[4];
        int i;
        if (tl->bases[g->base].state == YTL__STOPPED) continue;
        now = bnow[g->base];
        hi = bhi[g->base];
        c = g->cur;
        if (c >= 0 && tw[c].start_ns > now) c = -1;   /* the base went back */
        nx = c >= 0 ? tw[c].next : g->head;
        while (nx >= 0 && tw[nx].start_ns <= now) {
            ytl__tw_resolve(tl, g, nx, c);
            c = nx;
            nx = tw[c].next;
        }
        g->cur = c;
        e = c;
        for (w = nx; w >= 0 && tw[w].start_ns <= hi; w = tw[w].next)
            if (tw[w].flags & YTL__TW_STEPPED) e = w;
        if (e != c) {
            for (p = c, w = nx;; p = w, w = tw[w].next) {
                ytl__tw_resolve(tl, g, w, p);
                if (w == e) break;
            }
        }
        if (e >= 0) {
            ytl__tw_at(&tw[e], g->n, tw[e].flags & YTL__TW_STEPPED ? hi : now, v);
        } else if (tw[g->head].flags & YTL__TW_IMMEDIATE) {
            ytl__tw_resolve(tl, g, g->head, -1);
            for (i = 0; i < g->n; i++) v[i] = tw[g->head].from[i];
        } else if (g->flags & YTL__TG_REST) {
            for (i = 0; i < g->n; i++) v[i] = g->rest[i];
        } else {
            continue;   /* before the first start: the field is the caller's */
        }
        ytl__tg_rest(g);
        ytl__tg_write(g, v);
    }
}

/* A fired callback event: queue its tween, in firing order, for the calls
 * after the pass. A tween fires at most once per kind per evaluate. */
static void ytl__due(ytl_timeline* tl, const ytl_event* e) {
    ytl_tween_slot* w;
    if (e->target < 0 || e->target >= tl->tw_cap) return;
    w = &tl->tw[e->target];
    if (!(w->flags & YTL__TW_LIVE) || w->id != e->code) return;
    if (!w->due) {
        w->due_next = -1;
        if (tl->due_tail >= 0) tl->tw[tl->due_tail].due_next = e->target;
        else tl->due_head = e->target;
        tl->due_tail = e->target;
    }
    w->due |= (uint8_t)(e->kind == YTL_CALL_START ? YTL__DUE_START : YTL__DUE_COMPLETE);
}

/* After every field is written. A callback may post or kill tweens: a kill
 * unlinks its tween from this queue, and the head is popped before each
 * call, so the queue stays whole. */
static void ytl__calls(ytl_timeline* tl) {
    tl->in_calls = true;
    while (tl->due_head >= 0) {
        int32_t k = tl->due_head;
        ytl_tween_slot* w = &tl->tw[k];
        uint8_t bits = w->due;
        int id = w->id;
        void* user = w->user;
        ytl_anim_fn fc = w->on_complete;
        tl->due_head = w->due_next;
        if (tl->due_head < 0) tl->due_tail = -1;
        w->due = 0;
        if ((bits & YTL__DUE_START) && w->on_start) w->on_start(tl, id, user);
        /* The start callback may have killed it. */
        if ((bits & YTL__DUE_COMPLETE) && fc && (w->flags & YTL__TW_LIVE) && w->id == id)
            fc(tl, id, user);
    }
    tl->in_calls = false;
}

static int32_t ytl__tg_find(const ytl_timeline* tl, const void* p) {
    int32_t k;
    for (k = 0; k < tl->n_tg; k++)
        if (tl->tg[k].ptr == p) return k;
    return -1;
}

/* Delete tween k's callback events, one scan of its base's events each. */
static void ytl__tw_drop_events(ytl_timeline* tl, int32_t k) {
    const ytl_tween_slot* w = &tl->tw[k];
    const ytl__base* b = &tl->bases[tl->tg[w->target].base];
    int i;
    if (!w->on_start && !w->on_complete) return;
    for (i = b->begin + b->count - 1; i >= b->begin; i--) {
        const ytl_event* e = &tl->events[i];
        if ((e->kind == YTL_CALL_START || e->kind == YTL_CALL_COMPLETE) && e->target == k
            && e->code == w->id)
            ytl__delete(tl, i, false);
    }
}

/* Free tween k: out of its target's list and the callback queue, onto the
 * free list. A target with no tween left is freed, and the last target
 * takes its place, so the evaluate's pass reads a dense array. */
static void ytl__tw_free(ytl_timeline* tl, int32_t k, bool drop_events) {
    ytl_tween_slot* w = &tl->tw[k];
    int32_t gi = w->target, *pp;
    ytl_target_slot* g = &tl->tg[gi];
    if (drop_events) ytl__tw_drop_events(tl, k);
    for (pp = &g->head; *pp != k; pp = &tl->tw[*pp].next) {}
    *pp = w->next;
    if (g->cur == k) g->cur = -1;
    if (w->due) {
        int32_t prev = -1, j = tl->due_head;
        while (j != k) {
            prev = j;
            j = tl->tw[j].due_next;
        }
        if (prev < 0) tl->due_head = w->due_next;
        else tl->tw[prev].due_next = w->due_next;
        if (tl->due_tail == k) tl->due_tail = prev;
        w->due = 0;
    }
    w->flags = 0;
    w->next = tl->tw_free;
    tl->tw_free = k;
    tl->n_tw--;
    if (--g->n_tw == 0) {
        int32_t last = --tl->n_tg, j;
        if (gi != last) {
            tl->tg[gi] = tl->tg[last];
            for (j = tl->tg[gi].head; j >= 0; j = tl->tw[j].next) tl->tw[j].target = gi;
        }
    }
}

/* Kill every tween of target gi; the target goes with the last. */
static int ytl__tg_kill(ytl_timeline* tl, int32_t gi, bool drop_events) {
    int n = tl->tg[gi].n_tw, i;
    for (i = 0; i < n; i++) ytl__tw_free(tl, tl->tg[gi].head, drop_events);
    return n;
}

/* ytl_clear(): the base's events are gone already. Downward, so the target
 * that a free moves into gi has been seen. */
static void ytl__tg_clear(ytl_timeline* tl, int base) {
    int32_t gi;
    if (!tl->tw) return;
    for (gi = tl->n_tg - 1; gi >= 0; gi--)
        if (gi < tl->n_tg && (base == YTL_ALL_BASES || tl->tg[gi].base == base))
            ytl__tg_kill(tl, gi, false);
}

/* ytl_seq_cancel(): the sequence's events are gone already. */
static void ytl__tg_cancel(ytl_timeline* tl, uint32_t id) {
    int32_t k;
    for (k = 0; k < tl->tw_cap; k++)
        if ((tl->tw[k].flags & YTL__TW_LIVE) && tl->tw[k].seq_id == id) ytl__tw_free(tl, k, false);
}

/* The sequence's error with a message that names what ran out. */
static int ytl__seq_full(ytl_seq* q, const char* what, int have) {
    if (q->err >= 0) {
        q->err = YTL_ERR_FULL;
        q->err_call = q->n_calls - 1;
        snprintf(q->tl->error, sizeof(q->tl->error),
                 "ysp_timeline: sequence call %d: the %s is full (%d)", q->err_call, what, have);
    }
    return YTL_ERR_FULL;
}

static int ytl__anim(ytl_seq* q, const ytl_anim* a, size_t stride, int n) {
    ytl_timeline* tl = q->tl;
    ytl_tween_desc d, r;
    int64_t cyc, total, last_start, first = 0;
    int ncomp, i, c, rc, n_cb, new_tg = 0, freed_tg = 0, freed_tw = 0, freed_ev = 0;
    int32_t id0;
    bool once;
    if (!a || n < 1 || !a->target) return YTL_ERR_ARG;
    ncomp = a->n == 0 ? 1 : a->n;
    if (ncomp < 1 || ncomp > 4 || (a->type != YTL_F32 && a->type != YTL_I32)) return YTL_ERR_ARG;
    if (a->overwrite != YTL_OVERWRITE_NONE && a->overwrite != YTL_OVERWRITE_ALL) return YTL_ERR_ARG;
    /* The scalar checks are ytl_tween()'s, on component 0; the start is the
     * cursor, as for every tween of a sequence. */
    memset(&d, 0, sizeof d);
    d.to = a->to[0];
    d.from = a->from[0];
    d.from_set = a->from_set;
    d.duration_ns = a->duration_ns;
    d.start_ns = q->t;
    d.start_set = true;
    d.delay_ns = a->delay_ns;
    d.curve = a->curve;
    d.ease = a->ease;
    d.cycles = a->cycles;
    d.yoyo = a->yoyo;
    d.keep_velocity = a->keep_velocity;
    rc = ytl__tween_check(tl, q->base, &d, &r);
    if (rc < 0) return rc;
    for (c = 1; c < ncomp; c++) {
        if (!ytl__finite(a->to[c]) || (a->from_set && !ytl__finite(a->from[c]))) return YTL_ERR_ARG;
        if (a->ease == YTL_EASE_LOG && !(a->from[c] > 0.0f && a->to[c] > 0.0f)) return YTL_ERR_ARG;
    }
    if (a->ease == YTL_EASE_LOG && a->to_current) return YTL_ERR_ARG;
    once = a->cycles == 0 || a->cycles == 1;
    cyc = a->duration_ns * (a->yoyo ? 2 : 1);
    if (a->cycles > 1 && cyc > 0 && a->cycles > (YTL__LIMIT - 1 - r.start_ns) / cyc) return YTL_ERR_ARG;
    total = (once ? 1 : a->cycles > 1 ? a->cycles : 1) * cyc;
    /* Every start and end in range, the last tween's included. */
    if (!ytl__time_ok(a->stagger_ns)) return YTL_ERR_ARG;
    if (a->stagger_ns > 0 && a->stagger_ns > (YTL__LIMIT - 1 - r.start_ns - total) / (n > 1 ? n - 1 : 1))
        return YTL_ERR_ARG;
    if (a->stagger_ns < 0 && -a->stagger_ns > (r.start_ns + YTL__LIMIT - 1) / (n > 1 ? n - 1 : 1))
        return YTL_ERR_ARG;
    last_start = r.start_ns + (int64_t)(n - 1) * a->stagger_ns;
    /* Each field: the same shape as its target, and on this base unless the
     * call overwrites it. Overwritten targets are marked, so the room below
     * counts what their kill frees. */
    for (i = 0; i < n; i++) {
        const void* p = (const char*)a->target + (size_t)i * stride;
        int32_t gi = ytl__tg_find(tl, p);
        if (gi < 0) {
            if (i == 0 || stride != 0) new_tg++;
            continue;
        }
        if (tl->tg[gi].type != a->type || tl->tg[gi].n != ncomp) rc = YTL_ERR_ARG;
        else if (a->overwrite == YTL_OVERWRITE_NONE && tl->tg[gi].base != q->base) rc = YTL_ERR_BOUND;
        else if (a->overwrite == YTL_OVERWRITE_ALL && !(tl->tg[gi].flags & YTL__TG_MARK)) {
            tl->tg[gi].flags |= YTL__TG_MARK;
            freed_tg++;
            freed_tw += tl->tg[gi].n_tw;
            new_tg++;
        }
        if (rc < 0) break;
    }
    if (rc == 0 && freed_tg > 0) {
        for (i = 0; i < tl->n_events; i++) {
            const ytl_event* e = &tl->events[i];
            if ((e->kind == YTL_CALL_START || e->kind == YTL_CALL_COMPLETE) && e->target >= 0
                && e->target < tl->tw_cap && (tl->tw[e->target].flags & YTL__TW_LIVE)
                && tl->tw[e->target].id == e->code
                && (tl->tg[tl->tw[e->target].target].flags & YTL__TG_MARK))
                freed_ev++;
        }
    }
    n_cb = (a->on_start ? 1 : 0) + (a->on_complete && a->cycles != YTL_FOREVER ? 1 : 0);
    if (rc == 0) {
        if (new_tg - freed_tg > tl->tg_cap - tl->n_tg) rc = ytl__seq_full(q, "target pool", tl->tg_cap);
        else if (n - freed_tw > tl->tw_cap - tl->n_tw || n > INT32_MAX - tl->tw_next_id)
            rc = ytl__seq_full(q, "tween pool", tl->tw_cap);
        else if ((int64_t)n * n_cb - freed_ev > tl->capacity - tl->n_events
                 || (int64_t)n * n_cb > INT32_MAX - tl->next_id)
            rc = ytl__seq_full(q, "event storage", tl->capacity);
    }
    if (rc < 0) {
        for (i = 0; i < tl->n_tg; i++) tl->tg[i].flags = (uint8_t)(tl->tg[i].flags & ~YTL__TG_MARK);
        return rc;
    }
    /* Checked: from here on nothing fails. */
    for (i = tl->n_tg - 1; i >= 0; i--)
        if (i < tl->n_tg && (tl->tg[i].flags & YTL__TG_MARK)) ytl__tg_kill(tl, i, true);
    id0 = tl->tw_next_id;
    for (i = 0; i < n; i++) {
        void* p = (char*)a->target + (size_t)i * stride;
        int32_t gi = ytl__tg_find(tl, p), k, *pp;
        ytl_target_slot* g;
        ytl_tween_slot* w;
        if (gi < 0) {
            gi = tl->n_tg++;
            g = &tl->tg[gi];
            memset(g, 0, sizeof *g);
            g->ptr = p;
            g->type = (uint8_t)a->type;
            g->n = (uint8_t)ncomp;
            g->base = (uint8_t)q->base;
            g->head = -1;
            g->cur = -1;
        }
        g = &tl->tg[gi];
        k = tl->tw_free;
        w = &tl->tw[k];
        tl->tw_free = w->next;
        tl->n_tw++;
        g->n_tw++;
        memset(w, 0, sizeof *w);
        w->start_ns = r.start_ns + (int64_t)i * a->stagger_ns;
        w->duration_ns = a->duration_ns;
        for (c = 0; c < ncomp; c++) {
            w->to[c] = a->to_current ? 0.0f : a->to[c];
            w->from[c] = a->from_set ? a->from[c] : 0.0f;
        }
        w->cycles = a->cycles;
        w->ease = (uint8_t)a->ease;
        w->flags = (uint8_t)(YTL__TW_LIVE | (a->yoyo ? YTL__TW_YOYO : 0u)
                             | (a->ease == YTL_EASE_STEP ? YTL__TW_STEPPED : 0u)
                             | (a->from_set ? YTL__TW_FROM_SET : 0u)
                             | (a->to_current ? YTL__TW_TO_CUR : 0u)
                             | (a->keep_velocity ? YTL__TW_KV : 0u)
                             | (a->immediate ? YTL__TW_IMMEDIATE : 0u));
        if (a->ease == YTL_EASE_BEZIER) w->curve = a->curve;
        w->target = gi;
        w->id = tl->tw_next_id++;
        w->seq_id = q->id;
        w->due_next = -1;
        w->user = a->user;
        w->on_start = a->on_start;
        w->on_complete = a->on_complete;
        /* In (start, id) order: the new id is the largest, so it goes after
         * every tween that starts at or before it. One that starts before
         * the cursor's tween makes the next evaluate walk from the head. */
        for (pp = &g->head; *pp >= 0 && tl->tw[*pp].start_ns <= w->start_ns; pp = &tl->tw[*pp].next) {}
        w->next = *pp;
        *pp = k;
        if (g->cur >= 0 && w->start_ns < tl->tw[g->cur].start_ns) g->cur = -1;
        if (n_cb) {
            ytl_event e;
            memset(&e, 0, sizeof e);
            e.base = (uint8_t)q->base;
            e.target = k;
            e.code = w->id;
            e.user = (uint64_t)(uintptr_t)a->user;
            if (a->on_start) {
                e.time_ns = w->start_ns;
                e.kind = YTL_CALL_START;
                ytl__insert(tl, &e);
            }
            if (a->on_complete && a->cycles != YTL_FOREVER) {
                e.time_ns = w->start_ns + total;
                e.kind = YTL_CALL_COMPLETE;
                ytl__insert(tl, &e);
            }
        }
        if (i == 0) first = w->start_ns;
    }
    /* GSAP's "<" and ">": a staggered call is one child, from its first
     * start to its last end; an endless one ends after a cycle. */
    q->prev_start = a->stagger_ns < 0 ? last_start : first;
    q->prev_end = (a->stagger_ns < 0 ? first : last_start)
                  + (a->cycles == YTL_FOREVER ? cyc : total);
    return id0;
}

YTL_API int ytl_animate_n(ytl_seq* q, const ytl_anim* a, size_t stride, int n) {
    if (!ytl__seq_go(q)) return q ? q->err : YTL_ERR_ARG;
    return ytl__seq_fail(q, ytl__anim(q, a, stride, n));
}

YTL_API int ytl_animate(ytl_seq* q, const ytl_anim* a) {
    return ytl_animate_n(q, a, 0, 1);
}

static int ytl__tween1(ytl_seq* q, float* field, float from, float to, int64_t dur, int ease,
                       bool from_set, bool to_current) {
    ytl_anim a;
    memset(&a, 0, sizeof a);
    a.target = field;
    a.to[0] = to;
    a.from[0] = from;
    a.from_set = from_set;
    a.to_current = to_current;
    a.immediate = from_set;
    a.duration_ns = dur;
    a.ease = ease;
    return ytl_animate(q, &a);
}

YTL_API int ytl_tween_to(ytl_seq* q, float* field, float to, int64_t duration_ns, int ease) {
    return ytl__tween1(q, field, 0.0f, to, duration_ns, ease, false, false);
}

YTL_API int ytl_tween_from(ytl_seq* q, float* field, float from, int64_t duration_ns, int ease) {
    return ytl__tween1(q, field, from, 0.0f, duration_ns, ease, true, true);
}

YTL_API int ytl_tween_from_to(ytl_seq* q, float* field, float from, float to,
                              int64_t duration_ns, int ease) {
    return ytl__tween1(q, field, from, to, duration_ns, ease, true, false);
}

YTL_API int ytl_tween_set(ytl_seq* q, float* field, float value) {
    return ytl__tween1(q, field, 0.0f, value, 0, YTL_EASE_STEP, false, false);
}

static int ytl__kill(ytl_timeline* tl, const void* field) {
    int32_t gi;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!field) return YTL_ERR_ARG;
    gi = ytl__tg_find(tl, field);
    return gi < 0 ? 0 : ytl__tg_kill(tl, gi, true);
}

static int ytl__kill_tween(ytl_timeline* tl, int id) {
    int32_t k;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    for (k = 0; k < tl->tw_cap; k++) {
        if ((tl->tw[k].flags & YTL__TW_LIVE) && tl->tw[k].id == id) {
            ytl__tw_free(tl, k, true);
            return 0;
        }
    }
    return YTL_ERR_NOT_FOUND;
}

YTL_API int ytl_tween_count(const ytl_timeline* tl, int* n_targets) {
    if (n_targets) *n_targets = tl && tl->open ? tl->n_tg : 0;
    return tl && tl->open ? tl->n_tw : 0;
}

/* --- the frame --------------------------------------------------------- */

/* RT target of a fired event, the report's sort key: when its base reaches
 * its time. onset - residual is that on a base at rate 1 (and the peek
 * copies' RT time, with tl NULL), but residual is base ns, so on a scaled
 * base two bases at different rates would sort by lateness in base ns. A
 * paused base has no RT time for an event; onset - residual keeps v0.3.0's
 * order there. A clamped key (an event the base reached before -2^62 RT
 * ns) only orders the report; no record holds it. */
static int64_t ytl__target(const ytl_timeline* tl, const ytl_event* e) {
    if (tl) {
        const ytl__base* b = &tl->bases[e->base];
        if (b->scaled && b->state == YTL__RUNNING) {
            bool sat = false;
            return ytl__rt_s(b, e->time_ns, &sat);
        }
    }
    return e->onset - e->residual;
}

static bool ytl__before(const ytl_timeline* tl, const ytl_event* a, const ytl_event* b) {
    int64_t ta = ytl__target(tl, a), tb = ytl__target(tl, b);
    return ta < tb || (ta == tb && a->id < b->id);
}

/* Insert into the report, which holds the `cap` earliest so far. */
static void ytl__report(const ytl_timeline* tl, ytl_event* out, int cap, int n_out,
                          const ytl_event* e) {
    int j = n_out < cap ? n_out : cap - 1;
    if (cap <= 0) return;
    if (n_out >= cap && !ytl__before(tl, e, &out[cap - 1])) return;
    while (j > 0 && ytl__before(tl, e, &out[j - 1])) {
        out[j] = out[j - 1];
        j--;
    }
    out[j] = *e;
}

static int ytl__evaluate(ytl_timeline* tl, const ytl_frame* f,
                             ytl_event* fired, int cap) {
    int64_t lead_ns, bnow[YTL_MAX_BASES], bhi[YTL_MAX_BASES];
    int bi, i, n_fired = 0;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!f || f->period < 0 || cap < 0 || (cap > 0 && !fired)) return YTL_ERR_ARG;
    if (tl->in_calls) return YTL_ERR_ORDER;   /* from a target's callback */
    if (tl->has_onset && f->onset < tl->last_onset) return YTL_ERR_ORDER;
    tl->has_onset = true;
    tl->last_onset = f->onset;
    lead_ns = ytl__lead_ns(tl, f->period);

    for (bi = 0; bi < YTL_MAX_BASES; bi++) {
        ytl__base* b = &tl->bases[bi];
        int64_t now, hi_t;
        int begin, i_hi, start, hi_kept;
        bool sat = false;
        if (b->state == YTL__STOPPED) continue;
        /* The window ends at RT onset + lead x period on every base: the
         * lead is RT ns and is not scaled by the rate, so an event lands on
         * the frame nearest to when its base reaches it (ytl_window). */
        now = ytl__bt_s(b, f->onset, &sat);
        hi_t = ytl__bt_s(b, f->onset + lead_ns, &sat);
        if (sat && b->clamped < INT32_MAX) b->clamped++;
        begin = b->begin;
        i_hi = ytl__upper(tl->events, begin, begin + b->count, hi_t) - begin;

        /* The window can end behind the base's reach (a pause with a
         * lead, an anchor whose RT time is past this onset). Nothing
         * un-fires then: a frame already showed those events, and only an
         * anchor rewinds. Late events up to the window fire. */
        hi_kept = b->hi < i_hi ? b->hi : i_hi;
        start = b->dirty < hi_kept ? b->dirty : hi_kept;
        for (i = start; i < i_hi; i++) {
            ytl_event* e = &tl->events[begin + i];
            if (e->flags & YTL__DONE) continue;
            e->flags = (uint16_t)(YTL_EV_FIRED | (i < hi_kept ? YTL_EV_LATE : 0u));
            e->frame = f->index;
            e->onset = f->onset;
            e->residual = now - e->time_ns;
            if (ytl__drives(e->kind)) ytl__apply(tl, e);
            else if (e->kind == YTL_CALL_START || e->kind == YTL_CALL_COMPLETE) ytl__due(tl, e);
            ytl__report(tl, fired, cap, n_fired, e);
            n_fired++;
        }
        if (i_hi < b->hi) {
            int d = b->dirty > i_hi ? b->dirty : i_hi;
            while (d < b->hi && (tl->events[begin + d].flags & YTL__DONE)) d++;
            b->dirty = d < b->hi ? d : YTL__NONE;
        } else {
            /* i_hi == hi with hi_t behind the reach is possible when no
             * event lies between: the reach must not move back, or an
             * event added there later would not count as late. */
            b->hi = i_hi;
            b->dirty = YTL__NONE;
            if (!b->evaluated || hi_t > b->last_hi) b->last_hi = hi_t;
        }
        if (!b->evaluated || now > b->last_now) b->last_now = now;
        b->now_bt = now;
        b->evaluated = 1;
        bnow[bi] = now;
        bhi[bi] = hi_t;

        for (i = 0; i < tl->n_channels; i++) {
            ytl__chan* c = &tl->chans[i];
            if (tl->track_base[i] != bi + 1) continue;
            if (c->has_pend && (c->pend_stepped ? hi_t : now) >= c->pend.start_ns)
                ytl__promote(tl, i);
            if (c->has_track)
                tl->values[i] = ytl__sample_chan(tl, c, c->stepped ? hi_t : now);
        }
    }
    if (tl->n_tg > 0) ytl__targets(tl, bnow, bhi);
    if (tl->due_head >= 0) ytl__calls(tl);
    return n_fired;
}

YTL_API bool ytl_next_due(const ytl_timeline* tl, int64_t* rt) {
    int bi, i;
    bool found = false;
    int64_t best = 0;
    if (!tl || !tl->open || !rt) return false;
    for (bi = 0; bi < YTL_MAX_BASES; bi++) {
        const ytl__base* b = &tl->bases[bi];
        int start;
        if (b->state != YTL__RUNNING) continue;
        start = b->dirty < b->hi ? b->dirty : b->hi;
        for (i = start; i < b->count; i++) {
            const ytl_event* e = &tl->events[b->begin + i];
            int64_t t;
            bool sat = false;
            if (e->flags & YTL__DONE) continue;
            /* +-2^62 for an event the base reaches only past the range: a
             * time no caller's clock reaches, not a clamp of a real one. */
            t = ytl__rt_s(b, e->time_ns, &sat);
            if (!found || t < best) best = t;
            found = true;
            /* Past hi the events are sorted and unfired: the first is it. */
            if (i >= b->hi) break;
        }
    }
    if (found) *rt = best;
    return found;
}

static int ytl__peek(ytl_timeline* tl, int base, int64_t from_rt, int64_t to_rt,
                         ytl_event* out, int cap) {
    int bi, i, n = 0;
    if (!tl || !tl->open) return YTL_ERR_CLOSED;
    if (!ytl__base_arg(base) || cap < 0 || (cap > 0 && !out) || from_rt > to_rt
        || !ytl__time_ok(from_rt) || !ytl__time_ok(to_rt))
        return YTL_ERR_ARG;
    for (bi = 0; bi < YTL_MAX_BASES; bi++) {
        const ytl__base* b = &tl->bases[bi];
        int lo, hi;
        if (base != YTL_ALL_BASES && base != bi) continue;
        if (b->state != YTL__RUNNING) continue;
        /* An event's RT time is the first RT ns at which the base reaches
         * it, so it is in [from_rt, to_rt) exactly when the base time at
         * from_rt - 1 is before it and the one at to_rt - 1 is not. Put
         * this way the bounds stay right for any mapping that rounds down,
         * not only for rate 1. */
        /* A clamped bound is +-2^62, beyond every event's time, so the
         * search still finds exactly the events in the window, and their
         * RT times are inside it, so none of those is clamped. */
        {
            bool sat = false;
            lo = ytl__upper(tl->events, b->begin, b->begin + b->count, ytl__bt_s(b, from_rt - 1, &sat));
            hi = ytl__upper(tl->events, lo, b->begin + b->count, ytl__bt_s(b, to_rt - 1, &sat));
        }
        for (i = lo; i < hi; i++) {
            const ytl_event* e = &tl->events[i];
            ytl_event c;
            bool sat = false;
            if (e->flags & YTL__DONE) continue;
            c = *e;
            c.onset = ytl__rt_s(b, e->time_ns, &sat);
            c.residual = 0;
            ytl__report(NULL, out, cap, n, &c);
            n++;
        }
    }
    return n;
}

YTL_API double ytl_lead(const ytl_timeline* tl) {
    return tl && tl->open ? tl->lead : 0.0;
}

/* --- state and record -------------------------------------------------- */

YTL_API const float* ytl_values(const ytl_timeline* tl) {
    return tl && tl->open ? tl->values : NULL;
}

YTL_API float ytl_value(const ytl_timeline* tl, int channel) {
    if (!tl || !tl->open || channel < 0 || channel >= tl->n_channels) return 0.0f;
    return tl->values[channel];
}

YTL_API const ytl_event* ytl_events(const ytl_timeline* tl, int base, int* n) {
    if (n) *n = 0;
    if (!tl || !tl->open || !ytl__base_arg(base)) return NULL;
    if (base == YTL_ALL_BASES) {
        if (n) *n = tl->n_events;
        return tl->events;
    }
    if (n) *n = tl->bases[base].count;
    return tl->events ? tl->events + tl->bases[base].begin : NULL;
}

YTL_API const ytl_event* ytl_find(const ytl_timeline* tl, int id) {
    int k;
    if (!tl || !tl->open) return NULL;
    for (k = 0; k < tl->n_events; k++)
        if (tl->events[k].id == id) return &tl->events[k];
    return NULL;
}

/* --- the calls that write the message (ERRORS AND CLOSE) ---------------- */

YTL_API int ytl_anchor(ytl_timeline* tl, int base, int64_t rt_ns, int64_t base_time_ns) {
    return ytl__err(tl, ytl__anchor(tl, base, rt_ns, base_time_ns), "ytl_anchor");
}
YTL_API int ytl_pause(ytl_timeline* tl, int base, int64_t rt_ns) {
    return ytl__err(tl, ytl__pause(tl, base, rt_ns), "ytl_pause");
}
YTL_API int ytl_resume(ytl_timeline* tl, int base, int64_t rt_ns) {
    return ytl__err(tl, ytl__resume(tl, base, rt_ns), "ytl_resume");
}
YTL_API int ytl_stop(ytl_timeline* tl, int base) {
    return ytl__err(tl, ytl__stop(tl, base), "ytl_stop");
}
YTL_API int ytl_skip(ytl_timeline* tl, int base, int64_t rt_ns, int64_t base_time_ns) {
    return ytl__err(tl, ytl__skip(tl, base, rt_ns, base_time_ns), "ytl_skip");
}
YTL_API int ytl_rate(ytl_timeline* tl, int base, int64_t rt_ns, int32_t num, int32_t den) {
    return ytl__err(tl, ytl__rate(tl, base, rt_ns, num, den), "ytl_rate");
}
YTL_API int ytl_clamped(ytl_timeline* tl, int base) {
    return ytl__err(tl, ytl__clamped(tl, base), "ytl_clamped");
}
YTL_API int ytl_add(ytl_timeline* tl, const ytl_event* e) {
    return ytl__err(tl, ytl__add(tl, e), "ytl_add");
}
YTL_API int ytl_add_n(ytl_timeline* tl, const ytl_event* e, int n) {
    return ytl__err(tl, ytl__add_n(tl, e, n), "ytl_add_n");
}
YTL_API int ytl_remove(ytl_timeline* tl, int id) {
    return ytl__err(tl, ytl__remove(tl, id), "ytl_remove");
}
YTL_API int ytl_prune(ytl_timeline* tl, int base) {
    return ytl__err(tl, ytl__prune(tl, base), "ytl_prune");
}
YTL_API int ytl_clear(ytl_timeline* tl, int base) {
    return ytl__err(tl, ytl__clear(tl, base), "ytl_clear");
}
YTL_API int ytl_set_keys(ytl_timeline* tl, int channel, int base, const ytl_key* keys, int n) {
    return ytl__err(tl, ytl__set_keys(tl, channel, base, keys, n), "ytl_set_keys");
}
YTL_API int ytl_set_track(ytl_timeline* tl, int channel, int base, const ytl_track* tr) {
    return ytl__err(tl, ytl__set_track(tl, channel, base, tr), "ytl_set_track");
}
YTL_API int ytl_tween(ytl_timeline* tl, int channel, int base, const ytl_tween_desc* d) {
    return ytl__err(tl, ytl__tween(tl, channel, base, d), "ytl_tween");
}
YTL_API int ytl_kill(ytl_timeline* tl, const void* field) {
    return ytl__err(tl, ytl__kill(tl, field), "ytl_kill");
}
YTL_API int ytl_kill_tween(ytl_timeline* tl, int id) {
    return ytl__err(tl, ytl__kill_tween(tl, id), "ytl_kill_tween");
}
YTL_API int ytl_evaluate(ytl_timeline* tl, const ytl_frame* f, ytl_event* fired, int cap) {
    return ytl__err(tl, ytl__evaluate(tl, f, fired, cap), "ytl_evaluate");
}
YTL_API int ytl_peek(ytl_timeline* tl, int base, int64_t from_rt_ns, int64_t to_rt_ns,
                     ytl_event* out, int cap) {
    return ytl__err(tl, ytl__peek(tl, base, from_rt_ns, to_rt_ns, out, cap), "ytl_peek");
}

#endif /* YSP_TIMELINE_IMPLEMENTATION_GUARD */
#endif /* YSP_TIMELINE_IMPLEMENTATION */

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
