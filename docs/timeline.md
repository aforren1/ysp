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
(it was 12 KB in v0.1.0; since v0.7.0 it is 264 bytes and 68 KB, with a
second curve for a BEZIER yoyo's return leg). An evaluate reads a byte
per channel to find the tracked channels, so the size does not add to the
frame cost. A rig that needs fewer channels sets `YTL_MAX_CHANNELS`.

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

## Targets (v0.6.0)

Before v0.6.0, a tween writes into the value array, and the caller copies
each value into a stimulus field on every frame with a binding table
(`ygfx_apply(binds, n, ytl_values(&tl))`). GSAP, anime.js and Motion
write into the target object itself. v0.6.0 adds targets: a tween that
writes into a field of the caller, a `float`, `float[2..4]` or `int32_t`.

| GSAP | C |
|---|---|
| `gsap.to(dot, {x: 300, duration: 0.5, ease: "power2.out"})` | `ytl_tween_to(&q, &dot.x, 300.0f, YRT_MS(500), YTL_EASE_CUBIC_OUT);` |
| `gsap.from(dot, {opacity: 0, duration: 0.2})` | `ytl_tween_from(&q, &dot.opacity, 0.0f, YRT_MS(200), 0);` |
| `gsap.fromTo(dot, {x: 0}, {x: 300})` | `ytl_tween_from_to(&q, &dot.x, 0.0f, 300.0f, dur, ease);` |
| `tl.set(fix, {visible: 0}, 0.5)` | `ytl_at(&q, YRT_MS(500)); ytl_tween_set(&q, &fix.visible, 0.0f);` |
| `gsap.to(g, {color: ..., repeat: 3, yoyo: true, delay: 0.1, onComplete: f})` | `ytl_anim a = { 0 };` fields, then `ytl_animate(&q, &a);` |
| `gsap.to(dots, {y: 40, stagger: 0.05})` | `a.stagger_ns = YRT_MS(50); ytl_animate_n(&q, &a, sizeof dots[0], n);` |
| `"<"`, `">"`, `"+=0.2"` | `ytl_at(&q, q.prev_start)`, `ytl_at(&q, q.prev_end)`, `ytl_wait(&q, YRT_MS(200))` |
| `.addLabel("x")`, `"x+=0.2"` | `int64_t x = q.t;`, `ytl_at(&q, x + YRT_MS(200))` |
| `gsap.killTweensOf(dot)` | `ytl_kill(&tl, &dot.x);` |
| `overwrite: true` | `a.overwrite = YTL_OVERWRITE_ALL;` |

### The model

- **A target is a field.** It has an address, a type (`YTL_F32` or
  `YTL_I32`) and 1 to 4 components. One target per address. A target is
  bound to one base, as a channel is.
- **The latest start drives the field.** A target's tweens are sorted by
  (start, id). At base time t, the tween with the latest (start, id) of
  those that have started drives the field. This is the rule of an
  event-driven channel ("the latest (time, id) wins"). So the field is a
  function of base time and of the set of tweens, and a rewind, a skip or
  a seek gives the same values as continuous play.
- **The start value is the takeover's.** When a tween has no `from`, its
  `from` is the value the field has at its start: the earlier tween's
  value at that base time, or, for the first tween, the field's own value
  before the timeline wrote it (the rest value, read once). This is the
  takeover rule of `ytl_tween()`. The value is computed at the first
  evaluate that reaches the start, and kept. GSAP also records the start
  values at the first render.
- **Writes.** Before the first tween starts, the evaluate does not write
  the field, so the caller can set it. Two exceptions: a first tween with
  `immediate` (the default of `from` and `fromTo`, GSAP's immediateRender)
  shows its `from`; and after a rewind to a time before the first start,
  the field shows its rest value again. From the first write on, every
  evaluate writes the field until its tweens are killed or their base is
  cleared.
- **Frame placement is the core's.** A STEP tween (a set included) is read
  at the end of the frame's lead window, so it lands on the frame where
  an event at the same time lands. Other tweens are read at the predicted
  onset. All writes are in one pass at the end of `ytl_evaluate()`.
- **Callbacks are events.** `on_start` and `on_complete` add an event of
  kind `YTL_CALL_START` or `YTL_CALL_COMPLETE` at the start and at the end.
  The evaluate calls the functions after it writes every field, in the
  order in which the events fired.
- **Pools sized at open.** `desc.targets` and `desc.tweens` are arrays of
  the caller, as `desc.events` is. A call that does not fit returns
  `YTL_ERR_FULL`, writes the message and adds nothing.

### Decisions

- **The calls take a sequence, not the timeline.** A tween needs a base
  and a start. The sequence holds both, and also the labels, the
  positions, the sticky error and the cancel. GSAP's `gsap.to()` is a
  tween on the global timeline at "now": `ytl_seq_now(&tl, base)`.
  Rejected: a base argument on every call, which adds an argument and has
  no positions. Rejected: a default base in the handle, which is hidden
  state.
- **New names.** `ytl_to()` is the sequence's channel tween since v0.4.0,
  and C has no overloads. The target calls are `ytl_tween_to()`,
  `ytl_tween_from()`, `ytl_tween_from_to()`, `ytl_tween_set()`, and
  `ytl_animate()` with a desc. Rejected: renaming the v0.4.0 calls.
- **Overwrite: the latest start wins, and the earlier tween stays.** This
  is a GSAP timeline with `overwrite: false`, where the child that starts
  later renders last. The earlier tween still drives the field before the
  later start, so a re-anchored trial replays both. Rejected: GSAP's
  `"auto"`, which kills the overlapping earlier tween at the first render.
  A rewind could not bring it back. `YTL_OVERWRITE_ALL` kills the other
  tweens of the field at the call (GSAP's `overwrite: true`). It is for a
  script that retargets a field on a base that is never cleared: without
  it, the overwritten tweens fill the pool. Then the new tween starts from
  the field's value at its start, which is the last value written. Since
  v0.7.0, `ytl_prune()` also frees the overwritten tweens, after the base
  has passed them.
- **The timeline owns a field until a kill.** GSAP stops writing a field
  when its tween completes. Here the evaluate writes it until the
  target's tweens are killed or their base is cleared. The reason: the
  start value of a later tween comes from the earlier tween, not from the
  field. A write by the caller between the two would show on the screen
  and then be lost at the next start, with no sign. With ownership the
  next frame overwrites the caller's write, which a test or a look at the
  screen shows at once. The cost is one write per target per frame
  (measured below).
- **The rest value is read once.** It is the field's value before the
  first write of the target. A target whose tweens are all killed is
  freed, and the next tween on the field reads the field again. So a
  trial that clears its base starts from the values the caller set.
- **Callbacks are events, called after the pass.** As events, they land on
  the frame nearest to their time, a skip passes over them, a rewind
  makes them due again, and the storage records where they landed. They
  are called after every field is written, so a callback sees the frame's
  values. It can post or kill tweens; these take effect at the next
  evaluate. `ytl_evaluate()` from a callback is `YTL_ERR_ORDER`.
  Rejected: calls inside the pass, where a callback that posts or kills a
  tween changes the lists the pass walks. Rejected: a scan of the tweens
  on every frame to find due callbacks, which costs per tween per frame
  and repeats the event rules. The cost of the choice: each callback uses
  an event of the storage.
- **Keep the value array and `ygfx_apply()`.** Channels have users that
  targets do not serve: event-driven channels with the (time, id)
  recompute, sampled and repeating tracks (a 10-minute tracking target, a
  flicker), and the contiguous array for a uniform upload, a log and a
  designer's preview. 18 example programs use them. Targets serve what a
  channel does badly: a channel needs a number, a binding row and a copy
  per frame, and it holds one waiting tween. So neither subsumes the
  other. Rejected: a field pointer on a channel (`ytl_bind`), which is
  `ygfx_bind.field` again. Do not drive one field both ways: the binding
  runs after the evaluate and wins.
- **The same arithmetic as `ytl_tween()`.** A target tween and a channel
  tween with the same desc give the same bits. So the yoyo rule stays one
  rule in the header, and the test can compare the two paths. (In v0.6.0
  the return leg applied the same ease again; v0.7.0 changed it to GSAP's
  mirrored playback on both paths. See "Yoyo, set records and prune".)
- **Four more eases**: `QUAD_IN_OUT`, `CUBIC_IN`, `CUBIC_OUT` and
  `CUBIC_IN_OUT` (GSAP's `power1.inOut` and `power2.*`). They are +, -
  and *, so replay stays exact. Keys and channel tweens take them too.
- **No op-table form.** An op table is data for a pack, and a pack
  cannot hold a pointer.
- **Layout.** A target is 40 bytes and a tween 128 bytes, the fields that
  the evaluate reads in the first 64. Targets stay dense (a freed one
  takes the last one's place), so the pass reads one contiguous array.

### Measured cost

`examples/timeline/bench_targets.c` times three paths for 0, 1, 100 and
1000 animated fields, each a float in its own 256-byte stimulus:

- **targets**: one target tween per field (`ytl_tween_to()`).
- **channels**: one channel tween per field (`ytl_tween()`), then the
  binding copy, the loop of `ygfx_apply()` for `.field` bindings with its
  check pass.
- **held**: the targets after their tweens ended. The evaluate still owns
  and writes the fields.

Every tween is CUBIC_OUT over 1000 s, so it is in progress on each frame.
A sample is 64 evaluates between two clock reads. Each cell is the median
of 400 samples, the smaller of two runs of 5 interleaved rounds, in ns
per frame. Machine: Intel i7-1360P, Windows 11, on AC power, through the
measurement lock at 17% load. MSVC 19.44 /O2 (the msvc-full Release
build) and MinGW-w64 gcc 16.1 -O2:

| Fields | targets MSVC | channels MSVC | held MSVC | targets gcc | channels gcc | held gcc |
|---|---|---|---|---|---|---|
| 0 | 18.8 | 20.3 | 18.8 | 14.1 | 15.6 | 14.1 |
| 1 | 29.7 | 25.0 | 25.0 | 21.9 | 23.4 | 18.8 |
| 100 | 777 | 781 | 494 | 769 | 689 | 480 |
| 1000 | 7967 | 8314 | 4873 | 7553 | 7392 | 4609 |
| Per field, at 1000 | 7.95 | 8.29 | 4.85 | 7.54 | 7.38 | 4.59 |

The second run of each was within 8% of the first, except MSVC held
(5.8 ns per field). What the table shows:

- **A target costs about 8 ns per frame**, the same as a channel tween plus
  its binding copy (7.4 to 8.5 ns). Neither path wins on both compilers.
  So targets replace the copy at no cost; they do not make the frame
  faster.
- **0.5 ms holds about 60,000 animated fields** (62,000 to 66,000 from the
  1000-field rows). 1000 fields cost 8 us, 0.05% of a 16.7 ms frame and
  0.4% of a 2 ms frame at 500 Hz.
- **A held field costs about 5 ns**: the pass reads the target and its
  tween and writes the field. Ownership (see "Decisions") costs this much;
  a release after the end would save it.
- **One target costs 8 to 11 ns more than an empty frame**, against 5 to
  8 ns for one channel: the pass's fixed cost.

The channel path itself did not slow down. The first build sent every ease
of a key through the new eases' function (`ytl__shape`), one more switch
per key. A scratch program timed 256 LINEAR keyed channels against
v0.5.0, 5 interleaved rounds through the measurement lock, the minimum of
the medians in ns per evaluate:

| Build | MinGW gcc 16.1 -O2 | MSVC cl /O2 (Visual Studio 18) |
|---|---|---|
| v0.5.0 | 3105 | 3461 |
| Every ease through `ytl__shape` | 3291 (+6%) | 3754 (+8%) |
| Kept: v0.5.0's cases inline, the new eases through `ytl__shape` | 2695 | 3448 |

An earlier session at 0% load gave 1467 against 1344 (gcc) and 1636
against 1664 (MSVC) for v0.5.0 against the kept build, and COSINE keys
within 4% both ways. The machine's speed differed between the sessions,
so only the rows of one session compare.

### Verification

`tests/adapt/timeline_test_targets.h`, which `timeline_test.c` includes,
was written by the writer of the implementation. So a misreading can be
in the header, the manual and the checks alike. Two things limit that:
the eases and the repeat fold in the checks are written apart from the
header (pow() and floor division), and a target tween is compared bit for
bit with `ytl_tween()` on a channel, the path that the earlier blind
tests cover. The checks:

- Against `ytl_tween()`: 400 random descs (every ease, BEZIER curves,
  cycles, YTL_FOREVER, yoyo, from, keep_velocity), a second tween taking
  over from the first, 70 frames each: the same bits on all 28,000
  frames.
- Each of the 11 eases against the reference at 60 Hz onsets, as a target
  and as keys.
- to, from, fromTo and set; the field not written before the first start
  and owned after it; n = 3; int32_t rounding; stagger in both
  directions, with "<" and ">".
- The overwrite rules: the latest start, equal starts, a rewind that
  brings back the earlier tween, kill_tween of the driver, BOUND and a
  different shape, YTL_OVERWRITE_ALL with a change of base, clear and
  cancel.
- Callbacks: the frame each lands on (the frame of a MARK at the same
  time), the order, the field already written, a tween posted from one, a
  rewind that calls them again, a skip that calls neither, a nested
  `ytl_evaluate()` refused, a kill in on_start that skips on_complete, and
  the events a kill deletes.
- Every pool refusal and its message, all or none, and an overwrite that
  fits only by what it frees (tweens, and events).
- A set against a SET event at the same time, behind a ramp that starts in
  the same window: 900 cases at three leads, the same frame in all.
- A trial with every kind of call: a rewind replays 90 frames bit for bit;
  a fresh handle that seeks to 11 places with `ytl_skip()` shows the
  values of continuous play from there; two handles given the same calls
  have the same pools, storage and fields.
- A model of the rules in "The model", 12 runs of 500 random calls,
  frames, rewinds, skips, pauses and kills (1184 tweens, 451 of them STEP,
  221 immediate): every written field within 1e-4 of the model. Three
  model mutations are each caught: the first started tween drives, every
  start value is the rest value, STEP tweens read at the onset.

The test found two errors before the first run passed, both in the
checks: the model read the rest value after the header had written the
field, and two MARK events had the same code. It found none in the
header. The earlier checks pass unchanged, with the same model
statistics, except that the first number that is not an ease is now 11.

Mutations of the target code, one at a time: 20, all caught (`tgt-00`
to `tgt-19` in [tests/mutate/timeline.toml](../tests/mutate/timeline.toml)).
They include the first start driving, the cursor kept over a rewind, no
look-ahead for a set, a set read at the onset, every start value from the
rest value, the rest value read at every write, immediate ignored, a
moved target's tweens left behind (a crash), a nested evaluate accepted,
on_complete after a kill, and callbacks not queued.

The compile check `tests/compile/timeline.c` animates a stimulus struct
with each call, as C99 and as C++17 (`timeline.cpp`) with no cast. Clean
with MinGW gcc 16.1 (`-Wall -Wextra -Wpedantic -Werror`), MSVC 19.44 and
gcc 11.4 under WSL2. The test is clean under ASan and UBSan (gcc 11.4,
WSL2).

## Yoyo, set records and prune (v0.7.0)

v0.7.0 changes three things about tweens. The user approved them on
2026-10-11.

### Yoyo plays the forward leg backward

Until v0.6.0 the return leg of a yoyo applied the ease again, from `to`
to `from`. GSAP plays the tween backward: the value at s + d + x is the
value at s + d - x. The two agree for the symmetric eases only. With
CUBIC_OUT, v0.6.0's return leg started fast; GSAP's starts slow and ends
fast.

Decisions:

- **The return leg uses the reverse ease.** For every ease e, r(u) =
  1 - e(1 - u) is also an ease of the table: QUAD_IN returns with
  QUAD_OUT, CUBIC_IN with CUBIC_OUT and the reverse, BEZIER with its curve
  turned half a turn about (1/2, 1/2) (control points 1 - x2, 1 - y2 and
  1 - x1, 1 - y1, computed in float), and LINEAR, COSINE, the two IN_OUT
  eases and LOG with themselves. So the return leg stays a key segment
  that a caller can write with `ytl_set_keys()`, the keyed path has no new
  branch, and the symmetric eases keep the bits of v0.6.0. Rejected:
  private ease codes that read the forward segment at the mirrored time.
  They give the mirror bit for bit, but they add a compare to every keyed
  channel on every frame, and a caller's keys could not describe the
  track.
- **The reverse ease and the mirror agree to float rounding.** The test
  reads every non-STEP ease at 1 ms steps over two cycles: the largest
  difference between v(s + d + x) and v(s + d - x) is 1 float ulp, on
  BEZIER.
- **STEP keeps its square wave.** STEP jumps at the end of its leg. Its
  reverse jumps back at the start of the return leg, so `to` would show
  for no time at all, and a STEP yoyo would do nothing. The square wave
  (from, then to, then from) is what a STEP yoyo is for (the flicker in
  the manual).
- **The v0.6.0 rule is deleted.** The repository's yoyo tweens
  (`examples/gfx/gallery.c` and `text.c`) use COSINE and LOG, which give
  the same bits under both rules. An option would cost a flag and its
  tests for no user.
- **A second curve per channel.** A BEZIER yoyo on a channel needs its
  curve and the reverse. The channel record grows from 248 to 264 bytes,
  and the handle from 65,792 to 70,016 bytes (MinGW-w64, 64-bit). A
  sequence's block takes one more curve slot for it, and
  `ytl_check_ops()` counts it. A target tween turns its curve when it
  reads the return leg, with the same float expressions, so it gives the
  channel's bits.

### A set leaves a record

A set changes a field on the frame where an event at the same time
lands. Before v0.7.0 the storage had no trace of it, so "what changed on
the screen, and when" needed a MARK beside each set. Now each set adds an
event of kind `YTL_TARGET_SET` at its start: code is the tween's id, value
is `to[0]`, user is the field's address. A STEP tween is read at the end
of the lead window, where events land, so the record's frame is the frame
that showed the set. The test checks this in the 900 cases of the set
against a SET event, at three leads.

A set is a STEP tween of duration 0 without `to_current`, so
`ytl_animate()` with those fields records too. The record is a tween's
event, like a callback's: a kill, a cancel and an overwrite delete it, a
rewind fires it again, a skip skips it, and the pool check counts it.

**Starts and ends of tweens do not log.** A tween with a duration is read
at the onset. Its first frame is the first onset at or after its start,
and the caller's flip log already has that onset. A record would land by
the event rule, up to the lead (half a frame) early, and so could name the
frame before the first frame that shows the tween. Also, a record at each
start and end would put two events per tween in the storage, which a
script that retargets every frame must then prune. `on_start` and
`on_complete` give these events when a study needs them.

### Prune frees overridden tweens

The latest started tween drives a field, and the earlier tweens stay, so
that a rewind can bring them back. On a base that is never cleared (a
script on the RT base), they fill the pool. `YTL_OVERWRITE_ALL` was the
only way out. Now `ytl_prune()` frees them too.

The rule, per field: L is the last tween in (start, id) order that has
started by the base's latest evaluated onset and has its start values. L
or a later tween drives the field at every time from L's start on, and no
later tween reads a tween before L for its start value. So the tweens
before L are needed only by a rewind before L's start. The prune frees
them, except:

- a tween with a callback or record event that has not fired yet: a long
  tween's `on_complete`, or a late `on_start`;
- a tween whose callback is queued or running in this evaluate (a prune
  called from a callback).

Its fired events are deleted with the base's other fired events.

**The horizon.** The prune already deleted every fired event up to the
base's reach (the furthest window end evaluated). So a rewind before the
reach cannot replay the events, and the prune declares that past closed.
The horizon is reach + 1: from there on, a rewind or a seek replays the
targets bit for bit. Before it, a target shows what its remaining tweens
give, as a channel recomputes from the events that remain.
`ytl_horizon()` gives the time, the latest over the base's prunes, so a
player can keep its seeks inside the window. A clear ends it.

Rejected:

- **A refusal of a seek before the horizon.** `ytl_anchor()` and
  `ytl_skip()` have always accepted any time, and a pruned base has always
  recomputed from what remains. A refusal would break callers for a case
  the query lets them avoid.
- **A window argument on `ytl_prune()`.** The prune deletes every event up
  to the reach already, so the reach is the window that the caller
  declared. A second number could only disagree with it.
- **L as the last started tween, resolved or not.** A tween posted after
  the last evaluate at "now" has started but has no start value yet. It
  reads the tween before it at the next evaluate, so that tween must stay.
  Mutant `v07-19` checks this.

### Measured cost

The per-field cost of the evaluate, v0.6.0 against v0.7.0, with
`examples/timeline/bench_targets.c` (1000 fields, CUBIC_OUT, ns per field
per frame above the empty frame), and with a scratch copy of it whose
tweens are yoyos with 1 s legs, so that half of the frames read a return
leg. Each value is from one run of the bench (the minimum of 5
interleaved rounds' medians); the cells give the median and the range
of 5 runs in two sessions (the second at 8% load), old and new
interleaved. Machine: Intel i7-1360P, Windows 11, on AC power, through
the measurement lock. MinGW-w64 gcc 16.1 -O2 and MSVC cl /O2 (Visual
Studio 18):

| Workload | v0.6.0 gcc | v0.7.0 gcc | v0.6.0 MSVC | v0.7.0 MSVC |
|---|---|---|---|---|
| targets | 6.75 (5.73 to 7.80) | 6.98 (6.57 to 7.93) | 9.70 (8.51 to 10.40) | 8.30 (8.01 to 9.69) |
| yoyo targets | 7.94 (7.01 to 8.24) | 7.95 (6.76 to 8.96) | 10.73 (10.56 to 11.78) | 10.71 (9.36 to 11.61) |

One MSVC yoyo run of v0.7.0 measured 25.4 ns, with every other row of
the same run 2 to 3 times slower too; it is left out as a disturbed run.
The keyed path, 256 LINEAR channels (the scratch program of "Targets"),
the smallest of 8 runs' minimum per evaluate: gcc 1238 ns (v0.6.0)
against 1219 ns (v0.7.0), MSVC 1566 ns against 1578 ns.

So no change is larger than the spread of one build between runs (about
2 ns per field). The 8 ns per field of v0.6.0 stays, and the yoyo's
reverse ease (a switch on the return leg) does not show.

### Verification

The writer of the implementation also wrote the checks, in
`tests/adapt/timeline_test_targets.h` and `timeline_test.c`:

- **Yoyo.** Every ease but STEP, BEZIER on an asymmetric curve, as a
  channel tween, a sequence tween and a target tween with 2 cycles, read
  at 1 ms steps: the three paths give the same bits, and each is within
  2e-6 of the rule read literally (fold the cycle, mirror the return leg,
  read the forward leg with the pow()-based eases). CUBIC_OUT's return
  leg moves less in its first 10 ms than in its last 10 ms. The three
  models of the earlier tests (the blind model's tween driver, the
  sequence test's hand-built keys and the target model) now read the
  return leg at the mirrored time, or, for the hand-built keys, with the
  reverse ease and curve; the sequence test compares the builder with them
  bit for bit. The blind test's QUAD_IN check at the middle of the return
  leg now wants 0.25 (v0.6.0: 0.75). The STEP square wave and its 200
  random cases pass unchanged.
- **Records.** The fields of a record; no record for a jump with another
  ease, a STEP with a duration, or a STEP with `to_current`; a 3-component
  set; the report's frame equal to the first frame that shows the set; a
  rewind that fires it again; a skip that skips it; a kill, an overwrite
  and a cancel that delete it; a full storage that refuses a set. In the
  900 cases of a set against a SET event at three leads, the record's frame
  is the SET event's frame in all.
- **Prune.** A trial with 76 tweens on 4 fields, played to 983 ms and
  pruned: 58 tweens freed (the count per field is derived in the test),
  the horizon equal to `ytl_window()`'s end + 1, then continuous play, two
  rewinds and a skip inside the window, 120 frames, the same bits as a
  handle that was not pruned. A rewind to 0, before the horizon: the
  fields differ, and the fields whose first tweens were freed show their
  rest values. A second prune after it keeps the horizon; a clear ends
  it. A long tween's pending `on_complete` and a late `on_start` keep their
  tweens until they fire. A prune from an `on_start` keeps the tween being
  called and the tween queued after it, and both callbacks run.
- **The long run.** A script on the RT base retargets a field every frame
  and sets another every 5th frame, and prunes every 8th frame between its
  posts and the evaluate, for 100,000 frames (28 minutes at 60 Hz) in a
  pool of 16 tweens and 64 events: no call fails, at most 12 tweens are
  live, and every frame shows the same bits as a second handle that uses
  `YTL_OVERWRITE_ALL`. Without the prune the pool is full at frame 13.

The earlier checks pass with the same model statistics.

Mutations of the new code, one at a time: 26, all caught (`v07-00` to
`v07-25` in [tests/mutate/timeline.toml](../tests/mutate/timeline.toml)).
The first run left `v07-19` (L taken without its start values) alive: the
long run pruned after the evaluate, when every started tween was
resolved. It now prunes between the posts and the evaluate, as a script
does, and the mutant is caught. The whole list ran again after the
change: 92 mutants, 89 caught and the 3 known equivalents
(`rate-01`, `rate-03`, `rate-15`) alive, as expected.

Built and run on 2026-10-12: the `mingw` preset (MinGW-w64 gcc 16.1, 76
of 76 tests pass, no warning) and `msvc-full` (MSVC, 88 of 88 pass, no
warning; the first build failed on C4310, a cast that truncated a
constant, which is fixed). The test also builds with gcc -std=c11 -Wall
-Wextra -Wpedantic -Wshadow -Wfloat-conversion -Werror. A scratch
program hashed every value of a yoyo per ease under v0.6.0 and v0.7.0:
LINEAR, STEP, COSINE, LOG, QUAD_IN_OUT and CUBIC_IN_OUT give the same
hash, the other five differ.

The compile check `tests/compile/timeline.c` reads the set's record and
calls `ytl_prune()` and `ytl_horizon()`, as C99 and as C++17.

## Trigger channels and parameter tables (v0.8.0)

v0.8.0 closes two gaps that block the player
([experiment_format.md](experiment_format.md) 1.4, G1 and the timeline
part of G6).

### A trigger names its channel

Until v0.7.0, `ytl_trigger()` and `YTL_OP_TRIGGER` took a code only, and
the builder wrote target 0. A definition with two output roles (an EEG
amplifier and a second marker line) could not say in an op table which
role fires. The fired event had no field that told the frame loop where
to send the code.

Decisions:

- **The op's `ch` is the trigger channel.** `ytl_op` already has `ch`, and
  a TRIGGER did not read it. The event's `target` was free for a TRIGGER
  ("the caller's"). So the builder copies `ch` to `target`, and the report
  carries it unchanged: `yscr_trigger(&scr, e.target, e.code)`. No new op
  kind, no new field, and `ytl_op` stays 96 bytes.
- **A trigger channel is not a timeline channel.** It indexes the caller's
  outputs (`yscr_trigger()`'s channel, a role), so it drives no value, binds
  nothing and has no bound at `desc.n_channels`. A tween and a trigger can
  use the same number. Its range is 0 to `INT32_MAX`. The player checks it
  against its own roles.
- **Existing callers keep channel 0.** `ytl_trigger(&q, code)` and
  `YTL_TRIGGER(code)` are unchanged and give 0. The new forms are
  `ytl_trigger_ch(&q, ch, code)` and `YTL_TRIGGER_CH(ch, code)`. C has no
  default arguments, so a second name is the cost of not breaking the
  callers.
- **Negative is refused in sequences, not in `ytl_add()`.** The sequence
  calls, `ytl_run()` and `ytl_check_ops()` return `YTL_ERR_ARG` for a
  negative channel, so the table's `min` is a bound the check enforces.
  `ytl_add()` keeps any target for a TRIGGER, as before: the blind test's
  model adds TRIGGERs with targets from -1000 to 1000, and a caller of the
  raw event API can have done the same. (v0.8.1 reverses this: see
  [the next section](#ytl_add-refuses-a-negative-trigger-channel-v081).)
  One behavior changes: an op table
  that set a nonzero `ch` on a TRIGGER ran with target 0 before, and now
  carries `ch` (or is refused when `ch` is negative). No caller in the
  repository set it.

### One table per op kind

rig_spec.md principle 8 asks for a table of every parameter a designer
sets. For the timeline, that is the fields of an op table (4.5.1, "The
same sequence as data"), which a definition's `events` and `after` steps
lower to (experiment_format.md 2.5), and the plain fields of `ytl_desc`.

The row is the older tables' row (`name`, `type`, `min`, `max`, `def`,
`unit`, `doc`) plus `values`, the names and numbers of an enum. Every
new table of this round uses it, so the tables read alike. Three
choices:

- **`ytl_op_params(kind, &n)`, not one flat table.** A kind reads a subset
  of `ytl_op`: WAIT reads `t_ns` only, TRIGGER `ch` and `code`. A flat
  table would need a kind mask in every row, a field the shared shape does
  not have. Per-kind tables give the loader its key check directly ("is
  this key a row of this kind?") and give the designer one panel per kind.
  TO and THEN share a table, and so do ON and OFF, and WAIT and AT have
  the same field with two docs.
- **The list of kinds is a row, not a second struct.** `YTL_OP_NONE` (0,
  which the op enum did not name before) gives one row, `op`, of type
  `enum` with values `none=0,on=1,...,then=9`. So the kinds' names and
  numbers come from the same row shape as every other enum (the ease's
  values), and a loader parses one format. The names are the C suffixes
  in lower case (`set_value`), not the definition's step keys (`set`):
  the mapping from a step to an op is the player's (one step can give
  several ops), and the shared rule keeps names the same in every header.
- **`def` is the value in use for a zero field, not another spelling of
  zero.** `tween.cycles` 0 plays once, so `def` is 1; `lead` 0 is half a
  frame, so `def` is 0.5. The test checks both ways: the field at 0 and at
  `def` give the same bytes, and for these two the value in use (the
  tween's end, `ytl_lead()`) equals `def`.

What is not a row: a tween's `start_ns` and `start_set` (a sequence
refuses `start_set`, and `start_ns` is unused without it), the desc's
pointers, and targets (`ytl_anim`), which have no op form because a
pack's table cannot hold an address.

Bounds are inclusive and are the checks of `ytl_check_ops()` and
`ytl_open()`, field by field. Three details:

- A time's bound is 2^62 - 512, the largest double below 2^62. 2^62 - 1
  rounds to 2^62 as a double, which the check refuses, so that bound
  would lie. The 511 ns below 2^62 pass the check and are outside the
  table.
- `lead` is [0, 1) in the table. `YTL_LEAD_NONE` (-1) is a sentinel
  outside it, named in the row's doc.
- A rule that joins fields (a tween's end below 2^62, LOG with a positive
  `from`, a channel below `desc.n_channels`, `keep_velocity` only on a
  channel's first tween) is in the row's doc, not in `min` and `max`.

### Verification

The writer of the implementation also wrote the checks, in
`tests/adapt/timeline_test.c` (11e and 11f):

- **Trigger channels.** Channels 0, 3, `INT32_MAX`, 0 and 5 from
  `ytl_trigger()`, `ytl_trigger_ch()`, `YTL_TRIGGER()` and
  `YTL_TRIGGER_CH()` come back in the report's `target` and in
  `ytl_peek()`'s. A negative channel is refused by the call (sticky, the
  calls before it kept), by `ytl_check_ops()` and by `ytl_run()` with the
  handle, storage and arena unchanged. A channel past `n_channels`, and
  one equal to a tweened channel, pass. `ytl_add()` still takes target -5
  (until v0.8.1).
  The random sequence model now gives a TRIGGER's `ch` as its target and
  draws a negative channel for about 1 trigger in 29 (from the drawn code,
  so the random stream is unchanged): the builder, `ytl_run()` and the
  hand-built events agree on 600 tables.
- **Tables.** For every kind: the rows are exactly the fields the manual
  says the kind reads, in `ytl_op` order; every name is a field of
  `ytl_op` (a map written in the test from the declarations, with
  `offsetof`) and its `type` is the field's; `min` <= `def` <= `max`; `unit`
  is `"ns"` exactly for the `_ns` fields; `values` is set exactly for
  enums, in value order, contiguous from `min` to `max`, and each name
  gives its C constant (`trigger=4` is `YTL_OP_TRIGGER`, `bezier=6` is
  `YTL_EASE_BEZIER`). The field at 0 and at `def` give the same storage,
  arena and 40 frames of values. Every field that is not a row, set to a
  nonzero value, changes nothing. Each field at `min` and at `max` passes
  `ytl_check_ops()` (or fails for another reason than `YTL_ERR_ARG`:
  `tween.cycles` at its max fills the arena), and just past them is
  refused (integers: min - 1 and max + 1; times: +-2^62; floats: 0.25 past
  a finite bound, or an infinity). The desc: the same, by `ytl_open()`,
  and the handles from a zero field and from `def` are the same bytes.

Mutations of the new code, one at a time: 14, all caught (`v08-00` to
`v08-13` in [tests/mutate/timeline.toml](../tests/mutate/timeline.toml)):
a trigger that loses its channel (op and call), a negative channel taken
(call and check), a trigger channel checked against `n_channels`, a MARK
that reads `ch`, `def` 0 for `tween.cycles` and `lead`, and five wrong
rows (a channel's max, the ease's max, two kind names swapped, THEN with
no rows, a negative duration's min, a time bound of 2^62). A comparison
of a zero field with `def` alone cannot catch the two `def` mutants (0
and 1 cycles both play once), so the test also checks the value in use. The anchors of the whole list
(106) match; the earlier 92 were not run again this round.

Built and run on 2026-10-21: MinGW-w64 gcc 16.1 (the test at C11 with
`-Wall -Wextra -Wpedantic -Wshadow -Wfloat-conversion -Werror`; the
compile checks as C99, C++17 and, for the op macros, C++20); the `msvc`
preset (MSVC, no warning): the timeline, video and response tests, the
compile checks (C, C++17, C++20), `timeline_trial`, `timeline_tracking`
and `timeline_bench 500`; and the `msvc-full` preset (no warning): every
program of CI's "Run examples (simulated display)" step, with its
expected exit code, `screen_hello`, `gfx_trial` and `trial_eeg_triggers`
included. The sequence model now reports 600
tables: 164 clean, 394 refused with `YTL_ERR_ARG` and 42 with
`YTL_ERR_BOUND`.

## ytl_add() refuses a negative trigger channel (v0.8.1)

v0.8.0 refused a negative trigger channel in the sequence calls and the op
tables, but `ytl_add()` and `ytl_add_n()` still took one. So a fired
TRIGGER's `target` was a channel of 0 to `INT32_MAX` or any value,
depending on the path that added it. A frame loop that indexes its outputs
with `target` had to check for a negative value that most paths could not
give.

Decisions:

- **`ytl_add()` refuses it as the other paths do.** A TRIGGER with a
  negative `target` is `YTL_ERR_ARG`, with the message, and `ytl_add_n()`
  adds none of the batch. The check is in the one function that checks
  every event, so `ytl_trigger_ch()` lost its own copy of it: the call
  gives the same error, call index and message through `ytl_add()`.
- **No upper bound below `INT32_MAX`.** A trigger channel indexes the
  caller's outputs, and the timeline does not know how many there are
  (`yscr_trigger()` has 8, a player maps roles to several devices). The
  op table's row has `max` `INT32_MAX`, and the player checks a channel
  against its own roles (v0.8.0). A bound here would be a second, wrong
  copy of the player's.
- **The other reported-only kinds keep any target.** A MARK's and a user
  kind's `target` are the caller's.

Callers checked: every `YTL_TRIGGER` event and call in `examples/`,
`tests/` and `bindings/` gives channel 0 or a positive one.
`yds_event_from_timeline()` (ysp/dataset.h) copies `target` to
`control` and reads no sign; ysp/response.h reads `target` of ONSET
events only; ysp/screen.h, ysp/video.h and ysp/device.h read no
timeline event's target. Two places in the timeline test relied on the
old behavior: the v0.8.0 check that `ytl_add()` takes target -5, now a
check that it refuses it, and the random model, whose TRIGGERs drew
targets from -1000 to 1000. The model now flips a negative draw to
positive for 7 triggers in 8 (from the drawn code, so the random stream
is the same) and predicts `YTL_ERR_ARG` for the rest: its rejected adds
go from 1431 to 1491, every one predicted.

### Verification

In `tests/adapt/timeline_test.c` (4 and 11e): `ytl_add()` refuses
channels -1, -5 and `INT32_MIN` with `YTL_ERR_ARG` and the message
`ysp_timeline: ytl_add: invalid argument`; `ytl_add_n()` refuses a batch
with one such event and adds nothing; channels 0, 999 and `INT32_MAX`
pass; a MARK keeps targets -7 and -5. The v0.8.0 checks of the sequence
call (refused, sticky, call index 1, the call before it kept) pass with
the call's own check gone.

Mutations of the new check, one at a time: 4, all caught (`v081-00` to
`v081-03` in [tests/mutate/timeline.toml](../tests/mutate/timeline.toml)):
the check removed, -1 taken, 0 refused, and a MARK's negative target
refused. `v08-02`, the call's own check removed, went with that check.

The whole list ran again on 2026-10-22, MinGW-w64 gcc 16.1, `-O2`, 4 jobs,
on the Windows development machine with other workers building (CPU load
sampled every 20 s: median 99 %, mean 86 %, 1 to 5 builds live):
109 mutants, 106 caught and the 3 known equivalents (`rate-01`,
`rate-03`, `rate-15`) survived, no timeout, 609 s. The 3 survivors, run
again alone under the timing guard (load 1 %, AC), pass every check, as
does the unmutated test: they are equivalents, not load artifacts.

Built and run on 2026-10-22: the `msvc` preset (76 of 76 tests) and
`msvc-full` with `YSP_BUILD_PACK=ON` and layout off (92 of 92; GitHub was
unreachable, so Skribidi and HarfBuzz could not be fetched), no warning;
every program of CI's "Run examples (simulated display)" step on
`msvc-full`, 78 of 78 with the expected exit code; the `mingw` preset
with pack (82 of 82); WSL Ubuntu gcc 11.4 (`linux` preset, the Tracy
check off for the same reason; 79 of 79); and the timeline's compile
check and test under emscripten (CI's wasm flags) in node.

## Animate a stimulus field

This how-to animates the fields of ysp/gfx.h stimuli with targets. The
complete program is `examples/timeline/gfx_trial.c`. "Targets" above
gives the rules.

1. Give the timeline its pools. Make them as large as the fields and the
   tweens that are alive at one time.

   ```c
   static ytl_target_slot targets[8];
   static ytl_tween_slot tweens[16];
   td.targets = targets; td.target_capacity = 8;
   td.tweens = tweens;   td.tween_capacity = 16;
   ```

2. Set each field to the value it must show before its first tween. The
   timeline does not write a field before its first tween starts.

   ```c
   fix.visible = grating.visible = 0.0f;
   ```

3. Write the trial on a sequence, in the order that it plays. Check the
   sequence's error once, at the end.

   ```c
   ytl_seq q = ytl_seq_on(&tl, TRIAL);
   ytl_tween_set(&q, &fix.visible, 1.0f);
   ytl_wait(&q, YRT_MS(500));
   ytl_tween_set(&q, &fix.visible, 0.0f);
   ytl_tween_set(&q, &grating.visible, 1.0f);
   ytl_trigger(&q, 12);
   ytl_tween_from_to(&q, &grating.contrast, 0.0f, 0.3f, YRT_MS(100), YTL_EASE_COSINE);
   ytl_wait(&q, YRT_MS(600));
   ytl_tween_to(&q, &grating.contrast, 0.0f, YRT_MS(100), YTL_EASE_COSINE);
   ytl_wait(&q, YRT_MS(100));
   ytl_tween_set(&q, &grating.visible, 0.0f);
   if (q.err < 0) fprintf(stderr, "%s\n", ytl_error(&tl));
   ```

4. Anchor the trial base at the first frame, and call `ytl_evaluate()`
   with each frame's predicted onset before you draw. Do not copy these
   fields with `ygfx_apply()`: the evaluate wrote them.

5. Between trials, call `ytl_clear(&tl, TRIAL)`, set the fields again, and
   build the next trial. The clear frees the targets, so the next trial
   reads the fields again.

Variations:

- **A color or a position pair.** Use `ytl_animate()` with `a.n = 3` and
  `a.target = g.color`, or `a.n = 2` on two adjacent floats.
- **A stagger.** Set `a.stagger_ns` and call `ytl_animate_n(&q, &a,
  sizeof dots[0], n)` with `a.target = &dots[0].y`.
- **A record of a set.** Since v0.7.0 each set adds a `YTL_TARGET_SET`
  event, so the report of `ytl_evaluate()` and the storage give the frame
  that showed it. Log `fired[i].frame` with `fired[i].user` (the field's
  address) and `fired[i].value`. Give the storage one event per set. A
  trigger for the port is still a trigger, as step 3 adds.
- **A script that retargets.** Use `ytl_seq_now(&tl, base)`, and either
  set `a.overwrite = YTL_OVERWRITE_ALL` or call `ytl_prune(&tl, base)`
  from time to time, so the old tweens do not fill the pool. The prune
  also deletes the fired events, so log the reports first.

The conversion of `gfx_trial.c` from channels to targets took the file
from 147 to 138 lines. It removed the channel enum, the key table, the
binding table, an event helper and the per-frame `ygfx_apply()` call. A
scratch program ran both versions on the same frames at 60, 144 and
500 Hz: every frame showed the same `visible` values and the same contrast
bits.

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

- Parameter tables (v0.8.0): the names are the C fields. The map from a
  definition's step keys to ops and rows (`set` to `set_value`, `wait_s`
  to WAIT's `t_ns`, a role to a trigger channel) is the player's, and
  nothing checks it yet. The tables do not list a target tween's fields,
  a track (`ytl_track`) or a key (`ytl_key`): none is a step.
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
- Targets (v0.6.0):
  - `ytl_prune()` does not free target tweens. On a base that is never
    cleared, the tweens that a later start overrides stay in the pool
    until a kill; `YTL_OVERWRITE_ALL` is the way to retarget there.
  - A set changes a field and leaves no landing record. A mark or a
    trigger at the same time gives one.
  - No target form of the op tables, and no binding from Python, MATLAB
    or a script layer.
  - Not run on clang or emscripten. This round ran MinGW gcc 16.1, MSVC
    19.44 and gcc 11.4 under WSL2 (ASan and UBSan).
