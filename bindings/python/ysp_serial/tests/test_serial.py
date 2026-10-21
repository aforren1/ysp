"""Tests for ysp.serial that need no port. The unit checks run before the
library is called, so a Port that never opened reaches them; the calls that
pass them then fail with Closed, which proves the value got through."""
import math
import os
import re
import time

import pytest

import ysp.serial as yser


def unopened():
    # __new__ without __init__: a zeroed handle, which the library treats as
    # closed.
    return yser.Port.__new__(yser.Port)


def test_version_matches_pyproject():
    path = os.path.join(os.path.dirname(__file__), "..", "pyproject.toml")
    if not os.path.exists(path):
        pytest.skip("pyproject.toml is not beside the tests")
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    assert m, "no version in pyproject.toml"
    assert yser.__version__ == m.group(1)


def test_now_is_seconds_on_one_clock():
    t0 = yser.now()
    time.sleep(0.05)
    t1 = yser.now()
    assert isinstance(t0, float)
    # 50 ms of sleep is 0.05 s, not 50 000 (us) or 5e7 (ns).
    assert 0.04 < t1 - t0 < 1.0
    assert not hasattr(yser, "now_us")


def test_constants_are_seconds():
    assert yser.TIMEOUT_INFINITE == math.inf
    assert yser.DEFAULT_WRITE_TIMEOUT == 1.0
    assert yser.DEFAULT_RT_RUNTIME == pytest.approx(0.001)
    assert yser.DEFAULT_RT_DEADLINE == pytest.approx(0.010)
    assert yser.DEFAULT_RT_PERIOD == pytest.approx(0.010)
    for old in ("DEFAULT_WRITE_TIMEOUT_MS", "DEFAULT_RT_RUNTIME_NS",
                "DEFAULT_RT_DEADLINE_NS", "DEFAULT_RT_PERIOD_NS"):
        assert not hasattr(yser, old)


def test_open_fails_without_a_device():
    with pytest.raises(yser.Error):
        yser.Port("/dev/ysp-nothing" if os.name != "nt" else "COM250")


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
        yser.Port("/dev/ysp-nothing" if os.name != "nt" else "COM250", **kw)


@pytest.mark.parametrize("kw", [
    {"write_timeout": 0.25}, {"write_timeout": math.inf},
    {"write_timeout": 1e-12},                      # rounds up, never to 0
    {"rt_runtime": 0.0005, "rt_deadline": 0.002, "rt_period": 0.002},
])
def test_open_accepts_good_times(kw):
    # Accepted arguments reach the open, which fails for the missing device.
    with pytest.raises(yser.Error):
        yser.Port("/dev/ysp-nothing" if os.name != "nt" else "COM250", **kw)


@pytest.mark.parametrize("timeout", [0, 0.0, 0.001, 1e-12, 2, math.inf, yser.TIMEOUT_INFINITE])
def test_read_accepts_seconds(timeout):
    with pytest.raises(yser.Closed):
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
    with pytest.raises(yser.Closed):
        unopened().read(4, timeout=0.5, all=True)
    with pytest.raises(TypeError):
        unopened().read(4, timeout_ms=500)


@pytest.mark.parametrize("call", ["pulse", "pulse_async"])
def test_pulse_width_in_seconds(call):
    p = unopened()
    with pytest.raises(yser.Closed):
        getattr(p, call)(0x55, 0x00, 0.002)
    with pytest.raises(yser.Closed):
        getattr(p, call)(0x55, 0x00, width=0)
    for bad, exc in ((-0.001, ValueError), (math.inf, ValueError),
                     (math.nan, ValueError), (1e10, OverflowError)):
        with pytest.raises(exc):
            getattr(p, call)(0x55, 0x00, bad)
    with pytest.raises(TypeError):
        getattr(p, call)(0x55, 0x00, usec=2000)
    with pytest.raises(ValueError):
        getattr(p, call)(256, 0x00, 0.002)


def test_min_hold_in_seconds():
    assert yser.DEFAULT_MIN_HOLD == 0.00025
    assert unopened().min_hold == 0.0
    # Checked before the open, so the bad value is what raises.
    for bad, exc in ((-0.001, ValueError), (math.nan, ValueError), (math.inf, ValueError)):
        with pytest.raises(exc):
            yser.Port("ysp-test-no-such-port", min_hold=bad)
    with pytest.raises(yser.Error):
        yser.Port("ysp-test-no-such-port", min_hold=0.001)


def test_send_break_in_seconds():
    p = unopened()
    with pytest.raises(yser.Closed):
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


def test_driver_of_an_unopened_port():
    # 0.6.1: a handle that never opened has no driver and no extra DTR request.
    p = unopened()
    assert p.driver == ""
    assert p.rts_via_dtr is False


def test_list_ports_has_driver():
    for info in yser.list_ports():
        assert isinstance(info["driver"], str)


# --- async reader, without a port ------------------------------------------

def test_async_on_a_never_opened_port_raises_closed():
    p = unopened()
    assert p.async_stats is None
    with pytest.raises(yser.Closed):
        p.async_start()
    with pytest.raises(yser.Closed):
        p.async_read()
    with pytest.raises(yser.Closed):
        p.async_wait(0)
    assert p.async_stats is None            # the failed start left nothing


def test_async_stop_is_idempotent():
    p = unopened()
    assert p.async_stop() is None
    assert p.async_stop() is None
    p.close()
    assert p.async_stop() is None


def test_async_on_a_closed_port_raises_closed():
    p = unopened()
    p.close()
    with pytest.raises(yser.Closed):
        p.async_start(terminator=b"\n")
    with pytest.raises(yser.Closed):
        p.async_wait(math.inf)


@pytest.mark.parametrize("kw, exc", [
    ({"capacity": 0}, ValueError),
    ({"capacity": 1 << 25}, ValueError),
    ({"max_len": 0}, ValueError),
    ({"max_len": 65536}, ValueError),
    ({"record_size": -1}, ValueError),
    ({"terminator": b"\r\n"}, TypeError),          # one byte only
    ({"terminator": 256}, ValueError),
    ({"terminator": "\n"}, TypeError),             # str is not a byte
    ({"terminator": b"\n", "record_size": 8}, ValueError),  # ambiguous
])
def test_async_start_refuses_bad_arguments_before_the_port(kw, exc):
    with pytest.raises(exc):
        unopened().async_start(**kw)


@pytest.mark.parametrize("kw", [
    {}, {"terminator": 10}, {"terminator": b"\r"}, {"terminator": None},
    {"record_size": 26}, {"terminator": None, "record_size": 26},
    {"capacity": 1, "max_len": 1},
])
def test_async_start_accepts_good_arguments(kw):
    with pytest.raises(yser.Closed):
        unopened().async_start(**kw)


@pytest.mark.parametrize("timeout, exc", [
    (-1, ValueError), (math.nan, ValueError), (None, TypeError), (1e10, OverflowError),
])
def test_async_wait_refuses_bad_timeouts(timeout, exc):
    with pytest.raises(exc):
        unopened().async_wait(timeout)


def test_async_read_refuses_a_negative_max():
    with pytest.raises(ValueError):
        unopened().async_read(max=-1)
