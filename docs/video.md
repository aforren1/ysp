# ysp/video.h design notes

This note explains why `ysp/video.h` has its current form and records what
was measured. The manual in the header tells you how to use it. The plan
that asked for the header is section 4.6 of [rig_spec.md](rig_spec.md).

All numbers come from one machine: a Windows 11 25H2 laptop, i7-1360P, on
AC power, Intel Iris Xe, the 1920 x 1200 panel at 60.0008 Hz
(`\\.\DISPLAY1`), ANGLE 2.1.23876 from Docker Desktop. Window runs were
640 x 360, composed (tier 2), with low-contrast content. Timing runs held
the shared measurement lock. No photodiode was used, so no number here is
a measurement of light.

## Running status

This section is the running record. Update it when an item changes, so
that a restart loses nothing.

| Item | State |
|---|---|
| Scheduler, records, controls (play, pause, seek, loop, end, manual) | Done, tested on a virtual clock |
| Movie clock from a ysp_timeline base; own anchor without one | Done, tested (agreement under cadence and noise) |
| Follow another clock (`yvid_follow`, `yvid_follow_audio`) | Done, tested with a scripted 200 ppm clock and with the soundtrack on ysp_audio's scripted device; one run on the real device ("Soundtrack") |
| Decoder interface, frame sequence (read, write, raw, QOI), pl_mpeg | Done, tested; pl_mpeg run on generated clips |
| Decode-ahead on a pump, inline mode | Done, tested (one threaded case) |
| Upload through ysp/gfx.h | Done, measured; YUV as planes since round 2 |
| Mutations | v0.1: 23 of 23 caught; round 2: 29 of 31 ("Builds and mutations") |
| Build matrix | MSVC, MinGW, gcc (ASan, UBSan, TSan): pass; v0.2.0 on 2026-10-06: emcc 6.0.10, gcc 13.3 (WSL2, ASan and UBSan), clang 18.1, MinGW gcc 16.2: pass after a test fix (see "Builds and mutations") |
| Decode thread that cannot start (wasm without -pthread) | Falls back to inline decode; describe() says so |
| Follow-up (2026-10-05) | ysp/timeline.h v0.3.0: `ytl_lead()` replaces the read of `tl->lead`; seeks and manual jumps use `ytl_skip()`, and the SEEK record counts the skipped annotations. ysp/screen.h: `yscr_frame.done` completes every flip record. See "Seeks and annotations" and "Records" |
| Round 2 (2026-10-06): Media Foundation | Done (v0.2.0): `YVID_BACKEND_MF`, the MP4 index maker (with the H.264 SPS's color), `desc.hw_decode` (AUTO is DXVA, by M2). Tested on generated clips (`tests/media/make_video_clips.sh`, the core test with `YVID_TEST_MEDIA`); 9 of 11 mutations caught. M1, M3, M4 measured. See "Media Foundation" |
| Round 2: base rates (item R) | Done, tested (rates 1/2, 1001/1000, 2/1, a change mid-play, loop, pause, strict_cadence); 9 of 9 mutations caught; rate 1 byte-identical to v0.1.0. See "Base rates" |
| Round 2: soundtrack | Done on ysp/audio.h v0.2.0 (`yau_wav` on a stream); tested on ysp_audio's scripted device; 10 of 10 mutations caught; on the real device the flips stay within -0.42 to +0.08 ms (p1 to p99) of the audio clock, after a fix the run found (the shared start put on a display onset). See "Soundtrack" |
| Round 2: planar upload | Done, measured (M1, M2): kept; the RGBA8 conversion left the play path. See "Upload and conversion" and M2 |
| Round 2: GPU path | `YVID_PATH_GPU` done; on ysp/screen.h's `desc.d3d11_video` (in the tree since its v0.3.1 checkpoint). `examples/video/check.c` (against UPLOAD on hardware: the same value at every pixel) and `video_play --gpu` landed; measured (M7). An option: UPLOAD stays the default. See "GPU path" |
| Round 2: fixes | A data race on the free queue after a PENDING decode, found with a new threaded case under ThreadSanitizer; fixed ("Builds and mutations") |
| Round 2: missed bars | DXVA's cold open at 1080p60 (259 to 308 ms, bar 300 ms; M3); the software decoder's seek (M4); 2 Media Foundation mutations not caught (why, in "Builds and mutations") |
| Round 2: not built | `YVID_PATH_SHARED`; HEVC's VUI |
| v0.2.1 (2026-10-08): recovery after a decoder outage | Fixed: after `YVID_PENDING` the decode thread slept until the next seek. Tested (a deterministic threaded case and the real-time one); 2 of 2 mutations caught. See "Recovery after a decoder outage" |
| Not in this task | Capture, FFmpeg, AVFoundation |

## Decisions that changed the design

The design (2026-10-05, probe and plan) was reviewed. These changes are
the coordinator's and the user's:

1. **Judder is allowed by default**, as in Psychtoolbox's default movie
   mode. The frame due at each onset is shown; repeats and drops follow
   the cadence. A showing beyond floor(R/r) is REPEATED with reason
   CADENCE, and a frame the refresh cannot show is DROPPED with reason
   CADENCE. There is no tier penalty. `desc.strict_cadence` restores the
   refusal.
2. **One lead for video and annotations.** The due rule is
   ysp/timeline.h's QUANTIZATION rule with the movie base's lead. With a
   timeline, its lead is used and a different `desc.lead` is refused.
3. **The movie clock is read from the timeline**, not written into it.
   With `desc.timeline` and `desc.base`, `movie_time(onset)` is
   `ytl_base_time()`. play, pause, seek and loop anchor, pause and
   rewind the base. So a later ysp/timeline.h with an exact base rate
   gives slow motion with no change here. Rate is 1 in v0.1; round 2
   needed one change, the lead in RT ns ("Base rates").
4. The design's other recommendations stand: no second display-locked
   clock, 10-bit refused, the timestamp checked on every frame and the
   hash on CPU paths, pl_mpeg fetched and pinned, QOI in the header,
   manual mode for frame-index control.

## The due rule and frame times

Frame i covers movie time [t(i), t(i+1)) with
t(i) = ceil(i x den x 1e9 / num). The ceiling makes `t(i) <= x` for an
integer x the same as the exact rational comparison. So "the largest i
with t(i) <= m + L" is exact, and an annotation stored at t(i) on the
movie base lands on the same display frame as frame i (the test checks
this under a 3:2 cadence and 20 us of onset noise). The integer
arithmetic is split so that rates with num x den x 1e9 < 2^63 never
overflow; open refuses others.

Under 2 us of onset noise, 30 fps on 60 Hz, lead 0.5 placed every frame;
the never-early rule (`YVID_LEAD_NONE`) misplaced 26 to 39 of 700 display
frames (core test).

## Cadence and slips

At open, R/r is compared with integers. Within 200 ppm of k, each frame is
due for k display frames and repeats are DUE. Otherwise the describe line
gives the cadence and the durations, for example "cadence 3:2, frames on
screen 33.3 to 50.0 ms" for 24000/1001 on 60 Hz.

A slip is the movie clock and the display grid running at different
rates. To find slips, each decision is compared with a nominal schedule:
the due frame if the clock ran in step with the vblank count at the
nominal period (T / k at a multiple, the mode's period otherwise) since
the last anchor or slip. A difference is a slip (DRIFT), and the schedule
restarts from the clock's phase at that frame.

The first version tracked the offset between the due and the nominal
frame and called a change in it a slip, without restarting. The test
showed it wrong: after the first slip the phase of the frames against the
nominal grid is a fraction of a frame, so the offset alternates 0, 1,
0, 1 and every frame looked like a slip. Restarting at each slip counts
one record per slip, which the test checks against an exact model of the
clock and the grid (487.3 ppm: 4 slips in 9000 display frames, the first
at frame 1027 or 1029 against 1026 predicted, both signs).

An onset within a few ns of a frame boundary is a tie, where rounding or
noise decides; DRIFT is then the right reason. A rational rate on an
exact grid makes ties periodic (24000/1001 on exactly 60 Hz: every 1001
display frames; 500 ppm at 30 fps on 60 Hz: at every slip). The tests
avoid such grids by 10 ppb; a real display is never that exact.

## Decisions

j is the frame on screen, i the due frame, e the nominal due frame.

| Case | Decision | Why |
|---|---|---|
| i = j = e | REPEATED j | DUE at a multiple; CADENCE beyond floor(R/r) |
| i = j, i != e | REPEATED j | DRIFT |
| i > j, i ready | SHOWN i | DUE; DRIFT when the slip has no drop; SEEK, LOOP, MANUAL after those |
| i > j, i not ready, a frame in (j, i) ready | SHOWN that frame | DECODE_LATE |
| i > j, nothing ready | REPEATED j | DECODE_LATE |
| frames j+1 .. shown-1 | DROPPED | DECODE_LATE for those due before they were ready; DISPLAY_LATE when the vblank count jumped; DRIFT for a slip; CADENCE when the nominal schedule drops them too |

The decoder learns the due frame before it decodes, so a random-access
decoder skips a stalled frame as soon as it is older than due (one
DECODE_LATE repeat whatever the stall), and a GOP decoder waits for its
keyframe, then seeks to the next keyframe when that is shorter (core test:
stalls of 1, 3 and 20 frames at GOP 1 and 10).

A decoder failure ends the movie only after the frames decoded before it
are shown: they are good frames.

### Recovery after a decoder outage

The question (2026-10-08): in the threaded test, a scripted decoder at
250 frames per second on a 250 Hz display was `YVID_PENDING` for 300 ms
from frame 40, and no frame was shown between the end of the stall and a
seek at display frame 200, on Windows and on CI's macOS runner.

The cause was not the drop rule. The decode thread runs as the idle
callback of a ysp_rt pump. When the callback says there is no more to do,
the pump blocks until a message arrives. `yvid_update()` sends one only
when the decode thread marked itself idle, and before v0.2.1 it did that
only when it had no free slot. After `YVID_PENDING` it returned without
the mark, so nothing woke it until the seek posted a message. The
decoder's contract says "call again later"; the core did not. The
built-in backends (Media Foundation, pl_mpeg, the frame sequence) never
return `YVID_PENDING`, so only a custom decoder met this. In inline mode
`yvid_update()` steps the decoder on every display frame, so the
virtual-clock stall tests passed.

The fix: after `YVID_PENDING`, the decode thread marks itself idle, and
the next `yvid_update()` wakes it. The decoder is asked again once a
display frame.

The catch-up rule was already there, and it is unchanged:

- The frame thread publishes the due frame before it takes a frame. The
  decode thread decodes toward the larger of its next frame and the due
  frame. When the due frame is past the next keyframe, it seeks to the due
  frame's keyframe and discards forward from there. Thus it catches up as
  fast as the decoder can seek and discard, not one frame per display
  frame.
- The frame thread takes the newest ready frame that is not later than
  the due frame, and frees the older ones. Any number of frames can drop
  on one display frame; one DROP record holds the run.
- The frames passed over are DROPPED with reason DECODE_LATE (they were
  due before they were ready), and the display frames without a new frame
  are REPEATED with DECODE_LATE. A DUE reason would say that the frames
  were dropped on schedule, which they were not, so no DUE drop was added.
  No per-display bound on the drops is needed: the drops cost the frame
  thread nothing but one record.

Psychtoolbox does the same in kind: `Screen('PlayMovie')` "will return the
number of frames that needed to be dropped in order to keep video playback
in sync with realtime and audio playback". ysp_video gives each run its
display frame and reason as well.

Measured after the fix (Windows, MSVC and MinGW gcc; WSL gcc and
ThreadSanitizer):

| Case | Before | After |
|---|---|---|
| Deterministic: outage from frame 40 until display frame 120, the frame thread waiting for the decode thread to settle after each update | no frame after display frame 40; the wait for the decode thread timed out (2 s) | the due frame (121) on display frame 121; 81 frames DROPPED with DECODE_LATE, 81 display frames REPEATED; a new frame on every display frame after |
| Real time: outage of 300 ms from frame 40, then a seek at display frame 200 | after the stall 0 frames; shown 239 of 400 | the first frame 1 or 2 display frames after the stall; after the stall 100; shown 359 to 368 of 400 |

The real-time case checks the recovery against a loose bound, 50 display
frames, for a loaded CI runner. The deterministic case checks it exactly.

## Pause, resume and seek

A pause freezes the movie time at the onset of the display frame nearest
the requested time. The frozen frame stays on screen. A resume starts one
display period after the frozen time. The first version resumed at the
frozen time, which showed that movie time on two display frames, with the
pause between them (core test, and a mutation).

A seek posts the target to the decode thread with a new epoch. Slots of an
older epoch are freed at the head of the ready queue on every update; the
first version did not, so after a seek every slot held a stale frame and
the decoder had none free (core test).

## Records

One FRAME record per display frame while playing, pushed when its flip
record arrives. `yvid_update()` completes the records of every flip in
`yscr_frame.done` (ysp/screen.h lists each record completed since the
last begin). A frame filled by hand with only `last` still works: a
pending record older than `last` is then completed as ESTIMATED.
`yvid_flip_done()` from `yscr_on_flip()` gives every flip too. The
core test holds one flip record back a frame and delivers it in `done`
ahead of the newer one: the frame keeps its own onset and is not
ESTIMATED (a mutation that reads only `last` fails it). The record's
word 9 is the display frame index, the word YSCR_EV_FLIP carries.

## Seeks and annotations

A seek anchors the movie base with `ytl_skip()`: the annotations it
passes over are marked SKIPPED and never fire, and the one at the target
fires on the frame that shows it. The SEEK record's word 9 counts the
skipped annotations, and `info.skipped` sums them. Before ysp/timeline.h
v0.3.0 a forward seek fired every annotation it passed over, late, on one
display frame. In manual mode a jump skips the same way; a step to the
next frame fires the annotations up to that frame's time, as playing
would. Core test: a seek from frame 25 to 133 skips 108
annotations (the record equals the count of SKIPPED events) and no
annotation fires more than a period from its time; in manual mode the
jump from frame 2 to 150 skips the 147 between them and fires only 150's.
A loop wrap still uses an anchor: it rewinds, so the annotations fire
again in the next cycle.

The timeline's lead comes from `ytl_lead()`.

A base rate other than 1 needed the due frame from the base time at RT
onset + L, not the base time at the onset plus L: L is RT ns. Round 2
does that with `ytl_window()` ("Base rates").

## Frame sequence and the index

The frame sequence is as in the design (section 3.3), with XXH64 in place
of CRC-32 for the header, the frames and the index. One hash function
serves the container, the .yspvi index and the run-time check. XXH64
ran at 4.3 to 5.3 GB/s here (table below), so a 1080p I420 frame cost
0.6 to 0.7 ms on the decode thread. v0.2.2 changed the loads on MSVC (next
paragraph); the table is v0.2.1's.

v0.2.2: MSVC compiled the byte-by-byte 64-bit load as eight loads, which
the pack reader found first (docs/pack.md 9.1). With `memcpy` loads under
`_MSC_VER`, `yvid_xxh64()` on 1 GiB in memory went from 5.93 and 6.32 GB/s
to 11.87 and 14.11 GB/s (medians of 9, two interleaved rounds, the
measurement lock, MSVC 19.44 `/O2`); MinGW gcc 16.1 `-O2` stayed at 13.04
to 13.64 GB/s, its code unchanged. The hashes are the same (the test's
published vectors). Mutant x-01 (`tests/mutate/video.toml`, test
`video_msvc`) checks that the vectors catch a wrong load on the MSVC
path. The writer reserves the index at
create when `max_frames` is set, so writing allocates nothing per frame.

The .yspvi index: a 128-byte header (magic `YSPVIDX1`, version 1, codec,
size, format, rate, frame count, GOP, the five color fields, the media
file's size and the XXH64 of its first and last 64 KB, the XXH64 of the
header), then one XXH64 per decoded frame, then the command that made
the file, as text. `yvid_index_make()` decodes the file with pl_mpeg,
refuses B-frames, a GOP that changes length, a first frame that is not a
keyframe, and a frame whose time is not its index, and writes the index.

Checked on generated clips: a clip with `-bf 2` is refused ("B-frames
(frame 2)"); one byte changed in the middle of an indexed clip passes
the size and end hashes and is flagged by the frame hashes ("NOT
CANONICAL: hash mismatches"); one changed index hash is flagged.

## pl_mpeg

- **No delay.** By default pl_mpeg returns each frame one call late, to
  reorder B-frames, and its `picture_type` is then the next frame's. The
  canonical form has no B-frames, so the backend sets
  `plm_video_set_no_delay()`: decode order is display order, a frame
  comes out as it is decoded, and the index maker reads the type of the
  frame it got. Found when the index maker refused every clip ("the first
  frame is not a keyframe").
- **Files over 2 GB.** pl_mpeg's file buffer uses `ftell`, 32 bits on
  Windows. The backend gives pl_mpeg its own load, seek and tell callbacks
  over the header's 64-bit byte source, which also carries the reader.
  The load discards read bytes first, as pl_mpeg's file buffer does, so
  the buffer does not grow.
- **Heap.** `PLM_MALLOC`, `PLM_REALLOC` and `PLM_FREE` go through the
  header's counter. 0 heap calls per frame after frame 120 in every window
  run, pl_mpeg included.
- **Warnings.** MSVC /W4 reports C4244, C4267 and C4305 in pl_mpeg; they
  are disabled around its include only. gcc 11 and clang are clean.
- **Time.** pl_mpeg's frame time is a count over a double rate. The
  backend maps it to 90 kHz units and the core rounds to a frame index,
  which holds for hours at any MPEG-1 rate.

## Media Foundation

Round 2 (2026-10-06). An `IMFSourceReader` in synchronous mode, made in
`yvid_open()` and used on the decode thread. It reads MP4 with one H.264
or HEVC Main video track and gives NV12, which goes into the existing hash
and then to the planar upload. Nothing is linked: `mfplat.dll`,
`mfreadwrite.dll` and, for DXVA, `d3d11.dll` and `dxgi.dll` are loaded at
run time, and the GUIDs are local copies.
`tests/compile/video_com.cpp` checks every GUID, the `MFVideoArea`
layout and the IStream table against the Windows SDK on MSVC and MinGW.

One IStream, the header's own, carries a path (64-bit offsets), memory and
a `yvid_reader` to Media Foundation, so a pack entry read by byte range
plays like a file. Media Foundation calls it from its work-queue threads;
an SRWLOCK keeps one call at a time. COM: `CoIncrementMTAUsage()` at open
keeps a multithreaded apartment alive, so the decode thread needs no COM
setup. The test opens a movie from a frame thread in a single-threaded
apartment, as SDL3 leaves it, and decodes on the pump: it works.

The test clips come from `tests/media/make_video_clips.sh` (ffmpeg,
libx264 and libx265; each frame carries its index in 16 luma bars). The
core test runs the Media Foundation cases when `YVID_TEST_MEDIA` names
the clips' directory and skips them with a message otherwise.

### Findings that changed the code

| Finding | Seen on | Change |
|---|---|---|
| With `MF_LOW_LATENCY`, Microsoft's H.264 decoder gave frame 1 frame 0's time, then accumulated its 100 ns roundings (frame 4 at 999999 instead of 1000000) | c_720p30, every frame's time against its bars | Not set. Without it every time is the container's, to 100 ns |
| `MF_READWRITE_DISABLE_CONVERTERS` together with `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` fails reader creation (E_INVALIDARG) | every clip | The software decoder gets DISABLE_CONVERTERS. With hardware, the transform chain is read after the type is set, and a second transform (a converter) is refused |
| The output type's rate is a 100 ns approximation: 24000/1001 comes out 10000000/417083 | c_23976 | The index maker fits the rate from the compressed sample times: the simplest k/1 or k x 1000/1001 (k up to 1000) that puts every frame within 100 ns of its grid time. At play the index's rate is used; Media Foundation's must agree to 100 ppm |
| The output type says "mixed interlace or progressive" for every H.264 stream | every clip | Mixed is accepted at the type; the index maker checks each decoded sample's `MFSampleExtension_Interlaced` |
| Media Foundation reports none of the color the stream states (matrix, range, transfer, primaries, siting) | every clip | The index maker reads the H.264 SPS's VUI from `MF_MT_MPEG_SEQUENCE_HEADER` (about 140 lines): a desc field that contradicts it is refused, a zero desc field takes it. HEVC's VUI is not read (it sits behind profile_tier_level and the short-term reference picture sets), so an HEVC index needs the color in its desc |
| ffmpeg drops `-color_trc` and `-color_primaries` after a `geq` filter, so the SPS states only matrix and range | ffprobe on the clips | The script also passes the color in x264's and x265's own parameters |
| An absent `chroma_loc_info` means type 0 (left) by H.264 Annex E, and x264 omits it then | c_crop | The parser infers left, as the standard does |
| An MP4 edit list with an empty edit (a 100 ms start offset) plays from time 0 in Media Foundation, with no sign | r_offset | The index maker reads the video track's `elst` and refuses any list but one entry at media time 0 |
| A 1080-line H.264 frame is coded 1088 lines; the chroma plane starts after all 1088 | c_1080p30, against ffmpeg's frames | The display aperture gives the visible size; the chroma offset uses the surface's rows |
| Software, hardware MFT and DXVA give the index's bytes on every frame, and ffmpeg's decoder gives them on the first three | c_720p30, c_1080p30, c_1080p60, c_crop | One index serves every decoder and machine. A decoder that differs on any frame is a finding, and the frame is flagged HASH_MISMATCH |

### Refusals

The index maker refuses each generated refusal clip with its reason
(core test): B-frames ("decodes at ... but shows at ..."), frames missing
from the grid and a 1 ms timescale at 30000/1001 ("fit no constant
rate"), a keyframe forced off the GOP, 10-bit and 4:4:4 (by the H.264
profile), a 2:1 pixel aspect, interlace (per sample), MPEG-4 Part 2,
rotation (the display matrix; 270 degrees for ffmpeg's
`-display_rotation 90`), and an edit list. HEVC Main 8-bit decodes on this
machine. A machine without an HEVC decoder gets "no decoder gives NV12 ...
no HEVC decoder may be installed".

### Seeks

`seek(key)` sets the position half a frame past the keyframe's time (the
source takes the sync sample at or before it). It then reads forward to
the frame whose time maps to `key`, so `next()` returns the keyframe as
the interface requires. The index maker decodes every keyframe and the
frame after it again after a seek and compares both hashes: the property
the movie's seeks rely on, proved per file. Seeks to every target in the
test land on the exact frame (bars and hash).

## Soundtrack

Round 2. The first design, a `yvid_port` that took ysp/audio.h's one
source slot, was withdrawn: movie sound goes on the same path as every
other sound. The soundtrack is a `yau_wav` (ysp/audio.h v0.2.0) whose
stream is a voice, placed, recorded and confirmed like any sound.
`yau_play_at()` voices keep mixing beside it; the test plays a tone
during the movie and checks both.

How the movie drives it:

- `yvid_soundtrack(mv, au, &desc)` after `yvid_open()` and before the
  first play opens the WAV (path, memory or a `yvid_reader`, which has
  `yau_reader`'s layout) into a ring of the movie's own memory, 1 s by
  default. ysp_audio refuses a rate, a channel count or a sample type
  that is not the device's; ysp_video refuses the rest (below).
- The movie's decode thread feeds it: `yau_wav_step()` runs in the
  pump's idle step before each decode step. The frame thread wakes the
  pump when `yau_wav_wants()` says the ring is under half full.
- A start: the sound starts at the sample nearest the movie time (a tie
  to the earlier). With ASAP the target is the first predicted display
  onset both can reach: at or after now plus `yau_lead_ns()` plus one
  period, once the frames and the ring are in. The movie base is anchored
  at that target, so the base, the sound and the first frame share one
  origin (the real-device run below found the onset needed).
- A seek during play: the play is stopped at once
  (`yau_stop_at(id, 0, ramp)`), the decode thread retries
  `yau_wav_seek()` to the target's sample (BUSY until ysp_audio has
  ended the play) on each wake, the ring refills, and the movie resumes at
  the next shared ASAP target with a new play. A pause stops at its
  target; a resume is a new play from the sample one display period after
  the frozen time. A loop is the WAV's `loops = YAU_FOREVER`: sample s
  reads file frame s mod A, so the stream's numbering, and the follow,
  continue across cycles.
- The lock: `yvid_follow_audio()` takes the movie time at each onset
  from `yau_stream_sample_at()`, the soundtrack's own sample on the
  fit. Before the play's origin is fixed (PENDING) the base keeps its own
  anchor.
- Records: ysp_audio's ONSET (confirmed), STREAM, GAP and END are the
  sound's records. ysp_video adds `YVID_EV_SOUND` per start (1) and stop
  (2), with ysp_audio's play id, the first sample and the target.

Refusals that ysp_video keeps:

- **Length.** A = N x den x rate / num, the movie's duration in samples.
  The WAV must hold round(A) samples, a tie up: (2 x N x den x rate + num)
  / (2 x num) in integers. At 48 kHz and 30000/1001 fps, 151 frames are
  241841.6 samples: 241842 is accepted, 241841 and 241843 are refused. The
  test checks an integer A, a fraction, and a tie (A = 187.5 accepts 188,
  refuses 187).
- **Loop.** A loop needs A to be an integer, so that no cycle slips a part
  of a sample: at 30000/1001 and 48 kHz the frame count must be a multiple
  of 5 (the message says so).
- **Base rate.** No resampling: a soundtrack is refused at a base rate
  other than 1, and a later change of rate stops it (yvid_update()
  returns YVID_ERR_REFUSED once).

Tests, on ysp_audio's scripted device on the same virtual clock as the
display (identity samples in a float WAV in memory): the start on the
target's frame and the first video frame within 1 us of it; the base
starting at the target when the display misses that vblank; every output
sample equal to sample (frame minus origin); the movie
time against the audio within 10.4 us (half a sample) at +37 and -400 ppm
of device drift; a tone mixed beside it; pause, resume and seek (the stop
frame, silence, the new start's sample); a loop over three cycles; a late
timed start that skips the samples it missed; the refusals.

On the real device, under the lock, 2026-10-06 11:47 to 11:58, AC: the
10 min A/V clip of `make_video_clips.sh --av` (1080p30, DXVA, UPLOAD)
with its 48 kHz WAV on the default device (Realtek, WASAPI shared through
miniaudio, period 480, the speaker muted at volume 0 as it was), in the
composed window, `yvid_follow_audio()` on every frame. A scratch test
tool ran 60 s: play, at 20 s a seek to 300 s, at 36 s a pause, at 38 s a
resume. The times are ysp/screen.h's flip onsets and ysp/audio.h's fit,
so this checks the lock between the two clocks, not light against sound.

| | Before the fix (2 runs) | After (4 runs: 3 of 60 s, 1 of 20 s) |
|---|---|---|
| Each start's ONSET against its target | -0.05 to +0.01 ms | -0.014 to +0.010 ms |
| The first new frame of each start: flip onset minus its time on the audio clock | -6.3 to +7.5 ms | -0.19 to +0.07 ms |
| Every shown frame: flip onset minus its time on the audio clock, p1 / p50 / p99 | -6.5 / -6.2 / -3.1 ms; -2.4 / +4.5 / +7.5 ms | -0.42 to -0.09 / -0.15 to -0.02 / +0.03 to +0.08 ms |
| Gaps, underruns | 0, 0 | 0, 0 |

The device ran 44 to 48 ppm fast against the ysp_rt clock; the fit's
spread was 42 to 61 us.

**The fix.** The first runs show a constant offset of up to half a
display period between picture and sound, a different one each run. The
shared ASAP target was now + `yau_lead_ns()` + one period, a time
between two vblanks: the sound started on it exactly, and the frame due
then showed at the nearest vblank, so every frame after kept that phase.
The target is now the first predicted display onset at or after that
time. The scripted test did not see it: it allowed half a period. It now
wants the first frame within 1 us of the target, and the old code fails
it; and the base test makes the display miss the target's vblank, so the
base must still start at the target, not where the first frame landed.

Outside those numbers: in every run, about 1.3 s after the start, the
composed window showed three flips a vblank before ysp/screen.h's
predicted onset, then one a vblank late (a residual of -16.7 ms on one
frame; the movie time at the predicted onset was right). In one run the
compositor showed 3.6 s of flips one or two vblanks late (+16.5 and
+33.2 ms, the first of each flagged LATE, 2 DISPLAY_LATE drops). The
records report both as they happened. Both belong to the composed path,
not to the lock.

### Rerun: the early flips (2026-10-09)

The same run (the A/V clip, DXVA, UPLOAD, the soundtrack, the 60 s
script) was done again on ysp/screen.h v0.4.3, with a scratch tool that
logs every flip record, the scanout's vblanks (`D3DKMTGetScanLine` polled
on its own thread, as in docs/screen.md, "Held frames a vblank early"),
the console display state and the session lock. The display was on and
the session unlocked from the start to the end of every run below. Same
laptop, AC, 640 x 360 window or fullscreen. The ysp/screen.h in the tree
then (14:53) was v0.4.3 with another worker's depth changes in progress:
by default the path sets the depth, and `desc.depth_learn` keeps
v0.4.3's learner. Both were run. "Early" is a flip whose OS time is on
the scanout vblank before its planned one; the records' EARLY flag and
the scanout agreed on every flip.

| Configuration | Runs | Early flips a run | Late flips a run |
|---|---|---|---|
| DXGI_FLIP, window in the foreground, v0.4.3 learner | 4 | 3, 3, 3, 3 | 0 |
| DXGI_FLIP, window in the foreground, path sets the depth | 9 | 6 in 6 runs; 108, 112, 116 in 3 runs | 0 to 2, at frames 0 and 1 |
| DXGI_FLIP, fullscreen, v0.4.3 learner | 3 | 0, 1, 3 (frames 3 to 5) | 0 to 1, at frame 0 |
| DXGI_FLIP, fullscreen, path sets the depth | 4 | 0 | 0 to 2 at frames 0 and 1; in 1 run 4 more at 19.5 s |
| COMPOSITION, window or fullscreen | 8 | 0 | 0, except 1 run: 1 at frame 0 and 1 at 19.3 s |
| DXGI_FLIP, window or fullscreen in the background | 7 | not known: 93% of flips had no statistic (ESTIMATED) | not known |
| COMPOSITION, window or fullscreen in the background | 4 | not known: no statistic; one frame each 4 vblanks | not known |

A run in the foreground used the tool's `--topmost`, which is
`screen_flipstats --topmost` (one zero-size mouse move takes the
foreground). Without it, 11 of 18 runs stayed in the background,
mostly the second and third of a set; those are the background rows.

The early flips have one cause, the depth rule of ysp/screen.h, not its
grid (no flip in these runs is held, so the v0.4.3 grid fix does not
apply) and not ysp/video.h (it calls `yscr_flip()` with no target and
reads only `f.onset`). At 1.7 to 1.9 s the system takes the window from
the composed path to an overlay plane. From then on a present shows on
the first vblank after it (depth 1), but `GetFrameStatisticsMedia` still
reports the composed path for 5 more flips. The flips planned at depth 2
show a vblank early:

- v0.4.3's learner sees no path change, so it needs 3 flips in a row at
  depth 1 to lower the depth: 3 early flips. This is the finding of
  2026-10-06 (then at about 1.3 s).
- With the path setting the depth, the depth falls only when the report
  says overlay: 6 early flips. When open() learned depth 3 on the
  composed path (3 runs in 9), every composed flip was early (about 105),
  and the flips at the change 2 vblanks early; nothing lowers the depth
  until the path changes.

In fullscreen the learner showed the same thing at open: the window
is composed for its first frames, and 1 or 3 flips showed early at
frames 3 to 5. Without the scanout thread the counts were the same (6,
6, 116 in a window). The one late flip after the early ones and the
3.6 s of late flips of 2026-10-06 did not come again in 39 runs (28 in
the foreground): late flips came alone or 2 in a row. A window in the
background gave no statistic for 93% of its flips, so a late run there
would not be seen; the run of 2026-10-06 may have been one of those.
Nothing in ysp/video.h changed.

## Base rates

Round 2, with ysp/timeline.h v0.4.0 (`ytl_rate`, `ytl_get_rate`,
`ytl_rt_time`, `ytl_window`). The due frame is the largest i with
t(i) at or before `ytl_window()`, the base time at RT onset + L, so the
lead stays RT ns and a video frame lands where an annotation at its time
lands, at any rate. The other places that assumed rate 1:

| Place | At rate num/den |
|---|---|
| The loop wrap's RT time | `ytl_rt_time()` of t(N) |
| A resume | the frozen time plus one display period times num/den |
| The record's `due` | `ytl_rt_time()` of the frame's time |
| The nominal schedule (slips) | the advance per vblank and the lead in base ns; it starts again at a change of rate, so a change is not a slip |
| The cadence | R against r x num/den, again at each change; a `YVID_EV_RATE` record (num, den, the multiple, display frames per frame, whether strict_cadence refuses it) |
| strict_cadence | a rate whose cadence judders holds the frame on screen and makes yvid_update() return YVID_ERR_REFUSED until the rate gives a multiple again or the movie closes. A seek alone does not clear it: it lands and holds again |
| A soundtrack | refused (above) |

Tests: the annotation agreement at 1/2, 1001/1000 and 2/1 under 20 us of
onset noise, and with a change from 1 to 1/2 mid-play (no annotation
twice, no slip at the change); the cadence and the record's due at 1/2;
pause and resume at 1/2; a loop of 23.976 at 1/2 over 20 cycles (the
wraps on the rate's grid, the movie time across each wrap exact to 2 ns);
23.976 at 1/2 on 60 Hz (5.005 display frames per frame: CADENCE, no
DRIFT); strict_cadence; the soundtrack refusal. Nine mutations, each
caught. At rate 1 the records are byte-identical to v0.1.0's: a harness
built against both headers compared 17,455 lines over four scenarios
(seeks, pause, a seek that stays paused, a loop, timeline annotations
under noise).

## Upload and conversion

Round 2: the planes go up as they are. A YUV movie's slot holds its NV12
or I420 planes, tight (1.5 bytes a pixel instead of 4); the decode thread
copies a decoder's borrowed planes into it (Media Foundation's have a
padded pitch, and its buffer is the decoder's again at the next call);
the frame thread uploads them with `ygfx_texture_update_planes()` into a
`YGFX_NV12` or `YGFX_I420` texture whose encoding is the canonical
form's matrix, range and siting (and `desc.chroma` as `chroma_nearest`).
ysp/gfx.h's video program converts. light CODES passes the codes through
as device values (`YGFX_TRC_DEVICE`, `YGFX_PRIM_DEVICE`); light EOTF
(and AUTO under a calibration) gives the stated transfer and primaries,
so the movie is shown in linear light through the calibration. gfx.md
has the shader's accuracy: at 1:1 its chroma weights at the stated siting
equal the ones below exactly, and the pixel tests are within 3.1e-7.

The CPU conversion below stays as `yvid_yuv_to_rgba()`, a public tool
function (tests, export). Its numbers are the v0.1 record. M1 and M2
("Measurements") decided for the planes; the A/B build that kept the old
path is deleted.

v0.1: the decode thread converts I420 and NV12 to RGBA8 with the canonical
matrix and range; the frame thread uploads RGBA8 with one
`ygfx_texture_update()`. Chroma is brought to 4:4:4 by weights in sixteenths
for the stated siting (left: co-sited across, between rows down;
center: between both), or replicated (`YVID_CHROMA_NEAREST`). The
conversion is within 1 code of the same weights and matrix in double,
over every matrix, range, siting and both formats (core test).

The conversion runs in two plain loops per row, a chroma pass and a
matrix pass, so that a compiler can vectorize them. An A/B in one process
at 1080p: MSVC 19.44 /O2 6.80 ms (one pass) against 7.03 ms (two passes),
the same bytes; gcc 11 -O3 in WSL2 11.2 ms against 6.7 ms. Kept the two
passes.

## GPU path

Round 2. `YVID_PATH_GPU`: DXVA decodes on the screen's own D3D11 device;
the decode thread copies each kept frame on the GPU into one of the
movie's own NV12 textures; ysp/gfx.h imported each texture once at open,
and the frame thread rebinds the stimulus to the due one. No readback, no
upload, no slot memory. It was planned as ZERO_COPY: the decoder's own
surfaces imported and drawn. Findings that changed it:

| Finding | Seen on | Change |
|---|---|---|
| `MF_SA_D3D11_BINDFLAGS` on the reader's attributes is ignored: the surfaces came with bind flags 0x600 (DECODER, VIDEO_ENCODER), so they cannot be sampled | c_720p30, the import refused ("no SHADER_RESOURCE") | `MF_SOURCE_READER_D3D11_BIND_FLAGS` at reader creation. It works, but see the next row |
| Holding decoder surfaces for decode-ahead stalls the decoder: with ahead 6 (8 slots held) ReadSample never returned | every clip | No decoder surface is held. One `CopySubresourceRegion` a frame into the movie's own textures, and the sample is released at once. The decoder keeps its pool |
| The copy and the draw are on one immediate context (the screen's, with multithread protection on) | design | The context orders them; no fence and no keyed mutex. The slot on screen is not a copy target until another slot replaces it |
| The frame never reaches the CPU | design | The timestamps are checked; the frame hashes are not, and describe() says so |

Checked on hardware with `examples/video/check.c`: each movie drawn through UPLOAD and through
the GPU path, frame by frame in manual mode, each read back with
`ygfx_read_scene()`. At 1280 x 720, 1920 x 1080 (coded 1088) at 30 and
60 fps, 40 frames each with jumps across GOPs: every frame shows its own
index in the bars, and the two paths give the same value at every pixel
(worst difference 0), with a scratch copy of ysp/screen.h first and with
the tree's `desc.d3d11_video` after it landed.

`YVID_PATH_SHARED` (a second device and a shared texture) is not built:
the GPU path covers the case it was planned for. ysp/gfx.h's import test
proved the keyed-mutex variants if a renderer ever needs it.

M7 ("Measurements") weighs it against UPLOAD: 20 to 50 % less process
CPU at 1080p and close frame-thread means, but a longer draw and flip
tail; dropped frames did not separate the two. UPLOAD stays the default;
`gpu_path` is the knob.

## Measurements

Under the lock, 2026-10-05, MSVC 19.44 Release.

Decode thread, per frame (`examples/video/bench.c`, 200 repetitions or
600 frames):

| Work | 1280 x 720 mean / p99 | 1920 x 1080 mean / p99 |
|---|---|---|
| pl_mpeg decode (generated clip, q 3, GOP 30) | 2.22 / 4.88 ms | 5.54 / 12.98 ms |
| XXH64 of the I420 planes | 0.27 / 0.31 ms | 0.59 to 0.72 / 0.88 to 1.61 ms |
| I420 to RGBA8, sited chroma | 3.08 to 3.12 / 3.96 to 7.08 ms | 6.66 to 7.78 / 7.71 to 20.0 ms |
| I420 to RGBA8, nearest chroma | 2.33 / 2.93 ms | 5.41 / 9.97 ms |
| QOI decode, smooth frame (26.7:1) | 1.01 / 1.26 ms (3.7 GB/s) | 2.37 / 4.18 ms (3.5 GB/s) |
| QOI decode, noise (1.0:1) | 5.00 / 5.74 ms (0.74 GB/s) | 11.47 / 16.45 ms (0.72 GB/s) |

So a 1080p MPEG-1 frame costs about 14 ms of one core on the decode
thread (decode, hash, conversion): 2.4 times real time at 30 fps, and
not enough for 1080p at 60 fps. The conversion costs more than the
decode; the planar shader removes it.

Frame thread, `yvid_update()` including the upload, in a 640 x 360
window, display frames after the 120th (`examples/video/play.c`, 20 s
each). Bar: the 16.7 ms frame. gfx.md measured a full 1920 x 1200
RGBA8 upload at 1.1 to 2.6 ms mean and 4.4 to 5.8 ms p99.

| Movie | Frames with an upload: mean / p50 / p99 / max | Repeats: mean / p99 |
|---|---|---|
| 1280 x 720 MPEG-1, 30 fps | 0.52 / 0.46 / 1.00 / 2.08 ms | 0.008 / 0.017 ms |
| 1280 x 720 RGBA8 frame sequence, raw | 0.52 / 0.46 / 0.89 / 2.48 ms | 0.010 / 0.025 ms |
| 1920 x 1080 MPEG-1, 30 fps | 0.95 / 0.82 / 1.62 / 2.64 ms | 0.007 / 0.015 ms |
| 1920 x 1080 RGBA8 frame sequence, raw | 0.98 / 0.85 / 1.78 / 2.85 ms | 0.009 / 0.024 ms |

Every run: 0 heap calls after frame 120, no DECODE_LATE, every repeat
DUE (30 fps on 60.0008 Hz is a multiple at +13 ppm). One run had one
DISPLAY_LATE drop, and a 23.976 run one at a 117 ms stall of the
compositor (5 vblanks), both with the flip's LATE flag.

24000/1001 MPEG-1 on the window, three runs of 9 s: CADENCE repeats on
the 3-showing frames (105 to 109 a run), one DRIFT at the first frames of
a run (the screen's predicted onset moved by a period while the vblank
count moved by one) and one later in another.

What the composed window does to the counts. In the first runs,
ysp/screen.h sometimes planned two consecutive frames for the same vblank
in a composed window (onsets 136 ns apart, after flips flagged EARLY) and
sometimes skipped one, so a count of display frames per video frame read
1 and 4 where the cadence is 2 and 3. ysp/screen.h fixed the double
planning; three new 9 s runs at 24000/1001 (2026-10-05, 21:43, under the
lock) had no vblank planned twice (0 of 1600 display frames; 2 in 2 runs
before). One run showed only 2 and 3 vblanks per frame. The other two had
one frame on 4 vblanks and the next on 1, each at a vblank gap of 2 whose
flip carried LATE_TARGET (the frame loop was late), at the start and
about 1.7 s in; ysp/video.h called those DISPLAY_LATE or DRIFT. Every
record in those runs came through `yscr_frame.done` (n_done 1, and 2 or
3 after a gap). ysp/video.h's decisions follow the vblank count and the
onset either way.

Not measured: slips on the real panel (13 ppm: one each 20.9 min at
30 fps), fullscreen, the audio clock against a real device, a decode
thread under load, any GPU but the Iris Xe.

### Round 2: Media Foundation (M1, M3, M4)

Under the lock, 2026-10-06 10:17 to 10:36, AC, MSVC 19.44 Release,
`examples/video/bench.c --mp4` on the clips of
`tests/media/make_video_clips.sh --long` (testsrc2 with temporal noise,
libx264 CRF 20; 1080p30 at 64 Mb/s, 1080p60 at 36 Mb/s: harder than
most camera footage at these sizes). Three rounds, rows interleaved; the
table gives the median over the rounds. The first round ran about twice
as slow as the other two in every row; the medians are the later rounds'.
The bench decodes as fast as it can, so Media Foundation's worker threads
are busy throughout and compete for cores with the hash and the
conversion; a movie decodes at its own rate and pays less of that.

Decode thread per frame, mean / p99 ms. "Planar path" is what v0.2
does: ReadSample and the lock (for DXVA the readback), the hash, and the
copy of the planes into a slot. "RGBA8 path" is v0.1's, with the NV12 to
RGBA8 conversion instead of the copy.

| Clip | Decoder | ReadSample + lock | Hash | Conversion | Copy | RGBA8 path | Planar path | CPU of the process per frame (Media Foundation's threads) |
|---|---|---|---|---|---|---|---|---|
| 1280 x 720, 30 fps | software | 0.29 / 1.05 | 0.41 / 0.94 | 5.32 / 11.49 | 0.13 / 0.30 | 6.02 / 12.54 | 0.83 / 1.85 | 15.4 ms (9.6) |
| | hardware MFT | 0.28 / 0.99 | 0.41 / 0.81 | 5.12 / 9.92 | 0.12 / 0.28 | 5.80 / 11.30 | 0.81 / 1.75 | 14.8 ms (9.6) |
| | DXVA | 2.61 / 4.95 | 0.30 / 0.71 | 4.24 / 9.67 | 0.11 / 0.23 | 7.15 / 14.02 | 3.02 / 5.46 | 5.8 ms (0.7) |
| 1920 x 1080, 30 fps | software | 0.44 / 1.34 | 0.94 / 1.66 | 12.27 / 22.29 | 0.34 / 0.74 | 13.65 / 24.72 | 1.72 / 3.13 | 33.3 ms (20.8) |
| | hardware MFT | 0.42 / 1.29 | 0.89 / 1.58 | 11.74 / 21.40 | 0.32 / 0.70 | 13.05 / 23.04 | 1.63 / 3.01 | 31.5 ms (20.1) |
| | DXVA | 4.52 / 7.75 | 0.77 / 1.85 | 10.24 / 21.33 | 0.31 / 0.68 | 15.50 / 28.22 | 5.57 / 9.01 | 12.7 ms (1.0) |
| 1920 x 1080, 60 fps | software | 0.50 / 1.50 | 0.99 / 1.82 | 12.09 / 22.48 | 0.40 / 0.78 | 13.57 / 24.32 | 1.89 / 3.30 | 28.8 ms (15.4) |
| | hardware MFT | 0.45 / 1.42 | 0.96 / 1.90 | 11.32 / 21.53 | 0.35 / 0.78 | 12.72 / 23.75 | 1.75 / 3.25 | 26.1 ms (13.6) |
| | DXVA | 4.36 / 6.97 | 0.74 / 1.87 | 8.79 / 19.71 | 0.29 / 0.65 | 13.66 / 26.65 | 5.34 / 8.53 | 11.2 ms (0.8) |

Against the bar (decode thread mean at most half a frame period, p99 at
most one; 1080p60: 8.3 / 16.7 ms): the planar path meets it with every
decoder at every size. The RGBA8 path misses it at 1080p60 with every
decoder: its conversion alone takes 9 to 12 ms (6.7 to 7.8 ms in v0.1's
quiet bench; here Media Foundation's threads share the cores). That is
the planar path's case, measured.

"ReadSample" for the software decoder and the hardware MFT is short
because Media Foundation decodes ahead on its own threads: their cost is in
the CPU column, 13 to 21 ms of CPU per 1080p frame, more than one core at
1080p60. The "hardware MFT" (Intel's, reported by
`MFT_ENUM_HARDWARE_URL_Attribute`) costs as much CPU as the software
decoder here. DXVA costs a readback of 2.6 to 4.5 ms on the decode thread
and about 1 ms of Media Foundation's threads. M2 below decides AUTO.

Open to the first frame in hand (M3), cold / warm median: software 115
to 177 / 52 to 91 ms; hardware MFT 133 to 235 / 59 to 112 ms; DXVA 240 to
308 / 116 to 177 ms. The 300 ms bar is missed by DXVA at 1080p60 in two
rounds of three (293, 259, 308 ms cold).

A seek to a random frame, request to the target in hand (M4), mean / p99:
software 72 / 100 ms (720p, GOP 30), 154 / 228 ms (1080p30, GOP 30), 155 /
241 ms (1080p60, GOP 60); the hardware MFT about the same; DXVA 15 / 32,
32 / 70 and 42 / 127 ms. DXVA meets the bar (GOP x the mean decode + 50
ms) everywhere. The software decoder's frames after a seek cannot be
pipelined, so a seek costs about 5 ms per frame decoded (155 ms for up to
29 frames at GOP 30), against a ReadSample mean of 0.5 ms in steady play.

### Round 2: the frame thread (M2) and the decisions

Under the lock, 2026-10-06 10:41, AC, MSVC 19.44 Release,
`examples/video/play.c --hw ...` in the 640 x 360 composed window, 30 s a
run, two rounds, rows interleaved. "RGBA8" is v0.1's path (the decode
thread converts, the frame thread uploads 4 bytes a pixel), built for this
A/B only. `yvid_update()` on display frames with an upload, after the
120th, ms; the range is over the two rounds.

| Clip | Path, decoder | Mean | p99 | Max | Drops a run (why) |
|---|---|---|---|---|---|
| 1920 x 1080, 30 fps | planar, software | 0.40 to 0.47 | 0.72 to 1.01 | 1.63 | 0 to 1 (DRIFT) |
| | planar, hardware MFT | 0.39 to 0.45 | 0.74 to 0.81 | 1.24 | 0 |
| | planar, DXVA | 0.40 to 0.42 | 0.70 to 0.77 | 0.99 | 0 |
| | RGBA8, software | 0.90 to 1.07 | 1.72 to 1.98 | 2.62 | 0 |
| | RGBA8, DXVA | 0.92 to 0.97 | 1.49 to 1.60 | 2.15 | 0 |
| 1920 x 1080, 60 fps | planar, software | 0.40 to 0.42 | 0.68 to 0.78 | 1.20 | 1 (DISPLAY_LATE) |
| | planar, hardware MFT | 0.40 to 0.46 | 0.83 to 0.93 | 1.45 | 1 to 2 (DISPLAY_LATE, DRIFT) |
| | planar, DXVA | 0.46 | 0.69 to 0.83 | 1.04 | 1 (DISPLAY_LATE) |
| | RGBA8, software | 0.97 to 1.01 | 1.83 to 1.97 | 3.24 | 1 (DISPLAY_LATE) |
| | RGBA8, DXVA | 1.01 to 1.03 | 1.67 to 1.80 | 2.83 | 4 to 6, and 3 to 5 repeats (DECODE_LATE) |

Every run: 0 heap calls after frame 120. A repeat costs 0.005 to 0.011 ms
mean. The one DISPLAY_LATE drop per 60 fps run comes with every path and
decoder; its flip carried LATE.

Decisions, by M1 to M4 and M2:

- **Planar upload kept; the CPU conversion left the play path.** The
  planar path halves the frame thread's cost (0.4 against 1.0 ms mean),
  and on the decode thread it is the only one that meets the 1080p60 bar
  (M1). RGBA8 with DXVA at 1080p60 fell behind (DECODE_LATE). The A/B
  switch (`YVID__CPU_CONVERT`) is deleted; `yvid_yuv_to_rgba()` stays
  as a tool.
- **AUTO is DXVA, with the software decoder when no DXVA device opens.**
  The frame thread costs the same with every decoder (above). DXVA uses
  11 to 13 ms of process CPU per 1080p frame against 29 to 33 ms for the
  software decoder, and seeks 4 to 5 times faster (M4); it opens about
  100 ms slower (M3) and spends its readback on the decode thread, which
  has room for it.
- **The hardware MFT is deleted** (`YVID_HW_MFT`): the same CPU as the
  software decoder and the same frame-thread cost; no benefit measured.

### Round 2: the GPU path (M7)

Under the lock, AC, MSVC 19.44 Release, the 640 x 360 composed window,
DXVA in both rows, 30 s a run, rows interleaved: `video_play --hw dxva`
with and without `--gpu`. It times `yvid_update()`, the draw and the
flip on the frame thread, and the process CPU after frame 120. Three
sets:

- A and B (11:30 and 11:39): a scratch copy of ysp/screen.h with the
  video device option. In B another worker's GPU test ran during round 1
  and a compile during round 3; those rows are left out.
- C (13:56 to 14:08): the tree's ysp/screen.h (`desc.d3d11_video`) and
  the landed `video_play --gpu`, four rounds. Before each row the script
  waited until no compiler or test ran; one row still overlapped a
  compile. Other activity on the machine (the draw and flip's max reached
  74 ms on both paths) made C noisier than A and B.

| Clip | Path | `yvid_update()` mean / p99, ms | Draw + flip mean / p99, ms | Process CPU, % of a core | Drops a run |
|---|---|---|---|---|---|
| 1920 x 1080, 30 fps | UPLOAD (planes) | 0.41 to 0.73 / 0.71 to 2.41 (frames with an upload) | 0.60 to 1.63 / 0.91 to 15.4 | 13.9 to 20.6 | 0 to 3 |
| | GPU | 0.009 to 0.021 / 0.025 to 0.079 | 0.72 to 1.48 / 1.48 to 8.28 | 8.9 to 14.0 | 0 to 4 |
| 1920 x 1080, 60 fps | UPLOAD (planes) | 0.44 to 0.65 / 0.70 to 1.53 | 0.60 to 1.03 / 0.91 to 7.32 | 25.5 to 30.0 | A, B: 1 to 11 (median 1); C: 0, 27, 25, 0 |
| | GPU | 0.015 to 0.020 / 0.038 to 0.088 | 1.14 to 1.58 / 2.97 to 6.03 | 14.1 to 20.2 | A, B: 8 to 136 (median 19); C: 5, 20, 5, 6 |

Most drops were DISPLAY_LATE. `video_check` with the tree's
ysp/screen.h: bars right and worst difference 0 at 720p30, 1080p30 and
1080p60, as with the scratch copy.

What it shows:

- The GPU path takes the upload off the frame thread, but the draw and
  the flip take longer: the decoder's work and the copy share the
  screen's immediate context, and its lock, with the frame thread. The
  means of the two paths' frame-thread totals are close; the GPU path's
  tail is longer (draw + flip p99 1.5 to 8 ms against 0.9 to 2 ms in the
  quieter runs). Both are far inside the 16.7 ms frame.
- It saves 20 to 50 % of the process's CPU at 1080p (the readback and the
  plane copy), and the 23.7 MB of slots.
- Dropped frames do not separate the paths. In A and B the GPU path
  dropped more at 60 fps (median 19 a run against 1); in C it dropped
  fewer (5, 20, 5, 6 against 0, 27, 25, 0). In a composed window on a
  shared machine the compositor and the other load decide the drop
  count. GPU timestamps, fullscreen (tier 1) and another GPU were not
  measured.

Decision: `YVID_PATH_GPU` stays as an option, and UPLOAD stays the
default (`gpu_path` 0). UPLOAD needs no video device and has the shorter
frame-thread tail; the GPU path is the knob when CPU is short.

## Tests

`tests/adapt/video_test.c`, no GPU, no display, no pl_mpeg: a scripted
decoder, scripted display frames on a virtual clock, and ysp/gfx.h on the
simulated display with a recording backend, so each upload is checked
byte for byte. It covers the pure functions against their definitions,
XXH64 against python-xxhash's vectors, QOI round trips and truncations
(and the encoder's bytes against the reference `qoi.h` when it is on the
include path: equal), the conversion against double, 30 on 60 under noise
and the never-early control, five cadences against an exact model, slips
at 487.3 ppm both ways against a model, display drops, decode stalls with
the catch-up seek, seeks across GOPs (target exact, keyframe and discards
in the record), stay-paused seek, loop with the base rewound, end,
manual mode with the base held, pause and resume, the timeline agreement
at 23.976 on 60 (20 us noise), 30 on 60 and 60 on 30, a timeline lead
that differs, the follow clock at 200 ppm, the FRAME record field by field,
each refusal at open with its message, decoder faults, the frame sequence
in all ten formats from a file and from memory with padded strides,
damage, the reader, replay (byte-equal records over 3000 frames with a
stall, a display drop, a seek and a pause), 0 heap calls per frame, and
one case with the decode thread running for real.

Round 2 adds: the planes of every YUV frame sequence frame uploaded as
stored; the soundtrack on ysp/audio.h's scripted device ("Soundtrack");
the base rates ("Base rates"); and, with `YVID_TEST_MEDIA` set to the
clips of `tests/media/make_video_clips.sh` on Windows, Media Foundation:
the index of each canonical clip, each refusal clip refused with its
reason, the SPS's color (and a contradicting desc refused), the first
three frames' hashes equal to ffmpeg's decoder's, every frame's bars at
720p, 1080p (coded 1088) and 1280 x 718, 23.976, the three decoders bit-exact
with the index (and an index made by DXVA serving the software decoder),
seeks to eight targets across GOPs, memory and reader sources, a damaged
byte (hash mismatch), a stale index, 0 heap calls per frame, and the
decode thread for real from an STA frame thread. The threaded case now
also stalls the decoder (PENDING) for 300 ms at frame 40 on the real
decode thread ("Builds and mutations": it found a data race). The GPU
path needs a screen, so the core test checks only its refusals;
`examples/video/check.c` compares it with UPLOAD on hardware.

v0.2.1 makes the threaded case's stall an outage (every frame from 40 on
is PENDING until the time, so a seek cannot pass it), checks that a frame
is shown within 50 display frames of its end, and adds a deterministic
threaded case ("Recovery after a decoder outage"). Both failed on v0.2.0.

## Builds and mutations

Warnings as errors everywhere.

| Toolchain | What ran |
|---|---|
| MSVC 19.44, CMake, Release | compile check C and C++17 (with and without pl_mpeg), core test with the qoi.h comparison, examples; full ctest without pl_mpeg 39 of 40, the one failure `test_gfx`, which did not compile at that moment (gfx_test.c line 2061, another agent's edit in progress) |
| MSVC 19.44, cl alone | compile check as C and as C++17, each with and without pl_mpeg, /W4 /WX |
| MinGW-w64 gcc 16.1 (winlibs) | compile check C99, C11 and C++17 with pl_mpeg, C11 without; core test with the qoi.h comparison |
| gcc 11.4, WSL2 | compile check C99 -O3, C11 and C++17 under ASan and UBSan; core test under ASan and UBSan, and under ThreadSanitizer (no report; the compile check with its decode thread too). In the follow-up, ysp/gfx.h (mid-edit, v0.3) had unused-function warnings for a while; the mutation runs used -Wno-error=unused-function |
| emcc 6.0.10, node | compile check with threads, without (decodes inline), and with YRT_NO_THREADS; core test with threads and with YRT_NO_THREADS; pl_mpeg compiled as C11 and C++17 |

Not built here: clang on Linux and macOS, MinGW against SDL3 (CI does).

The core test caught each of 23 deliberate mutations of the header: the
at-or-after rule; counting vblanks instead of reading the clock;
DISPLAY_LATE and DECODE_LATE swapped; no catch-up to the due frame; a
resume that re-shows the frozen time; the follow clock ignored; record
completion one frame off; a missing DROP record; the hash check skipped;
the timestamp check skipped; the CADENCE repeat a showing late; frame
times floored; stale slots not purged; the due published after the inline
decode; QOI's LUMA range off by one (caught only by the qoi.h comparison:
the round trip still works); left siting weighted as center; an XXH64
constant; no resync after a slip; the loop wrap without the base rewind;
the canonical check skipping the siting (the loop wrap mutation is caught
by the run's 600 s timeout: the test hangs); `yscr_frame.done` ignored
in favor of `last`; a seek that anchors instead of skipping; a manual step
that skips instead of firing (caught after the test gained an annotation
between frames 5 and 6). A first try at "a seek lands on
the keyframe" was not caught because the catch-up to the due frame masks
it; it is not a fault the code can show. The test also found that the
first conversion scratch in the test was too small for the two-pass
conversion: `yvid_yuv_rows_bytes()` now sizes it, and the test checks.

Round 2 builds (2026-10-06, on the v0.2.0 header):

| Toolchain | What ran |
|---|---|
| MSVC 19.44, CMake, Release | compile checks C and C++17 with pl_mpeg, the COM check (`tests/compile/video_com.cpp`: every GUID, `MFVideoArea` and the IStream table against the Windows SDK), the core test with the clips (`YVID_TEST_MEDIA`), the examples |
| MinGW-w64 gcc 16.1 | compile check C99 and C++17 with pl_mpeg, the COM check |
| gcc 11.4, WSL2 | compile check C99 -O3 and C++17; core test under ASan and UBSan, and under ThreadSanitizer (run with `setarch -R`: this WSL kernel's address layout makes TSan stop at "unexpected memory mapping" otherwise) |
| emcc 6.0.10, node (2026-10-06) | the CI wasm job's commands: compile check with and without the decode thread, the core test, pl_mpeg compiled. The test first failed to compile: clang's `-Wunused-but-set-global` on five upload globals that only the Media Foundation cases read. Fixed in the test; the CI job had passed anyway, because `set -e` ignored the failed `emcc` on the left of `&&` (fixed in ci.yml) |
| gcc 13.3 (WSL2 and an Ubuntu 24.04 container), clang 18.1 (2026-10-06) | core test at -O2 and under ASan and UBSan; every target and ctest with the CI jobs' flags |
| MinGW-w64 gcc 16.2, MSYS2 (2026-10-06) | the MinGW CI job and the Screen job's MinGW step, with the COM check. The core test first failed: `-Wmaybe-uninitialized` on `yrt_event`s that a failed `yvid_result()` leaves unwritten. Fixed in the test |

Round 2 mutations, each on a copy of the header, run against the core
test (with the clips for Media Foundation). The Media Foundation and
soundtrack lists are in [tests/mutate/video.toml](../tests/mutate/video.toml);
the base-rate list was not kept:

| Area | Caught | Not caught, and why |
|---|---|---|
| Media Foundation | 9 of 11: pts to index rounded down; the chroma plane after the visible rows (caught by ffmpeg's frames); the display aperture ignored; the B-frame checks; the rate fit at 1 ms; the edit list check; the per-sample interlace check; the SPS's color ignored; the SPS's fields in the wrong order (caught after a clip with three different fields was added) | A seek that stops at an earlier keyframe: Media Foundation always positions on the keyframe itself on these clips, so the forward read never runs. The decoded times against the grid: the compressed times' rate fit catches every clip that would fail it; the check stays for a decoder that restamps frames (as `MF_LOW_LATENCY` did) |
| Soundtrack | 10 of 10, re-run on the final header: the length rounded down; the loop check removed; the follow from the device's stream (caught after the test checked for the CLOCK record); ASAP without the audio lead; a resume at the frozen sample; a movie time's sample rounded down and the base anchored at the landing onset (both caught after a test without the follow was added); a seek that does not stop the sound; no refusal at a base rate other than 1; the shared ASAP target not put on a display onset (the test now wants the first frame within 1 us of the sound's target; it allowed half a period). With the target on an onset, the anchor at the landing onset equals the anchor at the target unless the first frame lands late, so the base test now makes the display miss the target's vblank, and catches it again | |
| Base rates | 9 of 9 ("Base rates") | |
| Decode-ahead | 1 of 1 under ThreadSanitizer: the slot of a PENDING decode pushed back onto the free queue from the decode thread | |
| v0.2.1: PENDING on the decode thread (`v021-00`, `v021-01` in video.toml) | 2 of 2: PENDING does not mark the thread idle; PENDING while discarding toward the due frame not retried (an outage meets it when the due frame is in the same GOP) | |

**A data race, found by the new threaded case.** The free queue is
single-producer: the frame thread frees slots, the decode thread takes
them. After a decoder returned PENDING, the decode thread pushed the slot
it had taken back onto the queue: a second producer. The core test's one
threaded case never had a PENDING decode, so it did not show. With a
300 ms stall at frame 40 on the real decode thread, ThreadSanitizer
reports "data race ... in yvid__free_push". The decode thread now keeps
that slot for its next decode; ThreadSanitizer is quiet, and putting the
old push back makes it report again.

v0.2.1 builds: MSVC 19.44 C11 (`/W4 /WX`) and C++ (the compile checks),
MinGW gcc C11 (warnings as errors); WSL gcc 11.4 at -O2 and under
ThreadSanitizer (no report). The core test passes on all of them.

## Open, finish and close; messages (v0.3.0)

v0.3.0 applies the repository conventions (README.md, Conventions) to this
header. Four decisions:

- **The writer opens, finishes and closes.** `yvid_seq_create()` is
  `yvid_seq_open()`: it takes a desc and opens a file. The old
  `yvid_seq_close()` returned an `int`, because writing the index and the
  header can fail on a full disk; a close must return `void` and be safe
  twice. So the writing moved to `yvid_seq_finish()`, which returns 0 or
  a negative code, and `yvid_seq_close()` only releases. A writer not
  finished is finished by close, so a program that forgets finish does
  not lose its frames; its outcome is in `yvid_seq_error()`. The file is
  complete only after a finish that returned 0.
- **`yvid_probe()` returns an `int`.** It is a call without a handle
  that can fail for several reasons (`YVID_ERR_IO`, `YVID_ERR_FORMAT`,
  `YVID_ERR_REFUSED`), and `bool` is for open and predicates. The message
  stays in the caller's buffer.
- **Every failing call on a movie or a writer writes its message.** The
  controls, `yvid_update()`, `yvid_follow()`, `yvid_soundtrack()`,
  `yvid_follow_audio()`, `yvid_seq_write()` and `yvid_seq_finish()` run
  inside a wrapper, as in ysp/screen.h v0.6.0: a specific message stays,
  a call that wrote none gets "ysp_video: <call>: <code's text>", and a
  success puts the last failure's message back. The decode thread keeps
  its own message (`dt_error`), which `yvid_update()` copies on the frame
  thread, so no call races on `yvid_error()`.
- **Times.** Every time was already `int64_t` ns. The parameters that
  did not say so are renamed (`t_ns`, `movie_ns`, `resume_at_ns`,
  `m_ns`, `period_ns`); the rename is in the declarations only.
  `YVID_NS_PER_S` is gone: `YRT_S(1)` is the same value. The read-only
  fields (`yvid_info.duration`, the record's times) keep their names.
  `yvid_out.pts` is in the stream's own units (`timescale`), not ns.

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, and MinGW-w64 gcc
-std=c99 and -std=c++17 -pedantic for USAGE): `tests/adapt/video_test.c`
checks the writer's message after a full write, that a success keeps
it, finish twice, close on a zeroed writer, after a failed open and
twice, a close that finishes, and `yvid_probe()`'s codes.
`tests/compile/video.c` names the writer's calls and the probe by type.
Mutants v030-00 (a failure that writes nothing) and v030-01 (a close
that does not finish) are caught.

## Needed from the other headers

| Header | Request | Why |
|---|---|---|
| ysp/screen.h | `yscr_native()`: the D3D11 device and context, the EGL display, the adapter LUID | Delivered (2026-10-05); the DXVA device takes the LUID, the GPU path the device |
| ysp/screen.h | A desc option for a D3D11 device with VIDEO_SUPPORT and multithread protection | Delivered by the screen worker: `desc.d3d11_video` (v0.3.1). ysp/video.h reads the device's creation flags and its multithread protection itself. Before it landed, the GPU path ran with a scratch copy of ysp/screen.h (a test tool, never in the tree) |
| ysp/screen.h | Each completed flip record in `yscr_frame` | Delivered: `done`, `n_done`, `done_lost`; used |
| ysp/gfx.h | Planar `YGFX_NV12` and `YGFX_I420` with an encoding, converted by its video program | Delivered (v0.4); used ("Upload and conversion") |
| ysp/gfx.h | `ygfx_texture_import()`, `ygfx_texture_rebind()`, per-plane update, `ygfx_features()` | Delivered (v0.4); used (the planes update, and import and rebind on the GPU path) |
| ysp/gfx.h | `ygfx_screen(g)` and `ygfx_calibrated(g)` | Delivered; used |
| ysp/audio.h | A streaming voice and a WAV reader | Delivered (v0.2.0: `yau_stream`, `yau_wav`); used by the soundtrack |
| ysp/audio.h | A soundtrack start confirmed like any onset | Delivered with the stream: ONSET is confirmed (tier 2 on its conditions) |
| ysp/timeline.h | `ytl_skip(tl, base, rt, bt)` | Delivered (v0.3.0); used for seeks and manual jumps |
| ysp/timeline.h | `ytl_lead(tl)` | Delivered (v0.3.0); used |
| ysp/timeline.h | Exact base rates: `ytl_rate()`, `ytl_get_rate()`, `ytl_rt_time()`, `ytl_window()` | Delivered (v0.4.0); used ("Base rates") |

## Future work

- YVID_PATH_SHARED (a second device, one GPU copy, a shared fence or the
  keyed mutex the gfx test proved): only if a renderer cannot use the
  screen's device. The GPU path covers the case it was planned for.
- HEVC's VUI (its color), read from the SPS as H.264's is.
- DXVA's cold open at 1080p60 (259 to 308 ms against the 300 ms bar):
  `yvid_probe()` or an open during the inter-trial interval hides it.
- `examples/video/clips.c` (proposed): the test clips from Media Foundation's own
  encoder, so CI on Windows needs no ffmpeg (skipped where the encoder is
  missing).
- A `movie_play` example was asked for; the example is
  `examples/video/play.c` (program `video_play`), because CMake builds
  `examples/<lib>/*.c` for each header as `<lib>_<name>`.
- Recovery after a decoder stall: fixed in v0.2.1 (a lost wake-up after
  `YVID_PENDING`, not the drop rule; "Recovery after a decoder
  outage"). Not measured: the recovery after a hiccup of a built-in
  decoder (a slow `next()`, not PENDING) on real media, which the same
  catch-up rule covers.
- AVFoundation, FFmpeg, capture.
