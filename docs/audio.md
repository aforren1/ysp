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
written in any event-driven run. Traced in "Exclusive start (v0.5.1)":
both programs reach a state of real underruns in about half of the
opens, because miniaudio starts the stream on an empty buffer; v0.5.1
writes one silent buffer before Start.

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
exclusive mode stays opt-in, tier 3. The XRUN records of
`audio_schedule --exclusive`, above, are answered in "Exclusive start
(v0.5.1)".

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

## The Web Audio path (v0.4.1)

[web.md](web.md), "Audio: what the records say", has the measurements and
what a browser can claim; this section has the design decisions that
touch the core.

- **Reports, not callbacks.** A browser's `getOutputTimestamp()` is a
  position report in WASAPI's sense (a frame at the output, at a time), so
  the web device feeds it as `tick.pos` and the fit is least squares on
  reports. A page's timer reads it every 10 ms on the main thread, where
  the AudioContext lives; every block passes the newest pair, and one that
  has not moved feeds nothing, as on WASAPI.
- **No callback points.** A web block without a report adds no fit point
  (`web-02`). The first blocks have none at or past stream frame 0, and an
  AudioWorklet's entry time comes from `Date.now()` in 1 ms steps (its
  scope has no `performance.now()`, and `yrt_now_ns()` reads 0 there);
  mixed into a fit of reports, those points moved every plan. Only a
  device with `YAU_TIER_WEB` and `YAU_POS_DEVICE` takes this rule, so
  WASAPI and the null device are unchanged.
- **Two browsers, two readings.** Chrome 154's pairs lie within 0.15 ms
  of a line; Firefox 157's spread over 6 ms (its `contextTime` steps with
  its 512-frame callback, its `performanceTime` follows the read) and are
  stamped `outputLatency` before the read. The device keeps the lower
  envelope (a read at most 0.5 ms above the least late of the last 32
  reads; v0.5.2 moved it from the blocks to the reader, see
  [Web envelope](#web-envelope-v052)) and puts `outputLatency` back where
  a pair is that old. Both rules are in the device's JavaScript, not the
  core: a core rule on report
  spread (the halt rule's half period, 1.3 ms at 128 frames) restarted
  Firefox's fit 41 times in one run before them.
- **One tier.** Every web onset is `YAU_TIER_WEB` (5), confirmed or not
  (`web-01`): confirmation by a browser's own estimate does not make it
  better than tier 3, and an unconfirmed one is not worse than the
  estimate.
- **Waits yield.** On the main thread `yau_open()`'s warm-up and
  `yau_wait()` sleep through `emscripten_sleep()`: a busy wait there would
  stop the ScriptProcessorNode's callbacks and the timer that reads the
  reports. ysp/screen.h's web backend already needs ASYNCIFY or JSPI, and
  miniaudio's AudioWorklet path sleeps the same way at init.
- **The project's format.** The AudioContext is asked for the project's
  rate and channels; the browser converts to the device unseen, so the
  strict open's rule (no conversion) holds only up to the browser.
- **Tests.** `test_web_tier` in `tests/adapt/audio_test.c` runs the scripted
  device as a browser's: tier WEB, a report in one block of 4, entry times
  of 0. The sound lands on the truth's frame, confirmed and tier 5, and an
  unconfirmed one is tier 5 too. `tests/compile/audio.c` checks the tier's
  value. Mutants `web-01` and `web-02` are killed. The JavaScript and the
  rest of the device run only in a browser (`examples/web/av_sync.c`).

## Capture (v0.5.0)

v0.5.0 adds an input on the rig clock: `yau_capture`. This chapter has two
how-to guides, a reference table, the design notes and what was measured.
The manual is the CAPTURE, DETECTORS and WAV OUT sections of the header.

Conditions for every measurement in this chapter: the laptop of this note,
2026-10-12 and 13, on AC power, the measurement lock held for each run
(the guard script; its load figure is the CPU load when the run started),
miniaudio 0.11.25, MinGW-w64 gcc 16.1 -O2. The input is "Microphone Array
(Intel Smart Sound Technology for Digital Microphones)", the laptop's own
microphones behind the SST DSP: in shared mode float, 48000 Hz, 4
channels, a 480-frame period and a 1056-frame buffer; in exclusive mode
16 bits, 48000 Hz, 2 channels. Nobody spoke during the runs: the shared
input read -88 dBFS RMS (the endpoint's processing), the exclusive one
-70 dBFS RMS. No cable, no second interface and no TTL into an input
were available, so no number here is an input latency against a
reference. One run window was disturbed by another worker's browser
(Measurements, the note under the stall table).

### How to record a spoken response

`examples/audio/voice_rt.c` is this guide as a program (`--sim` runs it
without hardware).

1. Open the output and the input. Open the input with
   `yau_capture_desc.format.sample = YAU_S16` if it may run in exclusive
   mode, where microphones give 16 bits.
2. Wait until both fits are ready: `yau_capture_get_info()`'s
   `fit_ready`. `yau_capture_open()` waits for it already, a CUSTOM device
   does not.
3. For each trial, play the stimulus with `yau_play_at(t)`, and get the
   capture frame of `t` with `yau_capture_frame_at(t)`.
4. Each frame of the frame loop, read reader 0 with
   `yau_capture_acquire()` and `yau_capture_release()` (or
   `yau_capture_read()`). Keep the trial's frames if you save them. Feed
   `yau_vkey_feed()` from the frame where you start to listen. On
   speakers, the stimulus comes back through the microphone: listen from
   after its end.
5. When the voice key gives `YAU_VKEY_ON`, the voice onset is
   `yau_capture_time_at(ev.frame)`. The RT is that time minus the
   stimulus's onset from its record (`yau_wait()`).
6. Between trials, write the trial's frames with `yau_wavw_open()`,
   `yau_wavw_write()` and `yau_wavw_close()`. Write the first frame's
   capture frame number to your data file, so each sample has its time.
7. Subtract the loop's latency from the RT, or set the input's
   `desc.offset_ns` to minus the input's share of it, measured as in the
   next guide. Until you do, the RT holds the output's pipeline and the
   input's: 36.6 ms through the air on this laptop (shared output,
   exclusive input; Measurements).

For a long recording, open the input with `readers = 2` and let a
`yrt_pump` (reader 1) call `yau_capture_save()` into one WAV file while
the frame thread runs the voice key on reader 0. The file then has
silence where frames are missing, so its sample s is capture frame
first + s.

### How to measure the output-to-input latency

`examples/audio/loopback_latency.c` plays clicks (12 frames at -20 dBFS,
400 ms apart), finds each one in the input with `yau_edge`, and reports
the input fit's time of the edge minus the output fit's onset, per click.

With a cable (hand steps; this laptop had none):

1. Connect the line output to a line input with a cable. On a USB or
   PCIe interface, use its own output and input: one converter clock.
2. In Sound settings, set both endpoints' Default Format to 48000 Hz.
   Set the input's level so that a -20 dBFS click reads between -30 and
   -6 dBFS in the input.
3. Run, for the shared and the exclusive output against the same input:

       audio_loopback_latency --device "OUT" --in "IN" --paths --clicks 200 --csv lat.csv

   and for one duplex device, which needs both on one interface:

       audio_loopback_latency --device "OUT" --in "IN" --duplex --clicks 200

4. Read the median, p1 and p99 per path. Two fits of two clocks are
   compared: the program prints each fit's drift. On one converter clock
   the drifts agree within the fits' own noise.
5. The median is the whole loop: output pipeline, converter, cable,
   input pipeline. To split it, put a TTL from the trigger output into a
   second input channel at each target (`tests/loopback/audio_loopback.c
   --line --ttl-ch C --ttl-dtr PORT`): both edges are on the input's
   converter clock, so the input's latency cancels and the output's is
   left. The input's share is the loop's median minus that. Set the
   input's `desc.offset_ns` to minus the input's share and the output's
   `desc.onset_offset_ns` to the output's.
6. For the first sound after a silence (rig_spec.md section 13), on each
   output path:

       audio_first_sound --device "OUT" --in "IN" --gaps 1,10,60

   A path that gates on digital silence shows a later onset and a lower
   level for the first sound than for the repeat.

Acoustic, speaker to microphone: the same program with the Windows
endpoint options, which set the volume and mute for the run and put them
back after it:

    audio_loopback_latency --unmute --volume 0.2 --in-unmute --distance-cm CM

The air adds 2.9 us per millimeter (343 m/s).

### Reference

| Call | Thread | What it does |
|---|---|---|
| `yau_capture_open(c, d)`, `yau_capture_close(c)` | frame | An input alone: open, start, wait for the fit (0.5 s); stop and free the ring |
| `yau_desc.capture`, `.capture_desc` | frame | DUPLEX: `yau_open()` opens one device that plays and records |
| `yau_capture_update(c)` | frame | The device thread's fits into the frame thread's copy |
| `yau_capture_acquire(c, r, &n, &first, &gap)`, `_release(c, r, n)` | reader r | The contiguous frames a reader has not released; hand them back |
| `yau_capture_read(c, r, dst, max, &first)` | reader r | Copy and release, up to a gap |
| `yau_capture_save(c, r, wavw, max)` | reader r | To a WAV file, with silence for gaps |
| `yau_capture_time_of(c, F)`, `_time_at(c, f)`, `_frame_at(c, t)` | frame | Capture frame to ysp_rt ns and back, from the fit plus `offset_ns` |
| `yau_capture_get_info(c, &in)`, `_describe(c, buf, cap)` | frame | Format, fit (source, drift, spread, points, ready), tier, counts |
| `yau_capture_block(host, in, n, tick)` | device | The device side, for a CUSTOM input |
| `yau_vkey_init`, `yau_vkey_feed` | any | Voice key |
| `yau_edge_init`, `yau_edge_feed` | any | Click finder |
| `yau_wavw_open`, `_write`, `_close` | one | WAV writer |

| Record (`YRT_SRC_AUDIO`) | Fields |
|---|---|
| `YAU_EV_CAP_OPEN` (9) | i32[0] rate, [1] channels, [2] input format, [3] period, [4] buffer, [5] exclusive, [6] duplex, [7] backend, [8] position source |
| `YAU_EV_CAP_FIT` (10) | as `YAU_EV_FIT`: t_ns t0, aux generation, i64[0] W0, f64[1] frames per second, f64[2] spread ns, u32[6] points, u32[7] source |
| `YAU_EV_CAP_XRUN` (11) | i64[0] the frame where the count goes in, [1] frames lost (0: not known, the fit started over), [2] the first frame that may still carry a number from before the loss |
| `YAU_EV_CAP_DROP` (12) | i64[0] the first frame dropped, [1] frames dropped |
| `YRT_KIND_CLOCK` (`YRT_SRC_RT`), aux 0x41430000 + device index | once a second: u64[0] the reported position in capture frames, t_ns its stamp |

### Design: the fit of an input

An input has its own clock, as an output has. The header fits it the
same way: one point per bucket (200 ms of reports, 50 ms of callbacks),
its least late, a window of 1024 points, a refit each second, the slope
from 5 s of points on. `yau__refit_pts()` is the output's refit, shared;
only the points differ.

- From callbacks the point is (the frame after the block, the callback's
  entry). A block can reach the callback only after its last frame was
  recorded, so the entry is late and never early, as an output's
  callback is: the lower envelope fits it.
- From reports the point is (a position, its stamp). WASAPI's capture
  client has an `IAudioClock` too. Microsoft documents its position as
  "the sample that is currently being recorded".

What a capture report is on this laptop, from the scratch probe and
`audio_capstats` (shared mode, 10 s to 3 min):

| Quantity | Result |
|---|---|
| The position's step between blocks | 480 frames or 0: one engine pass each 10 ms |
| The stamp's step | every other pass: each 20 ms, at a pass where the position also moved |
| The stamp's age at the block | 10.2 to 21.3 ms |
| Position minus the frame after the block | 0 or 224 frames: miniaudio hands a 480-frame packet over as 256 + 224 (its stack buffer holds 256 frames of 4 channels) |
| Reports with a new stamp, against a least-squares line, 10 s | 79 us sd; +3.08 ppm |
| All reports, against the same line | not a line: a report between two stamps pairs a new position with the old stamp, 10 ms off |

So the header uses a report only when its stamp is new. Over 3 minutes
idle and 3 under load (8 spinning threads), no new-stamp report was more
than 1.2 ms off the final fit (p99 226 us idle, 149 us under load), with
no stall.

In exclusive mode (16 bits, 2 channels), the stamp moves with every
block, 10.0 to 10.2 ms old, and `GetStreamLatency` claims the period (10
ms at 480 frames, 3 ms at 144).

### Design: halts and losses

The first capture design looked for a loss as the position running ahead
of the frames handed over (backlog q), the way a WASAPI output's
position counts the silence of an exclusive-mode underrun. Measured, the
microphone does not do that. Stalls injected into the capture block, 60,
70 and 81 ms, against a 1056-frame (22 ms) buffer:

| What | Result |
|---|---|
| q after the stall | 800, 672 and 992 frames, back to a period's split within 3 blocks |
| The new-stamp reports after it | 47.9 to 48.1 ms late against the line before it, for good |

So the position stood for 48 ms while nothing was recorded, and the
frames on either side of that are 48 ms further apart than their numbers
say. In exclusive mode the position does count the lost frames: each 60
ms stall raised the backlog by 2400 frames for good. That is the output's HALT, seen from the input. The first build
without a rule for it set every later report aside (more than half a
period off the fit), the fit stopped at 8 or 9 points, and the capture
times were off by 0.35 to 3.7 s at the end of 40 s runs with stalls of
60 to 300 ms every 3 s.

The rules, in order of what they see:

- Three new-stamp reports in a row late by more than half a period or 5
  ms are a halt of the least delay but the first report's. The capture
  frames jump by it (a LOST gap) and the position is counted on by it.
  The first report can be the halt half over: after each 60 ms stall it
  was 38 ms late and the next ones 48, and a rule of two reports counted
  each stall as two halts (1828 and 480 frames). None was more than 1.2
  ms late without a stall here, against the 5 ms threshold.
- q at least a period over its floor in every block for 200 ms is a loss
  of the excess in whole periods: a device whose position counts what it
  could not deliver. Not seen here; the scripted input checks it.
- Without reports, blocks later than the fit by more than half a period
  in every block for 200 ms are a loss of the least of those delays. A
  backlog drains in a few blocks; a halt keeps every later block late.
- A device that flags a gap in its tick restarts the fit.

After every counted loss the fit starts over. A first build kept its
points and shifted the numbering instead; with a stall every 3 s, counts
off by up to 20 frames left steps in the window, and the fitted slope
came out at +68 ppm (+9 to +68 over six runs) against the device's +4.6.
The frames handed over between a loss and its finding keep their old
numbers. The record says from which frame: from the halt rule, three
stamps (40 to 60 ms); from the others, 200 ms.

### Design: the ring

One producer (the device thread), one or two readers, an index per
reader on its own cache line, acquire loads and release stores only. The
device writes a block only into slots that every reader has released;
when the ring is full it drops the new frames, not the old ones. Two
other designs were not taken:

- Overwrite the oldest and let a reader find out: a reader can copy a
  slot while the device writes it. Telling that from the index after the
  copy needs fences around plain memory accesses, which C99 does not
  have and ThreadSanitizer reports as races.
- A ring per reader: a second copy of every block on the device thread.

A dropped run is a gap: its entry (the ring position where writing
resumed, the frames missing, the kind) goes into a 64-entry queue before
the write index passes it, so a reader that sees the frames after it sees
the gap. The capture frame numbers count the missing frames, so the fit,
which is on device frames, is not touched by a drop, and a saved file
keeps its timeline with silence. A gap that finds the gap queue full
waits and grows; nothing is overwritten.

Cost of `yau_capture_block` per block (conversion, ring, fit), MinGW gcc
-O2, from block entry to its end: shared mode (4 channels, float, 224 or
256 frames) 1.5 us mean, 8.1 us p99 idle; 2.2 us mean, 11.4 us p99 under
load. In exclusive mode the measured span holds `GetPosition()`: 30 us
mean, 61 us p99 at 480 frames.

### Design: duplex

`yau_desc.capture` opens one miniaudio device of type duplex. On WASAPI
miniaudio runs both clients on one thread: it reads a capture packet
(waiting on the capture event), calls the data callback with the input
and an output block of the same size, then writes the output block
(waiting for room). The header's callback hands the input to the capture
handle, then renders. So the input paces the device thread, and the
output's queue gate (QUEUE), which waits on the output's event, is off:
`caps.queue` is 0. Each side keeps its own fit from its own client's
reports. On the laptop, the speakers (Realtek) and the microphones (SST)
are two endpoints.

### Design: the detectors

Both detectors run on frames and frame numbers, not times, so they are
deterministic: the same samples in any blocks give the same events
(checked with blocks of 1, 7, 256, 480 and 4096 frames). Times come after,
from the input's fit, at the interpolated frame.

The voice key's RMS is a running sum of squares in double over a ring of
the window's squares, summed again each time the ring wraps, so it does
not pick up rounding over hours. The onset is the frame where the RMS
crosses on_db, interpolated between the two frames around it. That
frame is late against the sound's true start by the time the RMS needs to
reach the level, which depends on the sound's rise and level. It is the
same rule as a hardware voice key with a threshold, and a fixed rule:
an analysis can model it. PTB's SimpleVoiceTriggerDemo takes the first
sample over a threshold, without RMS or hysteresis; it fires on a click.

The click finder has no window: the first frame over the level after
one under it, interpolated, then a dead time. On a ramp through the
level it returns the exact crossing (checked to 1e-6 frames).

### Measurements

Tool: `examples/audio/capstats.c` (`audio_capstats`), the input alone
unless the row says duplex. The main thread is reader 0 and reads every
10 ms. "Load" is 8 threads spinning at normal priority. "Lateness" is
each block's entry minus the final fit's time for the frame after it.
"Residual" is each new-stamp report's stamp minus the final fit's time
for its position. "Cost" is yau_capture_block from the block's entry to
its end, `GetPosition()` included. The guard's load is the CPU load when
the run started.

| Run | Blocks | Interval p50 / p99 / max ms | Lateness min / p50 / p99 / max ms | Residual p1 / p50 / p99 / max us | Fit spread us, drift ppm | Cost mean / p99 us | Losses |
|---|---|---|---|---|---|---|---|
| shared, idle, 180 s (load 5 %) | 36134 | 0.19 / 10.19 / 12.4 | 10.16 / 14.83 / 15.24 / 17.5 | -46 / 76 / 226 / 1180 | 33.6, +4.89 | 1.5 / 8.1 | 0 |
| shared, load, 180 s (3 %) | 36176 | 0.54 / 10.14 / 11.0 | 10.16 / 14.83 / 15.20 / 16.1 | -66 / 34 / 149 / 862 | 69.6, +4.38 | 2.2 / 11.4 | 0 |
| shared, idle, 120 s (10 %) | 24182 | 0.04 / 10.18 / 11.0 | 9.97 / 14.64 / 15.01 / 15.8 | -59 / 69 / 205 / 1052 | 41.4, +4.61 | | 0 |
| shared, period 96 asked, 30 s (0 %) | 15408 | 2.00 / 2.16 / 3.4 | 2.82 / 3.01 / 3.22 / 6.5 | -58 / 873 / 962 / 1514 | 171.9, +6.58 | 0.9 p50 / 6.7 | 0 |
| exclusive, 480 frames, 30 s (0 %) | 3104 | 10.00 / 10.20 / 10.6 | 9.96 / 10.16 / 10.29 / 10.8 | -42 / 109 / 231 / 707 | 40.3, +5.06 | 30 / 61 | 0 |
| exclusive, 480 frames, load, 30 s (4 %) | 3085 | 10.00 / 10.13 / 10.7 | 10.00 / 10.10 / 10.32 / 10.8 | -11 / 37 / 244 / 732 | 14.5, +4.90 | 38 / 73 | 0 |
| exclusive, 144 frames, 30 s (6 %) | 10275 | 3.07 / 3.96 / 5.3 | 3.05 / 3.84 / 4.27 / 5.6 | -15 / 200 / 934 / 1543 | 36.2, +5.20 | 37 p50 / 68 | 0 |
| duplex with the speakers, shared, 60 s (17 %) | 12174 | 0.11 / 10.18 / 10.6 | 20.82 / 25.48 / 25.86 / 26.5 | -75 / 104 / 187 / 566 | 52.6, +4.45 | 1.1 p50 / 9.4 | 0 |

- Shared mode: miniaudio hands each 480-frame packet over as 256 + 224
  frames (its 4096-byte stack buffer holds 256 frames of 4 float
  channels), so the intervals are 0.01 to 0.2 ms and 10 ms. The backlog
  (position minus the frame after the block) was 0 or 224 frames in every
  idle and load row.
- A 96-frame period, asked for in shared mode, was granted (a 210-frame
  buffer): blocks every 2 ms. Its reports scatter: new stamps every 4 ms,
  0.6 ms either side of a line (scratch analysis of the run's blocks),
  and the fit drifts +6.6 ppm against +4.4 to +5.2 in every 480-frame row.
  An earlier build that set aside reports more than half a period (1 ms)
  off the fit set aside 4987 of 7652 and fitted -22 ppm; hence the 5 ms
  floor.
- Exclusive mode gives 16 bits on 2 channels, a stamp with every block,
  and `GetStreamLatency` claims the period (10 ms at 480 frames, 3 ms at
  144). `GetPosition()` costs about 25 us there, against about 1 us in
  shared mode.
- Duplex: the output (Realtek speakers) and the input (SST microphones)
  are two endpoints; their fits gave +4.67 and +4.45 ppm in the same 60
  s. The output ran with no gate (`caps.queue` 0) and a lead of 33.6 ms,
  0 underruns. A block reaches the capture 20.8 ms or more after the
  input fit's time for its last frame, 10.6 ms more than without duplex:
  miniaudio's loop reads the input, then waits to write the output.
- Drift: the input and output fits of every run agree within the fits'
  own run-to-run spread (+4.4 to +5.2 ppm in runs of 30 s and more);
  whether the two endpoints share a crystal is not known from that.

Stalls injected into the capture block (`--stall-at 3 --stall MS
--stall-every 3`, 40 s, 13 stalls a run, shared mode):

| Stall | Losses found | Count per stall, frames (ms) |
|---|---|---|
| 10 ms | 0 | none lost |
| 20 ms | 13 of 13 | 383 to 392 (8.0 to 8.2) |
| 30 ms (*) | 13 of 13 | 864 to 906 (18.0 to 18.9) |
| 40 ms (*) | 13 of 13 | 1341 to 1349 (27.9 to 28.1) |
| 60 ms | 13 of 13 | 2303 to 2349 (48.0 to 48.9) |
| 150 ms | 13 of 13 | 6623 to 6667 (138.0 to 138.9) |
| 300 ms (*) | 13 of 13 | 13356 to 13833 (278 to 288) |
| 60 ms, exclusive (30 s) | 10 of 10 | 2400 each (50.0), by the backlog rule |

Each stall lost its length less about 12 ms; the input keeps about 12 ms
of slack, against a 22 ms buffer. One record per stall, no record for
10 ms. The final fit had no slope in these runs (a stall every 3 s
restarts it before 5 s of points), so its drift reads +0.00.

(*) The 30, 40 and 300 ms rows ran between 00:59 and 01:12 on 2026-10-13,
when another worker ran Firefox outside the measurement guard. They
count frames that the device lost during an injected stall, which the
stall's length sets; the 20, 60 and 150 ms rows of the same build in
that window agreed with the later undisturbed runs within 30 frames
(378 to 419, 2299 to 2342, 6620 to 6628 frames). Every other row of this
chapter ran outside that window, or ran again after it (exclusive, 144
frames: the same within the runs' spread).

Acoustic, speaker to microphone, through the air of one laptop chassis
(the distance between the speakers and the microphone array was not
measured). The output endpoint was unmuted at volume 0.40 for the run
and put back (muted, 0) after it; the input at 1.0 as found.
`audio_loopback_latency --unmute --volume 0.4 --clicks 10 --every-ms
500 --db -6 --burst-ms 20`:

| Output | Input | Found | Latency min / p50 / max ms, sd |
|---|---|---|---|
| shared | exclusive | 10 of 10 | 36.34 / 36.64 / 36.90, 0.18 |
| shared | shared | 0 of 10 | none: the input's peak during the bursts was -75 dBFS, under its -88 dBFS floor plus 25 dB |

The shared input does not pass the speakers' sound: its peak in the run
was the same as in silence. The shared endpoint's own processing (the
SST microphone array's effects, which give the -88 dBFS floor against
-70 in exclusive mode) removes it; which effect does was not checked.
So on this laptop an acoustic loop needs the exclusive input. The 36.6
ms is the whole loop: the output's queue and pipeline, the air (a few
centimeters: well under 1 ms), and the input's pipeline. It is not
split into output and input: no TTL into an input. Earlier attempts
with 12-frame clicks at -12 dBFS and volume 0.15 found no edge on any
path (input peaks under the edge level). Not run: the first sound after
a silence through the air (the run at volume 0.15 found no sound on the
shared input), exclusive output (its own underruns on this driver), and
duplex (shared both ways).

### Tests and mutations

`tests/adapt/audio_test_capture.h`, included by `audio_test.c`, drives
`yau_capture` through a scripted input on the virtual clock: device frame
w is recorded at T0 + w * kd at a set drift, a block is handed over Lin
frames after its last one, late by a seeded exponential, with a report
whose position moves every block and whose stamp changes every other
block, as measured. Every sample is a function of its device frame, so a
reader checks the frame of each one.

| Case | What it checks |
|---|---|
| Open | refusals (rate, channel count, resolution, a ring under 4 periods, readers), s16 widened exactly, `offset_ns` in every time, describe |
| Fit | reports and callbacks at -43, +100 and -100 ppm, 12 s: drift within 0.4 ppm (one seed, the output's bound), every frame of the last second within 1.9 us of its true time (callbacks: plus the constant Lin), `time_at` and `frame_at` against `time_of` |
| Overflow | a 0.1 s ring, two readers, nobody reads: 21 blocks dropped and counted, one DROP record, both readers resume on the right frame number, a read across the wrap stops at a gap |
| Losses | a stall the device rides out: no record. A loss the position counts: 2880 frames counted exactly, frames after it numbered right, times within 20 us. Without reports: counted 2881 of 2880, times within 1 us after. A 48 ms halt: counted 2308 of 2304 (the reports' own lateness), found 40 ms of frames later, times within 1.3 us after |
| Voice key | refusals; two speech-like words in -60 dBFS noise: onsets 6.4 and 16.0 ms into 10 and 25 ms attacks, equal to a brute-force reference to 1e-6 frames; the same events in blocks of 1, 7, 256, 480, 4096 frames; noise 10 dB under: nothing; a 2 ms click: rejected; a dip under on_db but over off_db for longer than hold_ns: no end; a frame gap: starts over; events past the caller's array counted |
| Edge | crossings at 1000.4 and 4999.3333 frames exactly; dead time; polarity; a held level is one edge |
| On the clock | four words through the scripted input at +61 ppm: each onset time equal to the true start plus the detector's delay within 2.2 us |
| WAV writer | float, 16 and 24 bits, 1, 2 and 4 channels, read back by the parser to the bit; rounding and clipping; the RF64 header past 4 GB; a capture saved across a gap has silence there |
| Threads | the device on a real thread, reader 1 on another, reader 0 here with stalls: about 45 % of the frames dropped, every sample read on its frame, read + dropped = made |

ThreadSanitizer (gcc 11.4, WSL2): 0 reports in 3 runs. With the device's
write-index store made plain: 3 reports per run; with reader 0's index
store made plain: 5 per run (3 runs each). `tests/mutate/audio.toml` has
22 capture mutants (cap-01 to cap-22; the rest of the list's anchors
checked again): 45 of 45 killed. The examples' `--sim` runs (CI) check
the loop's latency within 1 ms (measured 9.98 ms against 10.00), a 20 ms
gate in the sink (A - B 19.82 ms, 1.31 dB lower), and the voice RT of a
simulated participant within 8 ms (measured 3.4 ms over the loop).

Builds: MinGW-w64 gcc 16.1 (C11 test and examples with -Werror, the
compile check as C11 and C++17, `cmake --preset mingw` and ctest: 76 of
76), MSVC 19.44 (`msvc-full` and ctest: 88 of 88), gcc 11.4 in WSL2 (C99
-O3, C11 with ASan and UBSan, C11 with ThreadSanitizer). Not built here:
clang.

With the Web Audio path (v0.4.1) merged in: emcc 6.0.12 builds the core
test (capture cases included; it passes under node) and the header with
miniaudio's Web Audio backend, with and without the AudioWorklet
configuration, `-Werror`. That build found a WASAPI-only capture helper
unused off Windows (`-Wunused-function`); it is now compiled on WASAPI
builds only. In a browser an input has `YAU_TIER_WEB` (`test_capture_web_tier`,
mutant `cap-23`). No page has opened an input: web capture never ran
([web.md](web.md), "Not measured").

### The null device on a virtual clock (v0.6.0)

`YAU_BACKEND_NULL` was miniaudio's null device only: a timer thread on
real time, tier SIM. On a loaded CI runner its callbacks came late and
early, the fit of callback times spread by milliseconds (3.8 ms on a
macOS runner), and the `--sim` checks of `audio_schedule`,
`audio_av_sync`, `trial_audio_keyboard` and `trial_stop_signal` allowed 1
to 10 ms, each widened after a failure.

On ysp/rt.h's virtual clock (rt.h v0.9.0) `BACKEND_NULL` is now a device
of the header's own, "virtual null". Decisions:

- **A virtual timer per device, on the driver thread.** Each block is
  rendered when the frame thread's wait passes the block's time
  (`t0 + frames * 1e9 / rate`), with that time as `tick.t_entry`. The
  fit's points are then on one line: drift 0, spread 0. A real thread
  in lockstep with the clock would give the same times with a handshake
  per block.
- **The render runs on the frame thread.** The command and completion
  queues are single-producer single-consumer rings, and producer and
  consumer are then the same thread, at different times. Nothing in
  `yau_render()` waits, so it is safe inside a wait.
- **Its caps copy miniaudio's null device:** the project format, a
  period of `desc.period` (10 ms by default), a buffer of three periods,
  callback positions, tier SIM. One block lives in the handle's backend
  memory (8 KB), so the period is capped at 1024 stereo float frames;
  the caps say so. The capture side and DUPLEX get the same device,
  which hands the input silence.
- **A real backend is refused on the virtual clock**, with a message.
  Its callbacks come on real time, so their stamps would mean nothing.

Measured on 2026-10-14 (Windows 11 test laptop, MSVC 19.44 Release, and
MinGW-w64 gcc 16.1): every onset of the four examples is on its plan
within half a sample (at most 10.4 us at 48 kHz, where the plan falls
between two samples), and their checks now say so. `audio_schedule
--null`'s stop is on its plan within half a sample, and its four starts
are exactly 9600 frames apart. The same records in three idle runs and
two runs beside 24 CPU-bound processes.

Tests: `tests/adapt/audio_test_vnull.h` opens the null device on the
virtual clock, checks the caps, the exact fit and frame 0 at the open's
time, plays three clicks at times off the sample grid (each on its plan
within half a sample), repeats the run later on the clock and compares
it to the nanosecond from its own start, opens a virtual null input and
a DUPLEX pair (a 4-channel input: exactly 48000 frames in a virtual
second), and checks the refusal of a real backend for output and input.
Mutants vn-01 to vn-08 are caught (8 of 8).

## Not measured (capture)

- Capture times against a reference: no line-in cable and no TTL into an
  input, so every capture time is tier 3 and the input's share of a
  loop's latency is not known. The hand steps are in "How to measure the
  output-to-input latency".
- The first sound after a silence (rig_spec.md 13), by cable or through
  the air.
- Exclusive output in a loop; duplex in a loop (its input is shared, which
  does not hear the speakers here).
- Any capture backend but WASAPI; any input but the SST microphone array.
- A loss with a device that flags it (the YAU_TICK_XRUN path) on hardware.

## Exclusive start (v0.5.1)

The problem ("Through ysp/audio.h", above): on 2026-10-13
`audio_schedule --exclusive` gave LATE records at every lead and hundreds
of XRUN records in 4 of 5 runs, while `audio_clockstats --exclusive` gave
none. Conditions: the laptop of this note, 2026-10-13, AC power, the
speaker endpoint (Realtek behind Intel SST, 480-frame period unless a row
says 144), MinGW gcc 16.1 -O2, miniaudio 0.11.25, each run under the
measurement lock (load at the lock 0 to 20 %), runs of different builds
interleaved. Every run wrote silence except `audio_schedule`'s tones at
-40 dBFS, endpoint muted.

### Two states

Each exclusive open ends up in one of two states, and stays in it for the
whole run:

| | Good state | Bad state |
|---|---|---|
| Callbacks a second (100 needed at 480 frames) | 100 | 27 to 62 |
| Callback interval p50 / p99 | 0.24 to 0.38 ms / 75 to 76 ms | 12 to 32 ms / 76 to 80 ms |
| Frames written minus position, at callback entry | about 960 to 5460 | at most 955; the position runs past them |
| `GetPosition` p50 | 4.6 to 5.2 us | 84 to 92 us |
| XRUN records | 0 | 20 to 26 a second, from the first 0.2 s on |

In the bad state the driver takes one or two blocks per group of wakes,
plays silence while it waits for more, and counts the silence in its
position (in exclusive mode the position counts an underrun's silence:
"Exclusive mode", above). The records are real underruns, not false
positives of the detector: in two `audio_schedule` runs the position
ended 635,341 and 676,698 frames (13 and 14 s) past the frames written,
in 22.6 s. Each record is the rule "position at or past the frames
written"; none came from the halt or early-report rules. The position
itself stays a clean line (drift +2 to +5 ppm), so the fit plans on the
device's time and every onset that the starved queue cannot reach in time
is LATE, at any lead.

`audio_clockstats` and `audio_schedule` differ in nothing that matters
here: both open the same way, and both reach both states. The earlier
`audio_clockstats` runs were in the good state by chance.

### Bisect

`audio_clockstats --exclusive --seconds 15`, each version's header and
example, interleaved in one session, three runs each:

| Header | Commit | Opens in the bad state |
|---|---|---|
| v0.3.1 | d42cf92 | 2 of 3 |
| v0.4.0 | 7316129 | 1 of 3 |
| v0.4.1 | 3402098 | 2 of 3 |
| v0.5.0 | 24178ce | 1 of 3 |

Not a regression: every version tested reaches the bad state, and v0.3.1
has the exclusive setup of v0.3.0 (one period per buffer). Over all of
this day's runs, v0.5.0 opened in the bad state 19 times of 35 (12 to 30
s runs of both programs, at 480 and 144 frames). The rate moves between
sessions: 0 of 6 in one session, 5 of 8 in the next. So the v0.3.0 runs
that gave a 125 ms lead were good opens, not a different driver.

### The start decides

The two states differ from the first callbacks. miniaudio starts an
exclusive stream with an empty buffer, then renders and writes its first
block at once, then waits on the event:

- Good opens: the first `GetPosition` blocked for 40 to 47 ms and the
  second for 15 ms; then the driver pulled 10 to 12 blocks 0.1 ms apart
  and kept about 100 ms queued from then on.
- Bad opens: the first two callbacks came 0.14 ms apart, with no
  blocking; in one run the next wake came 16 ms later, and the queue
  never filled.

The raw probe (`tests/probe/wasapi_exclusive`), which writes one silent
buffer before Start and then waits for the event, was in the good state
in 6 of 6 runs; with `--no-prefill`, 1 of 8 runs was in the bad state
(the cave preset). Microsoft's exclusive-mode example also loads the
first buffer before Start.

### The fix

v0.5.1 writes one silent buffer (`AUDCLNT_BUFFERFLAGS_SILENT`, the whole
buffer) through miniaudio's `IAudioRenderClient` before
`ma_device_start()`. The device position counts those frames, so the
backend subtracts them: stream frame 0 is the first frame the header
renders, and a report before it is no report.

The first build of this showed a second fault. Its first 10 blocks had no
report (the prefill was still playing), and the core fed their callback
entry times to the fit, as it does on a device without reports. A
callback point is the output latency (about 100 ms here) before the
frame's play time, so the first fit was off, and the reports after it
looked late: a halt record (25.2 to 36.7 ms) at 0.62 to 0.89 s in 7 of 7
runs, while the position stayed within 0.02 ms of its line. Now a device
with position reports (`YAU_POS_DEVICE`) fits reports only; v0.4.1 made
the same rule for the browser's device alone. Unpatched good opens gave
a single halt record too, in 1 of 16 v0.5.0 runs (24.8 ms) and 1 of 4
older ones (33.7 ms); their first reports stood 4 to 104 ms early against
the final line while the driver started. Not traced further: with v0.5.1
the fit's first report comes after the prefill has played, and 15 of 15
runs gave no record.

### Measurements

Interleaved, patched and v0.5.0, in one session each:

| Program | v0.5.1 | v0.5.0 |
|---|---|---|
| `audio_clockstats --exclusive --seconds 12`, 8 runs each | 8 good, 0 XRUN records | 5 bad (252 to 305 records), 3 good |
| `audio_schedule --exclusive`, 4 runs each | 4 good, 0 XRUN records; shortest safe lead 125 ms in each; 14 residuals -12.8 to +10.0 us | 4 bad: 13 to 23 records before the first sound, 430 to 467 by the end; LATE at every lead |
| `audio_clockstats --exclusive --period 144 --seconds 12`, 3 runs each | 3 good, 0 XRUN records | 3 bad (294 to 338 records) |

The patched good state is the old good state: `GetPosition` p50 2.5 to
5.7 us (one run 41.9 us), stream frames written minus position 3779 to
4575 p50 (the device holds the 480-frame prefill on top). Shared mode
with the patch: `audio_schedule` shortest safe lead 40 ms, 14 residuals
-31.7 to +10.0 us, 0 XRUN records; `audio_clockstats` 20 s, 0 records.

The lead stays 125 ms: the bursts and the 100 ms the driver holds are the
driver's event-driven path ("Exclusive mode compared with cave-audio").
Exclusive mode stays opt-in, tier 3, and on this laptop slower than
shared mode.

### Tests

`test_report_less_start` in `tests/adapt/audio_test.c`: a scripted device
with reports whose first 10 blocks carry none, 4000 frames from render to
output. No XRUN record, and a sound lands on the truth's frame, tier 2,
confirmed. With the old rule it fails (one record). Mutant `start-01`
puts the old rule back and is killed. `tests/compile/audio_com.cpp`
checks the two `IAudioRenderClient` slots against the SDK. The prefill
itself runs only on hardware: to check a machine, open exclusive mode
several times, because one open shows one state.
`audio_clockstats --exclusive --seconds 12`, 8 times in a row, must give
0 XRUN records in every run.

## Web envelope (v0.5.2)

The problem: the sound demo (`av.html`) on the ScriptProcessorNode path
sometimes opened with no sound. A session before this one saw it in 2 of
6 Firefox runs and 1 Chrome run. Conditions below: the laptop of this
note, 2026-10-14, Windows 11, Realtek speakers, Firefox 157.0.1 and
Chrome 155, both headless unless a row says headed, emsdk 6.0.12,
`av.html --n 4` with the 2048-frame period the page asks for, each batch
under the measurement lock (load at the lock 0 to 19 %).

### How the opens were counted

The harness that is now `tests/probe/web_audio` opens the page as a
visitor does: a new browser and profile for each open, a trusted click on
Start, no autoplay flag or preference. `probe.js` wraps `AudioContext` and taps each
connection to the destination with an `AnalyserNode`, read every 100 ms.
An open is silent when `yau_open()` failed or when no context the program
kept carried a sample above 0.005 (the click peaks at 0.050). The probe
also records each context's states, its `resume()` and `suspend()` calls
with the page's user activation, and its callbacks.

### The cause

Every silent open had the same record: the first `yau_open()`, inside
the Start click, failed with `gave no steady callbacks within 3 s
(fit points=1 to 3)`, and the program ran its flashes with no sound.
The context was not the problem: it was allowed to start, it ran (0.44 to
0.74 s after `new AudioContext()`, after miniaudio's `suspend()` and
`resume()`), and it made 54 to 62 callbacks before the open gave up.

The reports were the problem. A debug build logged each block's report
and each read of `getOutputTimestamp()`. In Firefox, `contextTime` steps
with the browser's audio callback (8, 10.7 and 21.3 ms steps headless),
and `performanceTime` is the read's time, so a pair's offset
(`performanceTime` less `contextTime`) is a sawtooth 12 ms wide. v0.4.1
kept the lower envelope in the block's C code, over the pairs the blocks
saw. A ScriptProcessorNode block sees the pair that its own handler read
just before it, at a phase locked to the browser's audio callback. When
one lucky pair set the envelope low, every block's pair was above it by
more than 0.5 ms until that pair left the last 32 blocks, 1.4 s at 43 ms
a block. In 3 failing opens with the debug build, the reports stood still
for 0.4 to 0.7 s, moved, then stood still from 0.9 to 1.2 s after the
first block to the end of the open. The fit got 1 to 3 points and no
500 ms span. On the AudioWorklet path a block runs every 2.7 ms
and sees every pair, so the envelope held there.

### The fix

The reader keeps the envelope, over every read: the 10 ms timer's and the
ScriptProcessorNode handler's. It publishes only reads on the envelope
(at most 0.5 ms above the least late of its last 32 reads, about 0.3 s),
and the block takes each new published pair. The rule is the one v0.4.1
measured; only the samples it sees changed. The describe line's
`passed` count is now the reader's.

### One context

`av_sync.c` opens the sound in the Start tap, then closes it and opens it
again after the screen's setup (a stall of seconds with a
ScriptProcessorNode running left its timing wrong). Before this change
the second open made a new AudioContext, about 2 s after the tap.
Chrome and Firefox let it start (the page had a user activation once),
but WebKit starts audio only from a gesture, so on iOS Safari it may wait
suspended, and the open fails.

The web device now keeps the page's AudioContext on the ScriptProcessorNode
path. Around miniaudio's device init it puts a wrapper in place of
`AudioContext`: the first context made is the kept one when it is free and
at the asked rate, else a new one that is kept. miniaudio's sample-rate
queries in the same init get the same context, and the kept context's
`close()` does nothing. `yau_close()` suspends it (miniaudio's stop), the
next `yau_open()` resumes it with a new ScriptProcessorNode. No API
changed; a program that reopens needs no change. A second output open at
the same time, another rate, a DUPLEX device and the AudioWorklet path
(whose worklet module a context registers once) made their own context.
v0.6.1 brings the AudioWorklet path in: see
[One context on the AudioWorklet path](#one-context-on-the-audioworklet-path-v061).

### The warm-up limit

`yau_open()` waited at most 3 s from the device's start for the fit to
span 0.5 s. A browser's context runs some time after it is made (0.23 to
0.72 s in Chrome 155, 0.40 to 0.79 s in Firefox 157 on the
ScriptProcessorNode path, measured by the probe), and that time came out
of the 3 s. Now the 3 s run from the first callback the frame thread
sees, and the whole wait is at most 5 s from the start. The cap lets the
first callback come 2 s late, 2.5 times the latest measured, with the
full 3 s after it; a device that never calls back fails after 5 s, not 3.
The error says which: no callback, or no steady callbacks.

### Measurements

The v0.5.1 and v0.5.2 builds of the same `av_sync.c`. Headless rows: one
batch of 30 per cell. Headed rows: the two builds interleaved, 15 each.

| Opens with no sound | v0.5.1 | v0.5.2 |
|---|---|---|
| Firefox, ScriptProcessorNode, headless | 8 of 30 | 0 of 30 |
| Firefox, ScriptProcessorNode, headed | 3 of 15 | 0 of 15 |
| Firefox, AudioWorklet, headless | 0 of 30 | 0 of 30 |
| Chrome, ScriptProcessorNode, headless | 0 of 30 | 0 of 30 |
| Chrome, ScriptProcessorNode, headed | 0 of 15 | 0 of 15 |
| Chrome, AudioWorklet, headless | 0 of 30 | 0 of 30 |

A debug build of each timed the first `yau_open()` in Firefox
(ScriptProcessorNode, headless, 10 opens each, interleaved): 2.0 to 3.0 s
with v0.5.1, 1.4 to 2.3 s with v0.5.2, of which the context's start took
0.42 to 0.75 s. The open's 3 s limit runs from the device's start, after
the context is made, so v0.5.2 leaves about 0.7 s of it. With sound, the
other figures did not move: blocks with a report per run 173 to 229
(v0.5.1) and 172 to 198 (v0.5.2) on the ScriptProcessorNode path, 0 XRUN
records there; each trial's sound within -0.74 to +0.82 ms of its plan in
the headed Firefox runs of both builds.

The Chrome failure of the earlier session did not come back in 75 Chrome
opens of v0.5.1. In Chrome a pair spreads 0.15 ms, so the 0.5 ms rule
passed nearly every block's pair; a single pair far below the line, as
headless Chrome once gave (34 ms off, [web.md](web.md)), would have held
v0.5.1's reports for 32 blocks (1.4 s), and holds v0.5.2's for 32 reads
(0.3 s). That is a reading of the rule, not a measurement.

With one context and the warm-up limit (the final v0.5.2), headless, the
same conditions, one batch per cell, `tests/probe/web_audio/run.sh`. A
driver failure (a timeout or a detached frame before the page gave a
result; 3 in 128 runs) is not an open, so two cells got extra opens:

| | Opens | Silent | AudioContexts made per open | Sound after the reopen |
|---|---|---|---|---|
| Firefox, ScriptProcessorNode | 30 | 0 | 1 | 30 of 30 |
| Firefox, AudioWorklet | 31 | 0 | 3 (the device's, 2 rate queries) | no reopen |
| Chrome, ScriptProcessorNode | 31 | 0 | 1 | 31 of 31 |
| Chrome, AudioWorklet | 30 | 0 | 3 | no reopen |

On the ScriptProcessorNode path the reopen's `resume()` came 4.1 to 9.7 s
after the tap and ran the kept context in both browsers. Firefox reported
no transient activation at that call in all 30 opens; headless Chrome
under the driver reported one even 9.7 s after the tap, so Chrome's runs
do not show the case without one. 0 XRUN records on that path.

In the headless Firefox ScriptProcessorNode runs with sound, every
trial's flash record was 16.7 or 33.3 ms (one or two frames) before its
plan while the sound's was on it, in both builds. That is the headless
screen's record, not the sound's, and it is not a timing figure.

### Tests

The envelope is JavaScript in the device; it runs only in a browser, so
no native test or mutant reaches it. `tests/probe/web_audio/run.sh` is
the check: 30 opens per browser and path, 0 silent.

## One context on the AudioWorklet path (v0.6.1)

The problem: iOS Safari starts audio only in a context made or resumed
inside a user gesture, and v0.6.0 kept the page's AudioContext only on the
ScriptProcessorNode path. On the AudioWorklet path each `yau_open()` made
new contexts, so a reopen after the Start tap (`av_sync.c` closes and
opens its sound after the screen's setup) asked for audio from a context
made outside the gesture. Conditions below: the laptop of this note,
2026-10-21, Windows 11, Realtek speakers, Chrome 155 and Firefox 158,
headless unless a row says headed, emsdk 6.0.12, miniaudio 0.11.25,
`av.html --n 4`, `tests/probe/web_audio/run.sh` under the measurement
lock.

### Where the 3 contexts came from

`probe.js` now records the first stack frames at each
`new AudioContext()`. A build with `--profiling-funcs` names the C
functions. One AudioWorklet open, in both browsers, made:

| Context | Made by | Life |
|---|---|---|
| 1 | Emscripten's `emscripten_create_audio_context()` (`libwebaudio.js`), from miniaudio's `ma_device_init__webaudio()` | the device's; suspended, never closed, at `yau_close()` (`emscripten_destroy_audio_context()` only suspends) |
| 2 | miniaudio's `ma_context_get_device_info__webaudio()`, an `EM_ASM` that makes a context to read `sampleRate` and closes it, called through `ma_device_get_info()` for the device name in `ma_device_init()` | closed at once |
| 3 | the same function through `ma_device_get_name()`, from the "Log device information" block at the end of `ma_device_init()`; the log level does not gate the call | closed at once |

None came from `ysp/audio.h`'s own code, the page glue or the probe.
v0.5.2's wrapper had given 2 and 3 the device's context on the
ScriptProcessorNode path only, so that path made 1. Each reopen made 3
more: 6 a page with `av_sync.c`'s reopen.

### What blocked one context

The wrapper alone does not do: a context's worklet scope boots once.
miniaudio's init calls `emscripten_start_wasm_audio_worklet_thread_async()`
at each open, which adds Emscripten's module to the context's
`audioWorklet`, posts a boot message that instantiates the Wasm module in
the scope, and registers the processor `miniaudio`. On a context that
booted, the second boot would instantiate the module again in the same
scope, and `registerProcessor()` throws for a name the scope has.

A node per open does not do either. An `AudioWorkletNode` whose
`process()` returns true stays alive after `disconnect()`
([MDN, return value](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorkletProcessor/process#return_value)).
Measured with a page that counts a processor's calls: after
`disconnect()` it was called 379 times in 1 s in Chrome 155 and 383 in
Firefox 158 (48 kHz, 375 render quanta a second). Emscripten's processor
returns what miniaudio's callback returns, always true. Its processor holds the device pointer of its
open. After a close and a reopen of the same handle the pointer is the
new device's, so 2 processors would call miniaudio's callback for one
device, and the stream would advance twice a quantum. On a new handle
the old processor would call a device that is gone.

Three options were weighed. Keeping miniaudio's device across opens
needs the device outside the handle, and every use of it in the backend
changed. Owning the worklet setup in the header means its own copy of
miniaudio's backend. Both are larger than the third, which was chosen:
`yau__web_hold()` wraps two constructors during the device init. The
reasoning, not a measurement, decided it.

### The fix

- The `AudioContext` wrapper (v0.5.2) now runs on both paths. On a kept
  context that booted, it puts a stand-in `audioWorklet` on the context
  object: `addModule()` resolves, the boot message is dropped, and the
  processor's registration is answered as the scope answers it (a
  `_wsc` message with success). Emscripten's own message names are
  read from emsdk 6.0.12's `libwebaudio.js` and `audio_worklet.js`.
- An `AudioWorkletNode` wrapper gives the kept context one node for the
  page's life. The node calls a trampoline in the header with a slot
  that holds miniaudio's callback and the open device. `yau_close()`
  sets the device to 0 and waits out a call in flight; between opens the
  node writes silence. A reopen gets the same node, connected again.
- miniaudio's 2 sample-rate queries get the device's context, or, when
  that context is not kept, a stand-in with its `sampleRate`. Every
  output open makes 1 context or none.
- A context is reused at the same rate and, on the AudioWorklet path,
  the same channel count (the node's output count is fixed). Another
  rate or count makes a new kept context, and the old one is closed.
  A second output open at the same time, a DUPLEX device and a capture
  make their own context, as before (`context=own` on the describe
  line, which now says `kept`, `reused` or `own`).

miniaudio 0.11.25 never stores the worklet stack it allocates
(`pDevice->webaudio.pStackBuffer` stays 0, so `ma_device_uninit()`
frees nothing): 128 KB leak at each open, before and after this change.
The kept scope runs on its first open's stack, so the header clears that
field before the uninit: a later miniaudio that frees it must not free
the kept one.

### Measurements

The v0.6.0 and v0.6.1 headers with the same `av_sync.c`, which now
reopens on both builds. One batch of 30 per cell.

| | Opens | Silent | Contexts made a page, v0.6.0 | v0.6.1 | Sound after the reopen, v0.6.1 |
|---|---|---|---|---|---|
| Chrome, AudioWorklet | 30, 30 | 0, 0 | 6 (2 used) | 1 | 30 of 30 |
| Chrome, ScriptProcessorNode | 30, 30 | 0, 0 | 1 | 1 | 30 of 30 |
| Firefox, AudioWorklet | 30, 30 | 0, 0 | 6 (2 used) | 1 | 30 of 30 |
| Firefox, ScriptProcessorNode | 30, 30 | 0, 0 | 1 | 1 | 30 of 30 |

Every v0.6.1 open's describe line said `context=reused`. The reopen's
`resume()` came 3.1 to 5.2 s (Chrome) and 5.1 to 6.2 s (Firefox) after
the tap. Firefox reported no transient user activation at that call in
60 of 60 opens; headless Chrome under the driver reported one in all,
as in v0.5.2, so Chrome's runs do not show the case without one. v0.6.0
also played after its reopen in Chrome and Firefox: these browsers start
a context made after the tap once the page had a user activation. The
case this fixes is WebKit's, which no run here reached.

Sound minus plan (the browser's estimates, 120 trials a cell), v0.6.0
against v0.6.1, headless:

| | v0.6.0 | v0.6.1 |
|---|---|---|
| Chrome, AudioWorklet | -0.010 to +0.051 ms | -0.363 to +0.015 ms |
| Chrome, ScriptProcessorNode | -0.010 to +0.033 ms | -0.019 to +0.520 ms |
| Firefox, AudioWorklet | -1.72 to +1.61 ms, p5 to p95 -0.66 to +0.28 | -1.87 to +2.12 ms, p5 to p95 -0.49 to +0.65 |
| Firefox, ScriptProcessorNode | -1.06 to +0.72 ms, p5 to p95 -0.77 to +0.40 | -1.21 to +1.17 ms, p5 to p95 -0.55 to +0.66 |

Headed Firefox, the two builds alternated in batches of 5 opens:

| | v0.6.0 | v0.6.1 |
|---|---|---|
| AudioWorklet, 30 opens each | -1.30 to +1.71 ms, p5 to p95 -0.79 to +0.54, 11 of 120 beyond 0.8 ms | -1.67 to +2.07 ms, p5 to p95 -0.75 to +0.54, 11 of 120 |
| ScriptProcessorNode, 15 opens each | -0.78 to +1.02 ms, 4 of 60 beyond 0.8 ms | -0.75 to +1.44 ms, 1 of 60 |

No difference that these counts can show. The v0.5.2 note's -0.74 to
+0.82 ms was 15 headed Firefox 157 ScriptProcessorNode opens; Firefox 158
here gives a few trials beyond 0.8 ms in both versions, on both paths.
0 silent opens in the headed runs; 1 context a page with v0.6.1, 6 with
v0.6.0 on the AudioWorklet path. XRUN records (`yau_caps.xruns`), over
every run above: 0 to 9 an open on the AudioWorklet path with v0.6.0, 0
to 4 with v0.6.1, 0 on the ScriptProcessorNode path.

### Tests

The wrappers and the trampoline run only in a browser: no native test or
mutant reaches them. CI compiles the header with
`-DMA_ENABLE_AUDIO_WORKLETS -pthread` as C and C++.
`tests/probe/web_audio/run.sh` is the check: 30 opens per browser and
path, 0 silent, 1 context made a page (`summary.py`'s last line;
`--from` lists where the contexts were made).

## Not measured

- Latency to sound by cable (an acoustic loop is in "Capture"). No
  loopback cable; `--line` is built, not run. It
  reports the measured latency minus the OS's claim when it does.
  `--line --stream` (stream placement and latency) is built, not run.
- CoreAudio, ALSA, PulseAudio: compiled, never run. PulseAudio
  under WSLg plays to the Windows speakers and was not run.
- Web Audio: run in Chrome 154 (and Firefox, [web.md](web.md)) on this
  laptop; sound at the speaker never measured, so the browser's
  `outputLatency` in every web onset is unchecked.
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
