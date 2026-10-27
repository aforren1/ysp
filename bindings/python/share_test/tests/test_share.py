"""The tables of ysp.audio and ysp.timeline, seen from another extension.

ysp_share_test is a consumer built the way ysp-vis will be: audio.h and
timeline.h as declarations, the shims of bindings/share/ forwarding each call
to the owners' implementations. Here: the import checks (a table it cannot
use is refused with a clear message), objects made in Python and driven from
C through the shims, and the lifetime rule (a held object outlives its last
Python name, and its owner refuses to close it).
"""
import gc
import struct
import sys
import wave

import pytest

import ysp.audio as yau
import ysp.timeline as ytl
import ysp_share_test as c

P = 16666667            # a 60 Hz period, ns
T0 = 10_000_000_000     # the first onset, ns


def test_capsules():
    assert type(yau._C_API).__name__ == "PyCapsule"
    assert type(ytl._C_API).__name__ == "PyCapsule"


@pytest.mark.parametrize("what, words", [
    ("a_abi", "ABI"), ("a_size", "older"), ("a_version", "0.0.1"),
    ("a_sizeof", "sizeof(yau_wav)"), ("a_host", "host"),
    ("t_channels", "YTL_MAX_CHANNELS"), ("t_sizeof", "sizeof(ytl_timeline)"),
])
def test_refuses_a_table_it_cannot_use(what, words):
    rc, err = c.check_table(what)
    assert rc == -1 and words in err, err


def test_accepts_the_real_tables():
    assert c.check_table("a_none") == (0, "")
    assert c.check_table("t_none") == (0, "")


def test_wrong_objects_are_refused():
    with pytest.raises(TypeError, match="an Audio is needed"):
        c.hold_audio(object())
    with pytest.raises(TypeError, match="a Timeline is needed"):
        c.hold_timeline(yau)
    tl = ytl.Timeline(2)
    tl.close()
    with pytest.raises(TypeError, match="closed"):
        c.hold_timeline(tl)


def test_timeline_built_in_python_evaluated_in_c():
    tl = ytl.Timeline(["fix", "grating"])
    q = tl.seq(1)
    q.on("fix").wait(0.5).off("fix").on("grating")
    i = c.hold_timeline(tl)
    try:
        assert tl.pins == 1
        assert c.tl_anchor(i, 1, T0, 0) == 0
        n, v, bt, lead = c.tl_evaluate(i, T0, P, 0, 2)
        assert n == 1 and v == [1.0, 0.0] and bt == 0 and lead == 0.5
        n, v, bt, _ = c.tl_evaluate(i, T0 + 30 * P, P, 30, 2)
        assert n == 2 and v == [0.0, 1.0] and bt == 30 * P
        # The same handle: Python sees what C evaluated.
        assert list(tl.values) == [0.0, 1.0]
        assert tl.frames == 31          # the default index moved on, as evaluate() does
    finally:
        c.drop_timeline(i)
    assert tl.pins == 0


def test_timeline_callbacks_and_binds_run_through_the_table():
    tl = ytl.Timeline(["contrast"])
    hits = []
    f = tl.field(0.0)
    q = tl.seq(1)
    q.to(f, 1.0, duration=0.1, on_complete=lambda: hits.append("done"))

    class Stim:
        contrast = None
    s = Stim()
    tl.bind(s, "contrast", f)
    i = c.hold_timeline(tl)
    try:
        c.tl_anchor(i, 1, T0, 0)
        for k in range(10):
            c.tl_evaluate(i, T0 + k * P, P, k, 1)
        assert hits == ["done"]
        assert s.contrast == pytest.approx(1.0)
        # A callback that raises comes back as that exception.
        q2 = tl.seq(1)
        q2.at(0.3)

        def boom():
            raise KeyError("from a tween")
        q2.to(f, 0.0, duration=0.05, on_start=boom)
        with pytest.raises(KeyError, match="from a tween"):
            for k in range(10, 30):
                c.tl_evaluate(i, T0 + k * P, P, k, 1)
    finally:
        c.drop_timeline(i)


def test_timeline_lifetime():
    tl = ytl.Timeline(["a"])
    tl.seq(1).on("a")
    i = c.hold_timeline(tl)
    with pytest.raises(ytl.BusyError):
        tl.close()
    del tl
    gc.collect()
    c.tl_anchor(i, 1, T0, 0)               # the consumer's reference kept it
    n, v, _, _ = c.tl_evaluate(i, T0, P, 0, 1)
    assert n == 1 and v == [1.0]
    c.drop_timeline(i)                     # the last reference: the Timeline goes now
    with pytest.raises(ValueError):
        c.tl_anchor(i, 1, T0, 0)


@pytest.fixture
def audio():
    a = yau.Audio(backend="null")
    yield a
    if a.pins == 0:
        a.close()


def test_audio_play_through_the_shims(audio):
    i = c.hold_audio(audio)
    try:
        info = c.au_info(i)
        assert info["open"] and info["rate"] == 48000 and info["version"] == yau.__version__
        assert "backend=Null" in info["describe"]
        t = yau.now_ns() + 100_000_000
        sid = c.au_play_at(i, t)
        r = c.au_wait(i, sid)
        assert r["rc"] == 0 and r["target"] == t and r["buffer_id"] == 77
        assert r["tier"] == yau.TIER_SIM and abs(r["onset"] - t) < 1_000_000
        # The record is the owner's: Python reads it too.
        assert audio.result(sid).onset_ns == r["onset"]
    finally:
        c.drop_audio(i)


def test_audio_lifetime():
    a = yau.Audio(backend="null")
    i = c.hold_audio(a)
    assert a.pins == 1
    with pytest.raises(yau.BusyError, match="other module"):
        a.close()
    del a                                  # the consumer holds the only reference
    gc.collect()
    sid = c.au_play_at(i, 0)               # the device is still open
    assert c.au_wait(i, sid)["rc"] == 0
    c.drop_audio(i)                        # the last reference: the device closes
    with pytest.raises(ValueError):
        c.au_info(i)


def test_soundtrack_on_a_python_audio(audio, tmp_path):
    p = tmp_path / "track.wav"
    with wave.open(str(p), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(struct.pack("<4800h", *([1000] * 4800)))
    i = c.hold_audio(audio)
    try:
        t = yau.now_ns() + 150_000_000
        r = c.au_soundtrack(i, str(p), t)
        fed, rc2, smp, origin, started = r["stream"]
        assert r["rc"] == 0 and r["buffer_id"] == 5 and fed == 4800
        assert started and rc2 == 0 and smp == 0     # sample 0 is on the onset's frame
        assert origin == r["start_frame"]
    finally:
        c.drop_audio(i)


def test_forwarded_call_cost():
    """Prints ns per call; the numbers in the docs come from the guard's time mode."""
    tl = ytl.Timeline(2)
    i = c.hold_timeline(tl)
    try:
        b = c.bench(i, 2_000_000, 5)
    finally:
        c.drop_timeline(i)
    print("\nytl_value: shim %.2f ns, table %.2f ns, inline read %.2f ns" % (b["shim"], b["table"], b["inline"]))
    assert b["shim"] < 1000
