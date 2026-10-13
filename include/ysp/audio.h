/* ysp/audio.h - v0.4.1 - public domain single-header audio library
 *
 *   Sound at a time on the ysp/rt.h clock. A buffer is played with
 *   yau_play_at(buf, t); the header plans its first sample on the device
 *   frame that t falls on, through a fit of the device clock against the
 *   ysp_rt clock, and reports where it landed: the stream frame, its time
 *   from that fit, the residual, and whether a device position report
 *   confirmed the frame played (a tier). The time is the fit's, never an
 *   observed time. A stream (a ring that another thread fills: a long WAV
 *   file, a movie's soundtrack, synthesis) plays the same way, with the same
 *   records, and its sample s plays on one fixed frame, origin + s. Also
 *   WAV files (RIFF, RF64, BW64) streamed or loaded, tones, noise and clicks
 *   at the project rate, a strict open that refuses any format or rate
 *   conversion, and the device and source interfaces of the rig's Audio
 *   device and Audio source extensions.
 *
 *   REQUIRES ysp/rt.h beside it (the clock, the event ring, the thread
 *   elevation, the instrumentation macros), and miniaudio 0.11.25 or a
 *   later 0.11 release (miniaudio.h, one public-domain file) on the include
 *   path for the device backends. YAU_NO_MINIAUDIO builds the core alone.
 *
 *   Written in the single-header style of the stb / sokol libraries. C99 is
 *   the floor: it builds as C99, C11 and C++17, and in the C dialect MSVC
 *   compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_AUDIO_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *   ysp/rt.h's implementation comes with it, once, as with the other ysp
 *   headers, and so does miniaudio's (see BUILDING).
 *
 *   Play a tone at a time t on the ysp_rt clock:
 *
 *       #define YSP_AUDIO_IMPLEMENTATION
 *       #include "ysp/audio.h"
 *
 *       static yau_audio au;                          // zeroed
 *       yau_desc d = { 0 };                           // 48 kHz, 2 channels
 *       if (!yau_open(&au, &d)) die(yau_error(&au));
 *       yau_tone_desc td = { 0 };
 *       td.hz = 1000; td.dur_ns = YRT_MS(100); td.peak = 0.03f;
 *       td.ramp_ns = YRT_MS(5);
 *       yau_buf beep = yau_tone(&au, &td);
 *       yau_id id = yau_play_at(&au, beep, t);
 *       if (id < 0) die(yau_error(&au));
 *       ...
 *       yau_onset r;
 *       if (yau_result(&au, id, &r) == YAU_OK)      // or yau_wait()
 *           printf("%lld ns late, tier %d\n", (long long)r.residual, r.tier);
 *
 *   Psychtoolbox and PsychoPy, for comparison:
 *
 *       pa = PsychPortAudio('Open', [], 1, 1, 48000, 2);          % PTB
 *       PsychPortAudio('FillBuffer', pa, repmat(MakeBeep(1000, 0.1, 48000) * 0.03, 2, 1));
 *       onset = PsychPortAudio('Start', pa, 1, t, 1);
 *
 *       beep = sound.Sound(1000, secs=0.1, volume=0.03)           # PsychoPy
 *       beep.play(when=t)
 *
 *   yau_play_at() does not block. Its record arrives later, in the handle
 *   (yau_result) and in the event ring. yau_wait() blocks for it, as
 *   PsychPortAudio's waitForStart does.
 *
 *   A long WAV file, streamed, at t, with a fade 30 s later:
 *
 *       static yau_wav story;                         // zeroed; owns its ring
 *       yau_wav_desc wd = { 0 };
 *       wd.path = "story.wav";
 *       if (!yau_wav_open(&au, &story, &wd)) die(yau_wav_error(&story));
 *       yau_wav_feed(&story, 0);                      // fill the ring (1 s)
 *       yau_id id = yau_play_stream(&au, &story.stream, t);
 *       yau_stop_at(&au, id, t + YRT_S(30), YRT_MS(50));
 *       ...                                             // each frame: feed it,
 *       yau_wav_feed(&story, 0);                      // or let a pump do it (WAV FILES)
 *       ...
 *       yau_wav_close(&au, &story);                   // ends a play still running
 *       yau_close(&au);
 *
 *   From ysp/timeline.h: an event on a channel that the program maps to a
 *   sound goes to yau_play_at() at the event's own RT time, before that
 *   time. yau_lead_ns() says how far before (MODEL, LEAD).
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   TIME
 *     Every time is an int64_t count of nanoseconds on the ysp/rt.h clock,
 *     the unit ysp/timeline.h and ysp/screen.h take. A device that reports
 *     on another clock converts at the boundary: WASAPI's position stamp is
 *     QueryPerformanceCounter time in 100 ns units, the counter
 *     yrt_now_ns() reads.
 *
 *   THE STREAM
 *     The stream frame W counts the frames the header has handed to the
 *     device since open. Every plan and every record is in stream frames.
 *     A device position report (P, t) says that stream frame P was at the
 *     device's output at ysp_rt time t. On WASAPI it is
 *     IAudioClock::GetPosition(), read at every callback.
 *
 *   THE FIT
 *     The header fits t = t0 + (W - W0) * k to the reports (or, with no
 *     reports, to the callback entry times) over a sliding window, and
 *     refits once a second. k is the device's real frame period on the
 *     ysp_rt clock. Two clocks with their own crystals drift apart by
 *     tens of ppm; at 50 ppm a fixed rate would be one frame (20.8 us at
 *     48 kHz) wrong after 0.42 s, and 180 ms wrong after an hour. The fit
 *     follows the drift, so a plan stays on its frame. Reports pair a
 *     position with its own stamp, and are fitted by least squares.
 *     Callback entry times are late and never early, so they are fitted by
 *     their lower envelope (the line under every point with the smallest
 *     summed gap; Moon, Skelly and Towsley, INFOCOM 1999), which least
 *     squares would bias by the mean lateness. Each refit is a
 *     YAU_EV_FIT record. open() runs the stream silent until the fit
 *     spans 0.5 s.
 *
 *   PLAN
 *     The callback, not yau_play_at(), turns t into a frame, from the
 *     newest fit, and again in every callback until the sound starts, so a
 *     newer fit corrects it. The frame is the first one at or after the
 *     fractional frame of t minus a half: the nearest frame, a tie to the
 *     earlier one, which is ysp/timeline.h's rule at frame scale. So the
 *     residual includes up to half a frame of rounding (10.4 us at 48
 *     kHz). There is no sub-frame placement: it would need a filter on the
 *     stimulus. A target whose frame has already gone to the device starts
 *     on the first frame that has not, flagged LATE.
 *
 *   CONFIRMATION
 *     An onset's TIME is always the fit's time for its frame, a model
 *     value: no API reports when a given frame reached the output. What a
 *     position report adds is confirmation, in order: the device has
 *     played past the frame, with no underrun since the frame was
 *     rendered, so the frame is where the plan put it and the fit, made of
 *     the OS's own position reports, held while it played. The record
 *     completes then, about a buffer after the callback rendered the
 *     frame. An onset with no such report (no reports on this backend, an
 *     underrun, the stream closed first) completes with the same kind of
 *     time and YAU_ONSET_UNCONFIRMED. A time between two reports around
 *     the frame was the first design; measured on the test laptop, a
 *     WASAPI position is the audio engine's last pass, stamped when the
 *     call is made, so a single report can be hundreds of microseconds
 *     late, and the fit over many reports is the better time.
 *
 *   NO RESAMPLING, NO INSERTED FRAMES
 *     The header never resamples a buffer or the stream and never adds or
 *     drops a frame to follow the ysp_rt clock. Drift is absorbed by
 *     planning each onset in device frames. A long buffer therefore plays
 *     at the device's rate, and its end is off from start + n / rate by n
 *     times the drift; the END record gives where it really ended. Locking
 *     a movie's soundtrack to its video is ysp/video.h's job: the
 *     soundtrack is a stream, its sample s plays on frame origin + s, and
 *     the movie clock follows yau_stream_sample_at() (STREAMS).
 *
 *   LEAD
 *     A sound must reach the callback before the callback renders the
 *     frame it starts on. yau_lead_ns() is the shortest lead that can
 *     still be met now: the time of the first frame the next callback will
 *     render, plus one period, minus now. Hand sounds over at least that
 *     far ahead; ysp/timeline.h's look-ahead for audio is this number.
 *     On WASAPI shared mode it is mostly the queue (QUEUE) and the
 *     engine's own period and pipeline.
 *
 *   QUEUE (desc.queue, WASAPI shared mode)
 *     The periods of sound queued in the WASAPI buffer ahead of the
 *     engine's next pass when a block is rendered: 2 by default, or 1.
 *     Each period less is a period (10 ms here) less lead, and a period
 *     less margin against a callback that runs late: the queue must not
 *     run dry before the late block arrives. Measured on the test laptop:
 *     shortest safe lead 40 ms at 2, 30 ms at 1; a stall of 15 ms was
 *     ridden out at 2 and not at 1. miniaudio by itself renders one period
 *     before it waits for room, which the header no longer lets it do.
 *     An underrun at either queue is found and recorded (UNDERRUNS), but
 *     the sound still has a gap in it. Use 1 only on a machine where
 *     audio_clockstats --queue 1 under the experiment's load shows
 *     callbacks never 8 ms late. Exclusive mode has no queue: the device
 *     takes one whole buffer per event.
 *
 *   UNDERRUNS
 *     A callback that comes too late leaves the device without data. The
 *     header sees it from the reports (the device has played everything it
 *     was given) or, without reports, from a gap between callbacks longer
 *     than the buffer. Each one is a YAU_EV_XRUN record; every sound
 *     playing or waiting for its report then is flagged XRUN, and an
 *     onset not yet confirmed stays unconfirmed (tier 3). Measured on WASAPI, the position
 *     halts at the frames written while the device has no data, so every
 *     later frame plays later by the time it stood: the fit starts over
 *     from the next reports. A device whose position counts the silence it
 *     played instead shows it as the position past the frames written; the
 *     header then shifts its map by that many frames and keeps the fit.
 *     HALT: a short underrun (a period or so on WASAPI shared) halts the
 *     position before it reaches the frames written: the engine runs dry
 *     ahead of the position. The header sees it from the reports after it,
 *     which are all late against the fit by the halt: three moving reports
 *     in a row, each late by more than half a period, are a halt. Measured
 *     on the test laptop, no report was ever 1 ms late without one. Until
 *     the third, the late reports confirm nothing; then the XRUN record
 *     gives the halt's length, the fit starts over from the last report,
 *     and every sound in flight (planned on the old timing, played after
 *     the halt, or not yet confirmed) is XRUN, UNCONFIRMED, tier 3, its
 *     time the new fit's. A sound whose frame a report passed between the
 *     last on-time report and the first late one may have played before
 *     the halt: its time can be off by the halt either way, and its record
 *     says so (XRUN). The fit has no slope for 5 s after it and is not
 *     checked for halts for 0.5 s.
 *
 *   THE RECORD (yau_onset)
 *     id, target (t as given), onset (+ desc.onset_offset_ns), residual
 *     (onset - target), start_frame and end_frame (stream frames),
 *     device_pos (the report that confirmed it, or -1), rendered_at
 *     (when the callback rendered start_frame: onset - rendered_at is the
 *     lead the path used), buffer_id, tier and flags: PENDING, LATE,
 *     CANCELED (never started), STOPPED (ended early by a call), XRUN,
 *     CLIPPED (the mix went past full scale while it played), UNCONFIRMED
 *     (no position report confirmed the frame played), BELOW_TIER, LOST
 *     (the device went away), GAP (a stream played silence for missing
 *     samples). onset is the fit's time in every record. sample is the
 *     source's sample on start_frame (a buffer's offset; a stream's index,
 *     -1 if it never started); gap_frames the silent frames of a stream,
 *     final once end_frame is set.
 *
 *   TIERS (yau_tier, record.tier, caps.worst_tier, desc.min_tier)
 *     1  the onset is the OS's observation of that frame at the output,
 *        on a path a loopback test verified. No v0.1 path: every onset is
 *        a fit's time (CONFIRMATION).
 *     2  good under stated conditions: the time from the device-clock fit
 *        of OS position reports, the frame confirmed played with no
 *        underrun, on a path whose placement and time a loopback test
 *        checked on this machine. WASAPI shared on the test laptop: every
 *        burst at its planned frame in WASAPI's own loopback capture, and
 *        the capture against the record 67 us p99 from its median idle and
 *        20 us under load in one run (STATUS)
 *     3  unconfirmed (no report past the frame), or a path no loopback
 *        checked (WASAPI exclusive, which the loopback capture cannot see), or
 *        callback times only (CoreAudio, ALSA, PulseAudio, Web Audio in
 *        v0.1)
 *     YAU_TIER_SIM  the null device, or a device that says so
 *     desc.min_tier (1 to 3) flags BELOW_TIER on each onset worse than it;
 *     the run goes on.
 *
 *   THE RING
 *     With desc.ring set, the callback pushes, source YRT_SRC_AUDIO:
 *     YAU_EV_ONSET   t_ns onset; aux play id (low 32 bits);
 *                      u.i64[0] target, [1] start frame, [2] device
 *                      position or -1, [3] rendered_at; u.u32[8] buffer id;
 *                      u.u16[18] flags; u.u16[19] tier (bits 0..2),
 *                      desc.device_index (3..5), fit generation mod 1024
 *                      (6..15): YAU_EV_TIER_OF(), _DEVICE_OF(), _GEN_OF()
 *     YAU_EV_END     t_ns end (the fit's); aux id; i64[0] end frame,
 *                      [1] frames played (a stream's samples, silence not
 *                      counted); a stream also [2] silent frames, [3] the
 *                      next sample (where it stopped); u16[18] flags
 *     YAU_EV_STREAM  in the block that renders a stream's start: t_ns the
 *                      start frame's time (the fit's, not confirmed); aux
 *                      id; i64[0] origin, [1] start frame, [2] the sample
 *                      on it, [3] samples a late start skipped; u32[8] the
 *                      stream's id; u16[18] flags (LATE)
 *     YAU_EV_GAP     when a run of missing samples ends: t_ns its first
 *                      frame's time; aux id; i64[0] first silent frame,
 *                      [1] silent frames, [2] first missing sample, [3]
 *                      samples discarded so far in the play; u32[8] the
 *                      stream's id; u16[18] XRUN when a device underrun
 *                      came while it lasted
 *     YAU_EV_FIT     t_ns t0; aux generation; i64[0] W0; f64[1] frames
 *                      per second; f64[2] spread in ns; u32[6] points;
 *                      u32[7] 1 = reports, 0 = callback times
 *     YAU_EV_XRUN    t_ns when seen; aux the underrun's number (caps.xruns);
 *                      i64[0] stream frame; i64[1] frames the device
 *                      position moved without data; [2] a HALT's length in
 *                      ns (the least late of its reports but the first),
 *                      else 0; [3] the position where the halt was seen.
 *                      A halt right after an early report continues that
 *                      report's record: the same aux, counted once
 *     YAU_EV_OPEN    i32[0] rate, [1] channels, [2] device format
 *                      (YAU_OUT_*), [3] period, [4] buffer, [5] exclusive,
 *                      [6] low latency, [7] backend, [8] reports (0 or 1)
 *     and once a second a raw position report, YRT_SRC_RT /
 *     YRT_KIND_CLOCK: u.u64[0] the position in stream frames, u.u64[1] 0
 *     (the stamp is the device's), aux 0x41550000 + device_index, so the
 *     analysis can fit again offline. Every record is pushed with t_ns and
 *     tid set, so a push makes no system call.
 *
 *   OS LATENCY CLAIMS (caps.os_latency_ns, os_stream_latency_ns, _src)
 *     What the OS says about the delay to the output, as a claim, never as
 *     part of an onset. An onset is the time at the position point plus
 *     desc.onset_offset_ns, which only a line-in loopback test measures.
 *     os_latency_ns is the OS's claim from the position point to the
 *     output; os_stream_latency_ns is the API's own latency figure for the
 *     stream; -1 means no claim. Both go in the describe line and in a
 *     YAU_EV_CLAIM record at open (i64[0], i64[1], the API's name in
 *     bytes 16..39). On WASAPI the position is, in Microsoft's words, "the
 *     stream position of the sample that is currently playing through the
 *     speakers", so the claim is 0; the stream figure is
 *     IAudioClient::GetStreamLatency, "the maximum latency for the current
 *     stream". On CoreAudio (compiled, not run): kAudioDevicePropertyLatency
 *     plus kAudioDevicePropertySafetyOffset, and the buffer frame size on
 *     top for the stream figure. ALSA and PulseAudio: no claim in v0.1.
 *     PortAudio's equivalents are Pa_GetStreamInfo()->outputLatency and a
 *     callback's outputBufferDacTime; PsychPortAudio reports the same
 *     figures in GetStatus as PredictedLatency. The loopback test's --line
 *     mode prints the measured latency minus the claim.
 *
 *   ---------------------------------------------------------------------
 *   PROJECT FORMAT AND STRICT OPEN
 *   ---------------------------------------------------------------------
 *   desc.format is the project's rate (default 48000 Hz), channel count
 *   (default 2), channel map and resolution (YAU_F32, YAU_S16,
 *   YAU_S24). In memory every buffer is float, interleaved, at the
 *   project rate: 16- and 24-bit values are exact in a float.
 *
 *   open() asks the device for nothing and reads what the OS gives: the
 *   miniaudio device gets no format, no rate and no channel count, so
 *   neither Windows nor miniaudio converts anything. It then refuses, with
 *   a message that names what the device gave, unless the device rate is
 *   the project rate, the channel count is the project's, the channel map
 *   is desc.format.map (when given), and the device resolution is at
 *   least the project's (float counts as 24 bits). There is no fallback.
 *
 *   On Windows the device rate is the endpoint's Default Format in the
 *   Sound control panel, in shared AND in exclusive mode: miniaudio opens
 *   exclusive mode at that format too. Change it there (Sound > the device
 *   > Properties > Advanced), or change the project rate. Another rate in
 *   exclusive mode needs an Audio device extension that owns WASAPI.
 *
 *   The mixer writes the device's own sample format (float in shared mode;
 *   for example 32-bit integers with 24 valid bits in exclusive mode),
 *   rounding to nearest, with no dither, clipping at full scale and
 *   flagging every sound that played while it clipped. One sound at unity
 *   gain reproduces a 16- or 24-bit buffer bit for bit.
 *
 *   ---------------------------------------------------------------------
 *   PLAYING
 *   ---------------------------------------------------------------------
 *   yau_play_at(au, buf, t_ns) and yau_play(au, &desc) queue a sound and
 *   return its id, a positive number (with desc.stream, a stream: STREAMS). A negative return is an error code:
 *   a bad argument, a full queue, every voice in use, a closed or lost
 *   device, a buffer whose channel count is neither 1 nor the project's.
 *   A target already too late is not an error; the record says LATE.
 *   desc.at_ns 0 means the first frame the stream can still reach. desc.db is
 *   the gain (0 = unity), desc.channels a mask of output channels (0 =
 *   all): a mono buffer is copied to every channel in the mask, a buffer
 *   with the project's channel count plays only the channels in it.
 *   desc.offset and desc.frames choose a part of the buffer, desc.loops
 *   repeats it (YAU_FOREVER until stopped).
 *
 *   yau_cancel(id): a sound that has not started never plays (CANCELED);
 *   one that has stops at the next callback, with no ramp, so it can click.
 *   yau_stop_at(id, t, ramp): a raised-cosine fade of ramp ns that ends
 *   at t, on its frames (id 0: every sound). yau_gain_at(id, t, db,
 *   ramp): a linear change of gain that starts at t. There is no master
 *   gain; the OS volume is the user's.
 *
 *   yau_update() moves completed records from the callback into the
 *   handle; yau_result(), yau_wait() and yau_play() call it.
 *
 *   ---------------------------------------------------------------------
 *   STREAMS
 *   ---------------------------------------------------------------------
 *   A stream is a voice whose samples come from a ring that a producer
 *   thread fills while it plays: a file too long for memory, a movie's
 *   soundtrack, synthesis. It is played, planned, stopped, faded, routed,
 *   recorded and confirmed as a buffer is, by the same code; only where the
 *   samples come from differs.
 *
 *       static yau_stream st;                         // yours; 256 bytes
 *       yau_stream_desc sd = { 0 };
 *       sd.channels = 1;
 *       if (!yau_stream_open(&au, &st, &sd)) die(yau_error(&au));
 *       ...                                             // the producer thread:
 *       int64_t n;
 *       float* p;
 *       while ((p = yau_stream_acquire(&st, &n)) != NULL) {  // up to the wrap
 *           synth(p, n);
 *           yau_stream_commit(&st, n);
 *       }
 *       ...                                             // the frame thread:
 *       yau_id id = yau_play_stream(&au, &st, t);   // or yau_play(.stream)
 *
 *   THE RING holds project-rate float samples, interleaved, 1 or the
 *   project's channels (desc.frames, default 1 s), in desc.memory or the
 *   handle's arena. The producer widens integer samples to float itself:
 *   that is exact for 16 and 24 bits and is not a conversion of the sound.
 *   One producer thread at a time (which may be the frame thread), one
 *   reader (the device thread), 64-bit sample counts, acquire and release
 *   only: no lock, no allocation, no I/O on the device thread. A mono ring
 *   goes to every channel in the play's mask, as a mono buffer does.
 *   yau_stream_end() says no sample follows the last one committed; the
 *   play then ends there (END, not STOPPED). Without it the stream plays,
 *   or plays silence, until it is stopped.
 *
 *   THE LOCK RULE. In the block that renders its first frame, the callback
 *   fixes origin, the stream frame of sample 0: sample s plays on stream
 *   frame origin + s for the whole play. yau_stream_get_info() gives
 *   origin to any thread once .started is set; yau_stream_sample_at()
 *   and yau_stream_time_of() turn samples into ysp_rt times and back
 *   through the fit (frame thread). A movie clock that follows
 *   sample_at(t) follows the sound frame for frame. Nothing moves origin:
 *     A GAP. When the ring has no sample for a frame, the frame stays
 *     silent and the stream's position moves on by exactly that frame; the
 *     samples that arrive after their frame are discarded, never played
 *     late. Each run of silent frames is one YAU_EV_GAP record, the play
 *     is flagged GAP, and the counters in get_info (gaps, gap_frames,
 *     discarded), yau_onset.gap_frames and yau_caps.gaps count every
 *     one, also with no event ring. The edges of a gap are not ramped: a
 *     gap is a failure, and a ramp would change samples that did arrive.
 *     A LATE START. A target whose frame has gone to the device starts on
 *     the first frame that has not, LATE, with origin from the target, so
 *     the samples it missed are skipped (STREAM record i64[3]). A buffer
 *     instead starts late from its first sample.
 *
 *   SCHEDULING. At t > 0, as a buffer: the first sample is planned on the
 *   frame nearest t, in every block until it starts. Nothing about the
 *   ring is checked at the call, because the producer may catch up before
 *   a start seconds away; a start with an empty ring is a gap and the
 *   ONSET is still on the planned frame, flagged GAP. get_info's ready
 *   (the ring holds .preroll frames) and frames_to_start (the planned
 *   start frame minus the frame of the device's next block) tell the
 *   caller beforehand. At 0 a stream, unlike a buffer, waits until the
 *   ring holds desc.preroll frames (default a quarter of the ring;
 *   YAU_PREROLL_NONE: no wait) or every sample up to its end, and starts
 *   on the next block; the ONSET record shows when that was.
 *
 *   CONFIRMATION. The ONSET of a stream is confirmed exactly as a
 *   buffer's: tier 2 on WASAPI shared under the STATUS conditions,
 *   UNCONFIRMED and tier 3 otherwise. GAP records are not confirmed: a
 *   gap's frames are exact by construction (the mixer wrote the silence on
 *   them) and its time is the fit's, as an END record's is; whether the
 *   device timing held around it, the XRUN records say for the whole
 *   stream, in stream frames.
 *
 *   STATES (get_info .state). IDLE: fill it, play it. yau_play() takes
 *   it (QUEUED), the callback holds it (WAITING, then PLAYING), the play
 *   ends (ENDED). yau_stream_reset(st, first) empties the ring and
 *   numbers the next sample `first`; it is the producer's call and is
 *   YAU_ERR_BUSY unless the stream is IDLE or ENDED, so a stream plays
 *   once per reset and a reset can never race a play: a play takes the
 *   stream by compare-and-swap, and so does a reset. To seek: stop the
 *   play, reset at the new sample when it has ended, fill, play.
 *
 *   LOOPS are the producer's: write sample s from source position s mod A.
 *   The ring is read once, a loop held in memory is a buffer (which loops),
 *   and the sample count must go on across cycles for the lock rule.
 *   yau_wav does it (.loops). .loops, .offset, .frames and .buf with
 *   .stream are YAU_ERR_ARG.
 *
 *   CLOSE. yau_stream_close() ends an arena stream's hold on
 *   yau_arena_reset(), which is YAU_ERR_BUSY while one exists. It never
 *   fails: a play that still runs is canceled, and close waits for the
 *   device to end it (the device's buffer and two periods, 20 ms to 1 s),
 *   because the device reads the stream's struct and ring on every block.
 *   When the device does not answer in that time (it stopped, or a CUSTOM
 *   device's render runs on the closing thread), close leaves the stream
 *   detached with its memory in place: keep the struct and the ring valid,
 *   and an arena ring stays held, until yau_close(). yau_wav_close() does
 *   the same for its stream. Stop the producer (a pump that feeds a
 *   yau_wav) before a close; close does not stop your threads.
 *
 *   ---------------------------------------------------------------------
 *   WAV FILES
 *   ---------------------------------------------------------------------
 *   yau_wav reads a WAV file from a path, from memory or through a
 *   yau_reader (bytes by range, a pack entry; the same layout as
 *   ysp/video.h's yvid_reader) and streams it through its own stream,
 *   .stream. yau_wav_load() reads a whole file into the arena as a
 *   buffer instead. Accepted: RIFF, RF64 and BW64 (sizes from ds64),
 *   16-bit and 24-bit PCM, 24 valid bits in 32 (WAVE_FORMAT_EXTENSIBLE; the
 *   low byte is padding), 32-bit float, plain or EXTENSIBLE. Refused, with
 *   a message that names what the file has: any other format (8-bit, 32-bit
 *   integers and 64-bit floats are not exact in float), a rate that is not
 *   the device's (ysp_audio never resamples), a channel count that is
 *   neither 1 nor the project's, an EXTENSIBLE speaker mask that differs
 *   from desc.format.map, a damaged file. A float sample that is not a
 *   number ends the stream before it, with the sample in the error.
 *
 *   Feeding is the producer's: yau_wav_feed(w, 0) fills the ring.
 *   yau_wav_step is a yrt_idle_fn (one block of up to 4096 frames;
 *   true while there is room) and yau_wav_on_msg a yrt_msg_fn (a
 *   yau_wav_msg: a seek, or -1 to wake), so a pump does the reading:
 *
 *       yrt_pump_open(&pump, &(yrt_pump_desc){
 *           .msg_size = sizeof(yau_wav_msg), .capacity = 4,
 *           .on_msg = yau_wav_on_msg, .on_idle = yau_wav_step, .ctx = &story });
 *       ...                                                  // each frame
 *       if (yau_wav_wants(&story))                         // ring under half
 *           yrt_pump_submit(&pump, &(yau_wav_msg){ .seek = -1 });
 *
 *   A seek waits for the play to end (yau_wav_seek is YAU_ERR_BUSY
 *   until then; a seek message is kept and applied by the next step).
 *   .loops repeats the file (YAU_FOREVER), counting samples on. A path
 *   is opened with stdio and no stdio buffer (setvbuf _IONBF), so the C
 *   runtime allocates nothing on the producer thread; reads go to the OS
 *   (ReadFile, read()) 16 KB at a time and the OS file cache still applies.
 *   It is not FILE_FLAG_NO_BUFFERING or O_DIRECT, which need sector-aligned
 *   offsets and buffers. Only open, close and load (frame thread) and the
 *   producer calls touch the file.
 *
 *   ---------------------------------------------------------------------
 *   SYNTHESIS
 *   ---------------------------------------------------------------------
 *   yau_tone(), yau_noise() and yau_click() fill a buffer in the
 *   handle's arena at the project rate and return it; yau_alloc() returns
 *   a zeroed one to fill yourself. yau_fill_tone() and yau_fill_noise()
 *   do the same into your memory, with no handle. Levels are linear and
 *   required, because a zero default must not mean full scale: .peak for a
 *   tone or a click, .rms for noise; yau_db(-30) is 0.0316. .ramp_ns is a
 *   raised-cosine onset and offset, each that long. The sine is computed
 *   from the frame index, so a long tone does not drift in phase. A
 *   synthesized buffer is mono unless .channels says otherwise, and play
 *   routes it.
 *
 *   Noise is white, Gaussian (default) or uniform, from the 64-bit .seed:
 *   splitmix64 seeds xoshiro256+, Box-Muller makes the Gaussian values, so
 *   one seed gives the same samples on every platform. ysp/trials.h's draw
 *   is the place to get a seed. NOISE THAT WOULD CLIP IS REFUSED: a
 *   Gaussian draw has no bound, so at rms 0.3 a one-second buffer almost
 *   surely holds a sample past 1.0 (3.3 standard deviations). The error
 *   message gives the buffer's peak and the largest rms that would have
 *   fit for that seed; pick a lower rms or the uniform distribution, whose
 *   peak is rms x 1.73.
 *
 *   yau_arena_reset() frees the whole arena; it refuses (YAU_ERR_BUSY)
 *   while a sound from the arena is playing or waiting.
 *
 *   ---------------------------------------------------------------------
 *   BACKENDS
 *   ---------------------------------------------------------------------
 *   YAU_BACKEND_WASAPI (Windows; AUTO there)
 *     Shared mode by default; desc.exclusive takes the device for this
 *     program alone, and every other program goes silent. Through
 *     miniaudio with IAudioClient3 at desc.period when that is a multiple
 *     of the endpoint's fundamental period from its minimum to its
 *     maximum, or at the minimum when desc.period is under it; any other
 *     period falls back to the endpoint's default period. Only some
 *     drivers offer periods under the default: audio_wasapi_periods
 *     says. No automatic conversion, no hardware offload
 *     and no automatic rerouting: a default-device change makes the device
 *     LOST. Exclusive mode: one buffer of one period, event-driven.
 *     Positions: IAudioClock, called in each callback.
 *   YAU_BACKEND_COREAUDIO, _ALSA, _PULSE
 *     Compiled through miniaudio. v0.1 reads no position on them, so every
 *     onset is unconfirmed (tier 3) and the fit uses callback times. They have
 *     not run here (STATUS).
 *   YAU_BACKEND_WEB (Emscripten; AUTO there; v0.4.1; docs/web.md)
 *     miniaudio's Web Audio backend, on one of its two paths:
 *       AudioWorklet  build with -DMA_ENABLE_AUDIO_WORKLETS -pthread
 *                     -sAUDIO_WORKLET=1 -sWASM_WORKERS=1; it needs
 *                     SharedArrayBuffer, so a cross-origin isolated page.
 *                     The callback runs on the audio thread, 128 frames
 *                     (one render quantum) a call
 *       ScriptProcessorNode  any other build. The callback runs on the
 *                     page's main thread, desc.period frames rounded up to
 *                     a power of 2 (256 to 16384); a stalled main thread
 *                     stalls the sound
 *     Both need -sASYNCIFY or -sJSPI: on the main thread open() and
 *     yau_wait() sleep through emscripten_sleep(), so the page runs the
 *     callbacks and the timer below. The AudioContext runs at the project
 *     rate and channels, which the strict open then finds; the browser
 *     converts to the device's format unseen. An AudioContext starts only
 *     after a user gesture (a tap or click on the page; on iOS Safari the
 *     gesture must come first): open from the gesture's handler, or the
 *     context waits for miniaudio's next tap.
 *     Positions: a timer on the main thread reads getOutputTimestamp()
 *     every 10 ms (and the ScriptProcessorNode's handler each block):
 *     contextTime, the context frame the browser says is at the output,
 *     and performanceTime, when. Less the context frame of stream frame 0
 *     it is a position report, and every block passes the newest
 *     (pos_source DEVICE). Stream frame 0's context frame comes from the
 *     AudioWorklet's currentFrame, exact, or the ScriptProcessorNode
 *     block's playbackTime, which Chrome 154 gives as currentTime plus the
 *     buffer: late by up to the audio device's chunk (10 ms there). A
 *     block that moves it (a skipped block) is an XRUN. performance.now()
 *     maps to the ysp_rt clock by a correlation at open, on the main
 *     thread. An AudioWorklet's scope has no performance.now() (Chrome 154;
 *     yrt_now_ns() reads 0 there): its tick.t_entry is Date.now() less its
 *     offset to performance.now() at open, 1 ms steps. Its fit is of the
 *     reports only: a block without one adds no point.
 *     Tier YAU_TIER_WEB (5), the lowest, confirmed or not. Every onset is
 *     the browser's estimate: getOutputTimestamp() includes the browser's
 *     own figure for the output latency (outputLatency), which nothing here
 *     checked against sound. The describe line says the path, the reports,
 *     baseLatency and outputLatency, and block_moves. No browser runs
 *     capture or an exclusive device. Measured in docs/web.md, "Audio".
 *   YAU_BACKEND_NULL
 *     miniaudio's null device: a timer, no hardware. Tier SIM.
 *   YAU_BACKEND_CUSTOM
 *     desc.dev and desc.dev_ctx: your device (DEVICE).
 *
 *   ---------------------------------------------------------------------
 *   DEVICE (the Audio device extension)
 *   ---------------------------------------------------------------------
 *   A yau_device is a table of functions: open (fill yau_device_caps
 *   with what the hardware really runs at), start (begin calling the
 *   render function from the device's real-time thread), stop, close and
 *   describe. The device calls render(host, out, frames, &tick) once per
 *   block from one thread, with tick.t_entry (yrt_now_ns() at callback
 *   entry), tick.pos and tick.pos_t (a position report in stream frames
 *   and ysp_rt time, or pos -1) and tick.flags (YAU_TICK_XRUN when the
 *   API reported a gap). out is in the device's format (caps.out). The
 *   device owns its OS objects and converts its times to the ysp_rt clock;
 *   the core owns the plan, the fit, the mix and the records. The thread
 *   that will call render calls yau_rt_thread_init(host) once, before
 *   start. The header's timing statements end at this boundary for a
 *   device that is not its own. tests/adapt/audio_test.c has one.
 *
 *   ---------------------------------------------------------------------
 *   SOURCE (the Audio source extension)
 *   ---------------------------------------------------------------------
 *   desc.source: a function that adds frames into the float block the
 *   mixer is building, after the voices, with the frame's stream index and
 *   time (yau_clock). It runs on the device thread: it must not
 *   allocate, lock, do I/O or take unbounded time. One source per handle.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   A stream is 256 bytes in your memory, one cache line per writer (the
 *   producer's, the device thread's, the setup, the device thread's own),
 *   plus its ring; a yau_wav is about 17 KB (its stream and a 16 KB read
 *   buffer), so nothing allocates while it plays. A stream voice keeps its
 *   stream pointer in the buffer pointer's place, so the voice array, the
 *   hot data of every block, does not grow (200 bytes a voice).
 *   The handle, about 142 KB, holds the voices, the command and
 *   completion queues, the results of the last 256 sounds, the fit window
 *   and the miniaudio context and device: make it static or allocate it,
 *   not a local. The ONLY allocation the header makes is the
 *   synthesis arena: when desc.arena is NULL, open() takes desc.arena_bytes
 *   (default 8 MB) with malloc, and close() frees it. With your own arena
 *   the header's code allocates nothing. miniaudio allocates its own
 *   objects inside open(), none afterwards. The callback, yau_play(),
 *   yau_update() and yau_result() allocate nothing and take no lock;
 *   the two queues are single-producer single-consumer rings.
 *   One thread calls the API for a handle (the frame thread). A stream's
 *   producer calls (acquire, commit, write, end, reset, and yau_wav_feed,
 *   seek, step, on_msg) come from one other thread at a time, or the frame
 *   thread; yau_stream_get_info and yau_wav_wants from any. The device
 *   thread is miniaudio's; on WASAPI it joins MMCSS's "Pro Audio" task.
 *
 *   ---------------------------------------------------------------------
 *   ERRORS
 *   ---------------------------------------------------------------------
 *   yau_open() clears the message, and a failed open leaves one. Every
 *   call of the frame thread that returns a negative code writes
 *   yau_error() before it returns, in the frame loop too (yau_play,
 *   yau_update, yau_result, yau_stop_at, ...), with the call's name and the
 *   code's text, or a more specific message the call wrote; synthesis and
 *   yau_stream_open() write theirs. The write is an snprintf on the failure
 *   path only. The producer's calls (acquire, commit, write, end, reset)
 *   return codes and write nothing: they run on another thread, and a
 *   write into the handle would race with the frame thread's. yau_wav has
 *   a message of its own (yau_wav_error), which open and yau_wav_feed
 *   write, and open's code in error_code. The callback writes nothing.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Put miniaudio.h (0.11.25 or a later 0.11) on the include path, or name
 *   it in YAU_MINIAUDIO_HEADER. The implementation block defines
 *   MINIAUDIO_IMPLEMENTATION, with miniaudio's decoding, encoding, engine,
 *   resource manager, node graph and generation parts left out, unless the
 *   translation unit already included miniaudio's implementation or
 *   defines YAU_MINIAUDIO_EXTERNAL (miniaudio compiled elsewhere). Link
 *   -ldl -lpthread -lm on Linux. Windows links nothing: miniaudio loads its
 *   DLLs at run time. Define YAU_NO_MINIAUDIO to build the core with
 *   YAU_BACKEND_CUSTOM only. Include ysp/audio.h (or ysp/rt.h) before
 *   any system header in the implementation file. Define YAU_API to
 *   change the linkage of every function.
 *   Hot paths carry ysp/rt.h's instrumentation macros: zones yau.render,
 *   yau.mix, yau.fit; a plot of the queue depth in frames.
 *
 *   LICENSE: public domain / MIT-0, see end of file.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.4.1 - YAU_BACKEND_WEB (BACKENDS, WEB): position reports from the
 *          AudioContext's getOutputTimestamp() on miniaudio's AudioWorklet
 *          and ScriptProcessorNode paths, the context at the project's
 *          rate and channels, YAU_TIER_WEB (5) for every web onset, no fit
 *          point from a web block without a report, main-thread waits that
 *          yield (emscripten_sleep). The describe line's backend part may be
 *          160 bytes (was 32). Nothing changes on the other backends.
 *   v0.4.0 - the repository conventions (README.md). Breaks the API:
 *          yau_stream_init()/yau_stream_release() are bool
 *          yau_stream_open() and void yau_stream_close(), which never
 *          fails (a play still running is canceled and close waits for
 *          the device: STREAMS, CLOSE); yau_wav_open() returns bool (code
 *          in w->error_code), yau_wav_close() is void and never fails.
 *          YAU_S and YAU_MS are gone: ysp/rt.h's YRT_S and YRT_MS.
 *          Desc time fields end in _ns: yau_play_desc.at_ns,
 *          yau_tone_desc, yau_noise_desc and yau_click_desc dur_ns and
 *          ramp_ns; time parameters are named _ns. Every failing call of
 *          the frame thread writes yau_error() (ERRORS); the producer's
 *          calls return codes only. yau_stream_sample_at() and
 *          yau_stream_time_of() take a non-const handle for it. USAGE
 *          builds as C99 (C, { 0 }); the CHANGELOG moved to the end of
 *          this comment.
 *   v0.3.1 - HALT: an underrun of about a period is now found. On WASAPI
 *          shared mode it halts the position short of the frames written,
 *          so v0.3.0 never saw it, and every later onset time was off by
 *          the halt (10 ms here) while its record said tier 2. A report late
 *          against the fit by more than half a period, three moving reports
 *          in a row, is a halt: an XRUN record (u.i64[2] the halt in ns,
 *          [3] the position where it was seen), counted in caps.xruns, the
 *          fit starts over from that report, and every sound in flight is
 *          flagged XRUN, UNCONFIRMED, tier 3. Late reports confirm nothing
 *          until the run ends. An early report that comes just before the
 *          run (measured at queue 2) is the same underrun, counted once.
 *   v0.3.0 - QUEUE: on WASAPI shared mode the device thread renders a
 *          block only when the engine has taken the buffer down to
 *          desc.queue - 1 periods (default 2), on the event miniaudio waits
 *          on, instead of a period before it waits. Shortest safe lead on
 *          the test laptop 50 ms before, 40 ms at the default, 30 ms at
 *          desc.queue = 1; each period saved is also a period less margin
 *          against a stalled callback. miniaudio's own order (0 before) is
 *          gone. yau_desc.queue, yau_caps.queue, yau_device_open.queue,
 *          yau_device_caps.queue; queue= in the describe line. WASAPI
 *          exclusive mode asks miniaudio for one period per buffer: with
 *          three, each event took three callbacks in a burst and the run had
 *          underruns and onsets 62 ms off the plan; with one, no underrun and
 *          onsets on the plan, but this laptop's driver still buffers about
 *          100 ms in exclusive mode (docs/audio.md, "Periods and buffers").
 *   v0.2.1 - yau_wav_probe(): the WAV parse without a device, for the pack
 *          tool (docs/pack.md, AUDIO), with the same refusals except the
 *          device's rate, channels and speaker map, which the caller
 *          compares with the project's. The parse is split into that scan
 *          and the device checks; yau_wav_open() and yau_wav_load() give
 *          the same results and messages as v0.2.0.
 *   v0.2.0 - streams: yau_stream, a ring-fed voice played by yau_play()
 *          with .stream (or yau_play_stream), planned, recorded and
 *          confirmed as a buffer is; sample s on stream frame origin + s,
 *          late starts and ring underruns skip samples (GAPS); records
 *          YAU_EV_STREAM and YAU_EV_GAP, flag YAU_ONSET_GAP,
 *          yau_onset.sample and .gap_frames, yau_caps.gaps. WAV files:
 *          yau_wav (RIFF, RF64, BW64; 16-bit, 24-bit, 24 in 32, float)
 *          streamed from a path, memory or a byte-range reader, with
 *          yrt_pump hooks, and yau_wav_load into the arena. One gain,
 *          fade and routing kernel for buffers and streams (a buffer's
 *          output is byte for byte v0.1.0's). YAU_ERR_IO.
 *   v0.1.0 - first release: the scheduler and mixer, the device-clock fit,
 *          the onset confirmed by position reports past its frame, WASAPI
 *          shared and exclusive through miniaudio with IAudioClock
 *          positions, miniaudio's null device, the device and source
 *          interfaces, synthesis, the records, the parameter table.
 *
 *   STATUS: v0.4.0, 2026-10-10: tests/adapt/audio_test.c passes on the
 *   scripted device with new checks of close and the messages (MinGW-w64
 *   gcc 16.1, MSVC 19.44); the USAGE blocks compile as C99 (C, { 0 });
 *   3 of 3 new mutants caught (docs/audio.md, "Units, open and close").
 *   v0.3.1. Halts, measured on the same laptop (docs/audio.md,
 *   "Halts"): injected callback stalls of 5, 10, 15, 20 and 30 ms, 21 of
 *   each at queue 1 and at queue 2. Every stall that halted the position
 *   was found (126 of 126), 49 to 70 ms after the stall, the halt in the
 *   record within -0.19 to +0.36 ms of the position's shift; no record
 *   for the 84 stalls the queue rode out. False records in the long runs
 *   of v0.3.0, run again (10 minutes idle and 3 under load at queue 2 and
 *   at queue 1, 156,325 callbacks) and in 60 s of exclusive mode: 0. No
 *   report there was more than 0.72 ms late against its own line; the
 *   rule's threshold is 5 ms. With a sound each frame through 21 halts at
 *   queue 1, every flagged onset's time was within -150 to +161 us (p1 to
 *   p99) of the reports after the halt; at queue 2, 80 of 105 were, and
 *   25 that an early report passed kept the old time, 10.1 ms before
 *   (which is right is not known: no loopback). Shortest safe lead and
 *   tier 2 residuals as in v0.3.0. Queue 1 stays opt-in: its margin is 8
 *   to 10 ms of callback lateness, at the 8 ms measured with MMCSS.
 *   v0.3.0. Periods and buffers, measured on the same laptop
 *   (docs/audio.md, "Periods and buffers"): the endpoint's shared-mode
 *   engine period is 480 frames only (IAudioClient3 minimum = maximum =
 *   default), so no shared stream runs under 10 ms here; the shared buffer
 *   is 1056 frames. Frames written minus the device position, 10 minutes
 *   idle: 1898 (39.5 ms) before, 1418 at the default queue, 938 at queue 1.
 *   0 underruns at each queue in 10 minutes idle and 3 minutes under load;
 *   callback interval p99 10.17 to 10.34 ms, max 11.6 to 12.5 ms. Shortest
 *   safe lead (audio_schedule, 2 runs each): 50, 40 and 30 ms. A callback
 *   stall the queue rides out: 25 but not 35 ms before, 15 but not 25 ms at
 *   the default, 8 but not 15 ms at queue 1. Onset residuals within -32 to
 *   +21 us, tier 2, at every queue. An underrun that a stall of that size
 *   causes halts the position for about a period and was not detected
 *   (xruns 0; the position never reaches the frames written): v0.3.1
 *   finds it.
 *   Exclusive mode: 0 underruns in 60 s at 480 and 144 frames, onsets on
 *   the plan, but the driver queues 30 to 110 ms and the shortest safe lead
 *   is 125 ms; raw WASAPI shows the same, so it is the driver.
 *   v0.2.0. Streams, measured on the same laptop and device (AC,
 *   MSVC /O2, rows interleaved; docs/audio.md "Streams"): render cost
 *   per callback with 32 voices 10.35 us mean (v0.1.0 in the same session
 *   11.38), with 1 stream 10.48, with 4 streams 12.07 (p99 34.7); 0 gaps
 *   and 0 underruns in every row. A WAV stream on a yrt_pump under load
 *   (8 spinning threads, a 60 Hz frame loop) had 0 gaps for 3 minutes with
 *   rings of 0.25, 0.5 and 1 s; the default stays 1 s for the margin it
 *   leaves a producer that stalls (480 ms). 0 heap calls while 4 streams
 *   play. A WAV feed of 4096 frames costs the producer 5 to 24 us mean.
 *   Placement through a stream was checked on the scripted device only
 *   (every sample on frame origin + s, 2880 ring wraps, gaps, late starts,
 *   drift +-100 ppm); no loopback run with a stream yet (--line --stream
 *   is built). The buffer path is byte for byte v0.1.0's. The rest of
 *   this block is v0.1.0's, unchanged. Software timestamps only, on one Windows 11 25H2 laptop
 *   (i7-1360P), "Speakers (Realtek(R) Audio)" in WASAPI shared mode at 48000
 *   Hz, float, 2 channels, through miniaudio 0.11.25. No loopback cable was
 *   available: the delay to sound is UNMEASURED, desc.onset_offset_ns stays
 *   0 until tests/loopback/audio_loopback.c --line runs, and the OS's
 *   claim (0 ms on WASAPI) is not checked. docs/audio.md has the tables.
 *   Shared mode ran a 10 ms period whatever was asked; 10 minutes idle and
 *   3 minutes under load (8 spinning threads and a 60 Hz frame loop) gave 0
 *   underruns, callback interval p99 10.29 to 10.35 ms. The device ran +3.9
 *   ppm against QPC. Placement, through WASAPI's own loopback capture of the
 *   same endpoint (after the endpoint volume and an effect, so not bit-exact
 *   there): every burst at its planned frame distance from the one before in
 *   runs whose capture had no gap (700 of 700 idle, 500 of 500 under load).
 *   Time, capture against the onset record: deviation from the median 21.8
 *   us p50 and 67.3 us p99 idle (bar: 20.8 us, one frame; missed), 6.2 and
 *   19.8 us under load; the median, 33 ms, is the tap's offset, not a
 *   latency. The shortest lead with no LATE record was 43.5 to 54.5 ms.
 *   A WASAPI position is the engine's last pass stamped at the call (a
 *   report can be hundreds of us late); the reports are fitted by least
 *   squares on the least late report of each 200 ms, which predicted the
 *   next second within 110 us p99 over 10 minutes against 357 us for the
 *   lower envelope; callback times by the envelope (207 against 359 us). In
 *   a 60 ms stall the position halted at the frames written and later
 *   frames played 40 ms later; the fit restarts. Under load the position
 *   once jumped up to 45 ms ahead for tens of seconds; such reports are now
 *   kept out of the fit and flag the sounds in flight. Tiers: an onset's
 *   time is always the fit's, never observed, so no path is tier 1; WASAPI
 *   shared onsets confirmed by position reports are tier 2 on the
 *   conditions above (67 us p99 from the median idle, 20 us under load in
 *   one run, against a one-frame bar of 20.8 us). Exclusive mode (3
 *   runs of 30 s) delivered callbacks in bursts with 2 underruns a run: not
 *   usable on this machine. Cost per callback with 32 voices: 18.7 us mean,
 *   56.7 us p99; GetPosition() 2 to 4 us p50; one refit 5.5 us p50; the
 *   frame thread 1.5 us mean, 2.6 us p99 per play. No heap call from the
 *   header after open, none in open with a caller's arena (MSVC debug, the
 *   C runtime's hook; miniaudio's 10 at open are its own). A core test with
 *   a scripted device on a virtual clock (no miniaudio, no sound hardware)
 *   on MSVC, MinGW gcc 16.1, gcc 11.4 (C99 -O3, ASan and UBSan,
 *   ThreadSanitizer) and emcc (node); fourteen mutations of the header each
 *   make it fail. Compiled, never run: CoreAudio (with its latency claims),
 *   ALSA, PulseAudio, Web Audio (emcc as gnu11).
 *   Outside this STATUS block and docs/audio.md, a number in this header
 *   is a measurement only where the text says "measured".
 *
 */
#ifndef YSP_AUDIO_H_INCLUDED
#define YSP_AUDIO_H_INCLUDED

#define YAU_VERSION_MAJOR 0
#define YAU_VERSION_MINOR 4
#define YAU_VERSION_PATCH 1
#define YAU_VERSION_STRING "0.4.1"

#include "ysp/rt.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YAU_API
#define YAU_API extern
#endif

/* --- codes -------------------------------------------------------------- */

#define YAU_OK              0
#define YAU_PENDING         1     /* yau_result: not complete yet        */
#define YAU_ERR_ARG       (-1)
#define YAU_ERR_CLOSED    (-2)    /* the handle is not open                 */
#define YAU_ERR_FULL      (-3)    /* command queue or every voice in use    */
#define YAU_ERR_FORMAT    (-4)    /* buffer channels fit neither 1 nor the
                                     * project                               */
#define YAU_ERR_NOT_FOUND (-5)
#define YAU_ERR_LOST      (-6)    /* the device went away or was rerouted   */
#define YAU_ERR_BUSY      (-7)    /* arena reset while an arena sound plays */
#define YAU_ERR_TIMEOUT   (-8)
#define YAU_ERR_IO        (-9)    /* a file or reader could not be read     */

#define YAU_MAX_CHANNELS 64
#define YAU_MAX_VOICES   64
#define YAU_FOREVER     (-1)      /* yau_play_desc.loops, yau_wav_desc.loops */
#define YAU_PREROLL_NONE (-1)     /* yau_stream_desc.preroll: start at once */

/* Channel positions, the same numbers as miniaudio's MA_CHANNEL_*. */
#define YAU_CH_NONE  0
#define YAU_CH_MONO  1
#define YAU_CH_FL    2
#define YAU_CH_FR    3
#define YAU_CH_FC    4
#define YAU_CH_LFE   5
#define YAU_CH_BL    6
#define YAU_CH_BR    7
#define YAU_CH_SL   11
#define YAU_CH_SR   12
#define YAU_CH_AUX(n) (20 + (n))

/* Onset flags. */
#define YAU_ONSET_PENDING    0x001u /* not complete yet                      */
#define YAU_ONSET_LATE       0x002u /* target before the first reachable frame */
#define YAU_ONSET_CANCELED   0x004u /* never started                          */
#define YAU_ONSET_STOPPED    0x008u /* ended early by a stop or a cancel      */
#define YAU_ONSET_XRUN       0x010u /* an underrun while it played or waited  */
#define YAU_ONSET_CLIPPED    0x020u /* the mix clipped while it played        */
#define YAU_ONSET_UNCONFIRMED 0x040u /* no report confirmed the frame played */
#define YAU_ONSET_BELOW_TIER 0x080u /* tier worse than desc.min_tier          */
#define YAU_ONSET_LOST       0x100u /* the device went away                   */
#define YAU_ONSET_GAP        0x200u /* a stream played silence for missing
                                       * samples                               */

/* Ring record kinds under YRT_SRC_AUDIO, and the tier word's fields. */
#define YAU_EV_ONSET 1u
#define YAU_EV_END   2u
#define YAU_EV_FIT   3u
#define YAU_EV_XRUN  4u
#define YAU_EV_OPEN  5u
#define YAU_EV_CLAIM 6u   /* the OS LATENCY CLAIMS, at open           */
#define YAU_EV_STREAM 7u  /* a stream's start: its origin              */
#define YAU_EV_GAP   8u   /* a stream's run of missing samples         */
#define YAU_EV_TIER_OF(w)   ((unsigned)(w) & 0x7u)
#define YAU_EV_DEVICE_OF(w) (((unsigned)(w) >> 3) & 0x7u)
#define YAU_EV_GEN_OF(w)    ((unsigned)(w) >> 6)

/* Device sample formats (yau_device_caps.out). */
#define YAU_OUT_F32 1
#define YAU_OUT_S16 2
#define YAU_OUT_S24 3   /* packed, 3 bytes                                 */
#define YAU_OUT_S32 4   /* 32-bit containers, 24 or 32 valid bits          */

/* Position sources. */
#define YAU_POS_CALLBACK 0  /* callback entry times only                  */
#define YAU_POS_DEVICE   1  /* OS position reports with their own stamps   */

#define YAU_TICK_XRUN 0x1u  /* yau_tick.flags                            */

#define YAU_GAUSS   0
#define YAU_UNIFORM 1

/* yau_stream_info.state */
#define YAU_STREAM_IDLE    0   /* fill it, play it                         */
#define YAU_STREAM_QUEUED  1   /* yau_play() took it; the callback has not */
#define YAU_STREAM_WAITING 2   /* the callback has it; not started yet      */
#define YAU_STREAM_PLAYING 3
#define YAU_STREAM_ENDED   4   /* reset it before the next play             */

/* yau_wav_info.format */
#define YAU_WAV_S16    1
#define YAU_WAV_S24    2       /* packed, 3 bytes                           */
#define YAU_WAV_S24_32 3       /* 24 valid bits in 32 (EXTENSIBLE)          */
#define YAU_WAV_F32    4

typedef int64_t yau_id;
typedef struct yau_stream yau_stream;

typedef enum yau_sample {
    YAU_F32 = 0, YAU_S16 = 1, YAU_S24 = 2
} yau_sample;

typedef enum yau_backend {
    YAU_BACKEND_AUTO = 0,
    YAU_BACKEND_WASAPI,
    YAU_BACKEND_COREAUDIO,
    YAU_BACKEND_ALSA,
    YAU_BACKEND_PULSE,
    YAU_BACKEND_WEB,
    YAU_BACKEND_NULL,
    YAU_BACKEND_CUSTOM
} yau_backend;

typedef enum yau_tier {
    YAU_TIER_UNKNOWN = 0,
    YAU_TIER_1       = 1,  /* an OS observation of the frame at the output,
                              * verified: no v0.1 path                      */
    YAU_TIER_2       = 2,  /* fit time, confirmed played, on a loopback-
                              * checked path, under stated conditions       */
    YAU_TIER_3       = 3,  /* unconfirmed, or an unchecked path           */
    YAU_TIER_SIM     = 4,  /* no hardware                                  */
    YAU_TIER_WEB     = 5   /* a browser's estimate (v0.4.1; BACKENDS,
                              * WEB): the lowest, below SIM, as
                              * ysp/screen.h's YSCR_TIER_WEB              */
} yau_tier;

typedef struct yau_format {
    uint32_t       rate;        /* Hz; 0 = 48000                             */
    uint16_t       channels;    /* 0 = 2                                     */
    uint16_t       sample;      /* yau_sample; 0 = YAU_F32               */
    const uint8_t* map;         /* YAU_CH_* per channel; NULL = the device's
                                 * own order (reported, not checked)        */
} yau_format;

/* Interleaved float frames at the project rate. The memory is yours (or
 * the arena's); it must not change or move while a sound plays it. */
typedef struct yau_buf {
    float*   frames;
    int64_t  n;                 /* frames                                    */
    uint16_t channels;          /* 1, or the project's count                 */
    uint16_t reserved_;
    uint32_t id;                /* your buffer id, in every record           */
} yau_buf;

typedef struct yau_play_desc {
    yau_buf buf;
    int64_t   at_ns;            /* target, ysp_rt; 0 = as soon as it can     */
    float     db;               /* gain; 0 = unity                           */
    uint64_t  channels;         /* output mask; 0 = all                      */
    int64_t   offset;           /* first frame of buf                        */
    int64_t   frames;           /* frames per pass; 0 = to the end           */
    int32_t   loops;            /* passes after the first; YAU_FOREVER     */
    yau_stream* stream;       /* play this instead of buf (STREAMS); buf,
                                 * offset, frames and loops must then be 0  */
} yau_play_desc;

/* The record of one sound. */
typedef struct yau_onset {
    yau_id id;
    int64_t  target;            /* desc.at_ns                                */
    int64_t  onset;             /* the device-clock fit's time for start_frame,
                                 * ysp_rt ns (+ onset_offset_ns); a model
                                 * value, never an observed time         */
    int64_t  residual;          /* onset - target                            */
    int64_t  start_frame;       /* stream frame of the first sample; -1 = not
                                 * started                                  */
    int64_t  end_frame;         /* stream frame after the last; 0 = playing  */
    int64_t  device_pos;        /* the report that confirmed it, or -1       */
    int64_t  rendered_at;       /* ysp_rt ns of the callback that rendered
                                 * start_frame                              */
    uint32_t buffer_id;
    uint16_t flags;             /* YAU_ONSET_*                             */
    uint8_t  tier;              /* yau_tier                                */
    uint8_t  reserved_;
    int64_t  sample;            /* the source's sample on start_frame: the
                                 * stream's index, or the buffer offset     */
    int64_t  gap_frames;        /* a stream's silent frames (GAPS); final
                                 * once end_frame is set                    */
} yau_onset;

/* --- device and source interfaces (the extensions) ---------------------- */

typedef struct yau_tick {
    int64_t  t_entry;           /* ysp_rt ns at callback entry               */
    int64_t  pos;               /* a position report, stream frames; -1 = none */
    int64_t  pos_t;             /* its ysp_rt time                           */
    uint32_t flags;             /* YAU_TICK_XRUN                           */
    uint32_t reserved_;
} yau_tick;

typedef void (*yau_render_fn)(void* host, void* out, int32_t frames, const yau_tick* tick);

typedef struct yau_device_open {
    const struct yau_format* format;  /* the project format                */
    int32_t     period;         /* frames asked for; 0 = the device's choice */
    bool        exclusive;
    const char* device;         /* desc.device                               */
    int32_t     queue;          /* desc.queue                                */
} yau_device_open;

typedef struct yau_device_caps {
    uint32_t rate;
    uint16_t channels;
    uint16_t out;               /* YAU_OUT_*                               */
    uint8_t  map[YAU_MAX_CHANNELS];
    int32_t  period;            /* frames per callback, nominal              */
    int32_t  buffer;            /* frames the device holds                   */
    int32_t  pos_source;        /* YAU_POS_*                               */
    int32_t  tier;              /* the tier of a confirmed onset; 0 = 3      */
    int32_t  bits;              /* valid bits per sample; 0 = from out       */
    bool     exclusive;
    bool     low_latency;
    char     name[64];
    /* What the OS says, never added to an onset (OS LATENCY CLAIMS). An
     * empty os_latency_src means the device makes no claim. */
    int64_t  os_latency_ns;        /* from the position point to the output   */
    int64_t  os_stream_latency_ns; /* the API's own latency figure for the
                                    * stream; -1 = none                       */
    char     os_latency_src[32];   /* the API the claim comes from            */
    int32_t  queue;             /* periods kept ahead of the device; 0 = the
                                 * backend's own                             */
} yau_device_caps;

#define YAU_DEVICE_VERSION 1
typedef struct yau_device {
    uint32_t    version;        /* YAU_DEVICE_VERSION                      */
    const char* name;
    int  (*open)(void* ctx, const yau_device_open* in, yau_device_caps* caps,
                 char* err, size_t err_cap);
    int  (*start)(void* ctx, yau_render_fn render, void* host);
    void (*stop)(void* ctx);
    void (*close)(void* ctx);
    int  (*describe)(void* ctx, char* buf, size_t cap);   /* may be NULL     */
} yau_device;

typedef struct yau_clock {
    uint32_t rate;
    int64_t  frame;             /* stream frame of out[0]                    */
    int64_t  t_frame;           /* its ysp_rt time, from the fit             */
    double   ns_per_frame;      /* the fit's k                               */
} yau_clock;

#define YAU_SOURCE_VERSION 1
typedef struct yau_source {
    uint32_t    version;
    const char* name;
    /* Add `frames` frames into out (float, project channels, interleaved). */
    void (*render)(void* ctx, float* out, int32_t frames, const yau_clock* clk);
} yau_source;

/* --- open ---------------------------------------------------------------- */

typedef struct yau_desc {
    yau_format         format;
    yau_backend        backend;       /* 0 = AUTO                          */
    const char*          device;        /* name substring; NULL = OS default */
    bool                 exclusive;     /* WASAPI exclusive mode             */
    int32_t              period;        /* frames; 0 = 10 ms                 */
    int32_t              voices;        /* 0 = 32; at most YAU_MAX_VOICES  */
    yrt_ring*          ring;          /* records; NULL = none              */
    uint32_t             device_index;  /* 0..7, in every record             */
    int32_t              min_tier;      /* 0 = off; else flag worse onsets   */
    int64_t              onset_offset_ns;  /* from a line-in test; 0         */
    void*                arena;         /* synthesis memory; NULL = malloc   */
    size_t               arena_bytes;   /* 0 = 8 MB                          */
    const yau_source*  source;
    void*                source_ctx;
    const yau_device*  dev;           /* YAU_BACKEND_CUSTOM              */
    void*                dev_ctx;
    int32_t              queue;         /* WASAPI shared: periods queued
                                         * ahead of the engine, 1 or 2;
                                         * 0 = 2 (QUEUE)                    */
} yau_desc;

typedef struct yau_caps {
    yau_backend backend;
    uint32_t   rate;
    uint16_t   channels;
    uint16_t   out;             /* YAU_OUT_*                               */
    int32_t    period, buffer;  /* frames                                    */
    bool       exclusive;
    bool       low_latency;     /* WASAPI low-latency shared mode            */
    int32_t    pos_source;      /* YAU_POS_*                               */
    double     drift_ppm;       /* device rate against the ysp_rt clock      */
    double     fit_spread_ns;
    uint32_t   fit_points;
    int64_t    lead_ns;         /* yau_lead_ns() now                       */
    uint32_t   xruns;
    yau_tier worst_tier;
    uint8_t    map[YAU_MAX_CHANNELS];
    char       name[64];
    int64_t    os_latency_ns;   /* the OS's claim from the position point to
                                 * the output; -1 = none. Never in an onset */
    int64_t    os_stream_latency_ns;  /* the API's latency figure; -1 = none  */
    char       os_latency_src[32];    /* the API both come from             */
    uint32_t   gaps;            /* stream GAPS since open, every stream      */
    int32_t    queue;           /* periods queued ahead of the device; 0 = the
                                 * backend's own (QUEUE)                     */
} yau_caps;

typedef struct yau_param {
    const char* name;
    const char* type;           /* "u32", "i32", "i64", "f64", "bool", "enum" */
    double      min, max;
    double      def;            /* the value a zero field means              */
    const char* unit;
    const char* doc;
} yau_param;

typedef struct yau_tone_desc {
    double   hz;                /* required, below half the rate             */
    int64_t  dur_ns;            /* required; rounded to a frame              */
    float    peak;              /* required, (0, 1]                          */
    int64_t  ramp_ns;           /* raised-cosine on and off, each; 0 = none  */
    double   phase;             /* radians at frame 0                        */
    uint16_t channels;          /* 0 = 1                                     */
} yau_tone_desc;

typedef struct yau_noise_desc {
    int64_t  dur_ns;            /* required                                  */
    float    rms;               /* required; noise that would clip is refused */
    int64_t  ramp_ns;
    uint64_t seed;
    int32_t  dist;              /* YAU_GAUSS (0) or YAU_UNIFORM          */
    uint16_t channels;          /* 0 = 1; each channel its own noise         */
} yau_noise_desc;

typedef struct yau_click_desc {
    int64_t  dur_ns;            /* 0 = one frame                             */
    float    peak;              /* required, [-1, 1], not 0                  */
} yau_click_desc;

/* --- streams (STREAMS) ---------------------------------------------------- */

typedef struct yau_stream_desc {
    float*   memory;            /* frames x channels floats; NULL = the arena */
    int64_t  frames;            /* ring capacity; 0 = 1 s; at least 2 periods */
    uint16_t channels;          /* 1 or the project's; 0 = the project's     */
    uint32_t id;                /* in every record, as yau_buf.id          */
    int64_t  first;             /* index of the first sample written; 0      */
    int64_t  preroll;           /* frames a play at 0 waits for; 0 = a quarter
                                 * of the ring; YAU_PREROLL_NONE = none    */
} yau_stream_desc;

/* Caller-allocated (static, or inside your own struct), set up by
 * yau_stream_open(); every field is private. One cache line per writer:
 * the producer's, the device thread's, the read-mostly setup, and the
 * device thread's private state, so no line is written by two threads. */
struct yau_stream {
    /* --- producer --- */
    int64_t  w_;                /* samples committed: the index of the next  */
    int64_t  end_;              /* the index after the last sample; -1 = open */
    unsigned char pad0_[48];
    /* --- device thread (and the frame thread's CAS on state_) --- */
    int64_t  r_;                /* the next sample the ring holds; <= w_     */
    int64_t  next_;             /* the sample the next block plays           */
    int64_t  origin_;           /* stream frame of sample 0, once started    */
    int64_t  gap_frames_;
    int64_t  discarded_;
    int64_t  plan_;             /* the planned start frame; -1 = none yet    */
    uint32_t state_;            /* YAU_STREAM_*                            */
    uint32_t gaps_;
    unsigned char pad1_[8];
    /* --- set at init and reset --- */
    float*   mem_;
    int64_t  cap_;
    int64_t  first_;
    int64_t  preroll_;
    struct yau_audio* au_;
    yau_id id_;               /* the current play                          */
    uint32_t buf_id_;
    uint16_t ch_;
    uint16_t in_arena_;
    unsigned char pad2_[8];
    /* --- device thread only --- */
    int64_t  skip_;             /* samples a late start skipped              */
    int64_t  gap_f0_, gap_n_, gap_s0_;
    uint32_t gap_xr_;           /* the underrun count when the gap opened    */
    int32_t  gap_open_;
    unsigned char pad3_[24];
};

typedef struct yau_stream_info {
    int32_t  state;             /* YAU_STREAM_*                            */
    bool     started;           /* origin is fixed                           */
    bool     ready;             /* fill >= the preroll, or the end is in     */
    yau_id id;                /* the play; 0 = none                        */
    int64_t  first;             /* the first sample since init or reset      */
    int64_t  written;           /* the next sample the producer writes       */
    int64_t  read;              /* the next sample the ring holds            */
    int64_t  fill;              /* written - read, frames                    */
    int64_t  preroll;           /* the frames `ready` asks for               */
    int64_t  next;              /* the sample the device's next block plays  */
    int64_t  start_frame;       /* planned or fixed start; -1 = not planned  */
    int64_t  frames_to_start;   /* start_frame minus the next block's frame;
                                 * <= 0 once started                         */
    int64_t  origin;            /* stream frame of sample 0, once started    */
    int64_t  end;               /* the index after the last sample; -1 = open */
    uint32_t gaps;              /* runs of missing samples in this play      */
    int64_t  gap_frames;        /* silent frames in this play                */
    int64_t  discarded;         /* samples that came after their frame       */
} yau_stream_info;

/* --- WAV files (WAV FILES) ------------------------------------------------- */

/* The same layout as ysp/video.h's yvid_reader. */
typedef struct yau_reader {
    int64_t (*read)(void* ctx, int64_t offset, void* dst, int64_t n);  /* bytes, or < 0 */
    int64_t size;               /* bytes                                     */
} yau_reader;

typedef struct yau_wav_desc {
    const char*         path;   /* a file; or                                */
    const void*         data;   /* the file in memory, kept by you; or       */
    size_t              size;
    const yau_reader* reader; /* bytes by range (a pack entry)             */
    void*               reader_ctx;
    int32_t             loops;  /* passes after the first; YAU_FOREVER      */
    int64_t             start;  /* the first sample to stream; 0             */
    int64_t             ring;   /* ring frames; 0 = 1 s                      */
    float*              memory; /* ring memory; NULL = the arena             */
    uint32_t            id;     /* in every record                           */
} yau_wav_desc;

typedef struct yau_wav_info {
    uint32_t rate;
    uint16_t channels;
    uint16_t format;            /* YAU_WAV_*                               */
    int64_t  frames;            /* one pass                                  */
    int64_t  data_offset;       /* bytes                                     */
    uint32_t channel_mask;      /* EXTENSIBLE's; 0 = none                    */
    bool     rf64;              /* RF64 or BW64 sizes from ds64              */
} yau_wav_info;

#define YAU__WAV_SCRATCH 16384  /* bytes of raw samples per read            */

typedef struct yau__wsrc {      /* private: where a WAV file's bytes come from */
    void*               fh;     /* FILE*                                     */
    const unsigned char* data;
    yau_reader        reader;
    void*               ctx;
    int64_t             size;
} yau__wsrc;

/* Caller-allocated and zeroed, like the handle; about 17 KB. */
typedef struct yau_wav {
    yau_stream   stream;      /* play this                                 */
    yau_wav_info info;
    int32_t        error_code;  /* the YAU_ERR_* of a failed open, else 0    */
    /* private */
    yau__wsrc    src_;
    int32_t        loops_;
    int32_t        open_;
    int64_t        pos_;        /* the next sample to write                  */
    int64_t        total_;      /* samples in all passes; -1 = forever        */
    int64_t        seek_;       /* a seek waiting for the play to end; -1    */
    struct yau_audio* au_;
    char           error_[256];
    unsigned char  scratch_[YAU__WAV_SCRATCH];
} yau_wav;

typedef struct yau_wav_msg { int64_t seek; } yau_wav_msg;   /* -1 = wake only */

/* --- private state, in the handle ---------------------------------------- */

#define YAU__CMDS      256
#define YAU__MSGS      256
#define YAU__RESULTS   256
#define YAU__FIT_PTS   1024    /* 205 s of reports, 51 s of callbacks     */
#define YAU__ACC       4096    /* floats in the mix accumulator           */
#define YAU__BACKEND_WORDS 1536 /* 12 KB for miniaudio's context and device */
#define YAU__STREAM_VOICE  2    /* yau__voice.in_arena of a stream       */

typedef struct yau__fit {
    int64_t  w0, t0;            /* t = t0 + (w - w0) * k                     */
    double   k;
    double   spread;
    uint32_t n, gen;
    int32_t  ready, source;
} yau__fit;

typedef struct yau__cmd {
    int32_t  op;
    int32_t  loops;
    yau_id id;
    int64_t  t, ramp;
    float    db;
    uint16_t ch;
    uint16_t in_arena;
    uint32_t buf_id;
    float*   frames;
    yau_stream* st;
    int64_t  first, len;
    uint64_t mask;
} yau__cmd;

typedef struct yau__msg {
    int32_t     kind;
    int32_t     freed;          /* the voice slot is free again              */
    int32_t     in_arena;
    int32_t     reserved_;
    yau_onset rec;
    yau__fit  fit;
    int64_t     a, b;
} yau__msg;

typedef struct yau__voice {
    yau_id id;
    int32_t  state;             /* 0 free, 1 waiting, 2 playing, 3 ended     */
    int32_t  onset_done;
    /* A stream has no buffer, so it takes the buffer's slot and the voice
     * stays 200 bytes: the voice array is the hot data of every block. */
    union { float* f; yau_stream* st; } src;
    int64_t  first, len, in_loop;
    int32_t  loops_left;
    uint16_t ch, in_arena;      /* in_arena: 0, 1, or YAU__STREAM_VOICE    */
    uint64_t mask;
    int64_t  target, start, end, played, rendered_at;
    float    gain;
    float    g_from, g_to;
    int32_t  g_state;           /* 0 none, 1 waiting for its time, 2 ramping */
    int64_t  g_t, g_ramp, g_start, g_len;
    int32_t  s_state;           /* 0 none, 1 set                             */
    int64_t  s_t, s_ramp, s_start, s_end;
    uint32_t buf_id;
    uint16_t flags;
    uint16_t gen;
} yau__voice;

typedef struct yau__res {
    yau_onset rec;
    int32_t     used;           /* 1 waiting for the onset, 2 onset known    */
    int32_t     ended;
    uint16_t    end_flags;
} yau__res;

/* The handle. Caller-allocated and zeroed; every field is private. */
typedef struct yau_audio {
    /* --- frame thread to callback --- */
    uint32_t       cmd_head;
    unsigned char  pad0_[60];
    uint32_t       cmd_tail;
    unsigned char  pad1_[60];
    /* --- callback to frame thread --- */
    uint32_t       msg_head;
    unsigned char  pad2_[60];
    uint32_t       msg_tail;
    unsigned char  pad3_[60];
    int64_t        w_pub;       /* the stream frame after the last block      */
    uint32_t       lost;        /* the device went away                       */
    uint32_t       msgs_dropped;
    uint32_t       gaps_pub;    /* stream GAPS since open                     */
    unsigned char  pad4_[44];
    yau__cmd     cmd[YAU__CMDS];
    yau__msg     msg[YAU__MSGS];
    /* --- frame thread --- */
    int            open;
    int            started;
    uint32_t       err_n;       /* messages written: a call's own beats the
                                 * generic one (ERRORS)                    */
    char           error[512];
    yau_desc     desc;
    yau_format   fmt;
    yau_device_caps dcaps;
    const yau_device* dev;
    void*          dev_ctx;
    yau_id       next_id;
    int32_t        voices_max;
    int32_t        outstanding;
    int32_t        arena_live;
    int32_t        worst_tier_ft;
    uint32_t       xruns_ft;
    yau__fit     fit_ft;
    yau__res     res[YAU__RESULTS];
    unsigned char* arena;
    size_t         arena_cap, arena_used;
    int            arena_owned;
    int32_t        arena_streams;  /* streams whose ring is in the arena     */
    /* --- callback (device thread) --- */
    int64_t        w;           /* stream frame of the next block             */
    int64_t        p_off;       /* device position minus stream frame         */
    int64_t        last_entry;
    int64_t        last_clock_t;
    uint32_t       tid;
    uint32_t       xruns;
    int32_t        worst_tier;
    int            first_block;
    yau__fit     fit;
    int64_t        pt_w[YAU__FIT_PTS];
    int64_t        pt_t[YAU__FIT_PTS];
    int32_t        pt_head, pt_n;
    int32_t        hull[YAU__FIT_PTS];
    int            b_open;
    int64_t        b_w0, b_t0, b_bw, b_bt;
    double         b_best;
    int64_t        last_pos;
    int32_t        early_run;   /* reports in a row far before the fit      */
    int64_t        early_since;
    int32_t        late_run;    /* reports in a row far after the fit (HALT) */
    int64_t        late_w0;
    int32_t        late_again;  /* the run came right after an early report */
    double         late_min;
    int64_t        last_fit_t;
    int32_t        clip_any;
    uint32_t       gaps;
    yau__voice   voice[YAU_MAX_VOICES];
    float          acc[YAU__ACC];
    /* --- backend --- */
    uint64_t       backend_mem[YAU__BACKEND_WORDS];
} yau_audio;

/* --- API ----------------------------------------------------------------- */

/* YAU_VERSION_STRING of the implementation that was compiled. */
YAU_API const char* yau_version(void);

/* Static text for a YAU_* code ("ok" for 0 and other positive values). */
YAU_API const char* yau_strerror(int code);

/* Opens the device in the project format or fails (STRICT OPEN), starts it
 * silent, and waits until the fit spans 0.5 s (at most 3 s; not for a
 * CUSTOM device, whose render calls are the caller's). The handle must be
 * zeroed or closed. On false, yau_error() says why. */
YAU_API bool        yau_open(yau_audio* au, const yau_desc* desc);

/* Stops the device, completes every record (sounds that never played are
 * CANCELED, onsets not yet confirmed UNCONFIRMED), and frees the arena when
 * open() allocated it. Safe on a closed or zeroed handle. */
YAU_API void        yau_close(yau_audio* au);

/* The message of the last failure (ERRORS); "" when there is none. */
YAU_API const char* yau_error(const yau_audio* au);
YAU_API bool        yau_is_open(const yau_audio* au);
YAU_API void        yau_get_caps(const yau_audio* au, yau_caps* out);

/* One line for the log: backend, device, format, period, buffer, share
 * mode, position source, drift, fit spread, lead, underruns, worst tier.
 * Returns snprintf's count. */
YAU_API int         yau_describe(const yau_audio* au, char* buf, size_t cap);

/* Queue a sound (PLAYING). Returns its id (> 0) or a negative code. */
YAU_API yau_id yau_play_at(yau_audio* au, yau_buf buf, int64_t t_ns);
YAU_API yau_id yau_play(yau_audio* au, const yau_play_desc* d);
YAU_API int  yau_cancel(yau_audio* au, yau_id id);
YAU_API int  yau_stop_at(yau_audio* au, yau_id id, int64_t t_ns, int64_t ramp_ns);
YAU_API int  yau_gain_at(yau_audio* au, yau_id id, int64_t t_ns, float db, int64_t ramp_ns);

/* Moves completed records from the callback into the handle. Returns the
 * number moved, or YAU_ERR_LOST once the device has gone away. */
YAU_API int  yau_update(yau_audio* au);

/* The record of sound `id` into *out: YAU_OK when its onset is known
 * (end_frame stays 0 while it plays), YAU_PENDING before, with the plan
 * in *out, YAU_ERR_NOT_FOUND for an id that is not among the last 256. */
YAU_API int  yau_result(yau_audio* au, yau_id id, yau_onset* out);

/* yau_result() until it is not PENDING, for at most timeout_ns
 * (YAU_ERR_TIMEOUT). Sleeps on the ysp_rt clock in 1 ms steps. */
YAU_API int  yau_wait(yau_audio* au, yau_id id, int64_t timeout_ns, yau_onset* out);

/* The shortest lead a sound handed over now can still meet (LEAD); 0 before
 * the first callback. */
YAU_API int64_t yau_lead_ns(const yau_audio* au);

/* The fit (THE FIT): the stream frame on which time t_ns falls (the
 * nearest), and the time of a stream frame. -1 before the first fit. */
YAU_API int64_t yau_frame_at(const yau_audio* au, int64_t t_ns);
YAU_API int64_t yau_time_of(const yau_audio* au, int64_t frame);

/* The device side of the DEVICE contract. */
YAU_API void yau_render(void* host, void* out, int32_t frames, const yau_tick* tick);
YAU_API void yau_rt_thread_init(void* host);

/* Synthesis (SYNTHESIS). The handle forms return a buffer from the arena,
 * with frames NULL and a message in yau_error() on failure. */
YAU_API float     yau_db(float db);
YAU_API yau_buf yau_tone (yau_audio* au, const yau_tone_desc* d);
YAU_API yau_buf yau_noise(yau_audio* au, const yau_noise_desc* d);
YAU_API yau_buf yau_click(yau_audio* au, const yau_click_desc* d);
YAU_API yau_buf yau_alloc(yau_audio* au, int64_t n, uint16_t channels);
YAU_API int       yau_arena_reset(yau_audio* au);

/* The same into your memory: n frames of ch channels at `rate`. Return 0 or
 * YAU_ERR_ARG; yau_fill_noise returns YAU_ERR_FORMAT when the noise
 * would clip, with the buffer's peak in *peak (may be NULL). */
YAU_API int yau_fill_tone (float* out, int64_t n, uint16_t ch, uint32_t rate,
                               const yau_tone_desc* d);
YAU_API int yau_fill_noise(float* out, int64_t n, uint16_t ch, uint32_t rate,
                               const yau_noise_desc* d, float* peak);
/* Raised-cosine onset and offset ramps over on and off frames, in place. */
YAU_API int yau_ramp(float* f, int64_t n, uint16_t ch, int64_t on_frames, int64_t off_frames);
/* dst[at_frame ...] += gain * src, both with ch channels, clipped to dst. */
YAU_API int yau_mix(float* dst, int64_t dst_n, uint16_t ch, const float* src,
                        int64_t src_n, int64_t at_frame, float gain);

/* Streams (STREAMS). Frame thread. open takes the ring from desc.memory
 * or the arena; false with the message in the owner's yau_error(au) (the
 * stream has no buffer of its own: it is laid out a cache line per
 * thread). close never fails: a play still running is canceled and close
 * waits for the device to let go of the stream (STREAMS, "Close"), then
 * ends an arena stream's hold on yau_arena_reset(). Safe on a zeroed
 * stream, after a failed open, and twice. */
YAU_API bool yau_stream_open(yau_audio* au, yau_stream* st, const yau_stream_desc* d);
YAU_API void yau_stream_close(yau_audio* au, yau_stream* st);
/* yau_play() with .stream = st and .at_ns = t_ns. */
YAU_API yau_id yau_play_stream(yau_audio* au, yau_stream* st, int64_t t_ns);

/* The producer: one thread at a time, which may be the frame thread.
 * Wait-free. acquire returns the contiguous free region (up to the ring's
 * wrap) and its size in *frames, or NULL when the ring is full; commit
 * publishes the first `frames` of it. write copies and returns the frames
 * it took. end: no sample follows the last one committed. reset empties the
 * ring and numbers the next sample `first` (YAU_ERR_BUSY unless IDLE or
 * ENDED). */
YAU_API float*  yau_stream_acquire(yau_stream* st, int64_t* frames);
YAU_API int     yau_stream_commit(yau_stream* st, int64_t frames);
YAU_API int64_t yau_stream_write(yau_stream* st, const float* src, int64_t frames);
YAU_API int     yau_stream_end(yau_stream* st);
YAU_API int     yau_stream_reset(yau_stream* st, int64_t first);

/* Any thread. Each field is current when read; the set is not one
 * snapshot. */
YAU_API int yau_stream_get_info(const yau_stream* st, yau_stream_info* out);

/* Frame thread: the sample of a started stream at ysp_rt time t_ns (the
 * nearest frame minus origin), and the time of a sample, from the fit.
 * YAU_PENDING until the start fixes origin. Not const: a failure writes
 * the message. */
YAU_API int yau_stream_sample_at(yau_audio* au, const yau_stream* st, int64_t t_ns, int64_t* sample);
YAU_API int yau_stream_time_of(yau_audio* au, const yau_stream* st, int64_t sample, int64_t* t_ns);

/* WAV files (WAV FILES). open and close: frame thread; open returns false
 * with the message in yau_wav_error(w) and the code in w->error_code;
 * close never fails, as yau_stream_close(). feed, seek, step and on_msg:
 * the producer; they return codes, and feed writes yau_wav_error(w). wants: any thread. feed writes up to
 * max_frames (0 = until the ring is full or the file ends) and returns the
 * frames written or a negative code. step is a yrt_idle_fn (one block of
 * up to 4096 frames; true while the ring has room and the file has
 * samples); on_msg is a yrt_msg_fn for yau_wav_msg (a seek, or -1 to
 * wake). load reads a whole file into the arena as a buffer. */
YAU_API bool      yau_wav_open(yau_audio* au, yau_wav* w, const yau_wav_desc* d);
YAU_API void      yau_wav_close(yau_audio* au, yau_wav* w);
YAU_API int64_t   yau_wav_feed(yau_wav* w, int64_t max_frames);
YAU_API int       yau_wav_seek(yau_wav* w, int64_t sample);
YAU_API bool      yau_wav_wants(const yau_wav* w);
YAU_API bool      yau_wav_step(void* w);
YAU_API void      yau_wav_on_msg(void* w, const void* msg, uint32_t seq);
YAU_API const char* yau_wav_error(const yau_wav* w);
YAU_API yau_buf yau_wav_load(yau_audio* au, const yau_wav_desc* d);
/* v0.2.1: the WAV file's form without a device (the pack tool): the same
 * parse and refusals as yau_wav_open(), except the rate, the channel count
 * and the speaker map, which info gives for the caller to compare. 0 or a
 * negative code with the message in err. No allocation; a path is opened
 * and closed. */
YAU_API int       yau_wav_probe(const yau_wav_desc* d, yau_wav_info* info, char* err, size_t cap);

/* The table of desc fields a designer sets; *n gets the count. */
YAU_API const yau_param* yau_params(int* n);

#ifdef __cplusplus
}
#endif

#endif /* YSP_AUDIO_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_AUDIO_IMPLEMENTATION
#ifndef YSP_AUDIO_IMPLEMENTATION_GUARD
#define YSP_AUDIO_IMPLEMENTATION_GUARD

#ifndef YSP_RT_IMPLEMENTATION_GUARD
    #define YSP_RT_IMPLEMENTATION
    #include "ysp/rt.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#if !defined(YAU_NO_MINIAUDIO)
    #if defined(_WIN32)
        #ifndef WIN32_LEAN_AND_MEAN
            #define WIN32_LEAN_AND_MEAN
        #endif
        #include <windows.h>
    #endif
    #if !defined(YAU_MINIAUDIO_EXTERNAL) && !defined(MINIAUDIO_IMPLEMENTATION)
        #define MINIAUDIO_IMPLEMENTATION
        #define YAU__MA_CUTS 1
        #define MA_NO_DECODING
        #define MA_NO_ENCODING
        #define MA_NO_ENGINE
        #define MA_NO_RESOURCE_MANAGER
        #define MA_NO_NODE_GRAPH
        #define MA_NO_GENERATION
    #endif
    /* miniaudio's Web Audio backend is JavaScript inside EM_ASM, which
     * clang's pedantic warnings reject; they are about that JavaScript */
    #if defined(__clang__)
        #pragma clang diagnostic push
        /* first: older clang lacks some of the groups below, and must not
         * fail on their names */
        #pragma clang diagnostic ignored "-Wunknown-warning-option"
        #pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
        #pragma clang diagnostic ignored "-Wstrict-prototypes"
        #pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
        #pragma clang diagnostic ignored "-Wvariadic-macro-arguments-omitted"
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    #endif
    #ifdef YAU_MINIAUDIO_HEADER
        #include YAU_MINIAUDIO_HEADER
    #else
        #include "miniaudio.h"
    #endif
    #if defined(__clang__)
        #pragma clang diagnostic pop
    #endif
    /* The strict open reads ma_device fields that are public but not API
     * (playback.internal*, playback.converter.isPassthrough,
     * wasapi.pAudioClientPlayback), so the minor version is pinned. */
    #if MA_VERSION_MAJOR != 0 || MA_VERSION_MINOR != 11 || MA_VERSION_REVISION < 25
        #error "ysp_audio: needs miniaudio 0.11.25 or a later 0.11 release"
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Test-only seam, not API: tests/adapt/audio_test.c defines these to
 * run the frame-thread side on a virtual clock. */
#ifndef YAU__NOW
#define YAU__NOW() (yrt_now_ns())
#endif
#if defined(MA_HAS_WEBAUDIO) && !defined(YAU__SLEEP_UNTIL)
#include <emscripten/threading.h>
/* A wait on a browser's main thread must yield to it: without threads the
 * ScriptProcessorNode's callbacks run there, and with them the page's timer
 * that reads getOutputTimestamp() does (BACKENDS, WEB). So the main thread
 * sleeps through ASYNCIFY or JSPI, which ysp/screen.h's web backend needs
 * too, and miniaudio's AudioWorklet path itself. */
static void yau__web_sleep_until(int64_t t) {
    int64_t d = t - yrt_now_ns();
    if (!emscripten_is_main_runtime_thread()) { yrt_sleep_until(t, 0); return; }
    emscripten_sleep(d > 1000000 ? (unsigned)(d / 1000000) : 1u);
}
#define YAU__SLEEP_UNTIL(t) yau__web_sleep_until((int64_t)(t))
#endif
#ifndef YAU__SLEEP_UNTIL
#define YAU__SLEEP_UNTIL(t) yrt_sleep_until((int64_t)(t), 0)
#endif
/* Measurement seam, not API: examples/audio/clockstats.c records every
 * tick and the cost of every render through these. */
#ifndef YAU__ON_TICK
#define YAU__ON_TICK(au, tk) ((void)0)
#endif
#ifndef YAU__ON_RENDER_END
#define YAU__ON_RENDER_END(au) ((void)0)
#endif


#define YAU__BUCKET_NS  50000000LL    /* a fit point per 50 ms of callbacks */
#define YAU__BUCKET_POS_NS 200000000LL /* and per 200 ms of reports         */
#define YAU__REFIT_NS 1000000000LL    /* refit once a second                */
#define YAU__WARM_NS   500000000LL    /* open() waits for this much fit     */
#define YAU__SLOPE_NS  5000000000LL   /* fit the slope from this span on    */
#define YAU__REANCHOR_NS 2000000000LL /* reports early this long: start over */
#define YAU__HALT_REPORTS 3           /* late reports in a row that make a halt */

enum { YAU__OP_PLAY = 1, YAU__OP_CANCEL, YAU__OP_STOP, YAU__OP_GAIN };
enum { YAU__M_ONSET = 1, YAU__M_END, YAU__M_FIT, YAU__M_XRUN, YAU__M_FREE };

/* --- atomics ---------------------------------------------------------------
 * The two queues and every stream ring are single-producer single-consumer:
 * an acquire load of the other side's index and a release store of one's
 * own is the whole synchronization. A stream's state also takes a
 * compare-and-swap, from the frame thread and the producer only, so a reset
 * and a play cannot both claim it. The same pattern as ysp/rt.h's ring: on MSVC x86/x64 a
 * volatile access has acquire or release semantics and the barrier only
 * stops the compiler; ARM64 gets the explicit instructions. */
#if defined(_MSC_VER)
    #include <intrin.h>
    #if defined(_M_ARM64)
static uint32_t yau__ld32(const uint32_t* p) { return (uint32_t)__ldar32((unsigned __int32 volatile*)(uintptr_t)p); }
static void yau__st32(uint32_t* p, uint32_t v) { __stlr32((unsigned __int32 volatile*)p, v); }
static int64_t yau__ld64(const int64_t* p) { return (int64_t)__ldar64((unsigned __int64 volatile*)(uintptr_t)p); }
static void yau__st64(int64_t* p, int64_t v) { __stlr64((unsigned __int64 volatile*)p, (unsigned __int64)v); }
    #elif defined(_M_IX86)
static uint32_t yau__ld32(const uint32_t* p) { uint32_t v = *(const volatile uint32_t*)p; _ReadWriteBarrier(); return v; }
static void yau__st32(uint32_t* p, uint32_t v) { _ReadWriteBarrier(); *(volatile uint32_t*)p = v; }
static int64_t yau__ld64(const int64_t* p) { return _InterlockedCompareExchange64((volatile __int64*)(uintptr_t)p, 0, 0); }
static void yau__st64(int64_t* p, int64_t v) { (void)_InterlockedExchange64((volatile __int64*)p, v); }
    #else
static uint32_t yau__ld32(const uint32_t* p) { uint32_t v = *(const volatile uint32_t*)p; _ReadWriteBarrier(); return v; }
static void yau__st32(uint32_t* p, uint32_t v) { _ReadWriteBarrier(); *(volatile uint32_t*)p = v; }
static int64_t yau__ld64(const int64_t* p) { int64_t v = *(const volatile int64_t*)p; _ReadWriteBarrier(); return v; }
static void yau__st64(int64_t* p, int64_t v) { _ReadWriteBarrier(); *(volatile int64_t*)p = v; }
    #endif
static int yau__cas32(uint32_t* p, uint32_t expect, uint32_t v) {
    return (uint32_t)_InterlockedCompareExchange((volatile long*)p, (long)v, (long)expect) == expect;
}
#elif defined(__GNUC__) || defined(__clang__)
static int yau__cas32(uint32_t* p, uint32_t expect, uint32_t v) {
    return __atomic_compare_exchange_n(p, &expect, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static uint32_t yau__ld32(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yau__st32(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static int64_t yau__ld64(const int64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yau__st64(int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#else
#error "ysp_audio: needs atomic loads and stores (GCC, Clang or MSVC builtins)"
#endif

/* --- small helpers ----------------------------------------------------------- */

static void yau__err(yau_audio* au, const char* fmt, ...) {
    va_list ap;
    au->err_n++;
    va_start(ap, fmt);
    vsnprintf(au->error, sizeof au->error, fmt, ap);
    va_end(ap);
}

YAU_API const char* yau_strerror(int code);

/* A frame-thread call's failure, in yau_error(), unless the call wrote a
 * more specific message itself (err_n moved). snprintf on failure only. */
static int64_t yau__fail(yau_audio* au, uint32_t n0, int64_t rc, const char* what) {
    if (rc < 0 && au && au->err_n == n0) yau__err(au, "ysp_audio: %s: %s", what, yau_strerror((int)rc));
    return rc;
}
#define YAU__N0(au) ((au) ? (au)->err_n : 0u)

static int64_t yau__frames_of(int64_t ns, uint32_t rate) {
    /* nearest frame; ns up to 2^62 does not overflow through a double */
    double f = (double)ns * (double)rate / 1e9;
    return (int64_t)floor(f + 0.5);
}

YAU_API const char* yau_version(void) { return YAU_VERSION_STRING; }

YAU_API const char* yau_strerror(int code) {
    switch (code) {
    case YAU_ERR_ARG:       return "bad argument";
    case YAU_ERR_CLOSED:    return "not open";
    case YAU_ERR_FULL:      return "queue full or every voice in use";
    case YAU_ERR_FORMAT:    return "buffer format does not fit the project";
    case YAU_ERR_NOT_FOUND: return "no such sound among the last 256";
    case YAU_ERR_LOST:      return "device lost";
    case YAU_ERR_BUSY:      return "busy: still playing, waiting or in use";
    case YAU_ERR_TIMEOUT:   return "timeout";
    case YAU_ERR_IO:        return "read error";
    case YAU_PENDING:       return "pending";
    default:                  return code < 0 ? "unknown error" : "ok";
    }
}

YAU_API float yau_db(float db) { return (float)pow(10.0, (double)db / 20.0); }

/* --- the fit (device thread) ------------------------------------------------- */

static double yau__k_nom(const yau_audio* au) { return 1e9 / (double)au->dcaps.rate; }

static int64_t yau__fit_time(const yau__fit* f, int64_t w) {
    return f->t0 + (int64_t)floor((double)(w - f->w0) * f->k + 0.5);
}

/* The nearest frame to t, a tie to the earlier: the first frame at or after
 * x - 0.5. */
static int64_t yau__fit_frame(const yau__fit* f, int64_t t) {
    double x = (double)(t - f->t0) / f->k;
    return f->w0 + (int64_t)ceil(x - 0.5);
}

static void yau__fit_point(yau_audio* au, int64_t w, int64_t t) {
    int32_t i = (au->pt_head + au->pt_n) % YAU__FIT_PTS;
    if (au->pt_n == YAU__FIT_PTS) {
        au->pt_head = (au->pt_head + 1) % YAU__FIT_PTS;
        i = (au->pt_head + au->pt_n - 1) % YAU__FIT_PTS;
    } else {
        au->pt_n++;
    }
    au->pt_w[i] = w;
    au->pt_t[i] = t;
}

/* One point per bucket, its least late: 200 ms of reports or 50 ms of
 * callbacks, a window of 1024 points (measured best of 50 to 400 ms and 256
 * to 4096 points over 10 minutes, at a fit cost the callback can pay). Position
 * reports are fitted by least squares on those points, callback times by
 * their lower envelope: the line under every point with the smallest summed
 * gap, the edge of the lower convex hull that spans the mean x (Moon,
 * Skelly and Towsley, INFOCOM 1999). That pairing is measured: on the test
 * laptop a WASAPI position is the engine's last pass stamped at the call,
 * and over 10 minutes least squares predicted the next second within 110
 * us at p99 against 357 us for the envelope; on callback times the
 * envelope won, 207 against 359 us (docs/audio.md). Until the points
 * span YAU__SLOPE_NS the slope stays nominal and only the offset is
 * fitted, from the last second, because a slope from a shorter span was
 * worse than the drift it corrects. Computed on x = frames since the
 * oldest point and y = ns since it minus x at the nominal rate, so the
 * doubles stay small. */
static void yau__refit(yau_audio* au) {
    int32_t n = au->pt_n, i, j, h = 0;
    int64_t wb, tb, span;
    double kn = yau__k_nom(au), sx = 0, a, b, xm, spread = 0;
    int32_t* hull = au->hull;
    yau__fit* f = &au->fit;
    if (n < 1) return;
    wb = au->pt_w[au->pt_head];
    tb = au->pt_t[au->pt_head];
    span = au->pt_t[(au->pt_head + n - 1) % YAU__FIT_PTS] - tb;
#define YAU__X(k) ((double)(au->pt_w[((k) + au->pt_head) % YAU__FIT_PTS] - wb))
#define YAU__Y(k) ((double)(au->pt_t[((k) + au->pt_head) % YAU__FIT_PTS] - tb) - YAU__X(k) * kn)
    for (i = 0; i < n; i++) sx += YAU__X(i);
    xm = sx / n;
    b = 0;
    a = YAU__Y(0);
    if (n < 2 || span < YAU__SLOPE_NS) {
        /* the offset from the last second only, so the drift the nominal
         * slope ignores costs at most a second's worth */
        int64_t tl = au->pt_t[(au->pt_head + n - 1) % YAU__FIT_PTS];
        double sum = 0;
        int32_t m = 0;
        a = YAU__Y(n - 1);
        for (i = 0; i < n; i++) {
            if (au->pt_t[(au->pt_head + i) % YAU__FIT_PTS] < tl - 1000000000LL) continue;
            if (YAU__Y(i) < a) a = YAU__Y(i);
            sum += YAU__Y(i); m++;
        }
        if (au->dcaps.pos_source == YAU_POS_DEVICE && m > 0) a = sum / m;
    } else if (au->dcaps.pos_source == YAU_POS_DEVICE) {
        double sxx = 0, sxy = 0, sy = 0;
        for (i = 0; i < n; i++) sy += YAU__Y(i);
        for (i = 0; i < n; i++) {
            double dx = YAU__X(i) - xm, dy = YAU__Y(i) - sy / n;
            sxx += dx * dx; sxy += dx * dy;
        }
        b = sxx > 0 ? sxy / sxx : 0;
        a = sy / n - b * xm;
    } else {
        for (i = 0; i < n; i++) {
            while (h >= 2) {
                double x1 = YAU__X(hull[h - 2]), y1 = YAU__Y(hull[h - 2]);
                double x2 = YAU__X(hull[h - 1]), y2 = YAU__Y(hull[h - 1]);
                double x3 = YAU__X(i), y3 = YAU__Y(i);
                if ((x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1) <= 0) h--; else break;
            }
            hull[h++] = i;
        }
        j = 0;
        while (j + 1 < h - 1 && YAU__X(hull[j + 1]) <= xm) j++;
        if (h >= 2 && YAU__X(hull[j + 1]) > YAU__X(hull[j])) {
            double x1 = YAU__X(hull[j]), y1 = YAU__Y(hull[j]);
            double x2 = YAU__X(hull[j + 1]), y2 = YAU__Y(hull[j + 1]);
            b = (y2 - y1) / (x2 - x1);
            a = y1 - b * x1;
        }
    }
    /* the spread: rms residual for least squares, mean gap for the envelope */
    for (i = 0; i < n; i++) {
        double r = YAU__Y(i) - a - b * YAU__X(i);
        spread += au->dcaps.pos_source == YAU_POS_DEVICE ? r * r : r;
    }
    spread = au->dcaps.pos_source == YAU_POS_DEVICE ? sqrt(spread / n) : spread / n;
    {
        double xl = YAU__X(n - 1);
        f->w0 = wb + (int64_t)xl;
        f->t0 = tb + (int64_t)floor(a + b * xl + xl * kn + 0.5);
        f->k = kn + b;
        f->spread = spread;
        f->n = (uint32_t)n;
        f->gen++;
        f->source = au->dcaps.pos_source;
        f->ready = span >= YAU__WARM_NS;
    }
#undef YAU__X
#undef YAU__Y
}

/* --- queues ------------------------------------------------------------------ */

static int yau__msg_push(yau_audio* au, const yau__msg* m) {
    uint32_t head = au->msg_head, tail = yau__ld32(&au->msg_tail);
    if (head - tail >= YAU__MSGS) { au->msgs_dropped++; return YAU_ERR_FULL; }
    au->msg[head % YAU__MSGS] = *m;
    yau__st32(&au->msg_head, head + 1);
    return 0;
}

static int yau__cmd_push(yau_audio* au, const yau__cmd* c) {
    uint32_t head = au->cmd_head, tail = yau__ld32(&au->cmd_tail);
    if (head - tail >= YAU__CMDS) return YAU_ERR_FULL;
    au->cmd[head % YAU__CMDS] = *c;
    yau__st32(&au->cmd_head, head + 1);
    return 0;
}

/* --- records (device thread) ------------------------------------------------- */

static uint16_t yau__tierword(const yau_audio* au, int tier, uint32_t gen) {
    return (uint16_t)(((unsigned)tier & 7u) | ((au->desc.device_index & 7u) << 3) | ((gen & 1023u) << 6));
}

static void yau__ring(yau_audio* au, uint16_t kind, int64_t t, uint32_t aux, const yrt_payload* u) {
    yrt_event ev;
    if (!au->desc.ring) return;
    memset(&ev, 0, sizeof ev);
    ev.t_ns = (uint64_t)(t > 0 ? t : 1);
    ev.tid = au->tid;
    ev.source = (uint16_t)YRT_SRC_AUDIO;
    ev.kind = kind;
    ev.aux = aux;
    ev.u = *u;
    (void)yrt_ring_push(au->desc.ring, &ev);
}

static int yau__path_tier(const yau_audio* au) {
    if (au->dcaps.tier == YAU_TIER_SIM) return YAU_TIER_SIM;
    return au->dcaps.tier ? au->dcaps.tier : YAU_TIER_3;
}

static void yau__finish_voice_msg(yau__voice* v, yau__msg* m) {
    m->freed = v->onset_done && v->state == 3;
    m->in_arena = v->in_arena == 1;
    if (m->freed) v->state = 0;
}

#define YAU__NO_ORIGIN INT64_MIN
#define YAU__STREAM_RESETTING 5u   /* a reset is rewriting the stream     */

static int yau__is_stream(const yau__voice* v) { return v->in_arena == YAU__STREAM_VOICE; }

/* The sample on the start frame: a buffer's offset; a stream's index, kept
 * in `first` (unused by a stream) because its ONSET may complete after the
 * play has ended and the stream has gone back to the producer. A stream's
 * silent frames are kept in `len` for the same reason. */
static int64_t yau__voice_sample(const yau__voice* v) {
    if (yau__is_stream(v) && v->start < 0) return -1;
    return v->first;
}

/* A gap's record goes out when the run of missing samples closes, so it
 * carries its full length. Only to the event ring: the counters in the
 * stream carry gaps to the frame thread, and a burst of gaps must not
 * fill the queue that carries the ONSET and END messages. */
static void yau__gap_close(yau_audio* au, yau__voice* v, yau_stream* st) {
    yrt_payload u;
    if (!st->gap_open_) return;
    st->gap_open_ = 0;
    memset(&u, 0, sizeof u);
    u.i64[0] = st->gap_f0_;
    u.i64[1] = st->gap_n_;
    u.i64[2] = st->gap_s0_;
    u.i64[3] = st->discarded_;
    u.u32[8] = v->buf_id;
    u.u16[18] = (uint16_t)(au->xruns != st->gap_xr_ ? YAU_ONSET_XRUN : 0u);
    yau__ring(au, (uint16_t)YAU_EV_GAP, yau__fit_time(&au->fit, st->gap_f0_), (uint32_t)v->id, &u);
}

/* The play is over: the device stores r before ENDED, so a reset that sees
 * ENDED never races a late store, and touches the stream no more. */
static void yau__stream_detach(yau_audio* au, yau__voice* v) {
    yau_stream* st = v->src.st;
    yau__gap_close(au, v, st);
    yau__st32(&st->state_, YAU_STREAM_ENDED);
}

static void yau__onset_done(yau_audio* au, yau__voice* v, int64_t onset, int64_t devpos, int confirmed) {
    yau__msg m;
    yrt_payload u;
    /* SIM and WEB stay what they are: a browser's onset is its estimate,
     * confirmed or not */
    int tier = au->dcaps.tier == YAU_TIER_SIM || au->dcaps.tier == YAU_TIER_WEB ? au->dcaps.tier
             : (confirmed ? yau__path_tier(au) : YAU_TIER_3);
    uint16_t flags = v->flags;
    if (!confirmed) flags |= YAU_ONSET_UNCONFIRMED;
    if (au->desc.min_tier > 0 && tier > au->desc.min_tier) flags |= YAU_ONSET_BELOW_TIER;
    if (tier > au->worst_tier) au->worst_tier = tier;
    v->flags = flags;
    v->onset_done = 1;
    onset += au->desc.onset_offset_ns;
    memset(&m, 0, sizeof m);
    m.kind = YAU__M_ONSET;
    m.rec.id = v->id;
    m.rec.target = v->target;
    m.rec.onset = onset;
    m.rec.residual = v->target ? onset - v->target : 0;
    m.rec.start_frame = v->start;
    m.rec.device_pos = devpos;
    m.rec.rendered_at = v->rendered_at;
    m.rec.buffer_id = v->buf_id;
    m.rec.flags = flags;
    m.rec.tier = (uint8_t)tier;
    m.rec.sample = yau__voice_sample(v);
    m.rec.gap_frames = yau__is_stream(v) ? v->len : 0;
    memset(&u, 0, sizeof u);
    u.i64[0] = v->target;
    u.i64[1] = v->start;
    u.i64[2] = devpos;
    u.i64[3] = v->rendered_at;
    u.u32[8] = v->buf_id;
    u.u16[18] = flags;
    u.u16[19] = yau__tierword(au, tier, v->gen);
    yau__ring(au, (uint16_t)YAU_EV_ONSET, onset, (uint32_t)v->id, &u);
    yau__finish_voice_msg(v, &m);
    (void)yau__msg_push(au, &m);
}

static void yau__end_voice(yau_audio* au, yau__voice* v, int64_t end_frame) {
    yau__msg m;
    yrt_payload u;
    int64_t t = yau__fit_time(&au->fit, end_frame);
    v->end = end_frame;
    v->state = 3;
    memset(&m, 0, sizeof m);
    m.kind = YAU__M_END;
    m.rec.id = v->id;
    m.rec.end_frame = end_frame;
    m.rec.flags = v->flags;
    m.a = v->played;
    memset(&u, 0, sizeof u);
    u.i64[0] = end_frame;
    u.i64[1] = v->played;
    u.u16[18] = v->flags;
    if (yau__is_stream(v)) {
        yau__gap_close(au, v, v->src.st);
        u.i64[2] = v->len;
        u.i64[3] = v->start >= 0 ? v->first + (end_frame - v->start) : -1;
        m.rec.gap_frames = v->len;
        yau__st64(&v->src.st->next_, u.i64[3]);
    }
    yau__ring(au, (uint16_t)YAU_EV_END, t, (uint32_t)v->id, &u);
    if (yau__is_stream(v)) yau__stream_detach(au, v);
    yau__finish_voice_msg(v, &m);
    (void)yau__msg_push(au, &m);
}

/* A sound that never started: one ONSET record, CANCELED, at its plan. */
static void yau__cancel_waiting(yau_audio* au, yau__voice* v) {
    int64_t plan = v->target ? v->target : yau__fit_time(&au->fit, au->w);
    v->flags |= YAU_ONSET_CANCELED;
    v->start = -1;
    v->state = 3;
    if (yau__is_stream(v)) yau__stream_detach(au, v);
    yau__onset_done(au, v, plan, -1, 0);
}

/* --- commands (device thread) ------------------------------------------------ */

static yau__voice* yau__find(yau_audio* au, yau_id id) {
    int i;
    for (i = 0; i < YAU_MAX_VOICES; i++)
        if (au->voice[i].state && au->voice[i].id == id) return &au->voice[i];
    return NULL;
}

static void yau__apply(yau_audio* au, const yau__cmd* c) {
    int i;
    yau__voice* v;
    switch (c->op) {
    case YAU__OP_PLAY:
        for (i = 0; i < YAU_MAX_VOICES; i++) if (au->voice[i].state == 0) break;
        if (i == YAU_MAX_VOICES) return;   /* the frame thread counts voices */
        v = &au->voice[i];
        memset(v, 0, sizeof *v);
        v->id = c->id;
        v->state = 1;
        v->first = c->first;
        v->len = c->len;
        v->loops_left = c->loops;
        v->ch = c->ch;
        v->in_arena = c->in_arena;
        v->mask = c->mask;
        v->target = c->t;
        v->start = -1;
        v->gain = yau_db(c->db);
        v->buf_id = c->buf_id;
        if (c->st) {
            v->src.st = c->st;
            v->in_arena = YAU__STREAM_VOICE;
            v->first = 0;
            v->len = 0;
            yau__st32(&c->st->state_, YAU_STREAM_WAITING);
        } else {
            v->src.f = c->frames;
        }
        break;
    case YAU__OP_CANCEL:
        v = yau__find(au, c->id);
        if (!v || v->state == 3) return;
        if (v->state == 1) { yau__cancel_waiting(au, v); return; }
        v->s_state = 1; v->s_t = 0; v->s_ramp = 0;   /* stop now, hard */
        break;
    case YAU__OP_STOP:
    case YAU__OP_GAIN:
        for (i = 0; i < YAU_MAX_VOICES; i++) {
            v = &au->voice[i];
            if (!v->state || v->state == 3) continue;
            if (c->id && v->id != c->id) continue;
            if (c->op == YAU__OP_STOP) {
                v->s_state = 1; v->s_t = c->t; v->s_ramp = c->ramp;
            } else {
                v->g_state = 1; v->g_t = c->t; v->g_ramp = c->ramp; v->g_to = yau_db(c->db);
            }
        }
        break;
    default: break;
    }
}

/* --- the mixer (device thread) ----------------------------------------------- */

static int64_t yau__plan(yau_audio* au, int64_t t) {
    return t ? yau__fit_frame(&au->fit, t) : au->w;
}

/* Gain of voice v at stream frame f, ramps and the stop fade included. */
static float yau__gain_at(yau__voice* v, int64_t f) {
    float g = v->gain;
    if (v->g_state == 2) {
        if (f >= v->g_start + v->g_len) g = v->g_to;
        else if (f >= v->g_start) g = v->g_from + (v->g_to - v->g_from) * (float)(f - v->g_start + 1) / (float)v->g_len;
        else g = v->g_from;
    }
    if (v->s_state == 2 && f >= v->s_start && v->s_end > v->s_start) {
        double x = (double)(f - v->s_start) / (double)(v->s_end - v->s_start);
        g *= (float)(0.5 * (1.0 + cos(3.14159265358979323846 * x)));
    }
    return g;
}

static int64_t yau__stream_preroll(const yau_stream* st) {
    return st->preroll_ == YAU_PREROLL_NONE ? 0 : st->preroll_ > 0 ? st->preroll_ : st->cap_ / 4;
}

/* The ring holds the preroll, or every sample up to the end. Any thread. */
static int yau__stream_ready(const yau_stream* st) {
    int64_t e = yau__ld64(&st->end_), w = yau__ld64(&st->w_), r = yau__ld64(&st->r_);
    return w - r >= yau__stream_preroll(st) || (e >= 0 && w >= e);
}

/* Add k frames of src (v->ch channels, interleaved) at v's gain into acc
 * from stream frame f; c0 is the chunk's first frame. One kernel for
 * buffers and streams, so a stream's gain, ramp, fade and channel routing
 * are the same code as a buffer's: a mono source goes to every channel in
 * the mask, a source with the project's channels plays only the channels
 * in it. */
static void yau__mix_run(yau_audio* au, yau__voice* v, const float* src, int64_t f, int64_t k, int64_t c0) {
    int C = au->dcaps.channels;
    int64_t i;
    int constant;
    float g0;
    float* dst = au->acc + (f - c0) * C;
    int c, nc = 0, all;
    int chans[YAU_MAX_CHANNELS];
    constant = !(v->g_state == 2 && f < v->g_start + v->g_len && f + k > v->g_start)
            && !(v->s_state == 2 && f + k > v->s_start);
    if (v->g_state == 2 && f >= v->g_start + v->g_len) { v->gain = v->g_to; v->g_state = 0; constant = !(v->s_state == 2 && f + k > v->s_start); }
    g0 = yau__gain_at(v, f);
    /* the channel list once per run, not a mask test per sample */
    for (c = 0; c < C; c++) if (v->mask & ((uint64_t)1 << c)) chans[nc++] = c;
    all = nc == C;
    if (v->ch == 1 && constant && all && C == 2) {
        for (i = 0; i < k; i++) { float x = src[i] * g0; dst[2 * i] += x; dst[2 * i + 1] += x; }
    } else if (v->ch == 1) {
        for (i = 0; i < k; i++) {
            float x = src[i] * (constant ? g0 : yau__gain_at(v, f + i));
            for (c = 0; c < nc; c++) dst[i * C + chans[c]] += x;
        }
    } else if (constant && all) {
        for (i = 0; i < k * C; i++) dst[i] += src[i] * g0;
    } else {
        for (i = 0; i < k; i++) {
            float g = constant ? g0 : yau__gain_at(v, f + i);
            for (c = 0; c < nc; c++) dst[i * C + chans[c]] += src[i * C + chans[c]] * g;
        }
    }
}

/* Fix a stream's origin on its start frame: from the plan, so a late start
 * skips the samples it missed instead of moving every later sample. */
static void yau__stream_start(yau_audio* au, yau__voice* v) {
    yau_stream* st = v->src.st;
    yrt_payload u;
    int64_t plan = st->plan_;
    int64_t origin = plan - st->first_;
    st->skip_ = v->start - plan;
    v->first = v->start - origin;
    yau__st64(&st->origin_, origin);
    yau__st64(&st->plan_, v->start);
    yau__st32(&st->state_, YAU_STREAM_PLAYING);
    memset(&u, 0, sizeof u);
    u.i64[0] = origin;
    u.i64[1] = v->start;
    u.i64[2] = v->first;
    u.i64[3] = st->skip_;
    u.u32[8] = v->buf_id;
    u.u16[18] = v->flags;
    yau__ring(au, (uint16_t)YAU_EV_STREAM, yau__fit_time(&au->fit, v->start), (uint32_t)v->id, &u);
}

/* Mix stream voice v for stream frames [c0, c0 + cn). Sample s plays on
 * frame origin + s, always: a frame whose sample is not in the ring stays
 * silent (a gap) and the position moves on by exactly that frame; a sample
 * that arrives after its frame is dropped (discarded), never played late.
 * The read index never passes the write index, so the producer never
 * writes a slot this thread is reading. */
static void yau__mix_stream(yau_audio* au, yau__voice* v, int64_t c0, int32_t cn, int* contributed) {
    yau_stream* st = v->src.st;
    int64_t f = c0, stop_at, cend = c0 + cn, e, w, r, origin, cap = st->cap_;
    if (v->state == 1) {
        if (v->start < 0 || v->start >= cend) return;
        v->state = 2;
        yau__stream_start(au, v);
        f = v->start;
    } else if (v->state != 2) {
        return;
    }
    /* end before w: a producer stores w, then end, so an end seen here has
     * every sample up to it */
    e = yau__ld64(&st->end_);
    w = yau__ld64(&st->w_);
    r = st->r_;
    origin = st->origin_;
    stop_at = v->s_state == 2 ? v->s_end : INT64_MAX;
    while (f < cend) {
        int64_t s = f - origin, k = cend - f;
        if (f >= stop_at || (e >= 0 && s >= e)) {
            yau__st64(&st->r_, r);
            if (f >= stop_at) v->flags |= YAU_ONSET_STOPPED;
            yau__end_voice(au, v, f);
            return;
        }
        if (stop_at - f < k) k = stop_at - f;
        if (e >= 0 && e - s < k) k = e - s;
        if (r < s) {
            int64_t nr = w < s ? w : s;
            if (nr > r) { yau__st64(&st->discarded_, st->discarded_ + (nr - r)); r = nr; }
        }
        if (r == s && s < w) {
            int64_t at = s % cap;
            if (w - s < k) k = w - s;
            if (cap - at < k) k = cap - at;
            yau__gap_close(au, v, st);
            yau__mix_run(au, v, st->mem_ + at * v->ch, f, k, c0);
            *contributed = 1;
            r = s + k;
            v->played += k;
        } else {
            if (!st->gap_open_) {
                st->gap_open_ = 1;
                st->gap_f0_ = f;
                st->gap_s0_ = s;
                st->gap_n_ = 0;
                st->gap_xr_ = au->xruns;
                yau__st32(&st->gaps_, st->gaps_ + 1);
                au->gaps++;
                yau__st32(&au->gaps_pub, au->gaps);
                v->flags |= YAU_ONSET_GAP;
            }
            st->gap_n_ += k;
            v->len += k;
            yau__st64(&st->gap_frames_, v->len);
        }
        f += k;
    }
    yau__st64(&st->r_, r);
    yau__st64(&st->next_, cend - origin);
}

/* Mix voice v into acc for stream frames [c0, c0 + cn). */
static void yau__mix_voice(yau_audio* au, yau__voice* v, int64_t c0, int32_t cn, int* contributed) {
    int64_t f = c0, stop_at;
    if (yau__is_stream(v)) { yau__mix_stream(au, v, c0, cn, contributed); return; }
    if (v->state == 1) {
        if (v->start < 0 || v->start >= c0 + cn) return;
        v->state = 2;
        f = v->start;
    } else if (v->state != 2) {
        return;
    }
    stop_at = v->s_state == 2 ? v->s_end : INT64_MAX;
    while (f < c0 + cn) {
        int64_t k, avail;
        if (f >= stop_at) {
            v->flags |= YAU_ONSET_STOPPED;
            yau__end_voice(au, v, f);
            return;
        }
        avail = v->len - v->in_loop;
        k = c0 + cn - f;
        if (k > avail) k = avail;
        if (stop_at - f < k) k = stop_at - f;
        yau__mix_run(au, v, v->src.f + (v->first + v->in_loop) * v->ch, f, k, c0);
        *contributed = 1;
        f += k;
        v->in_loop += k;
        v->played += k;
        if (v->in_loop >= v->len) {
            if (v->loops_left != 0) {
                v->in_loop = 0;
                if (v->loops_left > 0) v->loops_left--;
            } else {
                yau__end_voice(au, v, f);
                return;
            }
        }
    }
}

static void yau__write_out(yau_audio* au, void* out, int64_t off, int32_t cn, int* clipped) {
    int C = au->dcaps.channels;
    int64_t i, ns = (int64_t)cn * C;
    const float* a = au->acc;
    int clip = 0;
    switch (au->dcaps.out) {
    case YAU_OUT_F32: {
        float* o = (float*)out + off * C;
        for (i = 0; i < ns; i++) {
            float x = a[i];
            if (x > 1.0f) { x = 1.0f; clip = 1; } else if (x < -1.0f) { x = -1.0f; clip = 1; }
            o[i] = x;
        }
    } break;
    case YAU_OUT_S16: {
        int16_t* o = (int16_t*)out + off * C;
        for (i = 0; i < ns; i++) {
            double x = floor((double)a[i] * 32768.0 + 0.5);
            if (a[i] > 1.0f || a[i] < -1.0f) clip = 1;
            if (x > 32767.0) x = 32767.0; else if (x < -32768.0) x = -32768.0;
            o[i] = (int16_t)x;
        }
    } break;
    case YAU_OUT_S24: {
        unsigned char* o = (unsigned char*)out + off * C * 3;
        for (i = 0; i < ns; i++) {
            double x = floor((double)a[i] * 8388608.0 + 0.5);
            int32_t v;
            if (a[i] > 1.0f || a[i] < -1.0f) clip = 1;
            if (x > 8388607.0) x = 8388607.0; else if (x < -8388608.0) x = -8388608.0;
            v = (int32_t)x;
            o[i * 3 + 0] = (unsigned char)(v & 0xFF);
            o[i * 3 + 1] = (unsigned char)((v >> 8) & 0xFF);
            o[i * 3 + 2] = (unsigned char)((v >> 16) & 0xFF);
        }
    } break;
    case YAU_OUT_S32: {
        int32_t* o = (int32_t*)out + off * C;
        for (i = 0; i < ns; i++) {
            double x = floor((double)a[i] * 2147483648.0 + 0.5);
            if (a[i] > 1.0f || a[i] < -1.0f) clip = 1;
            if (x > 2147483647.0) x = 2147483647.0; else if (x < -2147483648.0) x = -2147483648.0;
            o[i] = (int32_t)x;
        }
    } break;
    default: break;
    }
    *clipped = clip;
}

/* restart: the old fit points describe timing that no longer holds;
 * halt_ns and at: a halt found from late reports (HALT), else 0; again:
 * the record continues the last one (the same underrun), not counted */
static void yau__xrun_rec(yau_audio* au, int64_t t, int64_t gap, int restart, int64_t halt_ns, int64_t at,
                          int again) {
    yau__msg m;
    yrt_payload u;
    int i;
    if (!again) au->xruns++;
    au->late_run = 0;
    if (gap > 0) {
        /* The device counted gap frames of silence, so every frame from here
         * plays gap frames later than the fit said: move the old points by
         * the same amount and the fit stays continuous. */
        for (i = 0; i < YAU__FIT_PTS; i++) au->pt_w[i] -= gap;
        au->b_w0 -= gap; au->b_bw -= gap;
        au->last_pos -= gap;
        au->fit.w0 -= gap;
    } else if (restart) {
        /* The position stopped while the device had no data (measured on
         * WASAPI: it halts at the frames written) and every later frame
         * plays later by the time it stood; callback times cannot say how
         * far either. The old points describe the old timing: start over. */
        au->pt_n = 0; au->pt_head = 0; au->b_open = 0;
        au->fit.ready = 0;
        au->first_block = 1;
    }
    for (i = 0; i < YAU_MAX_VOICES; i++) {
        yau__voice* v = &au->voice[i];
        if (v->state == 2 || (v->state == 3 && !v->onset_done)) v->flags |= YAU_ONSET_XRUN;
    }
    memset(&u, 0, sizeof u);
    u.i64[0] = au->w;
    u.i64[1] = gap;
    u.i64[2] = halt_ns;
    u.i64[3] = at;
    yau__ring(au, (uint16_t)YAU_EV_XRUN, t, au->xruns, &u);
    if (again) return;
    memset(&m, 0, sizeof m);
    m.kind = YAU__M_XRUN;
    m.a = au->w;
    m.b = gap;
    (void)yau__msg_push(au, &m);
    YRT_MESSAGE("yau underrun");
}

static void yau__xrun(yau_audio* au, int64_t t, int64_t gap, int restart) {
    yau__xrun_rec(au, t, gap, restart, 0, 0, 0);
}

static void yau__publish_fit(yau_audio* au) {
    yau__msg m;
    yrt_payload u;
    memset(&m, 0, sizeof m);
    m.kind = YAU__M_FIT;
    m.fit = au->fit;
    (void)yau__msg_push(au, &m);
    memset(&u, 0, sizeof u);
    u.i64[0] = au->fit.w0;
    u.f64[1] = 1e9 / au->fit.k;
    u.f64[2] = au->fit.spread;
    u.u32[6] = au->fit.n;
    u.u32[7] = (uint32_t)au->fit.source;
    yau__ring(au, (uint16_t)YAU_EV_FIT, au->fit.t0, au->fit.gen, &u);
}

/* One point per bucket: its lowest against the fit's rate, which is the
 * least late. */
static void yau__feed(yau_audio* au, int64_t w, int64_t t) {
    double r;
    if (au->b_open && t - au->b_t0 >= (au->dcaps.pos_source == YAU_POS_DEVICE ? YAU__BUCKET_POS_NS : YAU__BUCKET_NS)) {
        yau__fit_point(au, au->b_bw, au->b_bt);
        au->b_open = 0;
        if (!au->fit.ready || t - au->last_fit_t >= YAU__REFIT_NS) {
            YRT_ZONE(zf, "yau.fit");
            yau__refit(au);
            YRT_ZONE_END(zf);
            au->last_fit_t = t;
            yau__publish_fit(au);
        }
    }
    if (!au->b_open) {
        au->b_open = 1;
        au->b_w0 = w; au->b_t0 = t;
        au->b_bw = w; au->b_bt = t; au->b_best = 0;
    }
    r = (double)(t - au->b_t0) - (double)(w - au->b_w0) * (au->fit.k > 0 ? au->fit.k : yau__k_nom(au));
    if (r < au->b_best) { au->b_best = r; au->b_bw = w; au->b_bt = t; }
}

YAU_API void yau_render(void* host, void* out, int32_t frames, const yau_tick* tk) {
    yau_audio* au = (yau_audio*)host;
    int64_t W, now_t;
    int i, have_pos;
    int64_t pw = 0;
    uint32_t head, tail, ncmd = 0;
    YRT_ZONE(zr, "yau.render");
    if (!au || !out || frames <= 0 || !tk) { YRT_ZONE_END(zr); return; }
    YAU__ON_TICK(au, tk);
    W = au->w;
    now_t = tk->t_entry;
    have_pos = au->dcaps.pos_source == YAU_POS_DEVICE && tk->pos >= 0 && tk->pos_t > 0;

    if (au->first_block) {
        /* Until the first fit, the nominal rate from the first stamp. */
        au->first_block = 0;
        au->fit.w0 = have_pos ? tk->pos : W;
        au->fit.t0 = have_pos ? tk->pos_t : now_t;
        au->fit.k = yau__k_nom(au);
        au->fit.source = au->dcaps.pos_source;
    }

    /* 1. underruns */
    if (have_pos) {
        pw = tk->pos - au->p_off;
        if (W > 0 && pw >= W) {
            int64_t gap = pw - W;
            if (gap > 0) au->p_off += gap;   /* the device counted its silence */
            pw = W;
            yau__xrun(au, now_t, gap, 1);
        }
    } else if (tk->flags & YAU_TICK_XRUN) {
        yau__xrun(au, now_t, 0, 1);
    } else if (au->dcaps.pos_source != YAU_POS_DEVICE && au->dcaps.tier != YAU_TIER_SIM
               && au->last_entry > 0 && au->dcaps.buffer > 0) {
        /* Not for the null device: it is a timer thread with no hardware
         * buffer, so a long gap is its scheduling jitter and drops nothing.
         * On a loaded CI VM every gap restarted the fit, which then never
         * warmed up. */
        int64_t gap = now_t - au->last_entry;
        if ((double)gap > (double)au->dcaps.buffer * yau__k_nom(au) * 1.5) yau__xrun(au, now_t, 0, 1);
    }
    if (have_pos && (tk->flags & YAU_TICK_XRUN)) yau__xrun(au, now_t, 0, 1);
    au->last_entry = now_t;

    /* 2. the fit */
    if (have_pos) {
        /* a report that has not moved since the last one is older than its
         * stamp says */
        /* A report can be late, never early. Measured under load on the test
         * laptop, WASAPI's position jumped ahead of the fit by up to 45 ms
         * for tens of seconds with no callback late enough to run the queue
         * dry. Whether the sound moved with it is not known (no loopback ran
         * under that load), so such a report is kept out of the fit, the
         * sounds in flight are flagged XRUN (their onsets planned), and if
         * the jump lasts YAU__REANCHOR_NS the fit starts over from the
         * reports as they are. */
        if (pw > au->last_pos && au->fit.ready) {
            double r = (double)(tk->pos_t - yau__fit_time(&au->fit, pw));
            double half = 0.5 * (double)au->dcaps.period * au->fit.k;
            if (r > half) {
                /* HALT. An underrun of about a period: the engine ran dry
                 * before the position reached the frames written, the
                 * position stood for a pass, and every later frame plays
                 * that much later (measured on WASAPI shared: 10.3 ms
                 * after stalls of 20 to 35 ms; docs/audio.md). From then
                 * on every report is late by the halt. One report can be
                 * late without a halt (a call preempted between reading
                 * the position and its stamp), so it takes
                 * YAU__HALT_REPORTS in a row. Until then the reports stay
                 * out of the fit and confirm nothing. Then the fit starts
                 * over, as after any underrun, from this report at once,
                 * so the sounds in flight, flagged XRUN because they were
                 * planned on the old timing, get times on the new one.
                 * The record's halt is the least late of the run but its
                 * first, which can be the halt half over (the position
                 * moved, then stood); a later report moved, so the device
                 * played again and the whole halt is in it. Measured at
                 * queue 2, the report before such a run was 9 ms early (the
                 * position ran ahead, then stood): that report has its
                 * XRUN record already, and the halt's record continues it,
                 * one underrun, counted once. */
                if (au->late_run++ == 0) { au->late_w0 = pw; au->late_again = au->early_run > 0; }
                else if (au->late_run == 2 || r < au->late_min) au->late_min = r;
                au->early_run = 0;
                if (au->late_run >= YAU__HALT_REPORTS) {
                    yau__xrun_rec(au, now_t, 0, 1, (int64_t)au->late_min, au->late_w0, au->late_again);
                    au->first_block = 0;
                    au->fit.w0 = pw; au->fit.t0 = tk->pos_t; au->fit.k = yau__k_nom(au);
                } else {
                    au->last_pos = pw;
                    goto fed;
                }
            } else if (r < -half) {
                au->late_run = 0;
                if (au->early_run++ == 0) {
                    au->early_since = tk->pos_t;
                    yau__xrun(au, now_t, 0, 0);
                }
                if (tk->pos_t - au->early_since >= YAU__REANCHOR_NS) {
                    au->pt_n = 0; au->pt_head = 0; au->b_open = 0;
                    au->fit.w0 = pw; au->fit.t0 = tk->pos_t; au->fit.k = yau__k_nom(au);
                    au->fit.ready = 0;
                    au->early_run = 0;
                } else {
                    au->last_pos = pw;
                    goto fed;
                }
            } else {
                au->early_run = 0;
                au->late_run = 0;
            }
        }
        if (pw > au->last_pos) yau__feed(au, pw, tk->pos_t);
    fed:
        au->last_pos = pw;
        if (au->desc.ring && tk->pos_t - au->last_clock_t >= 1000000000LL) {
            yrt_event ev;
            memset(&ev, 0, sizeof ev);
            ev.t_ns = (uint64_t)tk->pos_t;
            ev.tid = au->tid;
            ev.source = (uint16_t)YRT_SRC_RT;
            ev.kind = (uint16_t)YRT_KIND_CLOCK;
            ev.aux = 0x41550000u + (au->desc.device_index & 7u);
            ev.u.u64[0] = (uint64_t)pw;
            (void)yrt_ring_push(au->desc.ring, &ev);
            au->last_clock_t = tk->pos_t;
        }
    } else if (au->dcaps.tier != YAU_TIER_WEB || au->dcaps.pos_source != YAU_POS_DEVICE) {
        /* A browser's first blocks have no report at or past stream frame
         * 0, and an AudioWorklet's entry times have 1 ms steps: its fit is
         * of reports only (BACKENDS, WEB). */
        yau__feed(au, W, now_t);
    }
    YRT_PLOT("yau queued frames", have_pos ? (double)(W - pw) : 0.0);

    /* 3. commands, at most 64 a block */
    head = yau__ld32(&au->cmd_head);
    tail = au->cmd_tail;
    while (tail != head && ncmd < 64) {
        yau__apply(au, &au->cmd[tail % YAU__CMDS]);
        tail++; ncmd++;
    }
    yau__st32(&au->cmd_tail, tail);

    /* 4. confirm: the device has played past the onset frame with no
     * underrun since it was rendered; its time is the fit's, which these
     * reports made */
    if (have_pos) {
        for (i = 0; i < YAU_MAX_VOICES; i++) {
            yau__voice* v = &au->voice[i];
            if (!(v->state == 2 || v->state == 3) || v->onset_done || v->start < 0) continue;
            if (pw <= v->start || au->late_run > 0) continue;
            /* a report the fit has set aside confirms nothing */
            if (au->early_run > 0) v->flags |= YAU_ONSET_XRUN;
            yau__onset_done(au, v, yau__fit_time(&au->fit, v->start), tk->pos,
                              !(v->flags & YAU_ONSET_XRUN));
        }
    }

    /* 5. plan: start, gain and stop frames from the newest fit */
    for (i = 0; i < YAU_MAX_VOICES; i++) {
        yau__voice* v = &au->voice[i];
        if (v->state == 1) {
            int64_t f;
            if (yau__is_stream(v) && v->target == 0 && !yau__stream_ready(v->src.st)) {
                /* "as soon as it can" for a stream: once the ring holds its
                 * preroll, so it does not begin with a gap */
                v->start = -1;
                if (v->s_state && (v->s_t ? yau__plan(au, v->s_t) : W) <= W) yau__cancel_waiting(au, v);
                continue;
            }
            f = yau__plan(au, v->target);
            if (yau__is_stream(v)) yau__st64(&v->src.st->plan_, f);
            if (f < W) { f = W; v->flags |= YAU_ONSET_LATE; }
            else v->flags = (uint16_t)(v->flags & ~YAU_ONSET_LATE);
            v->start = f;
            v->gen = (uint16_t)au->fit.gen;
            if (v->s_state && (v->s_t ? yau__plan(au, v->s_t) : W) <= f) {
                yau__cancel_waiting(au, v);
                continue;
            }
            if (f < W + frames) v->rendered_at = now_t;
        }
        if (v->state != 1 && v->state != 2) continue;
        if (v->g_state == 1) {
            int64_t g = yau__plan(au, v->g_t);
            if (g < W) g = W;
            if (g < W + frames) {
                v->g_state = 2;
                v->g_start = g;
                v->g_len = yau__frames_of(v->g_ramp, au->dcaps.rate);
                if (v->g_len < 1) v->g_len = 1;
                v->g_from = v->gain;
            }
        }
        if (v->s_state == 1) {
            int64_t s = v->s_t ? yau__plan(au, v->s_t) : W;
            int64_t r = yau__frames_of(v->s_ramp, au->dcaps.rate);
            int64_t s0 = s - r;
            if (s0 < W) { s0 = W; s = W + r; }
            if (s0 < W + frames) { v->s_state = 2; v->s_start = s0; v->s_end = s; }
        }
    }

    /* 6. mix in chunks the accumulator holds */
    {
        int C = au->dcaps.channels;
        int32_t chunk = YAU__ACC / C, done = 0;
        YRT_ZONE(zm, "yau.mix");
        while (done < frames) {
            int32_t cn = frames - done < chunk ? frames - done : chunk;
            int64_t c0 = W + done;
            int clipped = 0;
            int contrib[YAU_MAX_VOICES];
            memset(au->acc, 0, sizeof(float) * (size_t)cn * (size_t)C);
            for (i = 0; i < YAU_MAX_VOICES; i++) {
                contrib[i] = 0;
                if (au->voice[i].state == 1 || au->voice[i].state == 2)
                    yau__mix_voice(au, &au->voice[i], c0, cn, &contrib[i]);
            }
            if (au->desc.source && au->desc.source->render) {
                yau_clock clk;
                clk.rate = au->dcaps.rate;
                clk.frame = c0;
                clk.t_frame = yau__fit_time(&au->fit, c0);
                clk.ns_per_frame = au->fit.k;
                au->desc.source->render(au->desc.source_ctx, au->acc, cn, &clk);
            }
            yau__write_out(au, out, done, cn, &clipped);
            if (clipped) {
                au->clip_any = 1;
                for (i = 0; i < YAU_MAX_VOICES; i++)
                    if (contrib[i]) au->voice[i].flags |= YAU_ONSET_CLIPPED;
            }
            done += cn;
        }
        YRT_ZONE_END(zm);
    }

    /* 7. onsets with no reports to confirm them complete now */
    if (au->dcaps.pos_source != YAU_POS_DEVICE) {
        for (i = 0; i < YAU_MAX_VOICES; i++) {
            yau__voice* v = &au->voice[i];
            if ((v->state == 2 || v->state == 3) && !v->onset_done && v->start >= 0 && v->start < W + frames)
                yau__onset_done(au, v, yau__fit_time(&au->fit, v->start), -1, 0);
        }
    }

    au->w = W + frames;
    yau__st64(&au->w_pub, au->w);
    YAU__ON_RENDER_END(au);
    YRT_ZONE_END(zr);
}

/* --- the queue gate (QUEUE) ---------------------------------------------------- */

/* The WASAPI buffer fill at or under which the gate lets a render through,
 * or -1 for no gate: shared mode only (exclusive mode takes a whole buffer
 * per event), and only when miniaudio's GetBuffer of one period always
 * finds room once the gate opens, so miniaudio never waits on the event
 * the gate took. A macro, not a function, because only the WASAPI build
 * uses it and the core test checks it without a device. */
#define YAU__QUEUE_PAD(queue, period, buffer, exclusive)                              \
    ((exclusive) || (queue) <= 0 || (period) <= 0                                     \
     || (int64_t)(buffer) - (period) < (int64_t)((queue) - 1) * (period)              \
     ? -1 : ((queue) - 1) * (period))

/* --- device thread setup ------------------------------------------------------ */

YAU_API void yau_rt_thread_init(void* host) {
    yau_audio* au = (yau_audio*)host;
    YRT_THREAD_INIT("yau device");
    if (au) {
        au->tid = yrt_thread_id();
    }
}

/* --- miniaudio backend ------------------------------------------------------- */

#if !defined(YAU_NO_MINIAUDIO)

#if defined(MA_SUPPORT_WASAPI)
/* IAudioClock and the IAudioClient slot that hands it out, declared here so
 * the header needs no SDK header and works when miniaudio was compiled in
 * another translation unit. tests/compile/audio_com.cpp checks both
 * against the Windows SDK. */
typedef struct yau__IAudioClockV {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, const IID*, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *GetFrequency)(void*, UINT64*);
    HRESULT (STDMETHODCALLTYPE *GetPosition)(void*, UINT64*, UINT64*);
    HRESULT (STDMETHODCALLTYPE *GetCharacteristics)(void*, DWORD*);
} yau__IAudioClockV;
typedef struct yau__IAudioClock { const yau__IAudioClockV* lpVtbl; } yau__IAudioClock;
typedef struct yau__IAudioClientV {
    void* slots0_[5];   /* IUnknown (3), Initialize, GetBufferSize */
    HRESULT (STDMETHODCALLTYPE *GetStreamLatency)(void*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *GetCurrentPadding)(void*, UINT32*);
    void* slots1_[7];   /* IsFormatSupported .. SetEventHandle */
    HRESULT (STDMETHODCALLTYPE *GetService)(void*, const IID*, void**);
} yau__IAudioClientV;
typedef struct yau__IAudioClient { const yau__IAudioClientV* lpVtbl; } yau__IAudioClient;
static const IID yau__IID_IAudioClock =
    { 0xcd63314f, 0x3fba, 0x4a1b, { 0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7 } };
#endif

#if defined(MA_HAS_WEBAUDIO)
/* What the page's timer last read from the AudioContext, on the main
 * thread, for the callback (BACKENDS, WEB): seq is odd while it writes. */
typedef struct yau__web_pub {
    uint32_t seq, reserved_;
    double   ctx_s;          /* getOutputTimestamp().contextTime            */
    double   perf_ms;        /* its performanceTime                         */
    double   base_s, out_s;  /* baseLatency, outputLatency; -1 = none      */
} yau__web_pub;
#endif

typedef struct yau__ma {
    ma_context ctx;
    ma_device  dev;
    int        ctx_up, dev_up, closing;
    yau_audio* au;
    yau_render_fn render;
    void*      host;
    uint32_t   allocs;      /* miniaudio heap calls; M7 counts them after start */
    ma_backend backend;
#if defined(MA_SUPPORT_WASAPI)
    yau__IAudioClock* clock;
    uint64_t   freq;
    int        gate;        /* the queue gate is on (QUEUE)                */
    uint32_t   gate_pad;    /* render when the WASAPI buffer holds at most this */
#endif
#if defined(MA_HAS_WEBAUDIO)
    yau__web_pub pub;       /* the main thread writes it (JavaScript)      */
    int64_t    web_off;     /* ysp_rt ns minus performance.now() ns        */
    int64_t    web_frames;  /* frames handed to the context                */
    int64_t    web_c0;      /* the context frame of stream frame 0; -1 = none */
    int64_t    web_ctx;     /* the newest report's context frame            */
    int64_t    web_est[8];  /* ScriptProcessorNode: the last estimates of web_c0 */
    int32_t    web_n_est;
    double     web_last;    /* the newest report's performanceTime; 0 = none */
    double     web_date_off;/* Date.now() minus performance.now(), ms       */
    uint32_t   web_moves;   /* times web_c0 moved: blocks the context skipped */
    int        web_idx, web_worklet, web_reports;
#endif
} yau__ma;
typedef char yau__ma_fits[sizeof(yau__ma) <= sizeof(((yau_audio*)0)->backend_mem) ? 1 : -1];

static void* yau__ma_malloc(size_t sz, void* ud) { ((yau__ma*)ud)->allocs++; return malloc(sz); }
static void* yau__ma_realloc(void* p, size_t sz, void* ud) { ((yau__ma*)ud)->allocs++; return realloc(p, sz); }
static void  yau__ma_free(void* p, void* ud) { if (p) ((yau__ma*)ud)->allocs++; free(p); }

#if defined(MA_SUPPORT_WASAPI)
/* QUEUE: miniaudio renders a block, then waits until the WASAPI buffer has
 * room for it, so the block waits one period in miniaudio and then lands
 * behind up to two periods in the buffer. The gate holds the render until
 * the engine has taken the buffer down to gate_pad frames, on the same
 * event miniaudio waits on. miniaudio's GetBuffer then always finds room,
 * so it never waits on the event the gate took. */
static void yau__ma_gate(yau__ma* m, ma_device* d) {
    yau__IAudioClient* ac = (yau__IAudioClient*)d->wasapi.pAudioClientPlayback;
    UINT32 pad = 0;
    int k;
    for (k = 0; k < 8 && !m->closing; k++) {
        if (FAILED(ac->lpVtbl->GetCurrentPadding(ac, &pad)) || pad <= m->gate_pad) return;
        /* a period is at most a few tens of ms; 100 ms means the device
         * stopped, and miniaudio's own wait then takes over */
        if (WaitForSingleObject((HANDLE)d->wasapi.hEventPlayback, 100) != WAIT_OBJECT_0) return;
    }
}
#endif

#if defined(MA_HAS_WEBAUDIO)
/* Attaches the reader to miniaudio's device (its window.miniaudio list,
 * miniaudio 0.11.25's JavaScript): a timer every 10 ms, and on the
 * ScriptProcessorNode path each callback too, writes the context's
 * getOutputTimestamp() into *pub; the ScriptProcessorNode's handler also
 * keeps the block's playbackTime. Returns -1 when the device is not there,
 * else (1 when getOutputTimestamp exists) | (2 on the AudioWorklet path). */
EM_JS(int, yau__web_attach, (int idx, void* pub), {
    var ma = typeof window !== 'undefined' ? window.miniaudio : null;
    var d = ma ? ma.get_device_by_index(idx) : null;
    if (!d || !d.webaudio) return -1;
    var ctx = d.webaudio, ok = typeof ctx.getOutputTimestamp === 'function', p = pub;
    function publish() {
        var o, s;
        if (!ok || ctx.state !== 'running') return;
        o = ctx.getOutputTimestamp();
        /* none yet, or one the browser has not renewed for 100 ms (seen
         * once, 1 s old, right after the context started) */
        if (!(o.contextTime > 0) || !(o.performanceTime > 0) || performance.now() - o.performanceTime > 100) return;
        s = Atomics.load(HEAP32, p >> 2);
        Atomics.store(HEAP32, p >> 2, (s + 1) | 0);
        HEAPF64[(p + 8) >> 3] = o.contextTime;
        HEAPF64[(p + 16) >> 3] = o.performanceTime;
        HEAPF64[(p + 24) >> 3] = typeof ctx.baseLatency === 'number' ? ctx.baseLatency : -1;
        HEAPF64[(p + 32) >> 3] = typeof ctx.outputLatency === 'number' ? ctx.outputLatency : -1;
        Atomics.store(HEAP32, p >> 2, (s + 2) | 0);
    }
    if (d.scriptNode) {
        var f = d.scriptNode.onaudioprocess;
        d.yauPT = -1;
        d.scriptNode.onaudioprocess = function(e) { d.yauPT = e.playbackTime; publish(); f(e); };
    }
    d.yauTimer = setInterval(publish, 10);
    return (ok ? 1 : 0) | (d.scriptNode ? 0 : 2);
})

EM_JS(void, yau__web_detach, (int idx), {
    var ma = typeof window !== 'undefined' ? window.miniaudio : null;
    var d = ma ? ma.get_device_by_index(idx) : null;
    if (d && d.yauTimer) { clearInterval(d.yauTimer); d.yauTimer = 0; }
})

/* The context frame the block now rendered plays at: the AudioWorklet's
 * currentFrame in its own scope, or the ScriptProcessorNode block's
 * playbackTime; -1 when neither is known. */
EM_JS(double, yau__web_block, (int idx), {
    if (typeof AudioWorkletGlobalScope !== 'undefined' && typeof currentFrame === 'number') return currentFrame;
    var ma = typeof window !== 'undefined' ? window.miniaudio : null;
    var d = ma ? ma.get_device_by_index(idx) : null;
    return d && d.yauPT >= 0 ? Math.round(d.yauPT * d.webaudio.sampleRate) : -1;
})

EM_JS(double, yau__web_perf_ms, (void), { return performance.now(); })
EM_JS(double, yau__web_date_ms, (void), { return Date.now(); })
static uint64_t yau__web_perf_ns(void) { return (uint64_t)(yau__web_perf_ms() * 1e6); }

/* performance.now() where the scope has it; an AudioWorklet's has not
 * (Chrome 154: no performance object, and yrt_now_ns() reads 0 there), so
 * Date.now() less its offset to performance.now() read at open: 1 ms
 * steps, and a jump when the system's wall clock is set. */
EM_JS(double, yau__web_now_ms, (double date_minus_perf), {
    return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now() - date_minus_perf;
})

/* The block's tick. The newest getOutputTimestamp() is the report of
 * every block: the context frame at the output, less the context frame of
 * stream frame 0, is the stream frame at the output, at performanceTime.
 * A report that has not moved since the last block is given again; the
 * core takes it as one that has not moved, which feeds nothing.
 * Stream frame 0's context frame: on the AudioWorklet path currentFrame
 * less the frames handed before, exact, so a change is a skipped block.
 * On the ScriptProcessorNode path the block's playbackTime, which Chrome
 * 154 gives as currentTime plus the buffer when the event runs: currentTime
 * moves in the audio device's chunks (480 frames here), and after the main
 * thread stalls the events that waited run back to back, so an estimate
 * is late by up to a chunk, or by the wait. The largest of the last 8 is
 * kept; one that moves by more than half a buffer is a skipped block. */
static void yau__web_tick(yau__ma* m, ma_uint32 n, yau_tick* tk) {
    double blk = yau__web_block(m->web_idx), ctx_s, perf;
    uint32_t s0, s1;
    tk->t_entry = (int64_t)llround(yau__web_now_ms(m->web_date_off) * 1e6) + m->web_off;
    /* a seqlock: the main thread may write while the worklet reads */
    s0 = yau__ld32(&m->pub.seq);
    if (!(s0 & 1u)) {
        ctx_s = *(volatile double*)&m->pub.ctx_s;
        perf = *(volatile double*)&m->pub.perf_ms;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = yau__ld32(&m->pub.seq);
        if (s0 == s1 && perf > m->web_last) {
            m->web_last = perf;
            m->web_ctx = (int64_t)floor(ctx_s * (double)m->dev.playback.internalSampleRate + 0.5);
        }
    }
    if (blk >= 0) {
        int64_t c0 = (int64_t)blk - m->web_frames, d;
        int i;
        if (!m->web_worklet) {   /* the largest of the last 8 estimates */
            m->web_est[m->web_n_est++ & 7] = c0;
            for (i = 0; i < 8 && i < m->web_n_est; i++) if (m->web_est[i] > c0) c0 = m->web_est[i];
        }
        d = c0 - m->web_c0;
        if (m->web_c0 >= 0 && (m->web_worklet ? d != 0 : (d > (int64_t)n / 2 || d < -(int64_t)n / 2))) {
            m->web_moves++;
            tk->flags |= YAU_TICK_XRUN;
        }
        m->web_c0 = c0;
    }
    m->web_frames += n;
    if (m->web_reports && m->web_c0 >= 0 && m->web_last > 0) {
        tk->pos = m->web_ctx - m->web_c0;
        tk->pos_t = (int64_t)llround(m->web_last * 1e6) + m->web_off;
    }
}
#endif

static void yau__ma_data(ma_device* d, void* out, const void* in, ma_uint32 n) {
    yau__ma* m = (yau__ma*)d->pUserData;
    yau_tick tk;
    (void)in;
#if defined(MA_SUPPORT_WASAPI)
    if (m->gate) yau__ma_gate(m, d);
#endif
    tk.t_entry = yrt_now_ns();
    tk.pos = -1;
    tk.pos_t = 0;
    tk.flags = 0;
    tk.reserved_ = 0;
#if defined(MA_HAS_WEBAUDIO)
    if (m->backend == ma_backend_webaudio) yau__web_tick(m, n, &tk);
#endif
#if defined(MA_SUPPORT_WASAPI)
    if (m->clock && m->freq) {
        UINT64 pos = 0, qpc = 0;
        if (SUCCEEDED(m->clock->lpVtbl->GetPosition(m->clock, &pos, &qpc)) && qpc) {
            /* pos counts GetFrequency() units since the stream started; the
             * stamp is QPC time in 100 ns units */
            tk.pos = (int64_t)((double)pos * (double)d->playback.internalSampleRate / (double)m->freq + 0.5);
            tk.pos_t = (int64_t)qpc * 100;
        }
    }
#endif
    if (m->render) m->render(m->host, out, (int32_t)n, &tk);
}

static void yau__ma_note(const ma_device_notification* nt) {
    yau__ma* m = (yau__ma*)nt->pDevice->pUserData;
    if (!m || !m->au) return;
    switch (nt->type) {
    case ma_device_notification_type_started:
        yau_rt_thread_init(m->host);
        break;
    case ma_device_notification_type_stopped:
    case ma_device_notification_type_rerouted:
    case ma_device_notification_type_interruption_began:
        if (!m->closing) yau__st32(&m->au->lost, 1u);
        break;
    default: break;
    }
}

static int yau__ma_out(ma_format f) {
    switch (f) {
    case ma_format_f32: return YAU_OUT_F32;
    case ma_format_s16: return YAU_OUT_S16;
    case ma_format_s24: return YAU_OUT_S24;
    case ma_format_s32: return YAU_OUT_S32;
    default: return 0;
    }
}

#if defined(MA_SUPPORT_COREAUDIO) && defined(MA_APPLE_DESKTOP)
/* Defined after yau__ma_open, beside the CoreAudio property code it uses. */
static void yau__coreaudio_claims(yau__ma* m, yau_device_caps* caps);
#endif

static int yau__ma_open(void* ctx, const yau_device_open* in, yau_device_caps* caps,
                          char* err, size_t err_cap) {
    yau__ma* m = (yau__ma*)ctx;
    ma_context_config cc = ma_context_config_init();
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    ma_device_info* infos = NULL;
    ma_uint32 ninfo = 0, i;
    ma_device_id* pick = NULL;
    ma_result r;
    int is_null = m->backend == ma_backend_null;
    cc.allocationCallbacks.pUserData = m;
    cc.allocationCallbacks.onMalloc = yau__ma_malloc;
    cc.allocationCallbacks.onRealloc = yau__ma_realloc;
    cc.allocationCallbacks.onFree = yau__ma_free;
    cc.threadPriority = ma_thread_priority_highest;
    r = ma_context_init(&m->backend, 1, &cc, &m->ctx);
    if (r != MA_SUCCESS) {
        snprintf(err, err_cap, "ysp_audio: the %s backend did not start (%s)",
                 ma_get_backend_name(m->backend), ma_result_description(r));
        return YAU_ERR_LOST;
    }
    m->ctx_up = 1;
    if (in->device && in->device[0]) {
        if (ma_context_get_devices(&m->ctx, &infos, &ninfo, NULL, NULL) == MA_SUCCESS)
            for (i = 0; i < ninfo; i++)
                if (strstr(infos[i].name, in->device)) { pick = &infos[i].id; break; }
        if (!pick) {
            snprintf(err, err_cap, "ysp_audio: no playback device named like '%s'", in->device);
            return YAU_ERR_ARG;
        }
    }
    dc.playback.pDeviceID = pick;
    /* Ask for nothing, so the client format is the device's own and neither
     * Windows nor miniaudio converts (STRICT OPEN). The null device has no
     * format of its own and takes the project's. */
    dc.playback.format = is_null ? (in->format->sample == YAU_S16 ? ma_format_s16 : ma_format_f32)
                                 : ma_format_unknown;
    dc.playback.channels = is_null ? in->format->channels : 0;
    dc.sampleRate = is_null ? in->format->rate : 0;
#if defined(MA_HAS_WEBAUDIO)
    /* A browser shows no device format: the AudioContext runs at the rate
     * it is asked for and converts to the device's own unseen (BACKENDS,
     * WEB), so ask for the project's format, which the strict open below
     * then finds. */
    if (m->backend == ma_backend_webaudio) {
        dc.playback.format = ma_format_f32;
        dc.playback.channels = in->format->channels;
        dc.sampleRate = in->format->rate;
    }
#endif
    dc.playback.pChannelMap = NULL;
    dc.playback.shareMode = in->exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    dc.periodSizeInFrames = (ma_uint32)in->period;
    /* Exclusive mode: miniaudio's default of 3 makes one buffer of three
     * periods, filled by three callbacks in a burst per event; measured, it
     * had underruns and onsets 62 ms off the plan, one period had neither
     * (docs/audio.md). Shared mode ignores it (IAudioClient3). */
    dc.periods = in->exclusive ? 1 : 0;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noFixedSizedCallback = MA_TRUE;
    dc.noClip = MA_TRUE;
    dc.noPreSilencedOutputBuffer = MA_TRUE;
    dc.dataCallback = yau__ma_data;
    dc.notificationCallback = yau__ma_note;
    dc.pUserData = m;
    dc.wasapi.noAutoConvertSRC = MA_TRUE;
    dc.wasapi.noDefaultQualitySRC = MA_TRUE;
    dc.wasapi.noHardwareOffloading = MA_TRUE;
    dc.wasapi.noAutoStreamRouting = MA_TRUE;
    /* MMCSS "Pro Audio" for the device thread: of miniaudio's default
     * priority, MMCSS, yrt_thread_elevate() and both, it had the smallest
     * p99 callback interval error under load, with no underrun in any
     * (docs/audio.md) */
    dc.wasapi.usage = ma_wasapi_usage_pro_audio;
    r = ma_device_init(&m->ctx, &dc, &m->dev);
    if (r != MA_SUCCESS) {
        snprintf(err, err_cap, "ysp_audio: %s did not open the device%s (%s)",
                 ma_get_backend_name(m->backend), in->exclusive ? " in exclusive mode" : "",
                 ma_result_description(r));
        return r == MA_BUSY || r == MA_ACCESS_DENIED ? YAU_ERR_LOST : YAU_ERR_FORMAT;
    }
    m->dev_up = 1;
    memset(caps, 0, sizeof *caps);
    caps->rate = m->dev.playback.internalSampleRate;
    caps->channels = (uint16_t)m->dev.playback.internalChannels;
    caps->out = (uint16_t)yau__ma_out(m->dev.playback.internalFormat);
    for (i = 0; i < m->dev.playback.internalChannels && i < YAU_MAX_CHANNELS; i++)
        caps->map[i] = (uint8_t)m->dev.playback.internalChannelMap[i];
    caps->period = (int32_t)m->dev.playback.internalPeriodSizeInFrames;
    caps->buffer = (int32_t)(m->dev.playback.internalPeriodSizeInFrames * m->dev.playback.internalPeriods);
    caps->exclusive = m->dev.playback.shareMode == ma_share_mode_exclusive;
    caps->pos_source = YAU_POS_CALLBACK;
    caps->tier = is_null ? YAU_TIER_SIM : YAU_TIER_3;
    {
        size_t k = strlen(m->dev.playback.name);
        if (k >= sizeof caps->name) k = sizeof caps->name - 1;
        memcpy(caps->name, m->dev.playback.name, k);
        caps->name[k] = 0;
    }
    if (!m->dev.playback.converter.isPassthrough || m->dev.playback.format != m->dev.playback.internalFormat
        || m->dev.sampleRate != m->dev.playback.internalSampleRate
        || m->dev.playback.channels != m->dev.playback.internalChannels) {
        snprintf(err, err_cap, "ysp_audio: miniaudio would convert between the program and "
                 "device '%s'; the header refuses any conversion", caps->name);
        return YAU_ERR_FORMAT;
    }
#if defined(MA_SUPPORT_COREAUDIO) && defined(MA_APPLE_DESKTOP)
    if (m->backend == ma_backend_coreaudio) yau__coreaudio_claims(m, caps);
#endif
#if defined(MA_HAS_WEBAUDIO)
    if (m->backend == ma_backend_webaudio) {
        /* BACKENDS, WEB: position reports from getOutputTimestamp(), whose
         * performanceTime is on performance.now()'s clock; that clock's
         * offset to ysp_rt is read here, on the main thread */
        yrt_corr_desc cd;
        yrt_corr c;
        int a;
        memset(&cd, 0, sizeof cd);
        cd.read = yau__web_perf_ns;
        if (yrt_correlate(&cd, &c) > 0) m->web_off = c.rt_ns - (int64_t)c.other;
        m->web_date_off = yau__web_date_ms() - yau__web_perf_ms();
        m->web_c0 = -1;
        m->web_idx = m->dev.webaudio.deviceIndex;
        a = yau__web_attach(m->web_idx, &m->pub);
        m->web_reports = a > 0 && (a & 1);
        m->web_worklet = a > 0 && (a & 2);
        caps->tier = YAU_TIER_WEB;
        caps->buffer = caps->period;
        if (m->web_reports) {
            caps->pos_source = YAU_POS_DEVICE;
            /* the browser's claim: contextTime is the frame at the output */
            caps->os_latency_ns = 0;
            caps->os_stream_latency_ns = -1;
            snprintf(caps->os_latency_src, sizeof caps->os_latency_src, "web:getOutputTimestamp");
        }
    }
#endif
#if defined(MA_SUPPORT_WASAPI)
    if (m->backend == ma_backend_wasapi && m->dev.wasapi.pAudioClientPlayback) {
        yau__IAudioClient* ac = (yau__IAudioClient*)m->dev.wasapi.pAudioClientPlayback;
        void* clk = NULL;
        UINT64 freq = 0;
        LONGLONG lat = -1;   /* a sentinel: S_OK with an unwritten value must not read as 0 */
        if (m->dev.wasapi.actualBufferSizeInFramesPlayback)
            caps->buffer = (int32_t)m->dev.wasapi.actualBufferSizeInFramesPlayback;
        /* Microsoft documents GetPosition's position as "the stream position
         * of the sample that is currently playing through the speakers", so
         * the OS's claim from the position point to the output is 0.
         * GetStreamLatency is "the maximum latency for the current stream". */
        if (SUCCEEDED(ac->lpVtbl->GetStreamLatency(ac, &lat)) && lat >= 0) caps->os_stream_latency_ns = (int64_t)lat * 100;
        else caps->os_stream_latency_ns = -1;
        if (SUCCEEDED(ac->lpVtbl->GetService(ac, &yau__IID_IAudioClock, &clk)) && clk) {
            m->clock = (yau__IAudioClock*)clk;
            if (SUCCEEDED(m->clock->lpVtbl->GetFrequency(m->clock, &freq)) && freq) {
                m->freq = freq;
                caps->pos_source = YAU_POS_DEVICE;
                /* shared mode: tier 2 on the conditions the digital
                 * loopback test checked (STATUS); exclusive mode cannot be
                 * loopback-captured */
                caps->tier = caps->exclusive ? YAU_TIER_3 : YAU_TIER_2;
                caps->os_latency_ns = 0;
                snprintf(caps->os_latency_src, sizeof caps->os_latency_src, "wasapi:iaudioclock");
            }
        }
        {
            int32_t pad = YAU__QUEUE_PAD(in->queue, caps->period, caps->buffer, caps->exclusive);
            if (pad >= 0) {
                m->gate = 1;
                m->gate_pad = (uint32_t)pad;
                caps->queue = in->queue;
            }
        }
    }
#endif
    return 0;
}

#if defined(MA_SUPPORT_COREAUDIO) && defined(MA_APPLE_DESKTOP)
/* kAudioDevicePropertyLatency + kAudioDevicePropertySafetyOffset of the
 * output scope: the device's claim from the time stamp CoreAudio gives a
 * render callback to the output; the buffer frame size comes on top for
 * the stream figure. Read once at open. Compiled on macOS by CI; it has not
 * run (STATUS). */
static void yau__coreaudio_claims(yau__ma* m, yau_device_caps* caps) {
    AudioObjectPropertyAddress a;
    UInt32 lat = 0, safe = 0, fsz = 0, n;
    AudioObjectID id = (AudioObjectID)m->dev.coreaudio.deviceObjectIDPlayback;
    ma_AudioObjectGetPropertyData_proc get = (ma_AudioObjectGetPropertyData_proc)m->ctx.coreaudio.AudioObjectGetPropertyData;
    double ns = 1e9 / (double)caps->rate;
    if (!get || !id) return;
    a.mScope = kAudioObjectPropertyScopeOutput;
    a.mElement = 0;
    a.mSelector = kAudioDevicePropertyLatency;       n = sizeof lat;  if (get(id, &a, 0, NULL, &n, &lat) != noErr) return;
    a.mSelector = kAudioDevicePropertySafetyOffset;  n = sizeof safe; if (get(id, &a, 0, NULL, &n, &safe) != noErr) return;
    a.mSelector = kAudioDevicePropertyBufferFrameSize; n = sizeof fsz; if (get(id, &a, 0, NULL, &n, &fsz) != noErr) fsz = 0;
    caps->os_latency_ns = (int64_t)((double)(lat + safe) * ns + 0.5);
    caps->os_stream_latency_ns = (int64_t)((double)(lat + safe + fsz) * ns + 0.5);
    snprintf(caps->os_latency_src, sizeof caps->os_latency_src, "coreaudio:ltnc+saft");
}
#endif

static int yau__ma_start(void* ctx, yau_render_fn render, void* host) {
    yau__ma* m = (yau__ma*)ctx;
    ma_result r;
    m->render = render;
    m->host = host;
    r = ma_device_start(&m->dev);
    return r == MA_SUCCESS ? 0 : YAU_ERR_LOST;
}

static void yau__ma_stop(void* ctx) {
    yau__ma* m = (yau__ma*)ctx;
    m->closing = 1;
    if (m->dev_up) (void)ma_device_stop(&m->dev);
}

static void yau__ma_close(void* ctx) {
    yau__ma* m = (yau__ma*)ctx;
    m->closing = 1;
#if defined(MA_SUPPORT_WASAPI)
    if (m->clock) { m->clock->lpVtbl->Release(m->clock); m->clock = NULL; }
#endif
#if defined(MA_HAS_WEBAUDIO)
    if (m->dev_up && m->backend == ma_backend_webaudio) yau__web_detach(m->web_idx);
#endif
    if (m->dev_up) { ma_device_uninit(&m->dev); m->dev_up = 0; }
    if (m->ctx_up) { ma_context_uninit(&m->ctx); m->ctx_up = 0; }
}

static int yau__ma_describe(void* ctx, char* buf, size_t cap) {
    yau__ma* m = (yau__ma*)ctx;
#if defined(MA_HAS_WEBAUDIO)
    if (m->backend == ma_backend_webaudio) {
        double base = m->pub.base_s, lat = m->pub.out_s;
        return snprintf(buf, cap, "%s path=%s reports=%s base_latency=%.1fms output_latency=%.1fms block_moves=%u",
                        ma_get_backend_name(m->backend), m->web_worklet ? "audioworklet" : "scriptprocessor",
                        m->web_reports ? "getOutputTimestamp" : "none", base >= 0 ? base * 1e3 : -1.0,
                        lat >= 0 ? lat * 1e3 : -1.0, (unsigned)m->web_moves);
    }
#endif
    return snprintf(buf, cap, "%s", ma_get_backend_name(m->backend));
}

static const yau_device yau__ma_device = {
    YAU_DEVICE_VERSION, "miniaudio",
    yau__ma_open, yau__ma_start, yau__ma_stop, yau__ma_close, yau__ma_describe
};

static int yau__ma_backend(yau_backend b, ma_backend* out) {
    switch (b) {
    case YAU_BACKEND_AUTO:
#if defined(_WIN32)
        *out = ma_backend_wasapi;
#elif defined(__APPLE__)
        *out = ma_backend_coreaudio;
#elif defined(__EMSCRIPTEN__)
        *out = ma_backend_webaudio;
#else
        *out = ma_backend_alsa;
#endif
        return 1;
    case YAU_BACKEND_WASAPI:    *out = ma_backend_wasapi; return 1;
    case YAU_BACKEND_COREAUDIO: *out = ma_backend_coreaudio; return 1;
    case YAU_BACKEND_ALSA:      *out = ma_backend_alsa; return 1;
    case YAU_BACKEND_PULSE:     *out = ma_backend_pulseaudio; return 1;
    case YAU_BACKEND_WEB:       *out = ma_backend_webaudio; return 1;
    case YAU_BACKEND_NULL:      *out = ma_backend_null; return 1;
    default: return 0;
    }
}

#endif /* !YAU_NO_MINIAUDIO */

/* --- frame thread ------------------------------------------------------------- */

static void yau__res_put(yau_audio* au, const yau_onset* rec, int stage) {
    yau__res* r = &au->res[(uint64_t)rec->id % YAU__RESULTS];
    if (r->rec.id != rec->id) return;
    if (stage == YAU__M_ONSET) {
        int64_t endf = r->rec.end_frame, gapf = r->rec.gap_frames;
        r->rec = *rec;
        r->rec.end_frame = endf;
        /* an END that came first has the final count */
        if (r->ended) r->rec.gap_frames = gapf;
        r->rec.flags = (uint16_t)(r->rec.flags | r->end_flags);
        r->used = 2;
    } else {
        r->rec.end_frame = rec->end_frame;
        r->rec.gap_frames = rec->gap_frames;
        r->end_flags = rec->flags;
        if (r->used == 2) r->rec.flags = (uint16_t)(r->rec.flags | rec->flags);
        r->ended = 1;
    }
}

static int yau__update_rc(yau_audio* au) {
    uint32_t head, tail;
    int n = 0;
    if (!au || !au->open) return YAU_ERR_CLOSED;
    head = yau__ld32(&au->msg_head);
    tail = au->msg_tail;
    while (tail != head) {
        const yau__msg* m = &au->msg[tail % YAU__MSGS];
        switch (m->kind) {
        case YAU__M_ONSET:
            yau__res_put(au, &m->rec, YAU__M_ONSET);
            if (m->rec.tier > au->worst_tier_ft) au->worst_tier_ft = m->rec.tier;
            n++;
            break;
        case YAU__M_END: yau__res_put(au, &m->rec, YAU__M_END); break;
        case YAU__M_FIT: au->fit_ft = m->fit; break;
        case YAU__M_XRUN: au->xruns_ft++; break;
        default: break;
        }
        if (m->freed) {
            au->outstanding--;
            if (m->in_arena) au->arena_live--;
        }
        tail++;
    }
    yau__st32(&au->msg_tail, tail);
    if (yau__ld32(&au->lost)) return YAU_ERR_LOST;
    return n;
}

static int yau__project_channels_ok(const yau_audio* au, uint16_t ch) {
    return ch == 1 || ch == au->dcaps.channels;
}

static yau_id yau__play_stream(yau_audio* au, const yau_play_desc* d) {
    yau__cmd c;
    yau__res* r;
    yau_stream* st = d->stream;
    if (d->buf.frames || d->buf.n || d->offset || d->frames || d->loops || d->at_ns < 0
        || st->au_ != au || !st->mem_) return YAU_ERR_ARG;
    if (au->outstanding >= au->voices_max) return YAU_ERR_FULL;
    /* the claim: a reset cannot run from here until the play has ended */
    if (!yau__cas32(&st->state_, YAU_STREAM_IDLE, YAU_STREAM_QUEUED)) return YAU_ERR_BUSY;
    memset(&c, 0, sizeof c);
    c.op = YAU__OP_PLAY;
    c.id = au->next_id + 1;
    c.t = d->at_ns;
    c.db = d->db;
    c.ch = st->ch_;
    c.buf_id = st->buf_id_;
    c.st = st;
    c.mask = d->channels ? d->channels : ~(uint64_t)0;
    yau__st64(&st->id_, c.id);
    if (yau__cmd_push(au, &c) < 0) {
        yau__st32(&st->state_, YAU_STREAM_IDLE);
        return YAU_ERR_FULL;
    }
    au->next_id++;
    au->outstanding++;
    r = &au->res[(uint64_t)c.id % YAU__RESULTS];
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->rec.id = c.id;
    r->rec.target = d->at_ns;
    r->rec.onset = d->at_ns;
    r->rec.start_frame = -1;
    r->rec.device_pos = -1;
    r->rec.buffer_id = st->buf_id_;
    r->rec.flags = YAU_ONSET_PENDING;
    r->rec.sample = -1;
    return c.id;
}

static yau_id yau__play_rc(yau_audio* au, const yau_play_desc* d) {
    yau__cmd c;
    yau__res* r;
    int64_t len;
    if (!au || !d) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    if (yau__update_rc(au) == YAU_ERR_LOST) return YAU_ERR_LOST;
    if (d->stream) return yau__play_stream(au, d);
    if (!d->buf.frames || d->buf.n <= 0 || d->offset < 0 || d->offset >= d->buf.n
        || d->frames < 0 || d->at_ns < 0 || d->loops < YAU_FOREVER) return YAU_ERR_ARG;
    if (!yau__project_channels_ok(au, d->buf.channels)) return YAU_ERR_FORMAT;
    len = d->frames ? d->frames : d->buf.n - d->offset;
    if (d->offset + len > d->buf.n) return YAU_ERR_ARG;
    if (au->outstanding >= au->voices_max) return YAU_ERR_FULL;
    memset(&c, 0, sizeof c);
    c.op = YAU__OP_PLAY;
    c.id = au->next_id + 1;
    c.t = d->at_ns;
    c.db = d->db;
    c.ch = d->buf.channels;
    c.buf_id = d->buf.id;
    c.frames = d->buf.frames;
    c.first = d->offset;
    c.len = len;
    c.loops = d->loops;
    c.mask = d->channels ? d->channels : ~(uint64_t)0;
    c.in_arena = (uint16_t)(au->arena && (unsigned char*)d->buf.frames >= au->arena
                            && (unsigned char*)d->buf.frames < au->arena + au->arena_cap);
    if (yau__cmd_push(au, &c) < 0) return YAU_ERR_FULL;
    au->next_id++;
    au->outstanding++;
    if (c.in_arena) au->arena_live++;
    r = &au->res[(uint64_t)c.id % YAU__RESULTS];
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->rec.id = c.id;
    r->rec.target = d->at_ns;
    r->rec.onset = d->at_ns;
    r->rec.start_frame = -1;
    r->rec.device_pos = -1;
    r->rec.buffer_id = d->buf.id;
    r->rec.flags = YAU_ONSET_PENDING;
    r->rec.sample = d->offset;
    return c.id;
}

static yau_id yau__play_at_rc(yau_audio* au, yau_buf buf, int64_t t) {
    yau_play_desc d;
    memset(&d, 0, sizeof d);
    d.buf = buf;
    d.at_ns = t;
    return yau__play_rc(au, &d);
}

static yau_id yau__play_stream_rc(yau_audio* au, yau_stream* st, int64_t t) {
    yau_play_desc d;
    if (!st) return YAU_ERR_ARG;
    memset(&d, 0, sizeof d);
    d.stream = st;
    d.at_ns = t;
    return yau__play_rc(au, &d);
}

/* --- streams: setup and the producer ------------------------------------------ */

static float* yau__arena_take(yau_audio* au, int64_t floats);

static void yau__stream_clear(yau_stream* st, int64_t first) {
    yau__st64(&st->w_, first);
    yau__st64(&st->end_, -1);
    yau__st64(&st->r_, first);
    yau__st64(&st->next_, first);
    yau__st64(&st->origin_, YAU__NO_ORIGIN);
    yau__st64(&st->gap_frames_, 0);
    yau__st64(&st->discarded_, 0);
    yau__st64(&st->plan_, -1);
    yau__st64(&st->first_, first);
    yau__st64(&st->id_, 0);
    yau__st32(&st->gaps_, 0);
    st->skip_ = 0;
    st->gap_open_ = 0;
    st->gap_f0_ = st->gap_n_ = st->gap_s0_ = 0;
    st->gap_xr_ = 0;
}

static int yau__stream_open_rc(yau_audio* au, yau_stream* st, const yau_stream_desc* d) {
    int64_t cap, pre;
    uint16_t ch;
    float* mem;
    if (!au || !st || !d) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    if (st->au_ && st->mem_ && st->state_ != YAU_STREAM_IDLE && st->state_ != YAU_STREAM_ENDED) return YAU_ERR_BUSY;
    cap = d->frames ? d->frames : (int64_t)au->dcaps.rate;
    ch = d->channels ? d->channels : au->dcaps.channels;
    pre = d->preroll;
    if (cap < 2 * (int64_t)au->dcaps.period || d->first < 0 || pre < YAU_PREROLL_NONE || pre > cap) {
        yau__err(au, "ysp_audio: a stream needs at least 2 periods (%d frames) of ring, first >= 0 "
                   "and preroll <= the ring", 2 * (int)au->dcaps.period);
        return YAU_ERR_ARG;
    }
    if (!yau__project_channels_ok(au, ch)) return YAU_ERR_FORMAT;
    if (st->au_ == au && st->in_arena_) { au->arena_streams--; }
    if (d->memory) {
        mem = d->memory;
    } else {
        mem = yau__arena_take(au, cap * ch);
        if (!mem) return YAU_ERR_FULL;
    }
    memset(st, 0, sizeof *st);
    st->mem_ = mem;
    st->cap_ = cap;
    st->ch_ = ch;
    st->buf_id_ = d->id;
    st->preroll_ = pre;
    st->au_ = au;
    st->in_arena_ = (uint16_t)(d->memory ? 0 : 1);
    if (st->in_arena_) au->arena_streams++;
    yau__stream_clear(st, d->first);
    yau__st32(&st->state_, YAU_STREAM_IDLE);
    return 0;
}

static int yau__cancel_rc(yau_audio* au, yau_id id);

YAU_API bool yau_stream_open(yau_audio* au, yau_stream* st, const yau_stream_desc* d) {
    uint32_t n0 = YAU__N0(au);
    int rc = yau__stream_open_rc(au, st, d);
    if (rc < 0) (void)yau__fail(au, n0, rc, "yau_stream_open");
    return rc == 0;
}

/* The device reads a playing stream's struct and ring on every block, so
 * close cancels the play and waits for the device to end it (it stores
 * ENDED last, yau__stream_detach) before it lets go: the device's buffer
 * and two periods, 20 ms to 1 s. A device that does not answer in that
 * time (stopped, or a CUSTOM device whose render runs on this thread)
 * leaves the stream detached with its memory in place: the device may
 * still read it, so its ring stays valid and an arena ring stays held
 * until yau_close(). A lost device reads nothing more. */
YAU_API void yau_stream_close(yau_audio* au, yau_stream* st) {
    uint32_t s;
    if (!au || !st || !st->mem_ || st->au_ != au) return;
    s = yau__ld32(&st->state_);
    if (s != YAU_STREAM_IDLE && s != YAU_STREAM_ENDED && au->open && !yau__ld32(&au->lost)) {
        int64_t id = yau__ld64(&st->id_), rate = au->dcaps.rate, wait_ns = 0, end;
        if (id > 0) (void)yau__cancel_rc(au, id);
        if (rate > 0)
            wait_ns = ((int64_t)au->dcaps.buffer + 2 * (int64_t)au->dcaps.period) * 1000000000 / rate;
        if (wait_ns < 20000000) wait_ns = 20000000;
        if (wait_ns > 1000000000) wait_ns = 1000000000;
        end = YAU__NOW() + wait_ns;
        for (;;) {
            (void)yau__update_rc(au);
            s = yau__ld32(&st->state_);
            if (s == YAU_STREAM_IDLE || s == YAU_STREAM_ENDED || yau__ld32(&au->lost)) break;
            if (YAU__NOW() >= end) {
                st->au_ = NULL;   /* detached: memory and hold stay (above) */
                return;
            }
            YAU__SLEEP_UNTIL(YAU__NOW() + 1000000);
        }
    }
    if (st->in_arena_) au->arena_streams--;
    st->in_arena_ = 0;
    st->mem_ = NULL;
    st->au_ = NULL;
}

YAU_API float* yau_stream_acquire(yau_stream* st, int64_t* frames) {
    int64_t w, r, space, at, n;
    if (frames) *frames = 0;
    if (!st || !st->mem_ || !frames) return NULL;
    w = yau__ld64(&st->w_);
    r = yau__ld64(&st->r_);
    space = st->cap_ - (w - r);
    if (space <= 0 || yau__ld64(&st->end_) >= 0) return NULL;
    at = w % st->cap_;
    n = st->cap_ - at < space ? st->cap_ - at : space;
    *frames = n;
    return st->mem_ + at * st->ch_;
}

YAU_API int yau_stream_commit(yau_stream* st, int64_t frames) {
    int64_t w, r, space, at, n;
    if (!st || !st->mem_ || frames < 0) return YAU_ERR_ARG;
    w = yau__ld64(&st->w_);
    r = yau__ld64(&st->r_);
    space = st->cap_ - (w - r);
    at = w % st->cap_;
    n = st->cap_ - at < space ? st->cap_ - at : space;
    if (frames > n || yau__ld64(&st->end_) >= 0) return YAU_ERR_ARG;
    /* the samples first, then the index that hands them over */
    yau__st64(&st->w_, w + frames);
    return 0;
}

YAU_API int64_t yau_stream_write(yau_stream* st, const float* src, int64_t frames) {
    int64_t done = 0, n;
    float* p;
    if (!st || !src || frames < 0) return YAU_ERR_ARG;
    while (done < frames && (p = yau_stream_acquire(st, &n)) != NULL) {
        if (n > frames - done) n = frames - done;
        memcpy(p, src + done * st->ch_, (size_t)(n * st->ch_) * sizeof(float));
        (void)yau_stream_commit(st, n);
        done += n;
    }
    return done;
}

YAU_API int yau_stream_end(yau_stream* st) {
    if (!st || !st->mem_) return YAU_ERR_ARG;
    yau__st64(&st->end_, yau__ld64(&st->w_));
    return 0;
}

YAU_API int yau_stream_reset(yau_stream* st, int64_t first) {
    uint32_t s;
    if (!st || !st->mem_ || first < 0) return YAU_ERR_ARG;
    s = yau__ld32(&st->state_);
    if (s != YAU_STREAM_IDLE && s != YAU_STREAM_ENDED) return YAU_ERR_BUSY;
    /* claimed first, so a play cannot take the stream half rewritten */
    if (!yau__cas32(&st->state_, s, YAU__STREAM_RESETTING)) return YAU_ERR_BUSY;
    yau__stream_clear(st, first);
    yau__st32(&st->state_, YAU_STREAM_IDLE);
    return 0;
}

YAU_API int yau_stream_get_info(const yau_stream* st, yau_stream_info* out) {
    uint32_t s;
    int64_t origin, wpub;
    if (!out) return YAU_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!st || !st->mem_) return YAU_ERR_ARG;
    s = yau__ld32(&st->state_);
    out->state = s == YAU__STREAM_RESETTING ? YAU_STREAM_QUEUED : (int32_t)s;
    out->id = yau__ld64(&st->id_);
    out->first = yau__ld64(&st->first_);
    out->end = yau__ld64(&st->end_);
    out->written = yau__ld64(&st->w_);
    out->read = yau__ld64(&st->r_);
    out->fill = out->written - out->read;
    out->preroll = yau__stream_preroll(st);
    out->ready = out->fill >= out->preroll || (out->end >= 0 && out->written >= out->end);
    out->next = yau__ld64(&st->next_);
    origin = yau__ld64(&st->origin_);
    out->started = origin != YAU__NO_ORIGIN;
    out->origin = out->started ? origin : 0;
    out->start_frame = yau__ld64(&st->plan_);
    wpub = st->au_ ? yau__ld64(&st->au_->w_pub) : 0;
    out->frames_to_start = out->start_frame >= 0 ? out->start_frame - wpub : 0;
    out->gaps = yau__ld32(&st->gaps_);
    out->gap_frames = yau__ld64(&st->gap_frames_);
    out->discarded = yau__ld64(&st->discarded_);
    return 0;
}

static int yau__stream_sample_at_rc(yau_audio* au, const yau_stream* st, int64_t t, int64_t* sample) {
    int64_t origin;
    if (!au || !st || !sample) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    origin = yau__ld64(&st->origin_);
    if (origin == YAU__NO_ORIGIN || au->fit_ft.k <= 0) return YAU_PENDING;
    *sample = yau__fit_frame(&au->fit_ft, t) - origin;
    return YAU_OK;
}

static int yau__stream_time_of_rc(yau_audio* au, const yau_stream* st, int64_t sample, int64_t* t) {
    int64_t origin;
    if (!au || !st || !t) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    origin = yau__ld64(&st->origin_);
    if (origin == YAU__NO_ORIGIN || au->fit_ft.k <= 0) return YAU_PENDING;
    *t = yau__fit_time(&au->fit_ft, origin + sample);
    return YAU_OK;
}

static int yau__send(yau_audio* au, int op, yau_id id, int64_t t, int64_t ramp, float db) {
    yau__cmd c;
    if (!au) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    if (id < 0 || t < 0 || ramp < 0) return YAU_ERR_ARG;
    if (yau__ld32(&au->lost)) return YAU_ERR_LOST;
    memset(&c, 0, sizeof c);
    c.op = op; c.id = id; c.t = t; c.ramp = ramp; c.db = db;
    return yau__cmd_push(au, &c);
}

static int yau__cancel_rc(yau_audio* au, yau_id id) {
    if (id <= 0) return YAU_ERR_ARG;
    return yau__send(au, YAU__OP_CANCEL, id, 0, 0, 0);
}
static int yau__stop_at_rc(yau_audio* au, yau_id id, int64_t t, int64_t ramp_ns) {
    return yau__send(au, YAU__OP_STOP, id, t, ramp_ns, 0);
}
static int yau__gain_at_rc(yau_audio* au, yau_id id, int64_t t, float db, int64_t ramp_ns) {
    return yau__send(au, YAU__OP_GAIN, id, t, ramp_ns, db);
}

static int yau__result_rc(yau_audio* au, yau_id id, yau_onset* out) {
    yau__res* r;
    if (!au || id <= 0) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    (void)yau__update_rc(au);
    r = &au->res[(uint64_t)id % YAU__RESULTS];
    if (!r->used || r->rec.id != id) return YAU_ERR_NOT_FOUND;
    if (out) *out = r->rec;
    return r->used == 2 ? YAU_OK : YAU_PENDING;
}

static int yau__wait_rc(yau_audio* au, yau_id id, int64_t timeout_ns, yau_onset* out) {
    int64_t end = YAU__NOW() + timeout_ns;
    for (;;) {
        int rc = yau__result_rc(au, id, out);
        if (rc != YAU_PENDING) return rc;
        if (yau__ld32(&au->lost)) return YAU_ERR_LOST;
        if (YAU__NOW() >= end) return YAU_ERR_TIMEOUT;
        YAU__SLEEP_UNTIL(YAU__NOW() + 1000000);
    }
}

YAU_API int64_t yau_frame_at(const yau_audio* au, int64_t t) {
    if (!au || !au->open || au->fit_ft.k <= 0) return -1;
    return yau__fit_frame(&au->fit_ft, t);
}
YAU_API int64_t yau_time_of(const yau_audio* au, int64_t frame) {
    if (!au || !au->open || au->fit_ft.k <= 0) return -1;
    return yau__fit_time(&au->fit_ft, frame);
}

YAU_API int64_t yau_lead_ns(const yau_audio* au) {
    int64_t w;
    if (!au || !au->open || au->fit_ft.k <= 0) return 0;
    w = yau__ld64(&au->w_pub);
    return yau__fit_time(&au->fit_ft, w + au->dcaps.period) - YAU__NOW();
}

YAU_API bool yau_is_open(const yau_audio* au) { return au && au->open; }
YAU_API const char* yau_error(const yau_audio* au) { return au ? au->error : ""; }

YAU_API void yau_get_caps(const yau_audio* au, yau_caps* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!au || !au->open) return;
    out->backend = au->desc.backend;
    out->rate = au->dcaps.rate;
    out->channels = au->dcaps.channels;
    out->out = au->dcaps.out;
    out->period = au->dcaps.period;
    out->buffer = au->dcaps.buffer;
    out->exclusive = au->dcaps.exclusive;
    out->low_latency = au->dcaps.low_latency;
    out->pos_source = au->dcaps.pos_source;
    out->drift_ppm = au->fit_ft.k > 0 ? (yau__k_nom(au) / au->fit_ft.k - 1.0) * 1e6 : 0;
    out->fit_spread_ns = au->fit_ft.spread;
    out->fit_points = au->fit_ft.n;
    out->lead_ns = yau_lead_ns(au);
    out->xruns = au->xruns_ft;
    out->worst_tier = (yau_tier)au->worst_tier_ft;
    memcpy(out->map, au->dcaps.map, sizeof out->map);
    memcpy(out->name, au->dcaps.name, sizeof out->name);
    out->os_latency_ns = au->dcaps.os_latency_ns;
    out->os_stream_latency_ns = au->dcaps.os_stream_latency_ns;
    memcpy(out->os_latency_src, au->dcaps.os_latency_src, sizeof out->os_latency_src);
    out->gaps = yau__ld32(&au->gaps_pub);
    out->queue = au->dcaps.queue;
}

static const char* yau__out_name(int o) {
    switch (o) {
    case YAU_OUT_F32: return "f32";
    case YAU_OUT_S16: return "s16";
    case YAU_OUT_S24: return "s24";
    case YAU_OUT_S32: return "s32";
    default: return "?";
    }
}

YAU_API int yau_describe(const yau_audio* au, char* buf, size_t cap) {
    yau_caps c;
    char be[160] = "", claim[96];   /* the web device's is about 120 bytes */
    if (!buf || cap == 0) return 0;
    if (!au || !au->open) return snprintf(buf, cap, "ysp_audio: closed");
    yau_get_caps(au, &c);
    if (c.os_latency_src[0])
        snprintf(claim, sizeof claim, "%s:%.3fms,stream:%.3fms", c.os_latency_src,
                 (double)c.os_latency_ns / 1e6, (double)c.os_stream_latency_ns / 1e6);
    else
        snprintf(claim, sizeof claim, "none");
    if (au->dev && au->dev->describe) (void)au->dev->describe(au->dev_ctx, be, sizeof be);
    else snprintf(be, sizeof be, "%s", au->dev && au->dev->name ? au->dev->name : "?");
    return snprintf(buf, cap,
        "ysp_audio: backend=%s device='%s' rate=%u ch=%u out=%s period=%d buffer=%d queue=%d "
        "share=%s%s pos=%s drift=%+.2fppm spread=%.0fns lead=%.2fms xruns=%u gaps=%u worst_tier=%d "
        "os_claim=%s%s",
        be, c.name, (unsigned)c.rate, (unsigned)c.channels, yau__out_name(c.out),
        (int)c.period, (int)c.buffer, (int)c.queue, c.exclusive ? "exclusive" : "shared",
        c.low_latency ? "(ac3)" : "", c.pos_source == YAU_POS_DEVICE ? "device" : "callback",
        c.drift_ppm, c.fit_spread_ns, (double)c.lead_ns / 1e6, (unsigned)c.xruns,
        (unsigned)c.gaps, (int)c.worst_tier, claim, yau__ld32(&au->lost) ? " LOST" : "");
}

/* --- open and close -------------------------------------------------------------- */

static int yau__resolution(int out, int bits) {
    if (bits) return bits;
    switch (out) {
    case YAU_OUT_F32: return 24;
    case YAU_OUT_S16: return 16;
    case YAU_OUT_S24: return 24;
    case YAU_OUT_S32: return 32;
    default: return 0;
    }
}

YAU_API bool yau_open(yau_audio* au, const yau_desc* desc) {
    yau_desc d;
    yau_device_open in;
    int rc, i, need;
    if (!au) return false;
    if (au->open) yau_close(au);
    memset(au, 0, sizeof *au);
    if (!desc) { yau__err(au, "ysp_audio: desc is NULL"); return false; }
    d = *desc;
    if (!d.format.rate) d.format.rate = 48000;
    if (!d.format.channels) d.format.channels = 2;
    if (!d.voices) d.voices = 32;
    if (d.voices < 0 || d.voices > YAU_MAX_VOICES) { yau__err(au, "ysp_audio: voices must be 1..%d", YAU_MAX_VOICES); return false; }
    if (d.format.channels > YAU_MAX_CHANNELS || d.format.sample > YAU_S24 || d.format.rate > 768000
        || d.period < 0 || d.min_tier < 0 || d.min_tier > 3 || d.device_index > 7 || d.queue < 0 || d.queue > 2) {
        yau__err(au, "ysp_audio: a desc field is out of range (channels <= %d, sample, rate, "
                   "period >= 0, min_tier 0..3, device_index 0..7, queue 0..2)", YAU_MAX_CHANNELS);
        return false;
    }
    if (d.source && d.source->version != YAU_SOURCE_VERSION) { yau__err(au, "ysp_audio: source version %u, header wants %d", d.source->version, YAU_SOURCE_VERSION); return false; }
    if (!d.period) d.period = (int32_t)(d.format.rate / 100);
    if (!d.queue) d.queue = 2;
    au->desc = d;
    au->fmt = d.format;
    au->voices_max = d.voices;
    au->first_block = 1;
    au->next_id = 0;

    if (d.backend == YAU_BACKEND_CUSTOM) {
        if (!d.dev || d.dev->version != YAU_DEVICE_VERSION || !d.dev->open || !d.dev->start) {
            yau__err(au, "ysp_audio: BACKEND_CUSTOM needs desc.dev with version %d, open and start", YAU_DEVICE_VERSION);
            return false;
        }
        au->dev = d.dev;
        au->dev_ctx = d.dev_ctx;
    } else {
#if defined(YAU_NO_MINIAUDIO)
        yau__err(au, "ysp_audio: built with YAU_NO_MINIAUDIO: only BACKEND_CUSTOM opens");
        return false;
#else
        yau__ma* m = (yau__ma*)(void*)au->backend_mem;
        if (!yau__ma_backend(d.backend, &m->backend)) { yau__err(au, "ysp_audio: unknown backend %d", (int)d.backend); return false; }
        m->au = au;
        au->dev = &yau__ma_device;
        au->dev_ctx = m;
#endif
    }
    memset(&in, 0, sizeof in);
    in.format = &au->fmt;
    in.period = d.period;
    in.exclusive = d.exclusive;
    in.device = d.device;
    in.queue = d.queue;
    rc = au->dev->open(au->dev_ctx, &in, &au->dcaps, au->error, sizeof au->error);
    if (rc < 0) {
        if (!au->error[0]) yau__err(au, "ysp_audio: the device did not open (%s)", yau_strerror(rc));
        if (au->dev->close) au->dev->close(au->dev_ctx);
        return false;
    }
    /* STRICT OPEN */
    need = d.format.sample == YAU_S16 ? 16 : 24;
    if (au->dcaps.rate != d.format.rate) {
        yau__err(au, "ysp_audio: device '%s' runs at %u Hz; the project is %u Hz. On Windows the "
                   "rate is the device's Default Format (Sound > the device > Properties > Advanced), "
                   "in exclusive mode too. Change it there or change the project rate.",
                   au->dcaps.name, (unsigned)au->dcaps.rate, (unsigned)d.format.rate);
        rc = -1;
    } else if (au->dcaps.channels != d.format.channels) {
        yau__err(au, "ysp_audio: device '%s' has %u channels; the project has %u. Change the "
                   "speaker setup or the project.", au->dcaps.name, (unsigned)au->dcaps.channels,
                   (unsigned)d.format.channels);
        rc = -1;
    } else if (yau__resolution(au->dcaps.out, au->dcaps.bits) < need) {
        yau__err(au, "ysp_audio: device '%s' takes %s samples, fewer bits than the project's; "
                   "the header does not reduce resolution", au->dcaps.name, yau__out_name(au->dcaps.out));
        rc = -1;
    } else if (d.format.map) {
        for (i = 0; i < d.format.channels; i++) {
            if (au->dcaps.map[i] != d.format.map[i]) {
                yau__err(au, "ysp_audio: device '%s' channel %d is position %u; the project map "
                           "says %u", au->dcaps.name, i, (unsigned)au->dcaps.map[i], (unsigned)d.format.map[i]);
                rc = -1;
                break;
            }
        }
    }
    if (rc >= 0 && au->dcaps.period <= 0) au->dcaps.period = d.period;
    au->dcaps.os_latency_src[sizeof au->dcaps.os_latency_src - 1] = 0;
    if (!au->dcaps.os_latency_src[0]) { au->dcaps.os_latency_ns = -1; au->dcaps.os_stream_latency_ns = -1; }
    if (rc < 0) { if (au->dev->close) au->dev->close(au->dev_ctx); return false; }

    /* the arena: the only allocation */
    if (d.arena) {
        au->arena = (unsigned char*)d.arena;
        au->arena_cap = d.arena_bytes;
    } else {
        size_t bytes = d.arena_bytes ? d.arena_bytes : (size_t)8 << 20;
        au->arena = (unsigned char*)malloc(bytes);
        if (!au->arena) { yau__err(au, "ysp_audio: no memory for a %lu-byte arena", (unsigned long)bytes); if (au->dev->close) au->dev->close(au->dev_ctx); return false; }
        au->arena_cap = bytes;
        au->arena_owned = 1;
    }
    au->open = 1;
    {
        yrt_payload u;
        memset(&u, 0, sizeof u);
        u.i32[0] = (int32_t)au->dcaps.rate;
        u.i32[1] = au->dcaps.channels;
        u.i32[2] = au->dcaps.out;
        u.i32[3] = au->dcaps.period;
        u.i32[4] = au->dcaps.buffer;
        u.i32[5] = au->dcaps.exclusive;
        u.i32[6] = au->dcaps.low_latency;
        u.i32[7] = (int32_t)d.backend;
        u.i32[8] = au->dcaps.pos_source;
        yau__ring(au, (uint16_t)YAU_EV_OPEN, YAU__NOW(), d.device_index, &u);
        memset(&u, 0, sizeof u);
        u.i64[0] = au->dcaps.os_latency_ns;
        u.i64[1] = au->dcaps.os_stream_latency_ns;
        memcpy(u.bytes + 16, au->dcaps.os_latency_src, 23);
        yau__ring(au, (uint16_t)YAU_EV_CLAIM, YAU__NOW(), d.device_index, &u);
    }
    rc = au->dev->start(au->dev_ctx, yau_render, au);
    if (rc < 0) {
        yau__err(au, "ysp_audio: device '%s' did not start", au->dcaps.name);
        yau_close(au);
        return false;
    }
    au->started = 1;
    if (d.backend != YAU_BACKEND_CUSTOM) {
        int64_t end = YAU__NOW() + 3000000000LL;
        while (!au->fit_ft.ready) {
            (void)yau__update_rc(au);
            if (au->fit_ft.ready) break;
            if (yau__ld32(&au->lost) || YAU__NOW() > end) {
                /* which of three failures: a lost device, no callbacks at
                 * all, or callbacks too irregular for the fit to warm up */
                yau__err(au, "ysp_audio: device '%s' gave no steady callbacks within 3 s "
                           "(lost=%u, fit points=%u, fits=%u)", au->dcaps.name,
                           (unsigned)yau__ld32(&au->lost), (unsigned)au->fit_ft.n, (unsigned)au->fit_ft.gen);
                yau_close(au);
                return false;
            }
            YAU__SLEEP_UNTIL(YAU__NOW() + 10000000);
        }
    }
    return true;
}

YAU_API void yau_close(yau_audio* au) {
    int i;
    if (!au || !au->open) return;
    if (au->started && au->dev->stop) au->dev->stop(au->dev_ctx);
    au->started = 0;
    /* the device thread has stopped: its state is ours now */
    (void)yau__update_rc(au);
    for (i = 0; i < YAU_MAX_VOICES; i++) {
        yau__voice* v = &au->voice[i];
        if (!v->state) continue;
        if (yau__ld32(&au->lost)) v->flags |= YAU_ONSET_LOST;
        if (v->state == 1) { yau__cancel_waiting(au, v); continue; }
        if (!v->onset_done) yau__onset_done(au, v, yau__fit_time(&au->fit, v->start), -1, 0);
        if (v->state == 2) { v->flags |= YAU_ONSET_STOPPED; yau__end_voice(au, v, au->w); }
    }
    (void)yau__update_rc(au);
    if (au->dev->close) au->dev->close(au->dev_ctx);
    if (au->arena_owned) free(au->arena);
    au->arena = NULL;
    au->arena_owned = 0;
    au->open = 0;
}

/* --- synthesis ------------------------------------------------------------------- */

YAU_API int yau_ramp(float* f, int64_t n, uint16_t ch, int64_t on, int64_t off) {
    int64_t i;
    int c;
    if (!f || n < 0 || ch == 0 || on < 0 || off < 0 || on + off > n) return YAU_ERR_ARG;
    for (i = 0; i < on; i++) {
        float w = (float)(0.5 * (1.0 - cos(3.14159265358979323846 * (double)i / (double)on)));
        for (c = 0; c < ch; c++) f[i * ch + c] *= w;
    }
    for (i = 0; i < off; i++) {
        float w = (float)(0.5 * (1.0 - cos(3.14159265358979323846 * (double)i / (double)off)));
        for (c = 0; c < ch; c++) f[(n - 1 - i) * ch + c] *= w;
    }
    return 0;
}

YAU_API int yau_mix(float* dst, int64_t dst_n, uint16_t ch, const float* src,
                        int64_t src_n, int64_t at, float gain) {
    int64_t i, n;
    if (!dst || !src || ch == 0 || dst_n < 0 || src_n < 0 || at < 0) return YAU_ERR_ARG;
    n = at >= dst_n ? 0 : (src_n < dst_n - at ? src_n : dst_n - at);
    for (i = 0; i < n * ch; i++) dst[at * ch + i] += gain * src[i];
    return 0;
}

YAU_API int yau_fill_tone(float* out, int64_t n, uint16_t ch, uint32_t rate,
                              const yau_tone_desc* d) {
    int64_t i, r;
    int c;
    double w;
    if (!out || !d || n <= 0 || ch == 0 || rate == 0) return YAU_ERR_ARG;
    if (!(d->hz > 0) || d->hz >= rate / 2.0 || !(d->peak > 0) || d->peak > 1.0f || d->ramp_ns < 0) return YAU_ERR_ARG;
    w = 2.0 * 3.14159265358979323846 * d->hz / (double)rate;
    for (i = 0; i < n; i++) {
        /* from the frame index, so the phase does not accumulate error */
        float s = (float)(d->peak * sin(w * (double)i + d->phase));
        for (c = 0; c < ch; c++) out[i * ch + c] = s;
    }
    r = yau__frames_of(d->ramp_ns, rate);
    if (2 * r > n) return YAU_ERR_ARG;
    return yau_ramp(out, n, ch, r, r);
}

static uint64_t yau__splitmix(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t yau__rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static uint64_t yau__xoshiro(uint64_t s[4]) {
    uint64_t r = s[0] + s[3], t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
    s[3] = yau__rotl(s[3], 45);
    return r;
}
/* (0, 1): the top 53 bits, never 0, so the logarithm is finite */
static double yau__unit(uint64_t s[4]) { return ((double)(yau__xoshiro(s) >> 11) + 0.5) / 9007199254740992.0; }

YAU_API int yau_fill_noise(float* out, int64_t n, uint16_t ch, uint32_t rate,
                               const yau_noise_desc* d, float* peak) {
    uint64_t s[4], x;
    int64_t i, total, r;
    float pk = 0;
    if (peak) *peak = 0;
    if (!out || !d || n <= 0 || ch == 0 || rate == 0 || !(d->rms > 0) || d->ramp_ns < 0
        || (d->dist != YAU_GAUSS && d->dist != YAU_UNIFORM)) return YAU_ERR_ARG;
    x = d->seed;
    s[0] = yau__splitmix(&x); s[1] = yau__splitmix(&x);
    s[2] = yau__splitmix(&x); s[3] = yau__splitmix(&x);
    total = n * ch;
    if (d->dist == YAU_UNIFORM) {
        double a = d->rms * 1.7320508075688772;
        for (i = 0; i < total; i++) out[i] = (float)(a * (2.0 * yau__unit(s) - 1.0));
    } else {
        for (i = 0; i < total; i += 2) {
            double u1 = yau__unit(s), u2 = yau__unit(s);
            double m = d->rms * sqrt(-2.0 * log(u1));
            out[i] = (float)(m * cos(2.0 * 3.14159265358979323846 * u2));
            if (i + 1 < total) out[i + 1] = (float)(m * sin(2.0 * 3.14159265358979323846 * u2));
        }
    }
    r = yau__frames_of(d->ramp_ns, rate);
    if (2 * r > n) return YAU_ERR_ARG;
    (void)yau_ramp(out, n, ch, r, r);
    for (i = 0; i < total; i++) { float a = out[i] < 0 ? -out[i] : out[i]; if (a > pk) pk = a; }
    if (peak) *peak = pk;
    return pk > 1.0f ? YAU_ERR_FORMAT : 0;
}

static float* yau__arena_take(yau_audio* au, int64_t floats) {
    size_t off = (au->arena_used + 63u) & ~(size_t)63u;
    size_t bytes = (size_t)floats * sizeof(float);
    if (!au->arena || floats <= 0 || off > au->arena_cap || bytes > au->arena_cap - off) {
        yau__err(au, "ysp_audio: the arena has %lu of %lu bytes free; this buffer needs %lu",
                   (unsigned long)(au->arena_cap - (off < au->arena_cap ? off : au->arena_cap)),
                   (unsigned long)au->arena_cap, (unsigned long)bytes);
        return NULL;
    }
    au->arena_used = off + bytes;
    return (float*)(void*)(au->arena + off);
}

static yau_buf yau__nobuf(void) { yau_buf b; memset(&b, 0, sizeof b); return b; }

YAU_API yau_buf yau_alloc(yau_audio* au, int64_t n, uint16_t channels) {
    yau_buf b = yau__nobuf();
    float* f;
    if (!au || !au->open || n <= 0) { if (au) yau__err(au, "ysp_audio: alloc needs an open handle and n > 0"); return b; }
    if (!channels) channels = 1;
    f = yau__arena_take(au, n * channels);
    if (!f) return b;
    memset(f, 0, (size_t)(n * channels) * sizeof(float));
    b.frames = f; b.n = n; b.channels = channels;
    return b;
}

YAU_API yau_buf yau_tone(yau_audio* au, const yau_tone_desc* d) {
    yau_buf b = yau__nobuf();
    int64_t n;
    size_t mark;
    uint16_t ch;
    if (!au || !au->open || !d) { if (au) yau__err(au, "ysp_audio: tone needs an open handle and a desc"); return b; }
    n = yau__frames_of(d->dur_ns, au->dcaps.rate);
    ch = d->channels ? d->channels : 1;
    if (n <= 0) { yau__err(au, "ysp_audio: tone .dur_ns rounds to 0 frames"); return b; }
    mark = au->arena_used;
    b = yau_alloc(au, n, ch);
    if (!b.frames) return b;
    if (yau_fill_tone(b.frames, n, ch, au->dcaps.rate, d) < 0) {
        au->arena_used = mark;
        yau__err(au, "ysp_audio: tone needs 0 < .hz < %u, 0 < .peak <= 1 and two ramps that fit in .dur_ns",
                   (unsigned)(au->dcaps.rate / 2));
        return yau__nobuf();
    }
    return b;
}

YAU_API yau_buf yau_noise(yau_audio* au, const yau_noise_desc* d) {
    yau_buf b = yau__nobuf();
    int64_t n;
    size_t mark;
    uint16_t ch;
    float pk = 0;
    int rc;
    if (!au || !au->open || !d) { if (au) yau__err(au, "ysp_audio: noise needs an open handle and a desc"); return b; }
    n = yau__frames_of(d->dur_ns, au->dcaps.rate);
    ch = d->channels ? d->channels : 1;
    if (n <= 0) { yau__err(au, "ysp_audio: noise .dur_ns rounds to 0 frames"); return b; }
    mark = au->arena_used;
    b = yau_alloc(au, n, ch);
    if (!b.frames) return b;
    rc = yau_fill_noise(b.frames, n, ch, au->dcaps.rate, d, &pk);
    if (rc == YAU_ERR_FORMAT) {
        au->arena_used = mark;
        yau__err(au, "ysp_audio: noise at rms %g peaks at %g, past full scale; the largest rms "
                   "that fits with this seed and length is %g (or use YAU_UNIFORM)",
                   (double)d->rms, (double)pk, (double)d->rms / (double)pk);
        return yau__nobuf();
    }
    if (rc < 0) {
        au->arena_used = mark;
        yau__err(au, "ysp_audio: noise needs .rms > 0, a known .dist and two ramps that fit in .dur_ns");
        return yau__nobuf();
    }
    return b;
}

YAU_API yau_buf yau_click(yau_audio* au, const yau_click_desc* d) {
    yau_buf b = yau__nobuf();
    int64_t n, i;
    if (!au || !au->open || !d) { if (au) yau__err(au, "ysp_audio: click needs an open handle and a desc"); return b; }
    if (d->peak == 0 || d->peak > 1.0f || d->peak < -1.0f || d->dur_ns < 0) { yau__err(au, "ysp_audio: click needs .peak in [-1, 1], not 0"); return b; }
    n = d->dur_ns ? yau__frames_of(d->dur_ns, au->dcaps.rate) : 1;
    if (n < 1) n = 1;
    b = yau_alloc(au, n, 1);
    if (!b.frames) return b;
    for (i = 0; i < n; i++) b.frames[i] = d->peak;
    return b;
}

static int yau__arena_reset_rc(yau_audio* au) {
    if (!au) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    (void)yau__update_rc(au);
    if (au->arena_live > 0 || au->arena_streams > 0) return YAU_ERR_BUSY;
    au->arena_used = 0;
    return 0;
}

/* --- WAV files ---------------------------------------------------------------- */

static int yau__wsrc_open(yau__wsrc* s, const yau_wav_desc* d, char* err, size_t cap) {
    int kinds = (d->path != NULL) + (d->data != NULL) + (d->reader != NULL);
    memset(s, 0, sizeof *s);
    if (kinds != 1) {
        snprintf(err, cap, "ysp_audio: a WAV desc needs exactly one of .path, .data or .reader");
        return YAU_ERR_ARG;
    }
    if (d->data) {
        s->data = (const unsigned char*)d->data;
        s->size = (int64_t)d->size;
    } else if (d->reader) {
        if (!d->reader->read || d->reader->size < 0) { snprintf(err, cap, "ysp_audio: the reader needs read and a size"); return YAU_ERR_ARG; }
        s->reader = *d->reader;
        s->ctx = d->reader_ctx;
        s->size = d->reader->size;
    } else {
        FILE* f = NULL;
#if defined(_MSC_VER)
        if (fopen_s(&f, d->path, "rb") != 0) f = NULL;
#else
        f = fopen(d->path, "rb");
#endif
        if (!f) { snprintf(err, cap, "ysp_audio: cannot open '%s'", d->path); return YAU_ERR_IO; }
        /* No stdio buffer: the C runtime would allocate one on the first
         * read, on the producer thread. Reads go straight to the OS (which
         * still caches the file); they are 16 KB at a time. */
        setvbuf(f, NULL, _IONBF, 0);
#if defined(_WIN32)
        if (_fseeki64(f, 0, SEEK_END) == 0) s->size = _ftelli64(f);
#else
        if (fseeko(f, 0, SEEK_END) == 0) s->size = (int64_t)ftello(f);
#endif
        s->fh = f;
        if (s->size < 0) { snprintf(err, cap, "ysp_audio: cannot size '%s'", d->path); fclose(f); s->fh = NULL; return YAU_ERR_IO; }
    }
    return 0;
}

static void yau__wsrc_close(yau__wsrc* s) {
    if (s->fh) fclose((FILE*)s->fh);
    memset(s, 0, sizeof *s);
}

/* n bytes at off into dst; the bytes read, or a negative code. */
static int64_t yau__wsrc_read(yau__wsrc* s, int64_t off, void* dst, int64_t n) {
    if (off < 0 || n < 0) return YAU_ERR_ARG;
    if (off >= s->size) return 0;
    if (n > s->size - off) n = s->size - off;
    if (s->data) { memcpy(dst, s->data + off, (size_t)n); return n; }
    if (s->reader.read) return s->reader.read(s->ctx, off, dst, n);
    if (s->fh) {
        FILE* f = (FILE*)s->fh;
#if defined(_WIN32)
        if (_fseeki64(f, off, SEEK_SET) != 0) return YAU_ERR_IO;
#else
        if (fseeko(f, (off_t)off, SEEK_SET) != 0) return YAU_ERR_IO;
#endif
        return (int64_t)fread(dst, 1, (size_t)n, f);
    }
    return YAU_ERR_ARG;
}

static uint32_t yau__le32(const unsigned char* b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t yau__le16(const unsigned char* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint64_t yau__le64(const unsigned char* b) { return (uint64_t)yau__le32(b) | ((uint64_t)yau__le32(b + 4) << 32); }

static int yau__wav_bytes(int format) {
    return format == YAU_WAV_S16 ? 2 : format == YAU_WAV_S24 ? 3 : 4;
}

/* RIFF, RF64 or BW64 (ds64 sizes); PCM 16 and 24, 24 in 32, float 32,
 * plain or WAVE_FORMAT_EXTENSIBLE. Everything else is refused by name: a
 * resource in another form is converted offline (rig_spec 5.2). The form
 * only; yau__wav_parse() adds the device's checks. */
static int yau__wav_scan(yau__wsrc* s, yau_wav_info* out, char* err, size_t cap) {
    static const unsigned char ext_guid_tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
    unsigned char h[64];
    int64_t pos = 12, ds64_data = -1, data_size = -1;
    int have_fmt = 0, rf64;
    uint16_t tag = 0, ch = 0, block = 0, bits = 0, valid = 0;
    uint32_t rate = 0, mask = 0;
    memset(out, 0, sizeof *out);
    if (yau__wsrc_read(s, 0, h, 12) != 12 || memcmp(h + 8, "WAVE", 4) != 0
        || (memcmp(h, "RIFF", 4) != 0 && memcmp(h, "RF64", 4) != 0 && memcmp(h, "BW64", 4) != 0)) {
        snprintf(err, cap, "ysp_audio: not a WAV file (no RIFF, RF64 or BW64 WAVE header)");
        return YAU_ERR_FORMAT;
    }
    rf64 = memcmp(h, "RIFF", 4) != 0;
    for (;;) {
        uint32_t size;
        if (yau__wsrc_read(s, pos, h, 8) != 8) {
            snprintf(err, cap, "ysp_audio: WAV file damaged: no data chunk (stopped at byte %lld)", (long long)pos);
            return YAU_ERR_FORMAT;
        }
        size = yau__le32(h + 4);
        if (!memcmp(h, "ds64", 4)) {
            if (size < 24 || yau__wsrc_read(s, pos + 8, h, 24) != 24) { snprintf(err, cap, "ysp_audio: WAV file damaged: short ds64 chunk"); return YAU_ERR_FORMAT; }
            ds64_data = (int64_t)yau__le64(h + 8);
        } else if (!memcmp(h, "fmt ", 4)) {
            int64_t n = size < 40 ? size : 40;
            if (size < 16 || yau__wsrc_read(s, pos + 8, h, n) != n) { snprintf(err, cap, "ysp_audio: WAV file damaged: short fmt chunk"); return YAU_ERR_FORMAT; }
            tag = yau__le16(h);
            ch = yau__le16(h + 2);
            rate = yau__le32(h + 4);
            block = yau__le16(h + 12);
            bits = yau__le16(h + 14);
            valid = bits;
            if (tag == 0xFFFE) {
                if (size < 40) { snprintf(err, cap, "ysp_audio: WAV file damaged: short EXTENSIBLE fmt chunk"); return YAU_ERR_FORMAT; }
                valid = yau__le16(h + 18);
                mask = yau__le32(h + 20);
                if (memcmp(h + 26, ext_guid_tail, 14) != 0) { snprintf(err, cap, "ysp_audio: WAV EXTENSIBLE subformat is not PCM or float; convert it with the pack tool"); return YAU_ERR_FORMAT; }
                tag = yau__le16(h + 24);
            }
            have_fmt = 1;
        } else if (!memcmp(h, "data", 4)) {
            if (!have_fmt) { snprintf(err, cap, "ysp_audio: WAV file damaged: data before fmt"); return YAU_ERR_FORMAT; }
            data_size = rf64 && size == 0xFFFFFFFFu ? ds64_data : (int64_t)size;
            out->data_offset = pos + 8;
            break;
        }
        pos += 8 + (int64_t)size + (size & 1u);
    }
    if (tag == 1 && bits == 16 && valid == 16) out->format = YAU_WAV_S16;
    else if (tag == 1 && bits == 24 && valid == 24) out->format = YAU_WAV_S24;
    else if (tag == 1 && bits == 32 && valid == 24) out->format = YAU_WAV_S24_32;
    else if (tag == 3 && bits == 32) out->format = YAU_WAV_F32;
    else {
        snprintf(err, cap, "ysp_audio: WAV format tag %u with %u bits (%u valid) is not 16-bit, 24-bit or "
                 "float PCM; convert it with the pack tool (32-bit integers and 64-bit floats are not exact "
                 "in float)", (unsigned)tag, (unsigned)bits, (unsigned)valid);
        return YAU_ERR_FORMAT;
    }
    if (ch == 0 || block != ch * yau__wav_bytes(out->format)) {
        snprintf(err, cap, "ysp_audio: WAV file damaged: block align %u for %u channels", (unsigned)block, (unsigned)ch);
        return YAU_ERR_FORMAT;
    }
    if (data_size < 0 || out->data_offset + data_size > s->size) {
        snprintf(err, cap, "ysp_audio: WAV file damaged: %lld data bytes at byte %lld, past the end (%lld bytes)",
                 (long long)data_size, (long long)out->data_offset, (long long)s->size);
        return YAU_ERR_FORMAT;
    }
    out->rate = rate;
    out->channels = ch;
    out->frames = data_size / block;
    out->channel_mask = mask;
    out->rf64 = rf64 && ds64_data >= 0;
    return 0;
}

static int yau__wav_parse(yau__wsrc* s, const yau_audio* au, yau_wav_info* out, char* err, size_t cap) {
    uint32_t rate, mask;
    uint16_t ch;
    int rc = yau__wav_scan(s, out, err, cap);
    if (rc < 0) return rc;
    rate = out->rate;
    ch = out->channels;
    mask = out->channel_mask;
    if (rate != au->dcaps.rate) {
        snprintf(err, cap, "ysp_audio: WAV file is %u Hz; the device runs at %u Hz. ysp_audio never "
                 "resamples: resample it offline (the pack tool)", (unsigned)rate, (unsigned)au->dcaps.rate);
        return YAU_ERR_FORMAT;
    }
    if (ch != 1 && ch != au->dcaps.channels) {
        snprintf(err, cap, "ysp_audio: WAV file has %u channels; the project has %u (1 is also accepted)",
                 (unsigned)ch, (unsigned)au->dcaps.channels);
        return YAU_ERR_FORMAT;
    }
    if (mask && ch > 1 && au->fmt.map) {
        /* the EXTENSIBLE speaker bits, in order, are positions 2.. (FL, FR,
         * FC, LFE, BL, BR, FLC, FRC, BC, SL, SR), as YAU_CH_* number them */
        int b, c = 0;
        for (b = 0; b < 32 && c < ch; b++) {
            if (!(mask & (1u << b))) continue;
            if (b > 10 || au->fmt.map[c] != (uint8_t)(b + 2)) {
                snprintf(err, cap, "ysp_audio: WAV channel %d is speaker bit %d; the project map says position %u",
                         c, b, (unsigned)au->fmt.map[c]);
                return YAU_ERR_FORMAT;
            }
            c++;
        }
    }
    return 0;
}

YAU_API int yau_wav_probe(const yau_wav_desc* d, yau_wav_info* info, char* err, size_t cap) {
    yau__wsrc src;
    char tmp[8];
    int rc;
    if (!err || !cap) { err = tmp; cap = sizeof tmp; }
    if (!d || !info) { snprintf(err, cap, "ysp_audio: yau_wav_probe needs desc and info"); return YAU_ERR_ARG; }
    rc = yau__wsrc_open(&src, d, err, cap);
    if (rc < 0) return rc;
    rc = yau__wav_scan(&src, info, err, cap);
    yau__wsrc_close(&src);
    return rc;
}

/* n samples (frames x channels) of raw bytes to float, exactly. In place
 * when dst and src overlap with dst at or before src: each value is read
 * before its float is written, and a float never reaches bytes not yet
 * read. Returns the index of the first non-finite float sample, or -1. */
static int64_t yau__widen(float* dst, const unsigned char* src, int64_t n, int format) {
    int64_t i;
    switch (format) {
    case YAU_WAV_S16:
        for (i = 0; i < n; i++) { int16_t v = (int16_t)yau__le16(src + 2 * i); dst[i] = (float)v / 32768.0f; }
        break;
    case YAU_WAV_S24:
    case YAU_WAV_S24_32: {
        int b = format == YAU_WAV_S24 ? 3 : 4, o = format == YAU_WAV_S24 ? 0 : 1;
        for (i = 0; i < n; i++) {
            const unsigned char* p = src + b * i + o;   /* the low byte of 24 in 32 is padding */
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v -= 0x1000000;
            dst[i] = (float)v / 8388608.0f;
        }
    } break;
    default:
        for (i = 0; i < n; i++) {
            uint32_t u = yau__le32(src + 4 * i);
            float x;
            memcpy(&x, &u, 4);
            if ((u & 0x7F800000u) == 0x7F800000u) return i;
            dst[i] = x;
        }
        break;
    }
    return -1;
}

YAU_API const char* yau_wav_error(const yau_wav* w) { return w ? w->error_ : ""; }

static int yau__wav_open_rc(yau_audio* au, yau_wav* w, const yau_wav_desc* d) {
    yau_stream_desc sd;
    int rc;
    if (!au || !d) return YAU_ERR_ARG;
    if (!au->open) return YAU_ERR_CLOSED;
    if (w->open_) yau_wav_close(au, w);
    memset(w, 0, sizeof *w);
    if (d->loops < YAU_FOREVER || d->start < 0 || d->ring < 0) { snprintf(w->error_, sizeof w->error_, "ysp_audio: WAV loops, start or ring out of range"); return YAU_ERR_ARG; }
    rc = yau__wsrc_open(&w->src_, d, w->error_, sizeof w->error_);
    if (rc < 0) return rc;
    rc = yau__wav_parse(&w->src_, au, &w->info, w->error_, sizeof w->error_);
    if (rc == 0 && w->info.frames == 0) { snprintf(w->error_, sizeof w->error_, "ysp_audio: WAV file has no samples"); rc = YAU_ERR_FORMAT; }
    w->total_ = d->loops == YAU_FOREVER ? -1 : w->info.frames * ((int64_t)d->loops + 1);
    if (rc == 0 && w->total_ >= 0 && d->start >= w->total_) { snprintf(w->error_, sizeof w->error_, "ysp_audio: WAV start %lld is past the end (%lld)", (long long)d->start, (long long)w->total_); rc = YAU_ERR_ARG; }
    if (rc == 0) {
        memset(&sd, 0, sizeof sd);
        sd.memory = d->memory;
        sd.frames = d->ring;
        sd.channels = w->info.channels;
        sd.id = d->id;
        sd.first = d->start;
        rc = yau__stream_open_rc(au, &w->stream, &sd);
        if (rc < 0) snprintf(w->error_, sizeof w->error_, "%.200s", au->error[0] ? au->error : "ysp_audio: the stream did not start");
    }
    if (rc < 0) { yau__wsrc_close(&w->src_); return rc; }
    w->loops_ = d->loops;
    w->pos_ = d->start;
    yau__st64(&w->seek_, -1);
    w->au_ = au;
    w->open_ = 1;
    return 0;
}

YAU_API bool yau_wav_open(yau_audio* au, yau_wav* w, const yau_wav_desc* d) {
    int rc;
    if (!w) return false;
    rc = yau__wav_open_rc(au, w, d);
    if (rc < 0 && !w->error_[0])
        snprintf(w->error_, sizeof w->error_, "ysp_audio: yau_wav_open: %s", yau_strerror(rc));
    w->error_code = rc < 0 ? rc : 0;
    return rc == 0;
}

/* Never fails: yau_stream_close() ends a play that still runs. */
YAU_API void yau_wav_close(yau_audio* au, yau_wav* w) {
    if (!au || !w || !w->open_) return;
    yau_stream_close(au, &w->stream);
    yau__wsrc_close(&w->src_);
    w->open_ = 0;
}

static int yau__wav_seek_now(yau_wav* w, int64_t sample) {
    int rc = yau_stream_reset(&w->stream, sample);
    if (rc < 0) return rc;
    w->pos_ = sample;
    yau__st64(&w->seek_, -1);
    return 0;
}

YAU_API int yau_wav_seek(yau_wav* w, int64_t sample) {
    if (!w || !w->open_ || sample < 0 || (w->total_ >= 0 && sample >= w->total_)) return YAU_ERR_ARG;
    return yau__wav_seek_now(w, sample);
}

YAU_API int64_t yau_wav_feed(yau_wav* w, int64_t max_frames) {
    int64_t done = 0, A, seek;
    int ch, bytes, block;
    if (!w || !w->open_ || max_frames < 0) return YAU_ERR_ARG;
    seek = yau__ld64(&w->seek_);
    if (seek >= 0 && yau__wav_seek_now(w, seek) < 0) return 0;   /* the play has not ended yet */
    A = w->info.frames;
    ch = w->info.channels;
    bytes = yau__wav_bytes(w->info.format);
    block = ch * bytes;
    while (max_frames == 0 || done < max_frames) {
        int64_t n, k, q, off, got, bad;
        float* p;
        if (w->total_ >= 0 && w->pos_ >= w->total_) {
            if (yau__ld64(&w->stream.end_) < 0) (void)yau_stream_end(&w->stream);
            break;
        }
        p = yau_stream_acquire(&w->stream, &n);
        if (!p) break;
        q = w->pos_ % A;                       /* a loop: sample s is file frame s mod A */
        k = n;
        if (max_frames && k > max_frames - done) k = max_frames - done;
        if (k > A - q) k = A - q;
        if (w->total_ >= 0 && k > w->total_ - w->pos_) k = w->total_ - w->pos_;
        if (k > YAU__WAV_SCRATCH / block) k = YAU__WAV_SCRATCH / block;
        off = w->info.data_offset + q * block;
        if (w->src_.data) {
            bad = yau__widen(p, w->src_.data + off, k * ch, w->info.format);
        } else {
            got = yau__wsrc_read(&w->src_, off, w->scratch_, k * block);
            if (got != k * block) {
                snprintf(w->error_, sizeof w->error_, "ysp_audio: WAV read of %lld bytes at %lld failed",
                         (long long)(k * block), (long long)off);
                return YAU_ERR_IO;
            }
            bad = yau__widen(p, w->scratch_, k * ch, w->info.format);
        }
        if (bad >= 0) {
            /* the stream ends before the first sample that is not a number */
            (void)yau_stream_commit(&w->stream, bad / ch);
            (void)yau_stream_end(&w->stream);
            snprintf(w->error_, sizeof w->error_, "ysp_audio: WAV sample %lld (channel %d) is not a finite "
                     "number; the stream ends there", (long long)(w->pos_ + bad / ch), (int)(bad % ch));
            w->pos_ = w->total_ = w->pos_ + bad / ch;
            return YAU_ERR_FORMAT;
        }
        (void)yau_stream_commit(&w->stream, k);
        w->pos_ += k;
        done += k;
    }
    return done;
}

YAU_API bool yau_wav_wants(const yau_wav* w) {
    int64_t fill;
    if (!w || !w->open_) return false;
    if (yau__ld64(&w->seek_) >= 0) return true;
    if (yau__ld64(&w->stream.end_) >= 0) return false;
    fill = yau__ld64(&w->stream.w_) - yau__ld64(&w->stream.r_);
    return fill < w->stream.cap_ / 2;
}

YAU_API bool yau_wav_step(void* wp) {
    yau_wav* w = (yau_wav*)wp;
    int64_t n = yau_wav_feed(w, 4096);
    int64_t space;
    if (n <= 0) return false;
    space = w->stream.cap_ - (yau__ld64(&w->stream.w_) - yau__ld64(&w->stream.r_));
    return space > 0 && yau__ld64(&w->stream.end_) < 0;
}

YAU_API void yau_wav_on_msg(void* wp, const void* msg, uint32_t seq) {
    yau_wav* w = (yau_wav*)wp;
    yau_wav_msg m;
    (void)seq;
    if (!w || !msg) return;
    memcpy(&m, msg, sizeof m);
    if (m.seek >= 0 && w->open_ && (w->total_ < 0 || m.seek < w->total_)) {
        /* kept until the play has ended; feed applies it */
        yau__st64(&w->seek_, m.seek);
        (void)yau__wav_seek_now(w, m.seek);
    }
}

YAU_API yau_buf yau_wav_load(yau_audio* au, const yau_wav_desc* d) {
    yau_buf b = yau__nobuf();
    yau__wsrc src;
    yau_wav_info info;
    int64_t n, raw, got = 0;
    size_t mark;
    float* f;
    unsigned char* base;
    if (!au || !au->open || !d) { if (au) yau__err(au, "ysp_audio: wav_load needs an open handle and a desc"); return b; }
    if (yau__wsrc_open(&src, d, au->error, sizeof au->error) < 0) return b;
    if (yau__wav_parse(&src, au, &info, au->error, sizeof au->error) < 0) { yau__wsrc_close(&src); return b; }
    if (info.frames == 0) { yau__err(au, "ysp_audio: WAV file has no samples"); yau__wsrc_close(&src); return b; }
    n = info.frames * info.channels;
    raw = info.frames * info.channels * yau__wav_bytes(info.format);
    mark = au->arena_used;
    f = yau__arena_take(au, n);
    if (!f) { yau__wsrc_close(&src); return b; }
    /* the raw bytes at the end of the buffer, widened forward in place */
    base = (unsigned char*)f + (size_t)(n * 4 - raw);
    while (got < raw) {
        int64_t k = yau__wsrc_read(&src, info.data_offset + got, base + got, raw - got);
        if (k <= 0) break;
        got += k;
    }
    yau__wsrc_close(&src);
    if (got != raw) { au->arena_used = mark; yau__err(au, "ysp_audio: WAV read failed at byte %lld", (long long)(info.data_offset + got)); return b; }
    {
        int64_t bad = yau__widen(f, base, n, info.format);
        if (bad >= 0) {
            au->arena_used = mark;
            yau__err(au, "ysp_audio: WAV sample %lld is not a finite number", (long long)(bad / info.channels));
            return b;
        }
    }
    b.frames = f;
    b.n = info.frames;
    b.channels = info.channels;
    b.id = d->id;
    return b;
}

YAU_API const yau_param* yau_params(int* n) {
    static const yau_param table[] = {
        { "format.rate",     "u32",  8000, 768000, 48000, "Hz",   "project sample rate; the device must run at it" },
        { "format.channels", "u32",  1, YAU_MAX_CHANNELS, 2, "",  "project channel count; the device must have it" },
        { "format.sample",   "enum", 0, 2, 0, "",                  "project resolution: 0 f32, 1 s16, 2 s24" },
        { "backend",         "enum", 0, 7, 0, "",                  "0 auto, 1 wasapi, 2 coreaudio, 3 alsa, 4 pulse, 5 web, 6 null, 7 custom" },
        { "exclusive",       "bool", 0, 1, 0, "",                  "WASAPI exclusive mode; silences every other program" },
        { "period",          "i32",  0, 65536, 480, "frame",       "frames per callback; 0 = 10 ms" },
        { "queue",           "i32",  0, 2, 2, "period",            "WASAPI shared: periods queued ahead of the engine; 1 saves a period of lead and a period of stall margin" },
        { "voices",          "i32",  1, YAU_MAX_VOICES, 32, "",  "sounds playing or waiting at once" },
        { "device_index",    "u32",  0, 7, 0, "",                  "device number in the records" },
        { "min_tier",        "i32",  0, 3, 0, "",                  "flag onsets whose tier is worse; 0 = off" },
        { "onset_offset_ns", "i64",  -1e9, 1e9, 0, "ns",           "added to every onset; from a line-in loopback test" },
        { "arena_bytes",     "u32",  0, 4294967295.0, 8388608, "B", "synthesis arena when desc.arena is NULL" },
        { "stream.frames",   "i64",  0, 1e9, 48000, "frame",       "ring capacity of a stream (yau_stream_desc); 0 = 1 s" },
        { "stream.preroll",  "i64",  -1, 1e9, 0, "frame",          "frames a stream played at 0 waits for; 0 = a quarter ring, -1 = none" },
        { "wav.loops",       "i32",  -1, 2147483647.0, 0, "",      "passes of a WAV stream after the first; -1 = forever" }
    };
    if (n) *n = (int)(sizeof table / sizeof table[0]);
    return table;
}

#ifdef __cplusplus
}
#endif

/* --- the calls that write the message (ERRORS) --------------------------- */

YAU_API int yau_update(yau_audio* au) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__update_rc(au), "yau_update");
}
YAU_API yau_id yau_play(yau_audio* au, const yau_play_desc* d) {
    uint32_t n0 = YAU__N0(au);
    return yau__fail(au, n0, yau__play_rc(au, d), "yau_play");
}
YAU_API yau_id yau_play_at(yau_audio* au, yau_buf buf, int64_t t_ns) {
    uint32_t n0 = YAU__N0(au);
    return yau__fail(au, n0, yau__play_at_rc(au, buf, t_ns), "yau_play_at");
}
YAU_API yau_id yau_play_stream(yau_audio* au, yau_stream* st, int64_t t_ns) {
    uint32_t n0 = YAU__N0(au);
    return yau__fail(au, n0, yau__play_stream_rc(au, st, t_ns), "yau_play_stream");
}
YAU_API int yau_cancel(yau_audio* au, yau_id id) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__cancel_rc(au, id), "yau_cancel");
}
YAU_API int yau_stop_at(yau_audio* au, yau_id id, int64_t t_ns, int64_t ramp_ns) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__stop_at_rc(au, id, t_ns, ramp_ns), "yau_stop_at");
}
YAU_API int yau_gain_at(yau_audio* au, yau_id id, int64_t t_ns, float db, int64_t ramp_ns) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__gain_at_rc(au, id, t_ns, db, ramp_ns), "yau_gain_at");
}
YAU_API int yau_result(yau_audio* au, yau_id id, yau_onset* out) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__result_rc(au, id, out), "yau_result");
}
YAU_API int yau_wait(yau_audio* au, yau_id id, int64_t timeout_ns, yau_onset* out) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__wait_rc(au, id, timeout_ns, out), "yau_wait");
}
YAU_API int yau_arena_reset(yau_audio* au) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__arena_reset_rc(au), "yau_arena_reset");
}
YAU_API int yau_stream_sample_at(yau_audio* au, const yau_stream* st, int64_t t_ns, int64_t* sample) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__stream_sample_at_rc(au, st, t_ns, sample), "yau_stream_sample_at");
}
YAU_API int yau_stream_time_of(yau_audio* au, const yau_stream* st, int64_t sample, int64_t* t_ns) {
    uint32_t n0 = YAU__N0(au);
    return (int)yau__fail(au, n0, yau__stream_time_of_rc(au, st, sample, t_ns), "yau_stream_time_of");
}

#endif /* YSP_AUDIO_IMPLEMENTATION_GUARD */
#endif /* YSP_AUDIO_IMPLEMENTATION */

/*
------------------------------------------------------------------------------
This software is available under the MIT No Attribution license (MIT-0), or
public domain, at your choice.

MIT No Attribution
Copyright (c) 2026 ysp contributors
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
*/
