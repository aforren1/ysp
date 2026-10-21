#!/usr/bin/env python3
"""Minimal ysp.serial Python demo.

Build/install first (from this directory):
    pip install .
or build in place:
    python setup.py build_ext --inplace

Run (the device is the only required argument; see DEVICE NAMES in
ysp/serial.h):
    python example.py /dev/ttyUSB0 0x2A
"""
import sys
import threading
import ysp.serial as yser


def listener(port, stop):
    """Reader thread: one reader and one writer may share a Port."""
    while not stop.is_set():
        try:
            data = port.read(64, yser.TIMEOUT_INFINITE)
        except yser.Interrupted:
            return                      # shutdown, not an error
        except yser.Disconnected:
            print("device disconnected", file=sys.stderr)
            return
        if data:
            print(f"response: {data.hex(' ')}")


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    device = sys.argv[1]
    code = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x01

    for info in yser.list_ports():
        print(f"  {info['name']}  {info['description']}  "
              f"{info['vid']:04X}:{info['pid']:04X}")

    with yser.Port(device, baud=115200, low_latency=True) as port:
        print(f"open: {port.device} at {port.baud} baud "
              f"(low latency {port.low_latency}, async policy {port.async_policy})")

        # The box may have sent a boot banner before we opened; drop it.
        port.purge(rx=True)

        stop = threading.Event()
        reader = threading.Thread(target=listener, args=(port, stop))
        reader.start()

        port.pulse(code, 0x00, 0.002)           # blocking 2 ms trigger (seconds)
        print(f"sent trigger 0x{code:02X} (2 ms blocking pulse)")

        port.pulse_async(code, 0x00, 0.002)     # non-blocking; worker writes 0x00
        print(f"queued async trigger 0x{code:02X}")
        port.drain()

        # Shutdown: interrupt, join, then close. Closing under a live reader
        # is undefined on POSIX.
        stop.set()
        port.interrupt()
        reader.join()

        # The async reader: a C thread reads and stamps each line when it
        # arrives, without the GIL. Writes stay allowed while it runs.
        port.async_start(terminator=b"\n", capacity=4096)
        t0 = yser.now()
        port.write_byte(code)
        if port.async_wait(0.5):
            for line, t in port.async_read():
                print(f"line {line!r} {1e3 * (t - t0):.3f} ms after the write")
        print(f"async reader: {port.async_stop()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except yser.Error as e:
        print(f"ysp.serial error: {e}", file=sys.stderr)
        sys.exit(1)
