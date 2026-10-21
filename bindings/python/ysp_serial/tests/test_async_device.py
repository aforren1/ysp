"""Tests of the async reader against an echo board (firmware/ysp_echo).

Skipped unless YSP_SERIAL_PORT names the board's port (COM3, /dev/ttyACM0).
The board answers "e <seq>" with "R <seq> <t_rx> <t_tx>" and streams
"A <t> 1 <k>" lines on "s <hz> <n>"; its stamps are micros().

The latency method is tests/compare/serial_bench's (sb_fit.m): echo
exchanges before and after a stream, the best one of each 200 ms, a line
through their midpoints maps board time to host time. A one-way latency is
good to +-unc (half the best path time); its spread is measured.

Run with -s to see the numbers. YSP_SERIAL_RESULTS=<dir> also writes them
to <dir>/async_device.json.
"""
import json
import math
import os
import random
import statistics
import time

import pytest

import ysp.serial as yser

PORT = os.environ.get("YSP_SERIAL_PORT")
pytestmark = pytest.mark.skipif(not PORT, reason="set YSP_SERIAL_PORT to a ysp_echo board")

RESULTS = {}
STREAM_HZ = 1000
STREAM_S = 10
N_PROBE = 200
LINE = 26          # "A <10> 1 <10>\n"
ECHO = 35          # "R <10> <10> <10>\n"


def setup_module(module):
    RESULTS["conditions"] = {
        "port": PORT, "version": yser.__version__, "date": time.strftime("%Y-%m-%d %H:%M"),
        "stream_hz": STREAM_HZ, "stream_s": STREAM_S, "n_probe": N_PROBE,
    }


def teardown_module(module):
    out = os.environ.get("YSP_SERIAL_RESULTS")
    if out and len(RESULTS) > 1:
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "async_device.json"), "w", newline="\n") as f:
            json.dump(RESULTS, f, indent=1)


def stats(v):
    """median and p99 as serial_bench's sb_stats: p99 is the order statistic
    at ceil(0.99 n)."""
    v = sorted(v)
    n = len(v)
    return {"n": n, "median": statistics.median(v), "p99": v[max(1, math.ceil(0.99 * n)) - 1],
            "max": v[-1], "min": v[0]}


def show(name, s, unit="us"):
    print(f"{name}: median {s['median']:.1f} {unit}, p99 {s['p99']:.1f} {unit}, "
          f"max {s['max']:.1f} {unit}, n {s['n']}")


@pytest.fixture
def port():
    p = yser.Port(PORT)
    quiet(p)
    p.write(b"i\n")
    ident = b""
    t_end = yser.now() + 1.0
    while not ident.endswith(b"\n") and yser.now() < t_end:
        ident += p.read(64, 0.1)    # the line can arrive in more than one read
    if b"echo" not in ident:
        p.close()
        pytest.skip(f"{PORT} is not a ysp_echo board (answered {ident!r})")
    quiet(p)
    yield p
    p.close()


def quiet(p):
    """End a stream and a partial command, and drop what is buffered."""
    p.async_stop()
    p.write(b"\nx\n")
    time.sleep(0.05)
    p.purge()


def spin_until(t):
    while yser.now() < t:
        pass


def parse_echo(r, seq):
    if len(r) != ECHO or r[:1] != b"R" or r[-1:] != b"\n" or int(r[2:12]) != seq:
        return None
    return int(r[13:23]), int(r[24:34])


def probes(p, n, seq0):
    """n echo exchanges with a blocking read: (t0, t2, rx, tx), host us and
    board us, of the good ones."""
    out = []
    for i in range(n):
        seq = seq0 + i
        # Random: leave the USB frame phase. The 2 ms floor makes one set
        # span more than two of the fit's 200 ms buckets: Python sends
        # faster than the MATLAB bench, and a fit of two points falls back
        # to a slope of 1.
        spin_until(yser.now() + 2e-3 + random.random() * 1e-3)
        t0 = yser.now()
        p.write(b"e %010d\n" % seq)
        r = p.read(ECHO, 1.0, all=True)
        t2 = yser.now()
        b = parse_echo(r, seq)
        if b:
            out.append((t0 * 1e6, t2 * 1e6, b[0], b[1]))
        else:
            time.sleep(0.02)
            p.purge()
    return out


def unwrap(v, ref):
    return ref + ((v - ref) % (1 << 32))


def fit(P):
    """sb_fit.m: map board us to host us. Returns (map, unc_us, ppm, points)."""
    ref = P[0][2]
    t0 = [e[0] for e in P]
    t2 = [e[1] for e in P]
    rx = [unwrap(e[2], ref) for e in P]
    tx = [unwrap(e[3], ref) for e in P]
    path = [(t2[i] - t0[i]) - (tx[i] - rx[i]) for i in range(len(P))]
    best = {}
    for i in range(len(P)):
        b = int((t0[i] - t0[0]) // 200e3)
        if b not in best or path[i] < path[best[b]]:
            best[b] = i
    keep = [best[b] for b in sorted(best)]
    x = [(rx[i] + tx[i]) / 2 for i in keep]
    y = [(t0[i] + t2[i]) / 2 for i in keep]
    x0 = x[-1]
    span_s = (y[-1] - y[0]) / 1e6
    slope = 1.0
    if span_s >= 2 and len(keep) >= 3:
        xm = statistics.fmean(xi - x0 for xi in x)
        ym = statistics.fmean(y)
        sxx = sum((xi - x0 - xm) ** 2 for xi in x)
        sxy = sum((xi - x0 - xm) * (yi - ym) for xi, yi in zip(x, y))
        slope = sxy / sxx
        y0 = ym - slope * xm
    else:
        y0 = statistics.median(yi - (xi - x0) for xi, yi in zip(x, y))
    unc = min(path[i] for i in keep) / 2
    return (lambda dev: y0 + slope * (unwrap(dev, ref) - x0)), unc, (1 / slope - 1) * 1e6, len(keep)


def write_costs(p, n, gap):
    """Host time of n one-byte writes of b'\\n' (the board ignores an empty
    line), gap seconds apart, in us."""
    dw = []
    for _ in range(n):
        spin_until(yser.now() + gap)
        a = yser.now()
        p.write(b"\n")
        dw.append((yser.now() - a) * 1e6)
    return dw


# --- behavior ---------------------------------------------------------------

def test_lifecycle(port):
    port.async_start()
    with pytest.raises(yser.Error, match="already running"):
        port.async_start()
    for call in (lambda: port.read(1, 0), port.available, port.interrupt,
                 lambda: port.purge(rx=True)):
        with pytest.raises(yser.Error, match="owns the input"):
            call()
    port.purge(rx=False, tx=True)              # writer role stays allowed
    assert port.async_read() == []
    assert port.async_wait(0) == 0
    t = yser.now()
    assert port.async_wait(0.05) == 0
    assert yser.now() - t >= 0.045
    st = port.async_stop()
    assert st["running"] is False and st["error"] is None
    assert port.async_stop() is None
    assert port.async_stats == st              # kept until the next start
    with pytest.raises(yser.Error, match="not running"):
        port.async_read()
    assert port.read(64, 0.01) == b""          # the reader role is back
    port.async_start(terminator=None)          # restart, one record per read
    port.write(b"e 0000000007\n")
    assert port.async_wait(1.0) >= 1
    time.sleep(0.01)
    got = b"".join(r[0] for r in port.async_read())
    assert parse_echo(got, 7)
    port.close()                                # stops the reader
    assert port.async_stats["running"] is False
    with pytest.raises(yser.Closed):
        port.async_read()


def read_line_report(p, timeout=0.5):
    """The board's next "L <t> <dtr> <rts>" report as (dtr, rts), or None."""
    buf = b""
    t_end = yser.now() + timeout
    while yser.now() < t_end:
        buf += p.read(64, 0.05)
        i = buf.find(b"L ")
        if i >= 0 and len(buf) >= i + 17:
            ln = buf[i:i + 17]
            return int(ln[13:14]), int(ln[15:16])
    return None


def test_driver_and_rts_reach_the_board(port):
    # 0.6.1: the extra DTR request goes only with usbser.sys, and an RTS
    # change reaches the board either way.
    if os.name == "nt":
        assert port.driver != ""
        assert port.rts_via_dtr == (port.driver.lower() == "usbser")
    else:
        assert port.driver == "" and port.rts_via_dtr is False
    port.set_dtr(False)
    port.set_rts(False)
    time.sleep(0.02)
    port.write(b"l 1\n")
    assert read_line_report(port) == (0, 0)
    for rts in (1, 0, 1):
        port.set_rts(bool(rts))
        assert read_line_report(port) == (0, rts)
    port.write(b"l 0\n")
    quiet(port)


def test_wait_is_interrupted_by_stop_from_another_thread(port):
    import threading
    port.async_start()
    caught = []

    def waiter():
        try:
            port.async_wait(math.inf)
        except yser.Interrupted as e:
            caught.append(e)

    th = threading.Thread(target=waiter)
    th.start()
    time.sleep(0.1)
    port.async_stop()
    th.join(2.0)
    assert not th.is_alive() and caught


def test_fixed_records_and_overflow(port):
    # 50 lines into a ring of 8: the reader keeps 8 and counts the rest.
    port.async_start(record_size=LINE, capacity=8)
    port.write(b"s 1000 50\n")
    time.sleep(0.2)
    recs = port.async_read(first=True)
    st = port.async_stop()
    assert len(recs) == 8
    assert all(len(r[0]) == LINE and r[0][:2] == b"A " for r in recs)
    assert all(r[2] <= r[1] for r in recs)     # first-byte stamp is not later
    assert st["lines"] == 50 and st["overflows"] == 42 and st["max_used"] == 8


def test_echo_round_trips_with_async_on(port):
    # The same exchanges with a blocking read first: a median moves by 100 to
    # 150 us from one open to the next (docs/serial.md, audit difference 7),
    # so only a baseline from the same open compares.
    base = []
    for seq in range(1000):
        spin_until(yser.now() + random.random() * 1e-3)
        t0 = yser.now()
        port.write(b"e %010d\n" % seq)
        r = port.read(ECHO, 1.0, all=True)
        base.append((yser.now() - t0) * 1e6)
        assert parse_echo(r, seq), r
    port.async_start()
    time.sleep(0.05)
    rtt, host = [], []
    for seq in range(1000):
        spin_until(yser.now() + random.random() * 1e-3)
        t0 = yser.now()
        port.write(b"e %010d\n" % seq)
        assert port.async_wait(1.0) == 1
        (r, t), = port.async_read()
        t2 = yser.now()
        assert parse_echo(r, seq), r
        rtt.append((t - t0) * 1e6)
        host.append((t2 - t0) * 1e6)
    st = port.async_stop()
    assert st["overflows"] == 0 and st["lines"] == 1000
    RESULTS["echo_rtt_read_us"] = stats(base)
    RESULTS["echo_rtt_stamp_us"] = stats(rtt)
    RESULTS["echo_rtt_host_us"] = stats(host)
    print()
    show("echo round trip, blocking read, no reader", RESULTS["echo_rtt_read_us"])
    show("echo round trip, write to the reader's stamp", RESULTS["echo_rtt_stamp_us"])
    show("echo round trip, write to async_read's return", RESULTS["echo_rtt_host_us"])


# --- measurement ------------------------------------------------------------

def stream(port, mode, seq0):
    """One stream of STREAM_HZ lines per second for STREAM_S s with the reader
    on. mode 'poll': the main thread sleeps 0.1 s, then async_read, as the
    MEX bench's pause(0.1). mode 'wait': async_wait(1.0), then async_read,
    with the time the records reach Python. mode 'line': no reader, a
    blocking read(26, 0.2, all=True) per line, stamped with now() after it
    returns, as the bench's 'line' mode."""
    nlines = STREAM_HZ * STREAM_S
    P1 = probes(port, N_PROBE, seq0)
    if mode != "line":
        port.async_start()
        time.sleep(0.05)
    recs, host = [], []
    c0 = time.process_time()
    w0 = yser.now()
    port.write(b"s %d %d\n" % (STREAM_HZ, nlines))
    deadline = w0 + STREAM_S + 2
    while len(recs) < nlines and yser.now() < deadline:
        if mode == "line":
            r = port.read(LINE, 0.2, all=True)
            t = yser.now()
            if r:
                recs.append((r, t))
                host.append(t)
            continue
        if mode == "poll":
            time.sleep(0.1)
        elif port.async_wait(1.0) == 0:
            continue
        got = port.async_read()
        t = yser.now()
        recs += got
        host += [t] * len(got)
    w1 = yser.now()
    c1 = time.process_time()
    st = port.async_stop() or {"overflows": 0, "max_used": 0, "reads": len(recs), "policy": None}
    quiet(port)
    P2 = probes(port, N_PROBE, seq0 + N_PROBE)
    to_host, unc, ppm, npts = fit(P1 + P2)
    ks, lat, lat_host = set(), [], []
    bad = 0
    for (r, t), h in zip(recs, host):
        if len(r) != LINE or r[:2] != b"A ":
            bad += 1
            continue
        ks.add(int(r[15:25]))
        tb = to_host(int(r[2:12]))
        lat.append(t * 1e6 - tb)
        lat_host.append(h * 1e6 - tb)
    res = {
        "missed": nlines - len(ks), "bad": bad, "overflows": st["overflows"],
        "max_used": st["max_used"], "reads": st["reads"], "policy": st["policy"],
        "cpu_pct": 100 * (c1 - c0) / (w1 - w0),
        "lat_stamp_us": stats(lat), "lat_python_us": stats(lat_host),
        "fit_unc_us": unc, "fit_ppm": ppm, "fit_points": npts, "probes": len(P1) + len(P2),
        "probe_rows": P1 + P2,   # (t0, t2, rx, tx): host us, board us
    }
    return res


@pytest.mark.parametrize("mode", ["line", "poll", "wait"])
def test_stream_1000_lines_per_second(port, mode):
    res = stream(port, mode, {"line": 100000, "poll": 200000, "wait": 300000}[mode])
    RESULTS["stream_" + mode] = res
    print(f"\nstream ({mode}): missed {res['missed']}, overflows {res['overflows']}, "
          f"max ring use {res['max_used']}, CPU {res['cpu_pct']:.1f} % of one core, "
          f"fit +-{res['fit_unc_us']:.0f} us, {res['fit_ppm']:.1f} ppm, {res['fit_points']} fit points")
    show("  latency, stamp (reader's, or now() after read) minus board stamp", res["lat_stamp_us"])
    if mode != "line":
        show("  latency, record in Python minus board stamp", res["lat_python_us"])
    assert res["missed"] == 0 and res["bad"] == 0 and res["overflows"] == 0
    # The stamp cannot precede the board's send by more than the fit's bound.
    assert res["lat_stamp_us"]["min"] > -res["fit_unc_us"] - 50


def test_write_cost_while_the_reader_runs(port):
    base = write_costs(port, 200, 1e-3)
    port.async_start()
    time.sleep(0.05)
    idle = write_costs(port, 200, 1e-3)
    # The same writes while the reader receives a stream.
    port.write(b"s %d %d\n" % (STREAM_HZ, 3 * STREAM_HZ))
    busy = write_costs(port, 200, 10e-3)
    st = port.async_stop()
    quiet(port)
    RESULTS["write_us"] = {"no_reader": stats(base), "reader_waiting": stats(idle),
                           "reader_receiving": stats(busy), "lines_during": st["lines"]}
    print()
    show("write(b'\\n'), no reader", RESULTS["write_us"]["no_reader"])
    show("write(b'\\n'), reader waiting", RESULTS["write_us"]["reader_waiting"])
    show("write(b'\\n'), reader receiving 1000 lines/s", RESULTS["write_us"]["reader_receiving"])
    # IOPort's writes beside its reader took 2 to 4 s (docs/serial.md).
    assert RESULTS["write_us"]["reader_receiving"]["max"] < 50e3
    assert RESULTS["write_us"]["reader_waiting"]["max"] < 50e3
