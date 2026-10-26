# ysp.box Python binding

A CPython extension for the [ysp/box.h](../../../include/ysp/box.h)
single-header library: the protocols of response boxes, trigger boxes and
ysp line boards. Use it in an analysis to decode a raw capture of a
session's bytes into input events and clock pairs, with no device layer,
port or thread. It also gives the bytes a host writes: timer and identity
queries, and output codes for trigger lines.

The extension uses the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x03080000`), so one `ysp/box.abi3.so` works on
CPython 3.8 and later. It has no run-time dependencies.

The distribution is `ysp-box` and the module is `ysp.box`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-box` installs alone or next to the other `ysp-*`
distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
```

In a development tree, setup.py finds `ysp/box.h` in the repository's
`include/` directory. An isolated build (an sdist or a cibuildwheel run)
sees only this directory. Copy the header and the `ysp/input.h` it
includes into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/box.h ../../../include/ysp/input.h include/ysp/
uv build --sdist
```

## How to decode a capture

Give each read's bytes and its read time in seconds. A frame or a line
split across two reads is kept until its rest comes.

```python
import ysp.box as box

d = box.Decoder("xid", kind=box.KIND_BOX, device=1)
for t, chunk in capture:            # read time (s), the bytes of that read
    r = d.decode(chunk, t)
    for e in r.events:              # Event(t, t_ns, ..., ticks)
        ...                         # e.ticks: the box's own clock, low 32 bits
    pairs += r.pairs                # Pair(ticks, probe): for a clock fit
```

A capture with no read times decodes in one call:
`box.decode("line", open("board.bin", "rb").read())`. Every event then has
`t` 0, and `ticks` and the pairs carry the device's times.

`decode()` returns `Decoded(events, pairs, outputs, text, garbage)`:

- `events`: a list of `Event`.
- `pairs`: a list of `Pair(ticks, probe)`, one for each frame that carries
  the device's time. `ticks` is the whole device time, `probe` is 0 for a
  frame the device sent on its own and the query's number for an answer
  to a timer query.
- `outputs`: a list of `Output(ticks, code)`, a line board's `O` reports.
- `text`: the identity lines (`I ...`) of a line board.
- `garbage`: the bytes in this call that fit no frame.

`Decoder.frames`, `Decoder.garbage` and `Decoder.pending` are the totals
since init or `reset()` and the bytes held for the next call.

## How to write trigger codes

`encode(family, op, code, width)` gives the bytes. Write them with any
serial library:

```python
port.write(box.encode("triggerbox", "set", 12))
port.write(box.encode("triggerbox", "set", 0))
port.write(box.encode("xid", "pulse", 3, 0.002))    # the box ends the pulse
port.write(box.encode("triggerbox", "close"))       # before closing: 0xFF
```

For an XID box, write each byte 1 ms apart (`slow_write("xid")`,
`SLOW_WRITE`) for queries, as pyxid2 does.

## The event

`Event` is a structseq with these fields in this order. `ysp.device` and
`ysp.response` use the same representation.

| Field | What it is |
|---|---|
| `t` | seconds on the ysp_rt clock; here the read time given to `decode()` |
| `t_ns` | `t` as an int of ns, exact |
| `device`, `control`, `code`, `mods` | the instance, the button or channel, the XID port, keyboard modifiers |
| `kind`, `type`, `flags`, `stamp` | `KIND_*`; `PRESS`, `RELEASE`, `SAMPLE`, `PROXIMITY`; `DEVTICKS` and the other flag bits; `TIER_*` |
| `unc` | the event's own uncertainty in seconds; 0 here |
| `x`, `y`, `value`, `aux` | floats; `value` is 1 for a press, 0 for a release, the value of a sample |
| `ticks` | with `DEVTICKS`: the low 32 bits of the device's clock |

## Times

Every time is seconds, as a `float`: the read time `t`, `Event.t`,
`Event.unc`, the `width` of `encode()`, `fixed_pulse()`, `tick()`,
`SLOW_WRITE` and `UNC_MAX`. A read time converts to the nearest ns of the
exact product `t x 1e9`, so `t = ns / 1e9` gives back `Event.t_ns == ns`
(checked for 3,000 values below 2^23 s). A negative time, NaN and infinity
raise `ArgumentError`. Device times (`ticks`, `Pair.ticks`,
`Output.ticks`) stay counts of the device's clock; `tick(family)` is the
nominal unit (0.001 s for XID, 1e-6 s for the line protocol and the
photodiode frame).

## Reference

| Name | What it is |
|---|---|
| `Decoder(family, kind=KIND_BOX, device=0)` | `ybox_init()`. `family` is a name (`"xid"`, `"line"`, `"photo"`, `"triggerbox"`, `"biosemi"`, `"mmbts"`, `"lines"`, `"parallel"`, any case) or a family constant. `kind` and `device` go into every event. |
| `Decoder.decode(data, t=0.0)` | `ybox_decode()` on every byte of `data` (bytes, bytearray, memoryview or a sequence of ints 0 to 255) read at time `t`. |
| `Decoder.reset()` | `ybox_init()` again. |
| `decode(family, data, t=0.0, kind=, device=)` | a new `Decoder` and one `decode()`. |
| `encode(family, op, code=0, width=0.0)` | `ybox_encode()`. `op` is `"set"`, `"pulse"`, `"width"`, `"close"` or `OP_*`. `b""` when the family has no bytes for the op. |
| `probe(family, seq=1)`, `identify(family)` | the bytes of a timer query and of an identity query; `b""` when the family has none (and for a line probe with `seq` 0). |
| `caps`, `code_max`, `fixed_pulse`, `default_baud`, `tick`, `slow_write`, `family_name` | the header's facts about a family. |
| constants | the families (`XID` ... `PARALLEL`), `CAP_*`, `OP_*`, and from ysp/input.h `KIND_*`, `PRESS`, `RELEASE`, `SAMPLE`, `PROXIMITY`, `REPEAT`, `SYNTHETIC`, `ERASER`, `UNLISTED`, `DEVTICKS`, `TIER_*`, `UNC_MAX` |
| `Error`, `ArgumentError` | `ArgumentError` is also a `ValueError`. A wrong type raises `TypeError`. |
