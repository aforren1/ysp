# ysp.audio Python binding

A CPython extension for the [ysp/audio.h](../../../include/ysp/audio.h)
single-header library: sound at a time on the ysp_rt clock. Use it in a
PsychoPy experiment that needs to know where each sound landed. A sound is
played at a time `t`; the header plans its first sample on the device frame
that `t` falls on, through a fit of the device clock, and its record gives
the onset (the fit's time, never an observed time), the residual, the
stream frame, and a tier.

The extension uses the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x030A0000`), so one `ysp/audio.abi3.so` works on
CPython 3.10 and later. It has no run-time dependencies. miniaudio 0.11.25
(one public-domain file) is compiled in.

The distribution is `ysp-audio` and the module is `ysp.audio`. Its version
is the header's.

## How to install

From this directory:

```sh
uv pip install .
```

setup.py finds the headers in the repository's `include/` and
`bindings/share/` directories. It takes `miniaudio.h` from
`YSP_MINIAUDIO_DIR`, or from `include/` next to `setup.py`, or else
downloads the pinned file and checks its SHA-256 value. An isolated build
(an sdist or a cibuildwheel run) sees only this directory: stage the files
first (MANIFEST.in lists them).

## How to play a sound at a time

```python
import ysp.audio as yau

a = yau.Audio()                                  # the OS default output
beep = a.tone(1000, 0.1, yau.db(-30), ramp=0.005)
t = yau.now() + a.lead + 0.05                    # seconds, ysp_rt clock
sid = a.play_at(beep, t)
...
r = a.wait(sid)                                  # or a.result(sid) each frame
print(r.onset, r.residual_ns, r.tier, r.flags & yau.LATE)
a.close()
```

`a.lead` is the shortest lead a sound handed over now can still meet. A
sound handed over later than that starts on the first frame the device has
not taken, and its record has `LATE`.

To open a device by name, give a part of a name from `yau.devices()`:
`yau.Audio(device="Realtek")`. The open refuses any format or rate
conversion: the device must run at `rate` (48000 Hz by default; on Windows
the Default Format in the Sound control panel) with `channels` channels.
For tests, `yau.Audio(backend="null")` opens miniaudio's null device (tier
`TIER_SIM`).

## How to stream a long WAV file

```python
w = a.wav("story.wav")                           # 16 or 24 bits, or float, at the device rate
w.feed()                                         # fill the ring (1 s)
sid = w.play(t)
while playing:
    if w.wants:
        w.feed()                                 # each frame, or from another thread
w.close()
```

A `Stream` (`a.stream(channels)`) is the same voice fed by `write()` from
Python data. Sample `s` of a stream plays on stream frame `origin + s`; a
sample that arrives after its frame is not played (a gap in the record).

## Reference

Every time is seconds (float). Records give the exact ns beside each time
(`onset_ns`, `target_ns`, `residual_ns`, `rendered_at_ns`). A negative or
NaN time is refused (`ArgumentError`). `math.inf` is "forever" for
`wait()`. Frame counts stay integers.

Module: `now()`, `now_ns()`, `db(x)`, `devices(backend="auto",
capture=False)`, `wav_info(path)`, the tiers (`TIER_1`, `TIER_2`, `TIER_3`,
`TIER_SIM`, `TIER_WEB`), the record flags (`PENDING`, `LATE`, `CANCELED`,
`STOPPED`, `XRUN`, `CLIPPED`, `UNCONFIRMED`, `BELOW_TIER`, `LOST`, `GAP`),
`FOREVER`, `MAX_VOICES`, `MAX_CHANNELS`, the WAV formats.

`Audio(device=None, *, backend, rate, channels, sample, exclusive, period,
voices, queue, min_tier, onset_offset, arena_bytes, device_index)`:
`close()`, a context manager, `is_open`, `error`, `lead`, `lead_ns`,
`caps()`, `describe()`, `frame_at(t)`, `time_of(frame)`, `update()`;
synthesis into the arena: `tone()`, `noise()`, `click()`, `buffer(data)`
(float32 or float64, 1-D or frames x channels), `load_wav(path)`,
`arena_reset()`; playing: `play_at(buffer, t)`, `play(sound, at, *, db,
channels, offset, frames, loops)`, `cancel(id)`, `stop(id, *, at, ramp)`,
`gain(id, db, *, at, ramp)`, `result(id)`, `wait(id, timeout)`; voices fed
from a ring: `stream()`, `wav()`; `pins` (SHARING).

`Onset` (a structseq): `id`, `target`, `target_ns`, `onset`, `onset_ns`,
`residual`, `residual_ns`, `start_frame`, `end_frame`, `device_pos`,
`rendered_at`, `rendered_at_ns`, `buffer_id`, `flags`, `tier`, `sample`,
`gap_frames`, `pending`.

`Buffer`: `frames`, `channels`, `id`, `duration`, `samples()`.
`Stream`: `write(data)`, `end()`, `reset(first)`, `info()`, `play()`,
`sample_at(t)`, `time_of(sample)`, `close()`. `Wav`: `feed()`, `seek()`,
`wants`, `file`, `info()`, `play()`, `sample_at()`, `time_of()`, `close()`.

`Capture(device=None, *, backend, rate, channels, sample, exclusive, period,
ring, offset, device_index)`: `read(max_frames)` gives a float32
memoryview of frames x channels and the capture frame of its first row;
`time_of(frame)` (an int, or a float from a detector), `frame_at(t)`,
`info()`, `describe()`, `close()`.

Exceptions: `Error`, and under it `ArgumentError` (also a `ValueError`),
`ClosedError`, `FullError`, `FormatError`, `NotFoundError`, `LostError`,
`BusyError`, `TimeoutError` (also the built-in one), `IOError` (also an
`OSError`).

## Threads

miniaudio's device thread never takes the GIL. `Audio()`, `close()`,
`wait()`, `Wav.feed()`, the closes, `Capture()` and `devices()` release
it. An `Audio` has one frame thread, as the header's handle does: while
one thread is in `wait()`, a call from another thread raises `BusyError`.
`Stream.write()` and `Wav.feed()` may come from one other thread.

## Sharing

The module's `_C_API` capsule holds the table that other extensions call
through (ysp-vis: a movie's soundtrack on this `Audio`).
docs/bindings.md, "Sharing one implementation across modules", has the
rules. While another module holds an `Audio`, `close()` raises `BusyError`.

## What is not bound

docs/audio.md, "Bindings", lists it with the reasons: the device and source
extensions (`desc.dev`, `desc.source`: callbacks on the device thread), the
event ring (`desc.ring`), duplex, the detectors and the WAV writer.
