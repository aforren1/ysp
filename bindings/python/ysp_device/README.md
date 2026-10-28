# ysp.device Python binding

A CPython extension that wraps [ysp/device.h](../../../include/ysp/device.h):
response boxes, trigger boxes and microcontroller boards as events on the
ysp_rt clock, and as outputs for trigger codes. It uses the Limited API
(one abi3 wheel per platform for CPython 3.10+) and has no run-time
dependencies. The distribution is `ysp-device`, the module `ysp.device`
(PEP 420 namespace `ysp`, no `__init__.py`). Its version is the header's.

The rig profile, [ysp/rigfile.h](../../../include/ysp/rigfile.h), is the
separate distribution `ysp-rigfile` ([its README](../ysp_rigfile/README.md)).
`Profile.desc(role)` gives the keyword arguments of `Device`, so a role
opens with `Device(**profile.desc(role))` and neither extension links the
other.

## Build and install

```sh
pip install .                         # from this directory
```

`setup.py` finds the headers in the repository's `include/`. A build
outside the tree (an sdist, cibuildwheel) needs copies in `include/ysp/`
next to `setup.py`: `device.h`, `rt.h`, `input.h`, `box.h`, `serial.h` and
`parallel.h` (see `MANIFEST.in`). The extension also compiles
`ysp_device_bind.h`, the event queue and the simulated board that the MEX
binding shares.

## How-to: read a response box and send triggers

```python
import ysp.device as yd
import ysp.rigfile as yrig                # pip install ysp-rigfile

prof = yrig.load()                       # the rig profile
for note in prof.notes:                  # a loopback file that failed its check
    print("rig profile:", note)
box = yd.Device(**prof.desc("resp"))     # or yd.Device("resp", "xid", "serial:0403:6001:FT4ABC12:")
trig = yd.Device(**prof.desc("trig"))
for d in (box, trig):
    if not d.wait_running(3.0):          # before the first output
        raise SystemExit("%s: %s" % (d.role, d.error))
print("resp tier:", prof.source(box)["tier"])   # TIER_1 only with checked bounds

trig.pulse(12, 0.002)                    # 12 for 2 ms, then 0
onset = yd.now()
for e in box.wait(2.0):                  # events, oldest first
    if e.type == yd.PRESS:
        print("button", e.control, "rt", e.t - onset, "+-", e.unc)
box.close()
trig.close()
```

Find a device's match key with `yd.list_ports()` (the `key` of each port).

## How-to: send the button's code at the press

A code sent from the frame loop goes out up to a frame after the press. A
`Rule` sends it from the box's reader thread
([docs/device.md](../../../docs/device.md#how-to-send-a-code-at-the-response)):

```python
rule = yd.Rule(trig, [{"kind": yd.KIND_BOX, "control": 1, "code": 101},
                      {"kind": yd.KIND_BOX, "control": 2, "code": 102}])
box = yd.Device(**prof.desc("resp"), rule=rule)
rule.arm(onset + 0.1, onset + 1.5)      # each trial: [onset + min rt, onset + deadline)
...
info = rule.info()                       # code, t_event (the press), t_before (the write)
box.close(); rule.close(); trig.close()  # this order: the input, the rule, the output
```

## Reference

| Name | What it is |
|---|---|
| `Device(role, family, key, *, device=0, kind=0, baud=0, pulse=None, latched=False, min_hold=None, probe=None, retry=None, silent=None, max_width=None, no_identify=False, no_elevate=False, queue=1024, rule=None, sim=None)` | One instance with its reader thread. `family`: a number (`XID`, `LINE`, `PHOTO`, `TRIGGERBOX`, `BIOSEMI`, `MMBTS`, `LINES`, `PARALLEL`) or its name. `device=0` takes the role's index from the process's role table. |
| `Device.wait_running(timeout)` | `True` when RUNNING, `False` on timeout; raises `Error` when the device ends FAILED (a wrong device at the key) |
| `Device.poll(max=0)`, `Device.wait(timeout, max=0)` | The queued events as a list of `Event`; `wait` blocks until one is there, the timeout, or `close()` |
| `Device.clear()`, `Device.pending` | Drop the queued events; the count queued |
| `Device.set(code)`, `.pulse(code, width)`, `.mark(code, text=None)` | Outputs; raise `Error` with `args == (message, ERR_*)` when nothing was written |
| `Device.last_out()` | The last output call of any thread: `t_before`, `t_after`, `width`, `code`, `seq`, `flags` (`OUT_*`), `result` |
| `Device.stats()` | The reader's counters, the clock fit (`fit`), `found`, `ident`, and the queue's counters (`queue_dropped` and others) |
| `Device.source()` | The events' source entry: `tier` is `TIER_UNKNOWN` (no loopback checked this layer); `ysp.rigfile.Profile.source(device)` gives the rig's tier |
| `Device.state`, `.state_name`, `.error`, `.role`, `.key`, `.family`, `.device`, `.kind`, `.pulse` | Status and the opened desc |
| `Rule(out, choices, width=None, max_fires=1)` | A response rule; `arm(start, until=math.inf)`, `window()`, `disarm()`, `info()`, `fires()`, `close()` |
| `Sim(family, ppm=20.0, frame=125e-6, tail=50e-6, key=None, identify=True)` | A simulated board for `Device(sim=...)`: `edge(t, level, channel=1)`, `press(t, duration, channel=1)`, `unplug(start, stop)`, `writes()`, `lines()`, `ticks(t)`, `key` |
| `Event` | A structseq: `t`, `t_ns`, `device`, `control`, `code`, `mods`, `kind`, `type`, `flags`, `stamp`, `unc`, `x`, `y`, `value`, `aux`, `ticks` |
| `now()`, `list_ports()`, `key_match()`, `family()`, `family_name()`, `state_name()`, `strerror()`, `roles()` | Module functions |
| `KIND_*`, `PRESS`, `RELEASE`, `SAMPLE`, `PROXIMITY`, `REPEAT`, `SYNTHETIC`, `ERASER`, `UNLISTED`, `DEVTICKS`, `TIER_*` | ysp/input.h constants (the C names without `YIN_`) |
| `Error`, `Closed` | Exceptions; `Closed` is a call on a closed device or rule |

## Units

Every time is seconds as a `float`: arguments, results and `now()`. The
binding rounds to the nearest nanosecond for the C call and refuses a
negative or NaN time before it. `math.inf` is "forever" for `wait()` and
`wait_running()`, "never" for `probe` (no timer queries), `silent` (never
LOST) and a rule window's end. An event's `t_ns` is the exact C value
beside `t`; `unc` is the C `unc_us` in seconds.

## Threads

The reader thread and the output worker are the header's. They never take
the GIL: the sink copies each event into a C ring that `poll()` and
`wait()` empty, and a full ring drops the new event and counts it
(`stats()["queue_dropped"]`). `wait()`, `wait_running()`, `close()` and
the outputs release the GIL; Ctrl+C ends a `wait()` within 50 ms. Any
thread may call the outputs; `close()` waits for the calls in progress.
One thread at a time takes the events: a second `poll()` during the first
raises `Error`.

The event time is on the ysp_rt clock (`ysp.device.now()`:
QueryPerformanceCounter on Windows, CLOCK_MONOTONIC on Linux). PsychoPy's
and Psychtoolbox's clocks may read the same counter with another origin;
that was not measured here, so pair a reading of each clock before you mix
them.

## Tests

```sh
pytest tests
```

The tests run on simulated boards (`Sim`): no hardware.
