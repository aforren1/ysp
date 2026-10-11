# How-to: choose a trigger path

This guide helps you select how a ysp program sends event codes (triggers)
to an EEG, MEG, eye tracker or other recorder. It compares four paths and
gives the data that the repository recorded for each. It does not replace
the manuals: the header's top comment is the reference, and
docs/device.md and docs/net.md have the step-by-step guides.

**Important:** none of these paths has sent a code to real hardware yet.
No parallel port, serial port, trigger box or LabStreamer was attached to
the development machine (device.h STATUS, docs/device.md "What needs the
user's hardware", docs/net.md "Not done"). Measure your path on your rig
before you use its numbers: `device_out_latency` (docs/device.md, "How-to:
measure an output's write-to-edge latency") and a photodiode do this.

## Decide

1. Find out what your recorder accepts: a TTL parallel input (most EEG
   amplifiers), a USB trigger interface, or a Lab Streaming Layer (LSL)
   stream.
2. Use the first row in this table that your hardware and needs match.

| Use this path | When | Gives you |
|---|---|---|
| `ysp/device.h` with a trigger box family (`YBOX_TRIGGERBOX`, `YBOX_BIOSEMI`, `YBOX_MMBTS`, `YBOX_XID`, `YBOX_LINE`, `YBOX_LINES`, `YBOX_PARALLEL`) | A new rig; you want a record of each code, codes at the flip, and reconnect after a replug | A record of each set, pulse and mark, with the time before and after the write; LOST, reconnect by match key and a GAP record; pulses that the device or a host worker ends |
| `ysp/net.h` (LSL markers) | The recorder reads LSL, or the lab already uses LabRecorder | Markers stamped on the ysp_rt clock, at the flip if you want; records with both clocks; its own LOST and re-resolve |
| `ysp/parallel.h` (`ypar_`) alone | An LPT port on a PCI or PCIe card or the motherboard, and you need the fewest parts | Writes and async pulses; no records, no reconnect |
| `ysp/serial.h` (`yser_`) alone | A device that takes bytes on a COM or USB-serial port and needs nothing that device.h adds | Bytes and async pulses; no records, no reconnect |

The devices plan (docs/devices_spec.md, line 791) ranks USB trigger boxes
before the parallel port for new rigs.

## Send the code at the flip

All four paths can send at the flip. ysp/screen.h runs a trigger channel
on a deadline worker at the planned vblank (screen.h, TRIGGERS). Give it
one of these:

- `ydev_trigger_channel()` (docs/device.md, "How-to: send the code at the
  flip"). The OUT record carries the FLIP flag and the deadline.
- `ynet_trigger_channel()` (net.h). The marker gets the deadline as its
  time, flag `YNET_OUT_FLIP`.
- Your own callback that calls `ypar_pulse_async()` or `yser_pulse()`
  (the example in screen.h's TRIGGERS section).

A trigger moves when the flip will be late (`YSCR_TRIG_MOVED`). Set
`desc.trigger_fence` to also move it when the GPU is late
(`YSCR_TRIG_GPU_MOVED`). The fence is off by default.

## What each path needs

| Path | Hardware | Software | Platforms |
|---|---|---|---|
| device.h | The box or board, on a serial or USB-serial port (or an LPT port for `YBOX_PARALLEL`) | ysp/serial.h, ysp/box.h, ysp/input.h, ysp/rt.h (ysp/parallel.h for `YBOX_PARALLEL`) | Windows, Linux, macOS |
| net.h | None for the marker; your recorder reads LSL | liblsl, loaded at run time (`tools/vendor_lsl.py`, docs/net.md "How-to: get liblsl") | Windows, Linux; macOS not run |
| parallel.h | A real LPT port; a USB printer adapter does not work | Linux: ppdev (`modprobe ppdev`) or direct port I/O. Windows: inpoutx64.dll and its kernel driver, installed as administrator; the driver can be blocked | Windows, Linux |
| serial.h | A COM or USB-serial port | nothing more | Windows, Linux, macOS |

For an FTDI USB-serial adapter, set `desc.low_latency`: the default
receive latency timer is 16 ms (serial.h, TIMING).

## What was measured

No number here is an electrical edge. Each is a software time on one
Windows 11 laptop (i7-1360P), from the doc named.

| Path | What | Conditions | Result | Source |
|---|---|---|---|---|
| screen.h trigger channel | Callback fire time minus its deadline, p99 | AC, worker on E-cores, fullscreen and window, 1 minute each | 1.0 to 3.2 us; 1 to 15 triggers a minute over 200 us, the largest 0.35 to 43 ms | screen.h, STATUS |
| screen.h trigger channel | The same, p99 | battery, worker on P-cores | 59 to 300 us | screen.h, STATUS |
| device.h | Host-timed 2 ms pulse width minus 2 ms | fake transport, MSVC, timing guard, AC | p50 +0.7 to +1.3 us, p99 +3.3 to +13.4 us, max +15.6 to +454 us | docs/device.md, Results |
| device.h | Cost of `ydev_out_pulse()` to the caller | the same | p50 5.3 to 8.7 us, p99 28 to 60 us | docs/device.md, Results |
| net.h | The sink's receive minus the push, one process | real liblsl 1.17.7, Windows, local loopback, measurement lock | p50 89 to 121 us, p99 269 to 299 us | docs/net.md, "Real liblsl: Windows and WSL2" |
| parallel.h | anything | | not measured: no LPT port | parallel.h, STATUS |
| serial.h | anything | | not measured: no serial port | serial.h, STATUS; docs/serial.md |

Under other load, LSL markers once reached a p50 of 42 and 685 ms (not
diagnosed; docs/net.md). A path through USB adds the USB frame: 1 ms at
full speed, 125 us at high speed (serial.h, TIMING).
