"""Tests for ysp.response: the binding's surface and units against
ysp/response.h's rules (tests/adapt/response_test.c checks the header
itself, with 21,119 checks; here each rule is checked once through the
binding)."""
import collections
import math
import os
import re
import tracemalloc
import types

import pytest

import ysp.response as yr

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(HERE, "..", "..", "..", "include", "ysp", "response.h")
INPUT_HEADER = os.path.join(HERE, "..", "..", "..", "include", "ysp", "input.h")


def test_version_matches_pyproject():
    text = open(os.path.join(HERE, "pyproject.toml")).read()
    m = re.search(r'^version = "([^"]+)"', text, re.M)
    assert m and yr.__version__ == m.group(1)
    assert yr.__version__.startswith(yr.version() + ".")


@pytest.mark.skipif(not os.path.exists(HEADER), reason="needs the repository's include/")
def test_constants_match_the_headers():
    text = open(HEADER).read() + open(INPUT_HEADER).read()

    def c(name):
        m = re.search(r"#define %s\s+\(?\(?(?:uint16_t\))?\s*(0x[0-9A-Fa-f]+|\d+)u?" % name, text)
        if not m:
            m = re.search(r"\b%s\s*=\s*(\d+)" % name, text)
        assert m, name
        return int(m.group(1), 0)

    for py, cn in [("R_SENT_MISMATCH", "YRSP_R_SENT_MISMATCH"), ("R_HOLD_TIMEOUT", "YRSP_R_HOLD_TIMEOUT"),
                   ("E_ENDED", "YRSP_E_ENDED"), ("E_CLASS_MASK", "YRSP_E_CLASS_MASK"), ("ENDED", "YRSP_ENDED"),
                   ("LOG_CAP", "YRSP_LOG_CAP"), ("MAX_SENT", "YRSP_MAX_SENT"), ("KIND_SYNC", "YIN_KIND_SYNC"),
                   ("KIND_USER", "YIN_KIND_USER"), ("PROXIMITY", "YIN_PROXIMITY"), ("DEVTICKS", "YIN_DEVTICKS"),
                   ("TIER_SIM", "YIN_TIER_SIM"), ("AXIS_GAMEPAD", "YIN_AXIS_GAMEPAD"), ("AXIS_WHEEL", "YIN_AXIS_WHEEL"),
                   ("UNC_MAX_US", "YIN_UNC_MAX")]:
        assert getattr(yr, py) == c(cn), py
    # the data columns: YRSP_DATA_COLS's names, in its order
    block = text[text.index("#define YRSP_DATA_COLS"):text.index("#define YRSP_N_DATA_COLS")]
    assert yr.DATA_COLS == tuple(re.findall(r'YTR_COL_SET\("([a-z_]+)"', block))


# --- the event -----------------------------------------------------------


def test_event_layout_and_units():
    e = yr.event(12.3456789014, control="f", unc=0.0004, stamp=yr.TIER_1)
    assert e.t_ns == 12345678901 and e.t == e.t_ns / 1e9
    assert e.kind == yr.KIND_KEYBOARD and e.type == yr.PRESS and e.control == yr.scancode("f") == 9
    assert e.value == 1.0 and e.unc == pytest.approx(0.0004) and e.stamp == 1
    assert len(e) == 16 and e[1] == e.t_ns and e[10] == e.unc
    names = ("t", "t_ns", "device", "control", "code", "mods", "kind", "type", "flags", "stamp", "unc", "x", "y",
             "value", "aux", "ticks")
    for i, n in enumerate(names):
        assert getattr(e, n) == e[i]
    r = yr.event(1.0, type=yr.RELEASE, control=9)
    assert r.value == 0.0
    big = yr.event(1.0, unc=1.0)
    assert big.unc == yr.UNC_MAX_US / 1e6   # 65.5 ms or more
    assert yr.event(t_ns=5).t_ns == 5
    b = yr.event(2.0, kind="box", control=3, device=7)
    assert b.kind == yr.KIND_BOX and b.device == 7
    for bad in (dict(t=-1.0), dict(t=math.nan), dict(t=1.0, unc=-0.001), dict(t=1.0, device=-1),
                dict(t=1.0, kind="box", control="f"), dict(t=1.0, kind="nope"), dict(t=1.0, control="nokey"), dict()):
        with pytest.raises(yr.ArgumentError):
            yr.event(**bad)


def test_feed_takes_tuples_attributes_and_other_packages_events():
    c = yr.Collector(["f", "j"])
    c.arm(1.0)
    c.set_onset(1.0)
    # another ysp package's Event: any 16-tuple in the same order
    Other = collections.namedtuple("Event", "t t_ns device control code mods kind type flags stamp unc x y value aux ticks")
    assert c.feed(Other(1.5, 1500000000, 0, 13, 0, 0, 0, yr.PRESS, 0, 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0)) == yr.ENDED
    assert c.finish().response == "j"
    # an object with the attributes; missing ones are 0, t_ns may be absent
    c.arm(2.0)
    c.set_onset(2.0)
    assert c.feed(types.SimpleNamespace(t=2.25, kind=0, type=yr.PRESS, control=9)) == yr.ENDED
    c.feed(types.SimpleNamespace(t=2.3, type=yr.RELEASE, control=9))
    r = c.finish()
    assert r.response == "f" and r.rt_ns == 250000000
    # t_ns wins over t when it is an int
    c.arm(3.0)
    c.set_onset(3.0)
    c.feed(types.SimpleNamespace(t=99.0, t_ns=3300000001, type=yr.PRESS, control=9))
    assert c.finish().rt_ns == 300000001
    with pytest.raises(yr.ArgumentError):
        c.feed((1.0, 2))                         # not 16 fields
    with pytest.raises(yr.ArgumentError):
        c.feed(types.SimpleNamespace(type=1))   # no time
    with pytest.raises(yr.ArgumentError):
        c.feed(yr.event(1.0, type=yr.NONE))     # the header refuses NONE
    assert "yrsp_feed" in c.error


def test_feed_reads_properties_and_slots():
    class Prop:                                   # fields from the class, none in the instance dict
        def __init__(self, t):
            self._t = t

        @property
        def t(self):
            return self._t

        type = yr.PRESS
        control = 13

    class Slots:
        __slots__ = ("t_ns", "type", "control")

        def __init__(self, t_ns):
            self.t_ns, self.type, self.control = t_ns, yr.PRESS, 9

    c = yr.Collector(["f", "j"])
    c.arm(1.0)
    c.set_onset(1.0)
    assert c.feed(Prop(1.3)) == yr.ENDED
    assert c.finish().response == "j"
    c.feed(yr.event(1.31, type=yr.RELEASE, control="j"))
    c.arm(2.0)
    c.set_onset(2.0)
    assert c.feed(Slots(2200000000)) == yr.ENDED
    r = c.finish()
    assert r.response == "f" and r.rt_ns == 200000000


def test_feed_reads_fields_a_getattr_gives():
    class Wrapped:                                # a wrapper that forwards to a dict
        def __init__(self, **kw):
            self._kw = kw

        def __getattr__(self, name):
            try:
                return self._kw[name]
            except KeyError:
                raise AttributeError(name)

    c = yr.Collector(["f", "j"])
    c.arm(1.0)
    c.set_onset(1.0)
    assert c.feed(Wrapped(t=1.25, type=yr.PRESS, control=13)) == yr.ENDED
    r = c.finish()
    assert r.response == "j" and r.rt_ns == 250000000


def test_feed_allocates_nothing_per_event():
    c = yr.Collector(["f", "j"], kinds=yr.KINDS_ALL)
    ev = yr.event(1.0, control="space")
    rel = yr.event(1.0, type=yr.RELEASE, control="space")
    tup = tuple(ev)
    ns = types.SimpleNamespace(**{k: getattr(ev, k) for k in ("t", "t_ns", "device", "control", "kind", "type")})
    nrel = types.SimpleNamespace(**{k: getattr(rel, k) for k in ("t", "t_ns", "device", "control", "kind", "type")})
    for a, b in ((ev, rel), (tup, tuple(rel)), (ns, nrel)):
        c.feed(a); c.feed(b)                     # warm up
        tracemalloc.start()
        before = tracemalloc.take_snapshot()
        for _ in range(2000):
            c.feed(a)
            c.feed(b)
        after = tracemalloc.take_snapshot()
        tracemalloc.stop()
        grown = sum(s.size_diff for s in after.compare_to(before, "filename") if s.size_diff > 0)
        assert grown < 2048, grown              # the snapshots' own bookkeeping, not per event


# --- the rules through the binding ------------------------------------------


def trial(**kw):
    return yr.Collector(["f", "j"], duration=1.5, min_rt=0.1, **kw)


def test_keyboard_trial_rt_from_the_onset():
    c = trial()
    c.arm(10.0)
    assert c.key(10.2, "f") == yr.OPEN          # EARLY: before the onset
    assert c.set_onset(10.5, src="flip", tier=yr.TIER_1, residual=-0.00002, frame=31) == yr.OPEN
    assert c.key(10.55, "j") == yr.OPEN         # ANTICIPATION
    c.key(10.555, "j", down=False)
    assert c.key(10.56, "space") == yr.OPEN     # no choice
    assert c.key(10.9, "j") == yr.ENDED
    c.key(10.97, "j", down=False)
    r = c.finish()
    assert r.response == "j" and r.choice == 1 and r.rt == pytest.approx(0.4) and r.rt_ns == 400000000
    assert r.rt_key_duration_ns == 70000000
    assert r.t_onset_ns == 10500000000 and r.onset_src == yr.ONSET_FLIP and r.onset_tier == 1
    assert r.onset_residual == pytest.approx(-0.00002) and r.onset_frame == 31
    # the space at 0.06 s is no choice and an anticipation too
    assert (r.n_early, r.n_anticipations, r.n_not_choice, r.n_entries) == (1, 2, 1, 4)
    assert r.flags & yr.R_RESPONDED and not r.flags & yr.R_ONSET_PLAN
    e = c.entries()
    assert [x.flags & yr.E_CLASS_MASK for x in e] == [yr.E_EARLY, yr.E_ANTICIPATION, yr.E_ANTICIPATION, yr.E_VALID]
    assert e[3].t_release_ns == 10970000000 and e[3].choice == 1 and e[2].choice is None
    assert c.choices == ("f", "j")


def test_timeout_and_no_deadline():
    c = trial()
    c.arm(1.0)
    c.set_onset(1.0)
    assert c.update(2.4999) == yr.OPEN
    assert c.update(2.5) == yr.ENDED
    c.key(2.6, "f")                              # LATE
    r = c.finish()
    assert r.flags & yr.R_TIMEOUT and r.rt is None and r.response is None and r.choice is None and r.n_late == 1
    d = yr.Collector(["f"], duration=math.inf)   # no deadline
    d.arm(0.0)
    d.set_onset(0.0)
    assert d.update(1e5) == yr.OPEN
    assert yr.Collector(["f"], duration=None).finish  # None is no deadline too


def test_plan_then_final_onset_reclassifies():
    c = trial()
    c.arm(0.0)
    c.set_onset(1.0, final=False, src="plan", residual=0.001, frame=60)
    assert c.plan_frame == 60
    assert c.key(1.11, "f") == yr.ENDED          # valid against the plan
    assert c.set_onset(1.0167, src="flip", tier=1, frame=60) == yr.ENDED   # a frame late: now an anticipation
    assert c.plan_frame is None
    r = c.finish()
    assert r.flags & yr.R_ENDED_EARLY and r.rt is None and r.n_anticipations == 1
    assert r.plan_residual == pytest.approx(0.001) and r.flags & yr.R_RECLASSIFIED
    c.arm(5.0)
    c.set_onset(5.0, final=False)
    r = c.finish()
    assert r.flags & yr.R_ONSET_PLAN


def test_held_keys_and_release_rules():
    c = trial()
    c.key(0.5, "f")                              # down before the window
    c.arm(1.0)
    c.set_onset(1.0)
    c.key(1.3, "f", repeat=True)                 # OS repeat: held, not a response
    r = c.finish()
    assert r.flags & yr.R_HELD_AT_OPEN and r.n_held == 1 and r.n_responses == 0
    h = trial(allow_held_key=True)
    h.key(0.5, "f")
    h.arm(1.0)
    h.set_onset(1.0)
    assert h.key(1.3, "f", repeat=True) == yr.ENDED
    assert h.entries()[0].flags & yr.E_HELD
    w = trial(wait_for_key_release=True, max_hold=0.5)
    w.arm(0.0)
    w.set_onset(0.0)
    assert w.key(0.4, "j") == yr.OPEN            # waits for the release
    assert w.update(0.8) == yr.OPEN
    assert w.update(0.9) == yr.ENDED             # max_hold after the press
    assert w.finish().flags & yr.R_HOLD_TIMEOUT


def test_persist_and_max_responses():
    c = yr.Collector(["f", "j"], persist=True, max_responses=2)
    c.arm(0.0)
    c.set_onset(0.0)
    assert c.key(0.3, "f") == yr.OPEN
    c.key(0.31, "f", down=False)
    assert c.key(0.5, "j") == yr.ENDED
    r = c.finish()
    assert r.n_responses == 2 and r.response == "f"


def test_kinds_modes_and_devices():
    allk = yr.Collector()                        # ALL mode, kinds 0: the keyboard only
    allk.arm(0.0)
    allk.set_onset(0.0)
    assert allk.feed(yr.event(0.2, kind="mouse", control=1)) == yr.OPEN
    assert allk.key(0.3, "q") == yr.ENDED
    r = allk.finish()
    assert r.choice is None and r.response is None and r.control == yr.scancode("q")
    every = yr.Collector(kinds="all")            # every kind but SYNC
    every.arm(0.0)
    every.set_onset(0.0)
    assert every.feed(yr.event(0.1, kind="sync", control=1)) == yr.OPEN
    assert every.feed(yr.event(0.2, kind="box", control=4)) == yr.ENDED
    sync = yr.Collector(kinds=["sync"])          # wait for the scanner
    sync.arm(0.0)
    sync.set_onset(0.0)
    assert sync.feed(yr.event(0.1, kind=yr.KIND_SYNC, control=1)) == yr.ENDED
    none = yr.Collector(mode="none")
    none.arm(0.0)
    none.set_onset(0.0)
    assert none.key(0.2, "f") == yr.OPEN and none.finish().n_entries == 1
    box = yr.Collector([{"name": "left", "kind": "box", "control": 1, "device": 2},
                        {"name": "right", "kind": yr.KIND_BOX, "control": 2, "device": 2}])
    box.arm(0.0)
    box.set_onset(0.0)
    assert box.feed(yr.event(0.2, kind="box", control=2, device=3)) == yr.OPEN   # another box
    assert box.feed(yr.event(0.3, kind="box", control=2, device=2)) == yr.ENDED
    assert box.finish().response == "right"


def test_keycode_and_crossing_choices():
    kc = yr.Collector([{"key": "a", "match": "keycode", "name": "A"}])
    kc.arm(0.0)
    kc.set_onset(0.0)
    assert kc.key(0.2, yr.scancode("q"), code=ord("a")) == yr.ENDED   # AZERTY: the q position types a
    assert kc.finish().response == "A"
    g = yr.Collector([{"name": "trigger", "kind": "gamepad", "cross": "rising",
                       "control": yr.AXIS_GAMEPAD + 5, "level": 0.5, "hysteresis": 0.2}])
    g.arm(0.0)
    g.set_onset(0.0)
    s = dict(kind="gamepad", type=yr.SAMPLE, control=yr.AXIS_GAMEPAD + 5)
    assert g.feed(yr.event(0.10, value=0.3, **s)) == yr.OPEN
    assert g.feed(yr.event(0.25, value=0.6, **s)) == yr.ENDED
    r = g.finish()
    assert r.rt_ns == 250000000 and g.entries()[0].flags & yr.E_CROSSING and g.entries()[0].value == pytest.approx(0.6)


def test_sources_give_the_stamp_quality():
    c = yr.Collector([{"key": "f"}], sources=[{"kind": "keyboard", "tier": 2, "partial": True,
                                               "lo": -0.0005, "hi": 0.0015, "note": "measured with a loopback"}])
    c.arm(0.0)
    c.set_onset(0.0)
    c.key(0.3, "f")
    r = c.finish()
    assert r.stamp_tier == 2 and r.stamp_partial is True
    assert r.stamp_lo == pytest.approx(-0.0005) and r.stamp_hi == pytest.approx(0.0015)
    row = c.data_row()
    assert row["rsp_tier"] == 2 and tuple(row) == yr.DATA_COLS


def test_trace():
    c = yr.Collector(["f"], trace_cap=3, trace_kinds=["mouse"])
    c.arm(0.0)
    for i in range(5):
        c.feed(yr.event(0.01 * i, kind="mouse", type=yr.SAMPLE, x=i, y=-i))
    c.key(0.2, "f")
    r = c.finish()
    t = c.trace()
    assert r.n_trace == 3 and r.flags & yr.R_TRACE_FULL and [e.x for e in t] == [0.0, 1.0, 2.0]
    assert all(isinstance(e, yr.Event) for e in t)


def test_codes_sent():
    c = trial()
    c.arm(0.0)
    c.set_onset(0.0)
    c.key(0.4, "j")
    c.set_sent([(0.4, yr.KIND_KEYBOARD, 0, yr.scancode("j"), 1, True)])
    r = c.finish()
    assert not r.flags & yr.R_SENT_MISMATCH and r.n_sent == 1
    c.set_sent([types.SimpleNamespace(t=0.4, t_ns=400000000, kind=0, device=0, control=yr.scancode("j"),
                                      choice=0, written=True)])     # the other choice's code
    assert c.finish().flags & yr.R_SENT_MISMATCH
    c.set_sent([], total=0)
    assert c.finish().flags & yr.R_SENT_MISMATCH                    # a response without its code


def test_score_matches_the_live_loop():
    events = [yr.event(0.5, control="f"), yr.event(0.6, control="f", type=yr.RELEASE),
              yr.event(1.05, control="j"), yr.event(1.62, control="f"), yr.event(1.70, control="f", type=yr.RELEASE),
              yr.event(1.75, control="j", type=yr.RELEASE)]
    live = trial()
    live.feed(events[0]); live.feed(events[1])
    live.arm(0.9)
    live.set_onset(1.0, src="flip", tier=1)
    live.feed_many(events[2:])
    a = live.finish()
    off = trial()
    b = off.score(events, 1.0, arm=0.9, src="flip", tier=1)
    assert a == b
    assert b.response == "f" and b.rt_ns == 620000000 and b.n_anticipations == 1
    # new rules on the same log: a minimum RT of 0.03 s makes the j a response
    again = yr.Collector(["f", "j"], duration=1.5, min_rt=0.03).score(events, 1.0, arm=0.9)
    assert again.response == "j" and again.rt_ns == 50000000 and again.rt_key_duration_ns == 700000000


def test_seconds_round_to_the_nearest_ns():
    c = yr.Collector(["f"])
    c.arm(86400.0)
    c.set_onset(86400.000000001)                 # a day of uptime keeps its ns
    c.key(86400.4000000004, "f")
    r = c.finish()
    assert r.t_response_ns == 86400400000000 and r.t_onset_ns == 86400000000001 and r.rt_ns == 399999999


def test_seconds_give_back_their_ns_below_2_23_s():
    # t = ns / 1e9 (the nearest double) converts back to ns: the whole
    # seconds exactly, the fraction scaled alone
    import random
    rng = random.Random(20261025)
    vals = [rng.randrange(0, 2 ** 23 * 10 ** 9) for _ in range(20000)]
    vals += [rng.randrange(10 ** 14, 10 ** 15) for _ in range(2000)] + [0, 1, 999999999, 2 ** 23 * 10 ** 9 - 1]
    for ns in vals:
        assert yr.event(ns / 1e9).t_ns == ns, ns


def test_errors_and_state():
    for kw in (dict(choices=["nokey"]), dict(choices=[{"controll": 4}]), dict(choices=["f"], mode="none"),
               dict(min_rt=-0.1), dict(duration=math.nan), dict(min_rt=2.0, duration=1.0), dict(max_hold=math.inf),
               dict(choices=[{"kind": "keyboard"}]), dict(kinds=["nope"]), dict(choices=["f"] * 33),
               dict(sources=[{"tier": 9}])):
        with pytest.raises(yr.ArgumentError):
            yr.Collector(**kw)
    with pytest.raises(TypeError):
        yr.Collector("fj")
    c = yr.Collector(["f"])
    with pytest.raises(yr.ArgumentError):
        c.arm(-1.0)
    with pytest.raises(yr.ArgumentError):
        c.set_onset(math.nan)
    with pytest.raises(yr.StateError):
        c.data_row()
    with c:
        assert not c.closed
    assert c.closed
    with pytest.raises(yr.StateError):
        c.key(1.0, "f")
    with pytest.raises(yr.StateError):
        c.finish()
    assert issubclass(yr.ArgumentError, ValueError) and issubclass(yr.StateError, yr.Error)


def test_key_names():
    assert yr.scancode("F") == 9 and yr.scancode("arrowleft") == yr.scancode("left") == 80
    assert yr.scancode("nokey") is None and yr.key_name(9) == "f" and yr.key_name(80) == "left"
    assert yr.key_name(0) is None
