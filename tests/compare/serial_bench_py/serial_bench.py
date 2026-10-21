"""Python serial bench: pyserial vs Psychtoolbox IOPort (PyPI wheel) vs ysp.serial.

The Python counterpart of ../serial_bench/serial_bench.m. Same parts, same
parameters, same metrics and the same board-stamp fit, so the numbers compare
with the MATLAB tables in docs/serial.md. README.md in this folder says how to
run it and how to read the output.

The orchestrator (no --worker) needs only the standard library. It runs each
API in its own process and its own uv environment, so the third-party wheels
never share an interpreter:

    uv run --no-project python serial_bench.py --port COM3 \
        --ysp C:/path/ysp_serial-0.5.0.2-cp38-abi3-win_amd64.whl

Each worker (--worker API) writes <api>_rows.json and its raw CSVs; the
orchestrator writes summary.txt, summary.csv, <api>_samples.csv and
conditions.txt. Times are microseconds, as in the MATLAB bench.
"""

import argparse
import collections
import csv
import json
import math
import os
import platform
import random
import subprocess
import sys
import threading
import time

NAN = float("nan")

# The PyPI releases this bench was written against. Pinned so a rerun measures
# the same code; change them to measure another release.
REQ = {
    "pyserial": "pyserial==3.5",
    "ioport": "psychtoolbox==3.0.22.2",
    "ioport_tuned": "psychtoolbox==3.0.22.2",
}
APIS = ["pyserial", "ioport", "ioport_tuned", "ysp"]
PARTS = ["clock", "noport", "calls", "rt", "stream"]

DEFAULTS = dict(
    port="", device="echo", baud=115200, label="",
    n_clock=100000, n_floor=100000, n_enum=200, n_open_absent=50,
    n_open=20, n_calls=2000, n_blocking=200, n_read_waiting=500,
    n_rt=1000, n_probe=200, stream_hz=1000, stream_s=10, max_block_s=20,
    write_sizes=[1, 8, 64, 512], absent_port="COM250",
    # Python sends echo probes faster than MATLAB: 200 probes with the rt
    # part's 0-1 ms gap span about 150 ms, one 200 ms bucket of the fit.
    # 2-3 ms apart they span about 0.5 s, so a stream's fit gets about six
    # points and a slope (the ysp-serial async test found the same).
    probe_gap_s=[0.002, 0.003],
)

TUNED = " BlockingBackgroundRead=1 PollLatency=0 HardwareBufferSizes=32768,32768"


# --------------------------------------------------------------------------
# Statistics and the board-clock fit: ports of sb_stats, sb_fit, sb_map,
# sb_unwrap, sb_reply, sb_cmd. Pure Python, so every environment has them.

def stats(v):
    v = sorted(x for x in v if not (isinstance(x, float) and math.isnan(x)))
    n = len(v)
    s = dict(n=n, median=NAN, p99=NAN, max=NAN, min=NAN, mean=NAN)
    if n == 0:
        return s
    m = n // 2
    s["median"] = v[m] if n % 2 else (v[m - 1] + v[m]) / 2
    s["p99"] = v[max(1, math.ceil(0.99 * n)) - 1]
    s["max"] = v[-1]
    s["min"] = v[0]
    s["mean"] = sum(v) / n
    return s


def unwrap(v, ref):
    return [ref + ((x - ref) % 4294967296) for x in v]


def fit(t0, t2, rx, tx):
    """Board clock (us) to host clock (us); see sb_fit.m for the method."""
    F = dict(ok=False, x0=NAN, y0=NAN, slope=1.0, ppm=NAN, unc=NAN, spread=NAN,
             npts=0, span_s=0.0)
    if len(t0) < 2:
        return F
    path = [(c - a) - (d - b) for a, c, b, d in zip(t0, t2, rx, tx)]
    best = {}
    for j, a in enumerate(t0):
        b = math.floor((a - t0[0]) / 200e3)
        if b not in best or path[j] < path[best[b]]:
            best[b] = j
    keep = [best[b] for b in sorted(best)]
    x = [(rx[j] + tx[j]) / 2 for j in keep]
    y = [(t0[j] + t2[j]) / 2 for j in keep]
    F["x0"] = x0 = x[-1]
    F["npts"] = len(keep)
    F["span_s"] = (y[-1] - y[0]) / 1e6
    dx = [xi - x0 for xi in x]
    if F["span_s"] >= 2 and F["npts"] >= 3:
        mx = sum(dx) / len(dx)
        my = sum(y) / len(y)
        sxx = sum((a - mx) ** 2 for a in dx)
        sxy = sum((a - mx) * (b - my) for a, b in zip(dx, y))
        F["slope"] = sxy / sxx
        F["y0"] = my - F["slope"] * mx
    else:
        F["y0"] = stats([b - a for a, b in zip(dx, y)])["median"]
    F["ppm"] = (1 / F["slope"] - 1) * 1e6
    F["unc"] = min(path[j] for j in keep) / 2
    F["spread"] = stats([abs(b - fmap(F, a)) for a, b in zip(x, y)])["p99"]
    F["ok"] = True
    return F


def fmap(F, dev):
    return F["y0"] + F["slope"] * (dev - F["x0"])


def cmd_for(seq, device):
    """'e <seq>\\n' (13 bytes) and the length of its answer."""
    return b"e %010d\n" % seq, (35 if device == "echo" else 13)


def parse_reply(r, seq, device):
    """(ok, board rx, board tx) for the answer r to echo command seq."""
    cmd, _ = cmd_for(seq, device)
    if device != "echo":
        return r == cmd, NAN, NAN
    if len(r) != 35 or r[0:1] != b"R" or r[34:35] != b"\n":
        return False, NAN, NAN
    try:
        if int(r[2:12]) != seq:
            return False, NAN, NAN
        return True, float(int(r[13:23])), float(int(r[24:34]))
    except ValueError:
        return False, NAN, NAN


def write_csv(path, names, rows):
    with open(path, "w", newline="\n") as f:
        f.write(",".join(names) + "\n")
        for row in rows:
            f.write(",".join("" if (isinstance(v, float) and math.isnan(v)) else "%.10g" % v
                             for v in row) + "\n")


# --------------------------------------------------------------------------
# Result rows and the shared metric code (sb_rt_rows, sb_stream_rows).

class Rows:
    def __init__(self, api):
        self.api = api
        self.rows = []

    def add(self, part, metric, values, note=""):
        if not isinstance(values, (list, tuple)):
            values = [values]
        self.rows.append(dict(api=self.api, part=part, metric=metric,
                              values=[float(v) for v in values], note=note))


def rt_rows(R, tag, X, cfg, out):
    ok = X["ok"]
    sel = [i for i, k in enumerate(ok) if k]
    R.add("rt", tag + "_failed", len(ok) - len(sel), "exchanges with a missing or wrong answer (count)")
    t0 = [X["t0"][i] for i in sel]
    t1 = [X["t1"][i] for i in sel]
    t2 = [X["t2"][i] for i in sel]
    w = [X["when"][i] for i in sel]
    R.add("rt", tag + "_rtt", [b - a for a, b in zip(t0, t2)], "write call start to read call return")
    R.add("rt", tag + "_write_call", [b - a for a, b in zip(t0, t1)], "write call duration inside the exchange")
    R.add("rt", tag + "_write_ret_to_read_ret", [b - a for a, b in zip(t1, t2)],
          "write call return to read call return")
    has_w = any(not math.isnan(v) for v in w)
    if has_w:
        R.add("rt", tag + "_stamp_minus_return", [a - b for a, b in zip(w, t2)],
              "API receive stamp minus host time after the read returned")
    if cfg["device"] == "echo" and len(sel) >= 2:
        rx = [X["rx"][i] for i in sel]
        tx = [X["tx"][i] for i in sel]
        ref = rx[0]
        rx = unwrap(rx, ref)
        tx = unwrap(tx, ref)
        F = fit(t0, t2, rx, tx)
        if F["ok"]:
            u = F["unc"]
            R.add("rt", tag + "_host_to_board", [fmap(F, a) - b for a, b in zip(rx, t0)],
                  "write call start to board read (+-%.0f us from the fit)" % u)
            R.add("rt", tag + "_write_ret_to_board", [fmap(F, a) - b for a, b in zip(rx, t1)],
                  "write call return to board read (+-%.0f us)" % u)
            R.add("rt", tag + "_board_to_host", [b - fmap(F, a) for a, b in zip(tx, t2)],
                  "board send stamp to read call return (+-%.0f us)" % u)
            R.add("rt", tag + "_board_turnaround", [b - a for a, b in zip(rx, tx)],
                  "board: command read to answer stamp")
            if has_w:
                R.add("rt", tag + "_stamp_minus_board_send", [a - fmap(F, b) for a, b in zip(w, tx)],
                      "API receive stamp minus mapped board send stamp (+-%.0f us)" % u)
            R.add("rt", tag + "_fit_unc", u, "half the best path time: bound of the one-way split")
            R.add("rt", tag + "_fit_spread", F["spread"], "p99 distance of the fit points from the line")
            R.add("rt", tag + "_fit_ppm", F["ppm"],
                  "board clock rate error, %d points over %.1f s" % (F["npts"], F["span_s"]))
    write_csv(os.path.join(out, "%s_rt_%s.csv" % (R.api, tag)),
              ["seq", "t0_us", "t1_us", "t2_us", "when_us", "board_rx_us", "board_tx_us", "ok"],
              zip(X["seq"], X["t0"], X["t1"], X["t2"], X["when"], X["rx"], X["tx"],
                  [1.0 if k else 0.0 for k in ok]))


def stream_rows(R, mode, S, cfg, out):
    """S: reads (list of bytes), host, stamp (us, NaN if none), probe, nlines,
    cpu_s, wall_s, note. Each A line gets the stamps of the read with its newline."""
    buf = bytearray()
    owner = []
    for i, r in enumerate(S["reads"]):
        buf += r
        owner.extend([i] * len(r))
    k_list, tdev, line_read = [], [], []
    bad = 0
    start = 0
    while True:
        nl = buf.find(b"\n", start)
        if nl < 0:
            break
        ln = buf[start:nl + 1]
        if len(ln) == 26 and ln[0:1] == b"A":
            try:
                tdev.append(float(int(ln[2:12])))
                k_list.append(int(ln[15:25]))
                line_read.append(owner[nl])
            except ValueError:
                bad += 1
        else:
            bad += 1
        start = nl + 1
    nu = len(set(k_list))
    R.add("stream", mode + "_lines_missed", S["nlines"] - nu,
          "of %d lines (count); %s" % (S["nlines"], S["note"]))
    R.add("stream", mode + "_lines_duplicated", len(k_list) - nu, "count")
    R.add("stream", mode + "_bad_lines", bad, "lines of the wrong length or kind (count)")
    if line_read:
        per = collections.Counter(line_read)
        per = [per[i] for i in sorted(per)]
        R.add("stream", mode + "_lines_per_read", per, "lines completed by one read (or callback)")
        R.add("stream", mode + "_coalesced_pct", 100.0 * sum(p for p in per if p > 1) / sum(per),
              "percent of lines that arrived in a read with another line")
    R.add("stream", mode + "_cpu_pct", 100.0 * S["cpu_s"] / S["wall_s"],
          "Python process CPU time / wall time over %.1f s, percent of one core" % S["wall_s"])
    lh = [S["host"][i] for i in line_read]
    ls = [S["stamp"][i] for i in line_read]
    P = S["probe"]
    sel = [i for i, k in enumerate(P["ok"]) if k]
    tmap = [NAN] * len(tdev)
    if len(sel) >= 2 and tdev:
        ref = P["rx"][sel[0]]
        F = fit([P["t0"][i] for i in sel], [P["t2"][i] for i in sel],
                unwrap([P["rx"][i] for i in sel], ref), unwrap([P["tx"][i] for i in sel], ref))
        if F["ok"]:
            tmap = [fmap(F, v) for v in unwrap(tdev, ref)]
            u = F["unc"]
            if any(not math.isnan(v) for v in lh):
                R.add("stream", mode + "_lat_host", [a - b for a, b in zip(lh, tmap)],
                      "host time after the read returned minus mapped board send stamp (+-%.0f us)" % u)
            if any(not math.isnan(v) for v in ls):
                R.add("stream", mode + "_lat_stamp", [a - b for a, b in zip(ls, tmap)],
                      "API receive stamp minus mapped board send stamp (+-%.0f us)" % u)
            R.add("stream", mode + "_fit_unc", u, "half the best probe path time")
            R.add("stream", mode + "_fit_ppm", F["ppm"], "%d points over %.1f s" % (F["npts"], F["span_s"]))
    write_csv(os.path.join(out, "%s_stream_%s.csv" % (R.api, mode)),
              ["k", "board_t_us", "read_index", "host_us", "stamp_us"],
              zip(k_list, tdev, line_read, lh, ls))


def cat_probes(P1, P2):
    return {k: P1[k] + P2[k] for k in ("t0", "t2", "rx", "tx", "ok")}


# --------------------------------------------------------------------------
# API adapters. Each gives the calls the parts time. A timed region holds the
# API call and nothing else that the MATLAB bench does not also time; data is
# converted to the API's own type before the clock starts (prep) and back to
# bytes after it stops (tob).

class PySerialApi:
    name = "pyserial"
    clock_name = "time.perf_counter"

    def __init__(self, cfg):
        import serial
        import serial.tools.list_ports
        self.serial = serial
        self.cfg = cfg
        self.clock = time.perf_counter
        self.version = "pyserial %s from %s" % (serial.__version__, os.path.dirname(serial.__file__))

    def floor_call(self):
        return None

    def list_ports(self):
        return self.serial.tools.list_ports.comports()

    def open(self, port):
        # pyserial asserts DTR and RTS on open by default, as the others do.
        return self.serial.Serial(port, self.cfg["baud"], bytesize=8, parity="N", stopbits=1,
                                  timeout=1, write_timeout=None)

    def open_note(self):
        return "serial.Serial(port, baud, timeout=1): overlapped handle, SetupComm(4096, 4096)"

    def close(self, s):
        s.close()

    prep = staticmethod(lambda b: b)
    tob = staticmethod(lambda r: bytes(r))
    size = staticmethod(len)

    def write(self, s, d):
        s.write(d)

    def discard(self, s):
        if s.in_waiting:
            s.reset_input_buffer()

    def available(self, s):
        return s.in_waiting

    def purge(self, s):
        s.reset_input_buffer()

    def read_empty_loop(self, s, n, clk):
        # A non-blocking read needs timeout=0; setting it reconfigures the
        # port (SetCommTimeouts, SetCommState), so it is set once, outside.
        s.timeout = 0
        d = [0.0] * n
        e = [0.0] * n
        for i in range(n):
            a = clk()
            s.read(64)
            b = clk()
            s.in_waiting
            c = clk()
            d[i] = (b - a) * 1e6
            e[i] = (c - b) * 1e6
        s.timeout = 1
        return d, e, "read(64) with timeout=0, nothing waiting", "in_waiting"

    def read_waiting(self, s, n):
        return s.read(n)

    read_waiting_note = "read(%d), timeout=1, with the bytes waiting"

    def read_block(self, s, n):
        return s.read(n), NAN

    def read_poll(self, s, n):
        nb = s.in_waiting
        if nb:
            return s.read(min(nb, n)), NAN
        return b"", NAN

    def stream_modes(self):
        return ["line", "readline", "poll", "async"]

    def stream_read(self, s, mode, L):
        if mode == "line":
            return s.read(L), NAN
        if mode == "readline":
            return s.readline(), NAN
        nb = s.in_waiting
        return (s.read(nb) if nb else b""), NAN

    stream_notes = {
        "line": "read(26), timeout=1, per line (blocking)",
        "readline": "readline(), timeout=1: read(1) per byte (serialutil.read_until)",
        "poll": "tight loop of in_waiting and read(in_waiting)",
    }

    def stream_async(self, s, cfg, R, nlines, L, clk, writes_only=False):
        import serial.threaded
        lines = collections.deque()

        class Proto(serial.threaded.Protocol):
            # Packetizer splits the same way; the stamp is taken first, when
            # the reader thread's read() has returned with the bytes.
            def __init__(self):
                self.buf = bytearray()

            def data_received(self, data):
                t = clk()
                self.buf += data
                while True:
                    j = self.buf.find(b"\n")
                    if j < 0:
                        break
                    lines.append((bytes(self.buf[:j + 1]), t))
                    del self.buf[:j + 1]

        th = serial.threaded.ReaderThread(s, Proto)
        th.start()
        th.connect()
        if writes_only:
            time.sleep(0.05)
            dw = [0.0] * 20
            nl = b"\n"
            for j in range(20):
                a = clk()
                while clk() - a < 1e-3:
                    pass
                a = clk()
                th.write(nl)
                dw[j] = (clk() - a) * 1e6
            R.add("stream", "async_write_call", dw,
                  "ReaderThread.write(newline) while the reader thread waits for a byte")
            th.stop()
            return None
        cmd = b"s %d %d\n" % (cfg["stream_hz"], nlines)
        a = clk()
        th.write(cmd)
        c0 = time.process_time()
        w0 = clk()
        R.add("stream", "async_cmd_write_call", (w0 - a) * 1e6,
              "ReaderThread.write(stream command) with the reader thread running")
        got = []
        while len(got) < nlines and clk() - w0 < cfg["stream_s"] + 2:
            time.sleep(0.1)
            while lines:
                got.append(lines.popleft())
        w1 = clk()
        c1 = time.process_time()
        th.stop()
        while lines:
            got.append(lines.popleft())
        reads = [g[0] for g in got]
        return dict(reads=reads, host=[NAN] * len(reads), stamp=[g[1] * 1e6 for g in got],
                    cpu_s=c1 - c0, wall_s=w1 - w0,
                    note="serial.threaded.ReaderThread, read(in_waiting or 1) with timeout=1, "
                         "stamp in data_received; main thread in time.sleep(0.1), then drains a deque")


class IOPortApi:
    clock_name = "GetSecs"

    def __init__(self, cfg, tuned):
        import numpy as np
        import psychtoolbox as ptb
        from psychtoolbox import IOPort, GetSecs
        self.np = np
        self.IOPort = IOPort
        self.cfg = cfg
        self.tuned = tuned
        self.name = "ioport_tuned" if tuned else "ioport"
        self.clock = GetSecs
        self.version = "psychtoolbox %s (IOPort %s) from %s" % (
            ptb.__version__, self._ioport_version(), os.path.dirname(ptb.__file__))
        self.conf = ("BaudRate=%d DataBits=8 Parity=None StopBits=1 FlowControl=None DTR=1 RTS=1 "
                     "ReceiveTimeout=1.0%s" % (cfg["baud"], TUNED if tuned else ""))

    def _ioport_version(self):
        v = self.IOPort("Version")
        if isinstance(v, dict):
            return v.get("version", str(v))
        return str(v)

    def floor_call(self):
        return lambda: self.IOPort("Verbosity"), "IOPort('Verbosity')"

    def list_ports(self):
        return None

    def open(self, port):
        h, err = self.IOPort("OpenSerialPort", port, self.conf)[:2]
        if h < 0:
            raise RuntimeError("IOPort could not open %s: %s" % (port, err))
        return h

    def quiet(self, on):
        if on:
            self._old = self.IOPort("Verbosity", 0)
        else:
            self.IOPort("Verbosity", self._old)

    def open_absent(self, port):
        r = self.IOPort("OpenSerialPort", port)
        h = r[0] if isinstance(r, tuple) else r
        if h >= 0:
            self.IOPort("Close", h)

    def open_note(self):
        return "OpenSerialPort '%s'" % self.conf

    def close(self, h):
        self.IOPort("Close", h)

    def prep(self, b):
        return self.np.frombuffer(b, dtype=self.np.uint8).copy()

    def tob(self, r):
        return bytes(self.np.asarray(r).astype(self.np.uint8).ravel())

    @staticmethod
    def size(r):
        # Read returns a 1 x n double matrix: len() is 1 even when n is 0.
        return r.size

    def write(self, h, d):
        self.IOPort("Write", h, d, 0)

    def write_blocking(self, h, d):
        self.IOPort("Write", h, d, 1)

    def discard(self, h):
        self.IOPort("Read", h)

    def available(self, h):
        return self.IOPort("BytesAvailable", h)

    def purge(self, h):
        self.IOPort("Purge", h)

    def read_empty_loop(self, h, n, clk):
        IOPort = self.IOPort
        d = [0.0] * n
        e = [0.0] * n
        for i in range(n):
            a = clk()
            IOPort("Read", h)
            b = clk()
            IOPort("BytesAvailable", h)
            c = clk()
            d[i] = (b - a) * 1e6
            e[i] = (c - b) * 1e6
        return d, e, "Read(h), non-blocking, with nothing waiting", "BytesAvailable(h)"

    def read_waiting(self, h, n):
        return self.IOPort("Read", h, 0, n)[0]

    read_waiting_note = "Read(h, 0, %d) with the bytes waiting"

    def read_block(self, h, n):
        r, when = self.IOPort("Read", h, 1, n)[:2]
        return r, when

    def read_poll(self, h, n):
        r, when = self.IOPort("Read", h, 0, n)[:2]
        return r, when

    def stream_modes(self):
        return ["line", "poll", "async"]

    def stream_read(self, h, mode, L):
        if mode == "line":
            r, when = self.IOPort("Read", h, 1, L)[:2]
        else:
            r, when = self.IOPort("Read", h)[:2]
        return r, when

    stream_notes = {
        "line": "Read(h, 1, 26) per line (blocking)",
        "poll": "tight loop of Read(h) (non-blocking)",
    }

    def stream_async(self, h, cfg, R, nlines, L, clk, writes_only=False):
        # As sb_ioport.m: the ring holds the whole stream with room, because
        # the reader thread never checks for a full ring
        # (PsychSerialWindowsGlue.c:1322-1335), and the main thread empties
        # it every 0.1 s.
        IOPort = self.IOPort
        np = self.np
        IOPort("ConfigureSerialPort", h,
               "InputBufferSize=%d Terminator=10 BlockingBackgroundRead=1 ReadFilterFlags=4 "
               "StartBackgroundRead=%d" % (L * (nlines + 1000), L))
        if writes_only:
            time.sleep(0.05)
            nl = self.prep(b"\n")
            dw = [0.0] * 5
            for j in range(5):
                a = clk()
                while clk() - a < 1e-3:
                    pass
                a = clk()
                IOPort("Write", h, nl, 0)
                dw[j] = (clk() - a) * 1e6
            R.add("stream", "async_write_call", dw,
                  "Write(h, newline, 0) while the background reader waits for a byte")
            IOPort("ConfigureSerialPort", h, "StopBackgroundRead")
            return None
        cmd = self.prep(b"s %d %d\n" % (cfg["stream_hz"], nlines))
        a = clk()
        IOPort("Write", h, cmd, 0)
        c0 = time.process_time()
        w0 = clk()
        R.add("stream", "async_cmd_write_call", (w0 - a) * 1e6,
              "Write(h, stream command, 0) with the background reader running")
        reads, stamp = [], []
        got = 0
        nq = 0
        while got < nlines * L and clk() - w0 < cfg["stream_s"] + 2:
            time.sleep(0.1)
            while True:
                r, when, err = IOPort("Read", h, 0, L)[:3]
                if err:
                    raise RuntimeError("async Read: %s" % err)
                r = np.asarray(r).ravel()
                if r.size == 0:
                    break
                if not r.any():
                    nq += 1      # a reader read that timed out stores a quantum of zeros
                    continue
                b = bytes(r.astype(np.uint8))
                b = b[:b.rfind(b"\n") + 1]
                reads.append(b)
                stamp.append(when * 1e6)
                got += len(b)
        w1 = clk()
        c1 = time.process_time()
        IOPort("ConfigureSerialPort", h, "StopBackgroundRead")
        return dict(reads=reads, host=[NAN] * len(reads), stamp=stamp, cpu_s=c1 - c0, wall_s=w1 - w0,
                    note="background reader thread, BlockingBackgroundRead=1, ReadFilterFlags=4, "
                         "InputBufferSize for %d lines; main thread in time.sleep(0.1), then "
                         "Read(h, 0, %d) until empty; %d empty quanta" % (nlines + 1000, L, nq))


class YspApi:
    name = "ysp"
    clock_name = "ysp.serial.now"

    def __init__(self, cfg):
        import ysp.serial as yser
        self.yser = yser
        self.cfg = cfg
        self.clock = yser.now
        self.version = "ysp.serial %s from %s" % (yser.__version__, os.path.dirname(yser.__file__))

    def floor_call(self):
        return self.yser.now, "ysp.serial.now()"

    def list_ports(self):
        return self.yser.list_ports()

    def open(self, port):
        return self.yser.Port(port, baud=self.cfg["baud"])

    def open_note(self):
        return "Port(port, baud=baud): 8N1, DTR and RTS asserted"

    def close(self, p):
        p.close()

    prep = staticmethod(lambda b: b)
    tob = staticmethod(lambda r: bytes(r))
    size = staticmethod(len)

    def write(self, p, d):
        p.write(d)

    def discard(self, p):
        p.read(4096, 0)

    def available(self, p):
        return p.available()

    def purge(self, p):
        p.purge()

    def read_empty_loop(self, p, n, clk):
        d = [0.0] * n
        e = [0.0] * n
        for i in range(n):
            a = clk()
            p.read(64, 0)
            b = clk()
            p.available()
            c = clk()
            d[i] = (b - a) * 1e6
            e[i] = (c - b) * 1e6
        return d, e, "read(64, 0) with nothing waiting", "available()"

    def read_waiting(self, p, n):
        return p.read(n, 0)

    read_waiting_note = "read(%d, 0) with the bytes waiting"

    def read_block(self, p, n):
        return p.read(n, 1.0, all=True), NAN

    def read_poll(self, p, n):
        return p.read(n, 0), NAN

    def stream_modes(self):
        return ["line", "poll", "async"]

    def stream_read(self, p, mode, L):
        if mode == "line":
            return p.read(L, 0.2, all=True), NAN
        return p.read(4096, 0), NAN

    stream_notes = {
        "line": "read(26, 0.2, all=True) per line",
        "poll": "tight loop of read(4096, 0)",
    }

    def stream_async(self, p, cfg, R, nlines, L, clk, writes_only=False):
        p.async_start(terminator=b"\n", capacity=nlines + 1000)
        if writes_only:
            time.sleep(0.05)
            dw = [0.0] * 20
            nl = b"\n"
            for j in range(20):
                a = clk()
                while clk() - a < 1e-3:
                    pass
                a = clk()
                p.write(nl)
                dw[j] = (clk() - a) * 1e6
            R.add("stream", "async_write_call", dw, "write(newline) while the reader thread waits for a byte")
            p.async_stop()
            return None
        cmd = b"s %d %d\n" % (cfg["stream_hz"], nlines)
        a = clk()
        p.write(cmd)
        c0 = time.process_time()
        w0 = clk()
        R.add("stream", "async_cmd_write_call", (w0 - a) * 1e6,
              "write(stream command) with the reader thread running")
        got = []
        while len(got) < nlines and clk() - w0 < cfg["stream_s"] + 2:
            time.sleep(0.1)
            got.extend(p.async_read(first=True))
        w1 = clk()
        c1 = time.process_time()
        st = p.async_stop()
        nfirst = sum(1 for g in got if g[2] != g[1])
        return dict(reads=[g[0] for g in got], host=[NAN] * len(got), stamp=[g[1] * 1e6 for g in got],
                    cpu_s=c1 - c0, wall_s=w1 - w0,
                    note="async_start reader thread, stamp when the read with the newline returned "
                         "(first-byte stamp differs on %d lines); main thread in time.sleep(0.1), then "
                         "async_read; %d overflows" % (nfirst, st["overflows"]))


# --------------------------------------------------------------------------
# The parts. One function per part, shared by every adapter.

def busy_until(clk, t):
    while clk() < t:
        pass


def run_rt(A, h, cfg, n, seq0, strat, gap):
    clk = A.clock
    A.write(h, A.prep(b"\n"))           # ends any partial line on the board
    time.sleep(0.05)
    A.purge(h)
    X = dict(seq=[seq0 + i + 1 for i in range(n)], t0=[NAN] * n, t1=[NAN] * n, t2=[NAN] * n,
             when=[NAN] * n, rx=[NAN] * n, tx=[NAN] * n, ok=[False] * n)
    read = A.read_block if strat == "block" else A.read_poll
    write = A.write
    tob = A.tob
    for i in range(n):
        seq = X["seq"][i]
        cmd, L = cmd_for(seq, cfg["device"])
        cmd = A.prep(cmd)
        busy_until(clk, clk() + gap[0] + random.random() * (gap[1] - gap[0]))   # leave the USB frame phase
        t0 = clk()
        write(h, cmd)
        t1 = clk()
        if strat == "block":
            r, when = read(h, L)
            t2 = clk()
            r = tob(r)
        else:
            r = b""
            while len(r) < L:
                q, when = read(h, L - len(r))
                r += tob(q)
                t2 = clk()
                if t2 - t0 > 1:
                    break
        ok, rx, tx = parse_reply(r, seq, cfg["device"])
        X["ok"][i] = ok
        X["rx"][i] = rx
        X["tx"][i] = tx
        X["t0"][i] = t0 * 1e6
        X["t1"][i] = t1 * 1e6
        X["t2"][i] = t2 * 1e6
        X["when"][i] = when * 1e6
        if not ok:
            time.sleep(0.02)
            A.purge(h)
    return X


def part_clock(A, R, cfg):
    clk = A.clock
    n = cfg["n_clock"]
    d = [0.0] * n
    for i in range(n):
        a = clk()
        b = clk()
        d[i] = (b - a) * 1e6
    R.add("clock", "clock_pair", d, "two back-to-back %s() calls" % A.clock_name)
    R.add("clock", "clock_step", min(x for x in d if x > 0), "smallest nonzero difference")


def part_noport(A, R, cfg):
    clk = A.clock
    n = cfg["n_floor"]
    f = A.floor_call()
    if f is None:
        R.add("noport", "floor_batch_mean", NAN, "pyserial has no call without a port object")
    else:
        fn, name = f
        a = clk()
        for _ in range(n):
            fn()
        b = clk()
        R.add("noport", "floor_batch_mean", (b - a) * 1e6 / n,
              "mean of %d %s calls: the cheapest call into the API" % (n, name))
    # The interpreter's own floor: a call of a Python function that does nothing.
    noop = lambda: None  # noqa: E731
    a = clk()
    for _ in range(n):
        noop()
    b = clk()
    R.add("noport", "py_call_floor", (b - a) * 1e6 / n,
          "mean of %d calls of an empty Python lambda, loop included" % n)
    lp = A.list_ports()
    if lp is None:
        R.add("noport", "list_ports", NAN, "IOPort has no port enumeration")
    else:
        d = [0.0] * cfg["n_enum"]
        for i in range(cfg["n_enum"]):
            a = clk()
            p = A.list_ports()
            b = clk()
            d[i] = (b - a) * 1e6
        R.add("noport", "list_ports", d, "%d ports found" % len(p))
    d = [0.0] * cfg["n_open_absent"]
    if hasattr(A, "quiet"):
        A.quiet(True)
    for i in range(cfg["n_open_absent"]):
        a = clk()
        try:
            if hasattr(A, "open_absent"):
                A.open_absent(cfg["absent_port"])
            else:
                A.close(A.open(cfg["absent_port"]))
        except Exception:
            pass
        b = clk()
        d[i] = (b - a) * 1e6
    if hasattr(A, "quiet"):
        A.quiet(False)
    R.add("noport", "open_absent", d, "open %s, which does not exist: the error path, with try/except"
          % cfg["absent_port"])
    for i in range(cfg["n_open_absent"]):
        a = clk()
        try:
            raise RuntimeError("baseline")
        except RuntimeError:
            pass
        b = clk()
        d[i] = (b - a) * 1e6
    R.add("noport", "py_error_baseline", d, "try: raise; except: pass, no port call")


def pace(cfg, nbytes):
    p = 1000.0 if cfg["device"] == "echo" else max(1000.0, 1.5 * nbytes * 10 / cfg["baud"] * 1e6)
    return min(cfg["n_calls"], max(50, int(cfg["max_block_s"] * 1e6 // p))), p


def part_calls_open(A, R, cfg):
    clk = A.clock
    dop = [0.0] * cfg["n_open"]
    dcl = [0.0] * cfg["n_open"]
    for i in range(cfg["n_open"]):
        a = clk()
        h = A.open(cfg["port"])
        b = clk()
        A.close(h)
        c = clk()
        dop[i] = (b - a) * 1e6
        dcl[i] = (c - b) * 1e6
    R.add("calls", "open", dop, A.open_note())
    R.add("calls", "close", dcl, "")


def part_calls(A, h, R, cfg):
    clk = A.clock
    for blocking in ([0, 1] if hasattr(A, "write_blocking") else [0]):
        write = A.write_blocking if blocking else A.write
        for sz in cfg["write_sizes"]:
            data = A.prep(b"z" * sz)    # no newline: the echo board drops it
            n, p = pace(cfg, sz)
            if blocking:
                n = min(n, cfg["n_blocking"])
            d = [0.0] * n
            for i in range(n):
                a = clk()
                write(h, data)
                b = clk()
                d[i] = (b - a) * 1e6
                A.discard(h)
                busy_until(clk, b + p / 1e6)
            if blocking:
                R.add("calls", "write_%d_blocking" % sz, d, "Write(h, data, 1), the default; gap %.0f us" % p)
            else:
                R.add("calls", "write_%d" % sz, d, "gap %.0f us" % p)
    A.write(h, A.prep(b"\n"))
    time.sleep(0.05)
    A.purge(h)
    d, e, nd, ne = A.read_empty_loop(h, cfg["n_calls"], clk)
    R.add("calls", "read_empty", d, nd)
    R.add("calls", "available", e, ne)
    if cfg["device"] != "none":
        m = cfg["n_read_waiting"]
        d = [NAN] * m
        L = 0
        for i in range(m):
            cmd, L = cmd_for(i + 1, cfg["device"])
            A.write(h, A.prep(cmd))
            t = clk()
            while A.available(h) < L and clk() - t < 0.2:
                pass
            a = clk()
            r = A.read_waiting(h, L)
            b = clk()
            if len(A.tob(r)) == L:
                d[i] = (b - a) * 1e6
        R.add("calls", "read_waiting", d, A.read_waiting_note % L)


def part_rt(A, h, R, cfg, out):
    for strat in ("block", "poll"):
        X = run_rt(A, h, cfg, cfg["n_rt"], 0, strat, (0.0, 1e-3))
        rt_rows(R, strat, X, cfg, out)


def part_stream(A, h, R, cfg, out):
    clk = A.clock
    L = 26
    nlines = int(round(cfg["stream_hz"] * cfg["stream_s"]))
    gap = tuple(cfg["probe_gap_s"])
    for mode in A.stream_modes():
        if mode == "async":
            # Writes beside a waiting reader are timed with a reader of their
            # own, before the probes: IOPort's take seconds each, and the
            # stream's clock fit must not span them (it lost its slope over
            # 200 s in a trial run and put the stamps 1.3 ms early).
            A.stream_async(h, cfg, R, nlines, L, clk, writes_only=True)
            A.write(h, A.prep(b"\n"))
            time.sleep(0.05)
            A.purge(h)
        P1 = run_rt(A, h, cfg, cfg["n_probe"], 1000000, "block", gap)
        if mode == "async":
            S = A.stream_async(h, cfg, R, nlines, L, clk)
        else:
            reads, host, stamp = [], [], []
            got = 0
            cmd = A.prep(b"s %d %d\n" % (cfg["stream_hz"], nlines))
            rd = A.stream_read
            tob = A.tob
            size = A.size
            c0 = time.process_time()
            w0 = clk()
            A.write(h, cmd)
            while got < nlines * L and clk() - w0 < cfg["stream_s"] + 2:
                r, when = rd(h, mode, L)
                if size(r):
                    t = clk()
                    r = tob(r)
                    reads.append(r)
                    host.append(t * 1e6)
                    stamp.append(when * 1e6)
                    got += len(r)
            w1 = clk()
            c1 = time.process_time()
            S = dict(reads=reads, host=host, stamp=stamp, cpu_s=c1 - c0, wall_s=w1 - w0,
                     note=A.stream_notes[mode])
        A.write(h, A.prep(b"x\n"))
        time.sleep(0.05)
        A.purge(h)
        P2 = run_rt(A, h, cfg, cfg["n_probe"], 2000000, "block", gap)
        S["probe"] = cat_probes(P1, P2)
        S["nlines"] = nlines
        stream_rows(R, mode, S, cfg, out)


def worker(api, cfg, out):
    if api == "pyserial":
        A = PySerialApi(cfg)
    elif api in ("ioport", "ioport_tuned"):
        A = IOPortApi(cfg, api == "ioport_tuned")
    elif api == "ysp":
        A = YspApi(cfg)
    else:
        raise SystemExit("unknown api %s" % api)
    R = Rows(api)
    info = dict(api=api, version=A.version, clock=A.clock_name, python=sys.version,
                executable=sys.executable)
    want = lambda p: p in cfg["parts"]  # noqa: E731
    try:
        if want("clock"):
            part_clock(A, R, cfg)
        if want("noport"):
            part_noport(A, R, cfg)
        if cfg["port"]:
            if want("calls"):
                part_calls_open(A, R, cfg)
            h = A.open(cfg["port"])
            try:
                A.purge(h)
                if cfg["device"] == "echo":
                    # The identity line goes to conditions.txt, so a run says
                    # which firmware answered.
                    A.write(h, A.prep(b"\ni\n"))
                    time.sleep(0.2)
                    r = b""
                    while True:
                        q = A.tob(A.read_poll(h, 256)[0])
                        if not q:
                            break
                        r += q
                    info["board"] = r.decode("ascii", "replace").strip()
                    A.purge(h)
                if want("calls"):
                    part_calls(A, h, R, cfg)
                if want("rt") and cfg["device"] != "none":
                    part_rt(A, h, R, cfg, out)
                if want("stream") and cfg["device"] == "echo":
                    part_stream(A, h, R, cfg, out)
            finally:
                try:
                    A.close(h)
                except Exception:
                    pass
    except Exception as e:  # keep what was measured, as serial_bench.m does
        import traceback
        traceback.print_exc()
        R.add("error", "stopped", NAN, repr(e))
    with open(os.path.join(out, "%s_rows.json" % api), "w", newline="\n") as f:
        json.dump(dict(info=info, rows=R.rows), f)


# --------------------------------------------------------------------------
# The orchestrator.

def write_outputs(cfg, out, apis, infos, rows, order):
    with open(os.path.join(out, "summary.csv"), "w", newline="\n") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["api", "part", "metric", "clock", "n", "median", "p99", "max", "min", "mean", "note"])
        lines = ["%-12s %-7s %-34s %6s %10s %10s %10s %10s  %s"
                 % ("api", "part", "metric", "n", "median", "p99", "max", "mean", "note")]
        for r in rows:
            s = stats(r["values"])
            clock = infos.get(r["api"], {}).get("clock", "")
            w.writerow([r["api"], r["part"], r["metric"], clock, s["n"]]
                       + ["%.6g" % s[k] for k in ("median", "p99", "max", "min", "mean")] + [r["note"]])
            lines.append("%-12s %-7s %-34s %6d %10.4g %10.4g %10.4g %10.4g  %s"
                         % (r["api"], r["part"], r["metric"], s["n"], s["median"], s["p99"],
                            s["max"], s["mean"], r["note"]))
    txt = "\n".join(lines)
    print(txt)
    with open(os.path.join(out, "summary.txt"), "w", newline="\n") as f:
        f.write("Times in microseconds unless the note says count or percent.\n" + txt + "\n")
    for api in apis:
        with open(os.path.join(out, "%s_samples.csv" % api), "w", newline="\n") as f:
            f.write("part,metric,value\n")
            for r in rows:
                if r["api"] == api:
                    for v in r["values"]:
                        f.write("%s,%s,%.10g\n" % (r["part"], r["metric"], v))

    def ps(cmd):
        try:
            return subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True,
                                  text=True, timeout=30).stdout.strip()
        except Exception:
            return "?"
    with open(os.path.join(out, "conditions.txt"), "w", newline="\n") as f:
        f.write("date: %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        f.write("label: %s\n" % cfg["label"])
        f.write("api order: %s\n" % " ".join(order))
        f.write("orchestrator Python: %s\n" % sys.version.replace("\n", " "))
        f.write("OS: %s\n" % platform.platform())
        if sys.platform == "win32":
            f.write("CPU: %s\n" % ps("(Get-CimInstance Win32_Processor).Name"))
            pw = ps("(Get-CimInstance Win32_Battery).BatteryStatus")
            f.write("power: %s\n" % {"2": "AC", "1": "battery"}.get(pw, pw))
        for api in order:
            i = infos.get(api)
            if i:
                f.write("api %s: %s; clock %s; Python %s at %s\n" % (
                    api, i["version"], i["clock"], i["python"].split()[0], i["executable"]))
                if "board" in i:
                    f.write("api %s: board identity: %s\n" % (api, i["board"]))
            else:
                f.write("api %s: no result\n" % api)
        for k in sorted(cfg):
            f.write("cfg.%s: %s\n" % (k, cfg[k]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--worker", help="internal: run one API in this process")
    ap.add_argument("--cfg", help="internal: the worker's cfg.json")
    ap.add_argument("--port", default="")
    ap.add_argument("--device", default="echo", choices=["none", "loopback", "echo"])
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--apis", default=",".join(APIS), help="comma list, run in this order")
    ap.add_argument("--parts", default=",".join(PARTS))
    ap.add_argument("--ysp", default="", help="requirement for ysp.serial: a wheel path, or ysp-serial==X")
    ap.add_argument("--python", default="3.13", help="interpreter request for uv run --python")
    ap.add_argument("--out", default="")
    ap.add_argument("--label", default="")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=VALUE",
                    help="override a count or size, e.g. --set n_rt=100 (JSON value)")
    a = ap.parse_args()

    if a.worker:
        with open(a.cfg) as f:
            cfg = json.load(f)
        worker(a.worker, cfg, os.path.dirname(os.path.abspath(a.cfg)))
        return

    cfg = dict(DEFAULTS)
    cfg.update(port=a.port, device=a.device if a.port else "none", baud=a.baud, label=a.label,
               parts=a.parts.split(","), python=a.python)
    for kv in a.set:
        k, v = kv.split("=", 1)
        if k not in DEFAULTS:
            raise SystemExit("unknown --set name %s" % k)
        cfg[k] = json.loads(v)
    if not a.port:
        print("serial_bench: no port given: running the clock and noport parts only")
    out = a.out or os.path.join(os.environ.get("TEMP", "/tmp"), "ysp_serial_bench_py",
                                time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    cfgpath = os.path.join(out, "cfg.json")
    with open(cfgpath, "w", newline="\n") as f:
        json.dump(cfg, f)
    order = a.apis.split(",")
    req = dict(REQ)
    req["ysp"] = a.ysp
    infos, rows = {}, []
    here = os.path.abspath(__file__)
    for api in order:
        if not req.get(api):
            print("serial_bench: SKIPPED %s: no requirement (give --ysp for ysp)" % api)
            continue
        # --isolated: a fresh environment per API, so the wheels never meet.
        cmd = ["uv", "run", "--no-project", "--isolated", "--python", a.python, "--with", req[api],
               "python", here, "--worker", api, "--cfg", cfgpath]
        print("serial_bench: running %s (%s)" % (api, req[api]), flush=True)
        rc = subprocess.run(cmd).returncode
        p = os.path.join(out, "%s_rows.json" % api)
        if rc != 0 or not os.path.exists(p):
            print("serial_bench: %s failed (exit %d)" % (api, rc))
            continue
        with open(p) as f:
            d = json.load(f)
        infos[api] = d["info"]
        rows += d["rows"]
    write_outputs(cfg, out, [x for x in order if x in infos], infos, rows, order)
    print("serial_bench: results in %s" % out)


if __name__ == "__main__":
    main()
