# ysp/audio.h design notes

This note explains why `ysp/audio.h` has its current form and records
what was measured. The manual in the header tells you how to use it. The
plan that asked for the header is section 4.4 of
[rig_spec.md](rig_spec.md).

All numbers come from one machine: a Windows 11 25H2 laptop (build 26200),
13th Gen Intel Core i7-1360P, on AC power, Balanced plan. The output is
"Speakers (Realtek(R) Audio)", whose Default Format is 48000 Hz, 2
channels, 24 bits in 32. The shared-mode mix format is float, 48000 Hz,
2 channels. miniaudio is 0.11.25. No loopback cable or second audio
interface was available, so no number here is an acoustic or electrical
latency. The user kept the endpoint muted at volume 0; runs that needed
the loopback tap unmuted it at volume 0 for the run and restored the mute.
Every timing run held the shared measurement lock, released between runs.

## What miniaudio does that the plan did not expect

Read in the miniaudio 0.11.25 source:

- With a nonzero sample rate in shared mode, miniaudio asks WASAPI to
  resample (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`) and then reports the
  rate it asked for as the device's internal rate. A read-back after
  init cannot see the OS resampler. So the header asks for no format, no
  rate and no channel count: the client format is then the device's, and
  miniaudio's converter is a passthrough, which the header checks.
- Exclusive mode opens at the endpoint's `PKEY_AudioEngine_DeviceFormat`,
  the Default Format in the Sound control panel, whatever rate was asked
  for. So exclusive mode does not fix a rate mismatch, and the open
  message says where to change the rate instead.
- miniaudio never uses `IAudioClock`, and its capture path throws away
  each packet's device position and QPC time. The header gets
  `IAudioClock` from miniaudio's `IAudioClient` itself, with its own COM
  declarations (`tests/compile/audio_com.cpp` checks every slot
  against the SDK on MSVC and MinGW), and the loopback test uses raw
  WASAPI.
- miniaudio starts the stream on an empty buffer. Measured: the position
  stays 0 for the first three callbacks, as Microsoft documents ("its
  device position might remain 0 for a few milliseconds until the audio
  data has had time to propagate"). Reports that have not moved are not
  used.

## What a WASAPI position is on this machine

Microsoft documents `IAudioClock::GetPosition`'s position as "the stream
position of the sample that is currently playing through the speakers",
with a QPC time "at the time that the audio endpoint device read the
device position". Measured over 10 minutes (60060 callbacks):

| Quantity | Result |
|---|---|
| stamp minus callback entry | 1.2 us p50, 3.3 us p99: the stamp is taken during the call |
| stream frames written minus position | 1857 to 1913, 1895 p50 (39.5 ms) |
| step of the position between callbacks | 440 to 478 frames, not 480 |
| residual of the reports from a straight line | sd 87 us; correlated (r = 0.54) with how late the callback ran |
| `GetPosition()` cost | 2.0 us p50, 8.1 us p99, 203 us max |

So a position is the engine's last pass, stamped when the call is made:
a report is late by up to a few hundred microseconds and never early.
The first design took an onset's time from the two reports around its
frame. One report can be hundreds of microseconds late, so the header
takes every onset's time from the fit over many reports instead. A
report then only confirms, in order, that the device has played past the
frame with no underrun since. No API reports when a given frame reached
the output, so the time is a model value, confirmed, never observed.

## Tiers

No v0.1 path reaches tier 1, whose definition (rig_spec.md section 8,
ysp/screen.h) is an OS observation of the onset on a path the header can
verify. WASAPI shared onsets are tier 2, with the conditions stated: the
time is the device-clock fit's, the frame was confirmed played without an
underrun by later position reports, and the digital loopback test found
every burst at its planned frame and the time 67 us p99 from its median
at idle and 20 us under load in one run, against a one-frame bar of 20.8
us. Exclusive mode is tier 3: the loopback capture cannot see it, and on
this laptop it is slower than shared mode (Periods and buffers). Every
unconfirmed onset (no report past its
frame, an underrun, callback times only) is tier 3 and carries
YAU_ONSET_UNCONFIRMED.

`IAudioClient::GetStreamLatency` returned 0 for this stream (the slot is
checked against the SDK). The header reports it as the claim it is.

## The fit

Two clocks with their own crystals drift apart. Measured here: the device
ran +3.9 ppm against QPC over 10 minutes. At 48 kHz one frame is 20.8 us,
so even this drift moves a nominal-rate plan by one frame in 5 s, and a
50 ppm device would do it in 0.4 s. The header fits the device's frame
period on the ysp_rt clock and plans each onset in device frames. It
never resamples and never inserts a frame (principle 5); a long buffer's
end follows the device clock and the END record says where it ended.

M5 replayed the 10-minute run through each candidate estimator, the way
the header runs it (one point per bucket, a refit every second over a
sliding window), and scored each prediction of the frame one second ahead
against the 10-minute line. Constant offsets do not matter here (the
loopback median absorbs them), so the score is the deviation from the
median.

| Estimator | 0.5 to 15 s: p50 / p99 us | 15 s to 10 min: p50 / p99 us |
|---|---|---|
| reports, least squares on the bucket minima, 50 ms, 1024 points | 6.2 / 22.1 | 23.7 / 108.4 |
| reports, the same, 50 ms, 4096 points | 6.2 / 22.1 | 12.2 / 98.8 |
| reports, the same, 200 ms, 1024 points (kept) | 6.3 / 21.3 | 10.5 / 109.8 |
| reports, the same, 400 ms, 512 points | 18.6 / 51.1 | 14.1 / 123.2 |
| reports, least squares on the bucket means, 50 ms | | 35.0 / 116.6 |
| reports, lower envelope of the bucket minima, 50 ms | 28.5 / 73.7 (slope from 5 s) | 40.5 / 357.5 |
| callbacks, lower envelope of the bucket minima, 50 ms (kept) | 55.5 / 101.0 | 33.9 / 206.7 |
| callbacks, least squares on the bucket means, 50 ms | | 61.7 / 359.1 |

The slope is fitted once the points span 5 s; before that the slope is
nominal and the offset comes from the last second. With least squares, a
slope from 2 s of reports gave 80.8 us at p99 in the first 15 s, from 5 s
22.1 us, from 10 s 20.3 us.

So reports are fitted by least squares on the least late report of each
200 ms, over 1024 points (205 s), and callback times (the only source on
the other backends) by the lower envelope of the least late callback of
each 50 ms. The design's guess, the envelope for reports, lost: the
reports are not purely late, and the envelope follows their outliers. On
the scripted device of the core test, where callbacks are late by an
exponential with a 1 ms mean, the envelope recovers the offset within
0.3 us and least squares on the same points misses it by 182 to 188 us.

One refit over 1024 points, timed (MSVC /O2, 200 runs, quiet machine):
least squares 5.5 us p50, 10.2 us max; the envelope 12.0 us p50, 22.9
us max. At 4096 points (timed under load) least squares took 58 us p50,
over the 50 us bar, for no better prediction. The fit runs in the
callback once a second.

### Reports early under load

In one 3-minute run with 8 spinning threads and a 60 Hz frame loop that
spins 8 ms per frame, the position jumped ahead of the fit by up to 45
ms for tens of seconds, with callbacks up to 24.5 ms late but the queue
never empty (written minus position fell to 480 frames at worst). Fitted
as they were, those reports made the fit claim -100 ppm. A report can be
late, never early, so the header now keeps any report more than half a
period early out of the fit, flags the sounds in flight XRUN (their
onsets unconfirmed, tier 3), and starts the fit over if the jump lasts 2 s.
The two loopback runs under the same load afterwards saw no such jump, so
whether the sound moves with the position when it jumps is not known.

## Scheduling accuracy (M4)

`tests/loopback/audio_loopback.c --digital` plays bursts of 64 frames
(a fixed +-1 sequence at -40 dBFS, at endpoint volume 0) and captures the
same endpoint with WASAPI loopback, raw. It finds each burst by
cross-correlation and compares where it is in the capture with where
ysp/audio.h planned it, and when.

The tap is not the samples as written. Muted, the capture was all zeros;
unmuted at volume 0.02 the bursts were scaled by 0.0029, at volume 0 by
0.00054, quantized to 2^-27 and smeared by a filter (the first nonzero
samples came before the burst). So the tap is after the endpoint volume
and an effect on this machine, and bit-exactness cannot be checked
there; placement and time can.

| Run | Bursts | At the planned frame distance from the one before | Capture time minus onset, deviation from its median: p50 / p99 / max us |
|---|---|---|---|
| idle, first fit design (envelope, 2 s slope) | 100 | 100 of 100 | 30 / 512 / 513 (a sawtooth: each refit moved the plan) |
| idle, least squares, 50 ms buckets | 700 | 700 of 700 | 17.8 / 41.0 / 90.9 |
| idle | 700 | 681 of 684, 2 across capture gaps (3 off by -480, -676 and -16 frames, next to the capture's own discontinuities) | 12.9 / 55.9 / 304 |
| idle, final header | 700 | 700 of 700 | 21.8 / 67.3 / 219.6 (63.5 / 67.8 after the first 6 s) |
| load, before the set-aside rule | 500 | 499 of 500 | 6.3 / 299.5 / 10005 |
| load, final header | 500 | 500 of 500 | 6.2 / 19.8 / 139.9 |

Placement held wherever the capture itself had no gap: no frame was
inserted, dropped or resampled between the mixer and the engine's
output. The time bar (p99 within a frame, 20.8 us) was missed at idle (56
to 67 us) and met in one run under load (19.8 us). The deviation wanders
slowly, by tens of microseconds over tens of seconds, and both sides of
the comparison are fits of noisy stamps of the same engine clock, so this
test cannot say which side wanders.

The capture's own packet stamps move by 228 us at p99, so the capture
time is the capture's frame count through a least-squares line of its
packets, not each packet's stamp. The median offset (33.0 ms) is the
distance from the position point to the tap; the tap comes later than
the "currently playing" position. A tap before the speakers should come
earlier, so either the position is not the sample at the speakers or the
loopback packet's stamp is not when the engine mixed it; this test cannot
tell which. It is not a latency.

Lead: with the default 10 ms period, the shortest lead with no LATE
record was 43.5 to 54.5 ms (three runs), against 50 to 55 ms from
`yau_lead_ns()` in the same runs. (v0.3.0: 40 ms at the default queue, 30
ms at queue 1; Periods and buffers.) That is what `ytl_peek()` must look ahead for audio on this
machine; `ytl_peek()` does not exist yet in ysp/timeline.h v0.2.0
(rig_spec.md 4.5.1).

## Callbacks (M2, M3) and cost (M6, M8)

Shared mode ran at a 10 ms period whatever was asked: 240 and 96 frames
gave the same 480-frame callbacks. The endpoint offers a 480-frame
engine period only (Periods and buffers). So the default period is 10 ms.

| Run | Callbacks | Interval p1 / p50 / p99 / p99.9 / max ms | Underruns |
|---|---|---|---|
| idle, 10 min | 60060 | 9.71 / 10.00 / 10.29 / 10.93 / 14.35 | 0 |
| asked 240 frames, 3 min | 18061 | 9.55 / 10.00 / 10.42 / 10.79 / 11.44 | 0 |
| asked 96 frames, 3 min | 18065 | 9.51 / 10.00 / 10.49 / 10.94 / 13.72 | 0 |
| load, 3 min | 18064 | 8.94 / 10.00 / 10.77 / 24.52 / 46.51 | 0 (the jump above) |
| load, asked 240, 3 min | 18061 | 9.48 / 10.00 / 10.51 / 10.88 / 14.83 | 0 |
| M3 setup: miniaudio's default priority, load, 3 min | 18088 | 9.71 / 10.00 / 10.28 / 11.12 / 19.32 | 0 |
| M3 setup: MMCSS "Pro Audio", load, 3 min | 18088 | 9.75 / 10.00 / 10.26 / 11.01 / 13.27 | 0 |
| M3 setup: `yrt_thread_elevate()`, load, 3 min | 18066 | 9.60 / 10.00 / 10.39 / 10.87 / 12.15 | 0 |
| M3 setup: MMCSS and `yrt_thread_elevate()`, load, 3 min | 18088 | 9.70 / 10.00 / 10.30 / 10.87 / 16.46 | 0 |
| final header (MMCSS), load, 3 min | 18066 | 9.66 / 10.00 / 10.35 / 10.97 / 18.03 | 0 |

The rule set before M3 was the fewest underruns, then the smallest p99
interval error. No setup had an underrun, and MMCSS had the smallest p99
error (0.26 ms, against 0.28, 0.39 and 0.30), so it is the one kept and
the others are deleted. The differences are small and come from one run
each; the earlier load run with `yrt_thread_elevate()` (the first row
under "load" above) had a p99.9 of 24.5 ms. A 10-minute idle run with
MMCSS was not repeated.

"Load" is 8 threads spinning at normal priority and a 60 Hz frame loop at
TIME_CRITICAL that spins 8 ms of each frame.

Exclusive mode, 3 runs of 30 s: the device opened at s32 (24 valid bits),
48000 Hz, a 1440-frame buffer, and `GetStreamLatency` said 30 ms. The
callbacks came in bursts (interval p50 0.12 to 0.15 ms, p99 72 to 80 ms,
max 98 to 101 ms), there were 2 underruns per run, `GetPosition` took up
to 45 ms, and the fit could not settle (spread 9 ms). Exclusive mode
through miniaudio 0.11.25 does not work on this machine; the header
still opens it, and its records say so (XRUN, a fit spread in the describe
line). Not fixed in v0.1. v0.3.0 asks for one period per buffer, which
removes the underruns and puts every onset on its plan, but the driver
still buffers about 100 ms (Periods and buffers).

Cost per callback (render, without `GetPosition`), MSVC /O2:

| Voices (stereo buffers) | Mean us | p50 us | p99 us | max us |
|---|---|---|---|---|
| 0 (10 min) | 3.3 | 2.5 | 18.6 | 1941 |
| 8, first mixer (1 min) | 23.5 | 22.5 | 57.7 | 709 |
| 32, first mixer (1 min) | 55.6 | 43.1 | 158.4 | 2367 |
| 8, final mixer (1 min) | 11.5 | 10.2 | 39.9 | 376 |
| 32, final mixer (1 min) | 18.7 | 17.0 | 56.7 | 269 |

The bar was 20 us mean and 100 us p99 at 32 voices. The first mixer
missed it (a channel-mask test per sample); the final one, with the
channel list built once per run and straight loops for full-mask mono to
stereo and for full-mask buffers, meets it (18.7 and 56.7 us). The max
values are preemptions. `GetPosition()` adds 2 to 4 us p50 and 6 to 20
us p99 (bar 20 us p99: met).

Frame thread (`yau_play_at` + `yau_update`, one sound per frame at 60
Hz, under the load): 1.86 us mean, 3.7 us p99, 410 us max. Bar 5 us mean
and 20 us p99: met.

## Allocations (M7)

MSVC debug build, `_CrtSetAllocHook`, with the caller's arena:

| Window | C runtime heap calls | Made by |
|---|---|---|
| `yau_open()` | 18 | 10 through the header's allocation callbacks by miniaudio (context, device enumeration for `desc.device`, the data converter, its two threads); 8 by the C runtime making per-thread data when an OS component raised and caught a C++ exception on the new threads. None by the header's own code |
| 20 s of playing, one sound per 60 Hz frame (1200 plays) | 0 | |
| control: one malloc and free per callback, 10 s | 2000 | the control |

The stacks came from `RtlCaptureStackBackTrace` in the hook, named from
the PDB. miniaudio made 0 allocations after open in every run. Without
the caller's arena, `yau_open()` adds one `malloc` for the arena, and
`yau_close()` frees it.

## Underruns (M0)

A 60 ms sleep injected in one callback, against 39.5 ms of queue:

| | Result |
|---|---|
| position during the stall | stood at the frames written (253440), for three callbacks, then moved on |
| written minus position after | 1880 to 1895, as before |
| time of a given position, before and after | 40.3 ms later after the stall |

So on WASAPI the position halts while the device has no data and counts
only frames played, and every later frame plays later by the time it
stood. The header sees the position reach the frames written, records an
XRUN, flags the sounds in flight, and starts the fit over. The first
design shifted the map by the silence instead, which would be right for a
device whose position counts the silence; the scripted device of the core
test checks both. Measured with the final header: one XRUN record for the
stall.

## Core test

`tests/adapt/audio_test.c` runs the header without miniaudio against
a scripted device on a virtual clock: a device clock at a set drift
(+-100 ppm), callbacks late by a seeded exponential, position reports
stamped late, injected underruns and position jumps, a tape of every
sample written. It checks the plan (200 of 200 start frames on the
truth's frame, or within 2 us of a tie), the fit (drift within 0.03 ppm,
offset within 1.1 us), LATE, cancel, stop with its fade, gain, loops,
routing, clipping, the replan after the slope is known, underruns of both
kinds (a position that counts the silence, one that halts), position
jumps, the ring records field by field, the queues and their limits, the
strict open's refusals, bit-exact 16- and 24-bit output, synthesis, the
arena, and the device on a real thread against the frame thread. Fourteen
deliberate mutations of the header each make it fail. With the two
queues' atomics made plain loads and stores, ThreadSanitizer reports 70
races; as written, none (3 runs).

Builds, warnings as errors: MSVC 19.44 (C, C++17; compile check, COM
check, core test, examples, loopback test), MinGW-w64 gcc 16.1 (the
same), gcc 11.4 in WSL2 (C99 -O3, C11 under ASan and UBSan, C11 under
ThreadSanitizer, C++17 compile check; the null device runs there), emcc
6.0.10 (core test run under node; the header with miniaudio's Web Audio
backend compiled as gnu11 and gnu++17, because miniaudio's `EM_ASM`
needs a GNU dialect). Not built here: clang on Linux and macOS (CI does).

## Streams (v0.2.0)

Movie sound must take the same path as every other sound. In
Psychtoolbox it does not: a movie's audio goes through GStreamer's own
sink, outside PsychPortAudio's schedule and records. A first soundtrack
design for `ysp/video.h` used `desc.source`. That slot is fixed at
`yau_open()` and there is only one. A soundtrack there would be
scheduled differently from every other sound, and ysp_audio confirms only
its own voices. So v0.2.0 adds a voice whose samples come from a ring.
`desc.source` stays the Audio source extension slot.

### Design

- A stream is a voice. It takes a voice slot and an id. It uses the same
  plan, the same confirmation by position reports, the same ONSET and END
  records and the same stop, gain and cancel commands as a buffer. Only
  the source of the samples differs. The gain, fade and channel-routing
  loops are one kernel for buffers and streams. The kernel was moved out
  of the buffer path with no change to its arithmetic. A regression tape
  of buffer voices through every path of the kernel (mono to stereo,
  masks, a stereo buffer, a gain ramp, a stop fade, loops, clipping, on
  f32 and s16 devices) is byte-identical between v0.1.0 and v0.2.0, on
  MSVC and on MinGW gcc. `audio_test --tape FILE` writes the tape.
- The lock rule. At the start the callback fixes origin, the stream frame
  of sample 0. Sample s plays on frame origin + s for the whole play. A
  ring underrun leaves the frame silent and moves the stream's position
  on by exactly that frame. A sample that arrives after its frame is
  discarded. A late start skips the samples it missed. Neither moves
  origin, so a movie clock that follows the stream stays on its sound
  frame for frame. The read index never passes the write index. The
  device thread keeps the play position separately, so the producer never
  writes a slot that the device thread reads.
- GAP records are not confirmed. A gap's frames are exact by
  construction, because the mixer wrote the silence on them. Its time is
  the fit's, as an END record's time is, and END records are not
  confirmed either. Whether the device timing held during a gap is in the
  XRUN records, in stream frames. A GAP record has the XRUN flag when a
  device underrun came while it lasted.
- GAP and STREAM records go only to the event ring. The callback-to-frame
  queue carries ONSET and END, and the loss of one of those would leak a
  voice count. Every gap is counted also without an event ring: in
  `yau_stream_get_info()` (gaps, gap_frames, discarded), in
  `yau_onset.gap_frames` and in `yau_caps.gaps`. A test with 300 gaps
  in one play dropped no message.
- Loops are the producer's job. The ring is read once. A loop held in
  memory is a buffer, and buffers already loop. The sample count must go
  on across cycles, or the lock rule breaks at each wrap. `yau_wav`
  loops by reading sample s from file frame s mod A.
- A play at 0 waits for the preroll (a quarter of the ring by default;
  `YAU_PREROLL_NONE` for no wait), so an "as soon as it can" stream
  does not begin with a gap. A timed play checks nothing at the call. The
  producer can catch up before a start that is seconds away, and a start
  with an empty ring is a recorded gap. `ready` and `frames_to_start` in
  `yau_stream_get_info()` let the caller check before the start.
- A reset and a play cannot race. Each takes the stream's state by
  compare-and-swap, and only the frame thread and the producer use it.
  The device thread only loads and stores, and it stores the read index
  before it stores ENDED.
- Memory. A stream is 256 bytes, one cache line for each writer. A stream
  voice keeps its stream pointer in the place of the buffer pointer, so
  the voice array stays 200 bytes a voice. A `yau_wav` is about 17 KB,
  of which 16 KB is its read buffer.
- The ring holds float samples. M-S2 measured an int16 ring, widened in
  the callback and fused with the mix: no benefit, so it was deleted.

### WAV files

The parser is in `ysp/audio.h` for three reasons. The ysp_video
soundtrack and every long sound need the same parser. `ysp/video.h`
already includes `ysp/audio.h` for the soundtrack. Loading audio is this
header's job (rig_spec 5.2: audio is widened to float exactly at load).
The parser accepts RIFF, RF64 and BW64 files with 16-bit, 24-bit,
24-in-32 or 32-bit float samples, and refuses everything else by name.

A path is read with stdio and `setvbuf(f, NULL, _IONBF, 0)`. So the C
runtime allocates no stdio buffer on the producer thread, and each read
(at most 16 KB) goes to the OS (`ReadFile` on Windows, `read()` on POSIX),
which still caches the file. The header does not use
`FILE_FLAG_NO_BUFFERING` or `O_DIRECT`, which bypass the OS cache and
need sector-aligned offsets, sizes and buffers. `yau_wav_load()` reads a
whole file into the arena and widens it in place.

### The WAV probe (v0.2.1)

The pack tool checks every AUDIO entry with the player's own parser, so
the two cannot disagree on what a valid WAV file is (docs/pack.md, AUDIO).
The tool has no device. v0.2.1 splits the parse in two: a scan of the
file's form (the chunks, the format tag, the bits, the block align, the
data size), and the device's checks (the rate, the channel count, the
speaker map). `yau_wav_probe()` runs the scan alone and gives the rate,
the channels and the speaker mask in `yau_wav_info` for the caller to
compare with the project's. `yau_wav_open()` and `yau_wav_load()` run
both, with the same messages as v0.2.0.

Tests: the probe gives `yau_wav_open()`'s info byte for byte from a path,
a reader and memory; it refuses each of the eight form refusals with its
reason and passes the rate and channel cases with their values; a missing
path is `YAU_ERR_IO`; and the test file can be removed after the probe
(on Windows an open file cannot be). Mutations: `tests/mutate/audio.toml`,
4 mutants (the probe checks the rate, the scan leaves the info empty, the
open skips the device checks, the probe leaves a path open), 4 killed.
The test passes on MinGW gcc 16.1, MSVC 19.44 and emcc 6.0.10 (node).

### Tests

`tests/adapt/audio_test.c` uses the scripted device and the virtual
clock, as before. The stream cases use identity samples. Every value is a
multiple of 2^-13, so sums are exact, and the checks compare every output
frame to the bit.

| Case | What it checks |
|---|---|
| Placement | A 60 s stream through a 1000-frame ring (2880 wraps), written in chunks of 1, 13, 479, 997 and 4097 frames: 2879016 frames checked, 0 wrong. 50 timed starts at every phase between frames, at +37, +100 and -100 ppm: each start on the truth's frame (or within 2 us of a tie), `sample` = first, tier 2, confirmed |
| Ring | Regions up to the wrap, a commit past the region refused, the space after a block, end, reset, the states. Refusals: loops, offset or a buffer with a stream; 3 channels; a ring under 2 periods; a negative first; a preroll over the ring; another handle's stream; an arena that is too small |
| Gaps | Three producer stalls, the gaps starting inside a block: silence exactly on the frames whose samples were missing, every later sample on its planned frame, one GAP record per run; records, counters and output agree. A gap at the start keeps the ONSET on its planned frame, flagged GAP |
| Late start | origin from the target, the skipped samples in the STREAM record and in the discard count. A late start with too little data is LATE and GAP |
| At 0 | Waits for the preroll, starts on the next block when the preroll is there. `YAU_PREROLL_NONE` starts at once. An end inside the preroll starts the play. A cancel while it waits gives CANCELED |
| Mix | 2 streams (mono to the right channel only, stereo) and a buffer voice in one run: the output is the model sum, bit for bit. A buffer and a stream with the same samples, gain ramp and stop fade on the two channels give identical output. CLIPPED reaches the stream |
| Device | Both kinds of device underrun while a stream plays (XRUN, UNCONFIRMED, the identity kept in stream frames), a device with callback times only (tier 3), close in the middle of a stream (ENDED) |
| Gap storm | 300 gaps in one play: 300 counted, 0 messages dropped, ONSET and END complete |
| Threads | A producer thread (random chunks, random stalls of up to 40 ms) races the device thread while the frame thread plays, stops and replays the stream 40 times and the producer resets it between plays. Every output frame is checked against the record of its play, and the silent frames equal the records' gap counts |
| WAV | Seven forms (s16, s24 mono, 24 in 32, float, EXTENSIBLE float, RF64 with ds64, BW64 with an odd chunk) parsed, streamed through a 1000-frame ring, played and checked to the bit, signs included. `yau_wav_load` of each gives the same values. Loops over a ring smaller than the file, a seek after the end, a seek message during a play that waits for the end. Path, reader and memory give identical samples. A NaN ends the stream before it. Ten refusals with their reasons, and the speaker mask against the project map |
| WAV on a pump | `yau_wav_step` and `yau_wav_on_msg` on a real `yrt_pump`, the device on a real thread, 12 plays with seeks sent as messages: every play starts on its seek sample, every frame checked |
| Regression tape | Buffer voices through every kernel path on f32 and s16 devices: byte-identical to v0.1.0 (MSVC and MinGW) |

Builds: MSVC 19.44 (C /W4 /WX; the compile check as C and C++17),
MinGW-w64 gcc 16.1 (C11 test; C11, C++17 and C99 compile checks; the
loopback test and audio_clockstats), gcc 11.4 in WSL2 (C99 -O3, C11 under
ASan and UBSan, C11 under ThreadSanitizer: 0 reports in 3 runs), emcc
6.0.10 (the core test under node with threads; the header with
miniaudio's Web Audio backend compiled as gnu11 and gnu++17). The compile
checks play a stream on miniaudio's null device. Again on 2026-10-06:
gcc 13.3 in WSL2 (the core test at -O2 and under ASan and UBSan), gcc
13.3 and clang 18.1 in an Ubuntu 24.04 container (every target, ctest
and `audio_tone --null`, with the CI jobs' flags), MinGW-w64 gcc 16.2
and the CI wasm job's commands: pass.

Mutations of the header, each in a scratch copy. All 18 below made the
core test fail. The last two are races that only ThreadSanitizer can see:

| Mutation | Failed checks |
|---|---|
| An underrun moves origin instead of discarding | 11 |
| A late start plays the first sample on its first frame (no skip) | 3 |
| The late skip off by one | 1 |
| The ring slot off by one at the wrap | 68 |
| Producer space one frame too large | 36 |
| A gap counts the whole block, not its missing frames | 5 (it survived until the gap test made gaps start inside a block) |
| A play at 0 starts without the preroll | 7 |
| Stream onsets skipped by the confirmation | 354 |
| `end` ignored | 459 |
| Late samples played instead of discarded | 12 |
| The stream path without the kernel's gain and fade | 7 |
| 24-bit samples without sign extension | 6 |
| ds64 sizes ignored | 4 |
| Reset allowed in any state | 15 |
| WAV loops without the modulo | 4 |
| GAP records not pushed | 2 |
| origin without the first sample's index | 300 |
| The ONSET record's `sample` off by one | 76 |
| `w` committed with a plain store (TSan) | 4, 4, 4 reports in 3 runs |
| The read index stored after ENDED (TSan) | 2, 2, 2 reports in 3 runs |

### Measurements

Conditions for every row: the laptop above, on AC power (battery status
2), the measurement lock held for each row and released between rows,
`C:\tmp\psy-quiet` checked before each row, MSVC 19.44 /O2, WASAPI shared
mode on the default endpoint with no format request. The endpoint stayed
muted at volume 0 and no endpoint setting was touched. All streams and
voices played silence or a -60 dBFS tone, and the mix cost does not
depend on the values. Rows were interleaved, one round after the other.
The tool is `examples/audio/clockstats.c` (`--streams`, `--wav`, `--ring`).
The baseline is v0.1.0 from `git show HEAD:psy_audio.h` (the header's
path before the rename to ysp), built from the
same tool source.

M-S1 and M-S2, render cost per callback, 60 s a row, 3 rounds. A stream
here is stereo, with a 1 s ring that a producer thread fills with
silence in 4096-frame steps.

| Row | Mean us, rounds 1 / 2 / 3 (average) | p99 us | max us | Gaps | Underruns |
|---|---|---|---|---|---|
| a. v0.1.0, 32 voices | 12.16 / 13.76 / 8.22 (11.38) | 29.4 / 27.2 / 27.2 | 123 / 70 / 65 | | 0 |
| b. v0.2.0, 32 voices | 9.92 / 12.87 / 8.26 (10.35) | 26.9 / 30.7 / 23.2 | 128 / 67 / 118 | | 0 |
| c. v0.2.0, 32 voices + 1 stream | 11.73 / 11.03 / 8.67 (10.48) | 30.3 / 30.5 / 25.5 | 68 / 112 / 113 | 0 | 0 |
| d. v0.2.0, 32 voices + 4 streams | 13.06 / 12.59 / 10.56 (12.07) | 33.7 / 34.7 / 30.5 | 75 / 91 / 115 | 0 | 0 |
| e. v0.2.0, 0 voices + 1 stream | 4.95 / 5.56 / 4.01 (4.84) | 13.8 / 13.4 / 11.2 | 87 / 64 / 34 | 0 | 0 |
| f. int16 ring, 32 voices + 4 streams | 13.14 / 11.18 / 12.09 (12.14) | 32.3 / 33.0 / 27.8 | 89 / 102 / 293 | 0 | 0 |

Bars and results:

- (b) against (a), no regression for buffer voices: met. The mean was
  1.0 us lower, inside the spread of the rounds (8.2 to 13.8 us for the
  same row). The 32-voice means are lower than v0.1.0's 18.7 us of
  2026-10-05 in both headers; the comparison is the interleaved one.
- (c) at most (b) + 2 us: met (+0.13 us).
- (d) at most (b) + 6 us and 25 us, p99 at most 100 us: met (+1.7 us,
  12.07 us, p99 34.7 us).
- M-S2: keep the int16 ring only if it is at least 1 us faster at 4
  streams. It was 0.07 us slower (12.14 against 12.07 us), so it was
  deleted and the ring holds float.

M-S3, the producer, no device. `acquire` + `commit`: 10000 calls each.
`yau_wav_feed` of 4096 frames: one 3-minute stereo file per format
(34.6, 51.8 and 69.1 MB), every 4096-frame block of it, from the path
twice and from memory once.

| Operation | Mean us | p50 us | p99 us | max us |
|---|---|---|---|---|
| acquire + commit, 480 frames | 0.04 | 0.00 | 0.10 | 0.10 |
| acquire + commit, 4096 frames | 0.05 | 0.00 | 0.10 | 0.20 |
| feed s16, path, pass 1 | 15.2 | 11.8 | 63.8 | 199.1 |
| feed s16, path, pass 2 | 15.4 | 14.1 | 58.8 | 109.7 |
| feed s16, memory | 5.1 | 4.5 | 8.7 | 35.0 |
| feed s24, path, pass 1 | 24.4 | 18.9 | 76.7 | 119.0 |
| feed s24, path, pass 2 | 22.1 | 18.9 | 71.0 | 129.7 |
| feed s24, memory | 13.9 | 15.1 | 19.6 | 47.4 |
| feed f32, path, pass 1 | 22.3 | 19.4 | 66.7 | 119.3 |
| feed f32, path, pass 2 | 21.8 | 19.7 | 65.9 | 150.5 |
| feed f32, memory | 11.9 | 8.4 | 19.1 | 48.9 |

Bars: acquire + commit at most 1 us p99, and feed at most 0.5 ms p99 per
4096 frames: both met. The page cache: each file was written just before
the run, so pass 1 was already served from the OS cache. Pass 1 and pass
2 do not differ, so this run says nothing about a cold read from the
disk. That was not measured, because Windows gives no user-level way to
drop a file from the cache. A cold read is a producer cost: the ring
(500 ms of margin at the default size, M-S4) absorbs it, and the device
thread never waits for it.

M-S4, the default ring size. One `yau_wav` stream (the 3-minute s16
file, looped) on a `yrt_pump`, woken by a 60 Hz frame loop when the
ring is under half. Load: 8 threads spinning at normal priority, and a
frame loop at TIME_CRITICAL that spins 8 ms of each frame. 3 minutes a
row, 2 rounds.

| Ring | Gaps | Lowest fill seen by the frame loop | Render mean us | Frame thread mean / p99 us |
|---|---|---|---|---|
| 0.25 s | 0, 0 | 110 ms, 110 ms | 8.46, 8.35 | 3.49 / 15.8, 3.11 / 14.7 |
| 0.5 s | 0, 0 | 230 ms, 230 ms | 8.11, 8.35 | 2.77 / 13.9, 2.64 / 12.8 |
| 1 s | 0, 0 | 480 ms, 480 ms | 8.34, 9.40 | 2.36 / 11.3, 2.51 / 11.2 |

The bar was 0 gaps and a lowest fill of at least half the ring. No row
met the second half, and the bar was wrong. The frame loop wakes the pump
when the ring falls under half, so the lowest fill is half the ring minus
one wake interval and a block (about 20 ms) at every size. It measures
the wake rule, not the producer. What the runs show is that a step of the
pump refills the ring before the next frame: no gap at any size. The
default stays 1 s. The margin against a producer stall is the half ring
that is left when the pump is woken: 480 ms at 1 s, 110 ms at 0.25 s. A
ysp_video pump that also decodes a 1080p frame (about 15 ms a step) and
reads a cold file shares that margin. The cost is memory: 384 KB at 48
kHz stereo. A smaller ring is one desc field for a program that wants
less memory.

M-S5, heap calls, MSVC debug build with `_CrtSetAllocHook`, 20 s, 4
voices and 4 streams, a frame loop playing a sound every frame:

| Run | C runtime heap calls in the run | miniaudio |
|---|---|---|
| 4 WAV streams on a pump | 0 | 0 |
| 4 streams from a producer thread | 0 | 0 |
| control (one malloc and free per callback, 10 s) | 2000 | 0 |

The first run of the producer-thread row counted 2 calls. In that run
the tool started the producer thread just before the count began. With
the thread started 100 ms before the count, the row reads 0, so the 2
calls are put down to the thread's start (they were not traced). The C
runtime calls of `fopen` in `yau_wav_open` come at open, on the frame
thread, before the count. None follow, because the stdio buffer is off.

M-S6, the frame thread under the same load as M-S4: `yau_play_at` and
`yau_update` per 60 Hz frame, plus, with streams,
`yau_stream_get_info` and `yau_wav_wants` for each stream and a pump
wake when one is under half. 60 s a row, 2 rounds, 8 voices.

| Row | Mean us | p99 us | max us | Render mean us |
|---|---|---|---|---|
| 4 WAV streams | 2.93, 2.70 | 13.0, 11.5 | 20, 18 | 13.77, 13.77 |
| no streams | 1.74, 1.97 | 2.6, 2.7 | 13, 8 | 9.30, 9.49 |

Bar: mean at most 5 us and p99 at most 20 us: met. The p99 with streams
is about 10 us higher. That difference was not broken down. The one
system call that streams add to the path is the pump wake
(`yrt_pump_submit` takes the pump's mutex and signals its condition
variable), which comes about once every 10 frames.

Not run: the line-in check of a stream (`tests/loopback/audio_loopback.c
--line --stream`, built, not run: the user runs the hardware checks).
WASAPI's loopback capture with a stream was not run either, because it
needs the endpoint unmuted.

## Periods and buffers (v0.3.0)

The question: why the shortest safe lead in WASAPI shared mode was about
50 ms here, and whether it can go under 10 ms. Conditions: the laptop
above, 2026-10-09, on AC power, MSVC 19.44 /O2 unless a row says gcc
(MinGW gcc 16.1 -O2), the measurement lock held for each row. Every run
played silence or a -40 dBFS tone at endpoint volume 0. "Load" is the
load of the earlier sections: 8 threads spinning at normal priority and a
60 Hz frame loop at TIME_CRITICAL that spins 8 ms of each frame.

### What the endpoint offers

`examples/audio/wasapi_periods.c` (`audio_wasapi_periods --init
--exclusive`) asks each render endpoint with
`IAudioClient3::GetSharedModeEnginePeriod` for its mix format, and
initializes (never starts) shared and exclusive streams to read the
buffer each one gets.

| Endpoint | Mix format | Device format | Engine period: default / fundamental / min / max (frames) | `GetDevicePeriod` default / min |
|---|---|---|---|---|
| Speakers (Realtek(R) Audio), the default | float, 48000 Hz, 2 ch | int 32 (24 valid), 48000 Hz, 2 ch | 480 / 480 / 480 / 480 | 10 ms / 3 ms |
| Pico Streaming Speaker, Steam Streaming Speakers, Steam Streaming Microphone (virtual) | float, 48000 Hz, 2 ch | int 16 or 32 | 480 / 480 / 480 / 480 | 10 ms / 3 ms |

No endpoint offers a shared-mode period under 10 ms. The 3 ms minimum of
`GetDevicePeriod` is exclusive mode's.

| Stream on the Realtek endpoint (initialized, not started) | Buffer frames | `GetStreamLatency` |
|---|---|---|
| shared, `Initialize` with a buffer of 0, 10 or 20 ms | 1056 (22 ms) | 0 |
| shared, `Initialize` with 30 ms | 1440 (30 ms) | 0 |
| shared, `InitializeSharedAudioStream` at 480 | 1056 (22 ms) | 0 |
| exclusive, event-driven, period 3 ms | 144 (3 ms) | 3 ms |
| exclusive, event-driven, period 10 ms | 480 (10 ms) | 10 ms |

### miniaudio's choice

In miniaudio 0.11.25, shared mode calls
`IAudioClient3::InitializeSharedAudioStream` when `GetSharedModeEnginePeriod`
succeeds and the period asked for, rounded down to a multiple of the
fundamental period and then clamped to the minimum and maximum, is at
least the period asked for. Otherwise it falls back to
`IAudioClient::Initialize` at `periods` (3) times the period. The
`AUTOCONVERTPCM` flag also blocks the IAudioClient3 path; the header never
sets it. Here the endpoint offers 480 frames only, so every period asked
for became 480: that is why 240 and 96 frames gave 480-frame callbacks
(Callbacks, above). The header got the IAudioClient3 stream (buffer 1056),
not the 1440-frame buffer, which was exclusive mode's. Nothing in the
header or in miniaudio blocks a smaller period on this endpoint: the
driver has none. On an endpoint that offers small periods, `desc.period`
must be a multiple of the fundamental period (or under the minimum, which
gives the minimum); any other value silently falls back to the default
period. The Realtek driver is the vendor's; whether Microsoft's in-box
HD Audio driver offers small periods on this codec was not tried, because
it means changing the machine's driver.

### Where the 39.5 ms went

At callback entry the stream frames written stood 1898 frames (39.5 ms)
ahead of the device position (10 minutes idle, p50; 1884 p1, 1901 p99).
miniaudio's WASAPI loop renders a block, then waits for room for it. So
each block waited one period in miniaudio, then went into the buffer
behind one queued period, then passed the engine's own pipeline (about 20
ms from the engine's pass to the position). With the period of phase
before the next callback, the lead was 50 ms.

v0.3.0 holds the render in the header's miniaudio callback until the
engine has taken the buffer down to `desc.queue - 1` periods, waiting on
the WASAPI event that miniaudio would wait on. miniaudio then finds room
at once and never waits on the event itself. A raw WASAPI loop (scratch,
gcc, 20 s), which writes at each event, gave the same steps: written
minus position after the write 2186 frames with 2 periods kept queued and
1707 with 1.

### Queue depth: lead against margin

`audio_clockstats --queue Q`; queue 0 is miniaudio's own order (the v0.2
behavior, removed in v0.3.0).

| Queue | Run | Callbacks | Interval p1 / p50 / p99 / p99.9 / max ms | Written minus position, p50 frames (ms) | Underruns |
|---|---|---|---|---|---|
| 0 (v0.2) | idle, 10 min | 60066 | 9.66 / 10.00 / 10.34 / 10.70 / 12.47 | 1898 (39.5) | 0 |
| 2 (default) | idle, 10 min | 60066 | 9.72 / 10.00 / 10.28 / 10.60 / 11.65 | 1418 (29.5) | 0 |
| 1 | idle, 10 min | 60065 | 9.70 / 10.00 / 10.31 / 10.63 / 11.73 | 938 (19.5) | 0 |
| 0 (v0.2) | load, 3 min | 18067 | 9.84 / 10.00 / 10.17 / 10.74 / 12.26 | 1895 (39.5) | 0 |
| 2 (default) | load, 3 min | 18088 | 9.84 / 10.00 / 10.17 / 10.53 / 11.63 | 1415 (29.5) | 0 |
| 1 | load, 3 min | 18086 | 9.82 / 10.00 / 10.19 / 10.66 / 12.27 | 934 (19.5) | 0 |

Render cost per callback was 2.9 to 3.4 us mean idle and 6.2 to 7.1 us
under load at every queue; the gate's wait is outside it.

The margin: `--stall-at 15 --stall MS` sleeps once in one callback. A
stall that the queue does not ride out halts the position; the table
gives the position's shift against its line after the stall (20 to 25 s
runs, with `--csv`).

| Stall | Queue 0 | Queue 2 | Queue 1 |
|---|---|---|---|
| 8 ms | ridden out | ridden out | ridden out (shift +8 frames) |
| 15 ms | ridden out | ridden out (-2 frames) | halted: -491 frames |
| 25 ms | ridden out (+4 frames) | halted: -502 frames | halted |
| 35 ms | halted: -495 frames | | |

Each period less in the queue is 10 ms less lead and 10 ms less margin.
In every load run of this note with MMCSS, the latest callback came at
most 8 ms late (an 18.03 ms interval, "final header" above); in this
session at most 2.3 ms.

v0.3.0 gave no XRUN record for any of these halts (xruns 0 in each). It
saw an underrun only when a report reached the frames written. After a
stall of one or two periods the late block is in the buffer before the
next report, so the position halts one block short of the frames written
and moves on, and every later onset time was off by the halt (10 ms)
while its record said tier 2. The 60 ms stall of M0 was seen because the
position stood at the frames written. Before v0.3.0 this took a stall of
about 30 ms; at the default it takes about 20 ms, at queue 1 about 10 ms.
v0.3.1 finds these halts (Halts, below).

### Shortest safe lead

`audio_schedule --queue Q`: 10 silent sounds per lead, leads from 200 to
5 ms; the shortest lead with no LATE record at it or any longer lead. Two
runs per queue, interleaved. Residual is fit onset minus plan for the 14
scheduled sounds of the run; every one was tier 2.

| Queue | Shortest safe lead | LATE at the next shorter lead | `yau_lead_ns()` at the handovers, min / median / max ms | Residual, min to max us |
|---|---|---|---|---|
| 0 (v0.2) | 50 ms, 50 ms | 3 and 5 of 10 at 45 ms | 49.3 / 54.5 / 59.3 | -24.7 to +10.0 |
| 2 (default) | 40 ms, 40 ms | 6 and 6 of 10 at 35 ms | 39.5 / 44.3 / 49.5 | -32.1 to +9.5 |
| 1 | 30 ms, 30 ms | 6 and 6 of 10 at 25 ms | 29.5 / 34.5 / 39.6 | -0.5 to +21.3 |

A third run with the final v0.3.0 build at the default: 40 ms, residuals
-6.8 to +26.7 us, tier 2. The residuals stay within the earlier runs'
range (the one-frame rounding allows +-10.4 us; the fit adds tens of
microseconds), so the shorter queue costs no precision. `yau_lead_ns()`
stays a conservative bound: its minimum is the measured safe lead.

Decision: queue 2 is the default. It saves 10 ms of lead, and its 20 ms
margin is more than twice the latest callback measured with MMCSS. Queue
1 saves 10 ms more, but its 10 ms margin is close to that 8 ms, so it is
opt-in. (v0.3.0 also gave as a reason that an underrun at queue 1 was not
detected; v0.3.1 detects it, and the decision stays: see Halts.) Queue 0
had a 10 ms larger margin and no fewer underruns in any run, so it was
deleted.

### Under 10 ms

Not on this endpoint in shared mode: the engine runs a 10 ms period, and
from the engine's pass to the position is about 20 ms more, whatever the
client does. The ways under 10 ms are a driver that offers small shared
periods (run `audio_wasapi_periods`), or exclusive mode on a driver that
handles it.

### Exclusive mode

Why it "opened but did not work": miniaudio's exclusive mode makes one
buffer of `periods` (3) periods, event-driven, and fills the whole buffer
at each event in three callbacks back to back. v0.3.0 asks for one
period per buffer. 60 s runs at idle, gcc builds, then `audio_schedule
--exclusive`:

| Run | Buffer | Callbacks | Interval p50 / p99 / max ms | Written minus position p50 (min to max) frames | Underruns | Shortest safe lead | Residual, 14 sounds |
|---|---|---|---|---|---|---|---|
| 3 periods of 480 (v0.2) | 1440 | 6069 | 0.10 / 81.4 / 103.7 | 5212 (-5188 to 8089) | 3, and 3 in the schedule run | none up to 200 ms | -7.1 us to +62.3 ms |
| 1 period of 480 | 480 | 6081 | 0.23 / 75.2 / 90.9 | 4602 (958 to 5715) | 0 | 125 ms | -5.2 to +7.9 us |
| 1 period of 144 | 144 | 20308 | 0.10 / 74.7 / 88.1 | 3934 (284 to 5132) | 0 | 125 ms | -7.1 to +4.4 us |

With one period the stream runs and every onset is on its plan, so the
change is kept. The callbacks still come in bursts about 75 ms apart, and
the driver holds 30 to 110 ms. A raw WASAPI exclusive loop (scratch, gcc,
10 s, event-driven, buffer prefilled before Start, MMCSS) showed the
same: at 144 frames, events p50 0.13 ms and p99 74.7 ms apart, written
minus position 1511 to 5085 frames; at 480 frames 1850 to 5417. So the
bursts are the driver's, not miniaudio's. `GetPosition` took up to 45 ms
in the miniaudio runs and 0.1 ms in the raw loop; that was not traced.

Two more raw findings, not used by the header. Exclusive mode without the
event (the client polls every 1 ms and keeps 2 periods of 144 frames in
the buffer) ran smoothly, with written minus position 583 frames (12 ms)
p50; miniaudio has no such mode. And in exclusive mode the position
counts the silence of an underrun: with 1 period kept, it ran up to 50778
frames past the frames written. Exclusive mode stays opt-in, tier 3
(the loopback capture cannot see it), and on this laptop it is slower
than shared mode.

### Files

`examples/audio/wasapi_periods.c` (the probe), `audio_clockstats
--queue --long`, `audio_schedule --exclusive --period --queue` and its
leads from 200 down to 5 ms. The core test checks that desc.queue reaches
the device and the caps (0 means 2), the refusal of values out of 0..2,
and the gate's threshold (`YAU__QUEUE_PAD`); the gate's wait and the
exclusive buffer run only on hardware. Mutations: 7 more in
`tests/mutate/audio.toml` (11 in all), 11 killed.

## Exclusive mode compared with cave-audio

The question: the user's cave-audio project (github.com/aforren1/cave-audio,
GPL-3.0) has a WASAPI exclusive sink, `src/sink/wasapi_sink.cpp`. Does
its setup avoid the bursts of "Exclusive mode" above on this laptop?
Conditions: the laptop above, 2026-10-13, AC power, the speaker endpoint
(Realtek(R) Audio behind Intel SST, muted at volume 0), MinGW gcc 16.1
-O2, the measurement lock held for each 30 s run, load at the lock 0 to
20 %. Every run except the `--tone` runs wrote silence.

### The setups

`tests/probe/wasapi_exclusive/wasapi_exclusive_probe.c` makes each choice
a switch. `--preset cave` is cave-audio's sink, `--preset raw` is the raw
loop of v0.3.0. No code is taken from cave-audio; the choices come from
reading it.

| Choice | cave-audio | ysp/audio.h (miniaudio 0.11.25) | Raw loop (v0.3.0) |
|---|---|---|---|
| Interface | `IAudioClient::Initialize` | the same | the same |
| Driving | the event (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`) | the event | the event |
| Period and buffer | the minimum of `GetDevicePeriod` (3 ms, 144 frames) for both | `desc.period` for both (one period per buffer since v0.3.0) | the period asked for, for both |
| `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` | `GetBufferSize`, a new client, a second `Initialize` | the same | not handled |
| Format | the first of float32, int32 (32 valid), int24 (packed), int16 that `IsFormatSupported(EXCLUSIVE)` takes: int24 here | `PKEY_AudioEngine_DeviceFormat`: int32 with 24 valid bits | the device format |
| Prefill | one buffer, `AUDCLNT_BUFFERFLAGS_SILENT` | miniaudio's | one buffer, silent flag |
| Thread | created before `Start`, COM (MTA) on it | miniaudio's | created after `Start`, no COM call |
| MMCSS | "Pro Audio", no priority call | "Pro Audio" | "Pro Audio" |
| At each event | `GetBuffer` of the whole buffer, `GetPosition`, the render, `ReleaseBuffer` with flags 0 | the whole buffer in callbacks of one period | the whole buffer with the silent flag, then `GetPosition` |
| Timer resolution | not changed | not changed | `timeBeginPeriod(1)` |
| Latency it reports | `GetStreamLatency` plus the buffer plus one block (352 frames at a 64-frame block) | the fit of the positions | none |

cave-audio's docs give 352, 416 and 544 frames of exclusive latency on a
Realtek endpoint at blocks of 64, 128 and 256 frames. Those are the
formula above, not a measurement. Its tests do not record callback
intervals or the queue behind the position. Its docs also say that the
format walk stopped at int24 and that the driver gave 144 frames at every
request: the same as here.

### Event-driven

"Lead needed" is the time from one wake to the play time of the first
frame the next wake writes, from a line fitted to the `GetPosition`
pairs: the shortest safe lead of a client that takes no time. "Back to
back" is a wake less than a quarter period after the one before it.

| Run | Format | Buffer | Interval p50 / p99 / max ms | Back to back | Written minus position min / p50 / max frames | Lead needed p50 / max ms |
|---|---|---|---|---|---|---|
| cave, 144 (1) | int24 | 144 | 0.13 / 74.6 / 76.9 | 8717 of 9666 | 1566 / 4094 / 5133 | 85.3 / 107.0 |
| cave, 144 (2) | int24 | 144 | 0.22 / 74.6 / 77.1 | 8647 of 9660 | 1511 / 4117 / 5085 | 85.8 / 106.0 |
| cave, 480 | int24 | 480 | 0.21 / 75.3 / 83.3 | 2099 of 2902 | 1853 / 4452 / 5420 | 92.8 / 113.0 |
| raw, 144 | int32 (24) | 144 | 0.52 / 36.8 / 77.1 | 5202 of 9663 | 1508 / 4932 / 5085 | 102.9 / 106.0 |
| raw, 480 | int32 (24) | 480 | 7.01 / 75.1 / 80.0 | 1257 of 2895 | 1177 / 4449 / 4744 | 92.8 / 98.9 |
| `audio_clockstats --exclusive --period 144` | int32 (24) | 144 | 0.16 / 74.8 / 86.2 | | 286 / 3993 / 5132 | |
| `audio_clockstats --exclusive` | int32 (24) | 480 | 0.35 / 75.1 / 89.2 | | 958 / 4686 / 5419 | |

The wakes come in groups of up to 27 (81 ms of audio at 144 frames), with
gaps of up to 77 ms between them. Then one setting of the cave preset at
a time, at 144 frames:

| Change from the cave preset | Lead needed max ms | Written minus position max frames |
|---|---|---|
| none (two runs) | 107.0, 106.0 | 5133, 5085 |
| the device format (int32, 24 valid) | 105.9 | 5085 |
| `AUDCLNT_BUFFERFLAGS_SILENT` in place of zeros | 106.0 | 5085 |
| `GetPosition` after `ReleaseBuffer` | 105.9 | 5084 |
| `Start` before the thread | 106.0 | 5086 |
| no MMCSS | 106.0 | 5085 |
| no prefill | 109.9 | 5276 |
| `timeBeginPeriod(1)` | 106.0 | 5086 |
| no COM call on the thread | 106.0 | 5086 |
| `AUDCLNT_STREAMOPTIONS_RAW` (`IAudioClient2`) | 106.0 | 5085 |
| 8 threads spinning | 106.0 | 5085 |
| a -40 dBFS tone; the tone gated 100 ms on and off | 106.0; 106.0 | 5084; 5085 |

No client setting changes the bursts or the queue. The position itself
is a clean clock in every run: the line through the `GetPosition` pairs
runs at 48000.22 frames/s (+5 ppm), with a residual of 6 us p50 and 19
us p99 (54 us max in one run, 191 us in another), and the stamp is 15 to
390 us after the wake (p1 to p99). The driver holds what the client
writes; it reports where it is correctly.

### Timer-driven

Exclusive mode without the event (`--poll`): `Initialize` with no flags,
a buffer duration and the 3 ms period; the thread calls `Sleep(1)` with
`timeBeginPeriod(1)` (wakes at 2.0 ms p50, 2.7 ms p99, 5.5 ms max) and
keeps a number of frames in the buffer. Neither cave-audio nor miniaudio
has this mode.

| Run | Format | Buffer granted | Kept frames | Written minus position min / p50 / max frames | Lead needed max ms |
|---|---|---|---|---|---|
| cave (four runs: buffer 288 or 960 asked, 8 threads spinning, gated tone) | int24 | 2048 | 288 | 1034 to 1103 / 1127 to 1191 / 1213 to 1263 | 25.5, 26.5, 26.4, 26.5 |
| cave, the silent flag | int24 | 2048 | 288 | 997 / 1095 / 1166 | 24.4 |
| raw, the format walk | int24 | 2048 | 288 | 954 / 1040 / 1118 | 23.5 |
| raw (two runs) | int32 (24) | 512 | 288 | 2604 / 2693 / 2783; 2657 / 2743 / 2826 | 58.1, 59.1 |
| cave, the device format | int32 (24) | 512 | 288 | 2606 / 2690 / 2783 | 58.1 |
| cave | int24 | 2048 | 480 | 1954 / 2059 / 2126 | 44.6 |
| cave | int24 | 2048 | 192 | the position passed the frames written at 14056 of 14557 wakes | |
| cave, no `timeBeginPeriod` (15.6 ms wakes) | int24 | 2048 | 288 | the position passed the frames written at every wake | |

With no event the stream runs smoothly: no groups, and the driver holds
about 18 ms past the kept frames at int24 and about 50 ms at int32 with 24
valid bits. So here, and only here, cave-audio's format choice matters:
int24 instead of the device format takes 33 ms off the lead.

### Through ysp/audio.h

`audio_schedule --exclusive`, the same day: at 480 frames, one run gave
125 ms as the shortest safe lead (the v0.3.0 value; 14 residuals -4.8 to
+837.6 us). Two more runs at 480 and two at 144 gave LATE records at
every lead up to 200 ms, 14 to 37 XRUN records before the first sound and
447 to 773 by the end. `audio_clockstats --exclusive` (no sounds) gave 0
XRUN records at 144 and at 480. The probe saw no position past the frames
written in any event-driven run. Not traced.

### Conclusion

cave-audio's setup is not better on this laptop: in event-driven
exclusive mode it gives the same groups of wakes, the same 1500 to 5100
frames behind the position and the same 106 ms lead as the raw loop and
the header. Each of its differences from the raw loop, one at a time,
changed nothing. cave-audio did not see it because it reports a latency
from a formula and does not measure the position against the frames
written. On this endpoint that formula (7.3 ms at a 64-frame block) is
low by at least 24 to 100 ms (the frames behind the position alone), and
the error changes from wake to wake, so no constant corrects it.

The cause is the driver's event-driven exclusive path, not exclusive
mode as such: without the event the same driver runs smoothly. The
earlier "Exclusive mode" note gave 583 frames (12 ms) for this mode in a
10 s run; these 30 s runs give 1130 frames at int24 and 2690 at the
device format.

No change to `ysp/audio.h` or its miniaudio config. miniaudio's exclusive
mode is event-driven only and opens at the device format, so no config
reaches the timer-driven int24 path. That path would need WASAPI code in
the header: a 1 ms timer, `timeBeginPeriod(1)`, and its own format walk.
Its raw lead (23.5 to 26.5 ms, with 288 frames kept) is 3.5 to 16.5 ms
under shared mode's 30 ms (queue 1) and 40 ms (queue 2) through the
header, before the header's own cost, and its margin is small: 192 frames
kept underran at almost every wake. Shared mode stays the default;
exclusive mode stays opt-in, tier 3. The open item is the XRUN records of
`audio_schedule --exclusive`, above.

Run it: `sh tests/probe/wasapi_exclusive/run.sh` (the cave and raw
presets at both periods), or the probe with one option changed.

## Halts (v0.3.1)

The problem (Queue depth, above): a stall of a period or two halts the
WASAPI position short of the frames written, so v0.3.0 saw no underrun,
and every later onset time was off by the halt while its record said
tier 2. Conditions: the laptop above, 2026-10-09, AC power, MSVC 19.44
/O2, the measurement lock held for each run.

### What a halt looks like

In the v0.3.0 stall runs (`audio_clockstats --csv`), against a line
through the reports of the second before the stall:

| Stall, queue | Report of the stalled callback | Next reports |
|---|---|---|
| 20 ms, queue 1 | position at frames written minus 480, +0.4 ms | moved 4 frames in 10 ms, then +10.3 ms late, and stay there |
| 25 ms, queue 2 | position at frames written minus 480, 4.1 ms early | one report that did not move, then +10.3 ms late |
| 35 ms, queue 0 (v0.2) | the same, 4.0 ms early | +10.3 ms late |

So after a halt every report is late by the halt, for good. Without a
halt, over the long runs below, no report was more than 0.72 ms late
against a least-squares line through its own 10 s, and the least late of
any 3 moving reports in a row was at most 0.23 ms late.

### The rule

A moving report (its position past the last one) late against the fit
by more than half a period (5 ms here) starts a run. Three in a row are
a halt; any report that is not late ends the run. Until the third, the
reports stay out of the fit and confirm no onset. Then:

- one XRUN record: `u.i64[2]` the halt in ns, the least late report of
  the run but its first (the first can be the halt half over: the
  position moved, then stood); `u.i64[3]` the position of the first late
  report; counted in `caps.xruns`;
- the fit starts over from that report, at once, as after any underrun;
- every sound in flight is flagged XRUN, UNCONFIRMED, tier 3, and gets
  its time from the new fit.

The check runs only while the fit is ready, so not for 0.5 s after open
or after a restart. Three reports and half a period are what the data
above allow: one late report can be a call preempted between the
position and its stamp, and 5 ms is 7 times the latest report measured
without a halt. A shorter run was not needed: every halt here was found
49 to 70 ms after the stall.

At queue 2, a 20 ms stall made the stalled callback's report 9.3 ms
early (the position jumped to the frames written minus 480, then
stood 19 ms): the existing check for early reports writes an XRUN record
for it, and the halt that follows continues that record (the same aux)
and is not counted again.

### Catch rate

`audio_clockstats --stall-at 3 --stall-every 3 --stall MS --csv`, 63 s,
21 stalls a run. A stall halted the position when the reports from 0.3
to 1.3 s after it were off the line of the second before it by more than
2.5 ms (median report).

| Stall | Queue 1: halted, found | Queue 2: halted, found |
|---|---|---|
| 5 ms (0.5 period) | 0 of 21, no record | 0 of 21, no record |
| 10 ms (1 period) | 21 of 21 (halt 10 ms), 21 | 0 of 21, no record |
| 15 ms (1.5 periods) | 21 of 21 (10 ms), 21 | 0 of 21, no record |
| 20 ms (2 periods) | 21 of 21 (20 ms), 21 | 21 of 21 (10 ms), 21 (an early record first) |
| 30 ms (3 periods) | 21 of 21 (30 ms), 21 | 21 of 21 (20 ms), 21 |

126 of 126 halts found, no record for the 84 stalls the queue rode out.
Each found 49 to 70 ms after the stall. The halt in the record minus the
position's shift: -0.19 to +0.36 ms (median +0.11). A stall at queue 1 is
ridden out at 5 ms (and at 8 ms in v0.3.0) and halts at 10 ms; at queue 2
it is ridden out at 15 ms and halts at 20 ms.

### False records

The long runs of v0.3.0, again, with the rule: 10 minutes idle and 3
minutes under load (8 spinning threads, a 60 Hz frame loop spinning 8 ms
and playing a sound each frame) at queue 2 and at queue 1, 156,325
callbacks: 0 XRUN records. Exclusive mode (480 frames, 60 s, callbacks
in bursts about 75 ms apart): 0 records. `audio_schedule` again: shortest
safe lead 40 ms at queue 2 and 30 ms at queue 1, as in v0.3.0; every
onset tier 2, residuals -53.2 to +9.3 us at queue 2 and -6.7 to +0.8 us
at queue 1.

### The onsets a halt affects

`--frame-work 0` plays a sound each frame (lead plus 20 ms) through 21
halts a run. Truth for the time of a frame: a least-squares line through
the reports from 0.3 s after its halt to the next stall. Offsets are p1 to
p99 against that line, whose mean report lateness puts every onset about 130
us early, flagged or not.

| Run | Flagged XRUN, tier 3 | Their time against the line | Not flagged |
|---|---|---|---|
| Queue 1, 10 ms stalls | 63 (3 a halt), residual +9.99 to +10.44 ms | -150 to +161 us | 3679 tier 2, -230 to +98 us; 34 LATE |
| Queue 2, 20 ms stalls | 105 (5 a halt) | 80 of them -147 to +131 us; 25 of them -10.49 to -10.07 ms | 3636 tier 2, -261 to +85 us; 35 LATE |

At queue 1 every flagged onset has the new timing: its residual says the
sound played 10 ms after its target, and its time is right. At queue 2,
80 of the 105 do too; the other 25 completed on the early report, before
the halt was found, with the old fit's time, 10.1 ms before the line.
They are frames the position passed in its jump; whether they reached
the output before or after the halt is not known (no loopback ran), and
their records say XRUN, tier 3. One sound a halt (21 a run) became LATE:
its target came before the first frame the new timing could reach. No
tier 2 onset's time moved with a halt.

### Queue 1 stays opt-in

The rule makes an underrun at either queue visible and its data correctly
labeled, but the sound still has a gap of a period or more. So the
choice is still lead against margin: queue 1 saves 10 ms of lead (30
against 40 ms), and its margin, 8 to 10 ms of callback lateness, is at
the latest callback measured with MMCSS (8 ms late); queue 2's, 15 to 20
ms, is twice that. In this session no callback was more than 2.2 ms late
(18,086 under load at queue 1, 0 underruns), which is not enough to call
8 ms the worst case. Queue 1 stays opt-in, for a machine whose
`audio_clockstats --queue 1` under the experiment's load shows no
callback 8 ms late.

### Tests and files

The scripted device of the core test gained the WASAPI halt: the engine
reads `halt_pipe` frames ahead of the position, so a late block halts the
position that far short of the frames written, for whole periods, and its
report reads the position standing (on time, late, or ahead); and stale
reports, late by 9 ms with no halt. `test_halt` checks halts of 1, 2 and 3
periods (a sound planned before the halt on a frame it moved: XRUN,
UNCONFIRMED, tier 3, its time within 60 us of the truth; the record's
halt within 60 us; one count; a later sound on the truth's frame, tier
2), a stall the queue rides out (no record), the early report before a
halt (two records, one count), two stale reports in a row and four
single ones (no record, the sound they pass tier 2). Mutations: 9 more in
`tests/mutate/audio.toml` (20 in all), 20 killed. `audio_clockstats
--stall-every` repeats the stall and lists every XRUN record.

## Units, open and close, messages (v0.4.0)

v0.4.0 applies the repository conventions (README.md, Conventions). No
rule of the mixer, the fit or the plan changes.

- **Desc time fields end in `_ns`.** `yau_play_desc.at_ns` and the
  synthesis descs' `dur_ns` and `ramp_ns`. They were `int64_t` ns
  already, so only the names change: code that wrote `.dur = 0.1` now
  fails to compile. `YAU_S()` and `YAU_MS()` are gone; ysp/rt.h's
  `YRT_S()` and `YRT_MS()` are the same macros. The record's fields
  (`target`, `onset`, `residual`, `rendered_at`) keep their names: the
  caller only reads them, and they are `int64_t` ns.
- **`yau_stream_open()` and `yau_stream_close()`.** `yau_stream_init()`
  took a desc and the ring, which is what open means. The stream has no
  message buffer: it is four cache lines, one per thread that writes it,
  and a message would add a fifth for one writer. Its open writes the
  owner's `yau_error()`.
- **Close never fails.** `yau_stream_release()` and `yau_wav_close()`
  returned `YAU_ERR_BUSY` while the device still played the stream, and a
  caller that ignored the code freed memory the device still read. Now
  close cancels the play and waits for the device to end it. The device
  stores ENDED after its last read of the stream, so when close sees
  ENDED, the struct and the ring are free. The wait is the device's
  buffer and two periods, from 20 ms to 1 s. A device that does not
  answer in that time (it stopped, or a CUSTOM device whose blocks run
  on the closing thread) gets the stream detached with its memory in
  place, because the device can still read it. An arena ring then stays
  held until `yau_close()`. ysp/video.h already waited for the device
  before it closed the soundtrack, and it still does.
- **Messages.** Every call of the frame thread that fails writes
  `yau_error()` with the call's name, unless the call wrote a more
  specific message first (a counter in the handle tells). The producer's
  calls run on another thread and return codes only, because a write to
  the handle's message would race with the frame thread.
  `yau_stream_sample_at()` and `yau_stream_time_of()` take a non-const
  handle for it. `yau_wav_open()` returns `bool`; its code is in
  `w->error_code`, which ysp/video.h reads to tell an unreadable file
  from a refused one.

Checked on 2026-10-10 (Windows 11, MinGW-w64 gcc 16.1 and MSVC 19.44,
the scripted device on a virtual clock): `tests/adapt/audio_test.c`
passes, with a new test of close on a playing stream (the bound in
virtual time, the hold kept, the next block ending the play) and of the
messages. The USAGE blocks compile as C99 and C++17. Mutants `conv-01`
to `conv-03` are caught.

## Not measured

- Latency to sound. No loopback cable; `--line` is built, not run. It
  reports the measured latency minus the OS's claim when it does.
  `--line --stream` (stream placement and latency) is built, not run.
- CoreAudio, ALSA, PulseAudio, Web Audio: compiled, never run. PulseAudio
  under WSLg plays to the Windows speakers and was not run.
- Exclusive mode beyond the 60 s runs of "Periods and buffers"; under
  load, never.
- Whether the in-box Microsoft HD Audio driver offers shared periods under
  10 ms on this codec.
- Whether the sound follows a WASAPI position jump under load: the jump
  happened once, in a run without the loopback capture.
- When the frames that a report passes in its jump just before a halt
  (Halts, queue 2) reach the output, before or after the halt. A
  line-in loopback with a stall would show it.
- Any other machine, driver, rate or channel count.
