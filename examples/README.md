# Examples

One program per file. CMake builds `examples/<lib>/<name>.c` as the
program `<lib>_<name>` (for example `examples/gfx/gallery.c` is
`gfx_gallery`). Some examples sit in the folder of the header they mainly
show but use another header's build; they keep their own file name as
their program name (`YSP_EXAMPLES_ACROSS` in `CMakeLists.txt`):
`rdk/gfx_rdk.c`, `rdk/gfx_rdk_bench.c`, `rdk/trial_rdk.c`, `timeline/gfx_trial.c`,
`layout/gfx_layout.c`, `device/photodiode_check.c` and the
`response/trial_*.c`; `net/labstreamer_flip.c` is `net_labstreamer_flip`
and `audio/av_sync.c` is `audio_av_sync`.

Run a program from the build folder (`build/<preset>/Release/` with MSVC,
`build/<preset>/` otherwise). "Needs" says what a run needs beyond the
build: SDL3 (a window), ANGLE (GL ES on Windows), a font, a device, or
nothing. `--sim` runs a windowed example on the simulated display, which
needs nothing; CI runs the commands marked "CI".

## aep

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `aep_sim` | Level-set and global acquisitions against a 2-D simulated observer | aep | `aep_sim` (CI) | nothing |
| `aep_bench` | The cost per trial against one display frame | rt, aep | `aep_bench` (CI) | nothing |
| `aep_audiometric` | The audiometric test function of Owen et al. 2021 | aep, stair | `aep_audiometric` (CI) | nothing |
| `aep_async` | The GP on the pump thread beside a 16 ms frame loop | aep | `aep_async` (CI) | nothing |
| `aep_optimize` | Finding the stimulus rated highest (optimization acquisitions) | aep | `aep_optimize` (CI) | nothing |
| `aep_pairwise` | Preferences from paired comparisons | aep | `aep_pairwise` (CI) | nothing |

## audio

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `audio_tone` | One quiet tone at a time and its onset record | audio | `audio_tone --null` (CI); `audio_tone` on the default device | miniaudio; a sound device without `--null` |
| `audio_clockstats` | The device clock and the callback on this machine: callback intervals, underruns, the queue (frames written minus the device position); plays silence | rt, audio | `audio_clockstats --seconds 10`; `audio_clockstats --queue 1 --load 8 --frame-work 8` | miniaudio; a sound device |
| `audio_wasapi_periods` | What each WASAPI render endpoint offers: mix and device formats, the device period, and the shared-mode engine periods of IAudioClient3 (default, fundamental, minimum, maximum); with `--init` and `--exclusive`, the buffer that each kind of stream gets. Plays nothing | none (raw WASAPI) | `audio_wasapi_periods --init` | Windows; built with the other audio examples, so CMake must find miniaudio |
| `audio_schedule` | PTB's sound schedule demos (BasicSoundScheduleDemo, SimpleSoundScheduleDemo, BasicAMAndMixScheduleDemo, BasicSoundChannelHoppingDemo): tones and clicks at stated times on one or both channels, a buffer looped and started again, a gain change and a stop at a time; each onset's plan, fit time, residual and tier; the spread against the plan and the shortest safe lead on this machine | audio | `audio_schedule --null` (CI); `audio_schedule --csv schedule.csv` | miniaudio; a sound device without `--null` |
| `audio_av_sync` | PTB's PsychPortAudioTimingTest: a corner flash and a click planned for one time; audio minus flip from the two records; with line boards (`firmware/ysp_line/`) on a photodiode and the sound, the true sound minus light and each header's onset offset (hardware mode never run) | screen, audio, device | `audio_av_sync --sim` (CI); `audio_av_sync`; `audio_av_sync --board <key> --sound-board <key> --db -20` | SDL3, miniaudio; a sound device; for the true offset, the boards with a photodiode and a sound comparator |

## color

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `color_convert` | Conversions, gamut questions and gamut mapping on a calibration | color | `color_convert` (CI) | nothing |
| `color_bench` | The cost of the conversions | rt, color | `color_bench` | nothing |

## device

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `photodiode_check` | The MCU photodiode on the device layer: ports and keys, a console monitor, or edge minus flip onset in a window | device, screen | `photodiode_check --list`; `photodiode_check --key serial:16C0:0483:: --monitor 10`; `photodiode_check --key <key>` | SDL3; the board of `firmware/ysp_line/` (none for `--list`) |
| `device_out_latency` | An output's write-to-edge latency by loopback: pulses on an output role, edges on an input board, the distribution in seconds and a file named by its SHA-256; `--store` keeps the file in the rig profile's `loopback/` and writes the role's binding and summary into the profile | device, rig (parallel) | `device_out_latency --sim` and `--sim --self` (no hardware); `device_out_latency --sim --n 20 --warmup 1 --store --rig-dir <folder>` (CI); `device_out_latency --out lines:<adapter key> --in <board key> --store` | nothing for `--sim`; else an output and a board of `firmware/ysp_line/` wired to it |
| `device_trigger_flip` | An output role as a ysp/screen.h trigger channel: codes at planned vblanks, written by the device layer, deadline to write in the log | device, screen (no SDL) | `device_trigger_flip` (an in-process box); `device_trigger_flip --out lines:<adapter key>` | nothing; a trigger output with `--out` |

## gfx

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `gfx_hello` | A drifting gabor: the manual's frame loop | gfx | `gfx_hello --sim --frames 30` (CI); `gfx_hello` | SDL3, ANGLE (Windows) |
| `gfx_gallery` | One labeled tile per feature, eight pages | gfx, timeline, outline | `gfx_gallery --sim` (CI); `gfx_gallery` | SDL3, ANGLE |
| `gfx_text` | Slug text from real fonts | gfx, timeline, outline | `gfx_text --sim` (CI); `gfx_text` | SDL3, ANGLE; system fonts (a bitmap font without them) |
| `gfx_load` | A frame loop under load, measured by the flip records | gfx | `gfx_load --sim --gabors 10 --dots 100 --seconds 1` (CI) | SDL3, ANGLE |
| `gfx_calib` | Photometer readings in, a calibration out | gfx | `gfx_calib` (CI: a synthetic display) | SDL3 to build; readings for a real display |
| `gfx_bench` | The cost per frame, offscreen | gfx, outline | `gfx_bench` | SDL3, ANGLE; a GL ES 3.0 device |
| `gfx_gratings` | PsychoPy's counterphase, second-order gratings and flashing wedge demos as USER shaders on timeline tracks: counterphase flicker with the achieved frequency from the flip records, contrast-modulated sine and noise carriers, a rotating checkerboard wedge, a render target sampled through a lens shader, an image as a graded mask | gfx, timeline | `gfx_gratings --sim` (CI); `gfx_gratings` | SDL3, ANGLE |
| `gfx_luminance_bits` | Psychtoolbox's PseudoGray and high-precision output tests: a shallow ramp and a 0.4 % grating through 8-bit rounding, the ordered dither, the noise dither and PseudoGray in a USER shader; levels, block-mean errors and shown contrast from the calibration; every code read back against the output stage's formulas | gfx, color | `gfx_luminance_bits --sim` (CI); `gfx_luminance_bits --cal display.yspcal` | SDL3, ANGLE; a photometer's calibration for any claim about light |
| `gfx_clut_sync` | Psychtoolbox's ClutAnimDemo and SyncedCLUTUpdateTest: a palette turned by `ygfx_set_lut()` each frame, a cancel test (image and table swapped on the same flip), both read back; then the OS gamma ramp set once a frame, timed against the vblank grid | gfx, screen | `gfx_clut_sync --sim` (CI); `gfx_clut_sync`; `gfx_clut_sync --fullscreen --os-ramp` (watch for flicker) | SDL3, ANGLE; Windows for the ramp phase |
| `gfx_filtered_noise` | Psychtoolbox's FastFilteredNoiseDemo on the CPU: band-pass oriented and 1/f noise by FFT each trial, uploaded as R32F images, beside NOISE GAUSSIAN and SIMPLEX; the power in the band for each, and the make and upload cost per trial against the frame | gfx | `gfx_filtered_noise --sim` (CI); `gfx_filtered_noise --every-frame` | SDL3, ANGLE |
| `gfx_gaze_contingent` | Psychtoolbox's GazeContingentDemo with the mouse as the gaze: a sharp Gaussian window on a blurred image; per frame the sample's stamp, the read and the flip onset (input to photon in software); `--inject` moves the pointer by SendInput | gfx, screen, input | `gfx_gaze_contingent --sim` (CI); `gfx_gaze_contingent`; `gfx_gaze_contingent --inject --out lat.csv` | SDL3, ANGLE; Windows for `--inject` and `--raw-mice` |
| `gfx_capture` | Stimulus capture during a trial: every Nth or marked frame of 100 drifting gabors and a bar to PPM files, by the writer thread or by `ygfx_capture_pump()`; flip records and `end()` cost with capture on and off | gfx | `gfx_capture --sim --frames 60 --every 3 --out capture_sim/f` and `--mark 5 --manual` (CI); `gfx_capture --fullscreen --every 1 --out cap/f` | SDL3, ANGLE |

## layout

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `gfx_layout` | Paragraphs in seven scripts, bidi strings, BudouX breaking and an editor, through pack/layout | gfx, outline, pack/layout | `gfx_layout` | SDL3, ANGLE, `YSP_BUILD_LAYOUT=ON`, Windows fonts |

## net

All three load liblsl at run time (docs/net.md, "How-to: get liblsl"); without it they print where they looked and exit 2.

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `net_marker_send` | An LSL marker outlet: marks sent on the ysp_rt clock, the cost of each push, the LSL stamp against the outlet's clock fit | net | `net_marker_send` (CI: exit 2, no liblsl); `net_marker_send --n 200 --interval 0.1` with a viewer | liblsl; an inlet (LabRecorder, `net_stream_view`) |
| `net_stream_view` | Any LSL stream on the ysp_rt clock: the streams and their keys, then each second the rate, gaps, drops, liblsl's offset and round trip, the remote clock fit, and the newest sample or marker | net | `net_stream_view --list` (CI: exit 2, no liblsl); `net_stream_view --key lsl:Data:: --seconds 20 --edge 0:1.0:0.2` | liblsl; a stream |
| `net_labstreamer_flip` | The Neurobehavioral Systems LabStreamer as the reference for display onsets: markers at the flip (or after the flip record), ysp's own edges on its Data stream, and its Latencies, per change of a corner patch | net, screen, json | `net_labstreamer_flip --frames 1200` (docs/net.md, "LabStreamer hand test") | SDL3; liblsl; a LabStreamer with its phototransistor on the patch |

## outline

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `outline_font` | A label from a font file: exact alpha (a PGM) and a curve set | outline | `outline_font <font.ttf> "label" 48 out.pgm` | a font file (CI checks the usage exit) |
| `outline_bench` | The cost of the builds against their bars | rt, outline | `outline_bench 1 <font folder>` | fonts (CI checks the "no fonts" exit) |

## pack

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `pack_bench` | The reader's costs (docs/pack.md 8): open, lookup, verification, a chunk through a cursor, allocations after open | rt, pack | `pack_bench <pack> [rounds]` | a pack; `ypak build` makes one |
| `trial_images` | The pack path end to end (PsychoPy's randomisedBlocks, navon, mentalRotation): textures and the conditions table from a pack the build makes with `ypak` from generated PNGs (`trial_images_assets`); Navon and rotation blocks in random order; a grating through a graded mask image (a target and MULTIPLY) | pack, gfx, timeline, trials, response | `trial_images --sim`; `trial_images` | SDL3, ANGLE, `YSP_BUILD_PACK=ON` (the core tool is enough: no glyph runs) |
| `ypak` (`pack/ypak.c`) | The pack tool: build a pack from a source description, verify, list, info, extract, cat, rebuild from the manifest, append to a player | pack, the tool library | `ypak build study.json -o study.ysppak` (the description: docs/pack.md 5.2); `ypak verify study.ysppak` (CI verifies the tool test's pack) | `YSP_BUILD_PACK=ON` and lodepng (`tools/vendor_pack.py --write`); glyph runs also need `YSP_BUILD_LAYOUT=ON` (docs/pack.md 5.1) |

## parallel

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `parallel_trigger` | A trigger byte on the parallel port | parallel | `parallel_trigger` | an LPT port; InpOut on Windows (CI checks the "no port" exit) |

## quest

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `quest_sim` | Psi and QUEST+ against a simulated observer | quest | `quest_sim` (CI) | nothing |
| `quest_qcsf` | The quick CSF as a custom model | quest | `quest_qcsf` (CI) | nothing |
| `quest_bench` | The cost of `yqst_next()` and `yqst_update()` | rt, quest | `quest_bench` (CI) | nothing |
| `quest_async` | QUEST+ on a thread beside a frame loop | quest | `quest_async 20 1` (CI) | nothing |

## rdk

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `rdk_bench` | The cost per frame, CPU only | rt, rdk | `rdk_bench --quick` (CI) | nothing |
| `gfx_rdk` | Random dot kinematograms drawn, four pages, replayed from logs | gfx, timeline, rdk | `gfx_rdk --sim` (CI); `gfx_rdk` | SDL3, ANGLE |
| `gfx_rdk_bench` | An RDK's cost per frame end to end, offscreen | gfx, rdk | `gfx_rdk_bench` | SDL3, ANGLE; a GL ES 3.0 device |
| `trial_rdk` | A dot motion direction task (PsychoPy's dots.py, jspsych-contrib plugin-rdk): coherence from the conditions, RT from the motion onset's flip record, the field's seed and replay digest in each row, each update's inputs in a steps file | gfx, timeline, trials, rdk, response | `trial_rdk --sim` (CI: replays every trial from the steps file); `trial_rdk` | SDL3, ANGLE |

## response

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `trial_keyboard` | jsPsych's html-keyboard-response as a small experiment: RT from the flip onset | gfx, timeline, trials, response | `trial_keyboard --sim --out trials.csv` (CI); `trial_keyboard` | SDL3, ANGLE |
| `trial_same_different` | jsPsych's same-different-html: two bars, a gap, Q or P; the SOA from flip records | gfx, timeline, trials, response | `trial_same_different --sim` (CI); `trial_same_different` | SDL3, ANGLE |
| `trial_srt` | jsPsych's serial-reaction-time: four positions, sequence and random blocks, an RSI | gfx, timeline, trials, response | `trial_srt --sim` (CI); `trial_srt` | SDL3, ANGLE |
| `trial_adjustment` | jsPsych's reconstruction (method of adjustment): G and H change a bar, space confirms; the steps in a second file | gfx, timeline, trials, response | `trial_adjustment --sim` (CI); `trial_adjustment` | SDL3, ANGLE |
| `trial_mouse_tracking` | A mouse-tracking trial with extension-mouse-tracking's data: samples per event, movement onset, raw mice on request | gfx, timeline, trials, response | `trial_mouse_tracking --sim` and `--sim --raw-mice` (CI); `trial_mouse_tracking`; `trial_mouse_tracking --raw-mice` | SDL3, ANGLE; Windows for `--raw-mice` in a window |
| `trial_audio_keyboard` | jsPsych's audio-keyboard-response: RT from the tone's onset record, with its tier | gfx, audio, trials, response | `trial_audio_keyboard --sim` (CI); `trial_audio_keyboard` | SDL3, ANGLE, miniaudio; a sound device without `--sim` |
| `trial_stop_signal` | The stop-signal task (jspsych-contrib plugin-stop-signal): a tone at a staircase SSD, display and sound on one clock | gfx, audio, timeline, trials, stair, response | `trial_stop_signal --sim` (CI); `trial_stop_signal` | SDL3, ANGLE, miniaudio; a sound device without `--sim` |
| `trial_2afc_adaptive` | Spatial 2AFC contrast detection (PsychoPy's psychophysicsStaircase, psychophysicsStairsInterleaved, JND_staircase_exp): a staircase and QUEST+ interleaved, the contrast in light through the calibration, the contrast the 8-bit codes show given to the methods | gfx, color, timeline, trials, stair, quest, response | `trial_2afc_adaptive --sim` (CI); `trial_2afc_adaptive --cal display.yspcal` | SDL3, ANGLE; a photometer's calibration for a claim about contrast |
| `trial_stroop` | PsychoPy's Stroop demos: color words in ink laid out by pack/layout, conditions from a CSV with weights, the order from rules text, practice with feedback, then a test | gfx, outline, pack/layout, timeline, table, trials, response | `trial_stroop --sim` (CI); `trial_stroop` | SDL3, ANGLE, `YSP_BUILD_LAYOUT=ON`; a font for a window run |
| `trial_two_keyboards` | PTB's KbQueueDemo per keyboard: two players race to a target on keyboards of their own, ids read at run time, one collector each, the winner by the stamps, a third keyboard and device 0 ignored, the bridge's second-report drops per trial | gfx, trials, response | `trial_two_keyboards --sim` (CI); `trial_two_keyboards`; `trial_two_keyboards --one-keyboard` (F and J) | SDL3, ANGLE; two keyboards on Windows (SDL's raw keyboard path) |

## rigfile

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `rigfile_profile` | The rig profile: load and check it (each loopback file by SHA-256), its roles, notes and hash; list profiles; a new profile; a binding by hand, also of an LSL stream (family `lsl`) | rigfile | `rigfile_profile`; `rigfile_profile --rig booth2`; `rigfile_profile --list`; `rigfile_profile --new booth-2`; `rigfile_profile --bind resp xid serial:0403:6001:FT4ABC12:`; `rigfile_profile --bind ref lsl lsl:Data::` (CI, in a folder of the runner's with `--dir`) | nothing |
| `rigfile_bench` | The costs of reading, checking, writing and hashing a profile, and ysp/json.h on a 1 MB document | rt, json, rigfile | `rigfile_bench` (under the measurement lock) | nothing |

## rt

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `rt_jitter` | What the waits are worth on this machine | rt | `rt_jitter 40 1000` (CI) | nothing |
| `rt_pump` | A slow computation moved off the frame loop | rt | `rt_pump 40` (CI, also under emcc) | nothing |
| `rt_ring_csv` | A frame loop and an audio-like callback logged to CSV | rt | `rt_ring_csv 30 rt_ring.csv` (CI, also under emcc) | nothing |
| `rt_ring_bench` | The cost of the ring, the trace macros and the correlation | rt | `rt_ring_bench 100000 2 200` (CI) | nothing |

## screen

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `screen_hello` | The frame loop with a timeline trial | screen, timeline | `screen_hello --sim` (CI); `screen_hello` | SDL3 |
| `screen_flipstats` | The swap path measured on this machine | screen | `screen_flipstats --sim --frames 120` (CI); `screen_flipstats --frames 600` | SDL3; a display |
| `screen_input` | How good the input timestamps are; devices; raw mice | screen | `screen_input`; `screen_input --devices`; `screen_input --mice` | SDL3; Windows for the injected keys |
| `screen_abort` | The abort combination and the panic watchdog | screen | `screen_abort` | SDL3; Windows |
| `screen_gamma` | The OS gamma ramp, checked without changing what is shown | screen | `screen_gamma` | SDL3; Windows |
| `screen_sync_check` | Psychtoolbox's PerceptualVBLSyncTest: full-screen black and white flicker with a moving bar, where tearing shows; per second, flips per refresh, off-grid flips, drops, path changes and the sync guard's evidence; the guard's verdict (exit 3 when it fired). Warns about the flicker; `--no-flicker` for a low-contrast run | screen | `screen_sync_check --sim` and `--sim-vsync-off` (CI); `screen_sync_check --no-flicker`; with a driver panel set to vsync off (docs/screen.md, "Hand test: the Intel setting") | SDL3; a display |

The X11 display-timing probe for the GLX backend is in `tests/probe/screen_x11/` (Linux only, not built by CMake): `sh tests/probe/screen_x11/run.sh`. See [screen_x11_probe.md](../docs/screen_x11_probe.md).

## serial

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `serial_trigger` | List ports, send a trigger byte, echo the answer | serial | `serial_trigger COM3 0x2A` | a serial port (CI checks the usage and "open fails" exits) |

## stair

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `stair_sim` | A 3-down-1-up staircase against a simulated observer | stair | `stair_sim` (CI) | nothing |

## timeline

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `timeline_trial` | One trial's timeline on a simulated 60 Hz display | timeline | `timeline_trial` (CI) | nothing |
| `timeline_tracking` | A sum-of-sines target, a flicker and a tween at 500 Hz | timeline | `timeline_tracking` (CI) | nothing |
| `timeline_bench` | The cost of `ytl_evaluate()` and the loads | rt, timeline | `timeline_bench 500` (CI) | nothing |
| `gfx_trial` | The timeline trial with gfx stimuli | gfx, timeline | `gfx_trial --sim` (CI); `gfx_trial` | SDL3, ANGLE |
| `trial_sternberg` | PsychoPy's Sternberg demo: 2, 4 or 6 digits at a 0.5 s SOA from one op table (`ytl_run()`), a probe after 1 s; each digit's SOA from its flip record | gfx, outline, timeline, trials, response | `trial_sternberg --sim` (CI); `trial_sternberg` | SDL3, ANGLE; a font (the 5 x 7 bitmap digits without one) |

## trials

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `trials_mocs` | Constant stimuli in a constrained order | trials | `trials_mocs` (CI) | nothing |
| `trials_save_rows` | One CSV row per trial with `ytr_data`: a flanker task, the session's columns and the caller's, a sidecar; `--sim` reads the file back with ysp/table.h | trials, table | `trials_save_rows --sim` (CI) | nothing |
| `trials_interleave` | Three staircases and catch trials, interleaved | trials, stair | `trials_interleave` (CI) | nothing |
| `trials_bench` | CSV parse rate, block views, open and next() costs | rt, trials | `trials_bench` | nothing |

## video

| Program | Shows | Headers | Run | Needs |
|---|---|---|---|---|
| `video_play` | A movie in a window, and the frame-thread cost | video | `video_play --sim` (CI); `video_play --file movie.mpg` | SDL3, ANGLE; a frame sequence or MPEG-1 file for `--file` |
| `video_bench` | The decode thread's cost per frame | video | `video_bench --size 64x48 --reps 3` (CI); `video_bench --mpg clip.mpg` | SDL3 to build; a clip for MPEG-1 |
| `video_check` | The GPU paths against the upload path, frame by frame | video | `video_check --file clip.mp4` | SDL3, ANGLE; Windows (Media Foundation); the test clips |

Probes that are not examples live in `tests/probe/`: the beam-racing probe for Windows is `tests/probe/beam_race/` (docs/beam_race_probe.md).
