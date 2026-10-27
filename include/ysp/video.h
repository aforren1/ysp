/* ysp/video.h - v0.5.1 - public domain single-header video playback library
 *
 *   A movie as a stimulus. On each display frame the header takes the
 *   frame's PREDICTED ONSET, asks the movie clock for the movie time at
 *   that onset, and shows the video frame due then, by ysp/timeline.h's
 *   lead rule. It never counts frames. Every display frame gets a record:
 *   the video frame, when it was due, when the flip showed it, and the
 *   decision (shown, repeated, dropped) with its reason. Frames are decoded
 *   ahead on a ysp/rt.h pump and uploaded into a ysp/gfx.h IMAGE texture.
 *   Also a frame sequence container (raw or QOI frames, read and write,
 *   value-exact), MPEG-1 through pl_mpeg, H.264 and HEVC in MP4 through
 *   Media Foundation on Windows, and the Video decoder extension interface
 *   of the rig's spec (section 12).
 *
 *   REQUIRES ysp/gfx.h (so ysp/screen.h and ysp/rt.h) and ysp/timeline.h
 *   beside it. pl_mpeg.h (commit c871f2b, one MIT file) on the include path
 *   for MPEG-1; YVID_NO_PL_MPEG builds without it. Media Foundation is an
 *   OS API, loaded at run time (BUILDING); YVID_NO_MF builds without it.
 *   ysp/audio.h is optional: include it first and yvid_follow_audio()
 *   and yvid_soundtrack() appear.
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
 *       #define YSP_VIDEO_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header. It brings the
 *   implementations of ysp/gfx.h, ysp/screen.h, ysp/rt.h and
 *   ysp/timeline.h with it, once, and pl_mpeg's (BUILDING).
 *
 *   Play a movie, centered at 1:1:
 *
 *       static yscr_screen scr; static ygfx_gfx gfx; static yvid_movie mv;
 *       yscr_desc sd = { 0 };
 *       if (!yscr_open(&scr, &sd)) die(yscr_error(&scr));
 *       ygfx_desc gd = { 0 };
 *       gd.screen = &scr;
 *       if (!ygfx_open(&gfx, &gd)) die(ygfx_error(&gfx));
 *       yvid_desc vd = { 0 };
 *       vd.path = "clip.yspseq";
 *       if (!yvid_open(&mv, &gfx, &vd)) die(yvid_error(&mv));
 *       yvid_stim_desc st = { 0 };
 *       ygfx_stim film = yvid_stim(&mv, &st);
 *       if (yvid_play_at(&mv, YVID_ASAP) < 0) die(yvid_error(&mv));
 *       yscr_frame f;
 *       int rc = YVID_OK;
 *       while (yscr_begin(&scr, &f) == YSCR_OK && (rc = yvid_update(&mv, &f)) == YVID_OK) {
 *           ygfx_begin(&gfx, &f); ygfx_draw(&gfx, &film); ygfx_end(&gfx);
 *           yscr_flip(&scr);
 *       }
 *       if (rc < 0) die(yvid_error(&mv));
 *       yvid_close(&mv); ygfx_close(&gfx); yscr_close(&scr);
 *
 *   The loop ends at the movie's end (YVID_ENDED), or when yscr_begin()
 *   stops (Shift+Esc by default, ysp/screen.h's abort). Every call on the
 *   movie that fails writes yvid_error() before it returns its code.
 *
 *   Psychtoolbox and PsychoPy, for comparison (from their documentation):
 *
 *       movie = Screen('OpenMovie', win, 'clip.mp4');               % PTB
 *       Screen('PlayMovie', movie, 1);
 *       while true
 *           tex = Screen('GetMovieImage', win, movie);
 *           if tex <= 0, break; end
 *           Screen('DrawTexture', win, tex); Screen('Flip', win); Screen('Close', tex);
 *       end
 *
 *       mov = visual.MovieStim(win, 'clip.mp4')                     # PsychoPy
 *       mov.play()
 *       while not mov.isFinished:
 *           mov.draw(); win.flip()
 *
 *   The differences: the stimulus is a ysp/gfx.h IMAGE (place, anchor,
 *   size, ori, opacity, groups and timeline bindings work as for any
 *   image) whose texture the movie rewrites, so it does not change from
 *   frame to frame. yvid_update() picks the frame due at f.onset; it
 *   never waits and never reads a clock. The file must be in the canonical
 *   form (CANONICAL FORM); the others accept any file and convert rate,
 *   size and color without a record. There is no rate argument. In C99
 *   and C++20 a compound literal does a desc in one line:
 *   yvid_open(&mv, &gfx, &(yvid_desc){ .path = "clip.yspseq" }).
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   TIME
 *     Times are int64_t ns on the ysp/rt.h clock. A movie time is ns from
 *     the start of frame 0. Video frame i covers movie time
 *     [t(i), t(i+1)) with t(i) = ceil(i * den * 1e9 / num) for the rate
 *     num/den (yvid_frame_time). The ceiling makes an integer comparison
 *     with t(i) the same as the exact rational one.
 *
 *   THE DUE FRAME (yvid_due)
 *     On a display frame with predicted onset T and period P, with m the
 *     movie time at T and L = lead x P truncated to whole ns, the due frame
 *     is the largest i with t(i) <= m + L. This is ysp/timeline.h's
 *     QUANTIZATION rule, so an annotation at t(i) on the movie base fires
 *     on the display frame that first shows frame i, with the same lead.
 *     desc.lead: 0 means 0.5 (the nearest display frame; onset noise of
 *     microseconds never moves a frame), YVID_LEAD_NONE never early
 *     (Psychtoolbox's "most recent frame"). With desc.timeline the
 *     timeline's own lead is used, and a different desc.lead is refused.
 *
 *   THE MOVIE CLOCK
 *     With desc.timeline and desc.base (1..7), the movie time at T is that
 *     base's time at T (ytl_base_time). yvid_play_at, pause_at, seek
 *     and loop anchor, pause and rewind the base; the caller evaluates the
 *     timeline after yvid_update() as usual, and annotations on the base
 *     fire on the display frame that shows them. The movie plays at the
 *     base's rate (BASE RATES). Without a timeline the header keeps its
 *     own anchor with the same arithmetic, at rate 1. A seek moves the base with
 *     ytl_skip(): the annotations it passes over are marked
 *     YTL_EV_SKIPPED and never fire, and the one at the target fires on
 *     the frame that shows it; the SEEK record counts the skipped ones.
 *     yvid_follow_audio() (with ysp/audio.h) or yvid_follow() re-anchors
 *     the movie clock every frame from another clock. With a soundtrack
 *     (SOUNDTRACK) the movie time is the soundtrack's sample at the onset,
 *     read through ysp_audio's fit; without one, the device's stream
 *     position since movie time 0. Audio is never resampled; video follows
 *     by showing the frame due, so frames repeat or drop when the two
 *     clocks slip.
 *
 *   CADENCE AND SLIPS
 *     At open the header compares the refresh R (desc.refresh_num/den, or
 *     the screen's mode) with the rate r. Within 200 ppm of an integer
 *     multiple k, each frame is due for k display frames; repeats there are
 *     DUE. Otherwise the cadence judders (23.976 fps on 60 Hz: 3, 2, 3, 2
 *     display frames per frame), as in Psychtoolbox's default movie mode:
 *     a showing beyond floor(R/r) is REPEATED with reason CADENCE, and a
 *     frame the refresh cannot show (r > R) is DROPPED with reason
 *     CADENCE. No tier penalty: the cadence is a property of the stimulus,
 *     and yvid_describe() states it with the frame durations.
 *     desc.strict_cadence refuses a refresh that is not a multiple (at
 *     open; at a change of base rate, BASE RATES).
 *     Each decision is also checked against a nominal schedule: the movie
 *     time the clock would give if it ran in step with the vblank count at
 *     the nominal period. A difference is a slip, reason DRIFT: the movie
 *     clock and the display grid run at different rates (13 ppm on the
 *     laptop panel: one slip every 21 minutes at 30 fps), or onset noise
 *     met a frame boundary exactly. The schedule restarts at each slip.
 *
 *   DECISIONS (yvid_record.decision, .why)
 *     j is the frame on screen, i the due frame, e the nominal due frame.
 *       i = j, i = e      REPEATED j, DUE or CADENCE (above)
 *       i = j, i != e     REPEATED j, DRIFT
 *       i > j, ready      SHOWN i (DUE; DRIFT when i != e; SEEK, LOOP or
 *                         MANUAL after those); frames j+1..i-1 DROPPED
 *       i > j, not ready  the newest ready frame after j that is not later
 *                         than i is SHOWN with DECODE_LATE, else j is
 *                         REPEATED with DECODE_LATE
 *     A dropped run is DECODE_LATE when its frames were due before they
 *     were ready, DISPLAY_LATE when the vblank count jumped (the flip
 *     before missed its vblank), CADENCE when the nominal schedule drops
 *     them too, and DRIFT otherwise. A frame is never shown late as if it
 *     were on time.
 *
 *   SEEK, LOOP, END, MANUAL
 *     yvid_seek(mt, resume) and yvid_seek_frame(i, resume) post the
 *     target to the decode thread. The frame on screen stays and FRAME
 *     records stop until the target is ready; the movie resumes at
 *     `resume` (YVID_ASAP: once desc.preroll frames are ready;
 *     YVID_STAY_PAUSED: shows the target and stays paused) with movie
 *     time t(target). A codec seeks to the keyframe at or before the target
 *     and decodes forward without keeping the frames; closed GOPs and no
 *     B-frames make that exact. desc.loop: after frame N-1 comes frame 0;
 *     the movie clock's base is rewound to 0 at t(N), so annotations fire
 *     again (1 ns of rounding per loop at a rate that is not a whole number
 *     of ns per frame). Without loop, yvid_update() returns YVID_ENDED
 *     from the first display frame whose due frame is N or later; the last
 *     frame stays in the texture. yvid_show(i) before yvid_update()
 *     shows frame i on this display frame (MANUAL), and the movie base is
 *     held at t(i): a step to the next frame fires the annotations up to
 *     t(i), a jump skips the ones it passes over. A frame outside the
 *     decoded window is a seek of the decoder, and the record says so.
 *
 *   CANONICAL FORM (checked at open)
 *     One rate num/den, one size and pixel format, closed GOPs of a
 *     constant length, no B-frames, and a stated color: matrix (BT.601,
 *     BT.709, BT.2020 NCL), range, transfer, primaries and chroma siting.
 *     "Unspecified" is refused. 10-bit is refused. A frame sequence holds
 *     its own description; an MPEG-1 or MP4 file needs its index beside it
 *     (path + ".yspvi"), made by yvid_index_make() (the pack tool's step): its
 *     description, the media file's size and the hashes of its first and
 *     last 64 KB, and the XXH64 of every decoded frame. Each value the
 *     backend reports must equal the index's. During play every frame's
 *     timestamp must map to the frame expected (else YVID_F_TS_MISMATCH)
 *     and every frame decoded on the CPU must hash to the index's value
 *     (else YVID_F_HASH_MISMATCH); the frame is still shown and the
 *     describe line says the file is not canonical.
 *
 *   MEDIA FOUNDATION (Windows; desc.backend MF, or AUTO for an MP4 file)
 *     An IMFSourceReader, synchronous, used on the decode thread. MP4 with
 *     one H.264 (Baseline, Main, High) or HEVC Main video track; an audio
 *     track is ignored. The reader may not add a converter or a video
 *     processor: the decoder gives NV12 itself or the file is refused.
 *     desc.hw_decode: OFF is Microsoft's software decoder; DXVA the
 *     decoder on a D3D11 device of its own (VIDEO_SUPPORT, on the screen's
 *     adapter) with each kept frame read back; AUTO is DXVA, and the
 *     software decoder when no DXVA device opens. Measured at 1080p60
 *     (docs/video.md): DXVA costs 5.3 ms a frame on the decode thread
 *     (most of it the readback) and about 1 ms of Media Foundation's CPU;
 *     the software decoder 1.9 ms on the decode thread but 13 to 21 ms of
 *     CPU on Media Foundation's threads, more than a core at 60 fps. DXVA
 *     seeks 4 to 5 times faster and opens about 100 ms slower. A hardware
 *     decoder MFT that writes system memory was measured too and removed:
 *     it cost as much CPU as the software decoder. Every decoder gave the
 *     index's bytes on every frame of the test clips: H.264 decoding is
 *     exact by the standard, so one index serves them all, and a decoder
 *     that differs is a HASH_MISMATCH, not a tolerance.
 *     The rate is the index's: Media Foundation reports a 100 ns
 *     approximation (24000/1001 comes out 10000000/417083). Media
 *     Foundation does not report the color the stream states, so
 *     yvid_index_make() reads it from the H.264 SPS (its VUI): a desc
 *     field that contradicts the stream is refused, and a field nobody
 *     states is refused. HEVC's color comes from the desc. The index maker
 *     refuses B-frames, frame times that fit no constant rate (to 100 ns),
 *     a GOP that changes, an edit list in the video track (Media
 *     Foundation plays a start offset from 0, other players do not),
 *     interlace, rotation, non-square pixels, 10-bit, 4:2:2 and 4:4:4, and
 *     a keyframe that does not decode to the same bytes after a seek. COM:
 *     open() keeps a multithreaded apartment alive (CoIncrementMTAUsage),
 *     so the decode thread needs no COM setup and the frame thread may be
 *     in a single-threaded apartment, as SDL3 leaves it. A path, memory and
 *     a yvid_reader all reach Media Foundation through one IStream, whose
 *     read() Media Foundation calls from its own threads, one at a time.
 *
 *   GPU PATH (desc.gpu_path YVID_PATH_GPU; Windows, Media Foundation)
 *     DXVA decodes on the screen's own D3D11 device, and the decode thread
 *     copies each kept frame on the GPU (CopySubresourceRegion) into one of
 *     the movie's NV12 textures, which ysp/gfx.h imported once at open
 *     (ygfx_texture_import). Nothing is read back or uploaded; the frame
 *     thread rebinds the stimulus to the due slot's texture
 *     (ygfx_texture_rebind), and the slot on screen is not decoded into
 *     until another replaces it. The decoder's own surfaces are not held:
 *     it has about nine, and holding them for decode-ahead stalls it. The
 *     copy and the draw go through one immediate context, which orders
 *     them, so there is no fence. Needs ygfx_features() with
 *     YGFX_FEAT_IMPORT_NV12, a screen with a D3D11 device (the DXGI_FLIP
 *     or COMPOSITION backend) made with VIDEO_SUPPORT and multithread
 *     protection (ysp/screen.h's desc.d3d11_video), and hw_decode AUTO
 *     or DXVA; open() refuses otherwise, by name. The timestamps are
 *     checked; the frame hashes are not (the frame never reaches the CPU).
 *     Against UPLOAD on the test clips: the same value at every pixel.
 *     Measured at 1080p (docs/video.md, M7): 20 to 50 % less process
 *     CPU and no slot memory. On the frame thread the upload leaves, but
 *     the draw and flip wait longer on the shared context (p99 1.5 to 8 ms
 *     against 0.9 to 2 ms); dropped frames in a composed window did not
 *     separate the paths. UPLOAD stays the default: it needs no video
 *     device and has the shorter tail.
 *     YVID_PATH_SHARED (a second device) is not built.
 *
 *   DECODE-AHEAD AND UPLOAD
 *     Each movie has its own ysp/rt.h pump at normal priority (desc.pin_cpu
 *     pins it). The pump decodes one frame per idle call into a free slot
 *     until desc.ahead frames are ready, then blocks until the frame
 *     thread frees a slot. Frames older than the due frame are decoded
 *     without being kept (a P-frame needs its reference), or skipped by a
 *     seek to the next keyframe when that is shorter. A slot holds the
 *     frame as decoded: I420 or NV12 planes with tight rows, or the frame
 *     sequence's own format. yvid_update() uploads the due frame with
 *     ygfx_texture_update_planes() into a YGFX_I420 or YGFX_NV12
 *     texture (ygfx_texture_update() for the other formats) and ends the
 *     frame's YSCR_PHASE_UPLOAD. ysp/gfx.h updates textures only between
 *     frames: call it after yscr_begin() and before ygfx_begin().
 *     ysp/gfx.h's video program converts YUV with the canonical form's
 *     matrix, range and siting (desc.chroma NEAREST replicates chroma).
 *     desc.light: CODES shows the R'G'B' codes as device values; EOTF
 *     decodes the stated transfer and primaries to linear light, so the
 *     movie goes through the screen's calibration like any stimulus; AUTO
 *     is EOTF for a YUV movie when ysp/gfx.h has a calibration
 *     (ygfx_calibrated), else CODES. EOTF is for YUV movies; an RGB frame
 *     sequence holds device values. Slots: ahead + 2 of the frame's bytes
 *     (1.5 a pixel for YUV), one allocation at open (or desc.mem); none on
 *     the GPU path. Nothing allocates per frame on either thread
 *     (yvid_heap_calls()). yvid_yuv_to_rgba() is the CPU conversion
 *     v0.1 ran on the pump, kept as a tool (an export, a test): integer
 *     arithmetic within 1 code of a double reference.
 *     desc.inline_decode runs the decode step inside yvid_update()
 *     instead, so a run is a pure function of its inputs (tests).
 *
 *   SOUNDTRACK (yvid_soundtrack, with ysp/audio.h)
 *     A WAV (path, memory or a yvid_reader) at the device's rate, played
 *     as a ysp/audio.h stream (yau_wav): a voice like any other, placed,
 *     recorded and confirmed by ysp_audio, and mixed with yau_play_at()
 *     sounds. Call it after yvid_open() and before the first play. The
 *     movie's decode thread fills the stream's ring (1 s by default). Sample
 *     s plays at movie time s / rate: a start at movie time mt begins with
 *     the sample nearest mt (a tie to the earlier). With YVID_ASAP the
 *     start is the first predicted display onset both can reach (at or
 *     after now + yau_lead_ns() + one display period, once the frames
 *     and the ring are in), and the movie base is anchored there, so sound
 *     and picture share one origin. A pause stops the sound at its target and a resume
 *     starts it again. A seek during play stops it at once (the desc's
 *     ramp_ns fades it), the decode thread moves the WAV to the target's
 *     sample once ysp_audio has ended the play, the ring refills, and the
 *     movie resumes at the next shared ASAP target with a new play.
 *     desc.loop loops the WAV with the movie, sample numbers continuing.
 *     yvid_follow_audio() then takes each display frame's movie time from
 *     the soundtrack's sample at the onset. Refused, by name: a WAV whose
 *     length is not the movie's duration in samples (N x den x rate / num,
 *     rounded to the nearest, a tie up), a loop whose duration is not a
 *     whole number of samples, and a base rate other than 1 (no
 *     resampling; a later change of rate stops the sound and
 *     yvid_update() returns YVID_ERR_REFUSED once). ysp/audio.h refuses
 *     a rate or a channel count that is not the device's.
 *
 *   BASE RATES (ytl_rate on the movie base, with desc.timeline)
 *     The movie plays at its base's rate: 1/2 is half speed, 2/1 double.
 *     The due frame is the largest i with t(i) at or before the base time
 *     at RT onset + L (ytl_window), so the lead stays in RT ns and an
 *     annotation at t(i) still fires on the display frame that shows frame
 *     i. yvid_update() reads the rate on every display frame. At a
 *     change, the cadence is computed again for R against r x num/den, the
 *     nominal schedule restarts (a change is not a slip), and a
 *     YVID_EV_RATE record says so. Under desc.strict_cadence a rate whose
 *     cadence judders holds the frame on screen and yvid_update() returns
 *     YVID_ERR_REFUSED until the rate gives a multiple again or the movie
 *     closes; a seek alone does not clear it (it lands, then holds). At
 *     rate 1 the records are those of v0.1.0, byte for byte.

 *   RECORDS (desc.ring; source YRT_SRC_VIDEO, aux desc.movie_index)
 *     YVID_EV_FRAME, one per display frame while playing, pushed when its
 *     flip record arrives (yscr_frame.done in a later yvid_update(), or
 *     yvid_flip_done() from yscr_on_flip()):
 *       t_ns       the flip's onset; the predicted onset when the flip has
 *                  no record (YVID_F_ESTIMATED) or was never shown
 *       u.i64[0]   video frame index
 *       u.i64[1]   due: the RT time of the frame's start on the movie clock
 *       u.i64[2]   the movie time at the predicted onset
 *       u.u32[6]   flags (YVID_F_*)
 *       u.u16[14]  decision bits 0..1, why bits 2..5, tier bits 6..8,
 *                  path bits 9..10 (YVID_EV_DECISION_OF() and friends)
 *       u.u16[15]  frames ready ahead
 *       u.u32[8]   display frames this video frame has been on screen
 *       u.u32[9]   the display frame index, low 32 bits: the word
 *                  YSCR_EV_FLIP carries, so a join needs no table
 *     The residual is t_ns - u.i64[1]; with the audio clock it is the A/V
 *     error of that frame. Other kinds:
 *       YVID_EV_DROP    t_ns due time of the first, i64[0] first frame,
 *                         i64[1] count, u16[14] why, u32[9] display index
 *       YVID_EV_OPEN    i32[0..5] w, h, format, fps num, den, codec;
 *                         i32[6] backend, i32[7] multiple (0 = judder),
 *                         u32[8] color word (matrix | range << 8 |
 *                         transfer << 16 | primaries << 24), i32[9] ahead
 *       YVID_EV_PLAY, _PAUSE  t_ns the display frame's onset, i64[0]
 *                         movie time, i64[1] id, i64[2] frame, i64[3] the
 *                         requested time (0 = ASAP)
 *       YVID_EV_SEEK    t_ns the onset that showed the target, i64[0]
 *                         target, i64[1] keyframe used, i64[2] frames
 *                         decoded and discarded, i64[3] the onset of the
 *                         request, u32[8] id, u32[9] annotations
 *                         skipped on the movie base
 *       YVID_EV_LOOP    t_ns onset, i64[0] cycle
 *       YVID_EV_END     t_ns onset, i64[0] last frame shown
 *       YVID_EV_CLOCK   t_ns anchor, i64[0] the other clock's unit at
 *                         movie time 0, i64[1] its rate
 *       YVID_EV_DECODE  t_ns start, i64[0] the decode's length in ns,
 *                         i64[1] frame: one decode longer than a frame
 *       YVID_EV_SOUND   t_ns the target (now for a stop at once);
 *                         i64[0] ysp_audio's play id, i64[1] the first
 *                         sample (a start) or -1 (a stop), i64[2] the
 *                         target, u32[8] 1 start, 2 stop, u32[9] the
 *                         control's id. ysp_audio's ONSET, STREAM, GAP and
 *                         END records under YRT_SRC_AUDIO carry the rest
 *       YVID_EV_RATE    t_ns onset; i32[0] num, i32[1] den, i32[2] the
 *                         multiple (0 = judder), f64[2] display frames per
 *                         video frame, u32[8] 1 when strict_cadence refuses
 *     Controls (play_at, pause_at, seek) return an id > 0; yvid_result()
 *     gives its PLAY, PAUSE or SEEK record once it happened.
 *
 *   TIERS
 *     A FRAME record carries its flip's tier (yscr_tier numbering): the
 *     decision says what was shown, the tier how well the onset is known.
 *     desc.min_tier flags worse frames YVID_F_BELOW_TIER; the movie goes
 *     on. A timestamp or hash mismatch is a fault, not a tier.
 *
 *   FRAME SEQUENCE (yvid_seq_*)
 *     One little-endian file. A 4096-byte header (magic YSPVSEQ1, size,
 *     format, compression, rate, frame count, color, index offset, XXH64 of
 *     the header), then per frame a 64-byte frame header (magic, index,
 *     size, the XXH64 of the decoded texels, and for capture a ysp_rt time,
 *     trial, event and camera time) and the data, each padded to 4096
 *     bytes, then the index (offset, size, flags, hash: 24 bytes per
 *     frame). Formats: the eight ysp/gfx.h texture formats, stored as
 *     ysp/gfx.h takes them (R16F and RGBA16F as float, rounded to half by
 *     the GPU), and I420 and NV12. RAW frames, or QOI for RGBA8 (lossless,
 *     coded by the header's own QOI, bit-equal with the reference qoi.h).
 *     Every frame is a keyframe, so a seek is a read. The writer appends
 *     from one thread, writes the index and fixes the header at close.
 *
 *   DECODER EXTENSION (yvid_decoder; desc.backend YVID_BACKEND_CUSTOM)
 *     The backend owns its codec objects and reports its stream's own
 *     timestamps; the core owns the clock, the index, the checks, the
 *     slots, the schedule and the records. Every call comes from the
 *     decode thread, never two at once. next() decodes the next frame in
 *     presentation order into the caller's planes (dst NULL: decode and
 *     discard), or lends its own planes (YVID_OUT_BORROWED), and returns
 *     0, YVID_ENDED, YVID_PENDING (nothing now; call again later) or a
 *     negative code. seek(key) positions it so the next next() returns the
 *     keyframe `key`. The core maps out.pts to a frame index as
 *     round(pts * num / (den * timescale)), or takes out.index when the
 *     backend knows it.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Links what ysp/gfx.h links. On Windows, Media Foundation (mfplat.dll,
 *   mfreadwrite.dll) and, for DXVA, d3d11.dll and dxgi.dll are loaded at
 *   run time and its GUIDs are copies in this file
 *   (tests/compile/video_com.cpp checks them against the SDK), so no
 *   import library and no mfuuid.lib is needed; YVID_NO_MF leaves it
 *   out. Windows Server needs its Media Foundation feature installed; open()
 *   says so when the DLLs are missing. pl_mpeg.h on the include path, or
 *   YVID_NO_PL_MPEG. Its implementation is compiled here unless
 *   YVID_PL_MPEG_EXTERNAL (then yvid_index_make() and the reader
 *   source are not available for MPEG-1). Its heap calls go through
 *   YVID_MALLOC, YVID_REALLOC and YVID_FREE (default: the C
 *   library's), which yvid_heap_calls() counts. YRT_NO_THREADS forces
 *   desc.inline_decode, and so do ysp/rt.h's virtual clock (v0.4.0) and a
 *   decode thread that cannot start (wasm built without -pthread): the
 *   movie plays, each decode inside yvid_update() on the frame thread, and
 *   yvid_describe() says so.
 *   With ysp/audio.h, the soundtrack calls audio.h's public functions
 *   only, so audio.h's implementation may be in another module.
 *   Define YVID_API to change the linkage.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.5.1 - The soundtrack's close waits on this header's clock
 *          (YVID__NOW) and a sleep of its own, not on ysp/audio.h's
 *          private YAU__NOW and YAU__SLEEP_UNTIL, which only audio.h's
 *          implementation defines. The soundtrack part now uses audio.h's
 *          public functions only, so it builds beside audio.h's
 *          declarations with the implementation in another module
 *          (bindings/share, tests/compile/share_shims.c). No API or
 *          behavior change.
 *   v0.5.0 - PARAMETERS: yvid_params() fills yscr_param's new last field,
 *          values (ysp/screen.h v0.9.0): light, gpu_path, hw_decode and
 *          chroma list their C enum's names and values
 *          ("auto=0,upload=1,shared=2,gpu=3"); every other row has NULL.
 *          The doc text no longer lists them. Needs ysp/screen.h v0.9.0.
 *   v0.4.0 - VIRTUAL CLOCK: on ysp/rt.h's virtual clock (yrt_virtual_begin,
 *          rt.h v0.9.0) a movie decodes inline, as with
 *          desc.inline_decode: a decode thread on real time fell behind a
 *          frame loop that never waits, and video_play --sim showed a
 *          different count of shown and dropped frames each run. Now the
 *          same each run. Decode and upload times are costs and read
 *          yrt_real_ns(), so they stay real on the virtual clock. Real
 *          clock: no change.
 *   v0.3.1 - pl_mpeg.h builds under -Wfloat-conversion (gcc and clang): its
 *          double-to-integer and double-to-float conversions are its own,
 *          so the include silences that warning as it already did
 *          -Wshorten-64-to-32. No change to ysp code.
 *   v0.3.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          The frame sequence writer opens, finishes and closes:
 *          yvid_seq_create() is yvid_seq_open(); yvid_seq_finish() (new)
 *          writes the index and the header and returns 0 or a code;
 *          yvid_seq_close() returns void, is safe on a zeroed writer,
 *          after a failed open and twice, and finishes a writer that was
 *          not finished. yvid_probe() returns 0 or a negative code
 *          instead of bool. Every call on a movie or a writer that fails
 *          writes yvid_error() or yvid_seq_error() first (play_at,
 *          pause_at, seek, seek_frame, show, update, follow, soundtrack,
 *          follow_audio, seq_write, seq_finish); a call that wrote none
 *          gets "ysp_video: <call>: <code's text>". Time parameters say
 *          ns (t_ns, movie_ns, resume_at_ns, m_ns, period_ns); every
 *          time was already int64_t ns. YVID_NS_PER_S is gone (YRT_S(1)).
 *          USAGE builds as C99 (C, { 0 }).
 *   v0.2.2 - XXH64 on MSVC: little-endian loads through memcpy instead of
 *          byte by byte, which MSVC did not fuse; yvid_xxh64() on 1 GiB
 *          went from 5.9 to 6.3 GB/s to 11.9 to 14.1 GB/s (MinGW gcc
 *          unchanged at 13.0 to 13.6), the same hashes. A 1080p I420
 *          frame's hash is half as long on the decode thread.
 *   v0.2.1 - Fixed: after a decoder's YVID_PENDING the decode thread
 *          slept until the next seek or control, so a custom decoder that
 *          stalled once showed no new frame again (the frame on screen
 *          REPEATED with DECODE_LATE). It now asks again once a display
 *          frame, and catches up as before: it decodes the due frame (from
 *          its keyframe), and the frames passed over are DROPPED with
 *          DECODE_LATE. The built-in backends never return PENDING.
 *   v0.2.0 - Media Foundation (Windows): H.264 and HEVC (8-bit 4:2:0) in
 *          MP4, decoded into NV12 by the software decoder or DXVA
 *          (desc.hw_decode; AUTO is DXVA, by measurement). yvid_index_make()
 *          makes the index of an MP4 file too: B-frames, a rate that no
 *          frame grid fits, a varying GOP, an edit list, interlace,
 *          rotation, non-square pixels, 10-bit and 4:2:2 or 4:4:4 are
 *          refused by name, the color is read from the H.264 SPS, and every
 *          keyframe is decoded again after a seek to prove it is a clean
 *          entry point. Planar upload: a YUV slot holds its planes and
 *          ysp/gfx.h's video program converts them, so light EOTF works;
 *          the CPU conversion left the play path (yvid_yuv_to_rgba()
 *          stays). YVID_PATH_GPU: DXVA on the screen's device, one GPU
 *          copy a frame, no readback and no upload. yvid_soundtrack(): a
 *          WAV played as a ysp/audio.h stream, locked to the picture by
 *          yvid_follow_audio(). Base rates: the movie plays at its
 *          ysp/timeline.h base's rate. Fixed: after a PENDING decode the
 *          decode thread pushed its slot onto the free queue, whose one
 *          producer is the frame thread (a data race). Breaking:
 *          yvid_index_desc gains backend, hw and no_seek_check;
 *          yvid_desc gains hw_decode and yvid_info gains hw;
 *          YVID_PATH_ZERO_COPY is now YVID_PATH_GPU; a YUV movie's
 *          texture is YGFX_NV12 or YGFX_I420, not RGBA8; light AUTO is
 *          EOTF under a calibration; a file that is not a frame sequence,
 *          MPEG-PS or MP4 is refused at open.
 *   v0.1.0 - first release: the scheduler (due frame by the timeline's
 *          lead rule, cadence, slips, drops and repeats with reasons), the
 *          movie clock read from a ysp_timeline base, the decoder
 *          interface, the frame sequence container (raw, QOI), pl_mpeg
 *          with its index, decode-ahead on a pump, upload through
 *          ysp/gfx.h, I420 and NV12 converted to RGBA8 on the decode
 *          thread, manual mode, seek, loop, the records.
 *
 *   STATUS: v0.5.1: tests/compile/share_shims.c builds this implementation
 *   beside audio.h's declarations only (MSVC 19.44 C and C++17, MinGW-w64
 *   gcc C11, warnings as errors); the core test passes as before.
 *   STATUS: v0.3.0: tests/adapt/video_test.c checks the writer's open,
 *   finish and close, yvid_probe()'s codes and the movie's messages; the
 *   rest is v0.2.0's. v0.2.0, on one Windows 11 laptop (i7-1360P, Iris Xe, ANGLE),
 *   in a 640 x 360 composed window (tier 2); docs/video.md has the
 *   tables. Measured at 1920 x 1080 with Media Foundation: yvid_update()
 *   on the frame thread with the planar upload 0.39 to 0.61 ms mean, 0.7
 *   to 1.3 ms p99 (v0.1's RGBA8 upload 0.9 to 1.07 / 1.5 to 2.0 ms), a
 *   repeat 0.01 ms, YVID_PATH_GPU 0.01 to 0.02 ms. The decode thread at
 *   1080p60: DXVA 5.3 ms mean, 8.5 ms p99 a frame; the software decoder
 *   1.9 / 3.3 ms on it but 13 to 21 ms of CPU a frame on Media
 *   Foundation's threads. Open 115 to 308 ms cold; a seek 32 to 42 ms mean
 *   with DXVA, about 155 ms with the software decoder. pl_mpeg 5.5 ms a
 *   1080p frame. The soundtrack on the real device (WASAPI shared, 48 kHz):
 *   each start within 14 us of its target on ysp_audio's fit, and the
 *   flips' onsets within -0.42 to +0.08 ms (p1 to p99) of each frame's
 *   time on the audio clock, outside a few frames the composed window
 *   flipped a vblank early or late; no gap, no underrun. No heap call per
 *   frame. The core test (a scripted decoder, display and audio device on
 *   a virtual clock; with YVID_TEST_MEDIA the generated MP4 clips)
 *   passes on MSVC 19.44 (C, C++17), MinGW gcc 16.1 (C99, C++17) and gcc
 *   11.4 (C99 -O3, ASan and UBSan, ThreadSanitizer); emcc ran for v0.1.
 *   Mutations: 23 of 23 (v0.1), 29 of 31 (v0.2; two Media Foundation
 *   checks not caught, docs/video.md says why). Not measured: light
 *   and sound (no photodiode or microphone), fullscreen, slips on the
 *   real panel, another GPU. Not built: YVID_PATH_SHARED, AVFoundation, FFmpeg,
 *   capture; open() refuses the ones a desc can ask for, by name.
 *   Outside it and this block, a number in this header is a measurement
 *   only where the text says "measured".
 */
#ifndef YSP_VIDEO_H_INCLUDED
#define YSP_VIDEO_H_INCLUDED

#define YVID_VERSION_MAJOR 0
#define YVID_VERSION_MINOR 5
#define YVID_VERSION_PATCH 1
#define YVID_VERSION_STRING "0.5.1"

#include "ysp/gfx.h"
#include "ysp/timeline.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YVID_API
#define YVID_API extern
#endif

/* --- results -------------------------------------------------------------- */

#define YVID_OK                    0
#define YVID_ENDED                 1  /* update: the due frame is past the last */
#define YVID_PENDING               2  /* result: not happened yet; decoder: later */
#define YVID_ERR_ARG             (-1)
#define YVID_ERR_CLOSED          (-2)
#define YVID_ERR_FORMAT          (-3)  /* not the canonical form              */
#define YVID_ERR_ORDER           (-4)  /* a call out of turn                  */
#define YVID_ERR_FULL            (-5)
#define YVID_ERR_DECODER         (-6)  /* the backend failed; yvid_error()  */
#define YVID_ERR_REFUSED         (-7)  /* a cadence, a rate, a light          */
#define YVID_ERR_LOST            (-8)  /* the GL context is new               */
#define YVID_ERR_IO              (-9)
#define YVID_ERR_NOT_FOUND      (-10)
#define YVID_ERR_NOT_IMPLEMENTED (-11)

#define YVID_ASAP           0          /* play_at, seek: the first frame it can */
#define YVID_STAY_PAUSED  (-1)         /* seek: show the target, stay paused */
#define YVID_LEAD_NONE   (-1.0)        /* desc.lead: never early             */

/* --- formats and color ------------------------------------------------------ */

/* 1 to 8 are ysp/gfx.h's texture formats, with its input types. */
#define YVID_FMT_R8       1
#define YVID_FMT_RG8      2
#define YVID_FMT_RGBA8    3
#define YVID_FMT_R16F     4    /* float in the file, half on the GPU        */
#define YVID_FMT_RGBA16F  5
#define YVID_FMT_R32F     6
#define YVID_FMT_RGBA32F  7
#define YVID_FMT_R16      8    /* uint16_t; ysp/gfx.h's R16UI, k / 65535     */
#define YVID_FMT_I420    32    /* Y, then Cb, then Cr; 8-bit 4:2:0           */
#define YVID_FMT_NV12    33    /* Y, then CbCr interleaved; 8-bit 4:2:0      */
#define YVID_FMT_P010    34    /* 10-bit 4:2:0: refused                     */

/* Color, every field 0 = unspecified (refused for YUV). RGB formats are
 * device values, as ysp/gfx.h images are: RGB, FULL, DEVICE, DEVICE, NONE. */
#define YVID_MATRIX_RGB      1
#define YVID_MATRIX_BT601    2
#define YVID_MATRIX_BT709    3
#define YVID_MATRIX_BT2020   4   /* non-constant luminance                  */
#define YVID_RANGE_LIMITED   1
#define YVID_RANGE_FULL      2
#define YVID_TRC_DEVICE      1
#define YVID_TRC_BT1886      2
#define YVID_TRC_SRGB        3
#define YVID_TRC_LINEAR      4
#define YVID_TRC_GAMMA22     5
#define YVID_PRIM_DEVICE     1
#define YVID_PRIM_BT709      2
#define YVID_PRIM_BT601_525  3
#define YVID_PRIM_BT601_625  4
#define YVID_PRIM_BT2020     5
#define YVID_SITING_NONE     1   /* not subsampled                          */
#define YVID_SITING_LEFT     2   /* MPEG-2, H.264, HEVC: co-sited across     */
#define YVID_SITING_CENTER   3   /* MPEG-1, JPEG: between both ways          */

#define YVID_CODEC_SEQ       1
#define YVID_CODEC_MPEG1     2
#define YVID_CODEC_H264      3
#define YVID_CODEC_HEVC      4
#define YVID_CODEC_CUSTOM    5

#define YVID_SEQ_RAW 0
#define YVID_SEQ_QOI 1

/* --- enums ------------------------------------------------------------------ */

typedef enum yvid_decision { YVID_SHOWN = 1, YVID_REPEATED = 2, YVID_DROPPED = 3 } yvid_decision;
typedef enum yvid_why {
    YVID_WHY_DUE = 0,      /* on schedule                                    */
    YVID_WHY_CADENCE,      /* the refresh is not a multiple of the rate      */
    YVID_WHY_DISPLAY_LATE, /* the flip before missed its vblank              */
    YVID_WHY_DECODE_LATE,  /* the frame was not decoded in time              */
    YVID_WHY_DRIFT,        /* the movie clock slipped against the grid       */
    YVID_WHY_SEEK,
    YVID_WHY_LOOP,
    YVID_WHY_MANUAL,
    YVID_WHY_COUNT
} yvid_why;

typedef enum yvid_backend {
    YVID_BACKEND_AUTO = 0,   /* from the file's first bytes                 */
    YVID_BACKEND_SEQ,        /* frame sequence                              */
    YVID_BACKEND_PLMPEG,     /* MPEG-1 in MPEG-PS, pl_mpeg, CPU             */
    YVID_BACKEND_MF,         /* Media Foundation (Windows): H.264, HEVC in MP4 */
    YVID_BACKEND_AVF,        /* AVFoundation: not built                     */
    YVID_BACKEND_FFMPEG,     /* not built                                   */
    YVID_BACKEND_CUSTOM      /* desc.decoder                                */
} yvid_backend;

typedef enum yvid_path {
    YVID_PATH_AUTO = 0,
    YVID_PATH_UPLOAD,        /* glTexSubImage2D through ysp/gfx.h           */
    YVID_PATH_SHARED,        /* Windows: a second device: not built         */
    YVID_PATH_GPU            /* Windows, Media Foundation: DXVA on the screen's
                                * device, one GPU copy per frame, no upload  */
} yvid_path;

/* Media Foundation's decoder (desc.hw_decode). */
typedef enum yvid_hw {
    YVID_HW_AUTO = 0,        /* DXVA, or the software decoder when no DXVA
                                * device opens (measured, docs/video.md) */
    YVID_HW_OFF,             /* Microsoft's software decoder                */
    YVID_HW_DXVA             /* DXVA on a D3D11 device                      */
} yvid_hw;

typedef enum yvid_light {
    YVID_LIGHT_AUTO = 0,     /* EOTF for YUV under a calibration, else CODES */
    YVID_LIGHT_CODES,        /* R'G'B' codes as device values               */
    YVID_LIGHT_EOTF          /* linear light, through the calibration (YUV) */
} yvid_light;

typedef enum yvid_chroma { YVID_CHROMA_SITED = 0, YVID_CHROMA_NEAREST = 1 } yvid_chroma;

/* --- the decoder extension ------------------------------------------------------ */

#define YVID_DECODER_VERSION 1

#define YVID_DEC_CPU           0x1u   /* decodes into CPU planes            */
#define YVID_DEC_RANDOM_ACCESS 0x2u   /* any frame is a keyframe            */
#define YVID_DEC_GPU_D3D11     0x4u   /* reserved                           */

#define YVID_OUT_KEYFRAME 0x1u
#define YVID_OUT_BORROWED 0x2u        /* out.planes are the backend's, valid
                                         * until its next call               */
#define YVID_OUT_HAS_HASH 0x4u        /* out.hash is the container's XXH64  */
#define YVID__OUT_GPU     0x80000000u /* private: copied on the GPU (YVID_PATH_GPU) */

/* A byte source read by range (a streamed pack entry). */
typedef struct yvid_reader {
    int64_t (*read)(void* ctx, int64_t offset, void* buf, int64_t n);   /* bytes, or < 0 */
    int64_t size;
} yvid_reader;

/* What the backend can report; -1 or 0 where it cannot (the index fills it). */
typedef struct yvid_stream {
    int32_t  w, h;                /* 0 = unknown                              */
    int32_t  format;              /* YVID_FMT_*; 0 = unknown                */
    int32_t  fps_num, fps_den;    /* 0 = unknown                              */
    int64_t  frames;              /* -1 = unknown                             */
    int32_t  gop;                 /* 0 = unknown; 1 = every frame a keyframe  */
    int32_t  codec;               /* YVID_CODEC_*                           */
    int16_t  matrix, range, transfer, primaries, siting;   /* -1 or 0 = unknown */
    int16_t  reserved_;
    int64_t  timescale;           /* pts units per second; 0 = pts unused     */
    uint32_t caps;                /* YVID_DEC_*                             */
    uint32_t reserved2_;
} yvid_stream;

/* Planes of a frame. The core's: tight rows; the backend's: its strides. */
typedef struct yvid_planes {
    uint8_t* data[3];
    int32_t  stride[3];           /* bytes per row                            */
    int32_t  w[3], h[3];          /* texels per plane                         */
} yvid_planes;

typedef struct yvid_out {
    int64_t       pts;            /* in stream.timescale units, as the stream says */
    int64_t       index;          /* the frame's index when known; else -1    */
    uint64_t      hash;           /* with YVID_OUT_HAS_HASH                 */
    uint32_t      flags;          /* YVID_OUT_*                             */
    uint32_t      reserved_;
    yvid_planes planes;         /* with YVID_OUT_BORROWED                 */
} yvid_out;

typedef struct yvid_decoder_open {
    const char*          path;
    const void*          data;
    size_t               size;
    const yvid_reader* reader;
    void*                reader_ctx;
    const void*          index;
    size_t               index_size;
} yvid_decoder_open;

typedef struct yvid_decoder {
    uint32_t    version;          /* YVID_DECODER_VERSION                   */
    const char* name;
    int  (*open)(void* ctx, const yvid_decoder_open* in, yvid_stream* out,
                 char* err, size_t err_cap);
    int  (*next)(void* ctx, yvid_planes* dst, yvid_out* out);
    int  (*seek)(void* ctx, int64_t key, int64_t key_pts);
    void (*close)(void* ctx);
    int  (*describe)(void* ctx, char* buf, size_t cap);   /* may be NULL       */
} yvid_decoder;

/* --- description, info, record ------------------------------------------------ */

typedef struct yvid_desc {
    const char*          path;          /* a file; or                          */
    const void*          data;          /* the file in memory; or              */
    size_t               size;
    const yvid_reader* reader;        /* a byte source                       */
    void*                reader_ctx;
    const void*          index;         /* the .yspvi bytes; NULL = path + ".yspvi" */
    size_t               index_size;
    yvid_backend       backend;       /* 0 = from the file                   */
    yvid_path          gpu_path;      /* 0 = UPLOAD                          */
    int32_t              ahead;         /* frames decoded ahead; 0 = 6         */
    int32_t              preroll;       /* frames ready before ASAP; 0 = ahead */
    bool                 loop;
    bool                 strict_cadence;/* refuse a refresh that is not a multiple */
    bool                 inline_decode; /* decode inside update(): tests       */
    bool                 reserved_;
    int32_t              inline_budget; /* decodes per update inline; 0 = no limit */
    double               lead;          /* 0 = 0.5; YVID_LEAD_NONE; (0, 1)   */
    yvid_light         light;
    yvid_chroma        chroma;
    ytl_timeline*      timeline;      /* the movie base lives here; or NULL  */
    int32_t              base;          /* 1 .. YTL_MAX_BASES-1 with a timeline */
    int32_t              refresh_num;   /* 0 = the screen's mode               */
    int32_t              refresh_den;
    yrt_ring*          ring;          /* records; NULL = none                */
    uint32_t             movie_index;   /* aux of every record                 */
    int32_t              min_tier;      /* 0 = off                             */
    void*                mem;           /* slot memory; NULL = allocated       */
    size_t               mem_bytes;
    const yvid_decoder* decoder;      /* YVID_BACKEND_CUSTOM               */
    void*                decoder_ctx;
    int32_t              pin_cpu;       /* decode thread; 0 = no pin           */
    yvid_hw            hw_decode;     /* Media Foundation's decoder; 0 = AUTO */
} yvid_desc;

typedef struct yvid_info {
    int32_t  w, h;
    int32_t  fps_num, fps_den;
    int64_t  frames;
    int64_t  duration;               /* ns: t(frames)                         */
    int32_t  gop;
    int32_t  codec;
    int32_t  format;                 /* the stream's                          */
    int32_t  upload_format;          /* the texture's (ygfx_format)         */
    uint8_t  matrix, range, transfer, primaries, siting;
    uint8_t  reserved_[3];
    int32_t  refresh_num, refresh_den;   /* 0 = unknown                       */
    double   per_frame;              /* display frames per video frame        */
    int32_t  multiple;               /* k, or 0 for a cadence that judders    */
    double   err_ppm;                /* the refresh against k x rate          */
    double   lead;                   /* resolved: 0.5, 0 for NONE, ...        */
    int32_t  ahead, slots;
    size_t   slot_bytes;
    yvid_backend backend;
    yvid_path    path;
    yvid_light   light;
    int32_t  worst_tier;
    uint64_t shown, repeated, dropped;
    uint64_t repeats_by[YVID_WHY_COUNT];
    uint64_t drops_by[YVID_WHY_COUNT];
    uint64_t ts_mismatch, hash_mismatch;
    uint64_t skipped;                /* annotations seeks passed over (ytl_skip) */
    yvid_hw hw;                    /* Media Foundation: the decoder in use   */
} yvid_info;

/* The record of one display frame while playing. */
typedef struct yvid_record {
    int64_t  display;        /* yscr_frame.index                             */
    int64_t  frame;          /* video frame index                              */
    int64_t  due;            /* RT time the frame was due on the movie clock   */
    int64_t  movie_t;        /* movie time at the predicted onset              */
    int64_t  predicted;      /* the predicted onset                            */
    int64_t  onset;          /* the flip's onset; 0 while pending              */
    uint8_t  decision;       /* yvid_decision                                */
    uint8_t  why;            /* yvid_why                                     */
    uint8_t  tier;           /* yscr_tier numbering; 0 while pending         */
    uint8_t  path;
    uint16_t flags;          /* YVID_F_*                                     */
    uint16_t ahead;          /* frames ready after this one                    */
    uint32_t shows;          /* display frames this frame has been on screen   */
    uint32_t reserved_;
} yvid_record;

#define YVID_F_PENDING        0x001u  /* the flip has no record yet          */
#define YVID_F_LATE           0x002u  /* the flip showed after its vblank    */
#define YVID_F_ESTIMATED      0x004u  /* t_ns is not an OS time              */
#define YVID_F_BELOW_TIER     0x008u
#define YVID_F_AUDIO_CLOCK    0x010u  /* the movie clock followed another clock */
#define YVID_F_TS_MISMATCH    0x020u  /* the decoder's time is not the index's */
#define YVID_F_HASH_MISMATCH  0x040u  /* the frame's hash is not the index's */
#define YVID_F_NOT_SHOWN      0x080u  /* the flip was skipped or canceled    */

/* Kinds under YRT_SRC_VIDEO. */
#define YVID_EV_FRAME   1u
#define YVID_EV_DROP    2u
#define YVID_EV_OPEN    3u
#define YVID_EV_PLAY    4u
#define YVID_EV_PAUSE   5u
#define YVID_EV_SEEK    6u
#define YVID_EV_LOOP    7u
#define YVID_EV_END     8u
#define YVID_EV_CLOCK   9u
#define YVID_EV_DECODE 10u
#define YVID_EV_SOUND  11u   /* the soundtrack started or stopped (SOUNDTRACK) */
#define YVID_EV_RATE   12u   /* the movie base's rate changed (BASE RATES)       */

#define YVID_EV_DECISION_OF(w) ((unsigned)(w) & 0x3u)
#define YVID_EV_WHY_OF(w)      (((unsigned)(w) >> 2) & 0xFu)
#define YVID_EV_TIER_OF(w)     (((unsigned)(w) >> 6) & 0x7u)
#define YVID_EV_PATH_OF(w)     (((unsigned)(w) >> 9) & 0x3u)

typedef struct yvid_stim_desc {
    ygfx_align place, anchor;      /* 0 = CENTER                            */
    float x, y, w, h, ori;           /* w, h 0 = the frame's texels           */
    float opacity;                   /* 0 = 1                                 */
    bool  linear;                    /* filter when scaled; default nearest   */
    const ygfx_group* group;
} yvid_stim_desc;

/* --- soundtrack (yvid_soundtrack(), with ysp/audio.h) ------------------------ */

/* The soundtrack is a WAV (RIFF, RF64, BW64; 16-bit, 24-bit or float)
 * that ysp/audio.h's yau_wav reads; the movie's decode thread feeds its
 * stream. */
typedef struct yvid_soundtrack_desc {
    const char*          path;       /* a WAV file; or                          */
    const void*          data;       /* the file in memory, kept by you; or     */
    size_t               size;
    const yvid_reader* reader;     /* bytes by range (a pack entry)           */
    void*                reader_ctx;
    float    db;                     /* gain; 0 = unity                        */
    uint64_t channels;               /* output mask, as yau_play_desc; 0 = all */
    int64_t  ramp_ns;                /* raised-cosine fade that ends a stop, a
                                      * pause or a seek; 0 = cut at the frame  */
    int64_t  ring;                   /* ring frames; 0 = 1 s                   */
} yvid_soundtrack_desc;

/* --- frame sequence writer ---------------------------------------------------- */

typedef struct yvid_seq_desc {
    const char* path;
    int32_t  w, h;
    int32_t  format;                 /* YVID_FMT_*                          */
    int32_t  compression;            /* YVID_SEQ_RAW, or _QOI for RGBA8     */
    int32_t  fps_num, fps_den;       /* den 0 = 1                             */
    uint8_t  matrix, range, transfer, primaries, siting;  /* YUV: required    */
    uint8_t  reserved_[3];
    int64_t  max_frames;             /* > 0: the index is reserved at create
                                      * and no frame allocates; 0 = grow     */
} yvid_seq_desc;

typedef struct yvid_seq_meta {     /* per frame, for capture; NULL = zeros  */
    int64_t t_ns, trial, event, camera_ns;
} yvid_seq_meta;

/* Writer handle. Caller-allocated and zeroed; private fields. */
typedef struct yvid_seq {
    void*    fh;
    yvid_seq_desc d;
    int64_t  n, cap, offset;
    uint8_t* index;
    uint8_t* scratch;
    size_t   scratch_bytes;
    int      open;
    char     error[256];
} yvid_seq;

/* --- the movie (private fields) ----------------------------------------------- */

#ifndef YVID_MAX_SLOTS
#define YVID_MAX_SLOTS 34
#endif
#define YVID__QCAP     64          /* power of two above YVID_MAX_SLOTS   */
#define YVID__MAX_PEND 16
#define YVID__RESULTS  8

typedef struct yvid__slot {
    int64_t  g;              /* global frame: cycle * frames + index           */
    int64_t  key;            /* keyframe used to reach it after a seek, or -1  */
    int64_t  discarded;      /* frames decoded and discarded before it         */
    uint32_t epoch;
    uint32_t flags;          /* YVID_F_TS_MISMATCH, _HASH_MISMATCH          */
    uint8_t* data;
    /* YVID_PATH_GPU: the slot's own NV12 texture on the screen's device,
     * and its import into ysp/gfx.h */
    void*    gtex;
    ygfx_tex gimp;
} yvid__slot;

typedef struct yvid__ctl {
    int64_t  id, t, target;
    int32_t  op, active;
} yvid__ctl;

typedef struct yvid__result {
    int64_t     id;
    yrt_event ev;
} yvid__result;

typedef struct yvid_movie {
    int                 open;
    int                 state;
    ygfx_gfx*         gfx;
    yscr_screen*      screen;
    ygfx_tex          tex;
    yvid_info         info;
    yvid_desc         d;
    const yvid_decoder* dec;
    void*               dec_ctx;
    void*               be_mem;          /* a built-in backend's state        */
    uint64_t*           hashes;          /* the index's, or NULL              */
    unsigned char*      mem_raw;         /* slots, as allocated               */
    int64_t             timescale;
    uint32_t            dec_caps;
    int32_t             n_slots;
    yvid__slot        slots[YVID_MAX_SLOTS];
    /* queues: ready (decode -> frame) and free (frame -> decode) */
    uint32_t            ready_q[YVID__QCAP];
    uint32_t            free_q[YVID__QCAP];
    uint32_t            ready_head, ready_tail, free_head, free_tail;
    uint32_t            idle, dec_err;
    int64_t             want_g;
    /* decode thread */
    uint32_t            dt_epoch;
    int64_t             dt_next, dt_pos, dt_key, dt_discarded;
    int                 dt_eos;
    char                dt_error[256];
    /* frame thread */
    uint32_t            ft_epoch;
    int64_t             last_display, last_vblank;
    int                 has_display;
    int64_t             anchor_rt, anchor_mt;
    int                 running;
    int64_t             cycle;
    int64_t             shown_g;             /* -1 = nothing yet               */
    uint32_t            shows;
    int64_t             late_from;
    int64_t             nom_m, nom_vb, nom_d;
    int                 nom_set;
    int                 just;                /* SEEK, LOOP, MANUAL for the next SHOWN */
    int64_t             seek_g, seek_id, seek_req_onset;
    int                 seek_resume_kind;    /* 0 ASAP, 1 at t, 2 stay paused  */
    int64_t             seek_resume_t;
    int64_t             manual_g;
    int                 manual_req;
    int                 seek_posted;
    yvid__ctl         ctl;                 /* a pending play or pause        */
    int64_t             next_id;
    yvid__result      results[YVID__RESULTS];
    int                 n_results;
    yvid_record       pend[YVID__MAX_PEND];
    int                 n_pend;
    yvid_record       last;
    int                 has_last;
    int64_t             lead_ns_last, period_last;
    int64_t             follow_f0;           /* the other clock's unit at mt 0 */
    int64_t             follow_last_m;
    uint32_t            follow_rate;
    uint32_t            anchor_epoch, follow_epoch;
    int                 following;
    uint64_t            upload_ns_last, upload_ns_max;
    int                 inline_mode;
    char                inline_why[128];     /* why the decode thread did not start */
    int                 dt_spare;            /* decode thread: a free slot it took and did not fill */
    int                 gpu;                 /* YVID_PATH_GPU                  */
    int                 gpu_shown;           /* the slot on screen; -1 = none     */
    int32_t             gpu_w, gpu_h;        /* the decoder's surface size        */
    int32_t             gpu_ax, gpu_ay;      /* the visible frame's corner in it  */
    /* the soundtrack (ysp/audio.h's part); snd NULL = none */
    void*               snd;
    const struct yvid__snd_ops* snd_ops;
    uint32_t            snd_rate;            /* Hz                              */
    uint32_t            snd_wake;            /* a wake for the feeder is queued */
    int64_t             snd_frames;          /* samples in one pass             */
    int64_t             snd_want;            /* the refill's first sample       */
    int64_t             snd_id;              /* ysp_audio's play id; 0 = none   */
    int64_t             snd_t;               /* the start's target              */
    int                 snd_phase;           /* 0 stopped, 1 refilling, 2 started */
    int                 snd_ctl;             /* the pending control has its sound */
    int32_t             rate_num, rate_den;  /* the movie base's rate, as last seen */
    int                 nom_resync;          /* the nominal schedule starts again here */
    char                error[512];
#if !defined(YRT_NO_THREADS)
    yrt_pump          pump;
#endif
} yvid_movie;

/* --- API ------------------------------------------------------------------- */

YVID_API const char* yvid_version(void);
YVID_API const char* yvid_strerror(int code);

/* The rule and the times, as pure functions, in ns. num/den is the rate;
 * m_ns a movie time; lead_ns = yvid_lead_ns(lead, period_ns), lead a
 * fraction of the period. yvid_frame_time() is frame i's movie time. */
YVID_API int64_t yvid_frame_time(int32_t num, int32_t den, int64_t i);
YVID_API int64_t yvid_due(int32_t num, int32_t den, int64_t m_ns, int64_t lead_ns);
YVID_API int64_t yvid_lead_ns(double lead, int64_t period_ns);

/* Reads the description (a frame sequence's header, or the index of a
 * codec file) without opening a movie. 0, or a negative code with the
 * reason in err (may be NULL). */
YVID_API int         yvid_probe(const yvid_desc* d, yvid_info* out, char* err, size_t cap);

/* Opens the movie on a gfx (NULL: decode and schedule, no texture). The
 * handle must be zeroed or closed. False with yvid_error() set. Every
 * later call on the movie that returns a negative code writes yvid_error()
 * first; the calls on a const movie read it and return the code only. */
YVID_API bool        yvid_open(yvid_movie* mv, ygfx_gfx* g, const yvid_desc* d);
YVID_API void        yvid_close(yvid_movie* mv);
YVID_API const char* yvid_error(const yvid_movie* mv);
YVID_API bool        yvid_is_open(const yvid_movie* mv);
YVID_API void        yvid_get_info(const yvid_movie* mv, yvid_info* out);
YVID_API int         yvid_describe(const yvid_movie* mv, char* buf, size_t cap);
YVID_API ygfx_stim yvid_stim(const yvid_movie* mv, const yvid_stim_desc* d);
YVID_API ygfx_tex  yvid_texture(const yvid_movie* mv);

/* Controls. Each returns an id > 0, or a negative code. Times are ns:
 * t_ns and resume_at_ns on the ysp_rt clock (or YVID_ASAP,
 * YVID_STAY_PAUSED), movie_ns on the movie's clock. */
YVID_API int64_t yvid_play_at(yvid_movie* mv, int64_t t_ns);
YVID_API int64_t yvid_pause_at(yvid_movie* mv, int64_t t_ns);
YVID_API int64_t yvid_seek(yvid_movie* mv, int64_t movie_ns, int64_t resume_at_ns);
YVID_API int64_t yvid_seek_frame(yvid_movie* mv, int64_t frame, int64_t resume_at_ns);
YVID_API int     yvid_show(yvid_movie* mv, int64_t frame);
YVID_API int     yvid_result(const yvid_movie* mv, int64_t id, yrt_event* out);

/* The frame: after yscr_begin(), before ygfx_begin(). */
YVID_API int     yvid_update(yvid_movie* mv, const yscr_frame* f);
YVID_API void    yvid_flip_done(yvid_movie* mv, const yscr_record* r);
YVID_API int     yvid_last(const yvid_movie* mv, yvid_record* out);
/* The movie time, ns, at t_ns on the ysp_rt clock. */
YVID_API int64_t yvid_movie_time(const yvid_movie* mv, int64_t t_ns);

/* Following another clock: before yvid_update(), every frame. unit_at(t)
 * is the clock's unit (a stream frame) at RT time t; rate its units per
 * second. Re-anchors the movie clock at f->onset. */
typedef int64_t (*yvid_unit_at_fn)(void* ctx, int64_t t);
YVID_API int yvid_follow(yvid_movie* mv, const yscr_frame* f,
                             yvid_unit_at_fn unit_at, void* ctx, uint32_t rate);

/* Frame sequences. open creates the file (false with yvid_seq_error());
 * write appends a frame; finish writes the index and the header and
 * closes the file, and returns 0 or a negative code: the file is complete
 * only after a finish that returned 0. close releases the writer: safe on
 * a zeroed writer, after a failed open, after finish and twice; a writer
 * not finished is finished first, its outcome in yvid_seq_error(). Each
 * failing call writes yvid_seq_error(). */
YVID_API bool yvid_seq_open(yvid_seq* w, const yvid_seq_desc* d);
YVID_API int  yvid_seq_write(yvid_seq* w, const void* const planes[3],
                                 const int32_t strides[3], const yvid_seq_meta* meta);
YVID_API int  yvid_seq_finish(yvid_seq* w);
YVID_API void yvid_seq_close(yvid_seq* w);
YVID_API const char* yvid_seq_error(const yvid_seq* w);

/* The index of an MPEG-1 (pl_mpeg) or MP4 (Media Foundation) file: decodes
 * every frame, checks the canonical form, writes index_path (NULL =
 * media_path + ".yspvi"). color: matrix, range, transfer, primaries,
 * siting. MPEG-1: 0 = BT.601, LIMITED, BT1886, BT709, CENTER. MP4: what
 * the stream states, which Media Foundation does not report, so each
 * field must be given (BT.709, LIMITED, BT1886, BT709, LEFT for a usual
 * x264 file); a given field that differs from a reported one is refused.
 * Returns frames, or a negative code with err. */
typedef struct yvid_index_desc {
    uint8_t matrix, range, transfer, primaries, siting;
    uint8_t reserved_[3];
    const char* note;                /* the command that made the file        */
    yvid_backend backend;          /* 0 = from the file: MPEG-PS pl_mpeg, MP4 MF */
    yvid_hw   hw;                  /* MF: the decoder that makes the hashes; 0 = OFF */
    bool        no_seek_check;       /* MF: skip the decode from each keyframe */
} yvid_index_desc;
YVID_API int64_t yvid_index_make(const char* media_path, const char* index_path,
                                     const yvid_index_desc* d, char* err, size_t cap);

/* Small pieces, public for tools and tests. */
YVID_API uint64_t yvid_xxh64(const void* data, size_t n, uint64_t seed);
YVID_API int64_t  yvid_qoi_encode(const uint8_t* rgba, int32_t w, int32_t h, int32_t channels,
                                      uint8_t* out, size_t cap);
YVID_API int      yvid_qoi_decode(const uint8_t* in, size_t n, uint8_t* rgba, int32_t w, int32_t h);
YVID_API size_t   yvid_qoi_max_bytes(int32_t w, int32_t h, int32_t channels);
/* rgba and stride must be 4-byte aligned, and rows 2-byte aligned (at least
 * yvid_yuv_rows_bytes(w) bytes): the conversion stores whole pixels and
 * returns YVID_ERR_ARG otherwise. A uint8_t array has no such alignment;
 * use a uint32_t array or malloc. */
YVID_API int      yvid_yuv_to_rgba(const yvid_planes* p, int32_t format, int32_t w, int32_t h,
                                       int32_t matrix, int32_t range, int32_t siting, int32_t chroma,
                                       uint8_t* rgba, int32_t stride, int16_t* rows);
YVID_API size_t   yvid_yuv_rows_bytes(int32_t w);
YVID_API uint64_t yvid_heap_calls(void);

/* The desc fields a designer sets (yscr_param's shape); *n gets the count.
 * An enum row's names are its C enum's suffixes in lower case ("dxva" is
 * YVID_HW_DXVA). */
YVID_API const yscr_param* yvid_params(int* n);

#ifdef __cplusplus
}
#endif

#endif /* YSP_VIDEO_H_INCLUDED */

/* --- with ysp/audio.h included first ----------------------------------------- */
#if defined(YSP_AUDIO_H_INCLUDED) && !defined(YSP_VIDEO_AUDIO_INCLUDED)
#define YSP_VIDEO_AUDIO_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif
/* yvid_follow() on the audio fit. With a soundtrack, the movie time is
 * the soundtrack's sample at f->onset (SOUNDTRACK); without one, the
 * device's stream frame at the output minus its frame at movie time 0. */
YVID_API int yvid_follow_audio(yvid_movie* mv, yau_audio* au, const yscr_frame* f);

/* After yvid_open(), before the first play: the movie's sound
 * (SOUNDTRACK). YVID_ERR_REFUSED with yvid_error() for a rate or
 * channel count that is not the device's, a length that is not the movie's
 * duration in samples, a loop that is not a whole number of samples, or a
 * base rate other than 1. */
YVID_API int yvid_soundtrack(yvid_movie* mv, yau_audio* au, const yvid_soundtrack_desc* d);
#ifdef __cplusplus
}
#endif
#endif

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_VIDEO_IMPLEMENTATION
#ifndef YSP_VIDEO_IMPLEMENTATION_GUARD
#define YSP_VIDEO_IMPLEMENTATION_GUARD

#ifndef YSP_GFX_IMPLEMENTATION_GUARD
    #define YSP_GFX_IMPLEMENTATION
    #include "ysp/gfx.h"
#endif
#ifndef YSP_TIMELINE_IMPLEMENTATION_GUARD
    #define YSP_TIMELINE_IMPLEMENTATION
    #include "ysp/timeline.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

/* Media Foundation: Windows, unless YVID_NO_MF. The SDK's headers give
 * the interfaces; the DLLs are loaded at run time. */
#if defined(_WIN32) && !defined(YVID_NO_MF)
    #define YVID__MF 1
    #if defined(_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable: 4201)   /* the SDK's nameless unions in C */
    #endif
    #include <windows.h>
    #include <objidl.h>
    #include <d3d11.h>
    #include <d3d10.h>
    #include <dxgi.h>
    #include <mfapi.h>
    #include <mfidl.h>
    #include <mfreadwrite.h>
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #endif
#else
    #define YVID__MF 0
#endif

#ifndef YVID_MALLOC
#define YVID_MALLOC(n)     malloc(n)
#define YVID_REALLOC(p, n) realloc((p), (n))
#define YVID_FREE(p)       free(p)
#endif
#ifndef YVID__NOW
#define YVID__NOW() (yrt_now_ns())
#endif
/* Decode and upload times are costs: the real clock, also on ysp/rt.h's
 * virtual clock, where yrt_now_ns() stands still between waits. */
#ifndef YVID__COST_NOW
#define YVID__COST_NOW() (yrt_real_ns())
#endif


/* --- atomics ---------------------------------------------------------------
 * The queues are single-producer single-consumer: an acquire load of the
 * other side's index and a release store of one's own. The idle flag is a
 * Dekker pair, so both sides use a full-barrier exchange. */
#if defined(_MSC_VER)
    #include <intrin.h>
static uint32_t yvid__ld32(const uint32_t* p) { uint32_t v = *(const volatile uint32_t*)p; _ReadWriteBarrier(); return v; }
static void yvid__st32(uint32_t* p, uint32_t v) { _ReadWriteBarrier(); *(volatile uint32_t*)p = v; }
static int64_t yvid__ld64(const int64_t* p) { return _InterlockedCompareExchange64((volatile __int64*)(uintptr_t)p, 0, 0); }
static void yvid__st64(int64_t* p, int64_t v) { (void)_InterlockedExchange64((volatile __int64*)p, v); }
static uint32_t yvid__xchg32(uint32_t* p, uint32_t v) { return (uint32_t)_InterlockedExchange((volatile long*)p, (long)v); }
static uint64_t yvid__inc64(uint64_t* p) { return (uint64_t)_InterlockedIncrement64((volatile __int64*)p); }
#elif defined(__GNUC__) || defined(__clang__)
static uint32_t yvid__ld32(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yvid__st32(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static int64_t yvid__ld64(const int64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yvid__st64(int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint32_t yvid__xchg32(uint32_t* p, uint32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static uint64_t yvid__inc64(uint64_t* p) { return __atomic_add_fetch(p, 1, __ATOMIC_RELAXED); }
#else
#error "ysp_video: needs atomic loads and stores (GCC, Clang or MSVC builtins)"
#endif

/* --- heap ----------------------------------------------------------------- */

static uint64_t yvid__heap_calls_n;

static void* yvid__malloc(size_t n) { yvid__inc64(&yvid__heap_calls_n); return YVID_MALLOC(n); }
static void* yvid__realloc(void* p, size_t n) { yvid__inc64(&yvid__heap_calls_n); return YVID_REALLOC(p, n); }
static void  yvid__free(void* p) { if (p) YVID_FREE(p); }

YVID_API uint64_t yvid_heap_calls(void) { return (uint64_t)yvid__ld64((const int64_t*)(const void*)&yvid__heap_calls_n); }

/* --- pl_mpeg ---------------------------------------------------------------- */

#ifndef YVID_NO_PL_MPEG
    #define PLM_MALLOC(sz)     yvid__malloc(sz)
    #define PLM_FREE(p)        yvid__free(p)
    #define PLM_REALLOC(p, sz) yvid__realloc((p), (sz))
    #if defined(_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable: 4244 4267 4305 4100 4189)
    #elif defined(__clang__)
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wshorten-64-to-32"
        #pragma clang diagnostic ignored "-Wfloat-conversion"
        #pragma clang diagnostic ignored "-Wunknown-warning-option"
    #elif defined(__GNUC__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wfloat-conversion"
    #endif
    #if !defined(YVID_PL_MPEG_EXTERNAL)
        #define PL_MPEG_IMPLEMENTATION
    #endif
    #include "pl_mpeg.h"
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #elif defined(__clang__)
        #pragma clang diagnostic pop
    #elif defined(__GNUC__)
        #pragma GCC diagnostic pop
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define YVID__NS INT64_C(1000000000)

enum { YVID__STOPPED = 0, YVID__PLAYING, YVID__PAUSED, YVID__SEEKING, YVID__MANUAL,
       YVID__ENDED, YVID__FAILED };
enum { YVID__OP_PLAY = 1, YVID__OP_PAUSE = 2 };
enum { YVID__MSG_WAKE = 1, YVID__MSG_SEEK = 2, YVID__MSG_SND = 3 };

/* The soundtrack's functions, from the part compiled with ysp/audio.h. */
struct yvid__snd_ops {
    bool    (*step)(void* s);                      /* decode thread: feed one block */
    void    (*refill)(void* s, int64_t sample);    /* decode thread: refill from sample */
    void    (*refilling)(void* s);                 /* frame thread: a refill is posted */
    int     (*ready)(void* s, int64_t sample);     /* the refill from sample is in   */
    bool    (*wants)(void* s);                     /* the feeder has work            */
    int64_t (*lead)(void* s);                      /* ysp_audio's lead now           */
    int64_t (*start)(void* s, int64_t t);          /* play at t; the play id or < 0  */
    void    (*stop)(void* s, int64_t t);           /* stop at t; 0 = at once         */
    void    (*close)(void* s);
};

typedef struct yvid__msg { uint32_t op, epoch; int64_t g; } yvid__msg;

static void yvid__fmt(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (!buf || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

YVID_API const char* yvid_version(void) { return YVID_VERSION_STRING; }

YVID_API const char* yvid_strerror(int code) {
    switch (code) {
    case YVID_OK: return "ok";
    case YVID_ENDED: return "ended";
    case YVID_PENDING: return "pending";
    case YVID_ERR_ARG: return "bad argument";
    case YVID_ERR_CLOSED: return "not open";
    case YVID_ERR_FORMAT: return "not the canonical form";
    case YVID_ERR_ORDER: return "call out of turn";
    case YVID_ERR_FULL: return "full";
    case YVID_ERR_DECODER: return "decoder failed";
    case YVID_ERR_REFUSED: return "refused";
    case YVID_ERR_LOST: return "GL context lost";
    case YVID_ERR_IO: return "I/O error";
    case YVID_ERR_NOT_FOUND: return "not found";
    case YVID_ERR_NOT_IMPLEMENTED: return "not implemented";
    default: return code >= 0 ? "ok" : "unknown error";
    }
}

/* A call on a handle: hides the message's first byte while the call runs,
 * so yvid__done() can tell whether the call wrote one. One byte each way,
 * in the frame loop too. e is the handle's error buffer (256 bytes or
 * more), or NULL. */
static char yvid__hold(char* e) {
    char c0 = 0;
    if (e) { c0 = e[0]; e[0] = '\0'; }
    return c0;
}

/* A failure that wrote nothing gets the call's name and the code's text;
 * a success leaves the message of the last failure as it was. */
static int64_t yvid__done(char* e, char c0, int64_t rc, const char* what) {
    if (!e) return rc;
    if (rc < 0) {
        if (!e[0]) yvid__fmt(e, 256, "ysp_video: %s: %s", what, yvid_strerror((int)rc));
    } else if (!e[0]) {
        e[0] = c0;
    }
    return rc;
}

/* --- time ----------------------------------------------------------------- */

static int64_t yvid__floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* The open check keeps num * den * 1e9 below 2^63, so r * den * 1e9 fits. */
YVID_API int64_t yvid_frame_time(int32_t num, int32_t den, int64_t i) {
    int64_t q, r;
    if (num <= 0 || den <= 0) return 0;
    if (i < 0) return -yvid_frame_time(num, den, -i);
    q = i / num;
    r = i % num;
    return q * (int64_t)den * YVID__NS + ((r * (int64_t)den * YVID__NS) + num - 1) / num;
}

YVID_API int64_t yvid_due(int32_t num, int32_t den, int64_t m, int64_t lead_ns) {
    int64_t x = m + lead_ns, d, a, b;
    if (num <= 0 || den <= 0) return 0;
    d = (int64_t)den * YVID__NS;
    a = yvid__floordiv(x, d);
    b = x - a * d;
    return a * num + (b * num) / d;
}

YVID_API int64_t yvid_lead_ns(double lead, int64_t period) {
    double l = lead == 0.0 ? 0.5 : lead < 0.0 ? 0.0 : lead;
    if (period <= 0) return 0;
    return (int64_t)(l * (double)period);
}

/* --- XXH64 ------------------------------------------------------------------
 * Its own copy rather than a dependency: the frame hash is the index's
 * check, and the format's reference vectors are in the test. */
#define YVID__P1 UINT64_C(0x9E3779B185EBCA87)
#define YVID__P2 UINT64_C(0xC2B2AE3D27D4EB4F)
#define YVID__P3 UINT64_C(0x165667B19E3779F9)
#define YVID__P4 UINT64_C(0x85EBCA77C2B2AE63)
#define YVID__P5 UINT64_C(0x27D4EB2F165667C5)

typedef struct yvid__xxh {
    uint64_t v[4];
    uint64_t total;
    unsigned char mem[32];
    uint32_t memsize;
    uint64_t seed;
} yvid__xxh;

static uint64_t yvid__rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
/* MSVC does not fuse the byte loads into one load, which halved XXH64's
 * rate there (measured, docs/video.md "Frame sequence and the index");
 * every MSVC target is little-endian. */
#if defined(_MSC_VER)
static uint64_t yvid__rd64(const unsigned char* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t yvid__rd32(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
#else
static uint64_t yvid__rd64(const unsigned char* p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static uint32_t yvid__rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#endif
static uint64_t yvid__xround(uint64_t acc, uint64_t in) {
    acc += in * YVID__P2;
    acc = yvid__rotl(acc, 31);
    return acc * YVID__P1;
}
static uint64_t yvid__xmerge(uint64_t acc, uint64_t v) {
    acc ^= yvid__xround(0, v);
    return acc * YVID__P1 + YVID__P4;
}
static void yvid__xxh_init(yvid__xxh* s, uint64_t seed) {
    memset(s, 0, sizeof *s);
    s->seed = seed;
    s->v[0] = seed + YVID__P1 + YVID__P2;
    s->v[1] = seed + YVID__P2;
    s->v[2] = seed;
    s->v[3] = seed - YVID__P1;
}
static void yvid__xxh_update(yvid__xxh* s, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    const unsigned char* end = p + n;
    s->total += n;
    if (s->memsize + n < 32) {
        if (n) memcpy(s->mem + s->memsize, p, n);
        s->memsize += (uint32_t)n;
        return;
    }
    if (s->memsize) {
        memcpy(s->mem + s->memsize, p, 32 - s->memsize);
        s->v[0] = yvid__xround(s->v[0], yvid__rd64(s->mem));
        s->v[1] = yvid__xround(s->v[1], yvid__rd64(s->mem + 8));
        s->v[2] = yvid__xround(s->v[2], yvid__rd64(s->mem + 16));
        s->v[3] = yvid__xround(s->v[3], yvid__rd64(s->mem + 24));
        p += 32 - s->memsize;
        s->memsize = 0;
    }
    {
        uint64_t v0 = s->v[0], v1 = s->v[1], v2 = s->v[2], v3 = s->v[3];
        while (p + 32 <= end) {
            v0 = yvid__xround(v0, yvid__rd64(p));
            v1 = yvid__xround(v1, yvid__rd64(p + 8));
            v2 = yvid__xround(v2, yvid__rd64(p + 16));
            v3 = yvid__xround(v3, yvid__rd64(p + 24));
            p += 32;
        }
        s->v[0] = v0; s->v[1] = v1; s->v[2] = v2; s->v[3] = v3;
    }
    if (p < end) {
        memcpy(s->mem, p, (size_t)(end - p));
        s->memsize = (uint32_t)(end - p);
    }
}
static uint64_t yvid__xxh_digest(const yvid__xxh* s) {
    uint64_t h;
    const unsigned char* p = s->mem;
    const unsigned char* end = s->mem + s->memsize;
    if (s->total >= 32) {
        h = yvid__rotl(s->v[0], 1) + yvid__rotl(s->v[1], 7) + yvid__rotl(s->v[2], 12) + yvid__rotl(s->v[3], 18);
        h = yvid__xmerge(h, s->v[0]);
        h = yvid__xmerge(h, s->v[1]);
        h = yvid__xmerge(h, s->v[2]);
        h = yvid__xmerge(h, s->v[3]);
    } else {
        h = s->seed + YVID__P5;
    }
    h += s->total;
    while (p + 8 <= end) {
        h ^= yvid__xround(0, yvid__rd64(p));
        h = yvid__rotl(h, 27) * YVID__P1 + YVID__P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)yvid__rd32(p) * YVID__P1;
        h = yvid__rotl(h, 23) * YVID__P2 + YVID__P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p) * YVID__P5;
        h = yvid__rotl(h, 11) * YVID__P1;
        p++;
    }
    h ^= h >> 33; h *= YVID__P2;
    h ^= h >> 29; h *= YVID__P3;
    h ^= h >> 32;
    return h;
}

YVID_API uint64_t yvid_xxh64(const void* data, size_t n, uint64_t seed) {
    yvid__xxh s;
    yvid__xxh_init(&s, seed);
    yvid__xxh_update(&s, data, n);
    return yvid__xxh_digest(&s);
}

/* --- formats ----------------------------------------------------------------- */

static int yvid__is_yuv(int32_t f) { return f == YVID_FMT_I420 || f == YVID_FMT_NV12; }
static int yvid__is_rgb(int32_t f) { return f >= YVID_FMT_R8 && f <= YVID_FMT_R16; }

/* Bytes per texel as ysp/gfx.h takes the format. */
static int32_t yvid__bpt(int32_t f) {
    switch (f) {
    case YVID_FMT_R8: return 1;
    case YVID_FMT_RG8: return 2;
    case YVID_FMT_RGBA8: return 4;
    case YVID_FMT_R16F: return 4;
    case YVID_FMT_RGBA16F: return 16;
    case YVID_FMT_R32F: return 4;
    case YVID_FMT_RGBA32F: return 16;
    case YVID_FMT_R16: return 2;
    default: return 0;
    }
}

static const char* yvid__fmt_name(int32_t f) {
    switch (f) {
    case YVID_FMT_R8: return "R8";
    case YVID_FMT_RG8: return "RG8";
    case YVID_FMT_RGBA8: return "RGBA8";
    case YVID_FMT_R16F: return "R16F";
    case YVID_FMT_RGBA16F: return "RGBA16F";
    case YVID_FMT_R32F: return "R32F";
    case YVID_FMT_RGBA32F: return "RGBA32F";
    case YVID_FMT_R16: return "R16";
    case YVID_FMT_I420: return "I420";
    case YVID_FMT_NV12: return "NV12";
    case YVID_FMT_P010: return "P010";
    default: return "?";
    }
}

/* The planes of a frame of format f at w x h, tight and contiguous. */
static size_t yvid__planes_layout(int32_t f, int32_t w, int32_t h, uint8_t* base, yvid_planes* p) {
    size_t total = 0;
    int32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    yvid_planes q;
    memset(&q, 0, sizeof q);
    if (yvid__is_rgb(f)) {
        q.w[0] = w; q.h[0] = h; q.stride[0] = w * yvid__bpt(f);
        total = (size_t)q.stride[0] * (size_t)h;
    } else if (f == YVID_FMT_I420) {
        q.w[0] = w; q.h[0] = h; q.stride[0] = w;
        q.w[1] = q.w[2] = cw; q.h[1] = q.h[2] = ch; q.stride[1] = q.stride[2] = cw;
        total = (size_t)w * (size_t)h + 2 * (size_t)cw * (size_t)ch;
    } else if (f == YVID_FMT_NV12) {
        q.w[0] = w; q.h[0] = h; q.stride[0] = w;
        q.w[1] = cw; q.h[1] = ch; q.stride[1] = cw * 2;
        total = (size_t)w * (size_t)h + (size_t)cw * 2 * (size_t)ch;
    }
    if (base) {
        q.data[0] = base;
        if (f == YVID_FMT_I420) {
            q.data[1] = base + (size_t)w * (size_t)h;
            q.data[2] = q.data[1] + (size_t)cw * (size_t)ch;
        } else if (f == YVID_FMT_NV12) {
            q.data[1] = base + (size_t)w * (size_t)h;
        }
    }
    if (p) *p = q;
    return total;
}

static int yvid__n_planes(int32_t f) { return f == YVID_FMT_I420 ? 3 : f == YVID_FMT_NV12 ? 2 : 1; }

/* Bytes of visible texels in plane k (the row length hashed and stored). */
static size_t yvid__row_bytes(int32_t f, const yvid_planes* p, int k) {
    if (yvid__is_rgb(f)) return (size_t)p->w[0] * (size_t)yvid__bpt(f);
    if (f == YVID_FMT_NV12 && k == 1) return (size_t)p->w[1] * 2;
    return (size_t)p->w[k];
}

static uint64_t yvid__hash_planes(int32_t f, const yvid_planes* p) {
    yvid__xxh s;
    int k, y;
    yvid__xxh_init(&s, 0);
    for (k = 0; k < yvid__n_planes(f); k++) {
        size_t rb = yvid__row_bytes(f, p, k);
        for (y = 0; y < p->h[k]; y++)
            yvid__xxh_update(&s, p->data[k] + (size_t)y * (size_t)p->stride[k], rb);
    }
    return yvid__xxh_digest(&s);
}

/* --- QOI --------------------------------------------------------------------
 * The header's own codec into caller-owned memory: the reference decoder
 * allocates its output per call, which the decode thread must not. The
 * encoder makes the reference encoder's bytes (the test compares). */
#define YVID__QOI_INDEX   0x00
#define YVID__QOI_DIFF    0x40
#define YVID__QOI_LUMA    0x80
#define YVID__QOI_RUN     0xc0
#define YVID__QOI_RGB     0xfe
#define YVID__QOI_RGBA    0xff
#define YVID__QOI_MASK    0xc0
#define YVID__QOI_HASH(c) (((unsigned)(c)[0] * 3u + (unsigned)(c)[1] * 5u + (unsigned)(c)[2] * 7u + (unsigned)(c)[3] * 11u) % 64u)

YVID_API size_t yvid_qoi_max_bytes(int32_t w, int32_t h, int32_t channels) {
    if (w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return 0;
    return (size_t)w * (size_t)h * (size_t)(channels + 1) + 14 + 8;
}

static void yvid__wbe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

YVID_API int64_t yvid_qoi_encode(const uint8_t* px, int32_t w, int32_t h, int32_t channels,
                                     uint8_t* out, size_t cap) {
    uint8_t index[64][4];
    uint8_t prev[4] = { 0, 0, 0, 255 }, cur[4];
    size_t p = 0, i, n, last;
    int run = 0;
    static const uint8_t pad[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    if (!px || !out || cap < yvid_qoi_max_bytes(w, h, channels)) return YVID_ERR_ARG;
    memset(index, 0, sizeof index);
    memcpy(out, "qoif", 4);
    yvid__wbe32(out + 4, (uint32_t)w);
    yvid__wbe32(out + 8, (uint32_t)h);
    out[12] = (uint8_t)channels;
    out[13] = 1;   /* all channels linear: device values */
    p = 14;
    n = (size_t)w * (size_t)h;
    last = n - 1;
    for (i = 0; i < n; i++) {
        const uint8_t* s = px + i * (size_t)channels;
        cur[0] = s[0]; cur[1] = s[1]; cur[2] = s[2];
        cur[3] = channels == 4 ? s[3] : prev[3];
        if (memcmp(cur, prev, 4) == 0) {
            run++;
            if (run == 62 || i == last) {
                out[p++] = (uint8_t)(YVID__QOI_RUN | (run - 1));
                run = 0;
            }
        } else {
            unsigned hpos;
            if (run > 0) {
                out[p++] = (uint8_t)(YVID__QOI_RUN | (run - 1));
                run = 0;
            }
            hpos = YVID__QOI_HASH(cur);
            if (memcmp(index[hpos], cur, 4) == 0) {
                out[p++] = (uint8_t)(YVID__QOI_INDEX | hpos);
            } else {
                memcpy(index[hpos], cur, 4);
                if (cur[3] == prev[3]) {
                    int vr = (int)(signed char)(uint8_t)(cur[0] - prev[0]);
                    int vg = (int)(signed char)(uint8_t)(cur[1] - prev[1]);
                    int vb = (int)(signed char)(uint8_t)(cur[2] - prev[2]);
                    int vg_r = vr - vg, vg_b = vb - vg;
                    if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                        out[p++] = (uint8_t)(YVID__QOI_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2));
                    } else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
                        out[p++] = (uint8_t)(YVID__QOI_LUMA | (vg + 32));
                        out[p++] = (uint8_t)((vg_r + 8) << 4 | (vg_b + 8));
                    } else {
                        out[p++] = YVID__QOI_RGB;
                        out[p++] = cur[0]; out[p++] = cur[1]; out[p++] = cur[2];
                    }
                } else {
                    out[p++] = YVID__QOI_RGBA;
                    out[p++] = cur[0]; out[p++] = cur[1]; out[p++] = cur[2]; out[p++] = cur[3];
                }
            }
        }
        memcpy(prev, cur, 4);
    }
    memcpy(out + p, pad, 8);
    p += 8;
    return (int64_t)p;
}

/* Decodes a 4-channel image of exactly w x h into rgba (tight). Every read
 * is bounds-checked: a damaged file fails, it does not overrun. */
YVID_API int yvid_qoi_decode(const uint8_t* in, size_t n, uint8_t* rgba, int32_t w, int32_t h) {
    uint8_t index[64][4];
    uint8_t px[4] = { 0, 0, 0, 255 };
    size_t p = 14, chunks, i, total;
    int run = 0;
    if (!in || !rgba || n < 14 + 8 || memcmp(in, "qoif", 4) != 0) return YVID_ERR_FORMAT;
    if (((uint32_t)in[4] << 24 | (uint32_t)in[5] << 16 | (uint32_t)in[6] << 8 | in[7]) != (uint32_t)w ||
        ((uint32_t)in[8] << 24 | (uint32_t)in[9] << 16 | (uint32_t)in[10] << 8 | in[11]) != (uint32_t)h)
        return YVID_ERR_FORMAT;
    if (in[12] != 3 && in[12] != 4) return YVID_ERR_FORMAT;
    memset(index, 0, sizeof index);
    chunks = n - 8;
    total = (size_t)w * (size_t)h;
    for (i = 0; i < total; i++) {
        if (run > 0) {
            run--;
        } else if (p < chunks) {
            int b1 = in[p++];
            if (b1 == YVID__QOI_RGB) {
                if (p + 3 > chunks) return YVID_ERR_FORMAT;
                px[0] = in[p]; px[1] = in[p + 1]; px[2] = in[p + 2]; p += 3;
            } else if (b1 == YVID__QOI_RGBA) {
                if (p + 4 > chunks) return YVID_ERR_FORMAT;
                px[0] = in[p]; px[1] = in[p + 1]; px[2] = in[p + 2]; px[3] = in[p + 3]; p += 4;
            } else if ((b1 & YVID__QOI_MASK) == YVID__QOI_INDEX) {
                memcpy(px, index[b1], 4);
            } else if ((b1 & YVID__QOI_MASK) == YVID__QOI_DIFF) {
                px[0] = (uint8_t)(px[0] + ((b1 >> 4) & 0x03) - 2);
                px[1] = (uint8_t)(px[1] + ((b1 >> 2) & 0x03) - 2);
                px[2] = (uint8_t)(px[2] + (b1 & 0x03) - 2);
            } else if ((b1 & YVID__QOI_MASK) == YVID__QOI_LUMA) {
                int b2, vg;
                if (p + 1 > chunks) return YVID_ERR_FORMAT;
                b2 = in[p++];
                vg = (b1 & 0x3f) - 32;
                px[0] = (uint8_t)(px[0] + vg - 8 + ((b2 >> 4) & 0x0f));
                px[1] = (uint8_t)(px[1] + vg);
                px[2] = (uint8_t)(px[2] + vg - 8 + (b2 & 0x0f));
            } else {
                run = b1 & 0x3f;
            }
            memcpy(index[YVID__QOI_HASH(px)], px, 4);
        } else {
            return YVID_ERR_FORMAT;   /* data ran out before the pixels did */
        }
        memcpy(rgba + i * 4, px, 4);
    }
    return YVID_OK;
}

/* --- YUV to RGBA8 -------------------------------------------------------------
 * 4:2:0 chroma is brought to 4:4:4 with weights in sixteenths (siting), then
 * the matrix in 16.16 fixed point. Within 1 code of the same weights and
 * matrix in double (the test). */
/* Two int16 chroma rows (one edge texel each side), then per-pixel U and V
 * in int32 for the matrix pass. */
YVID_API size_t yvid_yuv_rows_bytes(int32_t w) {
    return (size_t)((w + 1) / 2 + 2) * 2 * sizeof(int16_t) + (size_t)(w + 2) * 2 * sizeof(int32_t) + 16;
}

static void yvid__yuv_coef(int32_t matrix, int32_t range, int32_t* cm) {
    double kr = 0.299, kb = 0.114, kg, sy, sc, oy;
    if (matrix == YVID_MATRIX_BT709) { kr = 0.2126; kb = 0.0722; }
    else if (matrix == YVID_MATRIX_BT2020) { kr = 0.2627; kb = 0.0593; }
    kg = 1.0 - kr - kb;
    if (range == YVID_RANGE_FULL) { sy = 1.0; sc = 255.0 / 255.0; oy = 0; }
    else { sy = 255.0 / 219.0; sc = 255.0 / 224.0; oy = 16; }
    /* R = sy (Y - oy) + rv (Cr - 128); G = sy (Y - oy) - gu (Cb - 128) - gv (Cr - 128);
     * B = sy (Y - oy) + bu (Cb - 128). Chroma enters in sixteenths. */
    cm[0] = (int32_t)floor(sy * 65536.0 + 0.5);
    cm[1] = (int32_t)floor(sc * 2.0 * (1.0 - kr) * 65536.0 / 16.0 + 0.5);
    cm[2] = (int32_t)floor(sc * 2.0 * kb * (1.0 - kb) / kg * 65536.0 / 16.0 + 0.5);
    cm[3] = (int32_t)floor(sc * 2.0 * kr * (1.0 - kr) / kg * 65536.0 / 16.0 + 0.5);
    cm[4] = (int32_t)floor(sc * 2.0 * (1.0 - kb) * 65536.0 / 16.0 + 0.5);
    cm[5] = (int32_t)oy;
}

/* Two passes per row, each a plain loop a compiler can vectorize: chroma
 * to one U and V per pixel (in sixteenths, minus the offset), then the
 * matrix into packed RGBA. */
YVID_API int yvid_yuv_to_rgba(const yvid_planes* p, int32_t format, int32_t w, int32_t h,
                                  int32_t matrix, int32_t range, int32_t siting, int32_t chroma,
                                  uint8_t* rgba, int32_t stride, int16_t* rows) {
    int32_t cm[8];
    int32_t cw = (w + 1) / 2, ch = (h + 1) / 2, y, x;
    int16_t *vu, *vv;
    int32_t *U, *V;
    int nv12 = format == YVID_FMT_NV12;
    int left = siting == YVID_SITING_LEFT;
    int step = nv12 ? 2 : 1;
    if (!p || !rgba || !rows || w <= 0 || h <= 0 || !yvid__is_yuv(format)) return YVID_ERR_ARG;
    if (((uintptr_t)rgba & 3u) || (stride & 3) || ((uintptr_t)rows & 1u)) return YVID_ERR_ARG;
    yvid__yuv_coef(matrix, range, cm);
    vu = rows + 1;
    vv = rows + (cw + 2) + 1;
    U = (int32_t*)(void*)(((uintptr_t)(rows + 2 * (cw + 2)) + 15) & ~(uintptr_t)15);
    V = U + (w + 2);
    for (y = 0; y < h; y++) {
        const uint8_t* Y = p->data[0] + (size_t)y * (size_t)p->stride[0];
        uint32_t* o = (uint32_t*)(void*)(rgba + (size_t)y * (size_t)stride);
        const int32_t c0 = cm[0], c1 = cm[1], c2 = cm[2], c3 = cm[3], c4 = cm[4], oy = cm[5];
        int32_t cy = y >> 1;
        if (chroma == YVID_CHROMA_NEAREST) {
            const uint8_t* Ua = p->data[1] + (size_t)cy * (size_t)p->stride[1];
            const uint8_t* Va = nv12 ? Ua + 1 : p->data[2] + (size_t)cy * (size_t)p->stride[2];
            for (x = 0; x < cw; x++) {
                int32_t u = 16 * ((int32_t)Ua[x * step] - 128), v = 16 * ((int32_t)Va[x * step] - 128);
                U[2 * x] = u; U[2 * x + 1] = u;
                V[2 * x] = v; V[2 * x + 1] = v;
            }
        } else {
            /* Vertical: chroma rows sit between luma rows (both sitings), so
             * an even luma row takes 3/4 of its chroma row and 1/4 of the one
             * above, an odd row 1/4 of the one below. */
            int32_t cb = (y & 1) ? cy + 1 : cy - 1;
            const uint8_t *Ua, *Ub, *Va, *Vb;
            if (cb < 0) cb = 0;
            if (cb >= ch) cb = ch - 1;
            Ua = p->data[1] + (size_t)cy * (size_t)p->stride[1];
            Ub = p->data[1] + (size_t)cb * (size_t)p->stride[1];
            Va = nv12 ? Ua + 1 : p->data[2] + (size_t)cy * (size_t)p->stride[2];
            Vb = nv12 ? Ub + 1 : p->data[2] + (size_t)cb * (size_t)p->stride[2];
            for (x = 0; x < cw; x++) {
                vu[x] = (int16_t)(3 * (int32_t)Ua[x * step] + (int32_t)Ub[x * step]);
                vv[x] = (int16_t)(3 * (int32_t)Va[x * step] + (int32_t)Vb[x * step]);
            }
            vu[-1] = vu[0]; vv[-1] = vv[0];
            vu[cw] = vu[cw - 1]; vv[cw] = vv[cw - 1];
            if (left) {
                /* co-sited with even columns: even x takes it whole, odd x
                 * the mean of the two around it */
                for (x = 0; x < cw; x++) {
                    U[2 * x] = 4 * vu[x] - 2048;     U[2 * x + 1] = 2 * (vu[x] + vu[x + 1]) - 2048;
                    V[2 * x] = 4 * vv[x] - 2048;     V[2 * x + 1] = 2 * (vv[x] + vv[x + 1]) - 2048;
                }
            } else {
                for (x = 0; x < cw; x++) {
                    U[2 * x] = 3 * vu[x] + vu[x - 1] - 2048; U[2 * x + 1] = 3 * vu[x] + vu[x + 1] - 2048;
                    V[2 * x] = 3 * vv[x] + vv[x - 1] - 2048; V[2 * x + 1] = 3 * vv[x] + vv[x + 1] - 2048;
                }
            }
        }
        for (x = 0; x < w; x++) {
            int32_t yy = c0 * ((int32_t)Y[x] - oy) + 32768;
            int32_t r = (yy + c1 * V[x]) >> 16;
            int32_t g = (yy - c2 * U[x] - c3 * V[x]) >> 16;
            int32_t b = (yy + c4 * U[x]) >> 16;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            /* little-endian: R in the first byte */
            o[x] = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | UINT32_C(0xFF000000);
        }
    }
    return YVID_OK;
}

/* --- byte source ---------------------------------------------------------------- */

typedef struct yvid__src {
    FILE*                fh;
    const uint8_t*       mem;
    const yvid_reader* rd;
    void*                rd_ctx;
    int64_t              size;
} yvid__src;

static int yvid__fseek(FILE* f, int64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static FILE* yvid__fopen(const char* path, const char* mode) {
#if defined(_MSC_VER)
    FILE* f = NULL;
    if (fopen_s(&f, path, mode) != 0) return NULL;
    return f;
#else
    return fopen(path, mode);
#endif
}

static int yvid__src_open(yvid__src* s, const char* path, const void* data, size_t size,
                            const yvid_reader* rd, void* rd_ctx) {
    memset(s, 0, sizeof *s);
    if (path) {
        s->fh = yvid__fopen(path, "rb");
        if (!s->fh) return YVID_ERR_NOT_FOUND;
#if defined(_WIN32)
        if (_fseeki64(s->fh, 0, SEEK_END) != 0) { fclose(s->fh); s->fh = NULL; return YVID_ERR_IO; }
        s->size = _ftelli64(s->fh);
#else
        if (fseeko(s->fh, 0, SEEK_END) != 0) { fclose(s->fh); s->fh = NULL; return YVID_ERR_IO; }
        s->size = (int64_t)ftello(s->fh);
#endif
        if (s->size < 0) { fclose(s->fh); s->fh = NULL; return YVID_ERR_IO; }
        return YVID_OK;
    }
    if (data) { s->mem = (const uint8_t*)data; s->size = (int64_t)size; return YVID_OK; }
    if (rd && rd->read) { s->rd = rd; s->rd_ctx = rd_ctx; s->size = rd->size; return YVID_OK; }
    return YVID_ERR_ARG;
}

static void yvid__src_close(yvid__src* s) {
    if (s->fh) fclose(s->fh);
    memset(s, 0, sizeof *s);
}

/* Reads exactly n bytes at off, or fails. */
static int yvid__src_read(yvid__src* s, int64_t off, void* buf, int64_t n) {
    if (off < 0 || n < 0 || off + n > s->size) return YVID_ERR_IO;
    if (n == 0) return YVID_OK;
    if (s->mem) { memcpy(buf, s->mem + off, (size_t)n); return YVID_OK; }
    if (s->fh) {
        if (yvid__fseek(s->fh, off) != 0) return YVID_ERR_IO;
        return fread(buf, 1, (size_t)n, s->fh) == (size_t)n ? YVID_OK : YVID_ERR_IO;
    }
    if (s->rd) {
        int64_t got = 0;
        while (got < n) {
            int64_t r = s->rd->read(s->rd_ctx, off + got, (uint8_t*)buf + got, n - got);
            if (r <= 0) return YVID_ERR_IO;
            got += r;
        }
        return YVID_OK;
    }
    return YVID_ERR_IO;
}

/* XXH64 of the first and last 64 KB: a cheap identity of the media file;
 * the full SHA-256 is the pack loader's, once. */
static int yvid__src_ends(yvid__src* s, uint64_t* head, uint64_t* tail) {
    unsigned char* buf;
    int64_t n = s->size < 65536 ? s->size : 65536;
    int rc;
    buf = (unsigned char*)yvid__malloc((size_t)(n > 0 ? n : 1));
    if (!buf) return YVID_ERR_FULL;
    rc = yvid__src_read(s, 0, buf, n);
    if (rc == YVID_OK) *head = yvid_xxh64(buf, (size_t)n, 0);
    if (rc == YVID_OK) rc = yvid__src_read(s, s->size - n, buf, n);
    if (rc == YVID_OK) *tail = yvid_xxh64(buf, (size_t)n, 0);
    yvid__free(buf);
    return rc;
}

/* --- little-endian fields ------------------------------------------------------- */

static void yvid__w32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void yvid__w64(uint8_t* p, uint64_t v) { yvid__w32(p, (uint32_t)v); yvid__w32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t yvid__r32(const uint8_t* p) { return yvid__rd32(p); }
static uint64_t yvid__r64(const uint8_t* p) { return yvid__rd64(p); }

/* --- the canonical description ------------------------------------------------ */

typedef struct yvid__canon {
    int32_t w, h, format, fps_num, fps_den, gop, codec;
    int64_t frames;
    uint8_t matrix, range, transfer, primaries, siting;
} yvid__canon;

static int yvid__rate_ok(int32_t num, int32_t den) {
    if (num <= 0 || den <= 0) return 0;
    if ((double)num * (double)den * 1e9 >= 9.2e18) return 0;
    if ((double)num / (double)den > 1000.0 || (double)num / (double)den < 0.001) return 0;
    return 1;
}

static void yvid__rgb_color(yvid__canon* c) {
    if (!c->matrix) c->matrix = YVID_MATRIX_RGB;
    if (!c->range) c->range = YVID_RANGE_FULL;
    if (!c->transfer) c->transfer = YVID_TRC_DEVICE;
    if (!c->primaries) c->primaries = YVID_PRIM_DEVICE;
    if (!c->siting) c->siting = YVID_SITING_NONE;
}

/* The fields a canonical description must have; "" when it has them. */
static int yvid__canon_check(const yvid__canon* c, char* err, size_t cap) {
    if (c->w <= 0 || c->h <= 0 || c->w > 16384 || c->h > 16384) { yvid__fmt(err, cap, "size %dx%d", (int)c->w, (int)c->h); return YVID_ERR_FORMAT; }
    if (c->format == YVID_FMT_P010) { yvid__fmt(err, cap, "10-bit (P010) is refused: the panel link is 8 bits; re-encode as 8-bit 4:2:0"); return YVID_ERR_REFUSED; }
    if (!yvid__is_rgb(c->format) && !yvid__is_yuv(c->format)) { yvid__fmt(err, cap, "pixel format %d is not one ysp/video.h knows", (int)c->format); return YVID_ERR_FORMAT; }
    if (!yvid__rate_ok(c->fps_num, c->fps_den)) { yvid__fmt(err, cap, "rate %d/%d is not a canonical rate", (int)c->fps_num, (int)c->fps_den); return YVID_ERR_FORMAT; }
    if (c->frames <= 0) { yvid__fmt(err, cap, "no frames"); return YVID_ERR_FORMAT; }
    if (c->gop <= 0) { yvid__fmt(err, cap, "no constant GOP length"); return YVID_ERR_FORMAT; }
    if (yvid__is_yuv(c->format)) {
        if (c->matrix < YVID_MATRIX_BT601 || c->matrix > YVID_MATRIX_BT2020) { yvid__fmt(err, cap, "the matrix is unspecified"); return YVID_ERR_FORMAT; }
        if (c->range < YVID_RANGE_LIMITED || c->range > YVID_RANGE_FULL) { yvid__fmt(err, cap, "the range is unspecified"); return YVID_ERR_FORMAT; }
        if (c->transfer < YVID_TRC_BT1886 || c->transfer > YVID_TRC_GAMMA22) { yvid__fmt(err, cap, "the transfer is unspecified"); return YVID_ERR_FORMAT; }
        if (c->primaries < YVID_PRIM_BT709 || c->primaries > YVID_PRIM_BT2020) { yvid__fmt(err, cap, "the primaries are unspecified"); return YVID_ERR_FORMAT; }
        if (c->siting != YVID_SITING_LEFT && c->siting != YVID_SITING_CENTER) { yvid__fmt(err, cap, "the chroma siting is unspecified"); return YVID_ERR_FORMAT; }
    }
    return YVID_OK;
}

/* --- the .yspvi index ------------------------------------------------------------
 * 128-byte header, then frames x u64 XXH64 of the decoded planes, then the
 * note (the command that made the media file). */
#define YVID__IDX_HDR 128

typedef struct yvid__index {
    yvid__canon c;
    int64_t  media_size;
    uint64_t head, tail;
    uint64_t* hashes;
} yvid__index;

static int yvid__index_parse(const uint8_t* b, size_t n, yvid__index* out, int want_hashes, char* err, size_t cap) {
    uint32_t hdr;
    memset(out, 0, sizeof *out);
    if (n < YVID__IDX_HDR || memcmp(b, "YSPVIDX1", 8) != 0) { yvid__fmt(err, cap, "not a ysp_video index"); return YVID_ERR_FORMAT; }
    if (yvid__r32(b + 8) != 1) { yvid__fmt(err, cap, "index version %u is not one this header knows", (unsigned)yvid__r32(b + 8)); return YVID_ERR_FORMAT; }
    hdr = yvid__r32(b + 12);
    if (hdr != YVID__IDX_HDR) { yvid__fmt(err, cap, "index header size %u", (unsigned)hdr); return YVID_ERR_FORMAT; }
    if (yvid__r64(b + 120) != yvid_xxh64(b, 120, 0)) { yvid__fmt(err, cap, "the index header is damaged (hash)"); return YVID_ERR_FORMAT; }
    out->c.codec = (int32_t)yvid__r32(b + 16);
    out->c.w = (int32_t)yvid__r32(b + 20);
    out->c.h = (int32_t)yvid__r32(b + 24);
    out->c.format = (int32_t)yvid__r32(b + 28);
    out->c.fps_num = (int32_t)yvid__r32(b + 32);
    out->c.fps_den = (int32_t)yvid__r32(b + 36);
    out->c.frames = (int64_t)yvid__r64(b + 40);
    out->c.gop = (int32_t)yvid__r32(b + 48);
    out->c.matrix = b[52]; out->c.range = b[53]; out->c.transfer = b[54]; out->c.primaries = b[55]; out->c.siting = b[56];
    out->media_size = (int64_t)yvid__r64(b + 64);
    out->head = yvid__r64(b + 72);
    out->tail = yvid__r64(b + 80);
    if (out->c.frames <= 0 || out->c.frames > ((int64_t)1 << 40) ||
        (size_t)YVID__IDX_HDR + (size_t)out->c.frames * 8 > n) { yvid__fmt(err, cap, "the index is truncated"); return YVID_ERR_FORMAT; }
    if (want_hashes) {
        int64_t i;
        out->hashes = (uint64_t*)yvid__malloc((size_t)out->c.frames * 8);
        if (!out->hashes) { yvid__fmt(err, cap, "out of memory for the index"); return YVID_ERR_FULL; }
        for (i = 0; i < out->c.frames; i++) out->hashes[i] = yvid__r64(b + YVID__IDX_HDR + (size_t)i * 8);
    }
    return YVID_OK;
}

/* Reads path + ".yspvi" or the desc's bytes. */
static int yvid__index_load(const yvid_desc* d, yvid__index* out, int want_hashes, char* err, size_t cap) {
    uint8_t* buf = NULL;
    size_t n = 0;
    int rc;
    if (d->index) {
        return yvid__index_parse((const uint8_t*)d->index, d->index_size, out, want_hashes, err, cap);
    }
    if (!d->path) { yvid__fmt(err, cap, "no index: give desc.index, or a path with its .yspvi beside it"); return YVID_ERR_NOT_FOUND; }
    {
        char ip[1024];
        yvid__src s;
        yvid__fmt(ip, sizeof ip, "%s.yspvi", d->path);
        if (yvid__src_open(&s, ip, NULL, 0, NULL, NULL) != YVID_OK) {
            yvid__fmt(err, cap, "%s has no index (%s); make it with yvid_index_make(), the pack tool's step", d->path, ip);
            return YVID_ERR_NOT_FOUND;
        }
        n = (size_t)s.size;
        buf = (uint8_t*)yvid__malloc(n ? n : 1);
        if (!buf) { yvid__src_close(&s); return YVID_ERR_FULL; }
        rc = yvid__src_read(&s, 0, buf, (int64_t)n);
        yvid__src_close(&s);
        if (rc != YVID_OK) { yvid__free(buf); yvid__fmt(err, cap, "cannot read %s", ip); return rc; }
    }
    rc = yvid__index_parse(buf, n, out, want_hashes, err, cap);
    yvid__free(buf);
    return rc;
}

static void yvid__index_header(uint8_t* b, const yvid__canon* c, int64_t media_size, uint64_t head, uint64_t tail) {
    memset(b, 0, YVID__IDX_HDR);
    memcpy(b, "YSPVIDX1", 8);
    yvid__w32(b + 8, 1);
    yvid__w32(b + 12, YVID__IDX_HDR);
    yvid__w32(b + 16, (uint32_t)c->codec);
    yvid__w32(b + 20, (uint32_t)c->w);
    yvid__w32(b + 24, (uint32_t)c->h);
    yvid__w32(b + 28, (uint32_t)c->format);
    yvid__w32(b + 32, (uint32_t)c->fps_num);
    yvid__w32(b + 36, (uint32_t)c->fps_den);
    yvid__w64(b + 40, (uint64_t)c->frames);
    yvid__w32(b + 48, (uint32_t)c->gop);
    b[52] = c->matrix; b[53] = c->range; b[54] = c->transfer; b[55] = c->primaries; b[56] = c->siting;
    yvid__w64(b + 64, (uint64_t)media_size);
    yvid__w64(b + 72, head);
    yvid__w64(b + 80, tail);
    yvid__w64(b + 120, yvid_xxh64(b, 120, 0));
}

/* --- frame sequence: format -------------------------------------------------- */

#define YVID__SEQ_HDR   4096
#define YVID__SEQ_ALIGN 4096
#define YVID__SEQ_FHDR  64
#define YVID__SEQ_ENTRY 24

static int64_t yvid__align(int64_t v) { return (v + YVID__SEQ_ALIGN - 1) / YVID__SEQ_ALIGN * YVID__SEQ_ALIGN; }

static void yvid__seq_header(uint8_t* b, const yvid__canon* c, int32_t compression, int64_t index_offset) {
    memset(b, 0, 128);
    memcpy(b, "YSPVSEQ1", 8);
    yvid__w32(b + 8, 1);
    yvid__w32(b + 12, YVID__SEQ_HDR);
    yvid__w32(b + 16, (uint32_t)c->w);
    yvid__w32(b + 20, (uint32_t)c->h);
    yvid__w32(b + 24, (uint32_t)c->format);
    yvid__w32(b + 28, (uint32_t)compression);
    yvid__w32(b + 32, (uint32_t)c->fps_num);
    yvid__w32(b + 36, (uint32_t)c->fps_den);
    yvid__w64(b + 40, (uint64_t)c->frames);
    b[48] = c->matrix; b[49] = c->range; b[50] = c->transfer; b[51] = c->primaries; b[52] = c->siting;
    yvid__w64(b + 64, (uint64_t)index_offset);
    yvid__w64(b + 72, yvid_xxh64(b, 72, 0));
}

static int yvid__seq_parse(const uint8_t* b, yvid__canon* c, int32_t* compression, int64_t* index_offset,
                             char* err, size_t cap) {
    memset(c, 0, sizeof *c);
    if (memcmp(b, "YSPVSEQ1", 8) != 0) { yvid__fmt(err, cap, "not a frame sequence"); return YVID_ERR_FORMAT; }
    if (yvid__r32(b + 8) != 1 || yvid__r32(b + 12) != YVID__SEQ_HDR) { yvid__fmt(err, cap, "frame sequence version %u is not one this header knows", (unsigned)yvid__r32(b + 8)); return YVID_ERR_FORMAT; }
    if (yvid__r64(b + 72) != yvid_xxh64(b, 72, 0)) { yvid__fmt(err, cap, "the header is damaged or the writer never closed the file (hash)"); return YVID_ERR_FORMAT; }
    c->w = (int32_t)yvid__r32(b + 16);
    c->h = (int32_t)yvid__r32(b + 20);
    c->format = (int32_t)yvid__r32(b + 24);
    *compression = (int32_t)yvid__r32(b + 28);
    c->fps_num = (int32_t)yvid__r32(b + 32);
    c->fps_den = (int32_t)yvid__r32(b + 36);
    c->frames = (int64_t)yvid__r64(b + 40);
    c->matrix = b[48]; c->range = b[49]; c->transfer = b[50]; c->primaries = b[51]; c->siting = b[52];
    c->gop = 1;
    c->codec = YVID_CODEC_SEQ;
    *index_offset = (int64_t)yvid__r64(b + 64);
    if (*compression != YVID_SEQ_RAW && *compression != YVID_SEQ_QOI) { yvid__fmt(err, cap, "compression %d", (int)*compression); return YVID_ERR_FORMAT; }
    if (*compression == YVID_SEQ_QOI && c->format != YVID_FMT_RGBA8) { yvid__fmt(err, cap, "QOI frames must be RGBA8"); return YVID_ERR_FORMAT; }
    return YVID_OK;
}

/* --- frame sequence: writer ------------------------------------------------------ */

static const uint8_t yvid__zeros[YVID__SEQ_ALIGN] = { 0 };

YVID_API const char* yvid_seq_error(const yvid_seq* w) { return w ? w->error : ""; }

YVID_API bool yvid_seq_open(yvid_seq* w, const yvid_seq_desc* d) {
    yvid__canon c;
    yvid_planes pl;
    char e[200];
    if (!w) return false;
    memset(w, 0, sizeof *w);
    if (!d || !d->path) { yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: desc.path is required"); return false; }
    w->d = *d;
    if (w->d.fps_den == 0) w->d.fps_den = 1;
    memset(&c, 0, sizeof c);
    c.w = d->w; c.h = d->h; c.format = d->format; c.fps_num = w->d.fps_num; c.fps_den = w->d.fps_den;
    c.frames = 1; c.gop = 1; c.codec = YVID_CODEC_SEQ;
    c.matrix = d->matrix; c.range = d->range; c.transfer = d->transfer; c.primaries = d->primaries; c.siting = d->siting;
    if (yvid__is_rgb(c.format)) yvid__rgb_color(&c);
    if (yvid__canon_check(&c, e, sizeof e) < 0) { yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: %s", e); return false; }
    if (d->compression == YVID_SEQ_QOI && d->format != YVID_FMT_RGBA8) { yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: QOI needs RGBA8"); return false; }
    if (d->compression != YVID_SEQ_RAW && d->compression != YVID_SEQ_QOI) { yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: unknown compression %d", (int)d->compression); return false; }
    w->d.matrix = c.matrix; w->d.range = c.range; w->d.transfer = c.transfer; w->d.primaries = c.primaries; w->d.siting = c.siting;
    w->scratch_bytes = yvid__planes_layout(d->format, d->w, d->h, NULL, &pl);
    if (d->compression == YVID_SEQ_QOI) w->scratch_bytes = yvid_qoi_max_bytes(d->w, d->h, 4) + (size_t)d->w * (size_t)d->h * 4;
    w->scratch = (uint8_t*)yvid__malloc(w->scratch_bytes);
    w->cap = d->max_frames > 0 ? d->max_frames : 1024;
    w->index = (uint8_t*)yvid__malloc((size_t)w->cap * YVID__SEQ_ENTRY);
    if (!w->scratch || !w->index) { yvid__free(w->scratch); yvid__free(w->index); w->scratch = w->index = NULL; yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: out of memory"); return false; }
    w->fh = yvid__fopen(d->path, "wb");
    if (!w->fh) { yvid__free(w->scratch); yvid__free(w->index); w->scratch = w->index = NULL; yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: cannot create %s", d->path); return false; }
    /* the header is written at close; until then the file has none, so a
     * reader refuses a file whose writer died (the frame headers rebuild it) */
    if (fwrite(yvid__zeros, 1, YVID__SEQ_HDR, (FILE*)w->fh) != YVID__SEQ_HDR) { fclose((FILE*)w->fh); w->fh = NULL; yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: write failed"); return false; }
    w->offset = YVID__SEQ_HDR;
    w->open = 1;
    return true;
}

static int yvid__seq_write(yvid_seq* w, const void* const planes[3], const int32_t strides[3],
                           const yvid_seq_meta* meta) {
    yvid_planes in;
    uint8_t fh[YVID__SEQ_FHDR];
    FILE* f;
    uint64_t hash;
    size_t data_bytes = 0;
    int k, y, np;
    if (!w || !w->open) return YVID_ERR_CLOSED;
    if (!planes || !strides) return YVID_ERR_ARG;
    f = (FILE*)w->fh;
    yvid__planes_layout(w->d.format, w->d.w, w->d.h, NULL, &in);
    np = yvid__n_planes(w->d.format);
    for (k = 0; k < np; k++) {
        if (!planes[k] || strides[k] < (int32_t)yvid__row_bytes(w->d.format, &in, k)) return YVID_ERR_ARG;
        in.data[k] = (uint8_t*)(uintptr_t)planes[k];
        in.stride[k] = strides[k];
    }
    if (w->n >= w->cap) {
        uint8_t* ni;
        if (w->d.max_frames > 0) return YVID_ERR_FULL;
        ni = (uint8_t*)yvid__realloc(w->index, (size_t)w->cap * 2 * YVID__SEQ_ENTRY);
        if (!ni) return YVID_ERR_FULL;
        w->index = ni;
        w->cap *= 2;
    }
    hash = yvid__hash_planes(w->d.format, &in);
    if (w->d.compression == YVID_SEQ_QOI) {
        uint8_t* tight = w->scratch + yvid_qoi_max_bytes(w->d.w, w->d.h, 4);
        int64_t qn;
        for (y = 0; y < w->d.h; y++)
            memcpy(tight + (size_t)y * (size_t)w->d.w * 4, in.data[0] + (size_t)y * (size_t)in.stride[0], (size_t)w->d.w * 4);
        qn = yvid_qoi_encode(tight, w->d.w, w->d.h, 4, w->scratch, yvid_qoi_max_bytes(w->d.w, w->d.h, 4));
        if (qn < 0) return (int)qn;
        data_bytes = (size_t)qn;
    } else {
        for (k = 0; k < np; k++) data_bytes += yvid__row_bytes(w->d.format, &in, k) * (size_t)in.h[k];
    }
    memset(fh, 0, sizeof fh);
    memcpy(fh, "YSPF", 4);
    yvid__w32(fh + 4, YVID__SEQ_FHDR);
    yvid__w64(fh + 8, (uint64_t)w->n);
    yvid__w32(fh + 16, (uint32_t)data_bytes);
    yvid__w64(fh + 24, hash);
    if (meta) {
        yvid__w64(fh + 32, (uint64_t)meta->t_ns);
        yvid__w64(fh + 40, (uint64_t)meta->trial);
        yvid__w64(fh + 48, (uint64_t)meta->event);
        yvid__w64(fh + 56, (uint64_t)meta->camera_ns);
    }
    if (yvid__fseek(f, w->offset) != 0 || fwrite(fh, 1, sizeof fh, f) != sizeof fh) return YVID_ERR_IO;
    if (w->d.compression == YVID_SEQ_QOI) {
        if (fwrite(w->scratch, 1, data_bytes, f) != data_bytes) return YVID_ERR_IO;
    } else {
        for (k = 0; k < np; k++) {
            size_t rb = yvid__row_bytes(w->d.format, &in, k);
            if ((size_t)in.stride[k] == rb) {
                if (fwrite(in.data[k], 1, rb * (size_t)in.h[k], f) != rb * (size_t)in.h[k]) return YVID_ERR_IO;
            } else {
                for (y = 0; y < in.h[k]; y++)
                    if (fwrite(in.data[k] + (size_t)y * (size_t)in.stride[k], 1, rb, f) != rb) return YVID_ERR_IO;
            }
        }
    }
    {
        int64_t end = w->offset + YVID__SEQ_FHDR + (int64_t)data_bytes;
        int64_t padded = yvid__align(end);
        if (padded > end && fwrite(yvid__zeros, 1, (size_t)(padded - end), f) != (size_t)(padded - end)) return YVID_ERR_IO;
        yvid__w64(w->index + (size_t)w->n * YVID__SEQ_ENTRY, (uint64_t)w->offset);
        yvid__w32(w->index + (size_t)w->n * YVID__SEQ_ENTRY + 8, (uint32_t)data_bytes);
        yvid__w32(w->index + (size_t)w->n * YVID__SEQ_ENTRY + 12, 0);
        yvid__w64(w->index + (size_t)w->n * YVID__SEQ_ENTRY + 16, hash);
        w->offset = padded;
    }
    w->n++;
    return YVID_OK;
}

YVID_API int yvid_seq_write(yvid_seq* w, const void* const planes[3], const int32_t strides[3],
                            const yvid_seq_meta* meta) {
    char* e = w ? w->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__seq_write(w, planes, strides, meta), "seq_write");
}

static int yvid__seq_finish(yvid_seq* w) {
    uint8_t hdr[128];
    yvid__canon c;
    FILE* f;
    int rc = YVID_OK;
    if (!w || !w->open) return YVID_ERR_CLOSED;
    f = (FILE*)w->fh;
    memset(&c, 0, sizeof c);
    c.w = w->d.w; c.h = w->d.h; c.format = w->d.format; c.fps_num = w->d.fps_num; c.fps_den = w->d.fps_den;
    c.frames = w->n;
    c.matrix = w->d.matrix; c.range = w->d.range; c.transfer = w->d.transfer; c.primaries = w->d.primaries; c.siting = w->d.siting;
    if (yvid__fseek(f, w->offset) != 0 ||
        fwrite(w->index, 1, (size_t)w->n * YVID__SEQ_ENTRY, f) != (size_t)w->n * YVID__SEQ_ENTRY) rc = YVID_ERR_IO;
    yvid__seq_header(hdr, &c, w->d.compression, w->offset);
    if (rc == YVID_OK && (yvid__fseek(f, 0) != 0 || fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr)) rc = YVID_ERR_IO;
    if (fclose(f) != 0) rc = YVID_ERR_IO;
    yvid__free(w->index);
    yvid__free(w->scratch);
    w->fh = NULL; w->index = NULL; w->scratch = NULL;
    w->open = 0;
    if (rc == YVID_OK && w->n == 0) { yvid__fmt(w->error, sizeof w->error, "ysp_video: seq: no frames written"); rc = YVID_ERR_ARG; }
    return rc;
}

YVID_API int yvid_seq_finish(yvid_seq* w) {
    char* e = w ? w->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__seq_finish(w), "seq_finish");
}

YVID_API void yvid_seq_close(yvid_seq* w) {
    /* not finished: finish, so the frames are not lost; the outcome is in
     * yvid_seq_error() */
    if (w && w->open) (void)yvid_seq_finish(w);
}

/* --- frame sequence: reader backend ------------------------------------------------ */

typedef struct yvid__seqr {
    yvid__src src;
    yvid__canon c;
    int32_t compression;
    uint8_t* index;           /* frames x 24 bytes                              */
    uint8_t* comp;            /* one compressed frame                          */
    size_t   comp_bytes;
    int64_t  pos;
    size_t   frame_bytes;
} yvid__seqr;

static int yvid__seqr_open(void* ctx, const yvid_decoder_open* in, yvid_stream* out, char* err, size_t cap) {
    yvid__seqr* s = (yvid__seqr*)ctx;
    uint8_t hdr[128];
    int64_t index_offset, i, prev = 0;
    int rc;
    rc = yvid__src_open(&s->src, in->path, in->data, in->size, in->reader, in->reader_ctx);
    if (rc < 0) { yvid__fmt(err, cap, "cannot open %s", in->path ? in->path : "the source"); return rc; }
    if (s->src.size < YVID__SEQ_HDR || yvid__src_read(&s->src, 0, hdr, sizeof hdr) != YVID_OK) { yvid__fmt(err, cap, "too short for a frame sequence"); return YVID_ERR_FORMAT; }
    rc = yvid__seq_parse(hdr, &s->c, &s->compression, &index_offset, err, cap);
    if (rc < 0) return rc;
    if (s->c.frames <= 0 || s->c.frames > ((int64_t)1 << 32) || index_offset < YVID__SEQ_HDR ||
        index_offset + s->c.frames * YVID__SEQ_ENTRY > s->src.size) { yvid__fmt(err, cap, "the index is outside the file"); return YVID_ERR_FORMAT; }
    s->frame_bytes = yvid__planes_layout(s->c.format, s->c.w, s->c.h, NULL, NULL);
    s->index = (uint8_t*)yvid__malloc((size_t)s->c.frames * YVID__SEQ_ENTRY);
    if (!s->index) { yvid__fmt(err, cap, "out of memory for the index"); return YVID_ERR_FULL; }
    if (yvid__src_read(&s->src, index_offset, s->index, s->c.frames * YVID__SEQ_ENTRY) != YVID_OK) { yvid__fmt(err, cap, "cannot read the index"); return YVID_ERR_IO; }
    s->comp_bytes = 0;
    for (i = 0; i < s->c.frames; i++) {
        const uint8_t* e = s->index + (size_t)i * YVID__SEQ_ENTRY;
        int64_t off = (int64_t)yvid__r64(e);
        uint32_t size = yvid__r32(e + 8);
        if (off < YVID__SEQ_HDR || off % YVID__SEQ_ALIGN != 0 || off <= prev - 1 || off + YVID__SEQ_FHDR + (int64_t)size > index_offset) {
            yvid__fmt(err, cap, "index entry %lld is outside the frames", (long long)i);
            return YVID_ERR_FORMAT;
        }
        if (s->compression == YVID_SEQ_RAW && size != s->frame_bytes) { yvid__fmt(err, cap, "frame %lld has %u bytes, the format needs %zu", (long long)i, (unsigned)size, s->frame_bytes); return YVID_ERR_FORMAT; }
        if ((size_t)size > s->comp_bytes) s->comp_bytes = size;
        prev = off + 1;
    }
    if (s->compression == YVID_SEQ_QOI) {
        s->comp = (uint8_t*)yvid__malloc(s->comp_bytes + YVID__SEQ_FHDR);
        if (!s->comp) { yvid__fmt(err, cap, "out of memory"); return YVID_ERR_FULL; }
    }
    out->w = s->c.w; out->h = s->c.h; out->format = s->c.format;
    out->fps_num = s->c.fps_num; out->fps_den = s->c.fps_den;
    out->frames = s->c.frames; out->gop = 1; out->codec = YVID_CODEC_SEQ;
    out->matrix = s->c.matrix; out->range = s->c.range; out->transfer = s->c.transfer;
    out->primaries = s->c.primaries; out->siting = s->c.siting;
    out->timescale = 0;
    out->caps = YVID_DEC_CPU | YVID_DEC_RANDOM_ACCESS;
    s->pos = 0;
    return YVID_OK;
}

static int yvid__seqr_next(void* ctx, yvid_planes* dst, yvid_out* out) {
    yvid__seqr* s = (yvid__seqr*)ctx;
    const uint8_t* e;
    uint8_t fh[YVID__SEQ_FHDR];
    int64_t off;
    uint32_t size;
    if (s->pos >= s->c.frames) return YVID_ENDED;
    e = s->index + (size_t)s->pos * YVID__SEQ_ENTRY;
    off = (int64_t)yvid__r64(e);
    size = yvid__r32(e + 8);
    out->index = s->pos;
    out->pts = s->pos;
    out->hash = yvid__r64(e + 16);
    out->flags = YVID_OUT_KEYFRAME | YVID_OUT_HAS_HASH;
    s->pos++;
    if (!dst) return YVID_OK;   /* every frame is a keyframe: nothing to decode */
    if (yvid__src_read(&s->src, off, fh, sizeof fh) != YVID_OK) return YVID_ERR_IO;
    if (memcmp(fh, "YSPF", 4) != 0 || (int64_t)yvid__r64(fh + 8) != out->index || yvid__r32(fh + 16) != size)
        return YVID_ERR_FORMAT;
    if (s->compression == YVID_SEQ_QOI) {
        if (yvid__src_read(&s->src, off + YVID__SEQ_FHDR, s->comp, size) != YVID_OK) return YVID_ERR_IO;
        if (dst->stride[0] != s->c.w * 4) return YVID_ERR_ARG;
        return yvid_qoi_decode(s->comp, size, dst->data[0], s->c.w, s->c.h);
    }
    /* raw: the core's planes are tight and contiguous, as the file's are */
    return yvid__src_read(&s->src, off + YVID__SEQ_FHDR, dst->data[0], size);
}

static int yvid__seqr_seek(void* ctx, int64_t key, int64_t key_pts) {
    yvid__seqr* s = (yvid__seqr*)ctx;
    (void)key_pts;
    if (key < 0 || key >= s->c.frames) return YVID_ERR_ARG;
    s->pos = key;
    return YVID_OK;
}

static void yvid__seqr_close(void* ctx) {
    yvid__seqr* s = (yvid__seqr*)ctx;
    yvid__src_close(&s->src);
    yvid__free(s->index);
    yvid__free(s->comp);
    s->index = NULL; s->comp = NULL;
}

static int yvid__seqr_describe(void* ctx, char* buf, size_t cap) {
    yvid__seqr* s = (yvid__seqr*)ctx;
    return snprintf(buf, cap, "frame sequence %s", s->compression == YVID_SEQ_QOI ? "QOI" : "RAW");
}

static const yvid_decoder yvid__seq_decoder = {
    YVID_DECODER_VERSION, "seq", yvid__seqr_open, yvid__seqr_next, yvid__seqr_seek,
    yvid__seqr_close, yvid__seqr_describe
};

/* --- pl_mpeg backend ---------------------------------------------------------- */

#ifndef YVID_NO_PL_MPEG

typedef struct yvid__plm {
    plm_t*        plm;
    yvid__src   src;
    int64_t       read_pos;
    plm_frame_t*  pending;          /* the keyframe a seek decoded            */
    int32_t       num, den, w, h;
} yvid__plm;

static void yvid__plm_rate(double fps, int32_t* num, int32_t* den) {
    static const struct { double f; int32_t n, d; } t[] = {
        { 23.976, 24000, 1001 }, { 24.0, 24, 1 }, { 25.0, 25, 1 }, { 29.97, 30000, 1001 },
        { 30.0, 30, 1 }, { 50.0, 50, 1 }, { 59.94, 60000, 1001 }, { 60.0, 60, 1 } };
    size_t i;
    *num = 0; *den = 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (fabs(fps - t[i].f) < 0.002) { *num = t[i].n; *den = t[i].d; }
}

#if !defined(YVID_PL_MPEG_EXTERNAL)
/* Our own source behind pl_mpeg's buffer: 64-bit offsets (pl_mpeg's file
 * buffer uses ftell, 32 bits on Windows) and the reader. The read bytes are
 * discarded before each load, as pl_mpeg's own file buffer does, so the
 * buffer does not grow. */
static void yvid__plm_bload(plm_buffer_t* b, void* user) {
    yvid__plm* p = (yvid__plm*)user;
    uint8_t chunk[16384];
    int64_t n;
    plm_buffer_discard_read_bytes(b);
    n = p->src.size - p->read_pos;
    if (n > (int64_t)sizeof chunk) n = (int64_t)sizeof chunk;
    if (n > (int64_t)(b->capacity - b->length)) n = (int64_t)(b->capacity - b->length);
    if (n <= 0 || yvid__src_read(&p->src, p->read_pos, chunk, n) != YVID_OK) { b->has_ended = TRUE; return; }
    plm_buffer_write(b, chunk, (size_t)n);
    p->read_pos += n;
}
static void yvid__plm_bseek(plm_buffer_t* b, size_t off, void* user) {
    yvid__plm* p = (yvid__plm*)user;
    (void)b;
    p->read_pos = (int64_t)off;
}
static size_t yvid__plm_btell(plm_buffer_t* b, void* user) {
    yvid__plm* p = (yvid__plm*)user;
    (void)b;
    return (size_t)p->read_pos;
}
#endif

static int yvid__plm_open(void* ctx, const yvid_decoder_open* in, yvid_stream* out, char* err, size_t cap) {
    yvid__plm* p = (yvid__plm*)ctx;
    int rc;
    memset(p, 0, sizeof *p);
    if (in->data) {
        p->plm = plm_create_with_memory((uint8_t*)(uintptr_t)in->data, in->size, 0);
    } else {
#if !defined(YVID_PL_MPEG_EXTERNAL)
        plm_buffer_t* b;
        rc = yvid__src_open(&p->src, in->path, NULL, 0, in->reader, in->reader_ctx);
        if (rc < 0) { yvid__fmt(err, cap, "cannot open %s", in->path ? in->path : "the reader"); return rc; }
        b = plm_buffer_create_with_callbacks(yvid__plm_bload, yvid__plm_bseek, yvid__plm_btell, (size_t)p->src.size, p);
        if (b) p->plm = plm_create_with_buffer(b, TRUE);
#else
        if (!in->path) { yvid__fmt(err, cap, "pl_mpeg is external: a path or memory only"); return YVID_ERR_NOT_IMPLEMENTED; }
        p->plm = plm_create_with_filename(in->path);
#endif
    }
    if (!p->plm) { yvid__fmt(err, cap, "pl_mpeg could not open the stream"); return YVID_ERR_DECODER; }
    plm_set_audio_enabled(p->plm, FALSE);
    if (!plm_has_headers(p->plm) || plm_get_num_video_streams(p->plm) < 1) {
        yvid__fmt(err, cap, "pl_mpeg found no MPEG-1 video stream (MPEG-PS with a video stream is required)");
        return YVID_ERR_FORMAT;
    }
#if !defined(YVID_PL_MPEG_EXTERNAL)
    /* The canonical form has no B-frames, so decode order is display order:
     * each frame comes out as it is decoded, not one call later, and its
     * picture type is the one just decoded (the index maker reads it). */
    if (p->plm->video_decoder) plm_video_set_no_delay(p->plm->video_decoder, TRUE);
#endif
    p->w = plm_get_width(p->plm);
    p->h = plm_get_height(p->plm);
    yvid__plm_rate(plm_get_framerate(p->plm), &p->num, &p->den);
    out->w = p->w; out->h = p->h; out->format = YVID_FMT_I420;
    out->fps_num = p->num; out->fps_den = p->den;
    out->frames = -1; out->gop = 0; out->codec = YVID_CODEC_MPEG1;
    out->matrix = out->range = out->transfer = out->primaries = out->siting = -1;
    out->timescale = 90000;
    out->caps = YVID_DEC_CPU;
    return YVID_OK;
}

static int yvid__plm_next(void* ctx, yvid_planes* dst, yvid_out* out) {
    yvid__plm* p = (yvid__plm*)ctx;
    plm_frame_t* f = p->pending;
    p->pending = NULL;
    if (!f) f = plm_decode_video(p->plm);
    if (!f) return plm_has_ended(p->plm) ? YVID_ENDED : YVID_ERR_DECODER;
    out->pts = (int64_t)floor(f->time * 90000.0 + 0.5);
    out->index = -1;
    out->flags = YVID_OUT_BORROWED;
    out->planes.data[0] = f->y.data;  out->planes.stride[0] = (int32_t)f->y.width;
    out->planes.data[1] = f->cb.data; out->planes.stride[1] = (int32_t)f->cb.width;
    out->planes.data[2] = f->cr.data; out->planes.stride[2] = (int32_t)f->cr.width;
    out->planes.w[0] = p->w; out->planes.h[0] = p->h;
    out->planes.w[1] = out->planes.w[2] = (p->w + 1) / 2;
    out->planes.h[1] = out->planes.h[2] = (p->h + 1) / 2;
    (void)dst;
    return YVID_OK;
}

static int yvid__plm_seek(void* ctx, int64_t key, int64_t key_pts) {
    yvid__plm* p = (yvid__plm*)ctx;
    double t;
    (void)key_pts;
    if (p->num <= 0) return YVID_ERR_ARG;
    /* half a frame past the keyframe: pl_mpeg takes the last intra frame
     * before the time, and the PES time is in 90 kHz units */
    t = ((double)key + 0.5) * (double)p->den / (double)p->num;
    p->pending = plm_seek_frame(p->plm, t, FALSE);
    return p->pending ? YVID_OK : YVID_ERR_DECODER;
}

static void yvid__plm_close(void* ctx) {
    yvid__plm* p = (yvid__plm*)ctx;
    if (p->plm) plm_destroy(p->plm);
    yvid__src_close(&p->src);
    p->plm = NULL;
}

static int yvid__plm_describe(void* ctx, char* buf, size_t cap) {
    (void)ctx;
    return snprintf(buf, cap, "pl_mpeg c871f2b (MPEG-1, CPU)");
}

static const yvid_decoder yvid__plm_decoder = {
    YVID_DECODER_VERSION, "pl_mpeg", yvid__plm_open, yvid__plm_next, yvid__plm_seek,
    yvid__plm_close, yvid__plm_describe
};

#endif /* YVID_NO_PL_MPEG */

/* --- Media Foundation backend (Windows) --------------------------------------
 * An IMFSourceReader in synchronous mode, used from the decode thread only
 * after open. MF's DLLs are loaded at run time and its GUIDs are local
 * copies (tests/compile/video_com.cpp checks them against the SDK), so
 * nothing is linked. The reader may not insert a converter or a video
 * processor: a stream that the decoder cannot give as NV12 is refused, not
 * converted. */
#if YVID__MF

/* desc.hw_decode AUTO: chosen by measurement (docs/video.md, M1, M2). */
#ifndef YVID__HW_DEFAULT
#define YVID__HW_DEFAULT YVID_HW_DXVA
#endif

#define YVID__G(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
YVID__G(yvid__GUID_NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
YVID__G(yvid__IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
YVID__G(yvid__IID_ISequentialStream, 0x0c733a30, 0x2a1c, 0x11ce, 0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d);
YVID__G(yvid__IID_IStream, 0x0000000c, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
YVID__G(yvid__IID_IMFAttributes, 0x2cd2d921, 0xc447, 0x44a7, 0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3);
YVID__G(yvid__IID_IMF2DBuffer, 0x7dc9d5f9, 0x9ed9, 0x44ec, 0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb);
YVID__G(yvid__IID_IMF2DBuffer2, 0x33ae5ea6, 0x4316, 0x436f, 0x8d, 0xdd, 0xd7, 0x3d, 0x22, 0xf8, 0x29, 0xec);
YVID__G(yvid__IID_IMFDXGIBuffer, 0xe7174cfa, 0x1c9e, 0x48b1, 0x88, 0x66, 0x62, 0x62, 0x26, 0xbf, 0xc2, 0x58);
YVID__G(yvid__IID_IMFSourceReaderEx, 0x7b981cf0, 0x560e, 0x4116, 0x98, 0x75, 0xb0, 0x99, 0x89, 0x5f, 0x23, 0xd7);
YVID__G(yvid__IID_IMFTransform, 0xbf94c121, 0x5b05, 0x4e6f, 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d);
YVID__G(yvid__IID_ID3D10Multithread, 0x9b7e4e00, 0x342c, 0x4106, 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0);
YVID__G(yvid__IID_IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87);
YVID__G(yvid__MF_MT_MAJOR_TYPE, 0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
YVID__G(yvid__MF_MT_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
YVID__G(yvid__MFMediaType_Video, 0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
YVID__G(yvid__MFVideoFormat_NV12, 0x3231564e, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
YVID__G(yvid__MFVideoFormat_H264, 0x34363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
YVID__G(yvid__MFVideoFormat_HEVC, 0x43564548, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
YVID__G(yvid__MF_MT_FRAME_SIZE, 0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
YVID__G(yvid__MF_MT_FRAME_RATE, 0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0);
YVID__G(yvid__MF_MT_PIXEL_ASPECT_RATIO, 0xc6376a1e, 0x8d0a, 0x4027, 0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6);
YVID__G(yvid__MF_MT_INTERLACE_MODE, 0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd);
YVID__G(yvid__MF_MT_MINIMUM_DISPLAY_APERTURE, 0xd7388766, 0x18fe, 0x48c6, 0xa1, 0x77, 0xee, 0x89, 0x48, 0x67, 0xc8, 0xc4);
YVID__G(yvid__MF_MT_DEFAULT_STRIDE, 0x644b4e48, 0x1e02, 0x4516, 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6);
YVID__G(yvid__MF_MT_YUV_MATRIX, 0x3e23d450, 0x2c75, 0x4d25, 0xa0, 0x0e, 0xb9, 0x16, 0x70, 0xd1, 0x23, 0x27);
YVID__G(yvid__MF_MT_VIDEO_NOMINAL_RANGE, 0xc21b8ee5, 0xb956, 0x4071, 0x8d, 0xaf, 0x32, 0x5e, 0xdf, 0x5c, 0xab, 0x11);
YVID__G(yvid__MF_MT_TRANSFER_FUNCTION, 0x5fb0fce9, 0xbe5c, 0x4935, 0xa8, 0x11, 0xec, 0x83, 0x8f, 0x8e, 0xed, 0x93);
YVID__G(yvid__MF_MT_VIDEO_PRIMARIES, 0xdbfbe4d7, 0x0740, 0x4ee0, 0x81, 0x92, 0x85, 0x0a, 0xb0, 0xe2, 0x19, 0x35);
YVID__G(yvid__MF_MT_VIDEO_CHROMA_SITING, 0x65df2370, 0xc773, 0x4c33, 0xaa, 0x64, 0x84, 0x3e, 0x06, 0x8e, 0xfb, 0x0c);
YVID__G(yvid__MF_MT_VIDEO_ROTATION, 0xc380465d, 0x2271, 0x428c, 0x9b, 0x83, 0xec, 0xea, 0x3b, 0x4a, 0x85, 0xc1);
YVID__G(yvid__MF_MT_MPEG2_PROFILE, 0xad76a80b, 0x2d5c, 0x4e0b, 0xb3, 0x75, 0x64, 0xe5, 0x20, 0x13, 0x70, 0x36);
YVID__G(yvid__MF_MT_MPEG_SEQUENCE_HEADER, 0x3c036de7, 0x3ad0, 0x4c9e, 0x92, 0x16, 0xee, 0x6d, 0x6a, 0xc2, 0x1c, 0xb3);
YVID__G(yvid__MF_SOURCE_READER_D3D11_BIND_FLAGS, 0x33f3197b, 0xf73a, 0x4e14, 0x8d, 0x85, 0x0e, 0x4c, 0x43, 0x68, 0x78, 0x8d);
YVID__G(yvid__IID_ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);
YVID__G(yvid__MF_PD_DURATION, 0x6c990d33, 0xbb8e, 0x477a, 0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a);
YVID__G(yvid__MF_READWRITE_DISABLE_CONVERTERS, 0x98d5b065, 0x1374, 0x4847, 0x8d, 0x5d, 0x31, 0x52, 0x0f, 0xee, 0x71, 0x56);
YVID__G(yvid__MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0);
YVID__G(yvid__MF_SOURCE_READER_D3D_MANAGER, 0xec822da2, 0xe1e9, 0x4b29, 0xa0, 0xd8, 0x56, 0x3c, 0x71, 0x9f, 0x52, 0x69);
YVID__G(yvid__MF_BYTESTREAM_CONTENT_TYPE, 0xfc358289, 0x3cb6, 0x460c, 0xa4, 0x24, 0xb6, 0x68, 0x12, 0x60, 0x37, 0x5a);
YVID__G(yvid__MFSampleExtension_CleanPoint, 0x9cdf01d8, 0xa0f0, 0x43ba, 0xb0, 0x77, 0xea, 0xa0, 0x6c, 0xbd, 0x72, 0x8a);
YVID__G(yvid__MFSampleExtension_Interlaced, 0xb1d5830a, 0xdeb8, 0x40e3, 0x90, 0xfa, 0x38, 0x99, 0x43, 0x71, 0x64, 0x61);
YVID__G(yvid__MFSampleExtension_DecodeTimestamp, 0x73a954d4, 0x09e2, 0x4861, 0xbe, 0xfc, 0x94, 0xbd, 0x97, 0xc0, 0x8e, 0x6e);
YVID__G(yvid__MFT_FRIENDLY_NAME_Attribute, 0x314ffbae, 0x5b41, 0x4c95, 0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3);
YVID__G(yvid__MFT_ENUM_HARDWARE_URL_Attribute, 0x2fb866ac, 0xb078, 0x4942, 0xab, 0x6c, 0x00, 0x3d, 0x05, 0xcd, 0xa6, 0x74);
YVID__G(yvid__MFT_TRANSFORM_CLSID_Attribute, 0x6821c42b, 0x65a4, 0x4e82, 0x99, 0xbc, 0x9a, 0x88, 0x20, 0x5e, 0xcd, 0x0c);
YVID__G(yvid__CLSID_MSH264DecoderMFT, 0x62ce7e72, 0x4c71, 0x4d20, 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d);
#undef YVID__G

/* IsEqualGUID takes pointers in C and references in C++. */
static int yvid__guid_eq(const GUID* a, const GUID* b) { return memcmp(a, b, sizeof *a) == 0; }

#define YVID__MF_FIRST_VIDEO   0xFFFFFFFCu   /* MF_SOURCE_READER_FIRST_VIDEO_STREAM */
#define YVID__MF_ALL_STREAMS   0xFFFFFFFEu   /* MF_SOURCE_READER_ALL_STREAMS        */
#define YVID__MF_MEDIASOURCE   0xFFFFFFFFu   /* MF_SOURCE_READER_MEDIASOURCE        */

typedef HRESULT (WINAPI *yvid__MFStartup_fn)(ULONG, DWORD);
typedef HRESULT (WINAPI *yvid__MFShutdown_fn)(void);
typedef HRESULT (WINAPI *yvid__MFCreateAttributes_fn)(IMFAttributes**, UINT32);
typedef HRESULT (WINAPI *yvid__MFCreateMediaType_fn)(IMFMediaType**);
typedef HRESULT (WINAPI *yvid__MFCreateMFByteStreamOnStream_fn)(IStream*, IMFByteStream**);
typedef HRESULT (WINAPI *yvid__MFCreateDXGIDeviceManager_fn)(UINT*, IMFDXGIDeviceManager**);
typedef HRESULT (WINAPI *yvid__MFCreateSourceReaderFromByteStream_fn)(IMFByteStream*, IMFAttributes*, IMFSourceReader**);
typedef HRESULT (WINAPI *yvid__CoIncrementMTAUsage_fn)(void**);
typedef HRESULT (WINAPI *yvid__CoDecrementMTAUsage_fn)(void*);
typedef HRESULT (WINAPI *yvid__PropVariantClear_fn)(PROPVARIANT*);
typedef HRESULT (WINAPI *yvid__D3D11CreateDevice_fn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                      UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *yvid__CreateDXGIFactory1_fn)(REFIID, void**);

/* Loaded once per process; movies open and close on one thread, as the rest
 * of the API requires, so a plain count is enough. */
static struct {
    int refs, loaded;
    HMODULE plat, rw, ole, d3d, dxgi;
    yvid__MFStartup_fn Startup;
    yvid__MFShutdown_fn Shutdown;
    yvid__MFCreateAttributes_fn CreateAttributes;
    yvid__MFCreateMediaType_fn CreateMediaType;
    yvid__MFCreateMFByteStreamOnStream_fn ByteStreamOnStream;
    yvid__MFCreateDXGIDeviceManager_fn CreateDXGIDeviceManager;
    yvid__MFCreateSourceReaderFromByteStream_fn ReaderFromByteStream;
    yvid__CoIncrementMTAUsage_fn IncMTA;
    yvid__CoDecrementMTAUsage_fn DecMTA;
    yvid__PropVariantClear_fn PropVariantClear;
    yvid__D3D11CreateDevice_fn D3D11CreateDevice;
    yvid__CreateDXGIFactory1_fn CreateDXGIFactory1;
} yvid__mfl;

#define YVID__SYM(m, name) GetProcAddress((m), name)

static int yvid__mf_load(char* err, size_t cap) {
    HRESULT hr;
    if (yvid__mfl.refs > 0) { yvid__mfl.refs++; return YVID_OK; }
    if (!yvid__mfl.loaded) {
        yvid__mfl.plat = LoadLibraryW(L"mfplat.dll");
        yvid__mfl.rw = LoadLibraryW(L"mfreadwrite.dll");
        yvid__mfl.ole = LoadLibraryW(L"ole32.dll");
        if (!yvid__mfl.plat || !yvid__mfl.rw || !yvid__mfl.ole) {
            yvid__fmt(err, cap, "Media Foundation is not installed (mfplat.dll or mfreadwrite.dll missing; on Windows Server add the Media Foundation feature)");
            return YVID_ERR_NOT_IMPLEMENTED;
        }
        yvid__mfl.Startup = (yvid__MFStartup_fn)(void (*)(void))YVID__SYM(yvid__mfl.plat, "MFStartup");
        yvid__mfl.Shutdown = (yvid__MFShutdown_fn)(void (*)(void))YVID__SYM(yvid__mfl.plat, "MFShutdown");
        yvid__mfl.CreateAttributes = (yvid__MFCreateAttributes_fn)(void (*)(void))YVID__SYM(yvid__mfl.plat, "MFCreateAttributes");
        yvid__mfl.CreateMediaType = (yvid__MFCreateMediaType_fn)(void (*)(void))YVID__SYM(yvid__mfl.plat, "MFCreateMediaType");
        yvid__mfl.ByteStreamOnStream = (yvid__MFCreateMFByteStreamOnStream_fn)(void (*)(void))YVID__SYM(yvid__mfl.plat, "MFCreateMFByteStreamOnStream");
        yvid__mfl.CreateDXGIDeviceManager = (yvid__MFCreateDXGIDeviceManager_fn)(void (*)(void))YVID__SYM(yvid__mfl.plat, "MFCreateDXGIDeviceManager");
        yvid__mfl.ReaderFromByteStream = (yvid__MFCreateSourceReaderFromByteStream_fn)(void (*)(void))YVID__SYM(yvid__mfl.rw, "MFCreateSourceReaderFromByteStream");
        yvid__mfl.IncMTA = (yvid__CoIncrementMTAUsage_fn)(void (*)(void))YVID__SYM(yvid__mfl.ole, "CoIncrementMTAUsage");
        yvid__mfl.DecMTA = (yvid__CoDecrementMTAUsage_fn)(void (*)(void))YVID__SYM(yvid__mfl.ole, "CoDecrementMTAUsage");
        yvid__mfl.PropVariantClear = (yvid__PropVariantClear_fn)(void (*)(void))YVID__SYM(yvid__mfl.ole, "PropVariantClear");
        if (!yvid__mfl.Startup || !yvid__mfl.Shutdown || !yvid__mfl.CreateAttributes || !yvid__mfl.CreateMediaType ||
            !yvid__mfl.ByteStreamOnStream || !yvid__mfl.ReaderFromByteStream || !yvid__mfl.IncMTA ||
            !yvid__mfl.DecMTA || !yvid__mfl.PropVariantClear) {
            yvid__fmt(err, cap, "Media Foundation is too old here (Windows 8 or later is needed)");
            return YVID_ERR_NOT_IMPLEMENTED;
        }
        yvid__mfl.loaded = 1;
    }
    hr = yvid__mfl.Startup(0x00020070 /* MF_VERSION */, 1 /* MFSTARTUP_LITE: no sockets */);
    if (FAILED(hr)) { yvid__fmt(err, cap, "MFStartup failed (0x%08lx)", (unsigned long)hr); return YVID_ERR_DECODER; }
    yvid__mfl.refs = 1;
    return YVID_OK;
}

static void yvid__mf_unload(void) {
    if (yvid__mfl.refs <= 0) return;
    if (--yvid__mfl.refs == 0) yvid__mfl.Shutdown();
}

/* --- an IStream over the header's byte source ---------------------------------
 * One path for a file (64-bit offsets), memory and a yvid_reader (a pack
 * entry read by range). Media Foundation calls it from its own work-queue
 * threads; the lock keeps one call at a time. It is the header's own table in
 * the COM layout, so the same code builds as C and C++. */
typedef struct yvid__stm yvid__stm;
typedef struct yvid__stm_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(yvid__stm*, const IID*, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(yvid__stm*);
    ULONG   (STDMETHODCALLTYPE *Release)(yvid__stm*);
    HRESULT (STDMETHODCALLTYPE *Read)(yvid__stm*, void*, ULONG, ULONG*);
    HRESULT (STDMETHODCALLTYPE *Write)(yvid__stm*, const void*, ULONG, ULONG*);
    HRESULT (STDMETHODCALLTYPE *Seek)(yvid__stm*, LARGE_INTEGER, DWORD, ULARGE_INTEGER*);
    HRESULT (STDMETHODCALLTYPE *SetSize)(yvid__stm*, ULARGE_INTEGER);
    HRESULT (STDMETHODCALLTYPE *CopyTo)(yvid__stm*, void*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*);
    HRESULT (STDMETHODCALLTYPE *Commit)(yvid__stm*, DWORD);
    HRESULT (STDMETHODCALLTYPE *Revert)(yvid__stm*);
    HRESULT (STDMETHODCALLTYPE *LockRegion)(yvid__stm*, ULARGE_INTEGER, ULARGE_INTEGER, DWORD);
    HRESULT (STDMETHODCALLTYPE *UnlockRegion)(yvid__stm*, ULARGE_INTEGER, ULARGE_INTEGER, DWORD);
    HRESULT (STDMETHODCALLTYPE *Stat)(yvid__stm*, STATSTG*, DWORD);
    HRESULT (STDMETHODCALLTYPE *Clone)(yvid__stm*, void**);
} yvid__stm_vtbl;

struct yvid__stm {
    const yvid__stm_vtbl* vt;
    volatile LONG refs;
    SRWLOCK lock;
    yvid__src src;
    int64_t pos;
};

static HRESULT STDMETHODCALLTYPE yvid__stm_qi(yvid__stm* s, const IID* iid, void** out) {
    if (!out) return E_POINTER;
    if (yvid__guid_eq(iid, &yvid__IID_IUnknown) || yvid__guid_eq(iid, &yvid__IID_IStream) ||
        yvid__guid_eq(iid, &yvid__IID_ISequentialStream)) {
        *out = s;
        InterlockedIncrement(&s->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE yvid__stm_addref(yvid__stm* s) { return (ULONG)InterlockedIncrement(&s->refs); }
static ULONG STDMETHODCALLTYPE yvid__stm_release(yvid__stm* s) {
    LONG n = InterlockedDecrement(&s->refs);
    if (n == 0) { yvid__src_close(&s->src); yvid__free(s); }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_read(yvid__stm* s, void* buf, ULONG n, ULONG* got) {
    int64_t k;
    HRESULT hr = S_OK;
    AcquireSRWLockExclusive(&s->lock);
    k = s->src.size - s->pos;
    if (k < 0) k = 0;
    if (k > (int64_t)n) k = (int64_t)n;
    if (k > 0 && yvid__src_read(&s->src, s->pos, buf, k) != YVID_OK) { k = 0; hr = STG_E_READFAULT; }
    s->pos += k;
    ReleaseSRWLockExclusive(&s->lock);
    if (got) *got = (ULONG)k;
    if (hr == S_OK && k < (int64_t)n) hr = S_FALSE;
    return hr;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_write(yvid__stm* s, const void* b, ULONG n, ULONG* w) {
    (void)s; (void)b; (void)n; if (w) *w = 0; return STG_E_ACCESSDENIED;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_seek(yvid__stm* s, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* np) {
    int64_t p;
    AcquireSRWLockExclusive(&s->lock);
    p = origin == 0 ? move.QuadPart : origin == 1 ? s->pos + move.QuadPart : s->src.size + move.QuadPart;
    if (p < 0) { ReleaseSRWLockExclusive(&s->lock); return STG_E_INVALIDFUNCTION; }
    s->pos = p;
    ReleaseSRWLockExclusive(&s->lock);
    if (np) np->QuadPart = (ULONGLONG)p;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_setsize(yvid__stm* s, ULARGE_INTEGER n) { (void)s; (void)n; return STG_E_ACCESSDENIED; }
static HRESULT STDMETHODCALLTYPE yvid__stm_copyto(yvid__stm* s, void* d, ULARGE_INTEGER n, ULARGE_INTEGER* r, ULARGE_INTEGER* w) {
    (void)s; (void)d; (void)n; (void)r; (void)w; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_commit(yvid__stm* s, DWORD f) { (void)s; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE yvid__stm_revert(yvid__stm* s) { (void)s; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE yvid__stm_lockr(yvid__stm* s, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t) {
    (void)s; (void)o; (void)n; (void)t; return STG_E_INVALIDFUNCTION;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_stat(yvid__stm* s, STATSTG* st, DWORD flag) {
    (void)flag;
    if (!st) return E_POINTER;
    memset(st, 0, sizeof *st);
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = (ULONGLONG)s->src.size;
    st->grfMode = STGM_READ;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE yvid__stm_clone(yvid__stm* s, void** out) { (void)s; if (out) *out = NULL; return E_NOTIMPL; }

static const yvid__stm_vtbl yvid__stm_table = {
    yvid__stm_qi, yvid__stm_addref, yvid__stm_release, yvid__stm_read, yvid__stm_write,
    yvid__stm_seek, yvid__stm_setsize, yvid__stm_copyto, yvid__stm_commit, yvid__stm_revert,
    yvid__stm_lockr, yvid__stm_lockr, yvid__stm_stat, yvid__stm_clone
};

/* --- the backend -------------------------------------------------------------- */

#if defined(__cplusplus) && !defined(CINTERFACE)
    #define YVID__CALL(o, m, ...) ((o)->m(__VA_ARGS__))
    #define YVID__CALL0(o, m)     ((o)->m())
    #define YVID__IID(x)          (x)
    #define YVID__REF(x)          (x)
#else
    #define YVID__CALL(o, m, ...) ((o)->lpVtbl->m((o), __VA_ARGS__))
    #define YVID__CALL0(o, m)     ((o)->lpVtbl->m(o))
    #define YVID__IID(x)          (&(x))
    #define YVID__REF(x)          (&(x))
#endif
#define YVID__REL(o) do { if (o) { YVID__CALL0((o), Release); (o) = NULL; } } while (0)

/* --- the H.264 SPS's VUI ---------------------------------------------------------
 * Media Foundation reports none of the color the stream states, so the
 * index maker reads it from the sequence parameter set: colour_primaries,
 * transfer_characteristics, matrix_coefficients, video_full_range_flag and
 * the chroma sample location (H.264 Annex E). HEVC's SPS puts its VUI
 * behind the profile_tier_level and short-term reference picture sets, so it
 * is not parsed here: an HEVC index takes the color from its desc. */
typedef struct yvid__bits { const uint8_t* p; size_t n, pos; int err; } yvid__bits;

static uint32_t yvid__bu(yvid__bits* b, int k) {
    uint32_t v = 0;
    while (k-- > 0) {
        if (b->pos >= b->n * 8) { b->err = 1; return 0; }
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u);
        b->pos++;
    }
    return v;
}
static uint32_t yvid__bue(yvid__bits* b) {   /* ue(v) */
    int z = 0;
    while (!b->err && yvid__bu(b, 1) == 0) if (++z > 31) { b->err = 1; return 0; }
    return z ? ((1u << z) - 1u) + yvid__bu(b, z) : 0u;
}
static int32_t yvid__bse(yvid__bits* b) {    /* se(v) */
    uint32_t k = yvid__bue(b);
    return (k & 1u) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

/* out: matrix, range, transfer, primaries, siting as YVID_* values; -1
 * not stated, -2 stated but not one the canonical form has. Returns 1 when
 * an SPS was parsed. */
static int yvid__h264_vui(const uint8_t* nal, size_t n, int16_t out[5]) {
    uint8_t rb[512];
    size_t i, m = 0;
    int zeros = 0;
    yvid__bits b;
    uint32_t profile, k;
    for (k = 0; k < 5; k++) out[k] = -1;
    /* the RBSP: drop the emulation prevention bytes (00 00 03) */
    for (i = 1; i < n && m < sizeof rb; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        rb[m++] = nal[i];
    }
    b.p = rb; b.n = m; b.pos = 0; b.err = 0;
    profile = yvid__bu(&b, 8);
    yvid__bu(&b, 16);                 /* constraint flags, level            */
    yvid__bue(&b);                    /* seq_parameter_set_id               */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        uint32_t cf = yvid__bue(&b);
        if (cf == 3) yvid__bu(&b, 1);
        yvid__bue(&b); yvid__bue(&b);   /* bit depths                     */
        yvid__bu(&b, 1);
        if (yvid__bu(&b, 1)) {            /* scaling matrices               */
            for (k = 0; k < (cf != 3 ? 8u : 12u) && !b.err; k++) {
                if (yvid__bu(&b, 1)) {
                    int size = k < 6 ? 16 : 64, j, last = 8, next = 8;
                    for (j = 0; j < size && !b.err; j++) {
                        if (next != 0) next = (last + yvid__bse(&b) + 256) % 256;
                        last = next == 0 ? last : next;
                    }
                }
            }
        }
    }
    yvid__bue(&b);                    /* log2_max_frame_num_minus4          */
    k = yvid__bue(&b);                /* pic_order_cnt_type                 */
    if (k == 0) yvid__bue(&b);
    else if (k == 1) {
        uint32_t c, j;
        yvid__bu(&b, 1); yvid__bse(&b); yvid__bse(&b);
        c = yvid__bue(&b);
        for (j = 0; j < c && j < 256 && !b.err; j++) yvid__bse(&b);
    }
    yvid__bue(&b); yvid__bu(&b, 1);   /* max_num_ref_frames, gaps         */
    yvid__bue(&b); yvid__bue(&b);     /* size in macroblocks              */
    if (!yvid__bu(&b, 1)) yvid__bu(&b, 1);   /* frame_mbs_only, mb_adaptive */
    yvid__bu(&b, 1);                    /* direct_8x8_inference             */
    if (yvid__bu(&b, 1)) { yvid__bue(&b); yvid__bue(&b); yvid__bue(&b); yvid__bue(&b); }   /* cropping */
    if (b.err) return 0;
    if (!yvid__bu(&b, 1)) { out[4] = YVID_SITING_LEFT; return 1; }   /* no VUI: type 0 inferred */
    if (yvid__bu(&b, 1) && yvid__bu(&b, 8) == 255) yvid__bu(&b, 32);   /* aspect ratio */
    if (yvid__bu(&b, 1)) yvid__bu(&b, 1);   /* overscan                     */
    if (yvid__bu(&b, 1)) {              /* video_signal_type                */
        uint32_t prim, trc, mat;
        yvid__bu(&b, 3);
        out[1] = yvid__bu(&b, 1) ? YVID_RANGE_FULL : YVID_RANGE_LIMITED;
        if (yvid__bu(&b, 1)) {
            prim = yvid__bu(&b, 8); trc = yvid__bu(&b, 8); mat = yvid__bu(&b, 8);
            out[3] = prim == 1 ? YVID_PRIM_BT709 : prim == 5 ? YVID_PRIM_BT601_625 : (prim == 6 || prim == 7) ? YVID_PRIM_BT601_525 :
                     prim == 9 ? YVID_PRIM_BT2020 : prim == 2 ? -1 : -2;
            out[2] = (trc == 1 || trc == 6 || trc == 14 || trc == 15) ? YVID_TRC_BT1886 : trc == 13 ? YVID_TRC_SRGB :
                     trc == 8 ? YVID_TRC_LINEAR : trc == 4 ? YVID_TRC_GAMMA22 : trc == 2 ? -1 : -2;
            out[0] = mat == 1 ? YVID_MATRIX_BT709 : (mat == 5 || mat == 6) ? YVID_MATRIX_BT601 : mat == 9 ? YVID_MATRIX_BT2020 :
                     mat == 2 ? -1 : -2;
        }
    }
    if (yvid__bu(&b, 1)) {              /* chroma_loc_info                  */
        uint32_t top = yvid__bue(&b);
        yvid__bue(&b);
        out[4] = top == 0 ? YVID_SITING_LEFT : top == 1 ? YVID_SITING_CENTER : -2;
    } else {
        /* absent, the standard infers type 0 (E.2.1): x264 omits it then */
        out[4] = YVID_SITING_LEFT;
    }
    if (b.err) { for (k = 0; k < 5; k++) out[k] = -1; return 0; }
    return 1;
}

/* The SPS in an avcC record or in Annex B NAL units (MF_MT_MPEG_SEQUENCE_HEADER
 * is one or the other). */
static int yvid__h264_seqhdr(const uint8_t* p, size_t n, int16_t out[5]) {
    size_t i;
    if (n > 8 && p[0] == 1) {   /* avcC: version 1, then the first SPS's length */
        size_t len = (size_t)p[6] << 8 | p[7];
        if ((p[5] & 0x1f) > 0 && 8 + len <= n && (p[8] & 0x1f) == 7) return yvid__h264_vui(p + 8, len, out);
        return 0;
    }
    for (i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 7) {
            size_t j = i + 3;
            while (j + 2 < n && !(p[j] == 0 && p[j + 1] == 0 && (p[j + 2] == 1 || p[j + 2] == 0))) j++;
            if (j + 2 >= n) j = n;
            return yvid__h264_vui(p + i + 3, j - (i + 3), out);
        }
    }
    return 0;
}

/* MFVideoArea, as the SDK lays it out (checked in video_com.cpp). */
typedef struct yvid__mfarea { WORD fx; short x; WORD fy; short y; LONG cx, cy; } yvid__mfarea;

typedef struct yvid__mf {
    IMFSourceReader*      rd;
    IMFSample*            cur;           /* the frame lent to the core          */
    IMFMediaBuffer*       buf;
    IMF2DBuffer*          b2d;
    IMFSample*            pending;       /* the keyframe a seek read            */
    int64_t               pending_ts;
    ID3D11Device*         dev;
    IMFDXGIDeviceManager* dm;
    void*                 mta;           /* CO_MTA_USAGE_COOKIE                 */
    int                   loaded;
    int                   hw;            /* YVID_HW_*, resolved               */
    int                   dxgi_out;      /* the decoder outputs D3D11 textures  */
    int32_t               w, h, fw, fh, ax, ay, stride;
    int32_t               num, den;
    int32_t               codec;
    int64_t               duration;      /* 100 ns, from the source             */
    HRESULT               last_hr;
    int                   native;        /* compressed samples: the index's first pass */
    int                   interlaced;    /* the last sample is interlaced        */
    int                   vui_found;     /* an H.264 SPS was parsed              */
    int                   gpu;           /* YVID_PATH_GPU: frames copied on the GPU */
    void*                 gpu_dst;       /* the core's texture for the next frame */
    ID3D11DeviceContext*  ctx;           /* the screen's immediate context (GPU) */
    int32_t               tw, th;        /* the decoder's surface size           */
    int                   req_gpu;
    int                   req_auto;
    int16_t               vui[5];        /* its matrix, range, transfer, primaries, siting */
    char                  name[96];
    /* set by yvid_open() before open(): the movie's choice and the screen */
    int                   req_hw;
    int32_t               req_num, req_den;   /* the index's rate        */
    yscr_native_info    req_nat;
} yvid__mf;

static void yvid__mf_unlock(yvid__mf* m) {
    if (m->b2d) { YVID__CALL0(m->b2d, Unlock2D); YVID__REL(m->b2d); }
    else if (m->buf) YVID__CALL0(m->buf, Unlock);
    YVID__REL(m->buf);
    YVID__REL(m->cur);
}

static int64_t yvid__mf_index_of(const yvid__mf* m, int64_t ts) {
    /* round(ts * num / (den * 1e7)), split so a long stream cannot overflow */
    int64_t d = (int64_t)m->den * 10000000, a = yvid__floordiv(ts, d), b = ts - a * d;
    return a * m->num + (b * m->num + d / 2) / d;
}

static int16_t yvid__mf_matrix(UINT32 v) {
    switch (v) { case 1: return YVID_MATRIX_BT709; case 2: return YVID_MATRIX_BT601; case 4: return YVID_MATRIX_BT2020; default: return -1; }
}
static int16_t yvid__mf_range(UINT32 v) {
    switch (v) { case 1: return YVID_RANGE_FULL; case 2: return YVID_RANGE_LIMITED; default: return -1; }
}
static int16_t yvid__mf_trc(UINT32 v) {
    switch (v) {
    case 1: return YVID_TRC_LINEAR;
    case 4: return YVID_TRC_GAMMA22;
    case 5: case 13: return YVID_TRC_BT1886;   /* BT.709 / BT.2020 OETF: shown by BT.1886 */
    case 7: return YVID_TRC_SRGB;
    case 15: case 16: return -2;                  /* PQ, HLG: HDR, refused   */
    default: return -1;
    }
}
static int16_t yvid__mf_prim(UINT32 v) {
    switch (v) { case 2: return YVID_PRIM_BT709; case 4: return YVID_PRIM_BT601_625; case 5: case 6: return YVID_PRIM_BT601_525;
                 case 9: return YVID_PRIM_BT2020; default: return -1; }
}
static int16_t yvid__mf_siting(UINT32 v) {
    switch (v) { case 0: return -1; case 5: return YVID_SITING_LEFT; case 1: return YVID_SITING_CENTER; default: return -2; }
}

static UINT32 yvid__mf_u32(IMFMediaType* t, const GUID* key, UINT32 def) {
    UINT32 v = def;
    if (FAILED(YVID__CALL(t, GetUINT32, YVID__IID(*key), &v))) v = def;
    return v;
}

/* A D3D11 device for DXVA on the adapter the screen uses (its LUID), or the
 * default adapter. Its own device: the screen's has no VIDEO_SUPPORT. */
static int yvid__mf_device(yvid__mf* m, const yscr_native_info* nat, char* err, size_t cap) {
    IDXGIFactory1* fac = NULL;
    IDXGIAdapter1* pick = NULL;
    ID3D10Multithread* mt = NULL;
    UINT i, token = 0;
    HRESULT hr;
    static const D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!yvid__mfl.d3d) yvid__mfl.d3d = LoadLibraryW(L"d3d11.dll");
    if (!yvid__mfl.dxgi) yvid__mfl.dxgi = LoadLibraryW(L"dxgi.dll");
    if (yvid__mfl.d3d && !yvid__mfl.D3D11CreateDevice)
        yvid__mfl.D3D11CreateDevice = (yvid__D3D11CreateDevice_fn)(void (*)(void))YVID__SYM(yvid__mfl.d3d, "D3D11CreateDevice");
    if (yvid__mfl.dxgi && !yvid__mfl.CreateDXGIFactory1)
        yvid__mfl.CreateDXGIFactory1 = (yvid__CreateDXGIFactory1_fn)(void (*)(void))YVID__SYM(yvid__mfl.dxgi, "CreateDXGIFactory1");
    if (!yvid__mfl.D3D11CreateDevice || !yvid__mfl.CreateDXGIFactory1 || !yvid__mfl.CreateDXGIDeviceManager) {
        yvid__fmt(err, cap, "DXVA needs d3d11.dll, dxgi.dll and MFCreateDXGIDeviceManager");
        return YVID_ERR_NOT_IMPLEMENTED;
    }
    if (nat && (nat->luid_low || nat->luid_high) &&
        SUCCEEDED(yvid__mfl.CreateDXGIFactory1(YVID__IID(yvid__IID_IDXGIFactory1), (void**)&fac))) {
        for (i = 0; !pick; i++) {
            IDXGIAdapter1* ad = NULL;
            DXGI_ADAPTER_DESC1 dd;
            if (YVID__CALL(fac, EnumAdapters1, i, &ad) != S_OK) break;
            YVID__CALL(ad, GetDesc1, &dd);
            if (dd.AdapterLuid.LowPart == nat->luid_low && dd.AdapterLuid.HighPart == nat->luid_high) pick = ad;
            else YVID__REL(ad);
        }
        YVID__REL(fac);
    }
    hr = yvid__mfl.D3D11CreateDevice((IDXGIAdapter*)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       lv, 2, D3D11_SDK_VERSION, &m->dev, NULL, NULL);
    YVID__REL(pick);
    if (FAILED(hr)) { yvid__fmt(err, cap, "D3D11CreateDevice with VIDEO_SUPPORT failed (0x%08lx)", (unsigned long)hr); return YVID_ERR_DECODER; }
    /* MF's decoder and the source reader use the device from more than one thread */
    if (SUCCEEDED(YVID__CALL(m->dev, QueryInterface, YVID__IID(yvid__IID_ID3D10Multithread), (void**)&mt))) {
        YVID__CALL(mt, SetMultithreadProtected, TRUE);
        YVID__REL(mt);
    }
    hr = yvid__mfl.CreateDXGIDeviceManager(&token, &m->dm);
    if (SUCCEEDED(hr)) hr = YVID__CALL(m->dm, ResetDevice, (IUnknown*)m->dev, token);
    if (FAILED(hr)) { yvid__fmt(err, cap, "the DXGI device manager failed (0x%08lx)", (unsigned long)hr); return YVID_ERR_DECODER; }
    return YVID_OK;
}

static void yvid__mf_close(void* ctx) {
    yvid__mf* m = (yvid__mf*)ctx;
    yvid__mf_unlock(m);
    YVID__REL(m->pending);
    YVID__REL(m->rd);
    YVID__REL(m->dm);
    YVID__REL(m->ctx);
    YVID__REL(m->dev);
    if (m->mta) { yvid__mfl.DecMTA(m->mta); m->mta = NULL; }
    if (m->loaded) { yvid__mf_unload(); m->loaded = 0; }
}

/* Opens the reader. native: the compressed samples, no decoder (the index
 * maker's first pass). */
static int yvid__mf_open_ex(yvid__mf* m, const yvid_decoder_open* in, yvid_stream* out, int hw,
                              const yscr_native_info* nat, int native, int32_t num, int32_t den, char* err, size_t cap) {
    IMFAttributes* at = NULL;
    IMFByteStream* bs = NULL;
    IMFAttributes* bsa = NULL;
    IMFMediaType* nt = NULL;
    IMFMediaType* ot = NULL;
    yvid__stm* s;
    GUID sub;
    UINT64 v64 = 0;
    UINT32 v, prof, nvideo = 0, k;
    HRESULT hr;
    int rc;
    PROPVARIANT pv;
    memset(m, 0, sizeof *m);
    m->gpu = (hw >> 8) & 1;
    hw &= 0xff;
    m->hw = hw;
    m->native = native;
    m->req_num = num; m->req_den = den;
    rc = yvid__mf_load(err, cap);
    if (rc < 0) return rc;
    m->loaded = 1;
    if (FAILED(yvid__mfl.IncMTA(&m->mta))) m->mta = NULL;
    /* the byte source */
    s = (yvid__stm*)yvid__malloc(sizeof *s);
    if (!s) { yvid__fmt(err, cap, "out of memory"); return YVID_ERR_FULL; }
    memset(s, 0, sizeof *s);
    s->vt = &yvid__stm_table;
    s->refs = 1;
    InitializeSRWLock(&s->lock);
    rc = yvid__src_open(&s->src, in->path, in->data, in->size, in->reader, in->reader_ctx);
    if (rc < 0) { yvid__free(s); yvid__fmt(err, cap, "cannot open %s", in->path ? in->path : "the source"); return rc; }
    hr = yvid__mfl.ByteStreamOnStream((IStream*)(void*)s, &bs);
    yvid__stm_release(s);   /* the byte stream holds its own reference */
    if (FAILED(hr)) { yvid__fmt(err, cap, "MFCreateMFByteStreamOnStream failed (0x%08lx)", (unsigned long)hr); return YVID_ERR_DECODER; }
    /* the source resolver picks the MP4 source from the content type: the
     * stream has no file name to go by */
    if (SUCCEEDED(YVID__CALL(bs, QueryInterface, YVID__IID(yvid__IID_IMFAttributes), (void**)&bsa))) {
        YVID__CALL(bsa, SetString, YVID__IID(yvid__MF_BYTESTREAM_CONTENT_TYPE), L"video/mp4");
        YVID__REL(bsa);
    }
    hr = yvid__mfl.CreateAttributes(&at, 4);
    if (FAILED(hr)) { YVID__REL(bs); yvid__fmt(err, cap, "MFCreateAttributes failed"); return YVID_ERR_DECODER; }
    /* MF refuses DISABLE_CONVERTERS together with hardware transforms
     * (E_INVALIDARG, measured), so with hardware the chain is checked after
     * the type is set: the decoder and nothing else. MF_LOW_LATENCY is not
     * set: with it the Microsoft H.264 decoder stamped frame 1 with frame
     * 0's time and then accumulated 100 ns roundings (measured). */
    if (hw == YVID_HW_OFF || native) YVID__CALL(at, SetUINT32, YVID__IID(yvid__MF_READWRITE_DISABLE_CONVERTERS), TRUE);
    else YVID__CALL(at, SetUINT32, YVID__IID(yvid__MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS), TRUE);
    if (hw == YVID_HW_DXVA && !native && m->gpu) {
        /* the screen's own device (made with VIDEO_SUPPORT and protection),
         * and surfaces the GPU can copy from */
        UINT token = 0;
        HRESULT hr2;
        m->dev = nat ? (ID3D11Device*)nat->d3d11_device : NULL;
        if (!m->dev || !yvid__mfl.CreateDXGIDeviceManager) { yvid__fmt(err, cap, "the GPU path needs the screen's D3D11 device"); YVID__REL(at); YVID__REL(bs); return YVID_ERR_REFUSED; }
        YVID__CALL0(m->dev, AddRef);
        YVID__CALL(m->dev, GetImmediateContext, &m->ctx);
        hr2 = yvid__mfl.CreateDXGIDeviceManager(&token, &m->dm);
        if (SUCCEEDED(hr2)) hr2 = YVID__CALL(m->dm, ResetDevice, (IUnknown*)m->dev, token);
        if (FAILED(hr2)) { yvid__fmt(err, cap, "the DXGI device manager refused the screen's device (0x%08lx)", (unsigned long)hr2); YVID__REL(at); YVID__REL(bs); return YVID_ERR_DECODER; }
        YVID__CALL(at, SetUnknown, YVID__IID(yvid__MF_SOURCE_READER_D3D_MANAGER), (IUnknown*)m->dm);
    } else if (hw == YVID_HW_DXVA && !native) {
        rc = yvid__mf_device(m, nat, err, cap);
        if (rc < 0) { YVID__REL(at); YVID__REL(bs); return rc; }
        YVID__CALL(at, SetUnknown, YVID__IID(yvid__MF_SOURCE_READER_D3D_MANAGER), (IUnknown*)m->dm);
    }
    hr = yvid__mfl.ReaderFromByteStream(bs, at, &m->rd);
    YVID__REL(at);
    YVID__REL(bs);
    if (FAILED(hr)) { yvid__fmt(err, cap, "Media Foundation cannot read it (0x%08lx): MP4 only; remux with -f mp4", (unsigned long)hr); return YVID_ERR_FORMAT; }
    /* exactly one video stream, and only it selected */
    for (k = 0; k < 64; k++) {
        IMFMediaType* t = NULL;
        GUID major;
        if (FAILED(YVID__CALL(m->rd, GetNativeMediaType, k, 0, &t))) break;
        if (SUCCEEDED(YVID__CALL(t, GetGUID, YVID__IID(yvid__MF_MT_MAJOR_TYPE), &major)) &&
            yvid__guid_eq(&(major), &(yvid__MFMediaType_Video))) nvideo++;
        YVID__REL(t);
    }
    if (nvideo != 1) { yvid__fmt(err, cap, "%u video streams; the canonical form has one", (unsigned)nvideo); return YVID_ERR_FORMAT; }
    YVID__CALL(m->rd, SetStreamSelection, YVID__MF_ALL_STREAMS, FALSE);
    YVID__CALL(m->rd, SetStreamSelection, YVID__MF_FIRST_VIDEO, TRUE);
    hr = YVID__CALL(m->rd, GetNativeMediaType, YVID__MF_FIRST_VIDEO, 0, &nt);
    if (FAILED(hr)) { yvid__fmt(err, cap, "no video stream"); return YVID_ERR_FORMAT; }
    YVID__CALL(nt, GetGUID, YVID__IID(yvid__MF_MT_SUBTYPE), &sub);
    prof = yvid__mf_u32(nt, &yvid__MF_MT_MPEG2_PROFILE, 0);
    if (yvid__guid_eq(&(sub), &(yvid__MFVideoFormat_H264))) {
        m->codec = YVID_CODEC_H264;
        if (prof == 110 || prof == 122 || prof == 244) {
            YVID__REL(nt);
            yvid__fmt(err, cap, "H.264 profile %s: the canonical form is 8-bit 4:2:0 (High, Main or Baseline); re-encode with -pix_fmt yuv420p",
                        prof == 110 ? "High 10" : prof == 122 ? "High 4:2:2" : "High 4:4:4");
            return YVID_ERR_FORMAT;
        }
    } else if (yvid__guid_eq(&(sub), &(yvid__MFVideoFormat_HEVC))) {
        m->codec = YVID_CODEC_HEVC;
        if (prof != 0 && prof != 1) {
            YVID__REL(nt);
            yvid__fmt(err, cap, "HEVC profile %u: the canonical form is Main (8-bit 4:2:0)", (unsigned)prof);
            return YVID_ERR_FORMAT;
        }
    } else {
        char cc[5];
        memcpy(cc, &sub.Data1, 4); cc[4] = 0;
        for (k = 0; k < 4; k++) if (cc[k] < 32 || cc[k] > 126) cc[k] = '?';
        YVID__REL(nt);
        yvid__fmt(err, cap, "the video codec is '%s', not H.264 or HEVC; re-encode with libx264", cc);
        return YVID_ERR_FORMAT;
    }
    if (m->codec == YVID_CODEC_H264) {
        UINT8 hdr[1024];
        UINT32 got = 0;
        if (SUCCEEDED(YVID__CALL(nt, GetBlob, YVID__IID(yvid__MF_MT_MPEG_SEQUENCE_HEADER), hdr, (UINT32)sizeof hdr, &got)))
            m->vui_found = yvid__h264_seqhdr(hdr, got, m->vui);
    }
    if (yvid__mf_u32(nt, &yvid__MF_MT_VIDEO_ROTATION, 0) != 0) {
        v = yvid__mf_u32(nt, &yvid__MF_MT_VIDEO_ROTATION, 0);
        YVID__REL(nt);
        yvid__fmt(err, cap, "the stream is rotated by %u degrees; no rotation is applied at play: re-encode the pixels upright (-autorotate) or remux with -display_rotation 0", (unsigned)v);
        return YVID_ERR_FORMAT;
    }
    if (native) {
        hr = YVID__CALL(m->rd, SetCurrentMediaType, YVID__MF_FIRST_VIDEO, NULL, nt);
        ot = nt; nt = NULL;
        if (FAILED(hr)) { YVID__REL(ot); yvid__fmt(err, cap, "cannot select the compressed stream"); return YVID_ERR_DECODER; }
    } else {
        YVID__REL(nt);
        hr = yvid__mfl.CreateMediaType(&ot);
        if (SUCCEEDED(hr)) {
            YVID__CALL(ot, SetGUID, YVID__IID(yvid__MF_MT_MAJOR_TYPE), YVID__IID(yvid__MFMediaType_Video));
            YVID__CALL(ot, SetGUID, YVID__IID(yvid__MF_MT_SUBTYPE), YVID__IID(yvid__MFVideoFormat_NV12));
            hr = YVID__CALL(m->rd, SetCurrentMediaType, YVID__MF_FIRST_VIDEO, NULL, ot);
        }
        YVID__REL(ot);
        if (FAILED(hr)) {
            yvid__fmt(err, cap, "%s: no decoder gives NV12 for this stream (0x%08lx)%s", m->codec == YVID_CODEC_HEVC ? "HEVC" : "H.264",
                        (unsigned long)hr, m->codec == YVID_CODEC_HEVC ? "; no HEVC decoder may be installed (HEVC Video Extensions): re-encode as H.264"
                                                                           : "; the canonical form is 8-bit 4:2:0");
            return YVID_ERR_FORMAT;
        }
        hr = YVID__CALL(m->rd, GetCurrentMediaType, YVID__MF_FIRST_VIDEO, &ot);
        if (FAILED(hr)) { yvid__fmt(err, cap, "no output type"); return YVID_ERR_DECODER; }
    }
    /* the description */
    if (FAILED(YVID__CALL(ot, GetUINT64, YVID__IID(yvid__MF_MT_FRAME_SIZE), &v64))) v64 = 0;
    m->fw = (int32_t)(v64 >> 32); m->fh = (int32_t)(v64 & 0xffffffffu);
    m->w = m->fw; m->h = m->fh;
    if (!native) {
        yvid__mfarea a;
        UINT32 got = 0;
        if (SUCCEEDED(YVID__CALL(ot, GetBlob, YVID__IID(yvid__MF_MT_MINIMUM_DISPLAY_APERTURE), (UINT8*)&a, (UINT32)sizeof a, &got)) &&
            got == sizeof a) {
            m->ax = a.x; m->ay = a.y; m->w = (int32_t)a.cx; m->h = (int32_t)a.cy;
        }
        if ((m->ax & 1) || (m->ay & 1) || m->ax < 0 || m->ay < 0 || m->ax + m->w > m->fw || m->ay + m->h > m->fh) {
            YVID__REL(ot);
            yvid__fmt(err, cap, "display aperture %d,%d %dx%d in a %dx%d frame", (int)m->ax, (int)m->ay, (int)m->w, (int)m->h, (int)m->fw, (int)m->fh);
            return YVID_ERR_FORMAT;
        }
        m->stride = (int32_t)yvid__mf_u32(ot, &yvid__MF_MT_DEFAULT_STRIDE, (UINT32)m->fw);
    }
    if (SUCCEEDED(YVID__CALL(ot, GetUINT64, YVID__IID(yvid__MF_MT_FRAME_RATE), &v64)) && (v64 >> 32) && (v64 & 0xffffffffu)) {
        int64_t a = (int64_t)(v64 >> 32), b = (int64_t)(v64 & 0xffffffffu), g0 = a, g1 = b;
        while (g1) { int64_t t = g0 % g1; g0 = g1; g1 = t; }
        m->num = (int32_t)(a / g0); m->den = (int32_t)(b / g0);
    }
    if (SUCCEEDED(YVID__CALL(ot, GetUINT64, YVID__IID(yvid__MF_MT_PIXEL_ASPECT_RATIO), &v64)) &&
        (v64 >> 32) != (v64 & 0xffffffffu)) {
        YVID__REL(ot);
        yvid__fmt(err, cap, "pixel aspect %u:%u; the canonical form has square pixels: -vf setsar=1 (after scaling to the shape you want)",
                    (unsigned)(v64 >> 32), (unsigned)(v64 & 0xffffffffu));
        return YVID_ERR_FORMAT;
    }
    /* Progressive, or "mixed": H.264 can switch per frame, so the decoder
     * says mixed and each sample says what it is (the index maker checks
     * every sample, MFSampleExtension_Interlaced). Field modes are refused. */
    v = yvid__mf_u32(ot, &yvid__MF_MT_INTERLACE_MODE, 2);
    if (v != 2 /* MFVideoInterlace_Progressive */ && v != 7 /* MixedInterlaceOrProgressive */) {
        YVID__REL(ot);
        yvid__fmt(err, cap, "interlaced (MF interlace mode %u); the canonical form is progressive", (unsigned)v);
        return YVID_ERR_FORMAT;
    }
    out->w = m->w; out->h = m->h;
    out->format = native ? 0 : YVID_FMT_NV12;
    /* MF derives the type's rate from 100 ns durations (23.976 comes out
     * 10000000/417083, measured), so the stream's rate is the index's, made
     * from the sample times; MF's must only agree to 100 ppm. */
    if (m->req_num > 0 && m->req_den > 0) {
        double a = (double)m->num / (m->den > 0 ? m->den : 1), b = (double)m->req_num / m->req_den;
        if (m->num > 0 && fabs(a / b - 1.0) > 1e-4) {
            yvid__fmt(err, cap, "the stream runs %.4f fps, the index says %d/%d; the index is stale: make it again", a, (int)m->req_num, (int)m->req_den);
            return YVID_ERR_FORMAT;
        }
        m->num = m->req_num; m->den = m->req_den;
    }
    out->fps_num = 0; out->fps_den = 0;
    out->frames = -1; out->gop = 0; out->codec = m->codec;
    out->matrix = yvid__mf_matrix(yvid__mf_u32(ot, &yvid__MF_MT_YUV_MATRIX, 0));
    out->range = yvid__mf_range(yvid__mf_u32(ot, &yvid__MF_MT_VIDEO_NOMINAL_RANGE, 0));
    out->transfer = yvid__mf_trc(yvid__mf_u32(ot, &yvid__MF_MT_TRANSFER_FUNCTION, 0));
    out->primaries = yvid__mf_prim(yvid__mf_u32(ot, &yvid__MF_MT_VIDEO_PRIMARIES, 0));
    out->siting = yvid__mf_siting(yvid__mf_u32(ot, &yvid__MF_MT_VIDEO_CHROMA_SITING, 0));
    YVID__REL(ot);
    if (out->transfer == -2) { yvid__fmt(err, cap, "an HDR transfer (PQ or HLG); the canonical form is SDR"); return YVID_ERR_FORMAT; }
    if (out->siting == -2) { yvid__fmt(err, cap, "chroma siting other than left or center; re-encode with -chroma_sample_location left"); return YVID_ERR_FORMAT; }
    out->timescale = 10000000;   /* MF sample times are 100 ns */
    out->caps = YVID_DEC_CPU;
    memset(&pv, 0, sizeof pv);
    if (SUCCEEDED(YVID__CALL(m->rd, GetPresentationAttribute, YVID__MF_MEDIASOURCE, YVID__IID(yvid__MF_PD_DURATION), &pv))) {
        /* a VT_UI8: the value sits after the 8-byte header in every layout */
        memcpy(&m->duration, (const unsigned char*)&pv + 8, 8);
        yvid__mfl.PropVariantClear(&pv);
    }
    /* which decoder MF chose, for the describe line */
    yvid__fmt(m->name, sizeof m->name, "%s", native ? "compressed" : "decoder");
    if (!native) {
        IMFSourceReaderEx* rx = NULL;
        if (SUCCEEDED(YVID__CALL(m->rd, QueryInterface, YVID__IID(yvid__IID_IMFSourceReaderEx), (void**)&rx))) {
            GUID cat;
            IMFTransform* tr = NULL;
            if (SUCCEEDED(YVID__CALL(rx, GetTransformForStream, YVID__MF_FIRST_VIDEO, 0, &cat, &tr)) && tr) {
                IMFAttributes* ta = NULL;
                GUID clsid;
                WCHAR wn[80];
                int named = 0, hwmft = 0;
                if (SUCCEEDED(YVID__CALL(tr, GetAttributes, &ta)) && ta) {
                    UINT32 len = 0;
                    if (SUCCEEDED(YVID__CALL(ta, GetString, YVID__IID(yvid__MFT_FRIENDLY_NAME_Attribute), wn, 80, &len))) {
                        WideCharToMultiByte(CP_UTF8, 0, wn, -1, m->name, (int)sizeof m->name - 1, NULL, NULL);
                        named = 1;
                    }
                    if (SUCCEEDED(YVID__CALL(ta, GetStringLength, YVID__IID(yvid__MFT_ENUM_HARDWARE_URL_Attribute), &len))) hwmft = 1;
                    if (!named && SUCCEEDED(YVID__CALL(ta, GetGUID, YVID__IID(yvid__MFT_TRANSFORM_CLSID_Attribute), &clsid))) {
                        if (yvid__guid_eq(&(clsid), &(yvid__CLSID_MSH264DecoderMFT))) { yvid__fmt(m->name, sizeof m->name, "Microsoft H264 Video Decoder MFT"); named = 1; }
                    }
                    YVID__REL(ta);
                }
                if (!named) yvid__fmt(m->name, sizeof m->name, "%s", m->codec == YVID_CODEC_HEVC ? "an HEVC decoder" : "an H.264 decoder");
                (void)hwmft;
                YVID__REL(tr);
            }
            {   /* principle 5: the decoder alone, no converter after it */
                IMFTransform* t2 = NULL;
                GUID cat2;
                if (SUCCEEDED(YVID__CALL(rx, GetTransformForStream, YVID__MF_FIRST_VIDEO, 1, &cat2, &t2)) && t2) {
                    YVID__REL(t2);
                    YVID__REL(rx);
                    yvid__fmt(err, cap, "Media Foundation put a converter after the decoder; the canonical form needs none (the decoder must give NV12 itself)");
                    return YVID_ERR_FORMAT;
                }
            }
            YVID__REL(rx);
        }
    }
    return YVID_OK;
}

static int yvid__mf_open(void* ctx, const yvid_decoder_open* in, yvid_stream* out, char* err, size_t cap) {
    yvid__mf* m = (yvid__mf*)ctx;
    int hw = m->req_hw | (m->req_gpu ? 0x100 : 0);
    int auto_hw = m->req_auto && !m->req_gpu;
    int32_t num = m->req_num, den = m->req_den;
    yscr_native_info nat = m->req_nat;
    int rc = yvid__mf_open_ex(m, in, out, hw, &nat, 0, num, den, err, cap);
    if (rc < 0 && auto_hw && rc != YVID_ERR_FORMAT && rc != YVID_ERR_NOT_FOUND) {
        /* AUTO: no DXVA device here (WARP, a remote session); the software
         * decoder gives the same bytes, and info.hw says which ran */
        yvid__mf_close(m);
        rc = yvid__mf_open_ex(m, in, out, YVID_HW_OFF, &nat, 0, num, den, err, cap);
    }
    return rc;
}

/* The current type after the decoder changed it (it learns the coded size
 * from the first frames): the visible size must not change. */
static int yvid__mf_retype(yvid__mf* m) {
    IMFMediaType* t = NULL;
    UINT64 v64 = 0;
    yvid__mfarea a;
    UINT32 got = 0;
    int32_t w, h, ax = 0, ay = 0;
    GUID sub;
    if (FAILED(YVID__CALL(m->rd, GetCurrentMediaType, YVID__MF_FIRST_VIDEO, &t))) return YVID_ERR_DECODER;
    if (FAILED(YVID__CALL(t, GetGUID, YVID__IID(yvid__MF_MT_SUBTYPE), &sub)) || !yvid__guid_eq(&(sub), &(yvid__MFVideoFormat_NV12))) {
        YVID__REL(t);
        return YVID_ERR_FORMAT;
    }
    if (FAILED(YVID__CALL(t, GetUINT64, YVID__IID(yvid__MF_MT_FRAME_SIZE), &v64))) v64 = 0;
    m->fw = (int32_t)(v64 >> 32); m->fh = (int32_t)(v64 & 0xffffffffu);
    w = m->fw; h = m->fh;
    if (SUCCEEDED(YVID__CALL(t, GetBlob, YVID__IID(yvid__MF_MT_MINIMUM_DISPLAY_APERTURE), (UINT8*)&a, (UINT32)sizeof a, &got)) &&
        got == sizeof a) { ax = a.x; ay = a.y; w = (int32_t)a.cx; h = (int32_t)a.cy; }
    m->stride = (int32_t)yvid__mf_u32(t, &yvid__MF_MT_DEFAULT_STRIDE, (UINT32)m->fw);
    YVID__REL(t);
    if (w != m->w || h != m->h || (ax & 1) || (ay & 1) || ax + w > m->fw || ay + h > m->fh) return YVID_ERR_FORMAT;
    m->ax = ax; m->ay = ay;
    return YVID_OK;
}

/* The next sample in presentation order, or ENDED, or an error. */
static int yvid__mf_read(yvid__mf* m, IMFSample** smp, int64_t* ts) {
    int guard;
    *smp = NULL;
    for (guard = 0; guard < 64; guard++) {
        DWORD idx = 0, flags = 0;
        LONGLONG t = 0;
        HRESULT hr = YVID__CALL(m->rd, ReadSample, YVID__MF_FIRST_VIDEO, 0, &idx, &flags, &t, smp);
        m->last_hr = hr;
        if (FAILED(hr) || (flags & 0x1u /* ERROR */)) { YVID__REL(*smp); return YVID_ERR_DECODER; }
        if (flags & 0x2u /* ENDOFSTREAM */) { YVID__REL(*smp); return YVID_ENDED; }
        if (flags & 0x100u /* STREAMTICK: a gap */) { YVID__REL(*smp); m->last_hr = E_UNEXPECTED; return YVID_ERR_DECODER; }
        if ((flags & 0x20u /* CURRENTMEDIATYPECHANGED */) && !m->native && yvid__mf_retype(m) < 0) {
            YVID__REL(*smp);
            m->last_hr = E_UNEXPECTED;
            return YVID_ERR_FORMAT;
        }
        if (*smp) { *ts = (int64_t)t; return YVID_OK; }
    }
    return YVID_ERR_DECODER;
}

static int yvid__mf_next(void* ctx, yvid_planes* dst, yvid_out* out) {
    yvid__mf* m = (yvid__mf*)ctx;
    IMFSample* smp = NULL;
    int64_t ts = 0, rows;
    BYTE* scan0 = NULL;
    LONG pitch = 0;
    UINT32 clean = 0;
    int rc;
    yvid__mf_unlock(m);
    if (m->pending) { smp = m->pending; ts = m->pending_ts; m->pending = NULL; }
    else if ((rc = yvid__mf_read(m, &smp, &ts)) != YVID_OK) return rc;
    out->pts = ts;
    out->index = -1;
    if (SUCCEEDED(YVID__CALL(smp, GetUINT32, YVID__IID(yvid__MFSampleExtension_CleanPoint), &clean)) && clean)
        out->flags |= YVID_OUT_KEYFRAME;
    {
        UINT32 il = 0;
        m->interlaced = SUCCEEDED(YVID__CALL(smp, GetUINT32, YVID__IID(yvid__MFSampleExtension_Interlaced), &il)) && il;
    }
    if (!dst) { YVID__REL(smp); return YVID_OK; }   /* decoded and discarded: never read back */
    if (m->gpu) {
        /* one GPU copy of the surface into the core's texture, then the
         * surface is the decoder's again: holding surfaces starves its pool
         * (a deadlock at 8 held, measured) */
        IMFMediaBuffer* b = NULL;
        IMFDXGIBuffer* xb = NULL;
        ID3D11Texture2D* tex = NULL;
        UINT sub = 0;
        HRESULT hr = YVID__CALL(smp, GetBufferByIndex, 0, &b);
        if (SUCCEEDED(hr)) hr = YVID__CALL(b, QueryInterface, YVID__IID(yvid__IID_IMFDXGIBuffer), (void**)&xb);
        if (SUCCEEDED(hr)) hr = YVID__CALL(xb, GetResource, YVID__IID(yvid__IID_ID3D11Texture2D), (void**)&tex);
        if (SUCCEEDED(hr)) hr = YVID__CALL(xb, GetSubresourceIndex, &sub);
        YVID__REL(xb);
        YVID__REL(b);
        if (SUCCEEDED(hr) && tex) {
            D3D11_TEXTURE2D_DESC td;
            YVID__CALL(tex, GetDesc, &td);
            m->tw = (int32_t)td.Width; m->th = (int32_t)td.Height;
            if (td.Format != DXGI_FORMAT_NV12) hr = E_UNEXPECTED;
            else if (m->gpu_dst) YVID__CALL(m->ctx, CopySubresourceRegion, (ID3D11Resource*)m->gpu_dst, 0, 0, 0, 0, (ID3D11Resource*)tex, sub, NULL);
        }
        YVID__REL(tex);
        YVID__REL(smp);
        if (FAILED(hr)) { m->last_hr = hr; return YVID_ERR_DECODER; }
        out->flags |= YVID__OUT_GPU;
        out->planes.w[0] = m->w; out->planes.h[0] = m->h;
        return YVID_OK;
    }
    m->cur = smp;
    if (FAILED(YVID__CALL(smp, GetBufferByIndex, 0, &m->buf))) { m->last_hr = E_UNEXPECTED; return YVID_ERR_DECODER; }
    {
        IUnknown* dx = NULL;
        if (SUCCEEDED(YVID__CALL(m->buf, QueryInterface, YVID__IID(yvid__IID_IMFDXGIBuffer), (void**)&dx))) { m->dxgi_out = 1; YVID__REL(dx); }
    }
    rows = m->fh;
    {
        IMF2DBuffer2* b2 = NULL;
        if (SUCCEEDED(YVID__CALL(m->buf, QueryInterface, YVID__IID(yvid__IID_IMF2DBuffer2), (void**)&b2))) {
            BYTE* start = NULL;
            DWORD len = 0;
            HRESULT hr = YVID__CALL(b2, Lock2DSize, MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len);
            if (FAILED(hr)) { YVID__REL(b2); m->last_hr = hr; return YVID_ERR_DECODER; }
            /* a surface taller than the frame size (a decoder's alignment)
             * shows in the buffer's length */
            if (pitch > 0) {
                int64_t r = (int64_t)len * 2 / (3 * (int64_t)pitch);
                if (r > rows && r * 3 * (int64_t)pitch == (int64_t)len * 2) rows = r;
            }
            m->b2d = (IMF2DBuffer*)(void*)b2;   /* IMF2DBuffer2 derives from it: Unlock2D is the same slot */
        } else if (SUCCEEDED(YVID__CALL(m->buf, QueryInterface, YVID__IID(yvid__IID_IMF2DBuffer), (void**)&m->b2d))) {
            HRESULT hr = YVID__CALL(m->b2d, Lock2D, &scan0, &pitch);
            if (FAILED(hr)) { YVID__REL(m->b2d); m->last_hr = hr; return YVID_ERR_DECODER; }
        } else {
            DWORD cur = 0;
            HRESULT hr = YVID__CALL(m->buf, Lock, &scan0, NULL, &cur);
            if (FAILED(hr)) { m->last_hr = hr; return YVID_ERR_DECODER; }
            pitch = m->stride > 0 ? m->stride : m->fw;
        }
    }
    if (pitch <= 0) { m->last_hr = E_UNEXPECTED; return YVID_ERR_FORMAT; }
    out->flags |= YVID_OUT_BORROWED;
    out->planes.data[0] = scan0 + (size_t)m->ay * (size_t)pitch + (size_t)m->ax;
    out->planes.stride[0] = (int32_t)pitch;
    out->planes.w[0] = m->w; out->planes.h[0] = m->h;
    /* the chroma plane starts after every row of the surface, visible or not */
    out->planes.data[1] = scan0 + (size_t)rows * (size_t)pitch + (size_t)(m->ay / 2) * (size_t)pitch + (size_t)m->ax;
    out->planes.stride[1] = (int32_t)pitch;
    out->planes.w[1] = (m->w + 1) / 2; out->planes.h[1] = (m->h + 1) / 2;
    return YVID_OK;
}

static int yvid__mf_seek(void* ctx, int64_t key, int64_t key_pts) {
    yvid__mf* m = (yvid__mf*)ctx;
    PROPVARIANT pv;
    int64_t t100, n;
    HRESULT hr;
    (void)key_pts;
    yvid__mf_unlock(m);
    YVID__REL(m->pending);
    if (m->num <= 0) return YVID_ERR_ARG;
    /* half a frame past the keyframe's time: the source takes the sync
     * sample at or before it, and 100 ns rounding cannot fall short */
    t100 = (yvid_frame_time(m->num, m->den, key) + yvid_frame_time(m->num, m->den, key + 1)) / 200;
    memset(&pv, 0, sizeof pv);
    {   /* VT_I8 in the first two bytes, the value after the 8-byte header:
         * the same in every PROPVARIANT layout, and no union names needed */
        VARTYPE vt = VT_I8;
        memcpy(&pv, &vt, sizeof vt);
        memcpy((unsigned char*)&pv + 8, &t100, 8);
    }
    hr = YVID__CALL(m->rd, SetCurrentPosition, YVID__IID(yvid__GUID_NULL), YVID__REF(pv));
    m->last_hr = hr;
    if (FAILED(hr)) return YVID_ERR_DECODER;
    /* forward to `key` itself, so next() returns it as the interface says */
    for (n = 0; n < 100000; n++) {
        IMFSample* smp = NULL;
        int64_t ts = 0, i;
        int rc = yvid__mf_read(m, &smp, &ts);
        if (rc != YVID_OK) return rc < 0 ? rc : YVID_ERR_DECODER;
        i = yvid__mf_index_of(m, ts);
        if (i == key) { m->pending = smp; m->pending_ts = ts; return YVID_OK; }
        YVID__REL(smp);
        if (i > key) { m->last_hr = E_UNEXPECTED; return YVID_ERR_DECODER; }
    }
    return YVID_ERR_DECODER;
}

static int yvid__mf_describe(void* ctx, char* buf, size_t cap) {
    const yvid__mf* m = (const yvid__mf*)ctx;
    static const char* const hw[] = { "auto", "software", "DXVA" };
    return snprintf(buf, cap, "Media Foundation (%s, %s%s)", m->name, hw[m->hw >= 0 && m->hw <= 2 ? m->hw : 0],
                    m->dxgi_out ? ", D3D11 surfaces read back" : "");
}

/* Whether the screen's device can carry a decoder from another thread:
 * made with VIDEO_SUPPORT, and multithread protection on. Read from the
 * device itself, so no screen option name is assumed. */
static int yvid__mf_video_device(void* devp) {
    ID3D11Device* dev = (ID3D11Device*)devp;
    ID3D10Multithread* mt = NULL;
    int ok = 0;
    if (!dev || !(YVID__CALL0(dev, GetCreationFlags) & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)) return 0;
    if (SUCCEEDED(YVID__CALL(dev, QueryInterface, YVID__IID(yvid__IID_ID3D10Multithread), (void**)&mt)) && mt) {
        ok = YVID__CALL0(mt, GetMultithreadProtected) ? 1 : 0;
        YVID__REL(mt);
    }
    return ok;
}

/* YVID_PATH_GPU at open: the first frame gives the surface size (the
 * movie's textures must have it to take a copy), then back to frame 0. */
static int yvid__mf_peek(yvid__mf* m) {
    yvid_out o;
    yvid_planes d;
    int rc;
    memset(&o, 0, sizeof o);
    m->gpu_dst = NULL;
    rc = yvid__mf_next(m, &d, &o);
    if (rc != YVID_OK) return rc;
    return yvid__mf_seek(m, 0, 0);
}

/* The movie's textures on the screen's device: NV12, the surface's size,
 * sampled by the GPU. */
static int yvid__mf_textures(yvid__mf* m, yvid_movie* mv) {
    D3D11_TEXTURE2D_DESC td;
    int k;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)m->tw; td.Height = (UINT)m->th;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (k = 0; k < mv->n_slots; k++) {
        ID3D11Texture2D* t = NULL;
        HRESULT hr = YVID__CALL(m->dev, CreateTexture2D, &td, NULL, &t);
        if (FAILED(hr)) { m->last_hr = hr; return YVID_ERR_FULL; }
        mv->slots[k].gtex = t;
    }
    return YVID_OK;
}

static const yvid_decoder yvid__mf_decoder = {
    YVID_DECODER_VERSION, "Media Foundation", yvid__mf_open, yvid__mf_next, yvid__mf_seek,
    yvid__mf_close, yvid__mf_describe
};

#endif /* YVID__MF */

/* --- making the index ------------------------------------------------------------ */

/* Writes index_path (NULL = media_path + ".yspvi"): the header from c and the
 * media file's size and ends, the frame hashes, the note. */
static int yvid__index_write(const char* media_path, const char* index_path, const yvid__canon* c,
                               const uint64_t* hashes, int64_t n, const char* note, char* err, size_t cap) {
    yvid__src s;
    uint64_t head = 0, tail = 0;
    uint8_t hdr[YVID__IDX_HDR];
    char ip[1024];
    int64_t size, i;
    FILE* f;
    int rc = yvid__src_open(&s, media_path, NULL, 0, NULL, NULL);
    if (rc == YVID_OK) rc = yvid__src_ends(&s, &head, &tail);
    size = s.size;
    yvid__src_close(&s);
    if (rc < 0) { yvid__fmt(err, cap, "%s: cannot read it again", media_path); return rc; }
    yvid__index_header(hdr, c, size, head, tail);
    if (!index_path) { yvid__fmt(ip, sizeof ip, "%s.yspvi", media_path); index_path = ip; }
    f = yvid__fopen(index_path, "wb");
    if (!f) { yvid__fmt(err, cap, "cannot create %s", index_path); return YVID_ERR_IO; }
    rc = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr ? YVID_OK : YVID_ERR_IO;
    for (i = 0; i < n && rc == YVID_OK; i++) {
        uint8_t b8[8];
        yvid__w64(b8, hashes[i]);
        if (fwrite(b8, 1, 8, f) != 8) rc = YVID_ERR_IO;
    }
    if (rc == YVID_OK && note) {
        size_t ln = strlen(note);
        if (fwrite(note, 1, ln, f) != ln) rc = YVID_ERR_IO;
    }
    if (fclose(f) != 0) rc = YVID_ERR_IO;
    if (rc < 0) yvid__fmt(err, cap, "cannot write %s", index_path);
    return rc;
}

/* Keyframe bookkeeping shared by both makers: the first frame is a
 * keyframe and every GOP but the last has one length. */
static int yvid__gop_step(const char* path, int64_t n, int key, int64_t* last_key, int64_t* gop, char* err, size_t cap) {
    if (key) {
        if (*last_key >= 0) {
            if (*gop == 0) *gop = n - *last_key;
            else if (n - *last_key != *gop) {
                yvid__fmt(err, cap, "%s: GOP length changes at frame %lld (%lld, was %lld); re-encode with a fixed -g and -keyint_min, -sc_threshold 0",
                            path, (long long)n, (long long)(n - *last_key), (long long)*gop);
                return YVID_ERR_FORMAT;
            }
        } else if (n != 0) {
            yvid__fmt(err, cap, "%s: the first frame is not a keyframe", path);
            return YVID_ERR_FORMAT;
        }
        *last_key = n;
    } else if (n == 0) {
        yvid__fmt(err, cap, "%s: the first frame is not a keyframe", path);
        return YVID_ERR_FORMAT;
    }
    return YVID_OK;
}

static int yvid__hashes_grow(uint64_t** hashes, int64_t n, int64_t* cap_h) {
    if (n < *cap_h) return YVID_OK;
    {
        uint64_t* nh = (uint64_t*)yvid__realloc(*hashes, (size_t)*cap_h * 2 * 8);
        if (!nh) return YVID_ERR_FULL;
        *hashes = nh;
        *cap_h *= 2;
    }
    return YVID_OK;
}

#if !defined(YVID_NO_PL_MPEG) && !defined(YVID_PL_MPEG_EXTERNAL)
static int64_t yvid__index_make_plm(const char* media_path, const char* index_path,
                                      const yvid_index_desc* d, char* err, size_t cap) {
    yvid__plm p;
    yvid_stream st;
    yvid_decoder_open in;
    yvid__canon c;
    uint64_t* hashes = NULL;
    int64_t n = 0, cap_h = 4096, last_key = -1, gop = 0;
    char e[256];
    int rc;
    memset(&in, 0, sizeof in);
    memset(&st, 0, sizeof st);
    in.path = media_path;
    rc = yvid__plm_open(&p, &in, &st, e, sizeof e);
    if (rc < 0) { yvid__fmt(err, cap, "%s: %s", media_path, e); yvid__plm_close(&p); return rc; }
    if (st.fps_num <= 0) { yvid__fmt(err, cap, "%s: rate %.3f is not one MPEG-1 has", media_path, plm_get_framerate(p.plm)); yvid__plm_close(&p); return YVID_ERR_FORMAT; }
    hashes = (uint64_t*)yvid__malloc((size_t)cap_h * 8);
    if (!hashes) { yvid__plm_close(&p); return YVID_ERR_FULL; }
    for (;;) {
        yvid_out o;
        int64_t idx;
        int type;
        memset(&o, 0, sizeof o);
        rc = yvid__plm_next(&p, NULL, &o);
        if (rc == YVID_ENDED) break;
        if (rc < 0) { yvid__fmt(err, cap, "%s: decode failed at frame %lld", media_path, (long long)n); goto fail; }
        type = p.plm->video_decoder->picture_type;
        if (type == 3) { yvid__fmt(err, cap, "%s: B-frames (frame %lld); re-encode with -bf 0", media_path, (long long)n); rc = YVID_ERR_FORMAT; goto fail; }
        rc = yvid__gop_step(media_path, n, type == 1, &last_key, &gop, err, cap);
        if (rc < 0) goto fail;
        idx = (o.pts * st.fps_num + (int64_t)st.fps_den * 45000) / ((int64_t)st.fps_den * 90000);
        if (idx != n) { yvid__fmt(err, cap, "%s: frame %lld has the time of frame %lld: not a constant rate", media_path, (long long)n, (long long)idx); rc = YVID_ERR_FORMAT; goto fail; }
        if ((rc = yvid__hashes_grow(&hashes, n, &cap_h)) < 0) goto fail;
        hashes[n] = yvid__hash_planes(YVID_FMT_I420, &o.planes);
        n++;
    }
    if (n == 0) { yvid__fmt(err, cap, "%s: no frames", media_path); rc = YVID_ERR_FORMAT; goto fail; }
    if (gop == 0) gop = last_key == 0 && n > 0 ? n : 1;
    memset(&c, 0, sizeof c);
    c.codec = YVID_CODEC_MPEG1; c.w = st.w; c.h = st.h; c.format = YVID_FMT_I420;
    c.fps_num = st.fps_num; c.fps_den = st.fps_den; c.frames = n; c.gop = (int32_t)gop;
    c.matrix = d && d->matrix ? d->matrix : YVID_MATRIX_BT601;
    c.range = d && d->range ? d->range : YVID_RANGE_LIMITED;
    c.transfer = d && d->transfer ? d->transfer : YVID_TRC_BT1886;
    c.primaries = d && d->primaries ? d->primaries : YVID_PRIM_BT709;
    c.siting = d && d->siting ? d->siting : YVID_SITING_CENTER;
    rc = yvid__canon_check(&c, e, sizeof e);
    if (rc < 0) { yvid__fmt(err, cap, "%s: %s", media_path, e); goto fail; }
    yvid__plm_close(&p);
    rc = yvid__index_write(media_path, index_path, &c, hashes, n, d ? d->note : NULL, err, cap);
    yvid__free(hashes);
    return rc < 0 ? rc : n;
fail:
    yvid__plm_close(&p);
    yvid__free(hashes);
    return rc < 0 ? rc : YVID_ERR_FORMAT;
}
#endif

#if YVID__MF
/* A color field: the stream's when it states one, else the desc's; both
 * stated and different is refused, neither is refused. */
static int yvid__color_pick(int16_t stream, uint8_t given, const char* what, const char* fix, uint8_t* out, char* err, size_t cap) {
    if (stream > 0 && given && stream != given) {
        yvid__fmt(err, cap, "the stream states %s %d, the index desc says %d; drop the desc's value or re-encode", what, (int)stream, (int)given);
        return YVID_ERR_FORMAT;
    }
    if (stream > 0) { *out = (uint8_t)stream; return YVID_OK; }
    if (given) { *out = given; return YVID_OK; }
    yvid__fmt(err, cap, "the stream does not state its %s (an H.264 SPS without it, or HEVC, whose VUI is not read); state it in yvid_index_desc or encode it with %s", what, fix);
    return YVID_ERR_FORMAT;
}

/* MP4 boxes: the video track's edit list. MF plays an empty edit (a start
 * offset) from time 0 without saying so (measured), where another player
 * delays the picture, so the index maker reads the list itself. Returns 0,
 * or an error with the message. */
static int yvid__mp4_box(yvid__src* s, int64_t at, int64_t end, uint32_t* type, int64_t* body, int64_t* next) {
    uint8_t h[16];
    uint64_t sz;
    if (at + 8 > end || yvid__src_read(s, at, h, 8) != YVID_OK) return 0;
    sz = (uint64_t)h[0] << 24 | (uint64_t)h[1] << 16 | (uint64_t)h[2] << 8 | h[3];
    *type = (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7];
    *body = at + 8;
    if (sz == 1) {
        if (yvid__src_read(s, at + 8, h + 8, 8) != YVID_OK) return 0;
        sz = (uint64_t)h[8] << 56 | (uint64_t)h[9] << 48 | (uint64_t)h[10] << 40 | (uint64_t)h[11] << 32 |
             (uint64_t)h[12] << 24 | (uint64_t)h[13] << 16 | (uint64_t)h[14] << 8 | h[15];
        *body = at + 16;
    } else if (sz == 0) {
        sz = (uint64_t)(end - at);
    }
    if (sz < 8 || at + (int64_t)sz > end) return 0;
    *next = at + (int64_t)sz;
    return 1;
}
#define YVID__4CC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static int yvid__mp4_edits(const char* path, char* err, size_t cap) {
    yvid__src s;
    int64_t at = 0, body, next, moov = -1, moov_end = 0;
    uint32_t type;
    int rc = YVID_OK;
    if (yvid__src_open(&s, path, NULL, 0, NULL, NULL) != YVID_OK) return YVID_OK;   /* MF read it; nothing to add */
    while (yvid__mp4_box(&s, at, s.size, &type, &body, &next)) {
        if (type == YVID__4CC('m', 'o', 'o', 'v')) { moov = body; moov_end = next; break; }
        at = next;
    }
    for (at = moov; moov >= 0 && rc == YVID_OK && yvid__mp4_box(&s, at, moov_end, &type, &body, &next); at = next) {
        int64_t t_at, t_body, t_next, elst = -1, elst_end = 0;
        uint32_t t_type;
        int video = 0;
        if (type != YVID__4CC('t', 'r', 'a', 'k')) continue;
        for (t_at = body; yvid__mp4_box(&s, t_at, next, &t_type, &t_body, &t_next); t_at = t_next) {
            int64_t e_at, e_body, e_next;
            uint32_t e_type;
            if (t_type == YVID__4CC('e', 'd', 't', 's')) {
                for (e_at = t_body; yvid__mp4_box(&s, e_at, t_next, &e_type, &e_body, &e_next); e_at = e_next)
                    if (e_type == YVID__4CC('e', 'l', 's', 't')) { elst = e_body; elst_end = e_next; }
            } else if (t_type == YVID__4CC('m', 'd', 'i', 'a')) {
                for (e_at = t_body; yvid__mp4_box(&s, e_at, t_next, &e_type, &e_body, &e_next); e_at = e_next) {
                    uint8_t hd[12];
                    if (e_type == YVID__4CC('h', 'd', 'l', 'r') && yvid__src_read(&s, e_body, hd, 12) == YVID_OK &&
                        memcmp(hd + 8, "vide", 4) == 0) video = 1;
                }
            }
        }
        if (video && elst >= 0) {
            uint8_t eh[8], en[20];
            uint32_t count;
            int64_t mt;
            int v1;
            if (elst_end - elst < 8 || yvid__src_read(&s, elst, eh, 8) != YVID_OK) continue;
            v1 = eh[0] == 1;
            count = (uint32_t)eh[4] << 24 | (uint32_t)eh[5] << 16 | (uint32_t)eh[6] << 8 | eh[7];
            if (count == 0) continue;
            if (yvid__src_read(&s, elst + 8, en, v1 ? 20 : 12) != YVID_OK) continue;
            if (v1) mt = (int64_t)((uint64_t)en[8] << 56 | (uint64_t)en[9] << 48 | (uint64_t)en[10] << 40 | (uint64_t)en[11] << 32 |
                                   (uint64_t)en[12] << 24 | (uint64_t)en[13] << 16 | (uint64_t)en[14] << 8 | en[15]);
            else mt = (int64_t)(int32_t)((uint32_t)en[4] << 24 | (uint32_t)en[5] << 16 | (uint32_t)en[6] << 8 | en[7]);
            if (count != 1 || mt != 0) {
                yvid__fmt(err, cap, "%s: the video track has an edit list (%u entries, the first at media time %lld): a start offset or a cut that players treat differently; remux without it (-avoid_negative_ts make_zero, no -output_ts_offset)",
                            path, (unsigned)count, (long long)mt);
                rc = YVID_ERR_FORMAT;
            }
        }
    }
    yvid__src_close(&s);
    return rc;
}

/* The rate: the simplest num/den (k/1 or k*1000/1001, k up to 1000) that
 * puts every sample time within 100 ns (MF's unit) of its grid time. MF's
 * own rate is a 100 ns approximation and cannot be the canonical one. */
static int yvid__mf_fit_rate(const int64_t* ts, int64_t n, int32_t* num, int32_t* den) {
    double est;
    int k, f;
    if (n < 2 || ts[n - 1] <= ts[0]) return 0;
    est = (double)(n - 1) * 1e7 / (double)(ts[n - 1] - ts[0]);
    for (f = 0; f < 2; f++) {
        for (k = 1; k <= 1000; k++) {
            int32_t a = f ? k * 1000 : k, b = f ? 1001 : 1;
            int64_t i;
            if (fabs((double)a / b / est - 1.0) > 1e-3) continue;
            for (i = 0; i < n; i++) {
                int64_t g = yvid_frame_time(a, b, i), t = (ts[i] - ts[0]) * 100;
                if (t - g > 100 || g - t > 100) break;
            }
            if (i == n) { *num = a; *den = b; return 1; }
        }
    }
    return 0;
}

static int64_t yvid__index_make_mf(const char* media_path, const char* index_path,
                                     const yvid_index_desc* d, char* err, size_t cap) {
    yvid__mf* m;
    yvid_decoder_open in;
    yvid_stream st;
    yvid__canon c;
    uint64_t* hashes = NULL;
    int64_t* tsv = NULL;
    int64_t n = 0, n1 = 0, cap_h = 4096, cap_t = 4096, last_key = -1, gop = 0, prev_ts = 0, k;
    int32_t num = 0, den = 0;
    int hw = d && d->hw ? (int)d->hw : YVID_HW_OFF;
    int vui_found = 0;
    int16_t vui[5] = { -1, -1, -1, -1, -1 };
    char e[256];
    int rc;
    m = (yvid__mf*)yvid__malloc(sizeof *m);
    if (!m) return YVID_ERR_FULL;
    memset(&in, 0, sizeof in);
    memset(&st, 0, sizeof st);
    in.path = media_path;
    /* 1. the compressed samples, in decode order: B-frames and keyframes */
    tsv = (int64_t*)yvid__malloc((size_t)cap_t * 8);
    if (!tsv) { rc = YVID_ERR_FULL; goto fail; }
    rc = yvid__mf_open_ex(m, &in, &st, YVID_HW_OFF, NULL, 1, 0, 0, e, sizeof e);
    if (rc < 0) { yvid__fmt(err, cap, "%s: %s", media_path, e); goto fail; }
    for (;;) {
        IMFSample* smp = NULL;
        int64_t ts = 0;
        UINT32 clean = 0;
        UINT64 dts = 0;
        rc = yvid__mf_read(m, &smp, &ts);
        if (rc == YVID_ENDED) break;
        if (rc < 0) { yvid__fmt(err, cap, "%s: cannot read sample %lld (0x%08lx)", media_path, (long long)n1, (unsigned long)m->last_hr); goto fail; }
        if (FAILED(YVID__CALL(smp, GetUINT32, YVID__IID(yvid__MFSampleExtension_CleanPoint), &clean))) clean = 0;
        if (SUCCEEDED(YVID__CALL(smp, GetUINT64, YVID__IID(yvid__MFSampleExtension_DecodeTimestamp), &dts)) && (int64_t)dts != ts) {
            YVID__REL(smp);
            yvid__fmt(err, cap, "%s: sample %lld decodes at %lld but shows at %lld (100 ns): B-frames; re-encode with -bf 0",
                        media_path, (long long)n1, (long long)dts, (long long)ts);
            rc = YVID_ERR_FORMAT; goto fail;
        }
        YVID__REL(smp);
        if (n1 > 0 && ts <= prev_ts) {
            yvid__fmt(err, cap, "%s: sample %lld shows before the one decoded ahead of it: B-frames; re-encode with -bf 0", media_path, (long long)n1);
            rc = YVID_ERR_FORMAT; goto fail;
        }
        rc = yvid__gop_step(media_path, n1, clean != 0, &last_key, &gop, err, cap);
        if (rc < 0) goto fail;
        prev_ts = ts;
        if (n1 >= cap_t) {
            int64_t* nt = (int64_t*)yvid__realloc(tsv, (size_t)cap_t * 2 * 8);
            if (!nt) { rc = YVID_ERR_FULL; goto fail; }
            tsv = nt; cap_t *= 2;
        }
        tsv[n1++] = ts;
    }
    vui_found = m->vui_found;
    memcpy(vui, m->vui, sizeof vui);
    yvid__mf_close(m);
    if (n1 == 0) { yvid__fmt(err, cap, "%s: no frames", media_path); rc = YVID_ERR_FORMAT; goto fail; }
    if (gop == 0) gop = n1;
    if ((rc = yvid__mp4_edits(media_path, err, cap)) < 0) goto fail;
    if (tsv[0] != 0) {
        yvid__fmt(err, cap, "%s: the first frame is at %.3f ms, not 0 (an edit list or a start offset); remux with -avoid_negative_ts make_zero", media_path, (double)tsv[0] / 1e4);
        rc = YVID_ERR_FORMAT; goto fail;
    }
    if (!yvid__mf_fit_rate(tsv, n1, &num, &den)) {
        double est = n1 > 1 ? (double)(n1 - 1) * 1e7 / (double)(tsv[n1 - 1] - tsv[0]) : 0.0;
        yvid__fmt(err, cap, "%s: the frame times of %lld frames fit no constant rate (about %.4f fps); a variable rate, a dropped frame, or a coarse timescale: re-encode at a constant rate with -video_track_timescale set to a multiple of the rate",
                    media_path, (long long)n1, est);
        rc = YVID_ERR_FORMAT; goto fail;
    }
    yvid__free(tsv); tsv = NULL;
    /* 2. every frame decoded: on the grid, and its hash */
    rc = yvid__mf_open_ex(m, &in, &st, hw, NULL, 0, num, den, e, sizeof e);
    if (rc < 0) { yvid__fmt(err, cap, "%s: %s", media_path, e); goto fail; }
    st.fps_num = num; st.fps_den = den;
    hashes = (uint64_t*)yvid__malloc((size_t)cap_h * 8);
    if (!hashes) { rc = YVID_ERR_FULL; goto fail; }
    for (;;) {
        yvid_out o;
        yvid_planes dummy;
        int64_t t, grid;
        memset(&o, 0, sizeof o);
        rc = yvid__mf_next(m, &dummy, &o);
        if (rc == YVID_ENDED) break;
        if (rc < 0) { yvid__fmt(err, cap, "%s: decode failed at frame %lld (0x%08lx)", media_path, (long long)n, (unsigned long)m->last_hr); goto fail; }
        if (m->interlaced) {
            yvid__fmt(err, cap, "%s: frame %lld is interlaced; the canonical form is progressive", media_path, (long long)n);
            rc = YVID_ERR_FORMAT; goto fail;
        }
        t = o.pts * 100;
        grid = yvid_frame_time(st.fps_num, st.fps_den, n);
        if (n == 0 && t != 0) {
            yvid__fmt(err, cap, "%s: the first frame is at %.3f ms, not 0 (an edit list or a start offset); remux with -avoid_negative_ts make_zero", media_path, (double)t / 1e6);
            rc = YVID_ERR_FORMAT; goto fail;
        }
        /* 100 ns: MF's unit, so any container time that is a whole number
         * of ticks of a timescale that is a multiple of the rate passes */
        if (t - grid > 100 || grid - t > 100) {
            yvid__fmt(err, cap, "%s: frame %lld is at %lld ns, the grid of %d/%d fps puts it at %lld ns: a variable rate, a dropped frame, or a coarse timescale; re-encode at a constant rate with -video_track_timescale %d",
                        media_path, (long long)n, (long long)t, (int)st.fps_num, (int)st.fps_den, (long long)grid, (int)(st.fps_den == 1 ? st.fps_num * 1000 : st.fps_num));
            rc = YVID_ERR_FORMAT; goto fail;
        }
        if ((rc = yvid__hashes_grow(&hashes, n, &cap_h)) < 0) goto fail;
        hashes[n] = yvid__hash_planes(YVID_FMT_NV12, &o.planes);
        n++;
    }
    if (n != n1) { yvid__fmt(err, cap, "%s: the decoder gave %lld frames for %lld samples", media_path, (long long)n, (long long)n1); rc = YVID_ERR_FORMAT; goto fail; }
    /* 3. a seek to each keyframe decodes it and the next frame exactly: the
     * property the movie's seeks rely on, proved for this file */
    if (!(d && d->no_seek_check)) {
        for (k = 0; k < n; k += gop) {
            int64_t j;
            if (yvid__mf_seek(m, k, 0) != YVID_OK) { yvid__fmt(err, cap, "%s: the seek to keyframe %lld failed (0x%08lx)", media_path, (long long)k, (unsigned long)m->last_hr); rc = YVID_ERR_FORMAT; goto fail; }
            for (j = k; j < k + 2 && j < n; j++) {
                yvid_out o;
                yvid_planes dummy;
                memset(&o, 0, sizeof o);
                rc = yvid__mf_next(m, &dummy, &o);
                if (rc < 0 || yvid__mf_index_of(m, o.pts) != j || yvid__hash_planes(YVID_FMT_NV12, &o.planes) != hashes[j]) {
                    yvid__fmt(err, cap, "%s: frame %lld differs after a seek to keyframe %lld: a keyframe that is not a clean entry point (open GOP?); re-encode with -x264-params open-gop=0",
                                media_path, (long long)j, (long long)k);
                    rc = YVID_ERR_FORMAT; goto fail;
                }
            }
        }
    }
    /* the stream's own statement, from its SPS, where MF reports nothing;
     * a value the canonical form does not have is refused */
    if (vui_found) {
        static const char* const what[5] = { "matrix", "range", "transfer", "primaries", "chroma siting" };
        int16_t* f[5];
        int j;
        f[0] = &st.matrix; f[1] = &st.range; f[2] = &st.transfer; f[3] = &st.primaries; f[4] = &st.siting;
        for (j = 0; j < 5; j++) {
            if (vui[j] == -2) {
                yvid__fmt(err, cap, "%s: the stream states a %s the canonical form does not have (an HDR transfer, RGB, or another chroma location); re-encode with BT.709 or BT.601 color", media_path, what[j]);
                rc = YVID_ERR_FORMAT; goto fail;
            }
            if (*f[j] <= 0 && vui[j] > 0) *f[j] = vui[j];
        }
    }
    memset(&c, 0, sizeof c);
    c.codec = m->codec; c.w = st.w; c.h = st.h; c.format = YVID_FMT_NV12;
    c.fps_num = st.fps_num; c.fps_den = st.fps_den; c.frames = n; c.gop = (int32_t)gop;
    rc = yvid__color_pick(st.matrix, d ? d->matrix : 0, "matrix", "-colorspace bt709", &c.matrix, e, sizeof e);
    if (rc == YVID_OK) rc = yvid__color_pick(st.range, d ? d->range : 0, "range", "-color_range tv", &c.range, e, sizeof e);
    if (rc == YVID_OK) rc = yvid__color_pick(st.transfer, d ? d->transfer : 0, "transfer", "-color_trc bt709", &c.transfer, e, sizeof e);
    if (rc == YVID_OK) rc = yvid__color_pick(st.primaries, d ? d->primaries : 0, "primaries", "-color_primaries bt709", &c.primaries, e, sizeof e);
    if (rc == YVID_OK) rc = yvid__color_pick(st.siting, d ? d->siting : 0, "chroma siting", "-chroma_sample_location left", &c.siting, e, sizeof e);
    if (rc == YVID_OK) rc = yvid__canon_check(&c, e, sizeof e);
    if (rc < 0) { yvid__fmt(err, cap, "%s: %s", media_path, e); goto fail; }
    yvid__mf_close(m);
    yvid__free(m);
    rc = yvid__index_write(media_path, index_path, &c, hashes, n, d ? d->note : NULL, err, cap);
    yvid__free(hashes);
    return rc < 0 ? rc : n;
fail:
    yvid__mf_close(m);
    yvid__free(m);
    yvid__free(hashes);
    yvid__free(tsv);
    return rc < 0 ? rc : YVID_ERR_FORMAT;
}
#endif

static int yvid__backend_for(const yvid_desc* d);

YVID_API int64_t yvid_index_make(const char* media_path, const char* index_path,
                                     const yvid_index_desc* d, char* err, size_t cap) {
    int b;
    if (!media_path) { yvid__fmt(err, cap, "no media path"); return YVID_ERR_ARG; }
    if (d && d->backend) b = d->backend;
    else {
        yvid_desc q;
        memset(&q, 0, sizeof q);
        q.path = media_path;
        b = yvid__backend_for(&q);
        if (b == -1) { yvid__fmt(err, cap, "cannot open %s", media_path); return YVID_ERR_NOT_FOUND; }
    }
    if (b == YVID_BACKEND_PLMPEG) {
#if !defined(YVID_NO_PL_MPEG) && !defined(YVID_PL_MPEG_EXTERNAL)
        return yvid__index_make_plm(media_path, index_path, d, err, cap);
#else
        yvid__fmt(err, cap, "yvid_index_make needs pl_mpeg compiled into this file for MPEG-1");
        return YVID_ERR_NOT_IMPLEMENTED;
#endif
    }
    if (b == YVID_BACKEND_MF) {
#if YVID__MF
        return yvid__index_make_mf(media_path, index_path, d, err, cap);
#else
        yvid__fmt(err, cap, "%s: MP4 needs Media Foundation (Windows); AVFoundation and FFmpeg are not built", media_path);
        return YVID_ERR_NOT_IMPLEMENTED;
#endif
    }
    (void)&yvid__index_write; (void)&yvid__gop_step; (void)&yvid__hashes_grow;
    (void)index_path;
    yvid__fmt(err, cap, "%s: not an MPEG-PS or MP4 file; a frame sequence needs no index", media_path);
    return YVID_ERR_FORMAT;
}

/* --- the movie: helpers ----------------------------------------------------------- */

static void yvid__record_push(yvid_movie* mv, const yvid_record* r);

static void yvid__err(yvid_movie* mv, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mv->error, sizeof mv->error, fmt, ap);
    va_end(ap);
}

static void yvid__push(yvid_movie* mv, uint16_t kind, int64_t t, const yrt_payload* u) {
    yrt_event ev;
    if (!mv->d.ring) return;
    memset(&ev, 0, sizeof ev);
    ev.t_ns = (uint64_t)(t > 0 ? t : 1);
    ev.source = (uint16_t)YRT_SRC_VIDEO;
    ev.kind = kind;
    ev.aux = mv->d.movie_index;
    ev.u = *u;
    yrt_ring_push(mv->d.ring, &ev);
}

static void yvid__result_put(yvid_movie* mv, int64_t id, uint16_t kind, int64_t t, const yrt_payload* u) {
    yvid__result* r = &mv->results[mv->n_results % YVID__RESULTS];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->ev.t_ns = (uint64_t)(t > 0 ? t : 1);
    r->ev.source = (uint16_t)YRT_SRC_VIDEO;
    r->ev.kind = kind;
    r->ev.aux = mv->d.movie_index;
    r->ev.u = *u;
    mv->n_results++;
    yvid__push(mv, kind, t, u);
}

static int64_t yvid__ft(const yvid_movie* mv, int64_t i) {
    return yvid_frame_time(mv->info.fps_num, mv->info.fps_den, i);
}

static int64_t yvid__dueg(const yvid_movie* mv, int64_t m, int64_t lead_ns) {
    return yvid_due(mv->info.fps_num, mv->info.fps_den, m, lead_ns);
}

/* The movie time at RT time t, in the current cycle. With a timeline the
 * movie base is the clock (a later base rate needs no change here). */
static int64_t yvid__movie_time(const yvid_movie* mv, int64_t t) {
    int64_t m;
    if (mv->d.timeline && ytl_base_time(mv->d.timeline, mv->d.base, t, &m)) return m;
    return mv->running ? mv->anchor_mt + (t - mv->anchor_rt) : mv->anchor_mt;
}

/* The movie base's rate, num/den (ytl_get_rate); 1/1 without a
 * timeline, where the header's own anchor runs at rate 1. */
static void yvid__base_rate(const yvid_movie* mv, int64_t* num, int64_t* den) {
    int32_t n = 1, d = 1;
    if (mv->d.timeline) (void)ytl_get_rate(mv->d.timeline, mv->d.base, &n, &d);
    *num = n; *den = d;
}

/* floor(x * num / den) without overflow for |x| up to 2^62 and num, den
 * below 2^31: x is split by den. RT ns to base ns at the base's rate. */
static int64_t yvid__scale(int64_t x, int64_t num, int64_t den) {
    int64_t q, r;
    if (num == den) return x;
    q = yvid__floordiv(x, den);
    r = x - q * den;
    return q * num + yvid__floordiv(r * num, den);
}

/* The movie time the display frame's window reaches: the base time at RT
 * onset + L (ytl_window), so the lead stays RT ns at any base rate and a
 * video frame lands where an annotation at its time lands. Without a
 * timeline the clock runs at 1, where this is the movie time at the onset
 * plus L. */
static int64_t yvid__window_mt(const yvid_movie* mv, int64_t onset, int64_t period, int64_t lead_ns) {
    if (mv->d.timeline) {
        ytl_frame tf;
        int64_t e;
        tf.onset = onset; tf.period = period; tf.index = 0;
        if (ytl_window(mv->d.timeline, mv->d.base, &tf, &e)) return e;
    }
    return yvid__movie_time(mv, onset) + lead_ns;
}

static void yvid__anchor(yvid_movie* mv, int64_t rt, int64_t mt) {
    mv->anchor_rt = rt;
    mv->anchor_mt = mt;
    mv->running = 1;
    mv->anchor_epoch++;
    if (mv->d.timeline) ytl_anchor(mv->d.timeline, mv->d.base, rt, mt);
}

/* A seek's anchor: the annotations it passes over are skipped, not fired
 * late on one frame (ytl_skip). Returns how many were skipped. */
static int yvid__seek_anchor(yvid_movie* mv, int64_t rt, int64_t mt) {
    int n = 0;
    mv->anchor_rt = rt;
    mv->anchor_mt = mt;
    mv->running = 1;
    mv->anchor_epoch++;
    if (mv->d.timeline) {
        n = ytl_skip(mv->d.timeline, mv->d.base, rt, mt);
        if (n < 0) n = 0;
        mv->info.skipped += (uint64_t)n;
    }
    return n;
}

static void yvid__pause_clock(yvid_movie* mv, int64_t rt) {
    if (mv->running) mv->anchor_mt = mv->anchor_mt + (rt - mv->anchor_rt);
    mv->anchor_rt = rt;
    mv->running = 0;
    if (mv->d.timeline) {
        int64_t m;
        if (ytl_base_time(mv->d.timeline, mv->d.base, rt, &m)) mv->anchor_mt = m;
        ytl_pause(mv->d.timeline, mv->d.base, rt);
    }
}

/* Anchors and pauses at mt: manual mode and a seek that stays paused. A
 * jump skips the annotations it passes over; a step to the next frame
 * fires them, on the frame that shows it. Returns how many were skipped. */
static int yvid__hold_at(yvid_movie* mv, int64_t rt, int64_t mt, int jump) {
    int n = 0;
    mv->anchor_rt = rt;
    mv->anchor_mt = mt;
    mv->running = 0;
    mv->anchor_epoch++;
    if (mv->d.timeline) {
        if (jump) {
            n = ytl_skip(mv->d.timeline, mv->d.base, rt, mt);
            if (n < 0) n = 0;
            mv->info.skipped += (uint64_t)n;
        } else {
            ytl_anchor(mv->d.timeline, mv->d.base, rt, mt);
        }
        ytl_pause(mv->d.timeline, mv->d.base, rt);
    }
    return n;
}

/* --- the soundtrack, as the frame thread drives it ------------------------------
 * The sound is a ysp/audio.h stream that the decode thread fills from the
 * soundtrack's source. Sample s plays at movie time s / rate: a start at
 * movie time mt begins with the sample nearest mt (a tie to the earlier,
 * ysp_audio's rule) and a pause, a seek or the end stops it. */

/* The sample nearest movie time mt (ns) of the current cycle, counted
 * from the first cycle, as the stream numbers its samples. */
static int64_t yvid__snd_sample(const yvid_movie* mv, int64_t mt, int64_t cycle) {
    int64_t r = (int64_t)mv->snd_rate, q = yvid__floordiv(mt, YVID__NS), rem = mt - q * YVID__NS;
    return cycle * mv->snd_frames + q * r + (2 * rem * r + YVID__NS - 1) / (2 * YVID__NS);
}

static void yvid__snd_record(yvid_movie* mv, uint32_t what, int64_t t, int64_t sample, int64_t id) {
    yrt_payload u;
    memset(&u, 0, sizeof u);
    u.i64[0] = mv->snd_id; u.i64[1] = sample; u.i64[2] = t;
    u.u32[8] = what; u.u32[9] = (uint32_t)id;
    yvid__push(mv, (uint16_t)YVID_EV_SOUND, t > 0 ? t : YVID__NOW(), &u);
}

static int yvid__post(yvid_movie* mv, uint32_t op, int64_t g);

/* Stops the sound at t (0: at once) and refills from the sample of movie
 * time mt in cycle `cycle`, for the next start. */
static void yvid__snd_rearm(yvid_movie* mv, int64_t t, int64_t mt, int64_t cycle, int64_t id) {
    int64_t want;
    if (!mv->snd) return;
    want = yvid__snd_sample(mv, mt, cycle);
    if (mv->snd_phase == 1 && mv->snd_want == want) return;   /* that refill is on its way */
    if (mv->snd_phase == 2) { mv->snd_ops->stop(mv->snd, t); yvid__snd_record(mv, 2, t, -1, id); }
    mv->snd_want = want;
    mv->snd_phase = 1;
    mv->snd_ops->refilling(mv->snd);
    yvid__post(mv, YVID__MSG_SND, want);
}

static void yvid__snd_halt(yvid_movie* mv, int64_t t, int64_t id) {
    if (!mv->snd || mv->snd_phase != 2) return;
    mv->snd_ops->stop(mv->snd, t);
    yvid__snd_record(mv, 2, t, -1, id);
    mv->snd_phase = 0;
}

/* Starts the sound at t when its refill is in; true when it started. */
static int yvid__snd_go(yvid_movie* mv, int64_t t, int64_t id) {
    int64_t pid;
    if (!mv->snd || mv->snd_phase != 1 || !mv->snd_ops->ready(mv->snd, mv->snd_want)) return 0;
    pid = mv->snd_ops->start(mv->snd, t);
    if (pid <= 0) return 0;
    mv->snd_id = pid;
    mv->snd_t = t;
    mv->snd_phase = 2;
    mv->follow_last_m = INT64_MIN;   /* the follow starts again from this play */
    yvid__snd_record(mv, 1, t, mv->snd_want, id);
    return 1;
}

/* The first time both the sound and the next display frame can make, for
 * an ASAP control with a soundtrack. */
static int64_t yvid__snd_asap(yvid_movie* mv, const yscr_frame* f) {
    int64_t now = YVID__NOW(), lead = mv->snd_ops->lead(mv->snd);
    int64_t t = (now > f->onset ? now : f->onset) + (lead > 0 ? lead : 0) + f->period;
    /* on a predicted display onset: else the first frame shows up to half a
     * period away from the first sample, and every frame after keeps that
     * phase (measured on the real device: -6.2 and +4.5 ms in two runs) */
    if (f->period > 0) t = f->onset + (t - f->onset + f->period - 1) / f->period * f->period;
    return t;
}

/* --- queues ------------------------------------------------------------------------ */

static void yvid__free_push(yvid_movie* mv, uint32_t s) {
    uint32_t t = mv->free_tail;
    if (mv->gpu && (int)s == mv->gpu_shown) return;   /* its texture is on screen */
    mv->free_q[t & (YVID__QCAP - 1)] = s;
    yvid__st32(&mv->free_tail, t + 1);
}
static int yvid__free_pop(yvid_movie* mv) {
    uint32_t h = mv->free_head;
    int s;
    if (h == yvid__ld32(&mv->free_tail)) return -1;
    s = (int)mv->free_q[h & (YVID__QCAP - 1)];
    yvid__st32(&mv->free_head, h + 1);
    return s;
}
static void yvid__ready_push(yvid_movie* mv, uint32_t s) {
    uint32_t t = mv->ready_tail;
    mv->ready_q[t & (YVID__QCAP - 1)] = s;
    yvid__st32(&mv->ready_tail, t + 1);
}

/* --- decode thread ---------------------------------------------------------------- */

static void yvid__dt_fail(yvid_movie* mv, int rc, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mv->dt_error, sizeof mv->dt_error, fmt, ap);
    va_end(ap);
    yvid__st32(&mv->dec_err, (uint32_t)(-rc));
}

static void yvid__on_msg(void* ctx, const void* msg, uint32_t seq) {
    yvid_movie* mv = (yvid_movie*)ctx;
    yvid__msg m;
    (void)seq;
    memcpy(&m, msg, sizeof m);   /* the pump's slot has no alignment promise */
    if (m.op == YVID__MSG_SND && mv->snd) mv->snd_ops->refill(mv->snd, m.g);
    if (m.op == YVID__MSG_WAKE) yvid__st32(&mv->snd_wake, 0);
    if (m.op == YVID__MSG_SEEK) {
        mv->dt_epoch = m.epoch;
        mv->dt_next = m.g;
        mv->dt_eos = 0;
        mv->dt_key = -1;
        mv->dt_discarded = 0;
    }
}

/* PENDING: nothing now, ask again later. The pump blocks when on_idle has
 * no more to do, and only a message wakes it: marked idle, the thread gets
 * one from the next yvid_update(), so the decoder is asked again once a
 * display frame. Before v0.2.1 nothing woke it until a seek. */
static bool yvid__pending(yvid_movie* mv) {
    if (!mv->inline_mode) yvid__xchg32(&mv->idle, 1);
    return false;
}

/* One step: discard one frame on the way to the target, or decode one into a
 * free slot. True: there is more to do now. */
static bool yvid__dstep(yvid_movie* mv) {
    const int64_t N = mv->info.frames;
    int64_t want, g, idx;
    int s, rc;
    yvid_out out;
    yvid__slot* sl;
    if (yvid__ld32(&mv->dec_err) || mv->dt_eos) return false;
    want = yvid__ld64(&mv->want_g);
    g = mv->dt_next > want ? mv->dt_next : want;
    if (!mv->d.loop && g >= N) { mv->dt_eos = 1; return false; }
    idx = g % N;
    if (mv->dt_pos != idx) {
        int64_t key = (mv->dec_caps & YVID_DEC_RANDOM_ACCESS) ? idx : idx - idx % mv->info.gop;
        if (mv->dt_pos < 0 || mv->dt_pos > idx || mv->dt_pos < key) {
            YRT_ZONE(zs, "yvid.seek");
            rc = mv->dec->seek(mv->dec_ctx, key, mv->timescale > 0 ? yvid__ft(mv, key) / 1000 * mv->timescale / 1000000 : key);
            YRT_ZONE_END(zs);
            if (rc < 0) { yvid__dt_fail(mv, YVID_ERR_DECODER, "ysp_video: %s: seek to keyframe %lld failed (%d)", mv->dec->name, (long long)key, rc); return false; }
            mv->dt_pos = key;
            mv->dt_key = key;
        }
        if (mv->dt_pos < idx) {
            memset(&out, 0, sizeof out);
            out.index = -1;
            rc = mv->dec->next(mv->dec_ctx, NULL, &out);
            if (rc == YVID_PENDING) return yvid__pending(mv);
            if (rc != YVID_OK) { yvid__dt_fail(mv, YVID_ERR_DECODER, "ysp_video: %s: the stream ended or failed at frame %lld while seeking (%d)", mv->dec->name, (long long)mv->dt_pos, rc); return false; }
            mv->dt_pos++;
            mv->dt_discarded++;
            mv->dt_next = g;
            return true;
        }
    }
    /* a slot taken before and not filled (PENDING, an error): the free
     * queue has one producer, the frame thread, so it is kept here */
    if (mv->dt_spare >= 0) { s = mv->dt_spare; mv->dt_spare = -1; }
    else s = yvid__free_pop(mv);
    if (s < 0) {
        if (mv->inline_mode) return false;
        yvid__xchg32(&mv->idle, 1);
        s = yvid__free_pop(mv);
        if (s < 0) return false;
        yvid__xchg32(&mv->idle, 0);
    }
    sl = &mv->slots[s];
    memset(&out, 0, sizeof out);
    out.index = -1;
    {
        yvid_planes dst;
        int64_t t0 = YVID__COST_NOW(), dt;
        uint8_t* base = sl->data;
        YRT_ZONE(zd, "yvid.decode");
        yvid__planes_layout(mv->info.format, mv->info.w, mv->info.h, base, &dst);
#if YVID__MF
        if (mv->gpu) ((yvid__mf*)mv->dec_ctx)->gpu_dst = sl->gtex;   /* where the decoder copies */
#endif
        rc = mv->dec->next(mv->dec_ctx, &dst, &out);
        dt = YVID__COST_NOW() - t0;
        YRT_ZONE_END(zd);
        if (rc == YVID_PENDING) { mv->dt_spare = s; return yvid__pending(mv); }
        if (rc == YVID_ENDED) {
            mv->dt_spare = s;
            yvid__dt_fail(mv, YVID_ERR_DECODER, "ysp_video: %s: the stream ended at frame %lld; the index says %lld frames", mv->dec->name, (long long)idx, (long long)N);
            return false;
        }
        if (rc < 0) {
            mv->dt_spare = s;
            yvid__dt_fail(mv, YVID_ERR_DECODER, "ysp_video: %s: decode of frame %lld failed (%d)", mv->dec->name, (long long)idx, rc);
            return false;
        }
        if (dt > yvid__ft(mv, 1) && mv->d.ring) {
            yrt_payload u;
            memset(&u, 0, sizeof u);
            u.i64[0] = dt; u.i64[1] = idx;
            yvid__push(mv, (uint16_t)YVID_EV_DECODE, t0, &u);
        }
        sl->flags = 0;
        {
            int64_t got = out.index;
            if (got < 0 && mv->timescale > 0) {
                /* round(pts * num / (den * timescale)), in two steps so a long
                 * stream's pts cannot overflow the product */
                int64_t d = (int64_t)mv->info.fps_den * mv->timescale;
                int64_t a = yvid__floordiv(out.pts, d), b = out.pts - a * d;
                got = a * mv->info.fps_num + (b * mv->info.fps_num + d / 2) / d;
            }
            if (got != idx) sl->flags |= YVID_F_TS_MISMATCH;
        }
        {
            const yvid_planes* src = (out.flags & YVID_OUT_BORROWED) ? &out.planes : &dst;
            uint64_t want_h = 0;
            int have = 0;
            if (out.flags & YVID_OUT_HAS_HASH) { want_h = out.hash; have = 1; }
            else if (mv->hashes) { want_h = mv->hashes[idx]; have = 1; }
            if (out.flags & YVID__OUT_GPU) have = 0;   /* no bytes on the CPU: no hash */
            if (have && yvid__hash_planes(mv->info.format, src) != want_h) sl->flags |= YVID_F_HASH_MISMATCH;
            if (out.flags & YVID__OUT_GPU) {
                /* the frame is in the slot's texture already */
            } else if (yvid__is_yuv(mv->info.format) && (out.flags & YVID_OUT_BORROWED)) {
                /* the decoder's planes, with its pitch, into the slot's tight
                 * ones: what the upload sends and the next decode cannot touch */
                int k, y;
                for (k = 0; k < yvid__n_planes(mv->info.format); k++) {
                    size_t rb = yvid__row_bytes(mv->info.format, &dst, k);
                    for (y = 0; y < dst.h[k]; y++)
                        memcpy(dst.data[k] + (size_t)y * (size_t)dst.stride[k], out.planes.data[k] + (size_t)y * (size_t)out.planes.stride[k], rb);
                }
            } else if (out.flags & YVID_OUT_BORROWED) {
                int y;
                size_t rb = (size_t)mv->info.w * (size_t)yvid__bpt(mv->info.format);
                for (y = 0; y < mv->info.h; y++)
                    memcpy(sl->data + (size_t)y * rb, out.planes.data[0] + (size_t)y * (size_t)out.planes.stride[0], rb);
            }
        }
    }
    mv->dt_pos = idx + 1;
    sl->g = g;
    sl->epoch = mv->dt_epoch;
    sl->key = mv->dt_key;
    sl->discarded = mv->dt_discarded;
    mv->dt_key = -1;
    mv->dt_discarded = 0;
    mv->dt_next = g + 1;
    yvid__ready_push(mv, (uint32_t)s);
    return true;
}

#if !defined(YRT_NO_THREADS)
static bool yvid__on_idle(void* ctx) {
    yvid_movie* mv = (yvid_movie*)ctx;
    /* the soundtrack first: a block of it is 85 ms of sound, one frame's
     * decode at most a frame period */
    bool more = mv->snd ? mv->snd_ops->step(mv->snd) : false;
    return yvid__dstep(mv) || more;
}

static bool yvid__on_start(void* ctx, char* err, size_t cap) {
    (void)ctx; (void)err; (void)cap;
    YRT_THREAD_NAME("ysp_video decode");
    return true;
}
#endif

/* Hands a message to the decode thread, or runs it here inline. */
static int yvid__post(yvid_movie* mv, uint32_t op, int64_t g) {
    yvid__msg m;
    m.op = op; m.epoch = mv->ft_epoch; m.g = g;
    if (mv->inline_mode) { yvid__on_msg(mv, &m, 0); return YVID_OK; }
#if !defined(YRT_NO_THREADS)
    return yrt_pump_submit(&mv->pump, &m) < 0 ? YVID_ERR_FULL : YVID_OK;
#else
    return YVID_ERR_ARG;
#endif
}

static void yvid__wake(yvid_movie* mv) {
    if (mv->inline_mode) return;
    if (yvid__xchg32(&mv->idle, 0) == 1) yvid__post(mv, YVID__MSG_WAKE, 0);
}

/* --- open ---------------------------------------------------------------------- */

static int yvid__display_rate(yvid_movie* mv, int32_t* num, int32_t* den) {
    *num = mv->d.refresh_num; *den = mv->d.refresh_den > 0 ? mv->d.refresh_den : 1;
    if (*num > 0) return 1;
    if (mv->screen && yscr_is_open(mv->screen)) {
        yscr_caps c;
        yscr_get_caps(mv->screen, &c);
        if (c.mode.refresh_num > 0 && c.mode.refresh_den > 0) { *num = c.mode.refresh_num; *den = c.mode.refresh_den; return 1; }
        if (c.period_ns > 0) { *num = 1000000000; *den = (int32_t)c.period_ns; return 1; }
    }
    *num = 0; *den = 0;
    return 0;
}

static int yvid__backend_for(const yvid_desc* d) {
    uint8_t b[8];
    yvid__src s;
    if (d->backend != YVID_BACKEND_AUTO) return d->backend;
    if (d->decoder) return YVID_BACKEND_CUSTOM;
    if (yvid__src_open(&s, d->path, d->data, d->size, d->reader, d->reader_ctx) != YVID_OK) return -1;
    memset(b, 0, sizeof b);
    if (s.size >= 8) yvid__src_read(&s, 0, b, 8);
    yvid__src_close(&s);
    if (memcmp(b, "YSPVSEQ1", 8) == 0) return YVID_BACKEND_SEQ;
    if (b[0] == 0 && b[1] == 0 && b[2] == 1 && (b[3] == 0xBA || b[3] == 0xB3)) return YVID_BACKEND_PLMPEG;
    if (memcmp(b + 4, "ftyp", 4) == 0) return YVID_BACKEND_MF;
    return -2;   /* none of the three */
}

YVID_API int yvid_probe(const yvid_desc* d, yvid_info* out, char* err, size_t cap) {
    yvid__canon c;
    int b, rc;
    if (err && cap) err[0] = '\0';
    if (!d || !out) { yvid__fmt(err, cap, "ysp_video: probe: no desc or no info"); return YVID_ERR_ARG; }
    memset(out, 0, sizeof *out);
    b = yvid__backend_for(d);
    if (b == YVID_BACKEND_SEQ) {
        yvid__src s;
        uint8_t hdr[128];
        int32_t comp;
        int64_t io;
        if (yvid__src_open(&s, d->path, d->data, d->size, d->reader, d->reader_ctx) != YVID_OK ||
            yvid__src_read(&s, 0, hdr, sizeof hdr) != YVID_OK) { yvid__src_close(&s); yvid__fmt(err, cap, "cannot read the file"); return YVID_ERR_IO; }
        yvid__src_close(&s);
        if ((rc = yvid__seq_parse(hdr, &c, &comp, &io, err, cap)) < 0) return rc;
    } else if (b == YVID_BACKEND_PLMPEG || b == YVID_BACKEND_MF) {
        yvid__index ix;
        if ((rc = yvid__index_load(d, &ix, 0, err, cap)) < 0) return rc;
        c = ix.c;
    } else {
        yvid__fmt(err, cap, "yvid_probe reads frame sequences and indexed MPEG-1 and MP4 files only");
        return YVID_ERR_REFUSED;
    }
    out->w = c.w; out->h = c.h; out->fps_num = c.fps_num; out->fps_den = c.fps_den;
    out->frames = c.frames; out->gop = c.gop; out->codec = c.codec; out->format = c.format;
    out->matrix = c.matrix; out->range = c.range; out->transfer = c.transfer; out->primaries = c.primaries; out->siting = c.siting;
    out->duration = yvid_frame_time(c.fps_num, c.fps_den, c.frames);
    out->backend = (yvid_backend)b;
    return YVID_OK;
}

static int yvid__fill(int32_t* have, int32_t from_index, const char* what, char* err, size_t cap) {
    if (*have <= 0) { *have = from_index; return YVID_OK; }
    if (from_index > 0 && *have != from_index) {
        yvid__fmt(err, cap, "the stream says %s %d, the index says %d; the index is stale: make it again", what, (int)*have, (int)from_index);
        return YVID_ERR_FORMAT;
    }
    return YVID_OK;
}

static void yvid__close_partial(yvid_movie* mv) {
#if !defined(YRT_NO_THREADS)
    if (!mv->inline_mode) yrt_pump_close(&mv->pump);
#endif
    if (mv->snd) { mv->snd_ops->close(mv->snd); mv->snd = NULL; mv->snd_ops = NULL; }
#if YVID__MF
    if (mv->gpu) {
        int k;
        if (mv->gfx && mv->tex.id) ygfx_texture_rebind(mv->gfx, mv->tex, mv->tex);
        for (k = 0; k < mv->n_slots; k++) {
            if (mv->gfx && mv->slots[k].gimp.id) ygfx_texture_free(mv->gfx, mv->slots[k].gimp);
            if (mv->slots[k].gtex) { ID3D11Texture2D* t = (ID3D11Texture2D*)mv->slots[k].gtex; YVID__REL(t); }
            mv->slots[k].gtex = NULL;
            mv->slots[k].gimp.id = 0;
        }
        mv->gpu = 0;
    }
#endif
    if (mv->dec && mv->dec_ctx && mv->dec->close) mv->dec->close(mv->dec_ctx);
    if (mv->gfx && mv->tex.id) ygfx_texture_free(mv->gfx, mv->tex);
    yvid__free(mv->be_mem);
    yvid__free(mv->hashes);
    yvid__free(mv->mem_raw);
    mv->be_mem = NULL; mv->hashes = NULL; mv->mem_raw = NULL;
    mv->dec = NULL; mv->dec_ctx = NULL;
    mv->tex.id = 0;
}

YVID_API bool yvid_open(yvid_movie* mv, ygfx_gfx* g, const yvid_desc* desc) {
    yvid_decoder_open in;
    yvid_stream st;
    yvid__canon c;
    yvid__index ix;
    char e[384];
    int backend, rc, i;
    int32_t rn, rd;
    if (!mv) return false;
    if (mv->open) { yvid__err(mv, "ysp_video: already open"); return false; }
    memset(mv, 0, sizeof *mv);
    if (!desc) { yvid__err(mv, "ysp_video: no desc"); return false; }
    mv->d = *desc;
    mv->gfx = g;
    mv->screen = ygfx_screen(g);
    if (!(desc->lead == YVID_LEAD_NONE || (desc->lead >= 0.0 && desc->lead < 1.0))) { yvid__err(mv, "ysp_video: desc.lead must be in [0, 1) or YVID_LEAD_NONE"); return false; }
    mv->info.lead = desc->lead == 0.0 ? 0.5 : desc->lead == YVID_LEAD_NONE ? 0.0 : desc->lead;
    if (desc->timeline) {
        /* one rule for video and annotations: the timeline's own lead */
        double tl_lead = ytl_lead(desc->timeline);
        if (!ytl_is_open(desc->timeline)) { yvid__err(mv, "ysp_video: desc.timeline is not open"); return false; }
        if (desc->base < 1 || desc->base >= YTL_MAX_BASES) { yvid__err(mv, "ysp_video: desc.base must be 1..%d with a timeline", YTL_MAX_BASES - 1); return false; }
        if (desc->lead != 0.0 && mv->info.lead != tl_lead) { yvid__err(mv, "ysp_video: desc.lead (%g) differs from the timeline's (%g): video and annotations must share one rule", mv->info.lead, tl_lead); return false; }
        mv->info.lead = tl_lead;
    }
    if (desc->hw_decode != YVID_HW_AUTO && desc->hw_decode != YVID_HW_OFF && desc->hw_decode != YVID_HW_DXVA) {
        yvid__err(mv, "ysp_video: desc.hw_decode %d is not AUTO, OFF or DXVA", (int)desc->hw_decode);
        return false;
    }
    if (desc->ahead < 0 || desc->ahead > YVID_MAX_SLOTS - 2) { yvid__err(mv, "ysp_video: desc.ahead must be 0..%d", YVID_MAX_SLOTS - 2); return false; }
    if (desc->gpu_path == YVID_PATH_SHARED) {
        yvid__err(mv, "ysp_video: the SHARED GPU path is not built; YVID_PATH_GPU is (docs/video.md)");
        return false;
    }

    backend = yvid__backend_for(desc);
    if (backend == -2) { yvid__err(mv, "ysp_video: %s is not a frame sequence, MPEG-PS or MP4 file", desc->path ? desc->path : "the source"); return false; }
    if (backend < 0) { yvid__err(mv, "ysp_video: cannot open %s", desc->path ? desc->path : "the source"); return false; }
    if ((backend == YVID_BACKEND_MF && !YVID__MF) || backend == YVID_BACKEND_AVF || backend == YVID_BACKEND_FFMPEG) {
        yvid__err(mv, "ysp_video: %s: H.264 and HEVC need Media Foundation (Windows, without YVID_NO_MF); AVFoundation and FFmpeg are not built", desc->path ? desc->path : "the source");
        return false;
    }
    if (backend == YVID_BACKEND_CUSTOM) {
        if (!desc->decoder || desc->decoder->version != YVID_DECODER_VERSION || !desc->decoder->open ||
            !desc->decoder->next || !desc->decoder->seek) { yvid__err(mv, "ysp_video: desc.decoder is missing or of another version"); return false; }
        mv->dec = desc->decoder;
        mv->dec_ctx = desc->decoder_ctx;
    } else if (backend == YVID_BACKEND_SEQ) {
        mv->be_mem = yvid__malloc(sizeof(yvid__seqr));
        if (!mv->be_mem) { yvid__err(mv, "ysp_video: out of memory"); return false; }
        memset(mv->be_mem, 0, sizeof(yvid__seqr));
        mv->dec = &yvid__seq_decoder;
        mv->dec_ctx = mv->be_mem;
    } else if (backend == YVID_BACKEND_PLMPEG) {
#ifdef YVID_NO_PL_MPEG
        yvid__err(mv, "ysp_video: MPEG-1 needs pl_mpeg; this build has YVID_NO_PL_MPEG");
        return false;
#else
        mv->be_mem = yvid__malloc(sizeof(yvid__plm));
        if (!mv->be_mem) { yvid__err(mv, "ysp_video: out of memory"); return false; }
        memset(mv->be_mem, 0, sizeof(yvid__plm));
        mv->dec = &yvid__plm_decoder;
        mv->dec_ctx = mv->be_mem;
#endif
#if YVID__MF
    } else if (backend == YVID_BACKEND_MF) {
        yvid__mf* m = (yvid__mf*)yvid__malloc(sizeof(yvid__mf));
        yvid__index hx;
        if (!m) { yvid__err(mv, "ysp_video: out of memory"); return false; }
        memset(m, 0, sizeof *m);
        /* the rate is the index's (MF's is a 100 ns approximation) */
        if (yvid__index_load(desc, &hx, 0, e, sizeof e) < 0) { yvid__free(m); yvid__err(mv, "ysp_video: %s", e); return false; }
        m->req_num = hx.c.fps_num; m->req_den = hx.c.fps_den;
        m->req_hw = desc->hw_decode == YVID_HW_AUTO ? YVID__HW_DEFAULT : (int)desc->hw_decode;
        m->req_auto = desc->hw_decode == YVID_HW_AUTO;
        if (mv->screen) (void)yscr_native(mv->screen, &m->req_nat);
        if (desc->gpu_path == YVID_PATH_GPU) {
            const char* why = NULL;
            if (!g || !(ygfx_features(g) & YGFX_FEAT_IMPORT_NV12)) why = "this renderer cannot import NV12 D3D11 textures (ygfx_features)";
            else if (!m->req_nat.d3d11_device) why = "the screen has no D3D11 device (DXGI_FLIP or COMPOSITION)";
            else if (!yvid__mf_video_device(m->req_nat.d3d11_device)) why = "the screen's D3D11 device needs D3D11_CREATE_DEVICE_VIDEO_SUPPORT and multithread protection (ysp/screen.h's desc.d3d11_video)";
            else if (desc->hw_decode != YVID_HW_AUTO && desc->hw_decode != YVID_HW_DXVA) why = "hw_decode must be AUTO or DXVA";
            if (why) { yvid__free(m); yvid__err(mv, "ysp_video: YVID_PATH_GPU: %s", why); return false; }
            m->req_hw = YVID_HW_DXVA;
            m->req_gpu = 1;
        }
        mv->be_mem = m;
        mv->dec = &yvid__mf_decoder;
        mv->dec_ctx = m;
#endif
    } else {
        yvid__err(mv, "ysp_video: unknown backend %d", backend);
        return false;
    }
    mv->info.backend = (yvid_backend)backend;
    if (desc->gpu_path == YVID_PATH_GPU && backend != YVID_BACKEND_MF) {
        yvid__err(mv, "ysp_video: YVID_PATH_GPU is for Media Foundation movies (MP4)");
        yvid__close_partial(mv);
        return false;
    }
    memset(&in, 0, sizeof in);
    in.path = desc->path; in.data = desc->data; in.size = desc->size;
    in.reader = desc->reader; in.reader_ctx = desc->reader_ctx;
    in.index = desc->index; in.index_size = desc->index_size;
    memset(&st, 0, sizeof st);
    st.frames = -1;
    e[0] = 0;
    rc = mv->dec->open(mv->dec_ctx, &in, &st, e, sizeof e);
    if (rc < 0) {
        yvid__err(mv, "ysp_video: %s: %s", mv->dec->name, e[0] ? e : yvid_strerror(rc));
        if (mv->dec->close) mv->dec->close(mv->dec_ctx);
        mv->dec = NULL;
        yvid__close_partial(mv);
        return false;
    }
    /* the description: the stream's values, checked against the index's */
    memset(&c, 0, sizeof c);
    c.w = st.w; c.h = st.h; c.format = st.format; c.fps_num = st.fps_num; c.fps_den = st.fps_den;
    c.frames = st.frames; c.gop = st.gop; c.codec = st.codec;
    c.matrix = (uint8_t)(st.matrix > 0 ? st.matrix : 0); c.range = (uint8_t)(st.range > 0 ? st.range : 0);
    c.transfer = (uint8_t)(st.transfer > 0 ? st.transfer : 0); c.primaries = (uint8_t)(st.primaries > 0 ? st.primaries : 0);
    c.siting = (uint8_t)(st.siting > 0 ? st.siting : 0);
    memset(&ix, 0, sizeof ix);
    if (backend == YVID_BACKEND_PLMPEG || backend == YVID_BACKEND_MF || desc->index || (backend == YVID_BACKEND_CUSTOM && desc->path)) {
        int32_t v;
        rc = yvid__index_load(desc, &ix, 1, e, sizeof e);
        if (rc < 0) { yvid__err(mv, "ysp_video: %s", e); yvid__close_partial(mv); return false; }
        mv->hashes = ix.hashes;
        if (backend == YVID_BACKEND_PLMPEG || backend == YVID_BACKEND_MF || desc->path) {
            yvid__src s;
            uint64_t head = 0, tail = 0;
            if (yvid__src_open(&s, desc->path, desc->data, desc->size, desc->reader, desc->reader_ctx) == YVID_OK) {
                rc = s.size == ix.media_size ? yvid__src_ends(&s, &head, &tail) : YVID_ERR_FORMAT;
                yvid__src_close(&s);
                if (rc < 0 || head != ix.head || tail != ix.tail) {
                    yvid__err(mv, "ysp_video: the index does not belong to this file (size or hash of its ends differ); make it again");
                    yvid__close_partial(mv);
                    return false;
                }
            }
        }
        rc = YVID_OK;
        if (rc == YVID_OK) rc = yvid__fill(&c.w, ix.c.w, "width", e, sizeof e);
        if (rc == YVID_OK) rc = yvid__fill(&c.h, ix.c.h, "height", e, sizeof e);
        if (rc == YVID_OK) rc = yvid__fill(&c.format, ix.c.format, "format", e, sizeof e);
        if (rc == YVID_OK) rc = yvid__fill(&c.fps_num, ix.c.fps_num, "rate numerator", e, sizeof e);
        if (rc == YVID_OK) rc = yvid__fill(&c.fps_den, ix.c.fps_den, "rate denominator", e, sizeof e);
        if (rc == YVID_OK) rc = yvid__fill(&c.gop, ix.c.gop, "GOP", e, sizeof e);
        if (rc == YVID_OK) rc = yvid__fill(&c.codec, ix.c.codec, "codec", e, sizeof e);
        v = c.matrix; if (rc == YVID_OK) rc = yvid__fill(&v, ix.c.matrix, "matrix", e, sizeof e); c.matrix = (uint8_t)v;
        v = c.range; if (rc == YVID_OK) rc = yvid__fill(&v, ix.c.range, "range", e, sizeof e); c.range = (uint8_t)v;
        v = c.transfer; if (rc == YVID_OK) rc = yvid__fill(&v, ix.c.transfer, "transfer", e, sizeof e); c.transfer = (uint8_t)v;
        v = c.primaries; if (rc == YVID_OK) rc = yvid__fill(&v, ix.c.primaries, "primaries", e, sizeof e); c.primaries = (uint8_t)v;
        v = c.siting; if (rc == YVID_OK) rc = yvid__fill(&v, ix.c.siting, "siting", e, sizeof e); c.siting = (uint8_t)v;
        if (rc == YVID_OK && c.frames > 0 && c.frames != ix.c.frames) { yvid__fmt(e, sizeof e, "the stream says %lld frames, the index says %lld", (long long)c.frames, (long long)ix.c.frames); rc = YVID_ERR_FORMAT; }
        if (c.frames <= 0) c.frames = ix.c.frames;
        if (rc < 0) { yvid__err(mv, "ysp_video: %s: %s", desc->path ? desc->path : "the source", e); yvid__close_partial(mv); return false; }
    }
    if (yvid__is_rgb(c.format)) yvid__rgb_color(&c);
    rc = yvid__canon_check(&c, e, sizeof e);
    if (rc < 0) { yvid__err(mv, "ysp_video: %s: not the canonical form: %s", desc->path ? desc->path : "the source", e); yvid__close_partial(mv); return false; }
    mv->info.w = c.w; mv->info.h = c.h; mv->info.format = c.format;
    mv->info.fps_num = c.fps_num; mv->info.fps_den = c.fps_den;
    mv->info.frames = c.frames; mv->info.gop = c.gop; mv->info.codec = c.codec;
    mv->info.matrix = c.matrix; mv->info.range = c.range; mv->info.transfer = c.transfer;
    mv->info.primaries = c.primaries; mv->info.siting = c.siting;
    mv->info.duration = yvid__ft(mv, c.frames);
    mv->info.upload_format = c.format;   /* YUV goes up as planes (ysp/gfx.h's video program converts) */
    mv->info.path = YVID_PATH_UPLOAD;
    mv->gpu_shown = -1;
#if YVID__MF
    if (backend == YVID_BACKEND_MF && ((yvid__mf*)mv->be_mem)->gpu) {
        yvid__mf* m = (yvid__mf*)mv->be_mem;
        rc = yvid__mf_peek(m);
        if (rc < 0) { yvid__err(mv, "ysp_video: YVID_PATH_GPU: the first frame (0x%08lx)", (unsigned long)m->last_hr); yvid__close_partial(mv); return false; }
        mv->info.path = YVID_PATH_GPU;
        mv->gpu_w = m->tw; mv->gpu_h = m->th;
        mv->gpu_ax = m->ax; mv->gpu_ay = m->ay;
    }
#endif
    /* light: AUTO is linear light under a calibration, the codes without
     * one; an RGB frame sequence holds device values, so CODES */
    mv->info.light = YVID_LIGHT_CODES;
    if (yvid__is_yuv(c.format) && (desc->light == YVID_LIGHT_EOTF || (desc->light == YVID_LIGHT_AUTO && ygfx_calibrated(g))))
        mv->info.light = YVID_LIGHT_EOTF;
    if (!yvid__is_yuv(c.format) && desc->light == YVID_LIGHT_EOTF) {
        yvid__err(mv, "ysp_video: light EOTF is for YUV movies; an RGB frame sequence holds device values (light CODES)");
        yvid__close_partial(mv);
        return false;
    }
    mv->timescale = st.timescale;
    mv->dec_caps = st.caps;
#if YVID__MF
    if (backend == YVID_BACKEND_MF) mv->info.hw = (yvid_hw)((const yvid__mf*)mv->be_mem)->hw;
#endif
    /* the display against the rate */
    if (yvid__display_rate(mv, &rn, &rd)) {
        double R = (double)rn / (double)rd, r = (double)c.fps_num / (double)c.fps_den;
        double ratio = R / r;
        int32_t k = (int32_t)floor(ratio + 0.5);
        mv->info.refresh_num = rn; mv->info.refresh_den = rd;
        mv->info.per_frame = ratio;
        if (k >= 1 && fabs(ratio / k - 1.0) <= 200e-6) { mv->info.multiple = k; mv->info.err_ppm = (ratio / k - 1.0) * 1e6; }
        else if (desc->strict_cadence) {
            yvid__err(mv, "ysp_video: %s is %d/%d fps; the display runs %.4f Hz, which is not a multiple (%.4f display frames per frame). "
                        "Re-encode at a rate it divides, choose a mode with yscr_mode_multiple(), or leave desc.strict_cadence off to accept the cadence",
                        desc->path ? desc->path : "the movie", (int)c.fps_num, (int)c.fps_den, R, ratio);
            yvid__close_partial(mv);
            return false;
        }
    }
    /* slots and scratch */
    mv->info.ahead = desc->ahead > 0 ? desc->ahead : 6;
    mv->n_slots = mv->info.ahead + 2;
    /* the GPU path's frames are in textures: no CPU slot memory */
    mv->info.slots = mv->n_slots;
    /* a slot holds what is uploaded: the planes, tight, or RGBA8 */
    mv->info.slot_bytes = mv->info.path == YVID_PATH_GPU ? 0
                        : yvid__is_yuv(mv->info.upload_format) ? yvid__planes_layout(c.format, c.w, c.h, NULL, NULL)
                        : (size_t)c.w * (size_t)c.h * (size_t)yvid__bpt(mv->info.upload_format);
    {
        size_t stride = (mv->info.slot_bytes + 4095) / 4096 * 4096;
        size_t need = stride * (size_t)mv->n_slots + 4096;
        unsigned char* base;
        if (desc->mem) {
            if (desc->mem_bytes < need) { yvid__err(mv, "ysp_video: desc.mem holds %zu bytes; %zu needed", desc->mem_bytes, need); yvid__close_partial(mv); return false; }
            base = (unsigned char*)desc->mem;
        } else {
            mv->mem_raw = (unsigned char*)yvid__malloc(need);
            if (!mv->mem_raw) { yvid__err(mv, "ysp_video: out of memory for %zu bytes of slots", need); yvid__close_partial(mv); return false; }
            base = mv->mem_raw;
        }
        base = (unsigned char*)(((uintptr_t)base + 4095) & ~(uintptr_t)4095);
        for (i = 0; i < mv->n_slots; i++) {
            mv->slots[i].data = base + stride * (size_t)i;
            mv->slots[i].g = -1;
        }
    }
    if (g) {
        ygfx_texture_desc td;
        memset(&td, 0, sizeof td);
        td.w = mv->gpu_w > 0 ? mv->gpu_w : c.w;   /* GPU: the surface's size; the stim shows the frame */
        td.h = mv->gpu_h > 0 ? mv->gpu_h : c.h;
        td.format = (ygfx_format)mv->info.upload_format;
        if (yvid__is_yuv(mv->info.upload_format)) {
            /* ysp/gfx.h's video program: the canonical matrix, range and
             * siting; the codes as device values (CODES) or the stated
             * transfer and primaries to linear light (EOTF) */
            int eotf = mv->info.light == YVID_LIGHT_EOTF;
            td.format = c.format == YVID_FMT_NV12 ? YGFX_NV12 : YGFX_I420;
            td.enc.matrix = c.matrix; td.enc.range = c.range; td.enc.siting = c.siting;
            td.enc.transfer = (uint8_t)(eotf ? c.transfer : YGFX_TRC_DEVICE);
            td.enc.primaries = (uint8_t)(eotf ? c.primaries : YGFX_PRIM_DEVICE);
            td.enc.chroma_nearest = desc->chroma == YVID_CHROMA_NEAREST;
        }
        mv->tex = ygfx_texture(g, &td);
        if (mv->tex.id == 0) { yvid__err(mv, "ysp_video: ysp/gfx.h refused the texture: %s", ygfx_error(g)); yvid__close_partial(mv); return false; }
#if YVID__MF
        if (mv->info.path == YVID_PATH_GPU) {
            /* the slots' textures, made and imported once: nothing per frame */
            yvid__mf* m = (yvid__mf*)mv->be_mem;
            mv->gpu = 1;
            if (yvid__mf_textures(m, mv) < 0) { yvid__err(mv, "ysp_video: YVID_PATH_GPU: CreateTexture2D NV12 %dx%d (0x%08lx)", (int)m->tw, (int)m->th, (unsigned long)m->last_hr); yvid__close_partial(mv); return false; }
            for (i = 0; i < mv->n_slots; i++) {
                ygfx_import_desc d;
                memset(&d, 0, sizeof d);
                d.kind = YGFX_IMPORT_D3D11;
                d.w = m->tw; d.h = m->th;
                d.format = YGFX_NV12;
                d.enc = td.enc;
                d.d3d11_tex = mv->slots[i].gtex;
                mv->slots[i].gimp = ygfx_texture_import(g, &d);
                if (!mv->slots[i].gimp.id) { yvid__err(mv, "ysp_video: YVID_PATH_GPU: ysp/gfx.h refused the texture: %s", ygfx_error(g)); yvid__close_partial(mv); return false; }
            }
        }
#endif
    }
    for (i = 0; i < mv->n_slots; i++) yvid__free_push(mv, (uint32_t)i);
    mv->dt_spare = -1;
    mv->dt_pos = -1;
    mv->dt_key = -1;
    mv->shown_g = -1;
    mv->rate_num = 1; mv->rate_den = 1;
    mv->late_from = INT64_MAX;
    mv->next_id = 1;
    mv->state = YVID__STOPPED;
    /* On ysp/rt.h's virtual clock (v0.4.0) the frame loop never waits, so
     * a decode thread on real time fell behind it: shown 6, dropped 54 of
     * 60 frames in video_play --sim, a different count each run. */
    mv->inline_mode = desc->inline_decode || yrt_is_virtual() ? 1 : 0;
#if defined(YRT_NO_THREADS)
    mv->inline_mode = 1;
#else
    if (!mv->inline_mode) {
        yrt_pump_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.msg_size = sizeof(yvid__msg);
        pd.capacity = 32;
        pd.on_msg = yvid__on_msg;
        pd.on_idle = yvid__on_idle;
        pd.ctx = mv;
        pd.on_start = yvid__on_start;
        pd.pin_cpu = desc->pin_cpu;
        if (!yrt_pump_open(&mv->pump, &pd)) {
            /* A target without threads (wasm without -pthread) still plays,
             * decoding inside yvid_update(); describe() says so. */
            yvid__fmt(mv->inline_why, sizeof mv->inline_why, "%s", yrt_pump_error(&mv->pump));
            mv->inline_mode = 1;
        }
    }
#endif
    mv->open = 1;
    {
        yrt_payload u;
        memset(&u, 0, sizeof u);
        u.i32[0] = c.w; u.i32[1] = c.h; u.i32[2] = c.format; u.i32[3] = c.fps_num; u.i32[4] = c.fps_den;
        u.i32[5] = c.codec; u.i32[6] = backend; u.i32[7] = mv->info.multiple;
        u.u32[8] = (uint32_t)c.matrix | (uint32_t)c.range << 8 | (uint32_t)c.transfer << 16 | (uint32_t)c.primaries << 24;
        u.i32[9] = mv->info.ahead;
        yvid__push(mv, (uint16_t)YVID_EV_OPEN, YVID__NOW(), &u);
    }
    return true;
}

YVID_API void yvid_close(yvid_movie* mv) {
    int i;
    if (!mv || !mv->open) return;
    /* records whose flips never reported go out as they are, PENDING */
    for (i = 0; i < mv->n_pend; i++) yvid__record_push(mv, &mv->pend[i]);
    mv->n_pend = 0;
    yvid__close_partial(mv);
    mv->open = 0;
}

YVID_API const char* yvid_error(const yvid_movie* mv) { return mv ? mv->error : ""; }
YVID_API bool yvid_is_open(const yvid_movie* mv) { return mv && mv->open; }
YVID_API ygfx_tex yvid_texture(const yvid_movie* mv) { ygfx_tex t; t.id = mv && mv->open ? mv->tex.id : 0; return t; }

YVID_API void yvid_get_info(const yvid_movie* mv, yvid_info* out) {
    if (!out) return;
    if (!mv || !mv->open) { memset(out, 0, sizeof *out); return; }
    *out = mv->info;
}

YVID_API ygfx_stim yvid_stim(const yvid_movie* mv, const yvid_stim_desc* d) {
    ygfx_image_desc id;
    memset(&id, 0, sizeof id);
    if (d) {
        id.place = d->place; id.anchor = d->anchor; id.x = d->x; id.y = d->y; id.w = d->w; id.h = d->h;
        id.ori = d->ori; id.opacity = d->opacity; id.linear = d->linear; id.group = d->group;
    }
    id.tex = yvid_texture(mv);
    if (mv && mv->open && mv->gpu_w > 0) {
        /* the GPU path's texture is the decoder's surface size (1088 rows for
         * 1080): the stim shows the visible frame */
        id.src[0] = (float)mv->gpu_ax; id.src[1] = (float)mv->gpu_ay;
        id.src[2] = (float)mv->info.w; id.src[3] = (float)mv->info.h;
        if (id.w == 0.0f && id.h == 0.0f) { id.w = (float)mv->info.w; id.h = (float)mv->info.h; }
    }
    return ygfx_image(mv ? mv->gfx : NULL, &id);
}

/* --- controls ------------------------------------------------------------------- */

static int64_t yvid__play_at_body(yvid_movie* mv, int64_t t) {
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (mv->state == YVID__FAILED) return YVID_ERR_DECODER;
    if (mv->state == YVID__ENDED || mv->state == YVID__PLAYING) return YVID_ERR_ORDER;
    if (mv->state == YVID__SEEKING) {
        mv->seek_resume_kind = t == YVID_ASAP ? 0 : 1;
        mv->seek_resume_t = t;
        return mv->seek_id;
    }
    mv->ctl.op = YVID__OP_PLAY;
    mv->ctl.t = t;
    mv->ctl.id = mv->next_id++;
    mv->ctl.active = 1;
    mv->snd_ctl = 0;
    return mv->ctl.id;
}

YVID_API int64_t yvid_play_at(yvid_movie* mv, int64_t t) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return yvid__done(e, c0, yvid__play_at_body(mv, t), "play_at");
}

static int64_t yvid__pause_at_body(yvid_movie* mv, int64_t t) {
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (mv->state != YVID__PLAYING) return YVID_ERR_ORDER;
    mv->ctl.op = YVID__OP_PAUSE;
    mv->ctl.t = t;
    mv->ctl.id = mv->next_id++;
    mv->ctl.active = 1;
    mv->snd_ctl = 0;
    return mv->ctl.id;
}

YVID_API int64_t yvid_pause_at(yvid_movie* mv, int64_t t) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return yvid__done(e, c0, yvid__pause_at_body(mv, t), "pause_at");
}

static int64_t yvid__seek_frame_body(yvid_movie* mv, int64_t frame, int64_t resume_at) {
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (mv->state == YVID__FAILED) return YVID_ERR_DECODER;
    if (frame < 0 || frame >= mv->info.frames) return YVID_ERR_ARG;
    mv->ctl.active = 0;
    mv->state = YVID__SEEKING;
    mv->seek_g = frame;
    mv->seek_id = mv->next_id++;
    mv->seek_req_onset = 0;
    mv->seek_resume_kind = resume_at == YVID_ASAP ? 0 : resume_at == YVID_STAY_PAUSED ? 2 : 1;
    mv->seek_resume_t = resume_at;
    mv->ft_epoch++;
    mv->seek_posted = yvid__post(mv, YVID__MSG_SEEK, frame) == YVID_OK;
    yvid__st64(&mv->want_g, frame);
    /* the sound stops now and refills from the target (a seek lands in
     * cycle 0) */
    yvid__snd_rearm(mv, 0, yvid__ft(mv, frame), 0, mv->seek_id);
    /* the clock stops at the next onset (update), which is the first time
     * this thread knows */
    return mv->seek_id;
}

YVID_API int64_t yvid_seek_frame(yvid_movie* mv, int64_t frame, int64_t resume_at) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return yvid__done(e, c0, yvid__seek_frame_body(mv, frame, resume_at), "seek_frame");
}

static int64_t yvid__seek_body(yvid_movie* mv, int64_t movie_t, int64_t resume_at) {
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (movie_t < 0 || movie_t >= mv->info.duration) return YVID_ERR_ARG;
    return yvid_seek_frame(mv, yvid__dueg(mv, movie_t, 0), resume_at);
}

YVID_API int64_t yvid_seek(yvid_movie* mv, int64_t movie_t, int64_t resume_at) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return yvid__done(e, c0, yvid__seek_body(mv, movie_t, resume_at), "seek");
}

static int yvid__show_body(yvid_movie* mv, int64_t frame) {
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (mv->state == YVID__FAILED) return YVID_ERR_DECODER;
    if (frame < 0 || frame >= mv->info.frames) return YVID_ERR_ARG;
    mv->ctl.active = 0;
    yvid__snd_halt(mv, 0, 0);   /* manual mode has no sound */
    if (mv->state != YVID__MANUAL) {
        mv->state = YVID__MANUAL;
        if (mv->shown_g >= 0) mv->shown_g %= mv->info.frames;
        mv->cycle = 0;
    }
    mv->manual_g = frame;
    mv->manual_req = 1;
    return YVID_OK;
}

YVID_API int yvid_show(yvid_movie* mv, int64_t frame) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__show_body(mv, frame), "show");
}

YVID_API int yvid_result(const yvid_movie* mv, int64_t id, yrt_event* out) {
    int i, n;
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    n = mv->n_results < YVID__RESULTS ? mv->n_results : YVID__RESULTS;
    for (i = 0; i < n; i++) {
        const yvid__result* r = &mv->results[i];
        if (r->id == id) { if (out) *out = r->ev; return YVID_OK; }
    }
    return id > 0 && id < mv->next_id ? YVID_PENDING : YVID_ERR_NOT_FOUND;
}

YVID_API int64_t yvid_movie_time(const yvid_movie* mv, int64_t t) {
    if (!mv || !mv->open) return 0;
    return yvid__movie_time(mv, t) + mv->cycle * mv->info.duration;
}

/* --- records --------------------------------------------------------------------- */

static void yvid__record_push(yvid_movie* mv, const yvid_record* r) {
    yrt_payload u;
    memset(&u, 0, sizeof u);
    u.i64[0] = r->frame; u.i64[1] = r->due; u.i64[2] = r->movie_t;
    u.u32[6] = r->flags;
    u.u16[14] = (uint16_t)(r->decision | r->why << 2 | (r->tier & 7) << 6 | (r->path & 3) << 9);
    u.u16[15] = r->ahead;
    u.u32[8] = r->shows;
    u.u32[9] = (uint32_t)r->display;
    yvid__push(mv, (uint16_t)YVID_EV_FRAME, r->onset ? r->onset : r->predicted, &u);
}

static void yvid__complete(yvid_movie* mv, yvid_record* r, const yscr_record* fr) {
    r->flags = (uint16_t)(r->flags & ~YVID_F_PENDING);
    if (fr) {
        r->tier = fr->tier;
        if (fr->onset) r->onset = fr->onset;
        else r->flags |= YVID_F_NOT_SHOWN;
        if (fr->dropped > 0) r->flags |= YVID_F_LATE;
        if (fr->flags & (YSCR_FLIP_ESTIMATED | YSCR_FLIP_ONSET_PLANNED)) r->flags |= YVID_F_ESTIMATED;
    } else {
        r->flags |= YVID_F_ESTIMATED;
    }
    if (r->tier > mv->info.worst_tier) mv->info.worst_tier = r->tier;
    if (mv->d.min_tier > 0 && r->tier > mv->d.min_tier) r->flags |= YVID_F_BELOW_TIER;
    if (mv->has_last && mv->last.display == r->display) mv->last = *r;
    yvid__record_push(mv, r);
}

YVID_API void yvid_flip_done(yvid_movie* mv, const yscr_record* fr) {
    int i, j;
    if (!mv || !mv->open || !fr) return;
    for (i = 0; i < mv->n_pend; ) {
        yvid_record* r = &mv->pend[i];
        if (r->display <= fr->index) {
            yvid__complete(mv, r, r->display == fr->index ? fr : NULL);
            for (j = i + 1; j < mv->n_pend; j++) mv->pend[j - 1] = mv->pend[j];
            mv->n_pend--;
        } else {
            i++;
        }
    }
}

YVID_API int yvid_last(const yvid_movie* mv, yvid_record* out) {
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (!mv->has_last) return YVID_ERR_NOT_FOUND;
    if (out) *out = mv->last;
    return YVID_OK;
}

/* The movie time of global frame g, counting whole loops. */
static int64_t yvid__gt(const yvid_movie* mv, int64_t g) {
    return (g / mv->info.frames) * mv->info.duration + yvid__ft(mv, g % mv->info.frames);
}

/* mg: the global movie time at the onset. */
static void yvid__drop(yvid_movie* mv, int64_t first, int64_t count, int why, int64_t mg, int64_t onset, int64_t display) {
    yrt_payload u;
    if (count <= 0) return;
    mv->info.dropped += (uint64_t)count;
    mv->info.drops_by[why] += (uint64_t)count;
    memset(&u, 0, sizeof u);
    u.i64[0] = first % mv->info.frames;
    u.i64[1] = count;
    u.u16[14] = (uint16_t)why;
    u.u32[9] = (uint32_t)display;
    yvid__push(mv, (uint16_t)YVID_EV_DROP, onset - (mg - yvid__gt(mv, first)), &u);
}

/* A run j+1 .. rr-1 never shown: DECODE_LATE for the frames that were due
 * before they were ready, then by the vblank gap, the nominal schedule, or
 * a slip. */
static void yvid__drop_run(yvid_movie* mv, int64_t first, int64_t end, int64_t gap, int nominal,
                             int64_t mg, int64_t onset, int64_t display) {
    int64_t n = end - first, late = 0;
    if (n <= 0) return;
    if (mv->late_from < end) {
        late = end - (mv->late_from > first ? mv->late_from : first);
        if (late > n) late = n;
    }
    if (n - late > 0) {
        int w = gap > 1 ? YVID_WHY_DISPLAY_LATE
              : (nominal && mv->info.per_frame > 0 && mv->info.per_frame < 1.0) ? YVID_WHY_CADENCE
              : YVID_WHY_DRIFT;
        yvid__drop(mv, first, n - late, w, mg, onset, display);
    }
    if (late > 0) yvid__drop(mv, end - late, late, YVID_WHY_DECODE_LATE, mg, onset, display);
}

/* --- the frame ---------------------------------------------------------------------- */

/* Takes the newest ready slot of this epoch whose frame is not later than
 * `upto` and after `after`; frees the stale and the passed-over ones.
 * Returns the slot or -1. */
static int yvid__take(yvid_movie* mv, int64_t upto, int64_t after, int exact) {
    int best = -1;
    for (;;) {
        uint32_t h = mv->ready_head;
        int s;
        yvid__slot* sl;
        if (h == yvid__ld32(&mv->ready_tail)) break;
        s = (int)mv->ready_q[h & (YVID__QCAP - 1)];
        sl = &mv->slots[s];
        if (sl->epoch != mv->ft_epoch || sl->g <= after) {
            mv->ready_head = h + 1;
            yvid__free_push(mv, (uint32_t)s);
            continue;
        }
        if (sl->g > upto) break;
        if (exact && sl->g != upto) {   /* manual: only the frame asked for */
            mv->ready_head = h + 1;
            yvid__free_push(mv, (uint32_t)s);
            continue;
        }
        mv->ready_head = h + 1;
        if (best >= 0) yvid__free_push(mv, (uint32_t)best);
        best = s;
    }
    return best;
}

static uint32_t yvid__ready_count(const yvid_movie* mv) {
    return yvid__ld32(&mv->ready_tail) - mv->ready_head;
}

/* Frames ready from `from` on, consecutive, in this epoch. */
static int64_t yvid__ready_from(const yvid_movie* mv, int64_t from) {
    uint32_t h = mv->ready_head, t = yvid__ld32(&mv->ready_tail);
    int64_t n = 0, expect = from;
    for (; h != t; h++) {
        const yvid__slot* sl = &mv->slots[mv->ready_q[h & (YVID__QCAP - 1)]];
        if (sl->epoch != mv->ft_epoch || sl->g < expect) continue;
        if (sl->g != expect) break;
        n++;
        expect++;
    }
    return n;
}

/* Frees the slots of an older epoch at the head of the ready queue: after
 * a seek they come before every frame of the new one. */
static void yvid__purge(yvid_movie* mv) {
    for (;;) {
        uint32_t h = mv->ready_head;
        int s;
        if (h == yvid__ld32(&mv->ready_tail)) return;
        s = (int)mv->ready_q[h & (YVID__QCAP - 1)];
        if (mv->slots[s].epoch == mv->ft_epoch) return;
        mv->ready_head = h + 1;
        yvid__free_push(mv, (uint32_t)s);
    }
}

static int yvid__upload(yvid_movie* mv, int s) {
    int rc = YVID_OK;
    if (mv->gfx && mv->gpu) {
        /* the slot's texture is shown; the one shown before goes back to the
         * decoder. Its last draw is already on the device's one immediate
         * context, so a copy into it later runs after that draw */
        int64_t t0 = YVID__COST_NOW();
        int old = mv->gpu_shown, g;
        YRT_ZONE(zg, "yvid.rebind");
        g = ygfx_texture_rebind(mv->gfx, mv->tex, mv->slots[s].gimp);
        YRT_ZONE_END(zg);
        mv->gpu_shown = s;
        if (old >= 0 && old != s) yvid__free_push(mv, (uint32_t)old);
        mv->upload_ns_last = (uint64_t)(YVID__COST_NOW() - t0);
        if (mv->upload_ns_last > mv->upload_ns_max) mv->upload_ns_max = mv->upload_ns_last;
        return g == YGFX_ERR_ORDER ? YVID_ERR_ORDER : g < 0 ? YVID_ERR_ARG : YVID_OK;
    }
    if (mv->gfx) {
        int64_t t0 = YVID__COST_NOW();
        int g;
        YRT_ZONE(zu, "yvid.upload");
        if (yvid__is_yuv(mv->info.upload_format)) {
            yvid_planes pl;
            ygfx_planes gp;
            int k;
            yvid__planes_layout(mv->info.format, mv->info.w, mv->info.h, mv->slots[s].data, &pl);
            memset(&gp, 0, sizeof gp);
            for (k = 0; k < 3; k++) { gp.data[k] = pl.data[k]; gp.stride[k] = (size_t)pl.stride[k]; }
            g = ygfx_texture_update_planes(mv->gfx, mv->tex, &gp);
        } else {
            g = ygfx_texture_update(mv->gfx, mv->tex, 0, 0, mv->info.w, mv->info.h, mv->slots[s].data,
                                      (size_t)mv->info.w * (size_t)yvid__bpt(mv->info.upload_format));
        }
        YRT_ZONE_END(zu);
        mv->upload_ns_last = (uint64_t)(YVID__COST_NOW() - t0);
        if (mv->upload_ns_last > mv->upload_ns_max) mv->upload_ns_max = mv->upload_ns_last;
        if (g == YGFX_ERR_ORDER) rc = YVID_ERR_ORDER;
        else if (g == YGFX_ERR_LOST) rc = YVID_ERR_LOST;
        else if (g < 0) rc = YVID_ERR_ARG;
    }
    return rc;
}

static void yvid__pend_add(yvid_movie* mv, const yvid_record* r) {
    if (mv->n_pend == YVID__MAX_PEND) {
        /* a caller that never flips: the oldest goes out as it is */
        yvid__complete(mv, &mv->pend[0], NULL);
        memmove(&mv->pend[0], &mv->pend[1], sizeof(yvid_record) * (YVID__MAX_PEND - 1));
        mv->n_pend--;
    }
    mv->pend[mv->n_pend++] = *r;
    mv->last = *r;
    mv->has_last = 1;
}

/* The global frame due at global movie time mg. */
static int64_t yvid__due_global(const yvid_movie* mv, int64_t mg, int64_t lead_ns) {
    int64_t y = mg + lead_ns, D = mv->info.duration, q = yvid__floordiv(y, D);
    return q * mv->info.frames + yvid__dueg(mv, y - q * D, 0);
}

/* The nominal due frame: where the frame would be if the movie clock ran in
 * step with the vblank count at the nominal period (T / k at a multiple k,
 * else the mode's period) since the anchor or the last slip. A difference
 * is a slip; the schedule then starts again from the clock's phase, which
 * is where the frames now land. */
static int64_t yvid__nominal(const yvid_movie* mv, const yscr_frame* f, int64_t lead_ns) {
    int64_t dv = f->vblank - mv->nom_vb, m;
    if (mv->rate_num != mv->rate_den && mv->info.multiple <= 0) {
        /* base ns per vblank: the mode's period at the base's rate */
        int64_t rn = (int64_t)mv->info.refresh_den * mv->rate_num, rd = (int64_t)mv->info.refresh_num * mv->rate_den;
        if (mv->info.refresh_num > 0 && rn < ((int64_t)1 << 31) && rd < ((int64_t)1 << 31))
            m = mv->nom_m + yvid__floordiv(dv * rn, rd) * YVID__NS + yvid__floordiv((dv * rn - yvid__floordiv(dv * rn, rd) * rd) * YVID__NS, rd);
        else
            m = mv->nom_m + yvid__scale(dv * f->period, mv->rate_num, mv->rate_den);
    } else if (mv->info.multiple > 0) {
        int64_t q = (int64_t)mv->info.fps_num * mv->info.multiple;
        m = mv->nom_m + yvid__floordiv(dv * mv->info.fps_den * YVID__NS, q);
    } else if (mv->info.refresh_num > 0) {
        m = mv->nom_m + yvid__floordiv(dv * mv->info.refresh_den * YVID__NS, mv->info.refresh_num);
    } else {
        m = mv->nom_m + dv * f->period;
    }
    return yvid__due_global(mv, m, lead_ns);
}

/* The cadence at the base's rate: R against r x num / den (R7). Returns the
 * multiple, 0 for a cadence that judders. */
static int32_t yvid__cadence(yvid_movie* mv) {
    double R, r, ratio;
    int32_t k;
    if (mv->info.refresh_num <= 0 || mv->info.refresh_den <= 0) return 0;
    R = (double)mv->info.refresh_num / mv->info.refresh_den;
    r = (double)mv->info.fps_num / mv->info.fps_den * ((double)mv->rate_num / mv->rate_den);
    ratio = R / r;
    k = (int32_t)floor(ratio + 0.5);
    mv->info.per_frame = ratio;
    if (k >= 1 && fabs(ratio / k - 1.0) <= 200e-6) { mv->info.err_ppm = (ratio / k - 1.0) * 1e6; return k; }
    mv->info.err_ppm = 0.0;
    return 0;
}

/* strict_cadence refuses a rate whose cadence judders: the movie holds its
 * frame and yvid_update() returns YVID_ERR_REFUSED until the rate gives
 * a multiple again (a seek lands, then holds; close ends it). */
static int yvid__rate_refused(const yvid_movie* mv) {
    return mv->d.strict_cadence && mv->rate_num != mv->rate_den && mv->info.multiple == 0 && mv->info.refresh_num > 0;
}

/* A new base rate, seen at frame f: the cadence again, the nominal schedule
 * from this frame, and a RATE record (t onset, i32[0] num, i32[1] den,
 * i32[2] the multiple, f64[2] display frames per video frame, u32[8] 1 when
 * strict_cadence refuses it). */
static void yvid__rate_change(yvid_movie* mv, const yscr_frame* f, int64_t num, int64_t den) {
    yrt_payload u;
    mv->rate_num = (int32_t)num;
    mv->rate_den = (int32_t)den;
    mv->info.multiple = yvid__cadence(mv);
    mv->nom_resync = 1;
    memset(&u, 0, sizeof u);
    u.i32[0] = (int32_t)num; u.i32[1] = (int32_t)den; u.i32[2] = mv->info.multiple;
    u.f64[2] = mv->info.per_frame;
    u.u32[8] = (uint32_t)yvid__rate_refused(mv);
    yvid__push(mv, (uint16_t)YVID_EV_RATE, f->onset, &u);
    if (yvid__rate_refused(mv))
        yvid__err(mv, "ysp_video: the movie base runs at %lld/%lld: %.4f display frames per frame, not a multiple, and desc.strict_cadence refuses judder; the frame holds until the rate gives a multiple",
                    (long long)num, (long long)den, mv->info.per_frame);
}

/* The schedule starts again at an anchor: offset 0 there. */
static void yvid__nom_sync(yvid_movie* mv, const yscr_frame* f, int64_t m_global) {
    mv->nom_m = m_global;
    mv->nom_vb = f->vblank;
    mv->nom_d = 0;
    mv->nom_set = 1;
}

/* Shows (or repeats) on this display frame; i is the due global frame. */
static int yvid__decide(yvid_movie* mv, const yscr_frame* f, int64_t i, int64_t m, int64_t lead_ns) {
    const int64_t N = mv->info.frames;
    int64_t j = mv->shown_g, mg = m + mv->cycle * mv->info.duration;
    int64_t e = mv->nom_set && f->period > 0 ? yvid__nominal(mv, f, lead_ns) : i;
    int64_t d = i - e, slip = d - mv->nom_d;
    int64_t gap = mv->has_display ? f->vblank - mv->last_vblank : 1;
    int s, rc = YVID_OK;
    yvid_record r;
    memset(&r, 0, sizeof r);
    r.display = f->index;
    r.movie_t = m;
    r.predicted = f->onset;
    r.flags = YVID_F_PENDING | (mv->following ? YVID_F_AUDIO_CLOCK : 0);
    r.path = (uint8_t)mv->info.path;
    yvid__st64(&mv->want_g, i);
    s = yvid__take(mv, i, j, 0);
    if (s >= 0) {
        int64_t rr = mv->slots[s].g;
        int why;
        if (rr < i) { why = YVID_WHY_DECODE_LATE; if (mv->late_from > rr + 1) mv->late_from = rr + 1; }
        else if (mv->just) why = mv->just;
        else why = YVID_WHY_DUE;
        if (j >= 0 && !mv->just) {
            /* the run j+1 .. rr-1 that was never shown */
            int64_t first = j + 1, n = rr - first, late = 0;
            if (n > 0 && mv->late_from < rr) {
                late = rr - (mv->late_from > first ? mv->late_from : first);
                if (late > n) late = n;
            }
            if (n - late > 0) {
                int64_t rest = n - late;
                if (gap > 1) {
                    yvid__drop(mv, first, rest, YVID_WHY_DISPLAY_LATE, mg, f->onset, f->index);
                } else {
                    int64_t dr = slip > 0 ? (slip < rest ? slip : rest) : 0;
                    yvid__drop(mv, first, rest - dr, YVID_WHY_CADENCE, mg, f->onset, f->index);
                    yvid__drop(mv, first + rest - dr, dr, YVID_WHY_DRIFT, mg, f->onset, f->index);
                    slip -= dr;
                }
            }
            if (late > 0) yvid__drop(mv, rr - late, late, YVID_WHY_DECODE_LATE, mg, f->onset, f->index);
        }
        /* a slip with nothing dropped: this frame came a display frame early */
        if (why == YVID_WHY_DUE && slip > 0 && gap <= 1) why = YVID_WHY_DRIFT;
        if (j >= 0 && rr / N > j / N && why == YVID_WHY_DUE) why = YVID_WHY_LOOP;
        if (rr >= i && why != YVID_WHY_DECODE_LATE) mv->late_from = INT64_MAX;
        rc = yvid__upload(mv, s);
        if (mv->slots[s].flags & YVID_F_TS_MISMATCH) mv->info.ts_mismatch++;
        if (mv->slots[s].flags & YVID_F_HASH_MISMATCH) mv->info.hash_mismatch++;
        r.flags |= (uint16_t)mv->slots[s].flags;
        yvid__free_push(mv, (uint32_t)s);
        mv->shown_g = rr;
        mv->shows = 1;
        r.frame = rr % N;
        r.decision = YVID_SHOWN;
        r.why = (uint8_t)why;
        mv->info.shown++;
        if (slip != 0 || d != 0) yvid__nom_sync(mv, f, mg);
        if (j >= 0 && rr / N > j / N) {
            yrt_payload u;
            memset(&u, 0, sizeof u);
            u.i64[0] = rr / N;
            yvid__push(mv, (uint16_t)YVID_EV_LOOP, f->onset, &u);
        }
    } else if (j < 0) {
        /* nothing on screen yet and nothing ready: no record */
        if (mv->late_from > i) mv->late_from = i;
        return YVID_OK;
    } else {
        int why;
        if (i > j) { why = YVID_WHY_DECODE_LATE; if (mv->late_from > j + 1) mv->late_from = j + 1; }
        else if (i < j || slip < 0) why = YVID_WHY_DRIFT;
        else {
            int32_t q = (int32_t)floor(mv->info.per_frame);
            why = (mv->info.multiple > 0 || (int64_t)mv->shows < q) ? YVID_WHY_DUE : YVID_WHY_CADENCE;
        }
        mv->shows++;
        r.frame = j % N;
        r.decision = YVID_REPEATED;
        r.why = (uint8_t)why;
        mv->info.repeated++;
        mv->info.repeats_by[why]++;
        if (d != 0) yvid__nom_sync(mv, f, mg);
    }
    mv->just = 0;
    r.shows = mv->shows;
    r.ahead = (uint16_t)yvid__ready_count(mv);
    r.due = f->onset - (mg - yvid__gt(mv, mv->shown_g));
    if (mv->rate_num != mv->rate_den && mv->d.timeline) {
        /* the RT time the base reached the frame's time (R5): a movie-time
         * difference is not RT ns at another rate */
        int64_t rt;
        if (ytl_rt_time(mv->d.timeline, mv->d.base, yvid__gt(mv, mv->shown_g) - mv->cycle * mv->info.duration, &rt)) r.due = rt;
    }
    yvid__pend_add(mv, &r);
    return rc;
}

static void yvid__inline_decode(yvid_movie* mv) {
    int n = 0;
    if (!mv->inline_mode) return;
    if (mv->snd) while (mv->snd_ops->step(mv->snd)) {}
    while (yvid__dstep(mv)) {
        if (mv->d.inline_budget > 0 && ++n >= mv->d.inline_budget) break;
    }
}

/* A decoder failure ends the movie once the frames it made before are
 * used up: they are good frames. */
static int yvid__fail_check(yvid_movie* mv) {
    /* a failed movie says why at each call (v0.3: a failing call writes
     * its message) */
    if (mv->state == YVID__FAILED) { yvid__fmt(mv->error, sizeof mv->error, "%s", mv->dt_error); return YVID_ERR_DECODER; }
    if (yvid__ld32(&mv->dec_err) && yvid__ready_count(mv) == 0) {
        if (mv->state != YVID__FAILED) {
            yvid__fmt(mv->error, sizeof mv->error, "%s", mv->dt_error);
            mv->state = YVID__FAILED;
        }
        return YVID_ERR_DECODER;
    }
    return YVID_OK;
}

static int yvid__update_body(yvid_movie* mv, const yscr_frame* f) {
    int64_t lead_ns, m, i;
    int rc = YVID_OK, refused = 0;
    YRT_ZONE(z, "yvid.update");
    if (!mv || !mv->open) { YRT_ZONE_END(z); return YVID_ERR_CLOSED; }
    if (!f) { YRT_ZONE_END(z); return YVID_ERR_ARG; }
    if (mv->has_display && f->index <= mv->last_display) { YRT_ZONE_END(z); return YVID_ERR_ORDER; }
    /* every flip completed since the last begin; a frame filled by hand
     * may carry only the newest */
    if (f->done && f->n_done > 0) {
        int k;
        for (k = 0; k < f->n_done; k++) yvid_flip_done(mv, &f->done[k]);
    } else if (f->last) {
        yvid_flip_done(mv, f->last);
    }
    lead_ns = yvid_lead_ns(mv->info.lead == 0.0 ? -1.0 : mv->info.lead, f->period);
    mv->lead_ns_last = lead_ns;
    mv->period_last = f->period;
    yvid__purge(mv);
    if (mv->snd) {
        int64_t rn, rd;
        /* one queued wake at a time: the feeder clears it when it runs */
        if (yvid__ld32(&mv->snd_wake) == 0 && mv->snd_ops->wants(mv->snd)) {
            yvid__st32(&mv->snd_wake, 1);
            if (yvid__post(mv, YVID__MSG_WAKE, 0) != YVID_OK) yvid__st32(&mv->snd_wake, 0);
        }
        /* no resampling: a soundtrack plays at base rate 1 or not at all */
        yvid__base_rate(mv, &rn, &rd);
        if (rn != rd && mv->snd_phase == 2) {
            yvid__snd_halt(mv, 0, 0);
            yvid__err(mv, "ysp_video: the movie base runs at %lld/%lld; the soundtrack stopped (it plays at rate 1 only: no resampling)",
                        (long long)rn, (long long)rd);
            refused = 1;
        }
    }
    if (mv->d.timeline) {
        int64_t rn, rd;
        yvid__base_rate(mv, &rn, &rd);
        if (rn != mv->rate_num || rd != mv->rate_den) yvid__rate_change(mv, f, rn, rd);
    }
    /* the decoder learns this frame's due before it decodes (inline: now) */
    if (mv->state == YVID__PLAYING)
        yvid__st64(&mv->want_g, mv->cycle * mv->info.frames + yvid__dueg(mv, yvid__window_mt(mv, f->onset, f->period, lead_ns), 0));
    else if (mv->state == YVID__MANUAL)
        yvid__st64(&mv->want_g, mv->manual_g);
    yvid__inline_decode(mv);
    if (yvid__fail_check(mv) < 0) { mv->has_display = 1; mv->last_display = f->index; mv->last_vblank = f->vblank; YRT_ZONE_END(z); return YVID_ERR_DECODER; }

    /* a seek: land, then resume */
    if (mv->state == YVID__SEEKING) {
        if (!mv->seek_posted) mv->seek_posted = yvid__post(mv, YVID__MSG_SEEK, mv->seek_g) == YVID_OK;
        if (!mv->seek_req_onset) mv->seek_req_onset = f->onset;
        if (mv->running) yvid__pause_clock(mv, f->onset);
        {
            int64_t need = mv->d.preroll > 0 ? mv->d.preroll : mv->info.ahead;
            int64_t avail = yvid__ready_from(mv, mv->seek_g);
            int ready_target = avail > 0;
            int go = 0;
            if (need > mv->info.frames - mv->seek_g && !mv->d.loop) need = mv->info.frames - mv->seek_g;
            if (mv->seek_resume_kind == 2) go = ready_target;
            else if (mv->seek_resume_kind == 0) go = avail >= need;
            else go = ready_target && mv->seek_resume_t <= f->onset + lead_ns;
            if (mv->snd && mv->seek_resume_kind == 0) {
                /* ASAP with sound: the first frame both can reach, once
                 * the frames and the refill are in */
                if (go && mv->snd_ops->ready(mv->snd, mv->snd_want)) {
                    int64_t t = yvid__snd_asap(mv, f);
                    if (yvid__snd_go(mv, t, mv->seek_id)) { mv->seek_resume_kind = 1; mv->seek_resume_t = t; }
                }
                go = mv->seek_resume_kind == 1 && ready_target && mv->seek_resume_t <= f->onset + lead_ns;
            } else if (mv->snd && mv->seek_resume_kind == 1) {
                (void)yvid__snd_go(mv, mv->seek_resume_t, mv->seek_id);   /* LATE if the refill came too late */
            }
            if (go) {
                const yvid__slot* sl = NULL;
                uint32_t h, t = yvid__ld32(&mv->ready_tail);
                yrt_payload u;
                for (h = mv->ready_head; h != t; h++) {
                    const yvid__slot* c = &mv->slots[mv->ready_q[h & (YVID__QCAP - 1)]];
                    if (c->epoch == mv->ft_epoch && c->g == mv->seek_g) { sl = c; break; }
                }
                memset(&u, 0, sizeof u);
                u.i64[0] = mv->seek_g;
                u.i64[1] = sl ? sl->key : -1;
                u.i64[2] = sl ? sl->discarded : 0;
                u.i64[3] = mv->seek_req_onset;
                u.u32[8] = (uint32_t)mv->seek_id;
                mv->shown_g = -1;
                mv->cycle = 0;
                mv->late_from = INT64_MAX;
                mv->just = YVID_WHY_SEEK;
                if (mv->seek_resume_kind == 2) {
                    u.u32[9] = (uint32_t)yvid__hold_at(mv, f->onset, yvid__ft(mv, mv->seek_g), 1);
                    mv->state = YVID__PAUSED;
                    {
                        int s = yvid__take(mv, mv->seek_g, -1, 1);
                        if (s >= 0) {
                            rc = yvid__upload(mv, s);
                            yvid__free_push(mv, (uint32_t)s);
                            mv->shown_g = mv->seek_g;
                            mv->shows = 1;
                        }
                    }
                    mv->just = 0;
                } else {
                    /* with sound, the shared target is the origin of both */
                    u.u32[9] = (uint32_t)yvid__seek_anchor(mv, mv->snd && mv->snd_phase == 2 ? mv->snd_t : f->onset,
                                                             yvid__ft(mv, mv->seek_g));
                    yvid__nom_sync(mv, f, yvid__ft(mv, mv->seek_g));
                    mv->state = YVID__PLAYING;
                }
                yvid__result_put(mv, mv->seek_id, (uint16_t)YVID_EV_SEEK, f->onset, &u);
            }
        }
    }

    /* manual mode */
    if (mv->state == YVID__MANUAL) {
        int64_t g = mv->manual_g;
        yvid__st64(&mv->want_g, g);
        if (mv->running) yvid__pause_clock(mv, f->onset);
        if (g == mv->shown_g) {
            yvid_record r;
            memset(&r, 0, sizeof r);
            mv->shows++;
            r.display = f->index; r.frame = g; r.predicted = f->onset; r.movie_t = yvid__ft(mv, g);
            r.due = f->onset; r.decision = YVID_REPEATED; r.why = YVID_WHY_MANUAL; r.flags = YVID_F_PENDING;
            r.shows = mv->shows; r.ahead = (uint16_t)yvid__ready_count(mv); r.path = (uint8_t)mv->info.path;
            mv->info.repeated++; mv->info.repeats_by[YVID_WHY_MANUAL]++;
            yvid__pend_add(mv, &r);
        } else {
            int s = yvid__take(mv, g, -1, 1);
            yvid_record r;
            memset(&r, 0, sizeof r);
            r.display = f->index; r.predicted = f->onset; r.flags = YVID_F_PENDING; r.path = (uint8_t)mv->info.path;
            if (s >= 0) {
                rc = yvid__upload(mv, s);
                r.flags |= (uint16_t)mv->slots[s].flags;
                yvid__free_push(mv, (uint32_t)s);
                yvid__hold_at(mv, f->onset, yvid__ft(mv, g), g != mv->shown_g + 1);
                mv->shown_g = g;
                mv->shows = 1;
                mv->manual_req = 0;
                r.frame = g; r.movie_t = yvid__ft(mv, g); r.due = f->onset;
                r.decision = YVID_SHOWN; r.why = YVID_WHY_MANUAL;
                mv->info.shown++;
            } else {
                /* not decoded: the decoder seeks there; the frame on screen stays */
                if (mv->manual_req) {
                    mv->ft_epoch++;
                    yvid__purge(mv);
                    yvid__post(mv, YVID__MSG_SEEK, g);
                    mv->manual_req = 0;
                    yvid__inline_decode(mv);
                    s = yvid__take(mv, g, -1, 1);
                    if (s >= 0) {
                        rc = yvid__upload(mv, s);
                        r.flags |= (uint16_t)mv->slots[s].flags;
                        yvid__free_push(mv, (uint32_t)s);
                        yvid__hold_at(mv, f->onset, yvid__ft(mv, g), g != mv->shown_g + 1);
                        mv->shown_g = g; mv->shows = 1;
                        r.frame = g; r.movie_t = yvid__ft(mv, g); r.due = f->onset;
                        r.decision = YVID_SHOWN; r.why = YVID_WHY_MANUAL;
                        mv->info.shown++;
                    }
                }
                if (r.decision == 0) {
                    if (mv->shown_g >= 0) {
                        mv->shows++;
                        r.frame = mv->shown_g; r.movie_t = yvid__ft(mv, mv->shown_g); r.due = f->onset;
                        r.decision = YVID_REPEATED; r.why = YVID_WHY_SEEK;
                        mv->info.repeated++; mv->info.repeats_by[YVID_WHY_SEEK]++;
                    }
                }
            }
            if (r.decision) {
                r.shows = mv->shows;
                r.ahead = (uint16_t)yvid__ready_count(mv);
                yvid__pend_add(mv, &r);
            }
        }
    }

    /* a pending play or pause lands on the frame whose window reaches its time */
    if (mv->ctl.active && mv->snd && !mv->snd_ctl) {
        /* the sound's part of a control, once */
        if (mv->ctl.op == YVID__OP_PAUSE) {
            if (mv->ctl.t == YVID_ASAP) mv->ctl.t = yvid__snd_asap(mv, f);
            yvid__snd_halt(mv, mv->ctl.t, mv->ctl.id);
        } else {
            yvid__snd_rearm(mv, 0, mv->state == YVID__STOPPED ? 0 : mv->anchor_mt + yvid__scale(f->period, mv->rate_num, mv->rate_den),
                              mv->cycle, mv->ctl.id);
        }
        mv->snd_ctl = 1;
    }
    if (mv->ctl.active) {
        int land = mv->ctl.t == YVID_ASAP ? 1 : mv->ctl.t <= f->onset + lead_ns;
        /* A resume starts one display period after the frozen time: the
         * frozen frame was on screen for the pause. */
        int64_t mt0 = mv->state == YVID__STOPPED ? 0 : mv->anchor_mt + yvid__scale(f->period, mv->rate_num, mv->rate_den);
        int64_t lead_b = yvid__scale(lead_ns, mv->rate_num, mv->rate_den);   /* the window's width in base ns */
        if (mv->ctl.op == YVID__OP_PLAY && mv->ctl.t == YVID_ASAP) {
            int64_t need = mv->d.preroll > 0 ? mv->d.preroll : mv->info.ahead;
            int64_t from = yvid__dueg(mv, mt0, lead_b);
            if (mv->state != YVID__STOPPED && from <= mv->shown_g) from = mv->shown_g + 1;
            if (need > mv->info.frames - from && !mv->d.loop) need = mv->info.frames - from;
            if (from < mv->info.frames) {
                yvid__st64(&mv->want_g, from);
                land = yvid__ready_from(mv, from) >= need;
            }
        }
        if (mv->snd && mv->ctl.op == YVID__OP_PLAY && mv->snd_phase == 1) {
            if (mv->ctl.t == YVID_ASAP) {
                /* the first frame both can reach, once both are ready */
                if (land && mv->snd_ops->ready(mv->snd, mv->snd_want)) {
                    int64_t t = yvid__snd_asap(mv, f);
                    if (yvid__snd_go(mv, t, mv->ctl.id)) mv->ctl.t = t;
                }
                land = mv->ctl.t != YVID_ASAP && mv->ctl.t <= f->onset + lead_ns;
            } else {
                (void)yvid__snd_go(mv, mv->ctl.t, mv->ctl.id);   /* LATE if the refill came too late */
            }
        }
        if (land) {
            yrt_payload u;
            int64_t mt;
            memset(&u, 0, sizeof u);
            if (mv->ctl.op == YVID__OP_PLAY) {
                mt = mt0;
                /* with sound, the shared target is the origin of both */
                yvid__anchor(mv, mv->snd && mv->snd_phase == 2 ? mv->snd_t : f->onset, mt);
                yvid__nom_sync(mv, f, mt + mv->cycle * mv->info.duration);
                mv->state = YVID__PLAYING;
                u.i64[0] = mt; u.i64[1] = mv->ctl.id; u.i64[2] = yvid__dueg(mv, mt, lead_b); u.i64[3] = mv->ctl.t;
                yvid__result_put(mv, mv->ctl.id, (uint16_t)YVID_EV_PLAY, f->onset, &u);
            }
            mv->ctl.active = mv->ctl.op == YVID__OP_PAUSE ? 2 : 0;
        }
    }

    if (mv->state == YVID__PLAYING && yvid__rate_refused(mv)) {
        /* strict_cadence at a rate that judders: the frame on screen holds;
         * each refusal says why (v0.3: a failing call writes its message) */
        yvid__err(mv, "ysp_video: the movie base runs at %lld/%lld: %.4f display frames per frame, not a multiple, and desc.strict_cadence refuses judder; the frame holds until the rate gives a multiple",
                    (long long)mv->rate_num, (long long)mv->rate_den, mv->info.per_frame);
        refused = 1;
    } else if (mv->state == YVID__PLAYING) {
        m = yvid__movie_time(mv, f->onset);
        i = yvid__dueg(mv, yvid__window_mt(mv, f->onset, f->period, lead_ns), 0);
        while (i >= 0 && mv->d.loop && i >= mv->info.frames) {
            /* the loop: the base goes back to 0 at t(N); annotations fire
             * again. The wrap's RT time is when the base reached t(N), the
             * inverse of the base's mapping at its rate (R3). */
            int64_t rt_wrap = f->onset - (m - mv->info.duration);
            uint32_t keep = mv->anchor_epoch;
            if (mv->rate_num != mv->rate_den && mv->d.timeline)
                (void)ytl_rt_time(mv->d.timeline, mv->d.base, mv->info.duration, &rt_wrap);
            mv->cycle++;
            yvid__anchor(mv, rt_wrap, 0);
            if (mv->following) mv->anchor_epoch = keep;   /* the other clock runs on */
            m = yvid__movie_time(mv, f->onset);
            i = yvid__dueg(mv, yvid__window_mt(mv, f->onset, f->period, lead_ns), 0);
        }
        if (mv->nom_resync) { yvid__nom_sync(mv, f, m + mv->cycle * mv->info.duration); mv->nom_resync = 0; }
        if (i >= 0) {
            int64_t gi = i + mv->cycle * mv->info.frames;
            if (!mv->d.loop && i >= mv->info.frames) {
                yrt_payload u;
                memset(&u, 0, sizeof u);
                u.i64[0] = mv->shown_g;
                mv->state = YVID__ENDED;
                yvid__push(mv, (uint16_t)YVID_EV_END, f->onset, &u);
                if (mv->shown_g >= 0)
                    yvid__drop_run(mv, mv->shown_g + 1, mv->info.frames, f->vblank - mv->last_vblank, 0, m, f->onset, f->index);
            } else {
                rc = yvid__decide(mv, f, gi, m, yvid__scale(lead_ns, mv->rate_num, mv->rate_den));
            }
        }
        if (mv->ctl.active == 2) {
            yrt_payload u;
            memset(&u, 0, sizeof u);
            yvid__pause_clock(mv, f->onset);
            mv->state = YVID__PAUSED;
            u.i64[0] = mv->anchor_mt; u.i64[1] = mv->ctl.id; u.i64[2] = mv->shown_g % mv->info.frames; u.i64[3] = mv->ctl.t;
            yvid__result_put(mv, mv->ctl.id, (uint16_t)YVID_EV_PAUSE, f->onset, &u);
            mv->ctl.active = 0;
        }
    } else if (mv->state == YVID__STOPPED && !mv->ctl.active) {
        yvid__st64(&mv->want_g, 0);
    }

    yvid__wake(mv);
    mv->has_display = 1;
    mv->last_display = f->index;
    mv->last_vblank = f->vblank;
    if (mv->screen) yscr_mark(mv->screen, YSCR_PHASE_UPLOAD);
    YRT_PLOT("yvid.ready", yvid__ready_count(mv));
    YRT_ZONE_END(z);
    if (rc < 0) return rc;
    if (refused) return YVID_ERR_REFUSED;
    return mv->state == YVID__ENDED ? YVID_ENDED : YVID_OK;
}

YVID_API int yvid_update(yvid_movie* mv, const yscr_frame* f) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__update_body(mv, f), "update");
}

/* --- following another clock ------------------------------------------------------- */

/* Re-anchors the movie clock at f->onset to global movie time mg, read
 * from another clock. A clock that stalls or steps back would rewind the
 * base and fire its annotations again; the base runs on instead until the
 * clock moves. */
static int yvid__follow_to(yvid_movie* mv, const yscr_frame* f, int64_t mg) {
    int64_t m;
    if (mg <= mv->follow_last_m) return YVID_OK;
    mv->follow_last_m = mg;
    m = mg - mv->cycle * mv->info.duration;
    mv->anchor_rt = f->onset;
    mv->anchor_mt = m;
    if (mv->d.timeline) ytl_anchor(mv->d.timeline, mv->d.base, f->onset, m);
    return YVID_OK;
}

static int yvid__follow_body(yvid_movie* mv, const yscr_frame* f, yvid_unit_at_fn unit_at, void* ctx, uint32_t rate) {
    int64_t num, den, u, m;
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (!f || !unit_at || rate == 0) return YVID_ERR_ARG;
    yvid__base_rate(mv, &num, &den);
    if (num != den) return YVID_ERR_REFUSED;   /* a soundtrack is never resampled */
    if (mv->state != YVID__PLAYING || !mv->running) return YVID_OK;
    if (mv->follow_epoch != mv->anchor_epoch || !mv->following || mv->follow_rate != rate) {
        /* the other clock's unit at movie time 0 of this anchor */
        int64_t a = unit_at(ctx, mv->anchor_rt);
        int64_t mg = mv->anchor_mt + mv->cycle * mv->info.duration;
        yrt_payload pu;
        mv->follow_f0 = a - (int64_t)floor((double)mg * (double)rate / 1e9 + 0.5);
        mv->follow_rate = rate;
        mv->follow_epoch = mv->anchor_epoch;
        mv->follow_last_m = mg;
        mv->following = 1;
        memset(&pu, 0, sizeof pu);
        pu.i64[0] = mv->follow_f0; pu.i64[1] = rate;
        yvid__push(mv, (uint16_t)YVID_EV_CLOCK, mv->anchor_rt, &pu);
        return YVID_OK;
    }
    u = unit_at(ctx, f->onset);
    m = (int64_t)floor((double)(u - mv->follow_f0) * 1e9 / (double)rate + 0.5);
    return yvid__follow_to(mv, f, m);
}

YVID_API int yvid_follow(yvid_movie* mv, const yscr_frame* f, yvid_unit_at_fn unit_at, void* ctx, uint32_t rate) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__follow_body(mv, f, unit_at, ctx, rate), "follow");
}

/* --- describe and params ---------------------------------------------------------- */

static const char* yvid__why_name(int w) {
    static const char* const n[] = { "DUE", "CADENCE", "DISPLAY_LATE", "DECODE_LATE", "DRIFT", "SEEK", "LOOP", "MANUAL" };
    return w >= 0 && w < YVID_WHY_COUNT ? n[w] : "?";
}

YVID_API int yvid_describe(const yvid_movie* mv, char* buf, size_t cap) {
    char cad[200], counts[300], be[96];
    int w, n;
    size_t used = 0;
    if (!mv || !mv->open) return snprintf(buf, cap, "ysp_video %s: not open", YVID_VERSION_STRING);
    be[0] = 0;
    if (mv->dec && mv->dec->describe) mv->dec->describe(mv->dec_ctx, be, sizeof be);
    else if (mv->dec) yvid__fmt(be, sizeof be, "%s", mv->dec->name);
    if (mv->rate_num != mv->rate_den) {
        yvid__fmt(cad, sizeof cad, "base rate %d/%d: display %.4f Hz, %.4f display frames per frame%s",
                    (int)mv->rate_num, (int)mv->rate_den, mv->info.refresh_num > 0 ? (double)mv->info.refresh_num / mv->info.refresh_den : 0.0,
                    mv->info.per_frame, mv->info.multiple > 0 ? " (multiple)" : " (judders: CADENCE)");
    } else if (mv->info.refresh_num <= 0) {
        yvid__fmt(cad, sizeof cad, "display rate unknown: no cadence check");
    } else if (mv->info.multiple > 0) {
        yvid__fmt(cad, sizeof cad, "display %.4f Hz: %d display frames per frame (multiple, %+.1f ppm)",
                    (double)mv->info.refresh_num / mv->info.refresh_den, (int)mv->info.multiple, mv->info.err_ppm);
    } else {
        /* the cadence's counts over one cycle of up to 24 frames */
        double P = 1e9 * mv->info.refresh_den / mv->info.refresh_num;
        int64_t L = yvid_lead_ns(mv->info.lead == 0.0 ? -1.0 : mv->info.lead, (int64_t)P);
        int counts_n[24], k, f0 = 0, nf = 0, lo = 1 << 30, hi = 0;
        char pat[96];
        size_t pu = 0;
        memset(counts_n, 0, sizeof counts_n);
        for (k = 0; k < 2000 && nf < 24; k++) {
            int64_t due = yvid__dueg(mv, (int64_t)floor(k * P), L);
            if (due >= 24) break;
            if (due >= 0) { counts_n[due]++; if (due + 1 > nf) nf = (int)due + 1; }
        }
        pat[0] = 0;
        for (k = 0; k < nf - 1 && k < 10; k++) {
            pu += (size_t)snprintf(pat + pu, sizeof pat - pu, "%s%d", k ? ":" : "", counts_n[k]);
            if (pu >= sizeof pat) break;
        }
        for (k = 0; k < nf - 1; k++) { if (counts_n[k] < lo) lo = counts_n[k]; if (counts_n[k] > hi) hi = counts_n[k]; }
        (void)f0;
        if (lo == 0)
            yvid__fmt(cad, sizeof cad, "display %.4f Hz: %.4f display frames per frame, not a multiple: cadence %s, frames on screen 0 (dropped, CADENCE) to %.1f ms",
                        (double)mv->info.refresh_num / mv->info.refresh_den, mv->info.per_frame, pat, hi * P / 1e6);
        else
            yvid__fmt(cad, sizeof cad, "display %.4f Hz: %.4f display frames per frame, not a multiple: cadence %s, frames on screen %.1f to %.1f ms (CADENCE)",
                        (double)mv->info.refresh_num / mv->info.refresh_den, mv->info.per_frame, pat, lo * P / 1e6, hi * P / 1e6);
    }
    counts[0] = 0;
    used = (size_t)snprintf(counts, sizeof counts, "shown %llu, repeated %llu, dropped %llu",
                            (unsigned long long)mv->info.shown, (unsigned long long)mv->info.repeated, (unsigned long long)mv->info.dropped);
    for (w = 0; w < YVID_WHY_COUNT && used < sizeof counts; w++) {
        if (mv->info.repeats_by[w] || mv->info.drops_by[w])
            used += (size_t)snprintf(counts + used, sizeof counts - used, "; %s %llu/%llu", yvid__why_name(w),
                                     (unsigned long long)mv->info.repeats_by[w], (unsigned long long)mv->info.drops_by[w]);
    }
    n = snprintf(buf, cap, "ysp_video %s: %s, %dx%d %s, %d/%d fps, %lld frames, GOP %d; %s; lead %.2f; ahead %d (%d slots, %.1f MB%s%s); path %s %s; light %s; %s%s%s; worst tier %d",
                 YVID_VERSION_STRING, be, (int)mv->info.w, (int)mv->info.h, yvid__fmt_name(mv->info.format),
                 (int)mv->info.fps_num, (int)mv->info.fps_den, (long long)mv->info.frames, (int)mv->info.gop, cad,
                 mv->info.lead, (int)mv->info.ahead, (int)mv->info.slots,
                 (double)mv->info.slot_bytes * mv->info.slots / 1048576.0,
                 mv->inline_mode ? ", decoded inline" : "", mv->inline_why[0] ? ": no decode thread" : "",
                 mv->info.path == YVID_PATH_GPU ? "GPU" : "UPLOAD of",
                 mv->info.path == YVID_PATH_GPU ? "(DXVA on the screen's device, one GPU copy a frame; frame hashes not checked)" : yvid__fmt_name(mv->info.upload_format),
                 mv->info.light == YVID_LIGHT_EOTF ? "EOTF (linear)" : "CODES", counts,
                 mv->info.ts_mismatch ? "; NOT CANONICAL: timestamp mismatches" : "",
                 mv->info.hash_mismatch ? "; NOT CANONICAL: hash mismatches" : "", (int)mv->info.worst_tier);
    return n;
}

YVID_API const yscr_param* yvid_params(int* n) {
    static const yscr_param p[] = {
        { "ahead", "i32", 0, YVID_MAX_SLOTS - 2, 6, "frame", "frames decoded ahead", NULL },
        { "preroll", "i32", 0, YVID_MAX_SLOTS - 2, 6, "frame", "frames ready before an ASAP start; 0 = ahead", NULL },
        { "loop", "bool", 0, 1, 0, "", "frame 0 follows the last; annotations fire again", NULL },
        { "strict_cadence", "bool", 0, 1, 0, "", "refuse a refresh that is not a multiple of the rate", NULL },
        { "lead", "f64", -1, 0.999, 0.5, "frame", "the due rule's lead; -1 = never early", NULL },
        { "light", "enum", 0, 2, 0, "", "how a frame's values reach the screen",
          "auto=0,codes=1,eotf=2" },
        { "gpu_path", "enum", 0, 3, 0, "", "how a frame reaches a texture; shared is not built",
          "auto=0,upload=1,shared=2,gpu=3" },
        { "hw_decode", "enum", 0, 2, 0, "", "Media Foundation's decoder",
          "auto=0,off=1,dxva=2" },
        { "chroma", "enum", 0, 1, 0, "", "4:2:0 chroma upsampling: weighted at the stream's siting, or replicated",
          "sited=0,nearest=1" },
        { "base", "i32", 0, 7, 0, "", "the movie base on the timeline", NULL },
        { "min_tier", "i32", 0, 4, 0, "", "flag frames worse than this tier", NULL },
        { "movie_index", "u32", 0, 4294967295.0, 0, "", "aux of every record", NULL },
        { "pin_cpu", "i32", 0, 1024, 0, "", "decode thread's CPU; 0 = no pin", NULL },
    };
    if (n) *n = (int)(sizeof p / sizeof p[0]);
    return p;
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_VIDEO_IMPLEMENTATION_GUARD */
#endif /* YSP_VIDEO_IMPLEMENTATION */

/* The audio part of the implementation: compiled when ysp/audio.h came
 * first. */
#if defined(YSP_VIDEO_IMPLEMENTATION) && defined(YSP_AUDIO_H_INCLUDED) && !defined(YSP_VIDEO_AUDIO_IMPL)
#define YSP_VIDEO_AUDIO_IMPL
#ifdef __cplusplus
extern "C" {
#endif

/* The soundtrack's close sleeps in 1 ms steps while the device ends the
 * stream. A test seam like YVID__NOW. On a browser's main thread the
 * sleep yields, as ysp/audio.h's own waits do: without threads the device
 * callbacks run there, so a blocking sleep would wait for nothing. */
#ifndef YVID__SLEEP_UNTIL
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
static void yvid__sleep_until(int64_t t) {
    int64_t d = t - yrt_now_ns();
    if (!emscripten_is_main_runtime_thread()) { yrt_sleep_until(t, 0); return; }
    emscripten_sleep(d > 1000000 ? (unsigned)(d / 1000000) : 1u);
}
#define YVID__SLEEP_UNTIL(t) yvid__sleep_until((int64_t)(t))
#else
#define YVID__SLEEP_UNTIL(t) yrt_sleep_until((int64_t)(t), 0)
#endif
#endif

/* --- the soundtrack -------------------------------------------------------------
 * A ysp/audio.h stream (a voice, placed, recorded and confirmed like any
 * sound) that the movie's decode thread fills from the source. The ring is
 * the movie's own memory, so close() waits for the stream to end before it
 * frees it. */
typedef struct yvid__snd {
    yau_wav           w;           /* first: its stream's lines             */
    yau_audio*        au;
    float*              ring;
    float               db;
    uint64_t            mask;
    int64_t             ramp;
    yau_id            id;          /* the current play                     */
    uint32_t            refilling;   /* frame thread sets, decode thread clears */
    int64_t             seek_to;     /* decode thread: a refill waiting for the
                                      * play to end (yau_wav_seek is BUSY
                                      * until then); -1 none                 */
} yvid__snd;

static bool yvid__snd_step(void* v) {
    yvid__snd* s = (yvid__snd*)v;
    if (s->seek_to >= 0) {
        /* BUSY until the device has taken the stop; the frame thread wakes
         * this thread again on its next frame */
        if (yau_wav_seek(&s->w, s->seek_to) != YAU_OK) return false;
        s->seek_to = -1;
        yvid__st32(&s->refilling, 0);
    }
    if (yvid__ld32(&s->refilling)) return false;
    return yau_wav_step(&s->w);
}

static void yvid__snd_refill_dt(void* v, int64_t sample) { ((yvid__snd*)v)->seek_to = sample; }
static void yvid__snd_refilling(void* v) { yvid__st32(&((yvid__snd*)v)->refilling, 1); }

static int yvid__snd_ready(void* v, int64_t sample) {
    yvid__snd* s = (yvid__snd*)v;
    yau_stream_info si;
    if (yvid__ld32(&s->refilling)) return 0;
    if (yau_stream_get_info(&s->w.stream, &si) != YAU_OK) return 0;
    return si.state == YAU_STREAM_IDLE && si.first == sample && si.ready;
}

static bool yvid__snd_wants(void* v) {
    yvid__snd* s = (yvid__snd*)v;
    yau_stream_info si;
    (void)si;
    return yvid__ld32(&s->refilling) || yau_wav_wants(&s->w);
}

static int64_t yvid__snd_lead(void* v) { return yau_lead_ns(((yvid__snd*)v)->au); }

static int64_t yvid__snd_start(void* v, int64_t t) {
    yvid__snd* s = (yvid__snd*)v;
    yau_play_desc pd;
    yau_id id;
    memset(&pd, 0, sizeof pd);
    pd.stream = &s->w.stream;
    pd.at_ns = t;
    pd.db = s->db;
    pd.channels = s->mask;
    id = yau_play(s->au, &pd);
    if (id > 0) s->id = id;
    return id;
}

static void yvid__snd_stop(void* v, int64_t t) {
    yvid__snd* s = (yvid__snd*)v;
    if (s->id > 0) yau_stop_at(s->au, s->id, t, s->ramp);
}

static void yvid__snd_close(void* v) {
    yvid__snd* s = (yvid__snd*)v;
    int k, done = 0;
    if (!yau_is_open(s->au)) done = 1;   /* no callback reads the ring */
    else {
        if (s->id > 0) yau_cancel(s->au, s->id);
        for (k = 0; k < 500 && !done; k++) {   /* a device buffer, at most half a second */
            yau_stream_info si;
            yau_update(s->au);
            if (yau_stream_get_info(&s->w.stream, &si) == YAU_OK &&
                (si.state == YAU_STREAM_IDLE || si.state == YAU_STREAM_ENDED)) done = 1;
            else YVID__SLEEP_UNTIL(YVID__NOW() + 1000000);
        }
        if (done) yau_wav_close(s->au, &s->w);
    }
    /* a stream the device still holds keeps its ring: leaked, not freed
     * under the callback */
    if (done) { yvid__free(s->ring); yvid__free(s); }
}

static const struct yvid__snd_ops yvid__snd_table = {
    yvid__snd_step, yvid__snd_refill_dt, yvid__snd_refilling, yvid__snd_ready, yvid__snd_wants,
    yvid__snd_lead, yvid__snd_start, yvid__snd_stop, yvid__snd_close
};

static int64_t yvid__gcd64(int64_t a, int64_t b) { while (b) { int64_t t = a % b; a = b; b = t; } return a; }

static int yvid__soundtrack_body(yvid_movie* mv, yau_audio* au, const yvid_soundtrack_desc* d) {
    yau_caps c;
    yau_wav_desc wd;
    yvid__snd* s;
    int64_t N, num, den, rate, A2, want, rn, rd, cap;
    int rc;
    if (!mv || !mv->open) return YVID_ERR_CLOSED;
    if (!au || !d || (!d->path && !d->data && !d->reader)) { yvid__err(mv, "ysp_video: yvid_soundtrack needs an audio handle and a WAV (path, data or reader)"); return YVID_ERR_ARG; }
    if (mv->snd) { yvid__err(mv, "ysp_video: the movie has a soundtrack already"); return YVID_ERR_ORDER; }
    if (mv->state != YVID__STOPPED || mv->ctl.active) { yvid__err(mv, "ysp_video: give the soundtrack before the first play"); return YVID_ERR_ORDER; }
    if (!yau_is_open(au)) { yvid__err(mv, "ysp_video: the audio handle is not open"); return YVID_ERR_CLOSED; }
    yvid__base_rate(mv, &rn, &rd);
    if (rn != rd) {
        yvid__err(mv, "ysp_video: the movie base runs at %lld/%lld; a soundtrack plays at rate 1 only (no resampling)", (long long)rn, (long long)rd);
        return YVID_ERR_REFUSED;
    }
    yau_get_caps(au, &c);
    s = (yvid__snd*)yvid__malloc(sizeof *s);
    if (!s) { yvid__err(mv, "ysp_video: out of memory"); return YVID_ERR_FULL; }
    memset(s, 0, sizeof *s);
    /* the ring is the movie's: sized for the device's channel count, the
     * most a WAV may have */
    cap = d->ring > 0 ? d->ring : (int64_t)c.rate;
    if (cap < 4 * (int64_t)c.period) cap = 4 * (int64_t)c.period;
    s->ring = (float*)yvid__malloc((size_t)cap * c.channels * sizeof(float));
    if (!s->ring) { yvid__free(s); yvid__err(mv, "ysp_video: out of memory for the soundtrack's ring"); return YVID_ERR_FULL; }
    memset(&wd, 0, sizeof wd);
    wd.path = d->path; wd.data = d->data; wd.size = d->size;
    wd.reader = (const yau_reader*)(const void*)d->reader;   /* the same layout */
    wd.reader_ctx = d->reader_ctx;
    wd.loops = mv->d.loop ? YAU_FOREVER : 0;   /* a loop counts samples on: sample s is frame s mod A */
    wd.ring = cap;
    wd.memory = s->ring;
    wd.id = mv->d.movie_index;
    rc = yau_wav_open(au, &s->w, &wd) ? 0 : s->w.error_code;
    if (rc < 0) {
        /* rate, channels, sample type and damage: ysp_audio's refusals */
        yvid__err(mv, "ysp_video: the soundtrack: %s", yau_wav_error(&s->w));
        yvid__free(s->ring); yvid__free(s);
        return rc == YAU_ERR_NOT_FOUND || rc == YAU_ERR_IO ? YVID_ERR_IO : YVID_ERR_REFUSED;
    }
    /* The length: the movie's duration in samples, N x den x rate / num,
     * rounded to the nearest sample, a tie up. */
    N = mv->info.frames; num = mv->info.fps_num; den = mv->info.fps_den; rate = s->w.info.rate;
    rc = YVID_OK;
    if ((double)N * (double)den * (double)rate * 2.0 >= 9.0e18) { yvid__err(mv, "ysp_video: the movie is too long for a soundtrack"); rc = YVID_ERR_REFUSED; }
    if (rc == YVID_OK) {
        A2 = 2 * N * den * rate;   /* twice the duration in samples, times num */
        want = (A2 + num) / (2 * num);
        if (s->w.info.frames != want) {
            yvid__err(mv, "ysp_video: the soundtrack has %lld samples; the movie's %lld frames at %d/%d fps last %.3f samples at %u Hz, so %lld are needed (the duration rounded to the nearest sample)",
                        (long long)s->w.info.frames, (long long)N, (int)num, (int)den, (double)A2 / 2.0 / (double)num, (unsigned)rate, (long long)want);
            rc = YVID_ERR_REFUSED;
        } else if (mv->d.loop && (N * den * rate) % num != 0) {
            int64_t k = num / yvid__gcd64(num, den * rate);
            yvid__err(mv, "ysp_video: a loop needs the movie to last a whole number of samples, so that the sound does not slip a part of a sample each cycle; at %d/%d fps and %u Hz the frame count must be a multiple of %lld (it is %lld)",
                        (int)num, (int)den, (unsigned)rate, (long long)k, (long long)N);
            rc = YVID_ERR_REFUSED;
        }
    }
    if (rc < 0) { yau_wav_close(au, &s->w); yvid__free(s->ring); yvid__free(s); return rc; }
    s->au = au;
    s->db = d->db;
    s->mask = d->channels;
    s->ramp = d->ramp_ns;
    s->seek_to = -1;
    mv->snd = s;
    mv->snd_ops = &yvid__snd_table;
    mv->snd_rate = (uint32_t)rate;
    mv->snd_frames = s->w.info.frames;
    mv->snd_phase = 0;
    /* fill from the start now, so the first play does not wait for it */
    yvid__snd_rearm(mv, 0, 0, 0, 0);
    return YVID_OK;
}

YVID_API int yvid_soundtrack(yvid_movie* mv, yau_audio* au, const yvid_soundtrack_desc* d) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__soundtrack_body(mv, au, d), "soundtrack");
}

/* Sample s's time on the movie clock, counted from the first cycle. */
static int64_t yvid__snd_ns(int64_t s, uint32_t rate) {
    int64_t q = yvid__floordiv(s, (int64_t)rate), r = s - q * (int64_t)rate;
    return q * YVID__NS + (r * YVID__NS + (int64_t)rate / 2) / (int64_t)rate;
}

static int64_t yvid__au_unit_at(void* ctx, int64_t t) { return yau_frame_at((const yau_audio*)ctx, t); }

static int yvid__follow_audio_body(yvid_movie* mv, yau_audio* au, const yscr_frame* f) {
    yau_caps c;
    if (!au) return YVID_ERR_ARG;
    if (mv && mv->open && mv->snd) {
        /* The soundtrack's own clock: the sample at the onset, from ysp_audio's
         * fit and the stream's origin, is the movie time. */
        yvid__snd* s = (yvid__snd*)mv->snd;
        yau_stream_info si;
        int64_t smp, rn, rd;
        if (!f) return YVID_ERR_ARG;
        yvid__base_rate(mv, &rn, &rd);
        if (rn != rd) return YVID_ERR_REFUSED;
        if (mv->state != YVID__PLAYING || !mv->running || mv->snd_phase != 2) return YVID_OK;
        if (yau_stream_get_info(&s->w.stream, &si) != YAU_OK || si.state != YAU_STREAM_PLAYING ||
            si.id != mv->snd_id || !si.started) return YVID_OK;
        if (yau_stream_sample_at(au, &s->w.stream, f->onset, &smp) != YAU_OK) return YVID_OK;
        mv->following = 1;
        return yvid__follow_to(mv, f, yvid__snd_ns(smp, mv->snd_rate));
    }
    yau_get_caps(au, &c);
    return yvid_follow(mv, f, yvid__au_unit_at, au, c.rate);
}

YVID_API int yvid_follow_audio(yvid_movie* mv, yau_audio* au, const yscr_frame* f) {
    char* e = mv ? mv->error : NULL;
    char c0 = yvid__hold(e);
    return (int)yvid__done(e, c0, yvid__follow_audio_body(mv, au, f), "follow_audio");
}
#ifdef __cplusplus
}
#endif
#endif
