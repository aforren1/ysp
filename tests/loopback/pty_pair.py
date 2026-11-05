"""A virtual serial port pair for tests/loopback/serial_loopback.c, without socat.

Opens two pseudo-terminals, prints their two device names on one line, and
copies bytes between the two masters until it gets SIGINT or SIGTERM. It does
what `socat -d -d pty,raw,echo=0 pty,raw,echo=0` does, for machines that do not
have socat (a stock macOS). Standard library only; POSIX only.

    uv run --no-project python tests/loopback/pty_pair.py
    # prints: /dev/ttys003 /dev/ttys004
    build/macos-loopback/serial_loopback /dev/ttys003 /dev/ttys004

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
    # Keep both slaves open: on macOS a master whose last slave descriptor
    # closes reads EOF until the next slave open, and the copy loop would spin.
    masters = [master for master, _ in pairs]
    for m in masters:
        os.set_blocking(m, False)

    stop = False

    def on_signal(_sig, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    print(names[0], names[1], flush=True)

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
