"""Tests for ysp.rt: the binding's surface. tests/adapt/rt_test.c checks the
header itself; these check the conversions, the GIL, the refusals and that
the clock is the one the other ysp modules read."""
import math
import os
import random
import re
import signal
import subprocess
import sys
import threading
import time

import pytest

import ysp.rt as rt


def test_version_matches_pyproject():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    text = open(os.path.join(here, "pyproject.toml")).read()
    m = re.search(r'^version\s*=\s*"([^"]+)"', text, re.M)
    assert m and m.group(1) == rt.__version__


def test_now_is_seconds_and_ns_agree():
    a = rt.now_ns()
    s = rt.now()
    b = rt.now_ns()
    assert isinstance(a, int) and isinstance(s, float)
    assert a <= round(s * 1e9) + 1 and round(s * 1e9) <= b + 1
    assert b >= a > 0
    assert abs(rt.real_now() - rt.now()) < 1.0


def test_constants():
    assert rt.DEFAULT_SPIN in (0.0012, 0.0002)
    assert rt.VIRTUAL_T0 == 1000.0
    assert rt.FIT_POINTS == 1024
    assert (rt.FIT_POINT, rt.FIT_REFIT, rt.FIT_RESTART, rt.FIT_REJECTED, rt.FIT_PENDING) == (1, 2, 4, 8, 16)


@pytest.mark.parametrize("name", ["ysp.serial", "ysp.parallel"])
def test_same_clock_as_other_modules(name):
    """Each ysp extension compiles its own copy of ysp/rt.h. They read one
    counter: a read of the other module's now() lies between two of ours."""
    other = pytest.importorskip(name)
    for _ in range(2000):
        a = rt.now_ns()
        b = round(other.now() * 1e9)
        c = rt.now_ns()
        assert a - 1 <= b <= c + 1   # 1 ns: the float's rounding


def test_clock_info_and_describe():
    ci = rt.clock_info()
    assert 0 < ci["resolution"] < 1e-3 and isinstance(ci["hires_timer"], bool)
    line = rt.describe()
    assert line.startswith("ysp_rt: policy=NONE") and "clock_res=" in line


def test_sleep_until_lateness():
    t = rt.now() + 0.003
    late = rt.sleep_until(t)
    after = rt.now()
    assert late >= 0 and after >= t
    late = rt.sleep_until(rt.now() + 0.002, spin=0)
    assert late >= 0


def test_sleep_until_past_deadline_returns_at_once():
    t = rt.now() - 0.5
    late = rt.sleep_until(max(t, 0.0))
    assert late >= 0.5


def test_sleep_and_spin():
    t0 = rt.now()
    late = rt.sleep(0.002)
    assert rt.now() - t0 >= 0.002 and late >= 0
    t = rt.now() + 0.001
    late = rt.spin_until(t)
    assert rt.now() >= t and late >= 0


@pytest.mark.parametrize("bad", [-1.0, float("nan"), float("inf")])
def test_times_refused(bad):
    with pytest.raises(ValueError):
        rt.sleep_until(bad)
    with pytest.raises(ValueError):
        rt.sleep(bad)
    with pytest.raises(ValueError):
        rt.spin_until(bad)
    with pytest.raises(ValueError):
        rt.sleep_until(rt.now(), spin=bad)


def test_overflow_refused():
    with pytest.raises(OverflowError):
        rt.sleep_until(1e10)


def test_long_wait_ends_on_a_signal():
    """A long wait runs the signal handlers every 100 ms, so Ctrl+C ends it."""
    class Stop(Exception):
        pass

    def handler(signum, frame):
        raise Stop()

    old = signal.signal(signal.SIGINT, handler)
    try:
        threading.Timer(0.2, signal.raise_signal, (signal.SIGINT,)).start()
        t0 = rt.now()
        with pytest.raises(Stop):
            rt.sleep(30.0)
        assert rt.now() - t0 < 25.0
    finally:
        signal.signal(signal.SIGINT, old)


def test_thread_cleanup_in_a_thread():
    out = []

    def body():
        out.append(rt.sleep(0.001))
        rt.thread_cleanup()
        out.append(rt.sleep(0.001))   # a new timer on first use again

    th = threading.Thread(target=body)
    th.start()
    th.join()
    assert len(out) == 2 and min(out) >= 0


GIL_SCRIPT = r"""
import threading, time
import ysp.rt as rt
t0 = rt.virtual_begin()
done = []
th = threading.Thread(target=lambda: done.append(rt.sleep_until(t0 + 1.0)))
th.start()
time.sleep(0.1)            # needs the GIL while the thread waits
rt.sleep_until(t0 + 1.0)   # the driver moves the clock; the thread wakes
th.join()
assert done == [0.0] or done[0] >= 0, done
print("ok", done)
"""


def test_wait_releases_the_gil():
    """On the virtual clock a second thread's wait blocks until the driver
    moves the clock. If that wait held the GIL, the driver could not run and
    the script would hang until the timeout."""
    r = subprocess.run([sys.executable, "-c", GIL_SCRIPT], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and r.stdout.startswith("ok"), r.stderr


def test_virtual_clock():
    t0 = rt.virtual_begin()
    try:
        assert rt.is_virtual() and t0 == rt.VIRTUAL_T0 == rt.now()
        assert rt.now_ns() == 1000 * 10**9
        assert rt.now() == t0           # stands still between waits
        r0 = rt.real_now()
        assert rt.sleep_until(t0 + 10.0) == 0.0
        assert rt.now() == t0 + 10.0     # the wait jumped it
        assert rt.real_now() - r0 < 5.0  # 10 s virtual in far less real time
        assert rt.sleep(0.25) == 0.0 and rt.now() == t0 + 10.25
        with pytest.raises(rt.Error):
            rt.virtual_begin()
        try:
            import ysp.serial as ys   # its own copy of ysp/rt.h: still real
        except ImportError:
            ys = None
        if ys is not None:
            assert abs(ys.now() - rt.real_now()) < 1.0
    finally:
        rt.virtual_end()
    assert not rt.is_virtual()
    assert rt.virtual_begin(5.0) == 5.0
    rt.virtual_end()


def test_correlate_float_clock():
    c = rt.correlate(time.perf_counter)
    assert isinstance(c, rt.Corr) and isinstance(c.other, float)
    assert c.tries == 16 and 0 < c.width < 0.01
    assert c.t_ns == round(c.t * 1e9) or abs(c.t_ns - c.t * 1e9) < 2


def test_correlate_own_clock_is_inside_the_bracket():
    res_ns = round(rt.clock_info()["resolution"] * 1e9)
    c = rt.correlate(rt.now_ns, tries=8)
    assert isinstance(c.other, int) and c.tries == 8
    width_ns = round(c.width * 1e9)
    assert abs(c.other - c.t_ns) <= width_ns // 2 + res_ns


def test_correlate_edge_mode():
    """A 1 ms clock derived from ours changes exactly at a whole ms, so the
    edge's bracket holds other * 1e6 ns."""
    res_ns = round(rt.clock_info()["resolution"] * 1e9)
    c = rt.correlate(lambda: rt.now_ns() // 1000000, edge=True, tries=2, timeout=5.0)
    assert c.tries == 2
    width_ns = round(c.width * 1e9)
    assert abs(c.other * 1000000 - c.t_ns) <= width_ns // 2 + res_ns + 1


def test_correlate_errors():
    with pytest.raises(rt.Error):
        rt.correlate(lambda: 7, edge=True, timeout=0.01)

    def boom():
        raise KeyError("x")

    with pytest.raises(KeyError):
        rt.correlate(boom)
    with pytest.raises(KeyError):
        rt.correlate(boom, edge=True)
    with pytest.raises(TypeError):
        rt.correlate(lambda: "a")
    flip = iter([1, 2.0] * 20)
    with pytest.raises(TypeError):
        rt.correlate(lambda: next(flip))
    with pytest.raises(OverflowError):
        rt.correlate(lambda: -1)
    with pytest.raises(TypeError):
        rt.correlate(42)


def synthetic_device(fit, *, seconds, rate_ppm, offset_ticks, wrap_bits=0, seed=1, start=100.0, period=0.01):
    """A microsecond counter whose reports arrive 1 ms (USB frame) plus an
    exponential tail late. Returns the true map: host seconds of a tick."""
    rng = random.Random(seed)
    k = 1e-6 * (1 + rate_ppm * 1e-6)   # true seconds per tick
    mod = 1 << wrap_bits if wrap_bits else 0
    t = start
    while t < start + seconds:
        ticks = int(round((t - start) / k)) + offset_ticks
        host = t + 0.001 * rng.random() + rng.expovariate(1 / 0.0001)
        fit.add(ticks % mod if mod else ticks, host)
        t += period
    return lambda ticks: start + (ticks - offset_ticks) * k


def test_fit_late_tracks_a_drifting_clock():
    f = rt.Fit("late", tick=1e-6)
    with pytest.raises(rt.Error):
        f.map(0)
    truth = synthetic_device(f, seconds=30, rate_ppm=100, offset_ticks=12345678)
    info = f.info()
    assert info["ready"] and info["slope"] and info["epoch"] == 0
    assert abs(info["ppm"] - 100) < 5
    assert 25 < info["span"] < 31 and info["points"] > 140
    last = info["ticks0"]
    err = f.map(last) - truth(last)
    assert -0.0002 < err < 0.0015       # the lower envelope: the minimum delay
    assert isinstance(f.map_ns(last), int)
    assert abs(f.map_ns(last) - round(f.map(last) * 1e9)) <= 1
    many = f.map([last - 1000, last])
    assert len(many) == 2 and abs(many[1] - many[0] - 1000e-6) < 1e-7


def test_fit_wraps_a_32_bit_counter():
    f = rt.Fit("late", tick=1e-6, bits=32)
    start = (1 << 32) - 5_000_000      # wraps 5 s in
    truth = synthetic_device(f, seconds=10, rate_ppm=-80, offset_ticks=start, wrap_bits=32)
    unwrapped = f.unwrap(1000)
    assert unwrapped == (1 << 32) + 1000
    assert abs(f.map(1000) - truth(unwrapped)) < 0.0015


def test_fit_bracket_refuses_wide_pairs():
    f = rt.Fit("bracket", tick=1e-3, max_width=0.002)
    assert f.add(5, 10.0, 0.0005) & rt.FIT_POINT
    assert f.add(6, 10.001, 0.003) == rt.FIT_REJECTED
    assert f.info()["rejected"] == 1


def test_fit_restart_after_a_reset():
    f = rt.Fit("late", tick=1e-3, restart=0.05, restart_hold=1.0)
    flags = 0
    for i in range(1000):           # 10 s at 100 pairs a second, then a reset
        ticks = i * 10 if i < 500 else (i - 500) * 10
        flags |= f.add(ticks, 50.0 + i * 0.01)
    assert flags & rt.FIT_RESTART and f.info()["epoch"] == 1


def test_fit_numpy_like_ints_and_refusals():
    class Idx:
        def __index__(self):
            return 7

    f = rt.Fit("unbiased", tick=1e-3)
    f.add(Idx(), 1.0)
    assert isinstance(f.map(Idx()), float)
    with pytest.raises(TypeError):
        f.add(1.5, 1.0)
    with pytest.raises(OverflowError):
        f.add(-1, 1.0)
    with pytest.raises(ValueError):
        f.add(1, -1.0)
    with pytest.raises(ValueError):
        rt.Fit("fast", tick=1e-3)
    with pytest.raises(ValueError):
        rt.Fit("late", tick=0.0)
    with pytest.raises(ValueError):
        rt.Fit("late", tick=1e-3, bits=65)


def test_user_dir():
    d = rt.user_dir("config")
    assert d is None or d.endswith("ysp")
    r = rt.user_dir("cache", "rig/x")
    assert r is None or r.replace("\\", "/").endswith("ysp/rig/x")
    assert rt.user_dir("config", "..") is None
    with pytest.raises(ValueError):
        rt.user_dir("temp")
