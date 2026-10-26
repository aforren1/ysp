"""Tests for ysp.box: the binding against ysp/box.h's frames and bytes.

tests/adapt/box_test.c checks the header itself. Here: each family's frames
map to the Event and Pair fields as the header's manual says, a stream
decodes the same whole, byte by byte and in random chunks (with more frames
per call than the binding's arrays hold), random bytes never crash and give
only well-formed events, the encoders give the manual's bytes, and every
bad argument is refused with TypeError or ValueError before the C call.
"""
import math
import os
import random
import re
import struct

import pytest

import ysp.box as box

HERE = os.path.dirname(os.path.abspath(__file__))
EVENT_FIELDS = ("t", "t_ns", "device", "control", "code", "mods", "kind", "type", "flags", "stamp", "unc",
                "x", "y", "value", "aux", "ticks")


def test_version_matches_pyproject():
    text = open(os.path.join(os.path.dirname(HERE), "pyproject.toml")).read()
    assert re.search(r'^version = "([^"]+)"', text, re.M).group(1) == box.__version__
    assert box.__version__ == box.version() or box.__version__.startswith(box.version() + ".")


def test_event_is_a_structseq_in_the_shared_order():
    assert box.Event.n_fields == 16
    e = box.Event(tuple(range(16)))
    assert tuple(getattr(e, f) for f in EVENT_FIELDS) == tuple(range(16))
    assert len(e) == 16 and e[0] == 0 and e[15] == 15


def test_constants_are_the_headers():
    assert (box.XID, box.LINE, box.PHOTO, box.TRIGGERBOX, box.BIOSEMI, box.MMBTS, box.LINES, box.PARALLEL) == \
        tuple(range(1, 9))
    assert (box.KIND_KEYBOARD, box.KIND_BOX, box.KIND_SYNC, box.KIND_USER) == (0, 5, 7, 8)
    assert (box.PRESS, box.RELEASE, box.SAMPLE, box.PROXIMITY) == (1, 2, 3, 4)
    assert (box.REPEAT, box.SYNTHETIC, box.ERASER, box.UNLISTED, box.DEVTICKS) == (1, 2, 4, 8, 16)
    assert (box.TIER_UNKNOWN, box.TIER_1, box.TIER_2, box.TIER_3, box.TIER_SIM) == (0, 1, 2, 3, 4)
    assert box.UNC_MAX == 0.065535
    assert box.SLOW_WRITE == 0.001


# --- frames ---------------------------------------------------------------------


def xid_key(port, key, pressed, ms):
    return b"k" + bytes([port | (0x10 if pressed else 0) | ((key % 8) << 5)]) + struct.pack("<I", ms)


def xid_st2(port, key, pressed, ms):
    return b"o" + bytes([port, key % 8]) + (b"1" if pressed else b"0") + struct.pack("<I", ms) + b"\0"


def photo(level, us):
    return bytes([0xA5, level]) + struct.pack("<I", us)


def photo_sync(us):
    return bytes([0x5A, 0]) + struct.pack("<I", us)


def test_xid_key_frame_fields():
    d = box.Decoder("xid", kind=box.KIND_BOX, device=7)
    r = d.decode(xid_key(1, 3, True, 1234) + xid_key(0, 8, False, 0xFFFFFFFF), 2.5)
    assert r.garbage == 0 and r.outputs == [] and r.text == []
    a, b = r.events
    assert (a.t, a.t_ns, a.device, a.control, a.code, a.kind, a.type, a.flags, a.value, a.ticks) == \
        (2.5, 2500000000, 7, 3, 1, box.KIND_BOX, box.PRESS, box.DEVTICKS, 1.0, 1234)
    assert (a.mods, a.stamp, a.unc, a.x, a.y, a.aux) == (0, 0, 0.0, 0.0, 0.0, 0.0)
    assert (b.control, b.code, b.type, b.value, b.ticks) == (8, 0, box.RELEASE, 0.0, 0xFFFFFFFF)
    assert r.pairs == [box.Pair(1234, 0), box.Pair(0xFFFFFFFF, 0)]
    assert d.frames == 2 and d.garbage == 0


def test_xid_timer_answer_and_stimtracker():
    r = box.decode("xid", b"_e5" + struct.pack("<I", 777) + xid_st2(2, 0, True, 99))
    assert r.pairs == [(777, 1), (99, 0)]
    (e,) = r.events
    assert (e.control, e.code, e.type, e.ticks) == (8, 2, box.PRESS, 99)


def test_xid_broken_frame_is_garbage_and_the_next_frame_is_found():
    # port bits 2 and 3 set: broken (pyxid2 flushes on them)
    r = box.decode("xid", b"k\x0c" + xid_key(1, 1, True, 5))
    assert r.garbage == 2 and len(r.events) == 1 and r.events[0].ticks == 5


def test_line_protocol_fields():
    data = (b"S 100\n"
            b"E 18446744073709551615 4 1\n"
            b"A 300 2 -1.5\n"
            b"Q 9 400\r\n"
            b"I ysp-line 1 teensy41 left\n"
            b"# comment\n"
            b"X nonsense\n")
    d = box.Decoder("line", kind=box.KIND_SYNC, device=3)
    r = d.decode(data, 1.0)
    assert r.pairs == [(100, 0), (2**64 - 1, 0), (300, 0), (400, 9)]
    e, s = r.events
    assert (e.kind, e.device, e.control, e.type, e.value, e.ticks) == (box.KIND_SYNC, 3, 4, box.PRESS, 1.0, 0xFFFFFFFF)
    assert (s.control, s.type, s.value, s.ticks) == (2, box.SAMPLE, -1.5, 300)
    assert r.text == ["ysp-line 1 teensy41 left"]
    assert r.garbage == len(b"X nonsense\n")


def test_line_output_reports_all_come_back():
    # the header returns after each O report; the binding calls it again
    data = b"".join(b"O %d %d\n" % (1000 + k, k) for k in range(50))
    r = box.decode("line", data)
    assert r.outputs == [box.Output(1000 + k, k) for k in range(50)]
    assert len(r.pairs) == 50 and r.garbage == 0


def test_photo_frames():
    r = box.decode("photo", photo(1, 10) + photo_sync(20) + photo(0, 30), kind=box.KIND_SYNC)
    assert [(e.control, e.type, e.value, e.ticks) for e in r.events] == [(1, box.PRESS, 1.0, 10), (1, box.RELEASE, 0.0, 30)]
    assert [p.ticks for p in r.pairs] == [10, 20, 30]


@pytest.mark.parametrize("fam", ["triggerbox", "biosemi", "mmbts", "lines", "parallel"])
def test_output_only_families_count_every_byte_as_garbage(fam):
    d = box.Decoder(fam)
    r = d.decode(bytes(range(256)) * 4)
    assert r.events == [] and r.pairs == [] and r.garbage == 1024 and d.garbage == 1024


def test_two_identity_lines_in_one_call():
    # ysp/box.h v0.3.1: ybox_decode() returns after each identity line
    assert box.decode("line", b"I first\nI second\n").text == ["first", "second"]
    r = box.decode("line", b"I a\nS 5\nI b\nO 7 1\nI\n")
    assert r.text == ["a", "b", ""]
    assert [p.ticks for p in r.pairs] == [5, 7] and len(r.outputs) == 1


# --- streams split anywhere -------------------------------------------------------


def stream(fam, rng, n):
    parts = []
    for k in range(n):
        if fam == "xid":
            c = rng.randrange(3)
            if c == 0:
                parts.append(xid_key(rng.randrange(4), rng.randrange(8), rng.random() < 0.5, rng.getrandbits(32)))
            elif c == 1:
                parts.append(xid_st2(rng.randrange(256), rng.randrange(256), rng.random() < 0.5, rng.getrandbits(32)))
            else:
                parts.append(b"_e5" + struct.pack("<I", rng.getrandbits(32)))
        elif fam == "line":
            t = rng.getrandbits(rng.choice((32, 64)))
            c = rng.randrange(5)
            if c == 0:
                parts.append(b"S %d\n" % t)
            elif c == 1:
                parts.append(b"E %d %d %d\n" % (t, rng.randrange(1, 256), rng.randrange(2)))
            elif c == 2:
                parts.append(b"A %d %d %.3f\n" % (t, rng.randrange(1, 256), rng.uniform(-5, 5)))
            elif c == 3:
                parts.append(b"Q %d %d\n" % (rng.randrange(1, 2**32), t))
            else:
                parts.append(b"O %d %d\n" % (t, rng.randrange(256)))
        else:
            parts.append(photo(rng.randrange(2), rng.getrandbits(32)) if rng.random() < 0.7 else photo_sync(rng.getrandbits(32)))
        if rng.random() < 0.1:
            parts.append(b"\x01\x02\xfe")   # garbage between frames
    return b"".join(parts)


def chunked(fam, data, sizes):
    d = box.Decoder(fam, device=1)
    ev, pr, out, garbage, i = [], [], [], 0, 0
    for n in sizes:
        r = d.decode(data[i:i + n], 1.0)
        ev += r.events
        pr += r.pairs
        out += r.outputs
        garbage += r.garbage
        i += n
    assert i >= len(data)
    return ev, pr, out, garbage, d.frames, d.garbage


@pytest.mark.parametrize("fam", ["xid", "line", "photo"])
def test_stream_decodes_the_same_whole_bytewise_and_chunked(fam):
    rng = random.Random(fam)
    data = stream(fam, rng, 2000)
    whole = chunked(fam, data, [len(data)])
    assert len(whole[0]) + len(whole[1]) > 64 * 4   # many array fills per call
    assert whole[3] == whole[5]                     # garbage per call sums to the total
    assert chunked(fam, data, [1] * len(data)) == whole
    sizes = []
    while sum(sizes) < len(data):
        sizes.append(rng.randrange(1, 40))
    assert chunked(fam, data, sizes) == whole


@pytest.mark.parametrize("fam", ["xid", "line", "photo", "triggerbox", "biosemi", "mmbts", "lines", "parallel"])
def test_random_bytes(fam):
    rng = random.Random("random-" + fam)
    data = bytes(rng.getrandbits(8) for _ in range(200000))
    if fam == "line":   # random lines of random fields, a byte in 50 replaced, so some parse
        words = [b"0", b"1", b"2", b"255", b"256", b"-1", b"1.5", b"1e3", b"4294967296", b"18446744073709551616",
                 b"x", b"", b" "]
        lines = []
        while sum(map(len, lines)) < len(data):
            lines.append(rng.choice(b"SEAQIO#Z").to_bytes(1, "little") + b" " +
                         b" ".join(rng.choice(words) for _ in range(rng.randrange(5))) + rng.choice((b"\n", b"\r\n")))
        data = bytes(rng.getrandbits(8) if rng.random() < 0.02 else b for b in b"".join(lines)[:len(data)])
    sizes = []
    while sum(sizes) < len(data):
        sizes.append(rng.randrange(1, 300))
    ev, pr, out, garbage, frames, total = chunked(fam, data, sizes)
    assert garbage == total <= len(data)
    for e in ev:
        assert e.flags == box.DEVTICKS and e.device == 1 and e.kind == box.KIND_BOX and e.t_ns == 10**9
        assert e.type in (box.PRESS, box.RELEASE, box.SAMPLE)
        assert 0 <= e.ticks < 2**32
        if fam == "xid":
            assert 1 <= e.control <= 255 and e.value in (0.0, 1.0)
        elif fam == "line":
            assert 1 <= e.control <= 255
        else:
            assert e.control == 1
    for p in pr:
        assert 0 <= p.ticks < 2**64 and 0 <= p.probe < 2**32
    if fam in ("xid", "line", "photo"):
        assert ev and pr and garbage   # the bytes reach the frame rules, not only the garbage path
        assert chunked(fam, data, [len(data)])[:4] == (ev, pr, out, garbage)
    else:
        assert ev == pr == out == [] and garbage == len(data)


def test_partial_frame_waits_and_reset_forgets_it():
    d = box.Decoder("xid")
    f = xid_key(1, 2, True, 42)
    assert d.decode(f[:4]).events == [] and d.pending == 4
    assert len(d.decode(f[4:]).events) == 1 and d.pending == 0
    d.decode(f[:3])
    d.reset()
    assert d.pending == 0 and d.frames == 0
    r = d.decode(f[3:])
    assert r.events == [] and r.garbage == 3


def test_read_time_is_exact_ns():
    rng = random.Random(5)
    d = box.Decoder("photo")
    for _ in range(3000):
        ns = rng.randrange(0, 2**23 * 10**9)
        (e,) = d.decode(photo(1, 0), ns / 1e9).events
        assert e.t_ns == ns and e.t == ns / 1e9
    assert d.decode(photo(1, 0), 0).events[0].t_ns == 0
    assert d.decode(photo(1, 0)).events[0].t_ns == 0


def test_data_types_accepted():
    f = photo(1, 7)
    for data in (f, bytearray(f), memoryview(f), list(f), tuple(f)):
        assert box.decode("photo", data).events[0].ticks == 7
    assert box.decode("photo", b"") == box.Decoded([], [], [], [], 0)


# --- families and output bytes ----------------------------------------------------


def test_family_facts():
    assert box.family_name(1) == "xid" and box.family_name("XID") == "xid"
    assert box.caps("xid") == box.CAP_IN | box.CAP_OUT | box.CAP_PULSE | box.CAP_IDENT
    assert box.caps("photo") == box.CAP_IN
    assert box.caps("lines") == box.CAP_OUT | box.CAP_LINES
    assert box.caps("biosemi") == box.CAP_OUT | box.CAP_FIXED
    assert (box.code_max("xid"), box.code_max("lines"), box.code_max("triggerbox"), box.code_max("photo")) == \
        (65535, 3, 255, 0)
    assert box.fixed_pulse("biosemi") == box.fixed_pulse("mmbts") == 0.008 and box.fixed_pulse("xid") == 0.0
    assert box.default_baud("mmbts") == 9600 and box.default_baud("line") == 115200
    assert box.tick("xid") == 0.001 and box.tick("line") == 1e-6
    assert box.slow_write("xid") is True and box.slow_write("line") is False


def test_queries():
    assert box.probe("xid") == b"_e5" and box.probe("line", 42) == b"q 42\n"
    assert box.probe("line", 2**32 - 1) == b"q 4294967295\n"
    assert box.probe("line", 0) == b"" and box.probe("photo") == b""
    assert box.identify("xid") == b"_c1" and box.identify("line") == b"i\n" and box.identify("photo") == b""


def test_encode_bytes():
    assert box.encode("xid", "set", 0x1234) == b"mh\x34\x12"
    assert box.encode("xid", "pulse", 3, 0.002) == b"mp" + struct.pack("<I", 2) + b"mh\x03\x00"
    assert box.encode("xid", "pulse", 3, 0.0001) == b"mp" + struct.pack("<I", 1) + b"mh\x03\x00"
    assert box.encode("xid", box.OP_WIDTH, 0, 0.0) == b"mp\0\0\0\0"
    assert box.encode("line", "set", 5) == b"o 5\n"
    assert box.encode("line", "pulse", 5, 0.0015) == b"p 5 1500\n"
    assert box.encode("line", "pulse", 5, 0.0000015) == b"p 5 2\n"
    assert box.encode("triggerbox", "set", 200) == b"\xc8" and box.encode("triggerbox", "close") == b"\xff"
    assert box.encode("biosemi", "set", 7) == b"\x07" and box.encode("biosemi", "pulse", 7, 0.01) == b""
    assert box.encode("mmbts", "set", 1) == b"\x01" and box.encode("parallel", "set", 255) == b"\xff"
    assert box.encode("lines", "set", 3) == b"" and box.encode("xid", "close") == b""


@pytest.mark.parametrize("args", [
    ("xid", "set", 65536), ("lines", "set", 4), ("triggerbox", "set", 256), ("photo", "set", 0),
    ("line", "pulse", 1, 0.0000004), ("xid", "pulse", 1, (2**32) * 1e-6 + 1),
])
def test_encode_refusals_from_the_header(args):
    with pytest.raises(box.ArgumentError):
        box.encode(*args)


# --- refused arguments --------------------------------------------------------------


@pytest.mark.parametrize("call", [
    lambda: box.Decoder(None), lambda: box.Decoder("nope"), lambda: box.Decoder(0), lambda: box.Decoder(9),
    lambda: box.Decoder(-1), lambda: box.Decoder(2**70), lambda: box.Decoder(1.0), lambda: box.Decoder(b"xid"),
    lambda: box.Decoder("x" * 1000), lambda: box.Decoder("xid", kind=256), lambda: box.Decoder("xid", kind=-1),
    lambda: box.Decoder("xid", kind=1.5), lambda: box.Decoder("xid", kind=True), lambda: box.Decoder("xid", device=2**32),
    lambda: box.Decoder("xid", device="a"), lambda: box.Decoder(), lambda: box.Decoder("xid", 5, 1, 2),
    lambda: box.decode("xid", "text"), lambda: box.decode("xid", 5), lambda: box.decode("xid", [256]),
    lambda: box.decode("xid", [-1]), lambda: box.decode("xid", None), lambda: box.decode("xid", b"", -1.0),
    lambda: box.decode("xid", b"", math.nan), lambda: box.decode("xid", b"", math.inf), lambda: box.decode("xid", b"", "1"),
    lambda: box.decode("xid", b"", 1e10), lambda: box.decode("xid", b"", kind=300),
    lambda: box.encode("xid", "hold", 1), lambda: box.encode("xid", 0, 1), lambda: box.encode("xid", 5, 1),
    lambda: box.encode("xid", None, 1), lambda: box.encode("xid", "set", -1), lambda: box.encode("xid", "set", 2**32),
    lambda: box.encode("xid", "set", 1.0), lambda: box.encode("xid", "pulse", 1, -0.001),
    lambda: box.encode("xid", "pulse", 1, math.nan), lambda: box.encode("xid", "pulse", 1, math.inf),
    lambda: box.probe("line", -1), lambda: box.probe("line", 2**32), lambda: box.probe("line", "1"),
    lambda: box.caps(0), lambda: box.tick("none"), lambda: box.code_max(None), lambda: box.fixed_pulse([]),
    lambda: box.default_baud(99), lambda: box.slow_write(-5), lambda: box.identify(""), lambda: box.family_name(3.0),
])
def test_bad_arguments_are_refused(call):
    with pytest.raises((TypeError, ValueError)):
        call()


def test_random_arguments_never_crash():
    rng = random.Random(9)
    pool = [None, 0, 1, 2, 8, 9, -1, 2**31, 2**64, -2**63, 0.5, math.nan, math.inf, -math.inf, "", "xid", "LINE",
            "set", "pulse", "\x00", "é", b"", b"k", bytearray(b"\x00" * 9), [1, 2], [300], (), {}, object(), True]
    funcs = [box.decode, box.family_name, box.caps, box.code_max, box.fixed_pulse, box.default_baud, box.tick,
             box.slow_write, box.probe, box.identify, box.encode, box.Decoder]
    for _ in range(20000):
        f = rng.choice(funcs)
        args = [rng.choice(pool) for _ in range(rng.randrange(0, 6))]
        try:
            f(*args)
        except (TypeError, ValueError, OverflowError):
            pass
    d = box.Decoder("line")
    for _ in range(5000):
        try:
            d.decode(*[rng.choice(pool) for _ in range(rng.randrange(0, 4))])
        except (TypeError, ValueError, OverflowError):
            pass
