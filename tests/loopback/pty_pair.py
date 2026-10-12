"""A virtual serial port pair, for machines without socat.

    uv run --no-project python tests/loopback/pty_pair.py

Prints its pid and two pty names on one line ("<pid> /dev/pts/A /dev/pts/B"),
then copies bytes between them until SIGTERM, when it closes both masters:
each port then hangs up, as when socat is killed. It does what
`socat -d -d pty,raw,echo=0 pty,raw,echo=0` does for serial_loopback.c and
device_loopback.c. Standard library only; POSIX.
"""
import os
import select
import signal
import sys
import termios
import tty

stop = False


def on_term(signum, frame):
    global stop
    stop = True


def make():
    master, slave = os.openpty()
    tty.setraw(slave)
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    # Kept open: a master whose slave has no opener reads EIO and polls
    # readable until one opens it, which stalls the copy loop (measured: a
    # 64-byte write reached the peer only with the next 4 KiB).
    return master, slave, os.ttyname(slave)


def main():
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    a, keep_a, name_a = make()
    b, keep_b, name_b = make()
    print(os.getpid(), name_a, name_b, flush=True)
    peer = {a: b, b: a}
    while not stop:
        try:
            ready, _, _ = select.select([a, b], [], [], 0.2)
        except InterruptedError:
            continue
        for fd in ready:
            try:
                data = os.read(fd, 4096)
            except OSError:
                continue
            if data:
                os.write(peer[fd], data)
    os.close(a)
    os.close(b)
    os.close(keep_a)
    os.close(keep_b)


if __name__ == "__main__":
    sys.exit(main())
