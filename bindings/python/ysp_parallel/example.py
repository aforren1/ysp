#!/usr/bin/env python3
"""Minimal ysp_parallel Python demo.

Build/install first (from this directory):
    pip install .
or build in place:
    python setup.py build_ext --inplace

Run (Linux ppdev needs device access; see the BACKENDS section of ysp/parallel.h):
    python example.py 0x2A
"""
import sys
import ysp.parallel as pp


def main():
    code = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x01

    # Defaults: ppdev /dev/parport0 on Linux, inpout @ LPT1 on Windows.
    # For raw x86 port I/O instead (root):
    #     pp.Port(backend=pp.BACKEND_DIRECT, base_addr=pp.LPT1)
    with pp.Port() as port:
        print(f"port open (backend {port.backend}, base 0x{port.base_addr:X})")

        port.pulse(code, width=0.002)          # blocking 2 ms trigger (seconds)
        print(f"sent trigger 0x{code:02X} (2 ms blocking pulse)")

        port.pulse_async(code, width=0.002)    # non-blocking; worker drops it
        print(f"queued async trigger 0x{code:02X}")

        status = port.read_status()
        print(
            f"status = 0x{status:02X}  "
            f"busy={bool(status & pp.STATUS_BUSY)} "
            f"ack={bool(status & pp.STATUS_ACK)} "
            f"paper={bool(status & pp.STATUS_PAPER)} "
            f"select={bool(status & pp.STATUS_SELECT)} "
            f"error={bool(status & pp.STATUS_ERROR)}"
        )


if __name__ == "__main__":
    try:
        main()
    except pp.Error as e:
        print(f"ysp_parallel error: {e}", file=sys.stderr)
        sys.exit(1)
