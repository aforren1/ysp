"""pulse_bench.py - trigger pulses from Python, timed by the echo board.

The Python half of pulse_bench.c: the same protocol (firmware/ysp_echo
version 2, "b" byte stamps, "l" line reports, "e" probes), the same blocks
and the same CSV files, so pulse_stats.py reads both.

    python pulse_bench.py COM3 out_dir [n] [rev] [modes=a,b] [widths=1,2,5,10]

Modes, each cell with its own open:
    ysp_py_pulse        ysp.serial Port.pulse(0x55, 0, width)
    ysp_py_pulse_async  Port.pulse_async(0x55, 0, width)
    ysp_py_dtr          Port.set_dtr(True), time.sleep(width), set_dtr(False)
    ysp_py_rts          the same with set_rts
    pyserial_sleep      Serial.write(b'\\x55'), time.sleep(width), write(b'\\x00')
    pyserial_busy       the same with a perf_counter busy wait
    pyserial_dtr        Serial.dtr = True, time.sleep(width), dtr = False
    pyserial_rts        the same with rts
The ysp modes stamp with ysp.serial.now() (the ysp/rt.h clock); the pyserial
modes with time.perf_counter_ns(). On Windows both are
QueryPerformanceCounter. Needs pyserial and ysp-serial 0.5.0.2 or later.

Public domain / MIT-0, as the rest of ysp.
"""
import os
import random
import sys
import time

BLOCK = 20
N_PROBE = 10
ON, OFF = 0x55, 0x00

MODES = ["ysp_py_pulse", "ysp_py_pulse_async", "ysp_py_dtr", "ysp_py_rts",
         "pyserial_sleep", "pyserial_busy", "pyserial_dtr", "pyserial_rts"]


class YspPort:
    def __init__(self, dev):
        import ysp.serial as ys
        self.ys = ys
        self.p = ys.Port(device=dev)
        names = {getattr(ys, k): k[6:] for k in dir(ys) if k.startswith("ASYNC_")}
        self.policy = names.get(self.p.async_policy, str(self.p.async_policy))

    def now(self):
        return round(self.ys.now() * 1e9)

    def write(self, b):
        return self.p.write(b)

    def read(self, cap):
        return self.p.read(cap, 0.1)

    def purge(self):
        self.p.purge(rx=True)

    def line(self, rts, on):
        (self.p.set_rts if rts else self.p.set_dtr)(bool(on))

    def close(self):
        self.p.close()


class PyPort:
    def __init__(self, dev):
        import serial
        self.p = serial.Serial(dev, 115200, timeout=0.1, write_timeout=1.0)
        self.policy = "none"

    def now(self):
        return time.perf_counter_ns()

    def write(self, b):
        return self.p.write(b)

    def read(self, cap):
        return self.p.read(max(1, min(cap, self.p.in_waiting)))

    def purge(self):
        self.p.reset_input_buffer()

    def line(self, rts, on):
        if rts:
            self.p.rts = bool(on)
        else:
            self.p.dtr = bool(on)

    def close(self):
        self.p.close()


def is_line(m):
    return m.endswith("_dtr") or m.endswith("_rts")


def read_exact(p, n, timeout_ns):
    out = b""
    end = p.now() + timeout_ns
    while len(out) < n and p.now() < end:
        out += p.read(n - len(out))
    return out


def sleep_until(p, t_ns):
    """time.sleep for the bulk, a spin for the last 2 ms (between pulses only)."""
    left = (t_ns - p.now()) / 1e9
    if left > 0.002:
        time.sleep(left - 0.002)
    while p.now() < t_ns:
        pass


seq = 0


def probes(p, f, cell, mode, wus, n):
    global seq
    for _ in range(n):
        seq += 1
        c = b"e %010d\n" % seq
        sleep_until(p, p.now() + int(random.random() * 1e6))
        t0 = p.now()
        p.write(c)
        r = read_exact(p, 35, 500_000_000)
        t2 = p.now()
        if len(r) != 35 or r[:1] != b"R" or r[34:35] != b"\n" or int(r[2:12]) != seq:
            print(f"pulse_bench: probe {seq} failed ({len(r)} bytes)", file=sys.stderr)
            time.sleep(0.02)
            p.purge()
            continue
        f.write(f"{cell},{mode},{wus},{t0},{t2},{int(r[13:23])},{int(r[24:34])}\n")


def read_reports(p, f, cell, mode, wus, block, want, bytemode):
    buf = b""
    got = 0
    done = False
    quiet = p.now() + (1_500_000_000 if bytemode else 300_000_000)
    while not done and p.now() < quiet:
        r = p.read(65536)
        if r:
            buf += r
            quiet = p.now() + 300_000_000
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            k = line[:1]
            if k in (b"B", b"L") and len(line) == 16:
                t = int(line[2:12])
                v = int(line[13:16]) if k == b"B" else int(line[13:14]) + 2 * int(line[15:16])
                f.write(f"{cell},{mode},{wus},{block},{k.decode()},{t},{v}\n")
                got += 1
            elif k in (b"D", b"G") and len(line) == 23:
                f.write(f"{cell},{mode},{wus},{block},{k.decode()},{int(line[2:12])},{int(line[13:23])}\n")
                if k == b"D":
                    done = True
            else:
                print(f"pulse_bench: unexpected line in cell {cell}: {line!r}", file=sys.stderr)
        if not bytemode and got >= want:
            done = True
    if not done:
        print(f"pulse_bench: cell {cell} block {block}: {got} of {want} reports", file=sys.stderr)


def query_gap(p, f, cell, mode, wus, block):
    p.write(b"g\n")
    r = read_exact(p, 24, 500_000_000)
    if len(r) == 24 and r[:1] == b"G":
        f.write(f"{cell},{mode},{wus},{block},G,{int(r[2:12])},{int(r[13:23])}\n")
    else:
        print(f"pulse_bench: cell {cell}: no G line", file=sys.stderr)


def run_cell(dev, mode, width_ns, n, cell, run, o):
    wus = width_ns // 1000
    w = width_ns / 1e9
    t = time.perf_counter_ns()
    p = YspPort(dev) if mode.startswith("ysp") else PyPort(dev)
    open_ms = (time.perf_counter_ns() - t) / 1e6
    o["cells"].write(f"{cell},{mode},{wus},{run},{p.policy},{open_ms:.3f}\n")
    p.write(b"x\nl 0\n")
    time.sleep(0.05)
    p.purge()
    rts = mode.endswith("_rts")
    if is_line(mode):
        # RTS first: usbser.sys sends an RTS change only with the next DTR change
        p.line(1, 0)
        p.line(0, 0)
        time.sleep(0.02)
        p.write(b"l 1\n")
        r = read_exact(p, 17, 500_000_000)
        if len(r) != 17 or r[:1] != b"L" or r[13:14] != b"0" or r[15:16] != b"0":
            sys.exit(f"pulse_bench: cell {cell}: line report did not start ({r!r})")
    query_gap(p, o["board"], cell, mode, wus, -1)
    i = 0
    nblocks = (n + BLOCK - 1) // BLOCK
    for b in range(nblocks):
        bn = min(BLOCK, n - i)
        probes(p, o["probes"], cell, mode, wus, N_PROBE)
        if not is_line(mode):
            p.write(b"b %d 1000\n" % (2 * bn))
            time.sleep(0.02)
        nxt = p.now() + 50_000_000
        for _ in range(bn):
            sleep_until(p, nxt)
            t_off = t_off_ret = -1
            rc = 0
            t_start = p.now()
            if mode == "ysp_py_pulse":
                rc = p.p.pulse(ON, OFF, w)
                t_ret = p.now()
            elif mode == "ysp_py_pulse_async":
                rc = p.p.pulse_async(ON, OFF, w)
                t_ret = p.now()
            elif mode in ("pyserial_sleep", "pyserial_busy"):
                p.p.write(b"\x55")
                t_ret = p.now()
                if mode == "pyserial_sleep":
                    time.sleep(w)
                else:
                    while p.now() < t_ret + width_ns:
                        pass
                t_off = p.now()
                p.p.write(b"\x00")
                t_off_ret = p.now()
            else:
                p.line(rts, 1)
                t_ret = p.now()
                time.sleep(w)
                t_off = p.now()
                p.line(rts, 0)
                t_off_ret = p.now()
            o["pulses"].write(f"{cell},{mode},{wus},{b},{i},{t_start},{t_ret},{t_off},{t_off_ret},{rc}\n")
            i += 1
            nxt = t_start + width_ns + 50_000_000 + int(random.random() * 100e6)
        sleep_until(p, p.now() + width_ns + 30_000_000)
        read_reports(p, o["board"], cell, mode, wus, b, 2 * bn, not is_line(mode))
        query_gap(p, o["board"], cell, mode, wus, b)
    probes(p, o["probes"], cell, mode, wus, N_PROBE)
    if is_line(mode):
        p.write(b"l 0\n")
    p.close()
    for f in o.values():
        f.flush()


def open_csv(d, name, header):
    path = os.path.join(d, name)
    fresh = not os.path.exists(path)
    f = open(path, "a", newline="\n")
    if fresh:
        f.write(header + "\n")
    return f


def main(argv):
    if len(argv) < 3:
        sys.exit("usage: pulse_bench.py COM3 out_dir [n] [rev] [modes=a,b] [widths=1,2,5,10]")
    dev, d = argv[1], argv[2]
    n, rev, modes, widths = 200, False, list(MODES), [1, 2, 5, 10]
    for a in argv[3:]:
        if a == "rev":
            rev = True
        elif a.startswith("modes="):
            modes = a[6:].split(",")
            for m in modes:
                if m not in MODES:
                    sys.exit(f"pulse_bench: unknown mode {m}")
        elif a.startswith("widths="):
            widths = [int(x) for x in a[7:].split(",")]
        else:
            n = int(a)
    os.makedirs(d, exist_ok=True)
    o = {
        "probes": open_csv(d, "probes.csv", "cell,mode,width_us,t0_ns,t2_ns,rx_us,tx_us"),
        "pulses": open_csv(d, "pulses.csv", "cell,mode,width_us,block,i,t_start_ns,t_ret_ns,t_off_ns,t_off_ret_ns,rc"),
        "board": open_csv(d, "board.csv", "cell,mode,width_us,block,kind,t_us,v"),
        "cells": open_csv(d, "cells.csv", "cell,mode,width_us,run,async_policy,open_ms"),
    }
    run = 4 if rev else 3
    random.seed(run)
    cells = [(m, wi) for wi in range(len(widths)) for m in modes]
    order = list(range(len(cells)))
    if rev:
        order.reverse()
    for k, c in enumerate(order):
        m, wi = cells[c]
        print(f"pulse_bench: cell {k + 1}/{len(cells)} {m} {widths[wi]} ms", file=sys.stderr)
        run_cell(dev, m, widths[wi] * 1_000_000, n, run * 1000 + c, run, o)
    for f in o.values():
        f.close()


if __name__ == "__main__":
    main(sys.argv)
