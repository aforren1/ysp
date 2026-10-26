"""Tests for ysp.device on simulated boards (ysp.device.Sim): the board of
tests/adapt/device_test.c on the real clock, behind the header's transport
hook. No hardware. tests/adapt/device_test.c checks the header itself."""
import math
import os
import re
import threading
import time

import pytest

import ysp.device as yd

# Events on the real clock: the simulated USB delay is about 0.2 ms, and a
# loaded machine adds the reader thread's wake. The checks of a stamp's
# error use this bound; the fit's own numbers are the header's test's.
STAMP_TOL = 0.005


def collect(dev, n, timeout=5.0):
    out = []
    end = time.monotonic() + timeout
    while len(out) < n and time.monotonic() < end:
        out += dev.wait(0.2)
    return out


def wait_for(pred, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.005)
    return pred()


@pytest.fixture
def line():
    sim = yd.Sim("line")
    dev = yd.Device("photo", "line", "serial:16C0:0483::", sim=sim, no_elevate=True)
    assert dev.wait_running(3.0), dev.error
    yield sim, dev
    dev.close()


def test_version_matches_pyproject():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    text = open(os.path.join(here, "pyproject.toml")).read()
    m = re.search(r'^version\s*=\s*"([^"]+)"', text, re.M)
    assert m and m.group(1) == yd.__version__


def test_constants():
    assert (yd.XID, yd.LINE, yd.PHOTO, yd.TRIGGERBOX, yd.BIOSEMI, yd.MMBTS, yd.LINES, yd.PARALLEL) == tuple(range(1, 9))
    assert (yd.CLOSED, yd.OPENING, yd.RUNNING, yd.LOST, yd.FAILED) == (0, 1, 2, 3, 4)
    assert (yd.KIND_KEYBOARD, yd.KIND_BOX, yd.KIND_SYNC, yd.KIND_USER) == (0, 5, 7, 8)
    assert (yd.NONE, yd.PRESS, yd.RELEASE, yd.SAMPLE, yd.PROXIMITY) == (0, 1, 2, 3, 4)
    assert (yd.REPEAT, yd.SYNTHETIC, yd.ERASER, yd.UNLISTED, yd.DEVTICKS) == (1, 2, 4, 8, 16)
    assert (yd.TIER_UNKNOWN, yd.TIER_1, yd.TIER_2, yd.TIER_3, yd.TIER_SIM) == (0, 1, 2, 3, 4)
    assert yd.ERR_STATE == -3 and yd.ERR_TIMEOUT == -5
    assert yd.OUT_PULSE == 0x100 and yd.OUT_RULE == 0x200
    for f in range(yd.XID, yd.PARALLEL + 1):
        assert yd.family(yd.family_name(f)) == f
    assert yd.family("triggerbox") == yd.TRIGGERBOX
    with pytest.raises(ValueError):
        yd.family("nope")
    assert yd.state_name(yd.RUNNING) == "running"
    assert yd.strerror(yd.ERR_STATE)
    assert yd.key_match("serial:16C0:0483::", "serial:16c0:0483:ABC:1-1")
    assert not yd.key_match("serial:0403:6001::", "serial:16C0:0483:ABC:1-1")
    assert isinstance(yd.list_ports(), list)
    assert abs(yd.now() - yd.now()) < 1.0


def test_event_fields():
    assert yd.Event.n_fields == 16
    names = ["t", "t_ns", "device", "control", "code", "mods", "kind", "type", "flags", "stamp", "unc",
             "x", "y", "value", "aux", "ticks"]
    e = yd.Event(tuple(range(16)))
    assert [getattr(e, n) for n in names] == list(range(16))


def test_arguments_are_seconds():
    sim = yd.Sim("triggerbox")
    for bad in (-1.0, math.nan):
        with pytest.raises(ValueError):
            yd.Device("t", "triggerbox", sim.key, sim=sim, pulse=bad)
    with pytest.raises(ValueError):
        yd.Device("t", "triggerbox", sim.key, sim=sim, min_hold=0)
    with pytest.raises(ValueError):
        yd.Device("t", "triggerbox", sim.key, sim=sim, probe=0)
    with pytest.raises(ValueError):
        yd.Device("t", "triggerbox", sim.key, sim=sim, queue=0)
    with pytest.raises(ValueError):
        yd.Device("t", "nofamily", sim.key, sim=sim)
    with pytest.raises(ValueError):
        yd.Device("t", "line", sim.key, sim=sim)    # a Sim of another family
    with pytest.raises(yd.Error):
        yd.Device("t", "line", "COM5")             # not a key
    with yd.Device("t", "triggerbox", sim.key, sim=sim) as d:
        assert d.wait_running(2.0)
        with pytest.raises(ValueError):
            d.wait(-0.1)
        with pytest.raises(ValueError):
            d.pulse(1, -0.002)
        with pytest.raises(ValueError):
            d.wait_running(math.nan)
        assert d.wait(0) == []


def test_events_on_the_rt_clock(line):
    sim, dev = line
    assert dev.state == yd.RUNNING and dev.state_name == "running"
    assert dev.family == yd.LINE and dev.family_name == "line" and dev.kind == yd.KIND_BOX
    assert dev.role == "photo" and dev.key == "serial:16C0:0483::"
    assert dev.device == yd.roles()["photo"]
    st = dev.stats()
    assert st["ident"].startswith("ysp-line 1") and st["found"] == "sim line"
    t0 = yd.now() + 0.2
    truth = []
    for i in range(20):
        sim.press(t0 + i * 0.05, 0.02, channel=3)
        truth += [t0 + i * 0.05, t0 + i * 0.05 + 0.02]
    evs = collect(dev, 40)
    assert len(evs) == 40
    for i, (e, t) in enumerate(zip(evs, truth)):
        assert isinstance(e, yd.Event)
        assert e.type == (yd.PRESS if i % 2 == 0 else yd.RELEASE)
        assert e.control == 3 and e.kind == yd.KIND_BOX and e.device == dev.device
        assert e.t_ns == int(e.t_ns) and abs(e.t - e.t_ns / 1e9) < 1e-6
        assert abs(e.t - t) < STAMP_TOL, (i, e.t - t)
        assert e.flags & yd.DEVTICKS
        assert e.unc >= 0
    assert all(b.t >= a.t for a, b in zip(evs, evs[1:]))
    # the reader publishes its counters after the step that called the sink
    assert wait_for(lambda: dev.stats()["events"] == 40)
    st = dev.stats()
    assert st["queue_events"] == 40 and st["queue_dropped"] == 0
    assert st["probes"] > 0 and st["answers"] > 0 and st["fit"]["ready"]
    assert dev.source()["tier"] == yd.TIER_UNKNOWN and dev.source()["device"] == dev.device


def test_poll_max_clear_and_pending(line):
    sim, dev = line
    t0 = yd.now() + 0.05
    for i in range(5):
        sim.press(t0 + i * 0.01, 0.004)
    assert wait_for(lambda: dev.pending == 10)
    assert len(dev.poll(max=3)) == 3
    assert dev.pending == 7
    assert dev.clear() == 7
    assert dev.poll() == [] and dev.pending == 0
    with pytest.raises(ValueError):
        dev.poll(max=-1)


def test_wait_times_out_and_releases_the_gil(line):
    sim, dev = line
    count = [0]
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            count[0] += 1

    th = threading.Thread(target=spin)
    th.start()
    try:
        t = time.monotonic()
        assert dev.wait(0.3) == []
        took = time.monotonic() - t
    finally:
        stop.set()
        th.join()
    assert 0.25 < took < 2.0
    # the spinning thread ran while wait() blocked
    assert count[0] > 1000


def test_full_queue_drops_and_counts():
    sim = yd.Sim("line")
    with yd.Device("q", "line", sim.key, sim=sim, queue=4, no_elevate=True) as dev:
        assert dev.wait_running(3.0)
        t0 = yd.now() + 0.05
        for i in range(5):
            sim.press(t0 + i * 0.01, 0.004)
        assert wait_for(lambda: dev.stats()["queue_events"] == 10)
        st = dev.stats()
        assert st["queue_dropped"] == 6 and st["queue_capacity"] == 4 and st["queue_max_used"] == 4
        assert len(dev.poll()) == 4


def test_key_that_matches_nothing_stays_opening():
    sim = yd.Sim("line")
    with yd.Device("k", "line", "serial:16C0:0483:OTHER:", sim=sim) as dev:
        t = time.monotonic()
        assert dev.wait_running(0.3) is False
        assert time.monotonic() - t >= 0.25
        assert dev.state == yd.OPENING


def test_wrong_device_fails():
    sim = yd.Sim("line", identify=False)
    with yd.Device("w", "line", sim.key, sim=sim) as dev:
        with pytest.raises(yd.Error) as ei:
            dev.wait_running(5.0)
        assert "line device" in str(ei.value.args[0])
        assert ei.value.args[1] == yd.ERR_STATE
        assert dev.state == yd.FAILED


def test_unplug_lost_and_back():
    sim = yd.Sim("line")
    with yd.Device("u", "line", sim.key, sim=sim, retry=0.1, no_elevate=True) as dev:
        assert dev.wait_running(3.0)
        now = yd.now()
        sim.unplug(now + 0.1, now + 0.4)
        assert wait_for(lambda: dev.state == yd.LOST, 3.0)
        assert wait_for(lambda: dev.state == yd.RUNNING, 5.0)
        assert dev.stats()["opens"] == 2 and sim.opens == 2


def test_xid_and_photo():
    xs = yd.Sim("xid")
    ps = yd.Sim("photo")
    with yd.Device("x", "xid", xs.key, sim=xs) as x, yd.Device("p", "photo", ps.key, sim=ps) as p:
        assert x.wait_running(5.0), x.error
        assert p.wait_running(3.0), p.error
        assert x.stats()["ident"].startswith("xid product")
        t0 = yd.now() + 0.1
        xs.press(t0, 0.05, channel=2)
        ps.press(t0, 0.05)
        ex = collect(x, 2)
        ep = collect(p, 2)
        assert [e.type for e in ex] == [yd.PRESS, yd.RELEASE] and ex[0].kind == yd.KIND_BOX
        assert ex[0].control == 2
        assert [e.type for e in ep] == [yd.PRESS, yd.RELEASE] and ep[0].kind == yd.KIND_SYNC
        # XID ticks are 1 ms: an event lies anywhere in its tick
        assert abs(ex[0].t - t0) < STAMP_TOL + 0.001
        assert abs(ep[0].t - t0) < STAMP_TOL


def test_triggerbox_outputs():
    sim = yd.Sim("triggerbox")
    with yd.Device("trig", "triggerbox", sim.key, sim=sim) as dev:
        assert dev.wait_running(2.0)
        t = yd.now()
        dev.pulse(5, 0.002)
        o = dev.last_out()
        assert o["code"] == 5 and o["flags"] & yd.OUT_PULSE and o["result"] == yd.OK
        assert t <= o["t_before"] <= o["t_after"] and abs(o["width"] - 0.002) < 1e-9
        assert wait_for(lambda: len(sim.writes()) >= 3)
        w = sim.writes()
        # the 0 at the open, the code, the host-timed 0
        assert [d for _, d in w[:3]] == [b"\x00", b"\x05", b"\x00"]
        assert 0.0019 < w[2][0] - w[1][0] < 0.02
        dev.set(7)
        dev.mark(9, "stimulus on")
        with pytest.raises(yd.Error) as ei:
            dev.set(256)               # a TriggerBox code is a byte
        assert ei.value.args[1] == yd.ERR_ARG
        assert dev.stats()["outs"] >= 4
    # close writes 0 for the held code, then the close byte 0xFF
    w = [d for _, d in sim.writes()]
    assert w[-1] == b"\xff"
    with pytest.raises(yd.Closed):
        dev.pulse(1, 0.002)
    assert dev.state == yd.CLOSED and dev.stats()["state_name"] == "closed"
    dev.close()   # twice


def test_line_board_pulse_is_device_timed(line):
    sim, dev = line
    dev.pulse(7, 0.002)
    assert dev.last_out()["flags"] & yd.OUT_DEVICE_TIMED
    assert wait_for(lambda: any(d.startswith(b"p 7 2000") for _, d in sim.writes()))


def test_lines_and_fixed_width_families():
    ls = yd.Sim("lines")
    with yd.Device("dtr", "lines", ls.key, sim=ls) as dev:
        assert dev.wait_running(2.0)
        dev.set(1)
        dev.set(3)
        assert wait_for(lambda: len(ls.lines()) >= 3)
        assert [(c, ch) for _, c, ch in ls.lines()][-2:] == [(1, 1), (3, 2)]
    bs = yd.Sim("biosemi")
    with yd.Device("bio", "biosemi", bs.key, sim=bs) as dev:
        assert dev.wait_running(2.0)
        with pytest.raises(yd.Error) as ei:
            dev.pulse(1, 0.002)        # BioSemi ends every code after 8 ms
        assert ei.value.args[1] == yd.ERR_ARG
        dev.pulse(1, 0.008)


def test_output_before_running_and_input_only():
    sim = yd.Sim("triggerbox")
    with yd.Device("tb", "triggerbox", "serial:0403:6001:NOTHERE:", sim=sim) as dev:
        with pytest.raises(yd.Error) as ei:
            dev.pulse(1, 0.002)
        assert ei.value.args[1] == yd.ERR_STATE
    ps = yd.Sim("photo")
    with yd.Device("ph", "photo", ps.key, sim=ps) as dev:
        assert dev.wait_running(3.0)
        with pytest.raises(yd.Error) as ei:
            dev.set(1)
        assert ei.value.args[1] == yd.ERR_FAMILY


def test_roles_keep_their_index():
    a = yd.Sim("triggerbox")
    d1 = yd.Device("role_a", "triggerbox", a.key, sim=a)
    d2 = yd.Device("role_b", "triggerbox", a.key, sim=a)
    assert d1.device != d2.device
    ia = d1.device
    with pytest.raises(yd.Error):
        yd.Device("role_a", "triggerbox", a.key, sim=a)    # a role has one open instance
    d1.close()
    d3 = yd.Device("role_a", "triggerbox", a.key, sim=a)
    assert d3.device == ia
    d3.close()
    d2.close()
    with yd.Device("explicit", "triggerbox", a.key, sim=a, device=200) as d4:
        assert d4.device == 200


def test_response_rule_writes_at_the_press():
    os_ = yd.Sim("triggerbox")
    out = yd.Device("eeg", "triggerbox", os_.key, sim=os_, pulse=0.005)
    assert out.wait_running(2.0)
    rule = yd.Rule(out, [{"kind": yd.KIND_BOX, "control": 1, "code": 101},
                         {"kind": yd.KIND_BOX, "control": 2, "code": 102}])
    bs = yd.Sim("line")
    box = yd.Device("resp", "line", bs.key, sim=bs, rule=rule, no_elevate=True)
    assert box.wait_running(3.0)
    now = yd.now()
    rule.arm(now, math.inf)
    bs.press(now + 0.1, 0.03, channel=2)
    ev = collect(box, 2)
    assert len(ev) == 2
    assert wait_for(lambda: rule.info()["fired"] == 1)
    info = rule.info()
    assert info["code"] == 102 and info["choice"] == 1 and info["result"] == yd.OK
    assert abs(info["t_event"] - ev[0].t) < 1e-6
    assert info["t_before"] >= info["t_event"]
    f = rule.fires()
    assert len(f) == 1 and f[0]["code"] == 102 and f[0]["t_ns"] == ev[0].t_ns
    assert wait_for(lambda: any(d == bytes([102]) for _, d in os_.writes()))
    assert info["seq"] > 0 and out.stats()["outs"] >= 2   # the code and its trailing 0
    # one fire per arm by default
    bs.press(yd.now() + 0.05, 0.02, channel=1)
    collect(box, 2)
    assert rule.info()["fired"] == 1
    # the close order: input, rule, output
    with pytest.raises(yd.Error):
        out.close()
    with pytest.raises(yd.Error):
        rule.close()
    box.close()
    rule.close()
    with pytest.raises(yd.Closed):
        rule.arm(0)
    out.close()
    with pytest.raises(yd.Closed):
        yd.Rule(out, [{"code": 1}])


def test_rule_arguments():
    sim = yd.Sim("triggerbox")
    with yd.Device("r_out", "triggerbox", sim.key, sim=sim) as out:
        with pytest.raises(ValueError):
            yd.Rule(out, [])
        with pytest.raises(ValueError):
            yd.Rule(out, [{"kind": 5}])           # no code
        with pytest.raises(ValueError):
            yd.Rule(out, [{"code": 1, "button": 2}])
        with pytest.raises(yd.Error):
            yd.Rule(out, [{"code": 300}])         # above the box's byte
        with pytest.raises(ValueError):
            yd.Rule(out, [{"code": 1}], max_fires=0)
        r = yd.Rule(out, [{"code": 1}], max_fires=None, width=0.002)
        with pytest.raises(ValueError):
            r.arm(-1.0)
        r.arm(math.inf)                            # a closed window
        r.window(1.0, 2.0)
        r.disarm()
        assert r.info()["choice"] is None and r.fires() == []
        r.close()
        r.close()


def test_close_wakes_a_waiter(line):
    sim, dev = line
    got = []

    def waiter():
        got.append(dev.wait(math.inf))

    th = threading.Thread(target=waiter)
    th.start()
    time.sleep(0.1)
    dev.close()
    th.join(5.0)
    assert not th.is_alive() and got == [[]]
    assert not dev.is_open


def test_garbage_collection_closes_in_order():
    os_ = yd.Sim("triggerbox")
    out = yd.Device("gc_out", "triggerbox", os_.key, sim=os_)
    assert out.wait_running(2.0)
    rule = yd.Rule(out, [{"code": 1}])
    bs = yd.Sim("line")
    box = yd.Device("gc_box", "line", bs.key, sim=bs, rule=rule)
    del out, rule
    del box     # box, then the rule it held, then the output the rule held
    assert os_.writes()[-1][1] == b"\xff"
