"""Tests for ysp.audio on miniaudio's null device: no sound hardware.

tests/adapt/audio_test.c checks the header itself (plan, fit, mixer, records)
on a scripted device. Here: the binding drives it as the C API does. The
open and the device list, synthesis against its formulas, buffers from
Python data, the records of a play (seconds and exact ns, tier, flags),
cancel, stop, gain, wait without the GIL, the refusals, streams fed from
Python (also from another thread), WAV files streamed and loaded, the
capture of the null input, and close.
"""
import array
import math
import os
import re
import struct
import threading
import time
import wave
from fractions import Fraction

import pytest

import ysp.audio as yau

HERE = os.path.dirname(os.path.abspath(__file__))
# miniaudio's null device runs on the real clock and its fit is of callback
# times, which a loaded machine delays: the plan is the nearest frame by the
# fit of the moment, and a refit can move it by more than a frame. These
# tests check the binding's plumbing, so they allow 5 ms (240 frames).
TOL_NS = 5_000_000
TOL_FRAMES = 240


def test_version_matches_pyproject():
    with open(os.path.join(HERE, "..", "pyproject.toml")) as f:
        v = re.search(r'^version = "([^"]+)"', f.read(), re.M).group(1)
    assert yau.__version__ == v


def test_constants_and_clock():
    assert yau.TIER_SIM == 4 and yau.TIER_2 == 2 and yau.LATE == 0x2 and yau.UNCONFIRMED == 0x40
    a, b = yau.now(), yau.now_ns()
    assert abs(a * 1e9 - b) < 1e8
    assert yau.db(-20) == pytest.approx(0.1, rel=1e-6)


def test_devices():
    d = yau.devices(backend="null")
    assert len(d) == 1 and d[0]["default"] and "NULL" in d[0]["name"]
    assert isinstance(yau.devices(), list)
    assert isinstance(yau.devices(backend="null", capture=True), list)
    with pytest.raises(yau.ArgumentError):
        yau.devices(backend="nosuch")


@pytest.fixture(scope="module")
def au():
    a = yau.Audio(backend="null")
    yield a
    a.close()


def test_open_caps(au):
    assert au.is_open and au.error == ""
    c = au.caps()
    assert c["backend"] == "null" and c["rate"] == 48000 and c["channels"] == 2
    assert c["period_frames"] == 480 and c["period"] == pytest.approx(0.01)
    assert c["pos_source"] == "callback" and c["os_latency"] is None
    assert "backend=Null" in au.describe()
    # The lead is the time of the frame after the next block, minus now: at
    # most two periods, and below 0 while the next callback is more than a
    # period late on the fit. The null device's callbacks wait in Sleep(10)
    # steps, 15 ms on Windows' default timer tick: under load 1 to 5 % of
    # the leads read just after open were negative (docs/audio.md, "The
    # lead on the null device"). A broken clock or fit is far off.
    assert c["lead_ns"] != 0 and c["lead"] == c["lead_ns"] / 1e9
    assert -0.5 < au.lead <= 2 * c["period"] + TOL_NS / 1e9 and au.lead_ns != 0


def test_strict_open_refuses():
    with pytest.raises(yau.Error):
        yau.Audio(device="no such device name")
    with pytest.raises(yau.ArgumentError):
        yau.Audio(backend="null", channels=0)
    with pytest.raises(yau.ArgumentError):
        yau.Audio(backend="null", period=-1)


def test_tone(au):
    b = au.tone(1000, 0.1, 0.25, ramp=0.005, id=9)
    assert (b.frames, b.channels, b.id) == (4800, 1, 9) and b.duration == pytest.approx(0.1)
    x = b.samples()
    assert x.format == "f" and x.shape == (4800, 1)
    for k in (1000, 2000, 2345):               # past the ramp: the formula
        assert x[k, 0] == pytest.approx(0.25 * math.sin(2 * math.pi * 1000 * k / 48000), abs=1e-6)
    assert abs(x[0, 0]) < 1e-6                 # the raised-cosine ramp starts at 0
    s2 = au.tone(500, 0.01, 0.1, channels=2).samples()
    assert s2.shape == (480, 2) and s2[100, 0] == s2[100, 1]


def test_noise(au):
    a = au.noise(0.05, 0.05, seed=7).samples().tobytes()
    b = au.noise(0.05, 0.05, seed=7).samples().tobytes()
    c = au.noise(0.05, 0.05, seed=8).samples().tobytes()
    assert a == b and a != c
    x = array.array("f", a)
    rms = math.sqrt(sum(v * v for v in x) / len(x))
    assert rms == pytest.approx(0.05, rel=0.1)
    with pytest.raises(yau.Error, match="rms"):
        au.noise(1.0, 0.4, seed=1)             # Gaussian noise that would clip
    u = array.array("f", au.noise(0.05, 0.4, dist="uniform").samples().tobytes())
    assert max(abs(v) for v in u) <= 1.0


def test_click(au):
    x = au.click(0.5).samples()
    assert x.shape == (1, 1) and x[0, 0] == 0.5
    assert au.click(-0.2, 0.001).frames == 48


def test_buffer_from_data(au):
    b = au.buffer(array.array("f", [0.1, -0.2, 0.3]))
    assert (b.frames, b.channels) == (3, 1)
    assert list(b.samples().tobytes()) == list(array.array("f", [0.1, -0.2, 0.3]).tobytes())
    b = au.buffer([0.5, 0.25, 0.0, -0.25], channels=2)
    assert (b.frames, b.channels) == (2, 2)
    b = au.buffer(array.array("d", [0.1] * 10))          # float64, narrowed
    assert b.samples()[3, 0] == pytest.approx(0.1)
    with pytest.raises(TypeError):
        au.buffer(b"\x00\x00\x00\x00")
    with pytest.raises(yau.ArgumentError, match="finite"):
        au.buffer([0.0, float("nan")])
    with pytest.raises(yau.ArgumentError, match="whole frames"):
        au.buffer([0.0, 0.1, 0.2], channels=2)
    with pytest.raises(yau.ArgumentError):
        au.buffer([])


def test_buffer_from_numpy(au):
    np = pytest.importorskip("numpy")
    x = np.zeros((480, 2), np.float32)
    x[:, 1] = 0.5
    b = au.buffer(x)
    assert (b.frames, b.channels) == (480, 2)
    y = np.asarray(b.samples())
    assert y.shape == (480, 2) and (y == x).all()
    b = au.buffer(np.linspace(-1, 1, 101))                 # float64, 1-D
    assert b.frames == 101 and np.asarray(b.samples())[50, 0] == 0
    with pytest.raises(yau.ArgumentError):
        au.buffer(x, channels=1)


def check_record(r, t_s):
    assert not r.pending and r.tier == yau.TIER_SIM
    assert abs(Fraction(r.target_ns) - Fraction(t_s) * 10 ** 9) <= Fraction(1, 2)   # the nearest ns
    assert r.target == pytest.approx(t_s)
    assert r.residual_ns == r.onset_ns - r.target_ns
    assert r.onset == pytest.approx(r.onset_ns / 1e9) and r.residual == pytest.approx(r.residual_ns / 1e9)
    assert r.start_frame > 0 and r.flags & yau.UNCONFIRMED   # the null device reports no positions


def test_play_at_and_wait(au):
    b = au.tone(1000, 0.02, 0.03, id=4)
    t = yau.now() + 0.15
    sid = au.play_at(b, t)
    assert sid > 0
    r0 = au.result(sid)
    assert r0.pending and r0.flags & yau.PENDING
    r = au.wait(sid)
    check_record(r, t)
    assert r.buffer_id == 4 and abs(r.residual_ns) <= TOL_NS
    assert not r.flags & yau.LATE
    f = au.frame_at(r.onset)
    assert abs(f - r.start_frame) <= TOL_FRAMES
    s, ns = au.time_of(f)
    assert abs(ns - r.onset_ns) <= TOL_NS and s == pytest.approx(ns / 1e9)


def test_play_options(au):
    b = au.tone(440, 0.05, 0.03, channels=2)
    t = yau.now() + 0.12
    sid = au.play(b, t, db=-6, channels=[0], offset=480, frames=960)
    r = au.wait(sid, timeout=2.0)
    check_record(r, t)
    assert r.sample == 480
    sid = au.play(b)                                        # as soon as it can
    r = au.wait(sid)
    assert r.target_ns == 0 and not r.pending


def test_late(au):
    b = au.click(0.1)
    sid = au.play_at(b, yau.now() - 0.1)                    # already past: LATE
    r = au.wait(sid)
    assert r.flags & yau.LATE and r.residual_ns > 0


def test_cancel_stop_gain(au):
    b = au.tone(300, 0.2, 0.03)
    sid = au.play_at(b, yau.now() + 1.0)
    au.cancel(sid)
    r = au.wait(sid)
    assert r.flags & yau.CANCELED and r.start_frame == -1
    sid = au.play(b, yau.now() + 0.1, loops=-1)             # until stopped
    au.gain(sid, -12, at=yau.now() + 0.15, ramp=0.01)
    au.stop(sid, at=yau.now() + 0.3, ramp=0.02)
    au.wait(sid)
    deadline = time.time() + 3
    while time.time() < deadline:
        au.update()
        r = au.result(sid)
        if r.end_frame:
            break
        time.sleep(0.01)
    assert r.flags & yau.STOPPED and r.end_frame > r.start_frame


def test_refusals(au):
    b = au.click(0.1)
    for bad in (-1.0, float("nan")):
        with pytest.raises(yau.ArgumentError, match="negative or NaN"):
            au.play_at(b, bad)
        with pytest.raises(yau.ArgumentError):
            au.stop(0, at=bad)
    with pytest.raises(TypeError):
        au.play_at("not a buffer", 0.0)
    with pytest.raises(yau.NotFoundError):
        au.result(10 ** 9)
    sid = au.play_at(b, yau.now() + 5.0)
    with pytest.raises(yau.TimeoutError):
        au.wait(sid, timeout=0.02)
    au.cancel(sid)
    au.wait(sid)


def test_wait_forever(au):
    """math.inf is INT64_MAX ns: ysp/audio.h 0.7.2 saturates now + timeout (the
    sum overflowed in 0.7.1, and a wait without end timed out at once)."""
    b = au.click(0.1)
    for timeout in (math.inf, 4.6e9):                        # forever, and 2^62 ns less a little
        t = yau.now() + 0.1
        sid = au.play_at(b, t)
        assert au.result(sid).pending
        r = au.wait(sid, timeout=timeout)
        check_record(r, t)


def test_wait_releases_the_gil(au):
    b = au.click(0.1)
    sid = au.play_at(b, yau.now() + 0.3)
    n = [0]
    stop = threading.Event()
    errors = []

    def spin():
        while not stop.is_set():
            n[0] += 1
        try:
            pass
        except Exception as e:  # pragma: no cover
            errors.append(e)

    def poke():
        time.sleep(0.05)
        try:
            au.update()
        except yau.BusyError as e:
            errors.append(e)

    th = threading.Thread(target=spin)
    pk = threading.Thread(target=poke)
    th.start()
    pk.start()
    c0 = n[0]
    au.wait(sid)
    stop.set()
    th.join()
    pk.join()
    assert n[0] - c0 > 1000                 # the other thread ran meanwhile
    assert len(errors) == 1 and "wait()" in str(errors[0])   # one frame thread


def test_buffers_belong_to_their_audio(au):
    other = yau.Audio(backend="null")
    try:
        with pytest.raises(yau.ArgumentError, match="another Audio"):
            au.play_at(other.click(0.1), 0.0)
        b = other.click(0.1)
        other.arena_reset()
        with pytest.raises(yau.ArgumentError, match="arena_reset"):
            other.play_at(b, 0.0)
        with pytest.raises(yau.ArgumentError, match="arena_reset"):
            b.samples()
        long = other.tone(200, 1.0, 0.01)
        sid = other.play_at(long, 0.0)
        with pytest.raises(yau.BusyError):
            other.arena_reset()
        other.cancel(sid)
        other.wait(sid)
    finally:
        other.close()


def test_stream(au):
    st = au.stream(1, ring=0.5, id=3)
    assert st.info()["state"] == "idle"
    assert st.write([0.01] * 4800) == 4800
    t = yau.now() + 0.15
    sid = st.play(t)
    r = au.wait(sid)
    check_record(r, t)
    assert r.buffer_id == 3 and r.sample == 0
    i = st.info()
    assert i["started"] and i["origin"] == r.start_frame
    assert abs(st.sample_at(r.onset)) <= TOL_FRAMES
    s, ns = st.time_of(480)
    assert ns > r.onset_ns
    # A producer thread fills it while it plays; end() ends the play.
    done = threading.Event()

    def producer():
        chunk = array.array("f", [0.01] * 960)
        for _ in range(10):
            while st.write(chunk) == 0:
                time.sleep(0.002)
        st.end()
        done.set()

    th = threading.Thread(target=producer)
    th.start()
    th.join()
    assert done.is_set()
    deadline = time.time() + 3
    while time.time() < deadline and st.info()["state"] != "ended":
        au.update()
        time.sleep(0.01)
    assert st.info()["state"] == "ended"
    st.reset(100)
    assert st.info()["first"] == 100
    st.close()
    with pytest.raises(yau.ClosedError):
        st.write([0.0])
    with pytest.raises(TypeError):
        au.stream(1).write(b"abcd")


def write_wav(path, rate=48000, n=4800, ch=1, value=1000):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % (n * ch), *([value] * (n * ch))))


def test_wav_stream_and_load(au, tmp_path):
    p = tmp_path / "a.wav"
    write_wav(p)
    info = yau.wav_info(str(p))
    assert info["rate"] == 48000 and info["frames"] == 4800 and info["format"] == yau.WAV_S16
    w = au.wav(p, id=11)
    assert w.file["frames"] == 4800 and w.wants
    assert w.feed() == 4800
    t = yau.now() + 0.15
    sid = w.play(t)
    r = au.wait(sid)
    check_record(r, t)
    assert r.buffer_id == 11
    w.close()
    w2 = au.wav(data=p.read_bytes(), loops=1)
    assert w2.feed() == 9600                   # two passes, the count goes on
    w2.close()
    b = au.load_wav(p, id=2)
    assert (b.frames, b.channels, b.id) == (4800, 1, 2)
    assert b.samples()[10, 0] == pytest.approx(1000 / 32768)
    q = tmp_path / "b.wav"
    write_wav(q, rate=44100)
    with pytest.raises(yau.Error, match="44100"):
        au.wav(q)
    with pytest.raises(yau.Error):
        au.load_wav(q)
    with pytest.raises(yau.ArgumentError):
        au.wav()


def test_capture_null():
    with yau.Capture(backend="null", channels=1, ring=0.5) as cap:
        info = cap.info()
        assert info["tier"] == yau.TIER_SIM and info["rate"] == 48000 and info["channels"] == 1
        time.sleep(0.1)
        x, first = cap.read()
        assert x.format == "f" and x.shape[1] == 1 and x.shape[0] > 0 and first >= 0
        assert all(v == 0.0 for v in x.tobytes())
        s, ns = cap.time_of(first)
        assert abs(cap.frame_at(s) - first) <= 1
        assert cap.time_of(first + 0.5)[1] > ns
        assert "Null" in cap.describe() or "null" in cap.describe().lower()
    with pytest.raises(yau.ClosedError):
        cap.read()


def test_close():
    a = yau.Audio(backend="null")
    b = a.tone(500, 0.5, 0.01)
    sid = a.play_at(b, yau.now() + 0.5)
    st = a.stream(1)
    a.close()
    a.close()
    assert not a.is_open
    with pytest.raises(yau.ClosedError):
        a.play_at(b, 0.0)
    with pytest.raises(yau.ClosedError):
        a.result(sid)
    with pytest.raises(yau.ClosedError):
        st.write([0.0])
    st.close()
    with yau.Audio(backend="null") as a2:
        assert a2.is_open
    assert not a2.is_open
