# Devices: specification draft

Status: draft for review, 2026-10-08; decisions in section 16. Steps 1
to 3 of section 14.2 are built: `ysp/rt.h` v0.6.0 and v0.7.0 (the fit,
the per-user folders), `ysp/input.h` v0.3.0, `ysp/response.h` v0.1.4,
`ysp/box.h` v0.1.0 and v0.2.0 (outputs), `ysp/device.h` v0.1.0 and v0.2.0
(outputs, the trigger channel, roles), `firmware/ysp_line/`,
`examples/device/photodiode_check.c`, `out_latency.c` and
`trigger_flip.c` (docs/rt.md, docs/box.md, docs/device.md). The rig
profile (section 11.2) was decided on 2026-10-09 and is built:
`ysp/rigfile.h` v0.1.0 on the strict JSON reader `ysp/json.h` v0.1.0. Step
4 of section 14.2 (LSL) is built on 2026-10-09: `ysp/net.h` v0.1.0 and
`ysp/rigfile.h` v0.2.0 (family `lsl`) (docs/net.md). The rest is not built. No device in this document was attached to the
development machine, and no timing number in it was measured on a device
here. A number from another source carries its
source. "Unverified" marks a statement that was not checked against a
primary source (a manual, a specification or source code). Research
notes: `C:\tmp\psy-work\devices\`.

Sections 1 to 4 and 13 to 16 explain the design and its order. Sections
5 to 12 are the reference.
The document extends `docs/rig_spec.md` sections 4, 6, 8 and 12. It
replaces the single-device proposal for `ysp/box.h` of 2026-10-08
(`C:\tmp\psy-work\response\serial_box_design.md`). Section 15 says what
it keeps from that proposal.

## 1. Summary

A specialized device is any input or output that is not SDL's keyboard,
mouse, touch, pen or gamepad: response boxes, scanner pulses, trigger
outputs, eye trackers, marker streams, force sensors, photodiodes and
microphones used as event sources, VPixx hardware. A rig can have many
of them at once, of many kinds.

ysp splits a device into seven parts. Each part has one home:

| Part | Question it answers | Home |
|---|---|---|
| Transport | How do bytes, reports or samples move? | `ysp/serial.h`, `ysp/parallel.h` (exist); `ysp/hid.h`, `ysp/net.h` (new); a vendor library loaded at run time |
| Protocol | What do the bytes mean? | `ysp/box.h` (new, pure): decoders and encoders, tested on captured bytes |
| Clock | When did it happen, on the ysp_rt clock, and how good is that time? | `ysp/rt.h` v0.6 (new: the device clock fit); the source entry of `ysp/input.h` |
| Direction | Input events, output codes, or both? | `ysp/device.h` (new): one instance model for both |
| Identity | Which physical device is this, across a replug and across sessions? | `ysp/device.h`: a match key per transport; the rig profile binds keys to roles |
| Lifecycle | Open, identify, run, lose, reconnect, close | `ysp/device.h`: one state machine, every change in the log |
| Producer | How do events reach the frame loop and the data file? | `ysp/device.h`: a reader per instance, a sink (the input bridge by default), a stream ring for high-rate samples |

The player configures devices from two files. The pack names the roles
an experiment needs ("resp", "trig", "eye") and their requirements. A
rig profile, which stays on the rig, binds each role to a physical device
and holds its measured latency. The data file records the bound device of
each role, and every event carries its role.

The first three things to build (section 14):
1. The device clock fit in `ysp/rt.h`, with raw clock pairs in the log.
2. `ysp/device.h` with the serial transport, and `ysp/box.h` with three
   decoders: Cedrus XID, the ysp line protocol, and the photodiode frame
   of `tests/loopback/screen_loopback.c`. The photodiode is the first
   device on the layer, so the coming photodiode verifies it.
3. Outputs on the same model: trigger roles over `ysp/parallel.h`,
   `ysp/serial.h` and LSL markers, bound to `ysp/screen.h`'s trigger
   channels, with a loopback procedure for the latency of each.

## 2. Prior art

Read on 2026-10-08: source code where it is public (Psychtoolbox-3
`master`, PsychoPy `release` 2026.2.4 and its device plugins, OpenSesame,
jsPsych, liblsl, pyxid), the vendors' documentation otherwise. Notes with
line-level citations: `C:\tmp\psy-work\devices\survey_ptb_psychopy.md`
and `survey_lsl_devices.md`.

### 2.1 Tools

| Tool | Device model | Threads | Time base and device clocks | What ysp takes |
|---|---|---|---|---|
| Psychtoolbox: IOPort ([IOPort.c](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/PsychSourceGL/Source/Common/IOPort/IOPort.c)) | A serial handle; protocols in M-files per device (CedrusResponseBox, CMUBox, PsychRTBox) | One background read thread per port, realtime priority, polling (1 ms on Windows) or blocking | One stamp per read chunk, after `ReadFile` returns ([PsychSerialWindowsGlue.c](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/PsychSourceGL/Source/Windows/IOPort/PsychSerialWindowsGlue.c)); writes return pre- and post-write times | A thread per device; one stamp per read; bracketed writes |
| Psychtoolbox: PsychRTBox ([PsychRTBox.m](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/Psychtoolbox/PsychHardware/PsychRTBox.m)) | A box with its own clock | as IOPort | `SyncClocks` gives host, box and a confidence (0.3 ms typical; fails unless 1.3 ms within 0.5 s); `ClockRatio` measures drift; the manual says to keep raw box times and remap after the session (`BoxsecsToGetsecs`) | Raw device times in the data; an offline refit |
| Psychtoolbox: CedrusResponseBox ([.m](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/Psychtoolbox/PsychHardware/CedrusResponseBox.m)) | XID box | as IOPort | Timer reset or query inside a bracketed blocking write, a random sub-ms wait between tries "to desync us from the USB duty cycle", up to 5 tries, stop below 1 ms; drift from two queries | The BRACKET method, the random wait |
| Psychtoolbox: PsychDataPixx `GetPreciseTime` ([.m](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/Psychtoolbox/PsychHardware/DatapixxToolbox/DatapixxBasic/PsychDataPixx.m)) | VPixx device clock, latched by a register write | caller's | Up to 0.5 s of bracketed writes; rejects windows over 1.3 ms; keeps the smallest post-write minus box time (a minimum-offset filter, not a round-trip midpoint); a line fit over all samples after the session | The minimum-offset filter for BRACKET pairs; an acceptance bound |
| Psychtoolbox: PsychHID KbQueue ([KbQueueCreate.m](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/Psychtoolbox/PsychBasic/KbQueueCreate.m)) | Any HID device with buttons, by index | One realtime worker on DirectInput notifications (Windows), one stamp per wake | host | Device indices are enumeration order; `GetGamepadIndices` matches by serial and location but "does not work on Windows": the identity problem ysp's match keys address |
| Psychtoolbox: Eyelink | SR Research library | caller's | `Eyelink('TimeOffset')`, which "may 'jiggle' by 50 usec or more" ([EyelinkTimeOffset.c](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/PsychSourceGL/Source/Common/Eyelink/EyelinkTimeOffset.c)); its algorithm is inside the vendor library (unverified) | The SDK stamp source |
| PsychoPy hardware ([base.py](https://github.com/psychopy/psychopy/blob/release/psychopy/hardware/base.py), [manager.py](https://github.com/psychopy/psychopy/blob/release/psychopy/hardware/manager.py)) | `BaseResponseDevice` with `parseMessage`; a `DeviceManager` registry by user name; device plugins (`psychopy-cedrus`, `-bbtk`, `-curdes`, `-eyetracker-*`) | One shared listener thread, 0.1 s sleep by default, "not recommended" inside an experiment ([listener.py](https://github.com/psychopy/psychopy/blob/release/psychopy/hardware/listener.py)) | PsychoPy's clock is Psychtoolbox's `GetSecs`. `psychopy-cedrus` maps the box clock by one query with no round-trip correction | Devices by name (ysp: roles) |
| PsychoPy ioHub ([server.py](https://github.com/psychopy/psychopy/blob/release/psychopy/iohub/server.py), [devices](https://github.com/psychopy/psychopy/blob/release/psychopy/iohub/devices/__init__.py)) | Device classes (keyboard, mouse, serial with configurable parsers, PST box, eye tracker common interface); YAML config; HDF5 store per device | A separate process; polled devices as gevent greenlets on one OS thread, 1 ms interval suggested | Each event has `device_time`, `logged_time`, `delay`, `time = logged_time - delay`, `confidence_interval`. Both processes read one system clock, so no sync is needed between them. EyeLink and Tobii plugins compute `delay` as "now minus sample age", with no drift handling (a comment says it is still needed). The ioSync MCU did NTP-style sync and was removed in 2019 ([commit](https://github.com/psychopy/psychopy/commit/1cb310c6b47ab3e4aaae502c0a30317d4b397453)) | A per-event uncertainty; an eye-tracker interface shape (`sendMessage` with a time offset, `getLastSample`); not the process |
| OpenSesame ([libsrbox.py](https://github.com/open-cogsci/OpenSesame/blob/nightingale/opensesame_plugins/core/srbox/libsrbox.py)) | Plugins per device; PyGaze for trackers | The main thread reads | Stamp after each byte; PyGaze reports a tracker-minus-host offset | Nothing new |
| jsPsych extensions ([docs](https://www.jspsych.org/latest/developers/extension-development/)) | `initialize`, `on_start`, `on_load`, `on_finish` returning data per trial | the browser's | `performance.now()` in the handler | Per-trial hooks: the player's, not the device layer's |
| E-Prime ([devices](https://support.pstnet.com/hc/en-us/articles/115011298368), [latency](https://support.pstnet.com/hc/en-us/articles/360008833253)) | Devices declared per experiment; input masks per stimulus | closed | PST publishes per-device latency on Windows 11: USB keyboard 14.9 ms (SD 2.6), SRBox 2.5 (0.7), Chronos 0.08 (0.27), PCIe parallel port 0.09 (0.36) | A device table per experiment; latency published per device |
| Presentation ([port input](https://www.neurobs.com/pres_docs/html/03_presentation/06_hardware_interfacing/01_ports/02_port_input/01_port_input_channels.htm), [response_data](https://www.neurobs.com/pres_docs/html/04_reference/03_pcl_reference/04_response_types/07_response_data.htm)) | Port device, input channel, response device, response manager | closed | Serial interrupt per byte; `response_data.unc_dms()` gives each response's uncertainty in 0.1 ms | A per-event uncertainty |
| Lab Streaming Layer ([liblsl](https://github.com/sccn/liblsl)) | Typed streams (format, channels, nominal rate or irregular) from outlets to inlets | library threads | `lsl_local_clock()` is `std::chrono::steady_clock`; `lsl_time_correction` is an NTP-like exchange, 8 probes, the lowest round trip kept, about 0.2 ms wired ([inlet.h](https://github.com/sccn/liblsl/blob/main/include/lsl/inlet.h)); XDF files store clock offsets beside samples and readers refit at load ([spec](https://github.com/sccn/xdf/wiki/Specifications)). Kothe et al. 2025 measured per-device setup offsets of 6 to 20 ms that LSL cannot see and that must be measured per setup ([paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC12434378/)) | Clock offsets stored beside the data; per-setup latency by measurement |

### 2.2 What the survey settles

- **Raw device times plus a session refit.** PsychRTBox's manual,
  PsychDataPixx's post-session line fit and XDF's clock-offset chunks all
  keep raw device times and the clock pairs, and fit after the session.
  ysp does the same (section 6).
- **A per-event uncertainty.** ioHub (`confidence_interval`) and
  Presentation (`unc_dms`) carry one per event. ysp has a per-source
  bound and a per-event tier. Section 7 adds a 16-bit per-event
  uncertainty in the event's 2 reserved bytes.
- **A thread per device, not a process.** Psychtoolbox runs one native
  realtime thread per device. ioHub's process exists because Python has
  a global interpreter lock; it then polls on one OS thread. ysp is C.
- **Identity is unsolved elsewhere.** Psychtoolbox and ioHub address
  devices by enumeration index or one device per type. Only E-Prime's
  device table and PsychoPy's `DeviceManager` name devices, and neither
  survives a replug by a stable key.
- **No vendor timing number is a measurement of the rig.** LSL's paper
  found 6 to 20 ms setup offsets per device. Cedrus states "2 ms" only
  with its D2XX library and "+/-5 ms" otherwise
  ([timing](https://www.cedrus.com/support/xid/timing.htm)).

### 2.3 Device facts that shape the design

| Fact | Source | Consequence |
|---|---|---|
| XID: 6-byte frame `'k'`, info byte (bits 0 to 3 port, bit 4 pressed, bits 5 to 7 key), uint32 LE ms since the last `e5`; `_e5` answers with the timer; outputs `mp` (pulse ms) and `mh` (lines) | [XID commands](https://www.cedrus.com/support/xid/commands.htm), [pyxid2](https://github.com/cedrus-opensource/pyxid/blob/master/pyxid2/constants.py) | The proposal's frame layout is now verified against the vendor and pyxid2; FIT from frames plus BRACKET from `_e5` |
| FTDI latency timer: 16 ms default, 1 to 255 ms; pyxid2 sets 10 ms through D2XX | [FTDI](https://www.ftdichip.com/Support/Knowledgebase/settingacustomdefaultlaten.htm), [pyxid2](https://github.com/cedrus-opensource/pyxid/blob/master/pyxid2/internal.py) | With FIT stamps the timer delays delivery, not accuracy. HOST stamps through FTDI are tier 3 unless the timer is known |
| PST SRBox: 19200 baud, a bit-state byte every 1.25 ms (800 Hz) | [PST](https://support.pstnet.com/hc/en-us/articles/360010840594), [CMUBox.m](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/Psychtoolbox/PsychHardware/CMUBox.m) | FIT by sample count is possible |
| Keyboard-class HID devices are opened exclusively by the Raw Input manager; `ReadFile` is refused | [HID architecture](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/hid/hid-architecture.md) | Keyboard-mode boxes stay on SDL's raw keyboard; `ysp/hid.h` is for other usages only |
| USB polling: full speed 1 ms at best, high speed 125 us | [USB endpoint descriptor](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_endpoint_descriptor) | The floor of any HOST bound for a USB device |
| InpOut's `inpoutx64.sys` is listed as a vulnerable driver; Windows 11 22H2 enables Microsoft's vulnerable-driver blocklist by default; whether that list has InpOut is unverified | [LOLDrivers](https://github.com/magicsword-io/LOLDrivers), [Microsoft](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/microsoft-recommended-driver-block-rules) | `ysp/parallel.h` on Windows 11 is at risk; USB trigger boxes and DAQs first |
| Tobii Pro SDK: `system_time_stamp` is the host clock (the clock of `tobii_research_get_system_time_stamp`), synced by Cristian's algorithm; research license is non-commercial | [Tobii timing](https://developer.tobiipro.com/commonconcepts/timestamp-and-timing.html), [license](https://www.tobii.com/products/integration/tobii-sdk-license) | SDK stamp source; correlate its clock against ysp_rt once (`yrt_correlate`) |
| EyeLink: tracker ms (0.5 ms flag at 2 kHz); `eyelink_double_usec_offset()`; vendor real-time latency 1.4 ms at 2 kHz | [PTB source](https://github.com/Psychtoolbox-3/Psychtoolbox-3/blob/master/PsychSourceGL/Source/Common/Eyelink/EyelinkCreateDataStructs.c), [SR Research](https://www.sr-research.com/eyelink-1000-plus-technical-specifications/) | SDK offset or BRACKET; messages out |
| Gazepoint: TCP XML on port 4242; `TIME_TICK` "CPU ticks" with a frequency (QPC is inferred, unverified) | [API v2.0](https://www.gazept.com/dl/Gazepoint_API_v2.0.pdf) | If QPC, `yrt_ticks_to_ns()` maps it exactly |
| Pupil Neon: Unix-epoch ns, NTP-disciplined; a Time Echo protocol | [realtime API](https://github.com/pupil-labs/realtime-python-api/blob/main/src/pupil_labs/realtime_api/time_echo.py) | BRACKET over TCP |
| VPixx Pixel Mode: output from the video signal, ahead of light by a fixed amount (one frame on PROPixx at 120 Hz) | [VPixx](https://docs.vpixx.com/vocal/sending-triggers-with-pixel-mode) | A per-rig offset in the rig profile, by loopback |
| BioSemi: trigger inputs sampled into the Status channel (one-sample jitter); the USB trigger cable holds a byte 8 ms | [BioSemi](https://www.biosemi.com/faq/trigger_signals.htm), [cable](https://www.biosemi.com/faq/USB%20Trigger%20interface%20cable.htm) | Output families state their hold time |
| Brain Products TriggerBox: write 0 after each code to re-arm; vendor test max deviation 0.46 ms against LPT | [Brain Products](https://pressrelease.brainproducts.com/triggerbox-tips/) | Encoder includes the idle write |
| LabJack T4/T7 command-response: 0.7 ms through a high-speed hub, 2.3 ms direct USB | [LabJack](https://support.labjack.com/docs/a-1-data-rates-t-series-datasheet) | DAQ outputs are tier 2 at best, by loopback |
| WM_POINTER `PerformanceCount` is QPC at message receipt | [POINTER_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-pointer_info) | Touch and pen could leave tier 3 if SDL exposed it (later, `ysp/screen.h`) |
| Timing mega-study: MilliKey (1 kHz HID) as the response device; Windows 10 lags 9 to 12 ms across Presentation, E-Prime, PsychoPy | [Bridges et al. 2020](https://pmc.ncbi.nlm.nih.gov/articles/PMC7512138/) | A 1 kHz HID box on SDL's raw path is a first-class response device |

## 3. Principles for devices

These add to `docs/rig_spec.md` section 1.

1. **One clock, converted at the boundary.** A device time becomes a
   ysp_rt time in the device layer, never in the experiment. The raw
   device time and the clock pairs that converted it go in the log, so
   the analysis can fit again over the whole session.
2. **The stamp states its evidence.** Each device instance has a source
   entry (`yin_source`): a tier, measured bounds, and whether the bounds
   cover the device's own delay. A device without a loopback measurement
   is tier UNKNOWN, whatever its manual claims.
3. **Protocols are pure.** A decoder takes bytes and a host read time and
   gives events and clock pairs. It does no I/O, has no thread and does
   not allocate. Its test runs on captured bytes.
4. **Vendor libraries are loaded, not linked.** Glue for a vendor SDK
   loads the vendor's library at run time (`LoadLibrary`, `dlopen`), as
   `ysp/parallel.h` loads InpOut. The player builds and runs without it.
   ysp never redistributes a vendor binary.
5. **No allocation after start.** Each instance owns fixed buffers sized
   at open. Rings are caller memory.
6. **Every device and every change is in the log.** Open, identity, the
   source entry, each clock fit, each loss and reconnect, and each output
   write with its bracketing times.
7. **A role, not a port.** Experiments name roles. Only the rig profile
   names ports, serial numbers and paths.

## 4. The model

### 4.1 One instance, seven parts

```
                       rig profile: role "resp" -> key "serial:0403:6001:FT4ABC:A"
                                          |
  physical device --transport--> bytes --+--> protocol decoder (pure)
                    (ysp_serial,          |        |  events with device ticks
                     ysp_hid, ysp_net,    |        |  clock pairs (ticks, host ns)
                     vendor SDK)          |        v
                                          |   clock fit (ysp/rt.h) --> ysp_rt ns, tier, bounds
                                          |        |
                    reader thread  <------+        v
                    (one per instance)        sink: yscr_push_input()  (events)
                                              stream ring                (samples)
                                              log ring                   (records)

  output: ydev_out(role, code) --encoder (pure)--> transport write, bracketed, logged
          ysp/screen.h trigger channel --> the same call at the planned vblank
```

A **family** is one kind of device: a protocol and its clock rules
("xid", "srbox", "line", "photodiode", "triggerbox", "eyelink", "lsl").
An **instance** is one physical device of a family, open on one
transport. A **role** is the experiment's name for an instance.

### 4.2 Transport

A transport moves units. The unit decides how the stamp is taken:

| Transport | Unit | Stamp | Header |
|---|---|---|---|
| Serial (RS-232, USB-serial, USB-CDC) | bytes | after the blocking read returns; one stamp for all bytes of one read | `ysp/serial.h` (exists) |
| Parallel (LPT) | a byte on the data lines; status lines in | polled; output bracketed | `ysp/parallel.h` (exists) |
| HID, keyboard or mouse class | Raw Input reports | SDL's raw keyboard thread; ysp/screen.h's raw mice thread | `ysp/screen.h` (exists) |
| HID, other usages (gamepads, joysticks) | SDL joystick and gamepad events | SDL's | `ysp/screen.h` with `desc.gamepads` (exists, not run) |
| HID, vendor-defined usages | input reports | after the overlapped read completes | `ysp/hid.h` (new, later) |
| TCP, UDP | bytes, datagrams | after the receive returns | `ysp/net.h` (planned in rig_spec 4.7) |
| LSL | samples with the sender's time stamps and an offset estimate | the sender's, converted | `ysp/net.h`, liblsl loaded at run time |
| Vendor SDK | samples, events, messages | the SDK's, converted | one glue header per vendor, library loaded at run time |
| Audio capture | sample blocks with device frame positions | ysp/audio.h's fit of the capture clock | `ysp/audio.h` capture (planned, rig_spec 14.7 rank 7) |

USB bulk through WinUSB or libusb is not a transport here (section 13).

### 4.3 Protocol

A decoder is a state struct and one function:

```c
/* ysp/box.h, draft */
int ybox_decode(ybox_state* st, const uint8_t* bytes, size_t n,
                  int64_t t_read_ns, ybox_out* out);
```

It keeps a partial frame between calls in a fixed buffer. It writes
events (`yin_event`, with the raw device ticks when the protocol has a
clock) and clock pairs (device ticks, host read time) into `out`, whose
arrays the caller sizes. It reports garbage bytes and resynchronizations
as counts, never as events. An encoder is the reverse for commands and
output codes:

```c
int ybox_encode(const ybox_state* st, int op, uint32_t arg,
                  uint8_t* bytes, size_t cap);
```

A family that speaks through a vendor library has no decoder in
`ysp/box.h`: the library is the protocol.

### 4.4 Clock

Every event leaves the device layer with `t` on the ysp_rt clock. The
layer takes `t` from one of four stamp sources, and the source entry says
which:

| Stamp source | What `t` is | Typical families | Tier before a loopback | Tier after a loopback |
|---|---|---|---|---|
| HOST | the host's time when the read returned | byte-per-press boxes, HID boxes, a scanner pulse as a key | UNKNOWN, `partial` (3 if a known quantizer is in the path, such as a 16 ms FTDI latency timer) | 2, with bounds |
| FIT | the device's own time, mapped by a fit of (device ticks, host read time) pairs | XID boxes, the ysp line protocol, the photodiode frame, EyeLink, StimTracker | UNKNOWN | 1, if the loopback checks the map within the fit's bound |
| BRACKET | the device's time read by a round trip; each pair has a width (the round-trip time) | Cedrus XID (`_e5`), VPixx, EyeLink, Pupil Labs, any device that answers "what time is it" | UNKNOWN | 1 or 2 |
| SDK | the vendor library gives host time already (Tobii's system time stamp, LSL's corrected time) | Tobii Pro, LSL inlets | UNKNOWN | as measured |

The FIT and BRACKET estimators belong in `ysp/rt.h` (section 6), because
three places already need one: `ysp/audio.h` has its own lower-envelope
fit, `tests/loopback/screen_loopback.c` has an ad hoc one, and the
box proposal had a third.

### 4.5 Direction

One instance can read and write: a Cedrus c-pod or StimTracker, a
LabJack, an Arduino, VPixx hardware, EyeLink (samples in, messages out).
So input and output are two capabilities of one instance, not two
models. Outputs are described in section 8.

### 4.6 Identity

A **match key** names a physical device in a way that survives a replug
and a reboot. Its form depends on the transport:

| Transport | Key | Source | Gap |
|---|---|---|---|
| Serial | `serial:<vid>:<pid>:<serial>:<location>` | `yser_find_ports()` | Two FTDI cables with blank EEPROMs on Windows look the same (docs/serial.md) |
| SDL keyboard, mouse | `rawinput:<vid>:<pid>:<instance path hash>` | `GetRawInputDeviceInfo(RIDI_DEVICENAME)` on SDL's `which`, which is the Raw Input handle on Windows (docs/screen.md "Devices") | SDL 3.4 gives a name only (`SDL_GetKeyboardNameForID`); ysp/screen.h must add the path to its device record |
| SDL gamepad, joystick | `joystick:<vid>:<pid>:<serial>` or the path | `SDL_GetJoystickVendor`, `_Product`, `_Serial`, `_Path` (SDL 3.4 headers) | Many pads have no serial |
| HID | `hid:<vid>:<pid>:<serial>:<usage page>:<usage>:<interface>` | SetupAPI and `HidD_*` (Windows), sysfs (Linux) | As serial |
| Network | `tcp:<host>:<port>` plus the identity the device reports | the device | none |
| Vendor SDK | `<vendor>:<serial or address>` | the SDK | per SDK |

Besides the key, the family's **identify** step asks the device who it
is, where the protocol allows it (an XID model query, a tracker's
version string), and the log records the answer. A key that matches the
wrong kind of device fails at identify, before the session starts.

Two identical devices with no serial number (two keyboards of one model)
cannot be told apart by key across a reboot. The rig profile then binds
by activity: the player asks the operator to press a button on the
device for each role (section 11.3).

### 4.7 Lifecycle

```
CLOSED --open--> OPENING --identified--> RUNNING --stop--> CLOSED
                    |                      |
                    | wrong device         | disconnected, or no data for the
                    v                      v family's timeout
                  FAILED                 LOST --key seen again, reopened,
                                           ^     identified--> RUNNING
                                           +--- retry every 1 s
```

- Each transition is a log record with the time and the reason.
- LOST to RUNNING restarts the clock fit: a device's clock can reset on
  power-up. The first pairs after a reconnect are stamped HOST and
  flagged until the fit has its minimum span.
- A LOST interval is a gap record (first and last time). No event is
  invented for it.
- The device layer does not decide what a loss means for the
  experiment. The definition marks a role `required`; the player then
  refuses to start without it and marks a trial during which it was
  LOST (section 11).
- A disconnect must never look like silence (docs/serial.md
  "Disconnect mapping"). A family with a heartbeat (XID boxes do not
  have one; trackers do) also goes LOST after its timeout.

### 4.8 Producer: threads, the bridge and streams

Each instance has its own reader thread by default. Reasons:

- Blocking reads are the simplest correct model, and `ysp/serial.h` is
  built for it (`yser_interrupt()` stops a blocked read).
- Isolation: some USB-serial drivers never complete a read that was
  pending at unplug (docs/serial.md). Only that instance's thread
  hangs, and its interrupt ends it.
- A rig has a handful of devices. A thread costs about 1 MB of reserved
  stack and no CPU while blocked.

A shared reader thread (`WaitForMultipleObjects` over overlapped handles)
is a later optimization, only if a measurement shows a need.

Vendor libraries that call back on their own threads (Tobii) or that
want a poll (EyeLink) use the same emit path as the reader: one function
that maps the time, writes the event and the log record.

The reader stamps first, then parses. It runs at `yrt_thread_elevate()`
priority. Where it runs (P-core or E-core) is a measurement for the first
device: the trigger worker's rule (keep spinning threads off the vblank
DPC's CPU) does not apply to a thread that blocks, but the wake-up
latency of a blocked thread on each core type is not measured.

Two paths leave the reader:

| Path | For | Rate | Consumer |
|---|---|---|---|
| Sink, default `yscr_push_input()` | discrete events: presses, releases, pulses, edges, eye events; samples the frame thread must see | up to about 1 kHz per device | the frame loop through `yscr_poll()` and `yscr_event_input()`; `ysp/response.h` |
| Stream ring (a `yrt_ring` in caller memory) | every sample: gaze, force, analog, a mouse trace | any; the ring is sized for the longest drain stall (docs/rt.md) | the data writer; a "newest sample" slot for gaze-contingent display |

The bridge costs 1.9 to 2.6 us per push with a window and was measured
with 4 producers at about 1 kHz each (docs/screen.md). A 2 kHz
binocular tracker through it would cost about 5 ms of CPU per second and
30 to 35 doorbells per 60 Hz frame. That works, but it is waste for
samples nobody in the frame loop reads. So samples go to the stream; the
bridge gets events, and samples only when the experiment asks for them
(a gaze-contingent trial, a joystick trace for `ysp/response.h`
crossings). Not measured: the bridge above 4 kHz in total.

## 5. Headers: where each part lives

| Header | State | Adds | Needs |
|---|---|---|---|
| `ysp/rt.h` | v0.5.0 exists; v0.6 proposed | The device clock fit: FIT (lower envelope of late pairs) and BRACKET (minimum round trip) estimators, tick unwrapping, bounded memory; `YRT_SRC_DEVICE` (12) reserved | OS |
| `ysp/input.h` | v0.2.0 exists; v0.3.0 proposed | `YIN_KIND_SYNC` (7); the raw device ticks in the event's tail padding; `YIN_DEVTICKS` flag; eye-event controls | nothing |
| `ysp/box.h` | v0.2.0, built 2026-10-08 | Pure decoders, query bytes and output encoders for byte-stream devices: XID, the ysp line protocol, the photodiode frame, TriggerBox, BioSemi, MMBT-S, DTR and RTS, the parallel port (section 10) | `ysp/input.h` |
| `ysp/device.h` | v0.2.0, built 2026-10-08 (inputs, outputs, the trigger channel, roles); stream rings and the analog edge detector later | Instance, match keys, identify, lifecycle with reconnect, a reader thread or manual polling, a BRACKET fit with timer queries, a sink, log records | `ysp/rt.h`, `ysp/input.h`, `ysp/box.h`, `ysp/serial.h` (required: every step-2 family is serial; `desc.transport` replaces it). The bridge is the caller's sink, not glue in the header |
| `ysp/json.h` | v0.1.0, built 2026-10-09 | The strict JSON reader and canonical writer of the repository: the rig profile, the pack tool, later the experiment definition if it is JSON | nothing |
| `ysp/rigfile.h` | v0.1.0, built 2026-10-09 | The rig profile (11.2): load with errors that name the field, the SHA-256 check of each loopback file, the canonical write and the profile's hash, a role's desc and start, a binding, a stored loopback result | `ysp/device.h`, `ysp/json.h` |
| `ysp/screen.h` | v0.4.0 exists | Device path and match key in `YSCR_EV_DEVICE`; nothing else | SDL3 |
| `ysp/hid.h` | new, later | HID enumeration, input reports on overlapped reads, output and feature reports | OS (SetupAPI and hid.dll; hidraw; IOKit on macOS) |
| `ysp/net.h` | v0.1.0, built 2026-10-09 (LSL only) | LSL inlets and outlets with liblsl loaded at run time (docs/net.md); later: TCP and UDP transports, cross-machine clock offsets | `ysp/rt.h`, `ysp/input.h`; liblsl at run time |
| `ysp/audio.h` | v0.2.0 exists | Capture (planned): sample blocks on the capture fit, for the edge detector | miniaudio |
| `ysp/eyelink.h`, `ysp/tobii.h`, `ysp/labjack.h`, `ysp/daqmx.h`, `ysp/dpx.h` | new, later, one each | Vendor glue: the vendor library loaded at run time, only the functions used declared | the vendor library at run time |

Why `ysp/device.h` and `ysp/box.h` are two headers: a decoder is pure
computation and is fuzzed on bytes; the device layer has threads and OS
calls. A program that replays a capture needs only `ysp/box.h`. The
analysis side (a Python binding that decodes a raw capture) needs only
`ysp/box.h`.

Why the rig profile is `ysp/rigfile.h` and not part of `ysp/device.h`: the
profile is the rig's, not only its devices' (the display's onset offset
and calibration are in it too); it is file I/O, hashing and a parser,
which the device layer's run-time path does not need; and a change of the
file format then does not change the device layer's version. It includes
`ysp/device.h` (one direction) because binding fills a `ydev_desc` and
the family names are `ysp/box.h`'s.

Why the fit is in `ysp/rt.h` and not in a new header: every device header
already needs `ysp/rt.h`, the fit is about 300 lines, and the clock
correlation it extends is there. docs/rt.md deferred the drift fit to
`ysp/audio.h` until a second user existed; there are now two more.
`ysp/audio.h` can move to it later if a measurement shows equal results.

What is example glue, not a header:
- The sink that pushes to the bridge is one line (`yscr_push_input`),
  and `ysp/device.h` defines it inline when `ysp/screen.h` came first.
- Firmware for a DIY box and for the photodiode (an Arduino or Teensy
  sketch) is in `firmware/` (`firmware/ysp_line/`), not a header.
- A data writer that drains the stream rings to a file.

What is the player's:
- Reading the device table; binding its roles through the rig profile
  (`ysp/rigfile.h` reads, checks and writes the profile); the binding
  procedure; refusing to start.
- What a LOST required device does to a trial.
- The data file's layout (section 11.4).
- The runner protocol's device messages for the designer.

## 6. Reference: the clock fit (`ysp/rt.h` v0.6.0, built)

Built on 2026-10-08 (step 1 of section 14.2). The header's manual
(DEVICE CLOCK FIT) is the reference; `docs/rt.md` has the decisions
and the measured error on synthetic devices. In short:

| Item | As built |
|---|---|
| API | `yrt_fit_open(f, desc)` (returns true, or false with the reason in `yrt_fit_error(f)`), `yrt_fit_add(f, ticks, host_ns, width_ns)` (returns `YRT_FIT_POINT`, `_REFIT`, `_RESTART`, `_REJECTED`, `_PENDING` bits), `yrt_fit_map(f, ticks)` (0 before the first pair), `yrt_fit_unwrap()`, `yrt_fit_get(f, &info)` |
| Modes | LATE: the lower envelope (the hull edge spanning the mean x). BRACKET: LATE on the bracket ends (`host_ns` = t1), brackets over `max_width_ns` (1.3 ms) refused, so per bucket the smallest t1 minus device time wins. UNBIASED: bucket means, least squares |
| Defaults | bucket 200 ms; slope nominal until the points span 5 s (the draft said 60 s; `ysp/audio.h` measured 5 s); hold a pair more than 50 ms off the map, restart when held pairs agree for 1 s |
| Memory | 1024 points in the struct, about 40 KB, no allocation |
| Records | `YRT_KIND_CLOCK` per closed bucket (ticks, width, epoch), `YRT_KIND_FIT` (8, new) per refit at a new bucket; `aux` = `desc.clock_id` |
| Info | anchor, fitted tick and ppm, p99 spread, BRACKET width, span, points, pairs, epoch, rejected, pending, ready, slope |
| Error on synthetic devices | Dense LATE pairs: tens of us after a few minutes (-47 to +91 us for a ms timer behind a modeled USB path). Sparse pairs behind a 16 ms timer: ms for minutes (up to +8.7 ms at 2 min). BRACKET: -42 to +110 us with a full window. Rates within 0.9 ppm |
| Cost | 0.1 us a pair; a refit 9 to 23 us at p50, about 5 a second |

Differences from this section's draft of the same day: `yrt_fit_open`
returns a code; `desc.tick_bits` is `uint32_t`, `desc.mode` an `int`;
`restart_hold_ns`, `clock_id` and `ring` were added; the info gives the
anchor (`t0_ns`, `ticks0`) instead of "the time of tick 0", because the
anchor is where the map is exact. `screen_loopback.c` uses the fit
instead of its own line.

## 7. Reference: input events (`ysp/input.h` v0.3.0, built)

The event stays 48 bytes. Changes:

| Change | Detail | Why |
|---|---|---|
| `YIN_KIND_SYNC = 7` | Scanner volume pulses, TTL inputs, photodiode edges, sound-key edges: timing events that are not a participant's response | A scanner pulse is not a key press. A collector should not take one as a response, and a data file should not mix them. Kind 7 is free today |
| `uint16_t unc_us` at offset 26 (today `reserved_`) | This event's uncertainty in us: the fit's spread at this time, a BRACKET pair's width, a HOST stamp's known quantizer; 0 = the source entry's bounds; 65535 = 65.5 ms or more | ioHub and Presentation carry one per event (section 2.2). A fit's spread changes over a session; the source entry holds one bound for all of it |
| `uint32_t ticks` at offset 44 | The low 32 bits of the device's own clock for this event, in the source's unit; valid when `flags & YIN_DEVTICKS` (0x10) | Bytes 44 to 47 are tail padding today (the test pins `aux` at 40 and the size at 48). The raw time lets the analysis map the event again with an offline fit. 32 bits are enough because the log's clock pairs carry the full counter |
| Eye controls | `YIN_EYE_GAZE` (SAMPLE: x, y window pixels, value pupil), `_FIXATION`, `_SACCADE`, `_BLINK` (PRESS at start, RELEASE at end), PROXIMITY for track lost and found; `device` the instance, `code` the eye (0 left, 1 right, 2 both) | The existing types cover it: a blink is a press of the BLINK control |
| Response boxes | `YIN_KIND_BOX`, `control` the button (1-based), `code` the port or bank | As today |

`ysp/response.h` v0.1.4: `YRSP_KINDS_ALL` excludes SYNC; a mask that
names the SYNC bit, or a LIST choice of kind SYNC, takes it (section 16,
decision 3).

The source entry (`yin_source`) does not change. The device layer
writes one per instance: tier, bounds, `partial`, and a note that names
the stamp source (HOST, FIT, BRACKET, SDK) and the loopback that set the
bounds.

## 8. Reference: outputs and triggers

Outputs use the same instance, role and log as inputs. Three calls:

| Call | Effect | Record |
|---|---|---|
| `ydev_out_set(dev, code)` | Write a code now (a TTL byte, a marker) | role, code, time before the write, time after |
| `ydev_out_pulse(dev, code, width_ns)` | A code, then the idle code after `width_ns`. The device times the pulse when the family can (a TriggerBox Plus, a Cedrus c-pod, unverified for both); else `yser_pulse_async()` or `ypar_pulse_async()` | as above, and the trailing edge's times |
| `ydev_out_mark(dev, code, text)` | A message for devices that take text: EyeLink messages, LSL string markers | as above, with the text in the log |

- **At the flip.** `ysp/screen.h` trigger channels already run a callback
  at the planned vblank on a deadline worker (`desc.triggers`). The device
  layer supplies that callback: a channel's context is the output
  instance. The timeline's TRIGGER events name a role, and the player
  maps the role to its channel. No second scheduler.
- **Codes by the display.** VPixx Pixel Mode and pixel sync are
  `ysp/screen.h` codes (exist, docs/screen.md "Flip hooks"). The
  VPixx device then outputs on its own clock, locked to the video, which
  is the best output timing a rig can have. The device layer only binds
  the role and logs it.
- **Tiers for outputs.** The same scale. A host write bracketed in
  software is tier UNKNOWN until a loopback measures write-to-edge; then
  2 with bounds. A video-locked output (VPixx) can be 1 after a loopback.
- **Latency by loopback, per device and per rig.** Each output role gets
  a measured distribution (section 12) stored in the rig profile and
  copied to the data file. A latency is never taken from a manual.

## 9. Reference: identity, lifecycle and log records

`YRT_SRC_DEVICE` (12) records, one 64-byte ring record each:

| Kind | When | Payload |
|---|---|---|
| `DEVICE_OPEN` | identified | role index, family id, kind, instance id; the key's hash; then `MESSAGE` records with the key, model and firmware text (40 bytes each) |
| `DEVICE_SOURCE` | at open and when bounds change | tier, partial, lo_ns, hi_ns, stamp source |
| `DEVICE_STATE` | each lifecycle change | old state, new state, reason code, OS error |
| `DEVICE_GAP` | LOST to RUNNING | first and last time of the gap |
| `DEVICE_FIT` | each refit, at most 1 per second | offset, ppm, spread, points, epoch |
| `YRT_KIND_CLOCK` | thinned clock pairs | as defined in ysp/rt.h; `aux` the clock number |
| `DEVICE_OUT` | each output write | role index, code, time before, time after, flags (device-timed, flushed). Built as `YDEV_REC_OUT` (5): also the width, the flip deadline, the output's number, and flags for the trailing edge, a failure, a mark, a replaced edge and a board's own report (docs/device.md) |
| `DEVICE_GARBAGE` | a decoder resynchronized | bytes skipped, total |

Events themselves go through the sink and the stream rings, not as
device records. The player's writer joins them by role index.

## 10. Reference: device families

"Home" is where the family's code goes. "Verify" says what the user's
hardware can check now.

### 10.1 Input families

| Family | Devices | Transport | Protocol | Clock | Home | Verify |
|---|---|---|---|---|---|---|
| Keyboard-mode boxes | MilliKey, fORP in keyboard mode, BBTK pads, many button boxes, scanner pulse as a key | HID keyboard, SDL raw keyboard | USB HID | HOST (SDL's raw thread) | `ysp/screen.h` (exists); role binding by match key | Yes: the Dell receiver and the built-in keyboard (replug, binding by activity) |
| Mouse-class devices | mice, trackballs, a second mouse as a response device | Raw Input | USB HID | HOST (one stamp per `GetRawInputBuffer`) | `ysp/screen.h` raw mice (exists) | Yes: TrackPoint, touchpad, Dell mouse coming apart (docs/screen.md lists this as not verified) |
| Gamepads, joysticks, HID-joystick boxes | Xbox pads, HID joystick-mode boxes | SDL | SDL | HOST, SDL's driver | `ysp/screen.h` `desc.gamepads` | No pad on hand |
| Cedrus XID | RB-x40, Lumina, c-pod, m-pod, StimTracker | USB-serial (FTDI) | XID frames (6 bytes; StimTracker 2: 9 bytes) | FIT from frames, BRACKET from `_e5` queries (the box's ms timer) | `ysp/box.h` decoder, `ysp/device.h` | No box: decoder on published or captured bytes only |
| Bit-state boxes | PST Serial Response Box, other byte-per-sample boxes | serial | one byte per sample, one bit per button | HOST; FIT by sample count when the box streams at a fixed rate | `ysp/box.h` | No |
| Byte-per-press boxes | BITSI, simple DIY boxes | serial | a byte per press or release | HOST | `ysp/box.h` | Through a DIY board |
| ysp line protocol | DIY Arduino, Teensy, RP2040 boards with ysp's firmware | USB-CDC | lines `S <t>`, `E <t> <ch> <0|1>`, `A <t> <ch> <v>`, `Q <seq> <t>`, `I <text>` (docs/box.md says why not the comma form first drafted here) | BRACKET (syncs, edges and query answers) | `ysp/box.h`, firmware in `firmware/ysp_line/` | With any such board |
| Photodiode frame | the photodiode board of `screen_loopback.c` | USB-CDC | 6-byte frames, `0xA5` edge, `0x5A` sync | FIT | `ysp/box.h` | Yes, when the photodiode comes, if it is on a microcontroller (section 16) |
| Analog edges | photodiode or microphone on line-in; force sensor or photodiode on a DAQ or a board's ADC | audio capture; DAQ; serial | sample blocks | the capture fit, the DAQ's clock | `ysp/device.h` edge detector (pure) on `ysp/audio.h` capture or a DAQ glue | Line-in, when capture exists |
| HID vendor devices | Teensy raw HID, response pads in generic HID mode | HID | per device | HOST, or FIT if the report carries a time | `ysp/hid.h`, decoder in `ysp/box.h` | With a Teensy |
| LSL inlets | any LSL stream: EEG, physiology, trackers with LSL apps | LSL | LSL | SDK (LSL's offset estimate) | `ysp/net.h` | Yes: a local outlet on the same laptop |
| EyeLink | EyeLink 1000 Plus, Portable Duo | Ethernet, vendor library | vendor | BRACKET or the SDK's offset | `ysp/eyelink.h` | No tracker |
| Tobii Pro | Spectrum, Fusion, Spark | USB or Ethernet, vendor library | vendor | SDK (system time stamps) | `ysp/tobii.h` | No tracker |
| Gazepoint | GP3 | TCP, XML (Open Gaze API, port 4242) | text | SDK if `TIME_TICK` is QPC (unverified), else HOST | `ysp/net.h` and `ysp/box.h` | No |
| Pupil Labs | Core (ZMQ and msgpack), Neon (real-time API) | network | vendor | BRACKET (Core's `t` query; Neon's Time Echo) | later; or through their LSL app | No |
| VPixx inputs | DATAPixx digital and analog in, button boxes on it | USB, libdpx | vendor | the device's clock, BRACKET | `ysp/dpx.h` | No |
| Tablets, touch | Wacom, touch screens | SDL pen and touch | SDL | message time (tier 3 today) | `ysp/screen.h` | Touchpad only (it is a mouse to Windows) |

### 10.2 Output families

| Family | Devices | Transport | Encoding | Home | Verify |
|---|---|---|---|---|---|
| Parallel port | LPT cards, EEG amplifiers' parallel inputs | LPT | the byte on the data lines | `ysp/parallel.h` (exists); on Windows 11 the InpOut driver may be blocked (section 2.3) | No LPT on the laptop |
| Byte-to-TTL | Brain Products TriggerBox, BioSemi USB trigger, Neurospec MMBT-S, BBTK TTL modules | USB-serial | one byte | `ysp/serial.h` (exists); `ysp/box.h` encoder for each | No |
| XID outputs | c-pod, StimTracker | USB-serial | XID commands | `ysp/box.h` encoder | No |
| DIY boards | Arduino, Teensy with ysp firmware | USB-CDC or raw HID | ysp line protocol | `ysp/box.h` | With a board |
| DAQ | LabJack U3, U6, T4, T7; NI-DAQmx; MCC | vendor library | vendor | `ysp/labjack.h`, `ysp/daqmx.h` | No |
| Video-locked | VPixx Pixel Mode, pixel sync | the display | `ysp/screen.h` codes (exist) | `ysp/screen.h`, `ysp/dpx.h` for the device side | No |
| Markers | LSL string or int markers; EyeLink messages | LSL; vendor | text or int | `ysp/net.h`; `ysp/eyelink.h` | LSL: yes, locally |
| Serial lines | DTR or RTS as a TTL | serial | a line state | `ysp/serial.h` `yser_set_dtr` (exists) | With a USB-serial adapter |

## 11. The player and the designer

### 11.1 The device table in the pack

The experiment definition lists roles. It names no port, path or serial
number, so the pack runs on any rig that can bind its roles.
The definition's file format is not decided (rig_spec 6); YAML below is
for reading only.

```yaml
devices:
  - role: resp
    family: [xid, keyboard]     # either binds; the first that the rig has
    direction: in
    buttons: { 1: left, 2: right }
    required: true
    min_tier: 2
  - role: trig
    family: [parallel, triggerbox, xid]
    direction: out
    pulse: 0.002                # seconds (decided 2026-10-07)
    required: true
  - role: scanner
    family: [keyboard, xid, line]
    direction: in
    kind: sync
    map: { key: "5" }           # a keyboard-mode scanner pulse
  - role: eye
    family: [eyelink, tobii]
    direction: both
    required: false
```

Scripts and the definition use roles: `input.button{device = "resp",
choices = {"left", "right"}}`, `trigger("trig", 12)`,
`await(input.sync{device = "scanner", count = 1})`.

The designer builds this table from a Devices panel. Each family's
parameters come from its parameter table (rig_spec principle 8), so the
panel and the script binding have one source.

### 11.2 The rig profile

A file on the rig, not in the pack, like the display calibration
(`.yspcal`). It lives in the per-user config folder,
`yrt_user_dir(YRT_DIR_CONFIG, "rig", ...)` (`ysp/rt.h` v0.7.0): for
example `%APPDATA%\ysp\rig` on Windows and `~/.config/ysp/rig` on Linux.
That function refuses a folder other users can write, so a planted profile
cannot bind a role to another device or state a false latency.

| Field | Example |
|---|---|
| role binding | `resp -> serial:0403:6001:FT4ABC:A, family xid` |
| family options | baud, FTDI latency timer state, button map |
| measured latency per role | output write-to-edge distribution; input stamp-minus-event bounds; the loopback file's hash and date |
| display onset offset | `desc.onset_offset_ns` from the photodiode run |

The player reads both files, enumerates devices, binds each role, runs
each family's identify step, and refuses to start when a required role
is unbound, fails identify, or is below `min_tier`. The refusal names the
role and what was found.

#### The rig profile file (decided 2026-10-09, user; built)

Proposed in step 3; the user accepted it on 2026-10-09. Built as
`ysp/rigfile.h` v0.1.0 on `ysp/json.h` v0.1.0 (docs/device.md, "The rig
profile"; docs/json.md). The format of the experiment definition is not
decided (rig_spec 6); the profile should use the same syntax.

- **Fields.** `format` ("ysp-rig 1"); `rig` (a name); `written` (UTC);
  per role: `family`, `key`, `options` (baud, `latched`, the FTDI latency
  timer as measured, a button map), `pulse_s` if the rig fixes it, and
  `latency`: the loopback summary (`n`, `median_s`, `p5_s`, `p95_s`,
  `max_s`, `date`) with the result file's name and `sha256`; for inputs,
  `bounds` (`lo_s`, `hi_s`) from their loopback the same way; `display`:
  `onset_offset_s` and the `.yspcal` file's name and `sha256`. Durations
  in seconds.
- **Format: JSON, written in one canonical form** (keys in a fixed
  order, numbers with a fixed precision). Reasons: tools write most of
  the profile (the binding by activity, the loopback runs), so comments
  matter little; the designer and the runner protocol speak JSON already
  (rig_spec 6, the trace export); a strict grammar needs a small parser
  in the player and none in the browser; a canonical form gives a stable
  hash for the data file. If the definition goes to TOML (comments, hand
  edits), the profile should follow it rather than add a second syntax.
  YAML is not recommended: its implicit typing changes values (`no`,
  `1e3`) and its parser is large.
- **Loopback results.** The tool's own file (`device_out_latency` writes
  one now, "name value" lines in seconds) is kept beside the profile in
  `loopback/` and named in the profile by its SHA-256. The player
  checks each hash at load: a missing or changed file makes that role
  tier UNKNOWN and is logged, not silently used. The data file header
  copies each role's summary and hash and the profile's own hash.
- **Location.** `yrt_user_dir(YRT_DIR_CONFIG, "rig", ...)`:
  `%APPDATA%\ysp\rig\profile.json` and `...\rig\loopback\` on Windows,
  `~/.config/ysp/rig/` on Linux. That function refuses a folder other
  users can write, so a planted profile cannot bind a role to another
  device or state a false latency. A machine that hosts two rigs keeps
  `<name>.json` beside `profile.json`, and the player takes `--rig NAME`.

What the build settled (2026-10-09):

- **Canonical form**: the pack manifest's (keys in bytewise order, two
  spaces, LF, one final LF), written by `ysp/json.h`; seconds as the
  exact decimal of the nanoseconds with no trailing zeros, read back
  exactly. A default is left out. The profile's hash is the SHA-256 of
  these bytes, so whitespace or another spelling of a number in a hand
  edit does not change it.
- **Strict reading**: an unknown key at any level is refused, so a
  misspelled field is an error, not a default. Each error names the
  field path, the line and the column.
- **The file's name**: a loopback result is `loopback/<sha256>.txt`; the
  profile stores the hash only, so the name and the hash cannot disagree.
- **A fourth check result**: DIFFERS, the file is genuine but the
  profile's summary is not the file's (an output latency file of
  `device_out_latency` is compared field by field).
- **The tier**: tier 1 (ysp/input.h: a device clock mapped by a fit that
  a loopback checked) only for an input family with a device clock whose
  `bounds` file checks; every other role, and every role whose file is
  missing, changed or differs, gives `YIN_TIER_UNKNOWN`. No output has a
  tier.
- **Options**: `baud`, `latched` (MMBT-S only), `ftdi_latency_s`,
  `buttons` (code to name, at most 16). A new binding drops the role's
  latency and bounds, which belonged to the device it had.
- **The two tools** (2026-10-13, `ysp/rigfile.h` v0.4.0; docs/device.md,
  "The rig profile tools"): `rigfile_bounds` measures an input's bounds
  against a second line board's own reports of its pin (not the line-in:
  its capture times have no loopback check yet), lo and hi the least and
  most difference; `rigfile_onset` takes the CSV of a photodiode run and
  stores the median as `onset_offset_s`. The display gets `latency`, the
  run's summary and hash, checked at load like a role's; an offset that is
  not the file's DIFFERS.

### 11.3 Binding

Through the runner protocol, the designer's Run view lists the rig's
enumerated devices (key, name, family guess) and the pack's roles. The
operator binds a role by choosing a device, or by activity: "press a
button on the response device" binds the first device that sends a
press. Activity binding is the only way to tell apart two keyboards of
one model with no serial number. The binding is saved in the rig
profile.

### 11.4 The data file

- **Header**: one row per role: role, family, direction, key, model and
  firmware as the device reported them, kind, instance id, stamp source,
  tier, bounds, `partial`, the loopback reference, the clock fit at start.
  The rows are the device list of the session.
- **Event rows** (responses, sync events, outputs): `t` (seconds, ysp_rt
  clock), role, kind, control, type, value, x, y, stamp tier, device
  ticks (when the flag is set).
- **Streams**: one table per stream: `t`, device ticks, channels.
- **Clock pairs and fits**: the thinned pairs and each fit, so an offline
  fit can replace the online one.
- **Device states and gaps**.

Every event row names its role, so "which device produced this event" is
a column, not a lookup.

## 12. Verification: loopbacks per device

A device's tier comes from a loopback, never from its manual (rig_spec
8). The rig's reference instrument is the one with the best-checked
clock. The proposal: the line-in, once `ysp/audio.h` has capture. A
two-channel capture on the ysp_rt clock (tier 2 at best, through the
capture fit) records physical edges at the sample (20.8 us at 48 kHz).

| What | Wiring | Measures |
|---|---|---|
| Display onset | photodiode on line-in channel 1, or on the photodiode board | flip record minus light (exists: `screen_loopback.c`) |
| Trigger output | TTL output into line-in channel 2 (through a divider) | `ydev_out` time minus edge: the output role's latency |
| Input device with a clock (XID, line protocol) | the output TTL also wired to a box input (a box's external input, or a relay on a button) | event `t` minus the TTL edge on the capture: checks the FIT map |
| Button, keyboard by hand | a contact microphone on the button into line-in (unverified as a method; to test) | event `t` minus the sound onset: the whole chain, including the switch |
| Photodiode board's own fit | board on the display, line-in photodiode beside it | the two edge times agree within the fit's bound |

Software clock sync does not remove a device's fixed delay: LSL's
authors measured 6 to 20 ms per-device setup offsets that their clock
sync cannot see (section 2.1). So every role gets a loopback, also
when its clock sync is good.

What the user's hardware verifies (section 14 has the order):

| Hardware | Checks |
|---|---|
| Dell receiver, built-in keyboard | match keys survive a replug; binding by activity; two keyboards as two roles |
| TrackPoint, touchpad, Dell mouse | raw mice told apart (not yet verified in docs/screen.md); a second mouse as a response role |
| Photodiode (coming) | the first device on `ysp/device.h` if on a board; the FIT estimator on real USB delivery jitter; the display onset offset for the rig profile |
| Line-in (coming) | the reference instrument; output latency of any TTL the user can produce (a USB-serial adapter's DTR, as `audio_loopback --line --ttl-dtr` already does) |
| Nothing (software) | decoders on captured bytes; the fit on synthetic pairs; LSL with a local outlet; a simulated box on a virtual port pair (com0com on Windows) |

## 13. Non-goals

| Not done | Why |
|---|---|
| Recording EEG, MEG or physiology signals | The amplifier's recorder (BrainVision Recorder, ActiView, LabRecorder) does it and owns its format. ysp sends markers and records its own events. It reads a low-channel LSL stream only for online use (a threshold, a feedback signal). |
| One process per device (ioHub's model) | In one process, every thread reads the same clock with no conversion and no IPC delay, and events need no serialization. The cost: a crash in a vendor library ends the session. Section 16 asks whether that cost matters for EyeLink and Tobii. |
| A device description language (new protocols from a config file, as ioHub's serial parsers: fixed length, prefix, delimiter) | A protocol is code with a test on captured bytes. A config file that defines framing would be an untested interpreter. A new family is a decoder in `ysp/box.h`; DIY boards use the ysp line protocol. |
| Reimplementing closed vendor protocols (the EyeLink link, Tobii's) | The vendor's library is the supported path and changes with firmware. |
| Redistributing vendor libraries | Their licenses differ and change. The glue loads what the lab installed and names the version it needs. |
| Raw USB through libusb or WinUSB | It needs a driver replacement on Windows (Zadig), and no device in section 10 needs it. |
| Probing unknown devices to guess their family | Bytes sent to an unknown port can reset an Arduino or put a code on an EEG trigger line. A device is identified only after the rig profile says what it should be. |
| Bluetooth and BLE as timed transports | Their delivery is not bounded. A Bluetooth keyboard or mouse still works through the OS as a HOST device, and its tier comes from a loopback like any other. |
| Eye-tracker calibration screens inside the device layer | The player draws them with `ysp/gfx.h`; the glue only passes targets and results through the vendor library. |
| Online fixation and saccade detection by ysp | The tracker's own events, or the analysis. A gaze-contingent display reads the newest sample. |
| Force feedback, rumble, haptics | No paradigm in rig_spec section 14 needs them. |
| WebHID and Web Serial in the web player | The web player's timing is measured and excluded (rig_spec 6). Later, at tier 3, if a deployment asks. |
| A shared reader thread for all devices | One thread per instance until a measurement shows a cost (section 4.8). |

## 14. What to build first

### 14.1 Ranked by labs served

The ranking is a judgment from the survey (section 2), not from usage
data; no usage counts were found.

| Rank | Capability | Labs that need it | Built today | Verifiable on the user's hardware |
|---|---|---|---|---|
| 1 | Trigger outputs at the flip, logged, with measured latency | every EEG, MEG, physiology and many fMRI labs | bytes (`ysp/parallel.h`, `ysp/serial.h`) and the flip callback (`ysp/screen.h`); no role, no log record, no latency measurement | Partly: line-in plus a USB-serial adapter's DTR |
| 2 | Keyboard-mode boxes and the scanner pulse as a key, bound to roles | most fMRI labs; labs with HID boxes | SDL raw keyboard per device (`ysp/screen.h`); no match key, no SYNC kind | Yes: the keyboards on hand |
| 3 | Serial response boxes with device clocks (Cedrus XID first) | RT labs that need below-keyboard timing | bytes only | Decoder only; no box |
| 4 | Eye trackers (EyeLink first, then Tobii) | vision, attention, reading labs | nothing | No |
| 5 | LSL inlets and outlets | EEG labs on LSL; multi-machine rigs | nothing | Yes, locally |
| 6 | Photodiode and microphone as event sources (sync, voice key) | every lab that verifies timing; speech-production labs | the screen loopback's board protocol; no audio capture | Yes, when the photodiode and line-in come |
| 7 | DAQ input and output (LabJack, NI-DAQmx, MCC) | physiology, force and motor labs | nothing | No |
| 8 | VPixx device side (DATAPixx inputs, its clock) | vision labs with VPixx displays | the display codes (`ysp/screen.h`) | No |
| 9 | Vendor-defined HID devices (Teensy raw HID, HID-mode pads) | few | nothing | With a Teensy |

### 14.2 The first three

Each is small enough to finish and check before the next starts.

1. **The clock fit and the event changes.** `ysp/rt.h` v0.6: the LATE,
   BRACKET and UNBIASED estimators, tick unwrapping, restarts, thinned
   CLOCK records. `ysp/input.h` v0.3.0: `YIN_KIND_SYNC`, `unc_us`, the
   ticks field, the eye controls. `ysp/response.h` (patch): ALL mode
   excludes SYNC. Tests on synthetic pairs: a known offset and
   rate with late-only noise (exponential, plus 16 ms quantization), a
   timer reset, 100 ppm over 10 minutes, a 32-bit wrap; the fit's error
   stated. Then `screen_loopback.c` uses the fit instead of its own,
   which puts it on real USB data at the photodiode run.
   Serves: every FIT and BRACKET device in section 10.
2. **`ysp/device.h` with serial, and `ysp/box.h` with three decoders.**
   The first real device is an MCU photodiode (Teensy 4 or RP2040; the
   Teensy's native USB has no FTDI latency timer) on the ysp line
   protocol with a FIT clock.
   The instance, the family vtable, roles and match keys, the lifecycle
   with reconnect, the reader thread, the bridge sink, the stream ring,
   the log records. Decoders: Cedrus XID (pinned to bytes from the
   vendor's documentation or an open-source implementation, and to a real
   box before release), the ysp line protocol (with its firmware
   example), and the photodiode frame. Also in this step: the match key
   in `ysp/screen.h`'s device record, so keyboard-mode boxes and scanner
   pulses bind to roles (rank 2). Tests: decoders on captured and fuzzed
   bytes; the whole path on a virtual port pair (com0com on Windows, socat
   on Linux) with a writer thread that plays a box, unplugged in the
   middle. Hardware: the MCU photodiode is the first real device; the
   keyboards check binding and replug. The `ysp/screen.h` match key
   (decision 11) can follow in a later step.
   Serves ranks 2, 3 and 6.
   Built 2026-10-08 (docs/device.md): the instance, match keys, identify,
   the lifecycle with reconnect and the gap record, the reader thread and
   manual mode, BRACKET fits with timer queries, the three decoders
   (`ysp/box.h`), the firmware (compiled for the Teensy 4.x and the Pico,
   not run) and `examples/device/photodiode_check.c`. Left for later steps: roles
   and the family vtable (a family is a `ybox_family` and code in
   `ysp/device.h` for now), stream rings, the `ysp/screen.h` match key.
3. **Outputs on the model.** `ydev_out_set`, `_pulse`, `_mark`;
   trigger roles over `ysp/parallel.h` and `ysp/serial.h` (TriggerBox,
   BioSemi, MMBT-S, XID, DTR), bound to `ysp/screen.h` trigger channels;
   the DEVICE_OUT record; the loopback procedure of section 12 for
   write-to-edge latency, stored in the rig profile. Hardware: the user's
   USB-serial adapter (DTR) and an Arduino, Teensy or RP2040 board give
   the first output latencies on this laptop. USB trigger boxes rank
   before the parallel port for new rigs; `ysp/parallel.h` stays.
   Serves rank 1.
   Built 2026-10-08 (docs/device.md, docs/box.md): `ydev_out_set`,
   `_pulse`, `_mark` and `_trigger`, the `YDEV_REC_OUT` record with the
   times before and after each write; pulses timed by the device (XID
   `mp`/`mh`, the line protocol's new `p`, the fixed 8 ms of BioSemi and
   the MMBT-S) or by the instance's own ysp/rt.h worker (TriggerBox,
   MMBT-S at switch S, DTR and RTS, the parallel port), each trailing edge
   with its own record; encoders in `ysp/box.h` pinned to Cedrus's page
   and pyxid2, Brain Products' programming examples, BioSemi's page, and
   two open-source MMBT-S drivers (not Neurospec's manual); the ysp/screen.h
   trigger channel (`ydev_trigger_fn`, `ydev_trigger_channel()`) with
   `examples/device/trigger_flip.c`; roles by index (`ydev_roles`); the
   loopback tool `examples/device/out_latency.c` (a distribution and a
   file named by its SHA-256), checked against a simulated adapter and
   board; `firmware/ysp_line/` with outputs (compiled, not run). Not in
   it: LSL markers (step 4), the rig profile file (built afterwards,
   2026-10-09: `ysp/rigfile.h`, 11.2), any vendor device or board on the
   wire.

After these, in order:
4. LSL in `ysp/net.h` (rank 5; testable locally). With it, the user's
   Neurobehavioral Systems LabStreamer (own clock, 10 kHz 4-channel
   input with photodiode, audio, touch and analog inputs, 8 digital
   outputs, LSL streams, 0.1 ms claimed accuracy;
   [manual](https://www.neurobs.com/manager/content/docs/labstreamer/index.html),
   not read here) becomes the rig's independent reference instrument. It
   cross-checks the MCU photodiode on the same flips, and it can replace
   the line-in as the reference of section 12.
   Built 2026-10-09 (docs/net.md): `ysp/net.h` v0.1.0, two instance types
   (`ynet_outlet`, `ynet_inlet`) beside `ysp/device.h`'s, with its
   lifecycle numbers, roles, record layouts and sink; the sender's stamps
   mapped through a fit of `lsl_local_clock()` and a least-squares fit of
   liblsl's time corrections; marker events, threshold crossings and a
   stream ring; `ysp/rigfile.h` v0.2.0 binds a role to a stream (family
   `lsl`). The LabStreamer's manual was read: it is a networked box with its
   own clock that listens to one trigger stream and sends Data (10 kHz, 6
   float32 channels), Latencies (JSON) and Messages; it binds as three
   roles (docs/net.md, "The LabStreamer"). Measured against the real
   liblsl on Windows (1.17.7, fetched by `tools/vendor_lsl.py`) and WSL2.
   Not run: a LabStreamer.
5. The two other response devices the user has (to be named), after the
   Cedrus XID decoder of step 2.
6. EyeLink glue (rank 4), audio capture and the edge detector (rank 6),
   Tobii, `ysp/hid.h`, DAQ glue, VPixx.

## 15. The `ysp/box.h` proposal of 2026-10-08: what stays, what changes

| Item | Proposal | Here |
|---|---|---|
| Pure decoders for XID, bit-state, byte-per-press, line protocol | `ysp/box.h` | Kept, in `ysp/box.h`, which is now pure only; the photodiode frame added |
| Two stamps (host read, device time through a fit) | in `ysp/box.h` | Kept, as the HOST and FIT stamp sources; BRACKET and SDK added |
| The fit | sliding-window minimum for the offset, slope from two half-window minima, in `ysp/box.h` | Moved to `ysp/rt.h`; the estimator is the lower envelope `ysp/audio.h` already measured |
| Reader thread with `yser_interrupt()` and a sink callback | `ysp/box.h`, serial only | Kept, in `ysp/device.h`, for every transport and many instances |
| `device` = the caller's box index | caller's choice | The role's index, assigned by the device layer, the same in the data file |
| Device identity | one header line from `yser_find_ports()` | Match keys per transport, identify step, rig profile, activity binding |
| Outputs, streams, reconnect, many devices | absent | Added |
| Tests | synthetic pairs, captured bytes, virtual port pair | Kept, plus an unplug in the middle |

## 16. Decisions (2026-10-08)

The user answered questions 4, 5 and 9. The coordinator's recommendations
for the others are defaults: adopt unless the user changes them.

| # | Question | Decision | Status |
|---|---|---|---|
| 1 | The fit's home | `ysp/rt.h` v0.6. `ysp/audio.h` moves onto it only after a measurement shows equal results | Default |
| 2 | The event's spare bytes | Yes: `unc_us` at offset 26, `ticks` at offset 44; every existing offset unchanged; `ysp/input.h` v0.3.0; the layout test updated | Default |
| 3 | `YIN_KIND_SYNC` | Yes, kind 7. `ysp/response.h`'s ALL mode excludes it | Default |
| 4 | The photodiode | The user has several: MCU-based (RP2040, Teensy 4) and a Neurobehavioral Systems LabStreamer. The MCU photodiode over USB serial, on the ysp line protocol with a FIT clock, is the first real device (step 2). The LabStreamer is the independent reference once LSL exists (step 4) | User |
| 5 | Parts on hand | A USB-serial adapter and Arduino, Teensy and RP2040 boards | User |
| 6 | Threads or processes for vendor libraries | Threads first. A helper process only if a measurement shows interference | Default |
| 7 | Vendor glue | Headers that load the vendor library at run time (the ANGLE pattern); player modules later | Default |
| 8 | The rig profile | A file on the rig, outside the pack, in `yrt_user_dir(YRT_DIR_CONFIG, "rig")` (`ysp/rt.h` v0.7.0, added 2026-10-08). Its format (11.2): accepted by the user on 2026-10-09, built as `ysp/rigfile.h` v0.1.0 | Default; format: user |
| 9 | Devices first in hardware | About three response devices, Cedrus among them: the XID decoder first; the user names the other two | User |
| 10 | Names | `ysp/device.h` (`ydev_`), `ysp/box.h` (`ybox_`) | Default |
| 11 | `ysp/screen.h` device record | Add the Raw Input path and the match key, in a later step | Default |
| 12 | `ysp/parallel.h` on Windows 11 | Check whether Windows 11 blocks the InpOut driver on this laptop; report only, install nothing that weakens driver protection. USB trigger boxes rank first for new rigs; the parallel port stays | Default |

Checked for decision 12 on 2026-10-08, read only (nothing installed): the
vulnerable-driver blocklist is on (`VulnerableDriverBlocklistEnable` = 1),
virtualization-based security runs, memory integrity (HVCI) does not, and
no InpOut driver is installed. The blocklist policy on this laptop
(`driversipolicy.p7b`, 2026-09-07) names no InpOut file; it also blocks
by hash and signer, which this check did not read. So whether Windows 11
loads `inpoutx64.sys` here stays unverified until a parallel-port rig
installs it.

Still open: the names of the other two response devices (9).
