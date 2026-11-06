"""A virtual serial port pair for the loopback tests, without socat.

Opens two pseudo-terminals, prints its pid and their two device names on one
line, and copies bytes between the two masters until it gets SIGINT or
SIGTERM. Then it exits and closes both masters, so each port hangs up, as
when socat is killed. It does what `socat -d -d pty,raw,echo=0 pty,raw,echo=0`
does for serial_loopback.c and device_loopback.c, for machines that do not
have socat (a stock macOS, the Linux desktop). Standard library only; POSIX
only.

    uv run --no-project python tests/loopback/pty_pair.py &
    # prints: 12345 /dev/ttys003 /dev/ttys004   (Linux: /dev/pts/N names)
    build/macos-loopback/serial_loopback /dev/ttys003 /dev/ttys004
    build/macos-loopback/device_loopback /dev/ttys003 /dev/ttys004 12345

The pid is the python process's, which device_loopback kills for its hangup
check; under `uv run`, the shell's `$!` is uv's pid, not this one.

Like socat, this process holds both masters, so closing one slave does not
hang up the other: serial_loopback's disconnect check is informational here.
"""

import os
import select
import signal
import sys
import tty


def main() -> int:
    pairs = [os.openpty() for _ in range(2)]
    names = [os.ttyname(slave) for _, slave in pairs]
    for _, slave in pairs:
        tty.setraw(slave)
    # Keep both slaves open. A master whose slave has no opener reads EOF on
    # macOS and EIO on Linux until the next open. On macOS the copy loop would
    # spin; on Linux it stalled a first version that closed them (measured: a
    # 64-byte write reached the peer only with the next 4 KiB).
    masters = [master for master, _ in pairs]
    for m in masters:
        os.set_blocking(m, False)

    stop = False

    def on_signal(_sig, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    print(os.getpid(), names[0], names[1], flush=True)

    # Bytes read from one master and not yet accepted by the other. A peer
    # that stops reading fills its slave's input queue; the bytes wait here, so
    # the writer's side fills up too and its write timeout can fire.
    pending = [b"", b""]
    while not stop:
        rd = [masters[i] for i in range(2) if len(pending[i]) < 65536]
        wr = [masters[1 - i] for i in range(2) if pending[i]]
        try:
            r, w, _ = select.select(rd, wr, [], 0.5)
        except InterruptedError:
            continue
        for i in range(2):
            if masters[i] in r:
                try:
                    pending[i] += os.read(masters[i], 65536)
                except (BlockingIOError, InterruptedError):
                    pass
                except OSError:
                    return 1
            if pending[i] and masters[1 - i] in w:
                try:
                    n = os.write(masters[1 - i], pending[i])
                    pending[i] = pending[i][n:]
                except (BlockingIOError, InterruptedError):
                    pass
                except OSError:
                    return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
