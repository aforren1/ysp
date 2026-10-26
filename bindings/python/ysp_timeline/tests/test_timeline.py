"""Tests for ysp.timeline: the binding drives ysp/timeline.h as the C API does.

tests/adapt/timeline_test.c checks the header itself. Here: the manual's
trial by calls and as an op table (the same values and records), landing
records against the quantization rule, eases against their formulas, a
rewind and a seek that replay a trial bit for bit, two timelines given the
same calls, fields (to, from, fromTo, set, int, stagger, overwrite, kill,
callbacks), tracks (keys, Bezier keys, samples, a repeat), the times in
seconds, the refusals, peek, rates, bind() and the default frame index.
"""
import array
import math
import os
import re
import struct

import pytest

import ysp.timeline as ytl

HERE = os.path.dirname(os.path.abspath(__file__))
P = 16666667            # a 60 Hz period, ns
T0 = 10_000_000_000     # the first frame's onset, ns
TRIAL = 1


def s(ns):
    return ns / 1e9


def onset(i):
    return s(T0 + i * P)


def build_trial(tl, base=TRIAL):
    q = tl.seq(base)
    q.on("fix").wait(0.5).off("fix").on("grating").trigger(12)
    q.to("contrast", 0.5, duration=0.1, ease="cosine")
    q.wait(0.6).then("contrast", 0.0, duration=0.1, ease="cosine").off("grating")
    return q


TRIAL_OPS = [
    ("on", "fix"), ("wait", 0.5), ("off", "fix"), ("on", "grating"), ("trigger", 12),
    ("to", "contrast", 0.5, {"duration": 0.1, "ease": "cosine"}), ("wait", 0.6),
    ("then", "contrast", 0.0, {"duration": 0.1, "ease": "cosine"}), ("off", "grating"),
]


def new_tl(**kw):
    return ytl.Timeline(["fix", "grating", "contrast"], **kw)


def vbytes(tl):
    return bytes(tl.values.cast("B"))


def run(tl, first, n):
    """Frames first .. first + n - 1; per frame the values' and fields' bytes
    and the fired events, with frames counted from `first`."""
    out = []
    for i in range(first, first + n):
        ev = tl.evaluate(onset(i), s(P), i)
        out.append((vbytes(tl), bytes(tl.field_values.cast("B")),
                    [(e.kind, e.code, e.target, e.value, e.frame - first, e.residual_ns, e.flags) for e in ev]))
    return out


def test_version_matches_pyproject():
    text = open(os.path.join(os.path.dirname(HERE), "pyproject.toml")).read()
    assert re.search(r'^version = "([^"]+)"', text, re.M).group(1) == ytl.__version__
    assert ytl.__version__ == ytl.version() or ytl.__version__.startswith(ytl.version() + ".")


def test_manual_trial_values_and_records():
    tl = new_tl()
    q = build_trial(tl)
    assert q.t == 1.2 and q.t_ns == 1_200_000_000
    tl.anchor(TRIAL, onset(0))
    fired = {}
    vals = []
    for i in range(80):
        ev = tl.evaluate(onset(i), s(P), i)
        fired[i] = ev
        vals.append(tuple(tl.values))
    # fixation from frame 0; at 0.5 s (frame 30, 10 ns after the event) the
    # fixation goes, the grating comes, the trigger fires
    assert vals[0][:2] == (1.0, 0.0) and vals[29][:2] == (1.0, 0.0) and vals[30][:2] == (0.0, 1.0)
    kinds = [(e.kind, e.target, e.code) for e in fired[30]]
    assert kinds == [(ytl.OFFSET, 0, 0), (ytl.ONSET, 1, 0), (ytl.TRIGGER, 0, 12)]
    for e in fired[30]:
        assert e.frame == 30 and e.residual_ns == 30 * P - 500_000_000 == 10
        assert e.onset_ns == T0 + 30 * P and e.onset == onset(30) and e.time_ns == 500_000_000
        assert e.flags == ytl.EV_FIRED
    # the ramp is read at the onset: (1 - cos(pi u)) / 2 x 0.5
    for i in range(30, 37):
        u = min(max((i * P - 500_000_000) / 100_000_000, 0.0), 1.0)
        want = 0.5 * (1 - math.cos(math.pi * u)) / 2
        assert abs(vals[i][2] - want) < 1e-6
    # the grating goes at 1.2 s: 72 x P = 1.200000024 s, the nearest frame
    assert vals[71][1] == 1.0 and vals[72][1] == 0.0 and [e.kind for e in fired[72]] == [ytl.OFFSET]
    assert vals[72][2] == 0.0
    rec = tl.events(TRIAL)
    assert len(rec) == 5 and all(e.flags & ytl.EV_FIRED for e in rec)


def test_op_table_equals_calls():
    a, b = new_tl(), new_tl()
    build_trial(a)
    b.check_ops(TRIAL, TRIAL_OPS)
    b.seq(TRIAL).run(TRIAL_OPS)
    for tl in (a, b):
        tl.anchor(TRIAL, onset(0))
    assert run(a, 0, 90) == run(b, 0, 90)
    assert a.events() == b.events()


def test_check_ops_and_run_report_the_index():
    tl = new_tl()
    bad = TRIAL_OPS[:3] + [("to", "contrast", 0.5, {"duration": 0.1, "start": 1.0})]
    with pytest.raises(TypeError):
        tl.check_ops(TRIAL, bad)            # start is not an op table field
    bad = TRIAL_OPS[:3] + [("to", "contrast", 0.5, {"duration": 0.1, "cycles": "forever"})]
    with pytest.raises(ytl.ArgumentError) as ei:
        tl.check_ops(TRIAL, bad)
    assert ei.value.index == 3
    with pytest.raises(ytl.ArgumentError) as ei:
        tl.seq(TRIAL).run(bad)
    assert ei.value.index == 3
    assert tl.events() == []              # all or none


def test_rewind_replays_bit_for_bit():
    tl = new_tl()
    dot, alpha = tl.field((0.0, 0.0)), tl.field(0.0)
    build_trial(tl)
    r = tl.seq(TRIAL)
    r.to(dot, (300, 120), duration=0.4, ease="power2.out")
    r.from_(alpha, 1.0, duration=0.25, ease=(0.42, 0.0, 0.58, 1.0))
    r.at(0.45).to(dot, (0, 0), duration=0.3, ease="quad_in_out", yoyo=True, cycles=2)
    r.at(0.8).set(alpha, 0.25)
    tl.anchor(TRIAL, onset(0))
    first = run(tl, 0, 100)
    # trial time 0 again at frame 200: every event fires again, every tween replays
    tl.anchor(TRIAL, onset(200))
    second = run(tl, 200, 100)
    assert first == second
    assert any(f[2] for f in first)
    # the set's record names the field's slot
    sets = [e for e in tl.events(TRIAL) if e.kind == ytl.TARGET_SET]
    assert len(sets) == 1 and sets[0].user == alpha.index and sets[0].value == 0.25


def test_seek_gives_continuous_play():
    def make():
        tl = new_tl()
        dot = tl.field((5.0, 5.0))
        build_trial(tl)
        r = tl.seq(TRIAL)
        r.to(dot, (300, 120), duration=0.4, ease="power2.out")
        r.at(0.6).to(dot, (10, 10), duration=0.3, ease="cosine")
        r.at(0.65).set(dot, (7, 7))
        return tl
    ref = make()
    ref.anchor(TRIAL, onset(0))
    cont = run(ref, 0, 90)
    for k in (1, 29, 30, 31, 40, 70):
        tl = make()
        n = tl.skip(TRIAL, onset(k), s(k * P))
        assert n >= 0
        got = run(tl, k, 90 - k)
        assert [g[:2] for g in got] == [c[:2] for c in cont[k:]], k
        assert got[-1][1] != got[0][1] or k >= 39   # it moves until the set at 0.65 s: its tweens are alive
    # a skip passes over the events before its time and fires none of them
    tl = make()
    assert tl.skip(TRIAL, onset(40), s(40 * P)) == 5    # 4 events and the set's record
    assert all(e.flags == ytl.EV_SKIPPED for e in tl.events(TRIAL) if e.time_ns < 40 * P)


def test_two_timelines_same_bytes():
    a, b = new_tl(), new_tl()
    for tl in (a, b):
        build_trial(tl)
        tl.anchor(TRIAL, onset(3))
    ra, rb = run(a, 3, 60), run(b, 3, 60)
    assert ra == rb and a.events() == b.events()


def test_eases_against_formulas():
    forms = {
        "linear": lambda u: u, "quad_in": lambda u: u * u, "quad_out": lambda u: 1 - (1 - u) ** 2,
        "cubic_in": lambda u: u ** 3, "cubic_out": lambda u: 1 - (1 - u) ** 3,
        "power2.out": lambda u: 1 - (1 - u) ** 3, "sine.inOut": lambda u: (1 - math.cos(math.pi * u)) / 2,
        "quad_in_out": lambda u: 2 * u * u if u < 0.5 else 1 - 2 * (1 - u) ** 2,
        "cubic_in_out": lambda u: 4 * u ** 3 if u < 0.5 else 1 - 4 * (1 - u) ** 3,
        ytl.EASE_COSINE: lambda u: (1 - math.cos(math.pi * u)) / 2,
    }
    for name, f in forms.items():
        tl = ytl.Timeline(1)
        tl.seq(TRIAL).to(0, 10.0, duration=1.0, ease=name)
        tl.anchor(TRIAL, onset(0))
        for i in range(0, 61):
            tl.evaluate(onset(i), s(P), i)
            u = min(i * P / 1e9, 1.0)
            assert abs(tl.value(0) - 10 * f(u)) < 2e-5, (name, i)
    # a step lands with events: at the end of the lead window
    tl = ytl.Timeline(1)
    tl.seq(TRIAL).at(0.5).to(0, 1.0, duration=0.0, ease="step")
    tl.anchor(TRIAL, onset(0))
    got = [(tl.evaluate(onset(i), s(P), i), tl.value(0))[1] for i in range(40)]
    assert got.index(1.0) == 30
    with pytest.raises(ytl.ArgumentError):
        tl.seq(TRIAL).to(0, 1.0, ease="bounce")


def test_log_and_bezier_ease():
    tl = ytl.Timeline(2)
    q = tl.seq(TRIAL)
    q.to(0, 8.0, from_=1.0, duration=1.0, ease="log")
    q.to(1, 1.0, from_=0.0, duration=1.0, ease=(0.0, 0.0, 1.0, 1.0))   # the Bezier of a line
    tl.anchor(TRIAL, onset(0))
    for i in range(0, 61, 6):
        tl.evaluate(onset(i), s(P), i)
        u = i * P / 1e9
        assert abs(tl.value(0) - 8.0 ** u) < 1e-4
        assert abs(tl.value(1) - u) < 1e-5


def test_fields():
    tl = ytl.Timeline(0, fields=16, tweens=32)
    x = tl.field(2.0)
    rgb = tl.field((0.1, 0.2, 0.3))
    k = tl.field(0, int=True)
    assert rgb.n == 3 and k.is_int and float(x) == 2.0
    q = tl.seq(TRIAL)
    q.to(x, 12.0, duration=1.0)                 # from the rest value 2
    q.from_to(rgb, (1, 1, 1), (0, 0, 0.5), duration=0.5, delay=0.5)
    q.to(k, 10, duration=1.0)
    tl.anchor(TRIAL, onset(0))
    tl.evaluate(onset(0), s(P), 0)
    assert x.value == 2.0
    assert rgb.value == (1.0, 1.0, 1.0)         # immediate: from_to shows its from before the start
    tl.evaluate(onset(30), s(P), 30)
    assert abs(x.value - (2 + 10 * 30 * P / 1e9)) < 1e-4
    assert k.value == round(10 * 30 * P / 1e9)
    tl.evaluate(onset(60), s(P), 60)
    assert rgb.value == (0.0, 0.0, 0.5) and x.value == 12.0 and k.value == 10
    # the timeline owns a field after its first write: a caller's write goes at the next evaluate
    x.value = 99.0
    tl.evaluate(onset(61), s(P), 61)
    assert x.value == 12.0
    assert tl.kill(x) == 1 and x.kill() == 0
    x.value = 99.0
    tl.evaluate(onset(62), s(P), 62)
    assert x.value == 99.0
    # the bulk view: 4 words per slot
    fv = tl.field_values
    assert len(fv) == 64 and fv[4 * rgb.index + 2] == 0.5
    assert tl.tween_count() == (2, 2)
    with pytest.raises(ytl.ArgumentError):
        q.to(rgb, (1, 2), duration=0.1)
    with pytest.raises(TypeError):
        ytl.Field()
    # a field outlives its variable; free() gives the slot back
    del rgb
    assert tl.tween_count() == (2, 2)
    slot = k.index
    k.free()
    assert tl.tween_count() == (1, 1)
    with pytest.raises(ytl.ArgumentError):
        k.value
    assert tl.field(1.0).index == slot


def test_from_shows_from_then_returns():
    tl = ytl.Timeline(0)
    a = tl.field(0.75)
    tl.seq(TRIAL).at(0.5).from_(a, 0.0, duration=0.5)
    tl.anchor(TRIAL, onset(0))
    tl.evaluate(onset(0), s(P), 0)
    assert a.value == 0.0                       # immediate
    tl.evaluate(onset(90), s(P), 90)
    assert a.value == 0.75


def test_stagger_and_overwrite():
    tl = ytl.Timeline(0, fields=16, tweens=64)
    dots = tl.fields(5, 0.0)
    assert [d.index for d in dots] == list(range(dots[0].index, dots[0].index + 5))
    q = tl.seq(TRIAL)
    q.to(dots, 1.0, duration=0.1, stagger=0.1)
    assert q.prev_start == 0.0 and abs(q.prev_end - 0.5) < 1e-12 and q.last_id == 4
    tl.anchor(TRIAL, onset(0))
    tl.evaluate(onset(12), s(P), 12)          # 0.2 s: dots 0 and 1 done, dot 2 starting
    assert [d.value for d in dots[:2]] == [1.0, 1.0] and dots[3].value == 0.0
    with pytest.raises(ytl.ArgumentError):
        q.to([dots[0], dots[2]], 1.0, duration=0.1, stagger=0.1)   # not adjacent
    # overwrite kills the field's other tweens at the call
    q2 = tl.seq(TRIAL, now=True)
    q2.to(dots[4], 5.0, duration=0.1, overwrite=True)
    assert tl.tween_count()[0] == 5
    with pytest.raises(ytl.ArgumentError):
        q2.to(dots[4], 5.0, stagger=-0.1)


def test_set_lands_with_events_and_leaves_a_record():
    tl = ytl.Timeline(1, fields=4)
    f = tl.field(0.0)
    q = tl.seq(TRIAL)
    q.at(0.5).set(f, 1.0).set(0, 1.0)
    tl.anchor(TRIAL, onset(0))
    for i in range(40):
        ev = tl.evaluate(onset(i), s(P), i)
        if ev:
            assert i == 30 and f.value == 1.0 and tl.value(0) == 1.0
            assert sorted(e.kind for e in ev) == [ytl.SET, ytl.TARGET_SET]
            rec = [e for e in ev if e.kind == ytl.TARGET_SET][0]
            assert rec.user == f.index and rec.code == q.last_id
        else:
            assert (f.value == 1.0) == (i > 30)


def test_callbacks():
    tl = ytl.Timeline(0, fields=4)
    f = tl.field(0.0)
    calls = []
    q = tl.seq(TRIAL)
    q.to(f, 1.0, duration=0.2, on_start=lambda: calls.append(("start", f.value)),
         on_complete=lambda: calls.append(("done", f.value)))
    q.at(0.5).to(f, 2.0, duration=0.1, on_complete=True)       # events only
    tl.anchor(TRIAL, onset(0))
    kinds = []
    for i in range(40):
        kinds += [(i, e.kind) for e in tl.evaluate(onset(i), s(P), i)]
    assert calls == [("start", 0.0), ("done", 1.0)]          # after the fields are written
    assert kinds == [(0, ytl.CALL_START), (12, ytl.CALL_COMPLETE), (36, ytl.CALL_COMPLETE)]
    # a rewind calls them again
    tl.anchor(TRIAL, onset(40))
    tl.evaluate(onset(40), s(P), 40)
    assert calls[-1] == ("start", 0.0)

    def boom():
        raise RuntimeError("in a callback")
    tl2 = ytl.Timeline(0, fields=2)
    g = tl2.field(0.0)
    tl2.seq(TRIAL).to(g, 1.0, duration=0.1, on_start=boom)
    tl2.anchor(TRIAL, onset(0))
    with pytest.raises(RuntimeError):
        tl2.evaluate(onset(0), s(P), 0)
    assert tl2.evaluate(onset(1), s(P), 1) == ()          # the frame after goes on
    assert tl2.events(TRIAL)[0].flags & ytl.EV_FIRED      # the record holds the frame


def test_tracks():
    tl = ytl.Timeline(["x", "path", "flicker"])
    tl.set_keys("x", TRIAL, [(0.0, 0.0), (1.0, 10.0, (0.0, 0.0, 1.0, 1.0)), (2.0, 20.0)])
    xs = array.array("f", [float(i) for i in range(121)])     # 1 per sample at 120/s
    tl.set_track("path", TRIAL, samples=xs, rate=120, interp="linear")
    tl.set_track("flicker", TRIAL, keys=[(0.0, 1.0, "step"), (s(2 * P), 0.0, "step")], period=s(4 * P))
    tl.anchor(TRIAL, onset(0))
    flick = []
    for i in range(0, 61):
        tl.evaluate(onset(i), s(P), i)
        flick.append(tl.value("flicker"))
        assert abs(tl.value("path") - 120 * i * P / 1e9) < 1e-3
    assert abs(tl.value("x") - 10 * 60 * P / 1e9) < 1e-4
    assert flick[:8] == [1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0]
    tl.set_track("path", TRIAL)                      # removes it
    with pytest.raises(ytl.ArgumentError):
        tl.set_track("x", TRIAL, samples=[1.0, 2.0], rate=0)
    with pytest.raises(ytl.ArgumentError):
        tl.set_keys("x", TRIAL, [(1.0, 0.0), (0.5, 1.0)])   # unsorted
    with pytest.raises(ytl.BoundError):
        tl.add(1.0, "onset", base=TRIAL, target="x")
    assert tl.clear(TRIAL) == 0


def test_channel_tween_takes_over():
    tl = ytl.Timeline(1)
    tl.set_keys(0, TRIAL, [(0.0, 0.0), (10.0, 10.0)])       # 1 unit/s
    tl.anchor(TRIAL, onset(0))
    for i in range(31):
        tl.evaluate(onset(i), s(P), i)
    v30 = tl.value(0)
    assert v30 == pytest.approx(30 * P / 1e9, abs=1e-6)
    tl.tween(0, 0.0, base=TRIAL, duration=0.5)               # from "now", frame 30's base time
    tl.evaluate(onset(31), s(P), 31)
    assert tl.value(0) == pytest.approx(v30 * (1 - P / 0.5e9), abs=1e-6)
    tl.tween(0, 0.0, base=TRIAL, start=1.0, duration=0.1)
    with pytest.raises(ytl.ArgumentError):
        tl.tween(0, 0.0, base=TRIAL, duration=-1.0)


def test_times_are_seconds_and_refused():
    tl = new_tl()
    for bad in (-1e-9, float("nan"), float("inf"), 5e9):
        with pytest.raises(ytl.ArgumentError):
            tl.anchor(TRIAL, bad)
        with pytest.raises(ytl.ArgumentError):
            tl.evaluate(bad, 0.0)
    with pytest.raises(ytl.ArgumentError):
        tl.seq(TRIAL).wait(-0.1)
    # a time converts to the nearest ns, and a record gives the exact ns back
    e = tl.find(tl.add(0.1234567894, base=TRIAL))
    assert e.time_ns == 123456789 and e.time == 0.123456789
    tl.anchor(TRIAL, 1.0)
    assert tl.base_time(TRIAL, 1.25) == 0.25
    assert tl.rt_time(TRIAL, 0.25) == 1.25
    assert tl.window(TRIAL, 1.25, s(P)) == s(250_000_000 + P // 2)
    assert tl.next_due() == 1.0 + 0.123456789


def test_errors():
    tl = new_tl(events=4)
    tl.evaluate(onset(1), s(P), 1)
    with pytest.raises(ytl.OrderError):
        tl.evaluate(onset(0), s(P), 0)
    assert "ytl_evaluate" in tl.error
    with pytest.raises(ytl.ArgumentError):
        tl.anchor(0, 1.0)                  # the RT base always runs
    with pytest.raises(ytl.ArgumentError):
        tl.seq(9)
    q = tl.seq(TRIAL)
    q.on("fix").on("grating")
    with pytest.raises(ytl.ArgumentError) as ei:
        q.on(99)
    assert ei.value.index == 2 and q.error == -1
    with pytest.raises(ytl.ArgumentError):
        q.on("fix")                          # sticky
    assert q.cancel() == 2
    with pytest.raises(ytl.ArgumentError):
        tl.add(1.0, "trigger", target=-1)
    for i in range(4):
        tl.add(1.0 + i)
    with pytest.raises(ytl.FullError):
        tl.add(9.0)
    with pytest.raises(ytl.NotFoundError):
        tl.remove(12345)
    with pytest.raises(ytl.ArgumentError):
        new_tl().value("nope")
    with pytest.raises(TypeError):
        tl.seq(TRIAL).to("contrast", 1.0, duration=0.1, durration=0.2)
    tl.close()
    assert not tl.is_open
    with pytest.raises(ytl.ClosedError):
        tl.evaluate(onset(5))
    with pytest.raises(ytl.ClosedError):
        q.on("fix")
    with pytest.raises(ytl.ArgumentError):
        ytl.Timeline(ytl.MAX_CHANNELS + 1)


def test_late_event_peek_rate_pause():
    tl = new_tl()
    tl.anchor(TRIAL, onset(0))
    for i in range(10):
        tl.evaluate(onset(i), s(P), i)
    tl.add(0.05, "mark", base=TRIAL, code=7)                  # behind the reach: late
    ev = tl.evaluate(onset(10), s(P), 10)
    assert ev[0].code == 7 and ev[0].flags == ytl.EV_FIRED | ytl.EV_LATE
    ids = [tl.add(t, "trigger", base=TRIAL, code=1) for t in (0.5, 0.6, 0.7)]
    pk = tl.peek(TRIAL, onset(10), onset(10) + 0.45)   # base times to 0.6167
    assert [e.id for e in pk] == ids[:2] and pk[0].onset_ns == T0 + 500_000_000
    assert tl.peek(ytl.ALL_BASES, onset(10), onset(10)) == []
    tl.rate(TRIAL, onset(11), 1, 2)
    assert tl.get_rate(TRIAL) == (1, 2)
    assert tl.base_time(TRIAL, onset(11) + 0.2) == pytest.approx(s(11 * P) + 0.1, abs=1e-9)
    tl.pause(TRIAL, onset(12))
    assert tl.rt_time(TRIAL, 1.0) is None
    tl.resume(TRIAL, onset(20))
    assert tl.clamped(TRIAL) == 0
    tl.stop(TRIAL)
    assert tl.base_time(TRIAL, onset(21)) is None


def test_prune_horizon_and_callback_sweep():
    tl = ytl.Timeline(0, fields=2, tweens=8, events=32)
    f = tl.field(0.0)
    hits = []
    for k in range(3):
        q = tl.seq(ytl.BASE_RT, now=True)
        q.to(f, float(k), duration=0.01, on_complete=lambda: hits.append(1), overwrite=True)
        for i in range(3):
            tl.evaluate(onset(3 * k + i), s(P), 3 * k + i)
        tl.prune(ytl.BASE_RT)
    assert tl.horizon(ytl.BASE_RT) is not None and tl.horizon(TRIAL) is None
    assert len(hits) == 3


def test_bind_and_default_index():
    class Stim:
        pass
    tl = ytl.Timeline(["contrast"], fields=2)
    pos = tl.field((1.0, 2.0))
    g = Stim()
    tl.bind(g, "contrast", "contrast")
    tl.bind(g, "pos", pos)
    tl.bind(g, "y", pos, 1)
    tl.seq(TRIAL).to("contrast", 1.0, duration=1.0)
    tl.anchor(TRIAL, onset(0))
    tl.evaluate(onset(0), s(P))
    tl.evaluate(onset(30), s(P))
    assert tl.frames == 2
    assert g.contrast == pytest.approx(30 * P / 1e9, abs=1e-6) and g.pos == (1.0, 2.0) and g.y == 2.0
    assert tl.unbind(g) == 3
    e = tl.find(tl.add(0.0, base=ytl.BASE_RT))
    ev = tl.evaluate(onset(31), s(P))
    assert ev[0].id == e.id and ev[0].frame == 2


def test_values_view_and_names():
    tl = ytl.Timeline(["a", "b"], initial={"b": 0.5})
    v = tl.values
    assert v.format == "f" and list(v) == [0.0, 0.5] and v.readonly
    assert tl.channels == ("a", "b") and tl.channel("b") == 1 and tl.lead == 0.5
    assert ytl.Timeline(3, lead="none").lead == 0.0
    with pytest.raises(ytl.ArgumentError):
        ytl.Timeline(["a", "a"])


def test_add_many_and_user_kinds():
    tl = new_tl()
    first = tl.add_many([{"time": 0.1, "kind": 16, "code": 3, "user": 2 ** 63},
                         {"time": 0.2, "kind": "set", "target": "contrast", "value": 0.7, "base": TRIAL}])
    rec = tl.events()
    assert [e.id for e in rec] == [first, first + 1] and rec[0].user == 2 ** 63 and rec[0].kind == 16
    with pytest.raises(ytl.ArgumentError):
        tl.add_many([{"time": 0.3}, {"time": 0.4, "kind": 5}])   # header kinds are refused, all or none
    assert len(tl.events()) == 2


def _played(n_fields=0):
    tl = ytl.Timeline(4, fields=4)
    f = tl.field(1.0) if n_fields else None
    q = tl.seq(TRIAL)
    q.to(0, 1.0, duration=1.0).set(1, 0.5)
    if f is not None:
        q.to(f, 5.0, duration=1.0)
    tl.anchor(TRIAL, onset(0))
    for i in range(20):
        tl.evaluate(onset(i), s(P), i)
    return tl, f


@pytest.mark.parametrize("end", ["del", "close"])
def test_views_outlive_the_timeline(end):
    # A view the caller keeps holds the memory it shows: after the Timeline
    # is gone it reads the last values, stale but valid, and a write through
    # the field view lands in memory no tween reads.
    import gc
    tl, f = _played(n_fields=1)
    v, fv = tl.values, tl.field_values
    assert v.readonly and not fv.readonly
    before, fbefore = list(v), list(fv)
    assert before[1] == 0.5 and fbefore[0] == f.value
    with pytest.raises(BufferError):
        v.obj.extend(b"x")                     # the array under a view never moves
    if end == "close":
        tl.close()
        with pytest.raises(ytl.ClosedError):
            tl.evaluate(onset(20), s(P), 20)
    del tl, f
    gc.collect()
    junk = [bytearray(64) for _ in range(20000)]
    assert list(v) == before and list(fv) == fbefore
    fv[0] = 123.0
    assert fv[0] == 123.0
    del junk
