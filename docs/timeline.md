# ysp/timeline.h design notes

This note explains why `ysp/timeline.h` has its current form. The manual
in the header tells you how to use it. The plan that asked for the header
is section 4.5 of [rig_spec.md](rig_spec.md).

## Time is an integer

All times are `int64_t` nanoseconds. `yrt_now_ns()` returns this unit,
so a time from `ysp/rt.h` goes into the timeline with a cast.

The alternative was `double` seconds. We did not use it, for two reasons:

- Quantization compares an event time with a frame window. With integers,
  the comparison is exact. With `double`, an event at 0.5 s and a frame
  onset at 30/60 s can differ in the last bit, so the event can land one
  frame late.
- Replay must give the same bytes from the same inputs. Integer addition
  and comparison give the same result on every compiler. Only the track
  easings use `double`, and only `COSINE` and `LOG` call libm.

The range limit is 2^62 ns, about 146 years. With this limit, the sum of
two times cannot overflow.

## The state is a set of float channels

The spec says that evaluate "fills a state struct". The header does not
know the caller's stimuli, so the state is an array of floats. The caller
decides what each channel means. Onset and offset set a channel to 1 and
0, so visibility is a channel too. The array is contiguous, so the caller
can upload it as a uniform block.

## One driver and one base per channel

A channel takes events or keys, not both. All its drivers are on one
base. These rules keep one property true: a channel's value is a function
of which events have fired. That property is necessary for a rewind. When
a base moves back, the header resets the channels of that base and applies
the events that are still fired. If two bases could write one channel,
the order of their writes would be the order of the frames, and a rewind
of one base could not recompute the channel.

For the same reason, a late event does not overwrite a later event that
fired first. The value comes from the fired event with the latest
(time, id), not from the most recent write.

## The default lead is half a frame

An event lands on the frame nearest to its time. A desc lead of 0 means
this default, 0.5. `YTL_LEAD_NONE` gives the first frame whose onset is
at or after the event, which never shows an event early.

v0.1.0 had "never early" as the default, because it was the first rule
for `wait(dt)` in the spec. A measurement changed it. A predicted onset
carries noise, and a frame-aligned time is then a coin toss between two
frames. With 2 us of noise on a 60 Hz display, "never early" put about
12% of frame-aligned changes a frame late, and the nearest frame put none
on the wrong frame (see "STEP tracks are quantized like events"). The
spec's `wait(dt)` rule now uses the nearest frame too.

## Only an anchor rewinds

The first implementation rewound at evaluate. When a frame window ended
before the previous window, the fired events after the new window became
pending again. Two problems showed:

- The example program found the first. When the caller anchored a trial
  base at 0 again, the event at trial time 0 did not fire again. That
  event was inside the new window, so the rewind did not touch it.
- The independent test found the second. With a lead, a pause can freeze
  a base before an event that a frame already showed early. The next
  evaluate un-fired that event, and after the resume it fired again. A
  trigger went out twice.

Now `ytl_anchor()` is the only call that rewinds. An anchor at base
time `bt`, at or before the furthest base time evaluated on the base,
makes pending every fired event at or after `bt`. The event at `bt` is
included. An evaluate never un-fires an event, because a frame already
showed it.

The cost of this rule: a small backward re-anchor to correct a drift
fires again the events in that small interval. The manual says to correct
forward only.

## Tracks: keys, samples, repeats and tweens

A keyed track suits a few changes: a ramp, a fade, a path through a dozen
points. A long trajectory, such as a 10-minute sum-of-sines target for a
tracking task, needs a different form. As keys it costs 16 bytes per
point, and 8 of those bytes repeat a fixed step. So a track has a second
form: samples at a fixed rate, 4 bytes each, with no time column.

### The sample rate is an integer, not a step in ns

The first version stored the step in whole nanoseconds. At 120 samples/s
the step is 8,333,333.33 ns, so each sample was a third of a nanosecond
early, and after 10 minutes the table was 24 us early. On a moving target
that offset is an error. The example program found it. Sample i is now at
exactly t0_ns + i * 1e9 / rate, and the lookup splits the time into seconds
and nanoseconds, so every product stays below 2^63 for any rate up to 1e9.

Measured on the 5 Hz sum of sines below, CUBIC at 120 samples/s, worst
error away from the ends, as a fraction of the peak:

| Sample placement | Error |
|---|---|
| Step of whole ns (first version) | 6.44e-5 |
| Integer rate (now) | 7.34e-6 |

A rate that is not an integer, such as 59.94, is a resampling job for
the pack tool. That follows the rule of one canonical form per resource.

### How fine a table must be

Two signals, each the sum of seven sines with amplitude 1/f, 10 minutes
long:
- Set A (0.1 to 2.05 Hz) is typical of visuomotor tracking.
- Set B (0.15 to 4.95 Hz) is a harder case.

The table rate varies. The test reads the table at 2 million random times
and compares it with the analytic signal. The error is the worst absolute
error as a fraction of the peak (the sum of the amplitudes), away from the
first and last segment.

| Table rate | Size, 10 min | Set A linear | Set A cubic | Set B linear | Set B cubic |
|---|---|---|---|---|---|
| 60/s | 0.14 MB | 4.4e-4 | 8.1e-6 | 1.3e-3 | 6.1e-5 |
| 120/s | 0.29 MB | 1.1e-4 | 1.0e-6 | 3.2e-4 | 7.4e-6 |
| 250/s | 0.60 MB | 2.5e-5 | 1.3e-7 | 7.4e-5 | 8.2e-7 |
| 500/s | 1.2 MB | 6.4e-6 | 5.9e-8 | 1.8e-5 | 1.3e-7 |
| 1000/s | 2.4 MB | 1.6e-6 | 5.5e-8 | 4.6e-6 | 5.0e-8 |

Float rounding of the samples alone is 4e-8 to 5e-8 of the peak. For a
target that moves 1000 pixels from end to end, 1e-5 is 0.01 pixel.
Recommendation: use CUBIC at 120 samples/s or more. LINEAR needs about 8
times the rate for the same error, and its velocity jumps at every sample,
which matters if the analysis uses velocity.

### The ends of a cubic table

Catmull-Rom needs one sample beyond each end. The first version repeated
the end sample. That makes the slope zero at the end, and the error in the
first and last segment was 5.55e-5 (set B, 120/s). A sample extrapolated
in a line (v_-1 = 2 v_0 - v_1) gives 1.73e-5. The interior stays at
7.3e-6. A table that starts before the experiment needs it removes the
remaining error at the ends.

### Why samples and not sines at run time

The runtime could compute the sum of sines itself. It does not, for two
reasons:
- `sin()` differs in the last bit between C libraries, so the trajectory
  would differ between platforms.
- Interpolating a table is +, -, * and /, so a table that the pack tool
  computes once gives the same bits everywhere.

The cost is memory: 288 KB for 10 minutes at 120 samples/s.

### Repeats

A flicker, a counterphase reversal or an SSVEP drive is periodic. A
repeat folds the time into one cycle, so a 10-minute 7.5 Hz square wave
takes two keys, not 9000. The period is in whole nanoseconds. At 7.5 Hz
the period is 133,333,333 ns, a third of a nanosecond short per cycle,
which adds up to 1.5 us after 10 minutes.

### STEP tracks are quantized like events

A frame-locked flicker has its edges exactly on frame onsets: a 7.5 Hz
square wave on a 60 Hz display changes every 4 frames. Frame onsets are
rounded to whole nanoseconds, and a predicted onset carries estimation
noise. So an edge read exactly at the onset falls on either side of it.
Each test below runs 10 minutes and counts the frames whose state
disagrees with the frame count (the state changes every 4 frames at
60 Hz).

Edges at exact rational times, onsets rounded to ns, no noise:

| Read at | Wrong frames, 60 to 600 Hz |
|---|---|
| The onset | 3000 at every rate (of 36,000 to 360,000) |
| Onset + half a period | 0 |

The header's truncated period read at the onset also gave 0. That result
is luck: every truncated edge is a fraction of a nanosecond early, so it
never falls after a rounded onset.

The header at 60 Hz, with uniform noise added to the predicted onsets.
Channel 0 is a STEP track with a repeat. Channel 1 is 9000 SET events at
the edges:

| lead | Onset noise | STEP track wrong | Events wrong |
|---|---|---|---|
| None (never early) | 0 | 0 | 0 |
| None (never early) | +-2 us | 2791 | 4399 |
| None (never early) | +-20 us | 4350 | 4518 |
| 0.5 (the default) | 0 | 0 | 0 |
| 0.5 (the default) | +-2 us | 0 | 0 |
| 0.5 (the default) | +-20 us | 0 | 0 |

So a STEP track is read at the end of the lead window, like an event. A
ramp is still read at the onset, because it must show its value at the
moment the frame appears. Never early, a frame-aligned change is a coin
toss under any onset noise, for events and STEP tracks alike.

### Tweens

A script needs a fade that starts where the value is now, and it cannot
keep an array alive for it. `ytl_tween()` stores its keys (three with
yoyo), its Bezier curve and a waiting tween's desc in the channel record.
With this storage, a channel record is 232 bytes and the handle is 60 KB
(it was 12 KB in v0.1.0). An evaluate reads a byte per channel to find
the tracked channels, so the size does not add to the frame cost. A rig
that needs fewer channels sets `YTL_MAX_CHANNELS`.

The tween takes a desc (`ytl_tween_desc`), not eight arguments. The
reasons: principle 8 of the spec (every parameter a designer can set is
a plain value in a desc), two `int64_t` times that could be swapped
silently, an ease and a curve that had to agree, and fields that the
script draft needs (`from`, `cycles`, `yoyo`, `keep_velocity`). With C99
designated initializers, a call is about as short as GSAP's `to()`.

### The takeover

The first `ytl_tween()` replaced the channel's track at the call and
started from the value of the last evaluated frame. A script posts its
tween just before the next frame is evaluated, so the tween started one
frame late from a value one frame old. A moving object repeated its
position for one frame.

Now the tween waits until its start while the old track runs on, as
GSAP lets an overwritten tween run until the new one starts. At the
start, it takes the old track's value at that base time. With
`keep_velocity`, it also takes the old track's slope, and a cubic
Hermite segment brings the value to rest at the end value.

A dot moves at 10 units/s on a 500 Hz display. A script retargets it in
mid-flight to a speed of 15 units/s:

| Tween | Repeated positions | Largest step in frame-to-frame velocity |
|---|---|---|
| First version | 1 | 15.0 /s (10, then 0, then 15) |
| Takeover | 0 | 5.0 /s (10, then 15: what a linear retarget asks for) |
| Takeover with `keep_velocity` | 0 | 0.10 /s |

A step in target velocity is a confound for smooth pursuit, which
responds to velocity steps. `keep_velocity` removes it.

### Bezier easing

`YTL_EASE_BEZIER` is CSS `cubic-bezier()`, the curve designers already
edit with handles. A key holds an index into the track's curve table, so
a key stays 16 bytes. The solver uses Newton steps from s = u and
bisection when a step leaves [0, 1]. It is plain arithmetic, so replay
stays exact.

### Measured cost

The `tracks` workload of `timeline_bench` runs four kinds of track for 10
minutes: a sampled CUBIC path at 120/s, the same length as 600,001 keys, a
repeating flicker, and four Bezier channels. With gcc under WSL2 it costs
310 ns mean and 521 ns p99 at 60 Hz, and 241 ns mean and 311 ns p99 at
500 Hz. The p99 is 0.016% of a 2 ms frame.

## Skip: a seek that does not fire what it passes over

Before v0.3.0, a seek was an anchor. A forward anchor fires every event
that the base passed over on the next frame, late by its residual. The
ysp/video.h manual-mode test fired 148 annotations on one display frame
after a seek. On a movie base with one annotation per second, a
30-minute seek fires 1800. Each one is a false record: a trigger sent, a
coroutine resumed, a MARK logged, all at the wrong time.

`ytl_skip(tl, base, rt, bt)` is an anchor, then a mark on every pending
event of the base before `bt`. The mark is `YTL_EV_SKIPPED`. A skipped
event never fires. The return value is the count.

Decisions:
- **The call takes `rt`.** The request was `ytl_skip(tl, base, bt)`.
  A running base needs an RT time for its new position. Without `rt`, a
  seek is two calls, a skip and an anchor, that must agree on `bt`.
- **A skip back is an anchor back.** It rewinds as an anchor does, and
  then it skips the late events before `bt` that still wait for a window.
  The other choice was to refuse a skip back. The caller cannot read the
  furthest evaluated base time, so it cannot know whether a seek goes
  back. With this rule a seek calls `ytl_skip()` in both directions.
- **A skipped event counts for its channel.** After a seek, the channels
  show what continuous play shows at `bt`. The other choice keeps the
  values from before the seek: a seek past an onset then shows a
  stimulus that should be visible as hidden.
- **Only events before `bt`.** The event at `bt` fires on its frame, as
  the event at an anchor's `bt` fires after a rewind.
- **The reach and the furthest onset move to just before `bt`.** If the
  reach stays, an event added after the skip at a time before `bt` is not
  late. If the furthest onset stays, a later anchor back does not make
  the skipped events pending again. The model mutations `MUT_SKIP_REACH`
  and `MUT_SKIP_NOW` are these two errors, and the test finds both.
- **A new flag, not FIRED.** A caller that reads FIRED as "landed on a
  frame" stays correct. A skipped event has frame -1, onset = the skip's
  `rt` and residual = `bt` minus its time, so the record shows the seek.
- **Prune deletes skipped events** with the fired ones. Both are behind
  the base.

Tracks need nothing: a track is a function of base time. A tween that
waits for a start before `bt` takes over at the next evaluate, from the
old track's value at its start. The test compares a skip with an anchor
to the same place: the values are the same bits.

A skip costs a binary search and one write per skipped event. This note
did not time it.

## Peek: the look-ahead for sound

An evaluate fires an event on the frame that shows it, at most
lead x period early. ysp/audio.h needs an onset 43.5 to 54.5 ms before
its time ([audio.md](audio.md)), and it places a sound at the
event's own time, not at a frame. `ytl_peek(tl, base, from_rt, to_rt,
out, cap)` lists the pending events whose RT time is in `[from_rt,
to_rt)` and changes nothing.

Decisions:
- **A half-open window with a start.** The draft in rig_spec.md 4.5.1
  had only an end (`until_rt`). With a start, the audio path peeks
  `[last to_rt, now + lead)` on each frame. The windows follow each other
  with no overlap and no gap, so "already scheduled" is a range.
- **The RT time goes in `onset`, with residual 0.** In a report,
  onset - residual is the event's RT time. In a peek copy it is the same.
  The event is 64 bytes and has no free field for a second time.
- **Pending events only.** A fired event has its frame's record. An
  event added less than the audio lead before its time can be in no peek
  before a frame fires it. It is in that evaluate's report, and the
  caller plays it late from there.
- **Running bases only.** A paused base has no RT time for its events.
  The mapping gives an empty window for a paused base on its own, so the
  state check is there for a stopped base, whose anchor is stale.
- **The bounds are written as `bt(from_rt - 1) < time <= bt(to_rt - 1)`.**
  At rate 1 this is the same as converting the bounds directly. It also
  stays correct for any mapping that rounds down, which is what a rate
  needs (see "Base rates").

The test runs the audio path's use for 22 s at 60 Hz with a look-ahead of
55 ms plus a frame. Every one of 200 events was in exactly one peek, at
least 55 ms before its RT time and before the frame that fired it, and
fired once.

`ytl_lead()` returns 0 for `YTL_LEAD_NONE`, not -1. The value is the
one that multiplies the period, and ysp/video.h's own lead field uses the
same convention, so ysp/video.h can replace its read of `tl->lead` with
`ytl_lead(tl)` and change nothing else.

## Base rates

v0.4.0 runs a base at an exact rational rate. Slow motion and time
scaling are in the timeline: ysp/video.h reads movie time from the movie
base, so a video gets its rate from the base and plays at 1x inside
ysp/video.h. A soundtrack cannot follow a rate other than 1, because
ysp/audio.h does not resample, so ysp/video.h refuses that combination.

```c
int  ytl_rate(ytl_timeline* tl, int base, int64_t rt, int32_t num, int32_t den);
bool ytl_get_rate(const ytl_timeline* tl, int base, int32_t* num, int32_t* den);
bool ytl_rt_time(const ytl_timeline* tl, int base, int64_t bt, int64_t* rt);
int  ytl_clamped(const ytl_timeline* tl, int base);
```

The JS animation libraries have the same call:

| JS | C |
|---|---|
| GSAP `movie.timeScale(0.5)`; anime.js `anim.speed = 0.5`; Motion `controls.speed = 0.5` | `ytl_rate(&tl, MOVIE, next_onset, 1, 2)` |
| GSAP `movie.timeScale()` | `ytl_get_rate(&tl, MOVIE, &n, &d)` |
| GSAP `movie.time()`; Motion `controls.time` | `ytl_base_time(&tl, MOVIE, onset, &bt)` |
| none (computed by hand from `startTime()`) | `ytl_rt_time(&tl, MOVIE, bt, &rt)` |
| GSAP `movie.seek(12.5)`, which suppresses events by default | `ytl_skip(&tl, MOVIE, onset, YRT_MS(12500))` |
| GSAP `timeScale(0)` | refused; `ytl_pause()` |
| GSAP `reverse()`, `timeScale(-1)` | refused |

### Decisions

- **The call takes `rt`.** The first request was
  `ytl_rate(tl, base, num, den)`. The JS calls read their ticker's
  clock. This header reads no clock, so without a change point the new
  rate would apply from the last anchor and the base time would jump at
  the call. A sentinel for "the next evaluated onset" was rejected: it
  needs a pending-rate field and a branch per base per frame, and the
  caller already has the predicted onset it is about to evaluate.
- **num and den are in [1, 2^31 - 1], reduced by their gcd.** Equal
  rates are equal bytes, and 1/1 is the only form of rate 1. 1001/1000
  and 24000/1001 are exact. A double form (`0.5`) was not added. The
  script layer converts a number to num/den with den <= 1000, and the log
  records the fraction.
- **A change is not an anchor.** It re-anchors at (rt, bt(rt)) with the
  new rate and does not touch the fired set. When rt is the last
  evaluated onset, bt(rt) is the furthest evaluated base time, where an
  anchor rewinds: the event there would fire twice. The model mutation
  MUT_RATE_REWIND and a header mutation (the change through
  `ytl_anchor()`) are both caught.
- **"now" moves to the change point.** A rate change on a running base
  sets the base's "now" to bt(rt), as a pause sets it to the frozen time.
  A tween posted after the change then starts where the base is at the
  change. A change on a paused or stopped base, or to the rate the base
  already has, changes nothing at all.
- **Rate 0 and negative rates are refused.** `ytl_pause()` is the one way
  to freeze a base. At rate 0 the RT time of a base time does not exist,
  and two frozen states would double the cases that every rule and the
  model must cover. A base time that goes back conflicts with "an evaluate
  never un-fires". Every window would end behind the reach, so events
  would neither fire nor un-fire: an onset at 5 s, passed backward from
  10 s to 3 s, would leave the stimulus visible. A reverse scrub is a
  series of `ytl_skip()` calls, and each is a rewind in the record.
  ysp/video.h cannot decode backward in any case.
- **The lead stays RT time.** The window of a frame ends at the base time
  at RT time onset + lead x period, not at the base time at the onset
  plus lead x period. With the first form, an event lands on the frame
  nearest to when its base reaches it, at any rate.
- **Tweens run in base time,** like GSAP's timeScale on a parent
  timeline. At rate 1/2 a 200 ms tween takes 400 ms of RT. This is also
  the only rule that keeps "a tween is a track". A fade that must take
  200 ms of RT goes on the trial base, which has no rate in the script
  API: a rate there scales every wait and duration of a trial, which in
  psychophysics is a confound.
- **The residual stays base ns.** It is exact, and it is defined on a
  paused base. At rate num/den the RT lateness is about residual x den /
  num.
- **The report is ordered by when the base reaches each event.** v0.3.0
  ordered by onset - residual. At rate 1 the two are equal, so the bytes
  do not change. At other rates, onset - residual orders by lateness in
  base ns. Example: base A at 1/2 has an event 4 ms of base time late
  (8 ms of RT), and base B at 1 has one 6 ms late. By onset - residual,
  B's comes first; by RT time, A's does. The comparator reads the event's
  base, so it needs no storage. A paused base keeps onset - residual.

### Arithmetic

    bt(t) = anchor_bt + floor((t - anchor_rt) * num / den)
    rt(T) = anchor_rt + ceil((T - anchor_bt) * den / num)

**The two are exact inverses on "reached".** For integers t and T,
bt(t) >= T holds exactly when t >= rt(T). The proof: T - anchor_bt and
t - anchor_rt are integers, so the floor and the ceiling can each be
removed from one side of the inequality. Three things follow:
- Every rule that asks "has the base reached T" gives the same answer in
  RT and in base time: the window, peek, next_due and the report's order.
- Peek's bounds from v0.3.0, `bt(from_rt - 1) < time <= bt(to_rt - 1)`,
  are right at every rate.
- Quantization at a rate is the rate-1 rule applied to rt(T). An event
  lands on the first frame with onset + lead x period >= rt(T).

**Floor, not truncation.** bt is monotone only with the floor. C99's `/`
truncates toward zero, which at rate 1/2 maps t - anchor_rt = -1 and +1
both to 0. So the code moves the quotient down by one for a negative
remainder. Times before the anchor occur whenever rt is in the future. The
header mutation that removes the correction fails 117 checks.

**No overflow.** The difference d = t - anchor_rt is split as
d = q den + r with 0 <= r < den, so
bt = anchor_bt + q num + floor(r num / den):
- r num < 2^62 for every rate.
- q is held to (2^63 - 1 - 2^31) / num, stored per base, so q num and the
  sum cannot overflow.
- Past that bound, the result is more than 2^63 - 2^32 from 0, so with
  anchor_bt below 2^62 the base time is out of range anyway.
- The bound is not 2^62 / num. The first version used 2^62 / num, and the
  arithmetic test found it wrong: anchor_bt can bring a product beyond
  2^62 back into range (658 base times in the random check).

The inverse splits by num in the same way.

**A clamp has a record.** A base time at or past +-2^62 becomes +-2^62.
Two examples: rate 1000/1 held for 53 days, or an anchor near the limit.
No event time equals +-2^62, so every event still fires or waits as the
exact time says. The rule is that a timing library never clamps a time
without a record, so:
- `ytl_clamped()` counts the evaluates and pauses that met one.
- `ytl_base_time()`, `ytl_rt_time()` and `ytl_window()` return false
  for such a time.
- `ytl_rate()` refuses a running base whose time at rt is out of range.
- `ytl_next_due()` gives 2^62 for an event that a slow base reaches only
  past 2^62 RT ns, a time no clock reaches.

Before v0.4.0, the rate-1 sum anchor_bt + (t - anchor_rt) could overflow
with an anchor near the limit. UBSan found that in the new arithmetic
test. Now the rate-1 path uses the same checked add.

**No drift.** Each time is computed from the anchor, so a rate held for
10 hours is within 1 ns of the exact rational time, the same as after
1 s. A change re-anchors at bt(rt) and takes one floor, so after n
changes the base is behind the exact time by less than n + 1 ns, and
never ahead. The test runs 1000 changes and measures this in thousandths
of a ns. A carry field would make it exact. It was not added: a change is
a user action, and 1000 of them cost under 1 us.

**Replay is exact.** The new fields (num, den, the bounds, the anchor and
"now") are set only by open, rate, anchor, skip, pause and resume, from
integer arguments, by +, -, *, / and % with no overflow. C99 defines `/`
and `%` for negative operands, so the floor correction gives one result
on every conforming compiler.

### Where the change point goes

Pass the predicted onset of the next frame to evaluate. The other choices
are defined too:
- **rt from the last onset to the next:** the base time at each onset
  stays monotone. A window can still end behind the reach. With lead 0.5
  and rt at the last onset, that happens when the rate drops by more than
  3 times. Then nothing un-fires, and late events wait for the window.
- **rt before the last onset:** this places the change in the past. If the
  rate dropped, the next onset's base time can be before the last one's.
  Tracks step back for that frame, and events do not un-fire.
- **rt after the next onset:** the new mapping is extrapolated back to the
  next onset, as it is for an anchor or a resume with rt in the future.

The test sweeps the change point. It uses 500 and 1000 Hz, lead 0.5 and
never-early, and five rate pairs: 1 to 1/2, 1/2 to 3, 1001/1000 to
999/1000, 1 to 1/10, and 7/3 to 1. It covers 40 change points from frame
K-1's onset to frame K+1's, including the onsets, the window ends and a
ns either side. All 208,000 events land where the reference puts them.

### ytl_window

```c
bool ytl_window(const ytl_timeline* tl, int base, const ytl_frame* f, int64_t* end);
```

This call gives the base time that frame f's window reaches: the base
time at RT f->onset + lead x period, or the frozen time on a paused base.
The evaluate calls the same function, and the lead in ns is one
expression, so the call cannot disagree with the evaluate. It has two
users:
- The script scheduler (rig_spec.md 6.1) resumes a wait on frame f when
  the target is at or before `end`. With lead 0.5 that is the frame
  nearest to the target, the frame where an event at the same time lands.
- ysp/video.h computes its due frame from `end`. Today it adds the lead
  in RT ns to a movie time, which is correct only at rate 1.

The call does not return the base time at the onset (ytl_base_time
gives it) or the reach (a scheduler entry is not an event).

### ysp/video.h

The change belongs to that header:
- `yvid__base_rate()` reads `ytl_get_rate()`.
- The due frame becomes `yvid_due(num, den, window_end, 0)`.

At rate 1 both give the frame they give today. Four other places in
ysp/video.h assume 1 movie ns per RT ns:
- the loop wrap's RT time, which `ytl_rt_time()` gives;
- the resume offset of one display period;
- a record's due RT time;
- the nominal schedule and cadence multiple used to classify slips.

Each is listed for that header's worker.

### Measured cost

The bench gained a fifth workload, `rate`: the movie workload with its base
at 1001/1000, so every conversion on that base takes the scaled path.
Four builds were run with interleaved runs (A B C D, A B C D, and so on):
v0.3.0 (A), v0.4.0 (B), v0.4.0 without the rate-1 fast path (C), and
v0.4.0 with the rate fields first in the base record (D).

In the full workloads, the run-to-run spread on this machine was up to 2
times (for example MSVC movie at 60 Hz: 594 to 1017 ns mean for v0.3.0 over
5 runs), larger than any difference between the builds, in both batches (3
and 5 interleaved runs, MSVC 19.44, MinGW gcc 16.1, WSL gcc 11.4, 60 and
500 Hz). So the decisions come from a smaller program. It times 10^7
evaluates in one loop with only running bases and no events, so the
conversions are most of the work. Each cell is the minimum over runs, in
ns per evaluate:

| Build | Bases | MSVC | MinGW gcc | WSL gcc |
|---|---|---|---|---|
| v0.3.0 | 1 at rate 1 | 11.8 | 10.0 | 11.2 |
| v0.3.0 | 7 at rate 1 | 27.4 | 26.2 | 42.8 |
| v0.4.0 | 1 at rate 1 | 15.5 | 11.9 | 12.9 |
| v0.4.0 | 7 at rate 1 | 39.7 | 37.4 | 38.6 |
| v0.4.0 | 7 at 1001/1000 | 70.6 | 62.6 | 68.2 |
| v0.4.0 without the rate-1 fast path | 7 at rate 1 | 109 | 99.3 | 93.3 |
| v0.4.0 with the rate fields first | 7 at rate 1 | 45.2 | 40.8 | 49.8 |

What the table decided:
- **The rate-1 fast path stays.** Without it, a base at rate 1 does the
  two divisions too: 6 to 10 ns more per base per frame.
- **The conversion is inlined.** The first build called it out of line
  and cost about 3 ns more per base than v0.3.0 (MSVC 48.5, MinGW 56.5 ns
  at 7 bases). Split into an inlined rate-1 part and an out-of-line
  scaled part, gcc inlines it. MSVC 19.44 at /O2 did not inline it even
  with `__inline`, so the header uses `__forceinline` there.
- **The checked add stays.** What is left on MSVC, about 1.7 ns per
  running base per frame, is the range check that replaces v0.3.0's
  unchecked sum. On gcc it does not measure. It is what makes a time past
  2^62 a recorded clamp instead of an overflow.
- **A scaled base costs about 4 to 5 ns more per frame** than a base at
  rate 1: two 64-bit divisions for each of its two conversions. Eight
  scaled bases at 500 Hz cost 0.002% of the 2 ms frame.

The `rate` workload of timeline_bench (the movie at 1001/1000) measured
the same as `movie` within the spread: 1058 to 1583 ns against 1064 to
1538 ns mean on MinGW, 328 to 647 against 313 to 505 ns on WSL gcc at
500 Hz. One frame has 10,000 annotations to search and 32 keyed channels
to sample, so the conversions are lost in it.

### Layout

The base record grew from 64 to 88 bytes. Every field that an evaluate of
a rate-1 base reads or writes is in the first 64 bytes, including a
`scaled` flag. num, den and the quotient bounds follow and are read only
on a scaled base. The bases array starts at offset 48 in the handle, which
the caller allocates with 8-byte alignment, so even a v0.3.0 record could
cross a cache line. The field order is what the header controls.
It did not measure as a factor: the build with the rate fields first
was within the spread of the hot-first build (table above). sizeof is 88
bytes, and the handle is 61,616 bytes, against 61,424 for v0.3.0.

## Sequences

v0.4.0 adds the C authoring builder that rig_spec.md 4.5.1 drafted: a
cursor on a base, calls in the order a trial plays, and the same trial as
a table of ops. The goal is C that is about as short as a GSAP timeline,
with the timeline's timing unchanged.

| GSAP | C |
|---|---|
| `const tl = gsap.timeline()` | `ytl_seq q = ytl_seq_on(&tl, TRIAL);` |
| `.set(fix, {visible: 1})` | `ytl_on(&q, FIX);` |
| `"+=0.5"` | `ytl_wait(&q, YRT_MS(500));` |
| `"<"` (with the previous) | the default: `ytl_to` does not move the cursor |
| `.to(g, {...}, "<")` | `ytl_to(&q, CONTRAST, &(ytl_tween_desc){...});` |
| `.to(g, {...})` (after the previous) | `ytl_then(&q, CONTRAST, &(ytl_tween_desc){...});` |
| `.call(fn, args)` | `ytl_mark(&q, code, (uint64_t)(uintptr_t)fn);` |
| `.addLabel("x")`, `"x+=0.2"` | `int64_t x = q.t;`, then `ytl_at(&q, x + YRT_MS(200));` |
| `stagger: 0.02` | `ytl_on_n(&q, dots, 50, YRT_MS(20));` |
| `keyframes: [...]` | `ytl_keyframes(&q, ch, values, offsets, n, ease);` |
| `tl.restart()` | `ytl_anchor(&tl, TRIAL, onset, 0);`, which replays the tweens |
| `tl.kill()` | `ytl_seq_cancel(&q);` |

### What the review of the draft found

The draft lowered `ytl_to` to `ytl_tween()`, which gave two problems:
- **The draft trial lost its first ramp.** It posts both CONTRAST tweens
  at build time, and a channel holds one waiting tween: "a second tween
  before the first has started replaces it". GSAP's timeline holds any
  number of tweens per target. A script does not have this problem,
  because it posts each tween at its `now()`, just in time.
- **A re-run did not replay the tweens.** "A rewind does not undo a
  takeover", so a trial re-run by an anchor at 0 fired its events again
  but kept the last tween on the channel.

The review also found these problems:
- Two drafts used the name `ytl_trigger`: the builder call and a
  constructor in rig_spec.md 6.1.
- `ytl_set` read like `ytl_set_track`.
- There were two arenas, one in the handle and one in the player.
- The builder was not atomic but `ytl_run` was.
- `start_set` competed with the cursor.
- `ytl_to` had no end to return for YTL_FOREVER.
- `ytl_mark` had no `user` payload for the pointer that GSAP's `.call()`
  carries.

### Decisions

- **One keyed track per channel per sequence.** A sequence's tweens on a
  channel become one block of keys in a key arena: a key at the start
  and at the end of each segment, with cycles and yoyo unrolled. Between
  segments the value holds: the last key before a segment gets STEP. So a
  channel takes any number of tweens. After the takeover the block is an
  ordinary keyed track, a function of base time, and a rewind replays it.
- **The block waits, then takes over, as ytl_tween() does.** The block
  uses the channel's waiting slot, so the old driver runs until the first
  start. The first segment's `from`, when not set, is the old driver's
  value at that start. With keep_velocity, the slope comes from the old
  driver too. Only the run time knows these values. The block marks
  every key whose value depends on the takeover, with a bit in
  `ytl_key.reserved_`. These are the first key, the return keys of a
  yoyo, the keys of later cycles, and later segments that start where
  such a key ends. The takeover writes the value and clears the bits. So
  a sequence of one tween gives the same values, bit for bit, as
  `ytl_tween()` with the same desc. The test checks this on 400 random
  descs, over a moving track and over no track.
- **Refused in a sequence:**
  - `start_set`: the cursor is the start, and two sources for one value
    would be silent.
  - `YTL_FOREVER`: a repeat folds a whole track, not a segment. An
    endless flicker is a set_track with a repeat.
  - A tween that starts inside the channel's previous one: cutting an
    eased segment part way changes its shape, so the result would be
    neither tween.
  - keep_velocity on any tween but the channel's first in the sequence:
    its slope comes from the takeover.
- **Every key carries its tween's ease, the ends of cycles too.** The
  zero-length segments of those keys are never read. Without the ease, a
  STEP tween with cycles stopped being a STEP track, and the evaluate read
  it at the onset, not where events land. A header mutation (cycle ends
  LINEAR) checks this.
- **Names.** The builder keeps the short verbs. The constructors in
  rig_spec.md 6.1 are dropped, because the builder and designated
  initializers serve the same users. `ytl_set` is now
  `ytl_set_value`.
- **A sticky error, not atomic.** The calls before an error stay. An
  atomic builder would have to stage every event and key until a commit
  call, in storage that a short-lived sequence on the stack cannot own.
- **ytl_seq_cancel.** The storage is sorted by time, so a watermark
  cannot truncate it. Ids rise in add order, so cancel deletes the events
  of the sequence's base from the sequence's first id on, in one
  compaction pass. It then recomputes the base's channels and detaches
  the blocks that the sequence built. The manual states the two limits:
  - Events that other code added to the same base meanwhile are deleted
    too.
  - A block that already took over is removed, and its channel holds its
    value.
- **ytl_run is all or none.** It checks the whole table first. On an
  error it changes nothing, and the test compares the handle, the storage
  and the arena with memcmp.
- **ytl_check_ops checks binding by a rule, not a table.** It checks an
  op against the first earlier op that names its channel, or against the
  channel's state. A table per channel would need YTL_MAX_CHANNELS bytes
  on the stack (up to 64 KB) or a write in a const call. The rule costs
  O(n^2) compares, once, when a pack loads.

### The key arena

- **Where:** `desc.keys` and `desc.key_capacity`, the caller's storage, as
  `desc.events` is. The default is none: the handle is already 64 KB, and
  an inline arena would grow every handle for callers who never use a
  sequence.
- **Layout:** a curve and a key are both 16 bytes, so curves go in key
  slots. They are copied in and out with memcpy, never read through a key.
  Each block has a header slot (its counts and its channel), then its keys,
  then its curve slots. A channel stores an offset into the arena, not a
  pointer.
- **Compaction:** a block is live when its channel's track or waiting
  block is at its offset. So compaction is one pass with no table, and
  the offsets move with the blocks. A replaced block is garbage until a
  call needs its room. The first version also compacted in
  `ytl_clear()`. No test could see a difference, because the next call
  that needs room compacts anyway, so that compaction was deleted.
- **Full:** a call that does not fit after a compaction is YTL_ERR_FULL
  and adds nothing. A track is never cut short.
- **Growth:** a block at the top of the arena grows in place. Another
  block is copied to the top. So `ytl_check_ops` asks for room for the
  table's blocks plus one copy of the largest block. The check is
  conservative by at most one block: for the draft trial, the builder
  needs 5 slots and the check asks for 10.
- **Evaluate** never compacts and never allocates.

### Cost

An evaluate pays nothing for a sequence feature it does not use, with
one exception: the sampler must find an arena block's curves. The first
build passed the arena's curve slots as a new argument through the key
lookup and resolved a key's curve on every key. A program that times
10^6 evaluates of 32 keyed channels of 256 keys (no events, one base at
rate 1) measured it 21 to 28% slower than v0.3.0. The variants tried, in
interleaved runs under the measurement lock, minimum of 7 rounds, in ns
per evaluate:

| Variant | MSVC 19.44 | MinGW gcc 16.1 | gcc 11.4, WSL2 |
|---|---|---|---|
| v0.3.0 | 210 | 405 | 193 |
| First build | 302 | 527 | 246 |
| Curve looked up only for BEZIER and HERMITE keys | 263 | 468 | 212 |
| And the new channel fields after v0.3.0's | 254 | 440 | 211 |
| And a direct path for tracks not in the arena | 260 | 499 | 202 |
| Kept: no new argument; an arena block's curve slots follow its keys, so `curves` NULL means "after the keys" | 226 | 399 | 208 |

A caller's track with a BEZIER key always has a curve table (set_track
checks it), so NULL is free to carry that meaning, and every sampling
signature is v0.3.0's again. The direct path did not help and was
deleted.

The rest is at the resolution of this machine. Over 14 rounds in two
batches, the kept build was 1.8% above v0.3.0 on MinGW, 9% on MSVC and 13%
on WSL gcc: under 1 ns per channel per frame. But v0.3.0 itself moved by
up to 20% between batches (392 to 474 ns on MinGW), and in the last batch
the kept build was faster than v0.3.0 on MinGW and MSVC. A control,
v0.3.0 with its channel record padded by the 16 bytes that the sequence
fields add, could not be told apart from v0.3.0 either. Four BEZIER
channels measured the same in all builds within that spread. At 32
channels and 500 Hz, 24 ns is 0.001% of the 2 ms frame.

## Unit names, messages and close (v0.5.0)

v0.5.0 applies the repository conventions (README.md, Conventions). The
arithmetic does not change. Three decisions:

- **A time field that the caller writes ends in `_ns`.** These are
  `ytl_event.time_ns`, `ytl_key.time_ns`, `ytl_track.t0_ns` and
  `period_ns`, `ytl_tween_desc.duration_ns`, `start_ns` and `delay_ns`,
  and `ytl_op.t_ns`. Old code that wrote `.duration = 0.2` now fails to
  compile, and it does not truncate to 0 ns. The fields of `ytl_frame`
  keep their names, because a frame is a copy of a flip record (`onset`,
  `period`, `index`), and those names are the screen's. Also unchanged
  are the landing fields that only the header writes (`onset`,
  `residual`). `YTL_S()` and `YTL_MS()` are gone, and the header defines
  the `YRT_*` macros of ysp/rt.h under the same guard.
- **Each failing call writes the message.** Before, only `ytl_open()`
  wrote it. Now each call that returns a `YTL_ERR_*` code writes it, and
  so does a sequence call that sets `q.err`. The message names the call,
  or the sequence call's index. The write is an `snprintf` on the failure
  path only, so a call that succeeds does one more compare. This cost was
  not measured. `ytl_peek()`, `ytl_clamped()` and `ytl_check_ops()` now
  take a non-const handle, because a failure writes into it. A query that
  returns `bool` writes nothing, because false is an answer there.
- **`ytl_close()`.** The handle owns nothing, so close only marks it
  closed. Every open has a close. After a close, a call returns
  `YTL_ERR_CLOSED`.

The tests that compared the whole handle with `memcmp` after a refused
call (`ytl_run`, `ytl_skip`, `ytl_peek`) now compare the bytes before the
message, and they check the message. Checked on 2026-10-10 (Windows 11,
MinGW-w64 gcc 16.1 -O2 and MSVC 19.44): `tests/adapt/timeline_test.c`
passes, with a new test for close and messages. The USAGE blocks build
and run as C99 (`-Wall -Wextra -Wpedantic -Werror`) and C++17. Mutants
`conv-01` (no message), `conv-02` (close keeps the handle open) and
`conv-03` (no sequence message) are caught.

## Storage

Events go in a sorted array that the caller supplies, and the caller sets
its size. At movie scale, the array holds thousands of annotations, so a
compile-time maximum would make every handle large. Each base has a range
in the array, a fired prefix and a lowest late index. With these, an
evaluate is a binary search per base plus the work for the events that
fire.

Channels and bases have compile-time maximums (256 and 8). There are few
of them, and the handle stays at about 12 KB.

Keys are not copied. A pack table or a static array is used where it is.
An adapter would only copy the same data.

## Measured costs

`examples/timeline/bench.c` gives these values. The machine is the
development machine. The compilers are gcc 11.4 at -O2 under WSL2 and
MSVC 19.44 at /O2 on Windows 11. The time for one frame includes one
clock read. The display is 60 Hz. Each cell is the range over three
runs, because the values changed by up to 1.6 times from one run to the
next.

| Workload | gcc mean | gcc p99 | MSVC mean | MSVC p99 |
|---|---|---|---|---|
| Trial: 5 events, 1 keyed channel, a rewind each trial | 51-82 ns | 86-144 ns | 53-70 ns | 100 ns |
| Movie: 10,000 annotations, 32 keyed channels of 256 keys, 8 SET channels | 297-429 ns | 0.6-1.2 us | 321-527 ns | 0.7-1.6 us |
| Script: 1 add per frame on the RT base, prune every 60 frames | 60-91 ns | 84-134 ns | 57-92 ns | 100-200 ns |

On Windows the clock ticks every 100 ns, so the MSVC quantiles are
multiples of 100 ns.

The worst p99 is 1.6 us, about 0.01% of a 16.7 ms frame. The worst single
frame was 648 us (gcc, movie), about 4% of a frame. "Slow single frames"
below shows that such frames come from the machine, not the header.

### Refresh rate

Displays at 360 Hz and 500 Hz are on sale, so the header must not assume
60 Hz. Nothing in it does: the period is an argument, and times are whole
nanoseconds. The cost of one evaluate does not depend on the rate. The
budget does: 2 ms at 500 Hz, 1 ms at 1000 Hz. Run
`timeline_bench <refresh_hz>` to measure a rate. The movie workload
covers two hours at each rate, so 3.6 million frames at 500 Hz. One run
at each rate:

| Rate | Budget | Movie mean, gcc / MSVC | Movie p99, gcc / MSVC | p99 share of the budget |
|---|---|---|---|---|
| 60 Hz | 16.7 ms | 343 / 269 ns | 651 / 600 ns | 0.004% |
| 500 Hz | 2 ms | 308 / 258 ns | 606 / 500 ns | 0.03% |
| 1000 Hz | 1 ms | 277 / 260 ns | 575 / 600 ns | 0.06% |

The test checks quantization on grids of 60, 144, 240, 360, 500 and 1000
Hz.

### Slow single frames

At high rates a slow single frame matters more. The slowest movie frame
was 0.85 ms at 500 Hz (gcc) and 0.44 ms at 1000 Hz (MSVC). To find out
whether the header causes these frames, a scratch program ran two
identical timelines in lockstep on the same inputs and timed each one on
every frame. Work done by the header is the same in both timelines, so a
frame that the header makes slow is slow in both. A frame made slow by
the machine or the OS is slow in one only.

| 500 Hz, 3.6 million frames | Slow in A | Slow in B | Slow in both |
|---|---|---|---|
| gcc, WSL2, above 20 us | 319 | 337 | 1 |
| gcc, WSL2, above 5 us | 1006 | 1023 | 4 |
| MSVC, Windows 11, above 20 us | 200 | 191 | 2 |
| MSVC, Windows 11, above 5 us | 445 | 434 | 8 |

The slow frames are not the header's. They are more frequent in the movie
workload than in the trial workload. The movie uses about 800 KB of
memory, against a few hundred bytes for the trial. A probable cause is
that an interruption evicts that memory from the cache. This note did not
measure that cause. The benchmark's "floor" line gives the machine's own
slow frames with no header code between the clock reads.

### Loads

| Load of 10,000 events | gcc | MSVC |
|---|---|---|
| Sorted, one `ytl_add_n` | 0.17-0.23 ms | 0.29-0.41 ms |
| Sorted, one `ytl_add` each | 0.14-0.22 ms | 0.32-0.46 ms |
| Shuffled, one `ytl_add` each | 23-37 ms | 23-34 ms |

A shuffled load is quadratic, because each add moves the tail of the
array. Sort the table before you load it. The pack tool sorts it.

## Verification

`tests/adapt/timeline_test.c` was written from the manual only, by a
writer who did not read the implementation. It has its own reference
model. It checks quantization on a 60 Hz grid at three leads, against a
ceiling-division model, with event times on the window edges. It also
checks every easing against its own formulas, binding, late events,
rewinds, remove, prune and clear, and replay. Replay runs 600 calls into
two handles and compares the storages with `memcmp`. A random model check
runs 10 seeds of 800 steps and compares the whole storage and every
channel value after each step.

The writer broke the model on purpose in several ways to show that the
check finds errors. These mutations were caught: the lead window off by
one, `>` for `>=` in the rewind, `<` for `<=` in the rewind threshold, the
LATE flag dropped, and an evaluate-time un-fire put back.

The test found four header bugs before the first release:
- The duplicate fire after a pause with a lead (see "Only an anchor
  rewinds").
- A window behind the reach moved the reach back, so a later add was not
  marked late.
- On the first evaluate of a base, a negative window did not become the
  reach.
- A remove of an event that never fired reset its channel to the initial
  value.

It also found one disagreement between two sentences of the manual
(`ytl_set_keys` on another base). The header's behavior was kept and
the manual was changed.

For v0.2.0 a second writer extended the test the same way, from the
manual only. It covers sampled tracks, repeats, Bezier, tweens and the
STEP read rule, and it adds track operations to the random model check.
Six new model mutations are each caught: repeated instead of phantom end
samples, a whole-ns sample step, the hold one cycle late, the hold folded,
the Bezier parameter used as x, and STEP tracks read at the onset. The
test found one disagreement: the limit on the last sample's time was a
whole-second bound in the code but "within a second of 2^62" in the
manual. The check is now exact (a last sample at or past 2^62, rounded
up to a whole ns), and the manual says so. Two other bugs of v0.2.0 were
found before the test ran, both by the example program: the
whole-ns sample step and the repeated end samples.

A third writer ported the test to `ytl_tween_desc` and covered the
takeover. Three more model mutations are each caught: `from` read at the
call, the slope ignored with `keep_velocity`, and the yoyo return leg
missing. The writer found that the header's "now" for a tween did not
match the manual. The header used the furthest evaluated base time and,
after a rewinding anchor, the time just before the anchor. Both came from
bookkeeping that serves the rewind, not the tween. Now each base keeps
its own "now": the base time of the last evaluated onset, or the time a
later anchor, pause or resume set. The random model uses that rule with
no cases excluded, and fails 10 checks against the first version's rule.

For v0.3.0 the writer of the implementation extended the test. This is
weaker than the earlier rounds: the same person wrote the code, the
manual and the model, so a misreading can be in all three. The checks:
- Directed: the 30-minute seek against an anchor to the same place (1978
  skipped against 1980 fired late on one frame, the same channel
  values), a skip back against an anchor back (the same storage bytes),
  a late event that waits behind the reach and is skipped by a skip back,
  the reach and the rewind after a skip, prune and remove of skipped
  events, a never-anchored and a paused base, tracks and a waiting tween
  after a skip against an anchor (the same bits), peek's window edges,
  order across bases, cap and total, paused, stopped, fired, skipped and
  late events, refusals, and purity by `memcmp` of the whole handle and
  the storage around every peek.
- The audio look-ahead for 22 s at 60 Hz (see "Peek").
- Ten more seeds of the random model, with skip and peek among the ops.
  The model compares the whole storage and every value after each step,
  and the handle and the storage are unchanged by each peek. The first
  ten seeds run as before, op for op, and give the same counts.
- Replay: skip and peek are among the calls hashed in both handles.

Model mutations, each caught: a skip that only anchors, skipped SETs
not counted for the channel, the reach or the furthest onset not moved,
the event at `bt` skipped, a rewind that leaves skipped events skipped,
peek's window closed at `to_rt`, fired events in a peek, a peek copy's
onset as its base time, and a skip back that leaves waiting late events
pending.

Mutations of the header, made one at a time on a copy: 23. 20 are
caught. Three cannot change any result, and the reasons are:
- `FIRED` for `DONE` in the evaluate's advance of the lowest late index.
  That index is a lower bound for a scan that skips done events.
- `FIRED` for `DONE` in `ytl_next_due()`. Its scan starts at the lowest
  late event, which is pending, and every skipped event after it in the
  storage has a time that is not earlier. So the minimum cannot change.
- A paused base admitted to a peek. The paused mapping gives the frozen
  time for every RT time, so the window is empty.

The v0.2.0 test, unchanged, passes against v0.3.0 with the same model
counts. `timeline_trial` and `timeline_tracking` print the same bytes as
with v0.2.0, except the version, on gcc 11.4, MSVC 19.44 and MinGW gcc
16.1. One run of `timeline_bench` at 60 Hz (gcc 11.4, WSL2): trial 51 ns,
movie 322 ns mean and 599 ns p99, script 60 ns, tracks 246 ns. These are
inside the v0.2.0 ranges above.

For v0.4.0 the writer of the implementation extended the test again, so
the same weakness applies: one person wrote the code, the manual and the
model. Two things limit it. The reference arithmetic is written apart from
the header's: a 94-bit product in 32-bit limbs and a long division, where
the header splits the difference by the denominator. Under gcc the
reference is itself checked against `__int128` on every call (452,109
checks, 0 different). The checks:
- Arithmetic: 200,000 random cases of the mapping and its inverse, with
  rates up to 2^31 - 1 on either side and anchors and times anywhere in
  +-2^62, out-of-range results included. The property bt(rt(T)) >= T >
  bt(rt(T) - 1) is checked through the header. The floor before the
  anchor is checked by hand at rate 1/2.
- Quantization: grids of 60, 144, 240, 360, 500 and 1000 Hz, at rates 1/2,
  2, 1001/1000, 1000/1001, 3/7 and 25/24, at lead 0.5, 0.25 and never
  early. The event times are on the base times of window ends, a ns
  either side, and on frame onsets. The report's order is checked against
  rt(T).
- The change point swept across a frame (see "Where the change point
  goes"): 208,000 event landings and every window end.
- Directed checks for each rule in "Decisions": no rewind at the change
  point (the storage is unchanged by memcmp, and a later anchor still
  rewinds), a window behind the reach and a late event that waits for
  it, "now", a call with the same rate (the handle is unchanged by
  memcmp), pause, resume, stop, skip, clear and open. Also: a 200 ms
  tween at 1/2, the keep_velocity velocity ratio across a change, 600
  STEP tracks against events at a rate, peek's edges at rt(T) - 1, rt(T)
  and rt(T) + 1 at 1/3 and 7/3, next_due, the report's order across
  bases at 1/2 and 3, 10 hours at 1001/1000, 1000 changes, and every
  clamp rule.
- The random model: 12 more runs (seeds 201 to 210, and two at 500 Hz)
  with rate, window and rt_time among the ops. The model's mapping uses
  the exact reference. Each run checks at least 300 rate changes, 100 of
  them on running bases, 20 windows behind the reach after a change, and
  300 events fired on scaled bases. With `TL_V030_MODEL=1` the test runs
  only the 20 earlier model runs, and their statistics line is the same as
  v0.3.0's.
- Replay: a second replay run with rate and window calls among the ops.
  The two handles' bases and storages are compared with memcmp.

Model mutations, each caught: a change that rewinds, truncation instead of
floor, the window as bt(onset) + lead (the form ysp/video.h has today),
the inverse rounded down, a pause at rate 1, the report by onset -
residual, a change that leaves "now", a same-rate call that re-anchors,
and an anchor that resets the rate. All 28 model mutation bits fail.

Mutations of the header, one at a time on a copy: 24. 21 are caught.
They are `rate-00` to `rate-23` in
[tests/mutate/timeline.toml](../tests/mutate/timeline.toml).
Three cannot change any result, and the reasons are:
- `r <= 0` for `r < 0` in the floor correction. With r = 0 it gives
  q - 1 and r = den, and (q - 1) num + den num / den is q num.
- The quotient bound `q >= lim` for `q > lim`. At q = lim the scaled part
  is more than 2^63 - 2^32, so the sum is out of range whether or not the
  scale saturates.
- A rate change that re-anchors a paused base. A paused base's time does
  not read its anchor's RT time, the resume sets it again, and its "now"
  is already the frozen time.

Two of the 21 were caught only after the first round, when a check was
added for each: a time of exactly 2^62, and the clamp record of a base at
rate 1.

The test found two bugs before the release:
- The first quotient bound was 2^62 / num. It refused base times that the
  anchor's base time brings back into range (658 of 200,000 random cases).
- UBSan, on the new arithmetic test: the rate-1 sum anchor_bt + (t -
  anchor_rt) overflowed for an anchor near the limit. This was v0.3.0's
  expression, undefined behavior out of the contract, and is now the
  checked add.

The v0.3.0 test, unchanged, passes against v0.4.0. `timeline_trial` and
`timeline_tracking` print the same bytes as with v0.3.0, except the
version, on MinGW gcc 16.1 and MSVC 19.44. Built and run with MinGW gcc
16.1 (C99, C11, C++17), gcc 11.4 under WSL2 (C11 with ASan and UBSan, C99
at -O3) and MSVC 19.44 (default C, C11, C++17).

For the sequences, the same writer added these checks:
- The draft trial (rig_spec.md 4.5.1), built by calls and as a table:
  CONTRAST against the keyed reference on every frame, so both ramps
  play in full. A re-anchor at 0 replays the values and the event counts
  of every frame bit for bit. The table and the calls give the same
  storage.
- ytl_run failures, each with the handle, the storage and the arena
  compared with memcmp, and the same code and index from
  ytl_check_ops: a zeroed op, start_set, FOREVER, both directions of
  BOUND, overlap, a channel out of range, a full storage, an arena one
  slot short (9 against the 10 the check asks for), and no arena. The
  builder with 5 slots succeeds; with 4 it keeps its first tween and
  refuses the second.
- A sequence of one tween against ytl_tween() with the same desc, on
  400 random descs (every ease, cycles, yoyo, delay, from, keep_velocity),
  over a moving track and over none: the same bits on every frame.
- Cancel (the events after the first id deleted, a fired event's channel
  recomputed from an older event, a waiting block dropped and the old
  track running on, a promoted one held), on_n (all or none, the
  stagger, FULL), the cross-base stop, and the arena under clears and
  growth.
- A random model of 600 tables of up to 15 ops, with errors among them,
  built three ways: by the builder calls, by ytl_run, and by hand with
  ytl_add and ytl_set_track of keys that the test lowers itself.
  Values are compared on 160 frames and the storages with memcmp. The
  runs cover 166 clean tables, 392 ARG, 42 BOUND, 1241 tweens and 913
  events.

Mutations of the sequence code in the header, one at a time: 19, all
caught (`seq-00` to `seq-18` in
[tests/mutate/timeline.toml](../tests/mutate/timeline.toml)). They include the draft's overwrite, a yoyo return key not marked
for the takeover, a compaction that leaves the offsets, ytl_check_ops
without binding, ytl_run without its check, the gap not held, the
takeover value or the Hermite tangent not written, growth in place
without moving the curves, overlap allowed, cancel without the
recompute, cancel deleting every event of the base, the check without
room for a copy, on_n not checked first, STEP detection off, cycle ends
LINEAR, start_set accepted, no cross-base stop, and an arena block's
curves read from the start of its keys instead of their end.

The compile checks are `tests/compile/timeline.c`, which builds a
sequence by calls and by an op table, and `timeline_ops.cpp`, the
table as C++20 (with `-Wno-missing-field-initializers` on g++). Both are
clean on MinGW gcc 16.1, MSVC 19.44 (C++20) and gcc 11.4.

## Not done

- No snapshot format. The storage is the record. A resume after a crash
  adds the events again and anchors again.
- One timeline is for one display. A second display that flips on its own
  grid needs its own timeline or its own evaluate.
- No run on macOS or on a big-endian machine (CI builds macOS). v0.4.0
  ran on 2026-10-06 with emcc 6.0.10 under node (compile check, test),
  gcc 13.3 in WSL2 (the test at -O2 and under ASan and UBSan), and gcc
  13.3 and clang 18.1 in an Ubuntu 24.04 container (the CI jobs): pass.
- ysp/video.h still anchors on a seek and reads `tl->lead`. Changing it
  to `ytl_skip()` and `ytl_lead()` is that header's change, and so
  are the rate changes listed in "Base rates".
- The cost of a skip is not timed. It is one write per skipped event.
- A base rate's change is exact to under 1 ns per change, not exactly
  rational (see "Arithmetic").
