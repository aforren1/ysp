"""Tests for ysp.serial that need no port. The unit checks run before the
library is called, so a Port that never opened reaches them; the calls that
pass them then fail with Closed, which proves the value got through."""
import math
import os
import re
import time

import pytest

import ysp.serial as ps


def unopened():
    # __new__ without __init__: a zeroed handle, which the library treats as
    # closed.
    return ps.Port.__new__(ps.Port)


def test_version_matches_pyproject():
    path = os.path.join(os.path.dirname(__file__), "..", "pyproject.toml")
    if not os.path.exists(path):
        pytest.skip("pyproject.toml is not beside the tests")
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    assert m, "no version in pyproject.toml"
    assert ps.__version__ == m.group(1)


def test_now_is_seconds_on_one_clock():
    t0 = ps.now()
    time.sleep(0.05)
    t1 = ps.now()
    assert isinstance(t0, float)
    # 50 ms of sleep is 0.05 s, not 50 000 (us) or 5e7 (ns).
    assert 0.04 < t1 - t0 < 1.0
    assert not hasattr(ps, "now_us")


def test_constants_are_seconds():
    assert ps.TIMEOUT_INFINITE == math.inf
    assert ps.DEFAULT_WRITE_TIMEOUT == 1.0
    assert ps.DEFAULT_RT_RUNTIME == pytest.approx(0.001)
    assert ps.DEFAULT_RT_DEADLINE == pytest.approx(0.010)
    assert ps.DEFAULT_RT_PERIOD == pytest.approx(0.010)
    for old in ("DEFAULT_WRITE_TIMEOUT_MS", "DEFAULT_RT_RUNTIME_NS",
                "DEFAULT_RT_DEADLINE_NS", "DEFAULT_RT_PERIOD_NS"):
        assert not hasattr(ps, old)


def test_open_fails_without_a_device():
    with pytest.raises(ps.Error):
        ps.Port("/dev/ysp-nothing" if os.name != "nt" else "COM250")


@pytest.mark.parametrize("kw, exc", [
    ({"write_timeout": -0.5}, ValueError),
    ({"write_timeout": 0}, ValueError),            # a write cannot poll
    ({"write_timeout": math.nan}, ValueError),
    ({"write_timeout": 1e10}, OverflowError),      # past int64 ns
    ({"write_timeout": "1"}, TypeError),
    ({"rt_runtime": -0.001}, ValueError),
    ({"rt_deadline": math.inf}, ValueError),
    ({"write_timeout_ms": 1000}, TypeError),       # the old keyword is gone
    ({"rt_runtime_ns": 1000000}, TypeError),
])
def test_open_refuses_bad_times_before_the_device(kw, exc):
    # The device does not exist; the argument error must come first.
    with pytest.raises(exc):
        ps.Port("/dev/ysp-nothing" if os.name != "nt" else "COM250", **kw)


@pytest.mark.parametrize("kw", [
    {"write_timeout": 0.25}, {"write_timeout": math.inf},
    {"write_timeout": 1e-12},                      # rounds up, never to 0
    {"rt_runtime": 0.0005, "rt_deadline": 0.002, "rt_period": 0.002},
])
def test_open_accepts_good_times(kw):
    # Accepted arguments reach the open, which fails for the missing device.
    with pytest.raises(ps.Error):
        ps.Port("/dev/ysp-nothing" if os.name != "nt" else "COM250", **kw)


@pytest.mark.parametrize("timeout", [0, 0.0, 0.001, 1e-12, 2, math.inf, ps.TIMEOUT_INFINITE])
def test_read_accepts_seconds(timeout):
    with pytest.raises(ps.Closed):
        unopened().read(1, timeout)


@pytest.mark.parametrize("timeout, exc", [
    (-1, ValueError), (-1e-9, ValueError), (-math.inf, ValueError),
    (math.nan, ValueError), (1e10, OverflowError), (None, TypeError),
    (4294967295 * 1e3, OverflowError),             # the old infinite, in s
])
def test_read_refuses(timeout, exc):
    with pytest.raises(exc):
        unopened().read(1, timeout)


def test_read_keyword_is_timeout():
    with pytest.raises(ps.Closed):
        unopened().read(4, timeout=0.5, all=True)
    with pytest.raises(TypeError):
        unopened().read(4, timeout_ms=500)


@pytest.mark.parametrize("call", ["pulse", "pulse_async"])
def test_pulse_width_in_seconds(call):
    p = unopened()
    with pytest.raises(ps.Closed):
        getattr(p, call)(0x55, 0x00, 0.002)
    with pytest.raises(ps.Closed):
        getattr(p, call)(0x55, 0x00, width=0)
    for bad, exc in ((-0.001, ValueError), (math.inf, ValueError),
                     (math.nan, ValueError), (1e10, OverflowError)):
        with pytest.raises(exc):
            getattr(p, call)(0x55, 0x00, bad)
    with pytest.raises(TypeError):
        getattr(p, call)(0x55, 0x00, usec=2000)
    with pytest.raises(ValueError):
        getattr(p, call)(256, 0x00, 0.002)


def test_send_break_in_seconds():
    p = unopened()
    with pytest.raises(ps.Closed):
        p.send_break(0.01)
    with pytest.raises(ValueError):
        p.send_break(-0.01)
    with pytest.raises(TypeError):
        p.send_break(ms=10)


def test_properties_in_seconds():
    p = unopened()
    assert p.write_timeout == 0.0
    assert p.sched == {"runtime": 0.0, "deadline": 0.0, "period": 0.0}
    assert not hasattr(p, "write_timeout_ms")
