"""Tests for ysp.parallel that need no port. The unit checks run before the
library is called, so a Port that never opened reaches them; the calls that
pass them then fail with Error (port not open), which proves the value got
through."""
import math
import os
import re
import time

import pytest

import ysp.parallel as ypar


def unopened():
    # __new__ without __init__: a zeroed handle, which the library treats as
    # closed.
    return ypar.Port.__new__(ypar.Port)


def test_version_matches_pyproject():
    path = os.path.join(os.path.dirname(__file__), "..", "pyproject.toml")
    if not os.path.exists(path):
        pytest.skip("pyproject.toml is not beside the tests")
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    assert m, "no version in pyproject.toml"
    assert ypar.__version__ == m.group(1)


def test_now_is_seconds_on_one_clock():
    t0 = ypar.now()
    time.sleep(0.05)
    t1 = ypar.now()
    assert isinstance(t0, float)
    assert 0.04 < t1 - t0 < 1.0
    assert not hasattr(ypar, "now_us")


def test_constants_are_seconds():
    assert ypar.DEFAULT_RT_RUNTIME == pytest.approx(0.001)
    assert ypar.DEFAULT_RT_DEADLINE == pytest.approx(0.010)
    assert ypar.DEFAULT_RT_PERIOD == pytest.approx(0.010)
    for old in ("DEFAULT_RT_RUNTIME_NS", "DEFAULT_RT_DEADLINE_NS", "DEFAULT_RT_PERIOD_NS"):
        assert not hasattr(ypar, old)


@pytest.mark.parametrize("kw, exc", [
    ({"rt_runtime": -0.001}, ValueError),
    ({"rt_deadline": math.nan}, ValueError),
    ({"rt_period": math.inf}, ValueError),
    ({"rt_period": 1e10}, OverflowError),
    ({"rt_runtime_ns": 500000}, TypeError),        # the old keyword is gone
])
def test_open_refuses_bad_times(kw, exc):
    with pytest.raises(exc):
        ypar.Port(**kw)


@pytest.mark.parametrize("call", ["pulse", "pulse_async"])
def test_pulse_width_in_seconds(call):
    p = unopened()
    with pytest.raises(ypar.Error):
        getattr(p, call)(0x55, 0.002)
    with pytest.raises(ypar.Error):
        getattr(p, call)(0x55, width=0)
    for bad, exc in ((-0.001, ValueError), (math.inf, ValueError),
                     (math.nan, ValueError), (1e10, OverflowError),
                     (None, TypeError)):
        with pytest.raises(exc):
            getattr(p, call)(0x55, bad)
    with pytest.raises(TypeError):
        getattr(p, call)(0x55, usec=2000)
    with pytest.raises(ValueError):
        getattr(p, call)(256, 0.002)


def test_sched_in_seconds():
    assert unopened().sched == {"runtime": 0.0, "deadline": 0.0, "period": 0.0}


def test_open_without_a_port():
    # No parallel port on a CI runner or on most machines. With one present
    # the open succeeds, and the effective reservation reads back in seconds.
    try:
        port = ypar.Port(rt_runtime=0.0005, rt_deadline=0.002, rt_period=0.002)
    except ypar.Error:
        return
    with port:
        s = port.sched
        assert 0 < s["runtime"] <= s["deadline"] <= s["period"] < 1
