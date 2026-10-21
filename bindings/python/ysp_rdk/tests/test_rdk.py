"""Tests for ysp.rdk: the binding regenerates ysp/yrdk.h's fields bit for bit.

tests/adapt/rdk_test.c checks the header itself. Here: the 8 golden
digests (the same numbers as the C test and tests/compare/rdk_ref.py), a
trial logged by C (c_trial.bin, from make_c_trial.c) replayed into equal
arrays, frozen noise across coherence levels, snapshots, the outputs and
the refusals, and the seconds API (0.2.0.1): every time is seconds, and a
time converts to the same ns as the C log has. CI runs this file on Linux,
macOS arm64 and Windows; a digest that differs on one of them is a bug,
not a tolerance.
"""
import os
import re
import struct

import numpy as np
import pytest

import ysp.rdk as yrdk

HERE = os.path.dirname(os.path.abspath(__file__))
# The schedules are integer ns, as the C test's; the binding takes seconds,
# so each time crosses as ns / 1e9 (the double nearest), which converts back
# to the same ns.
P = 16666667
T0 = 1000000000


def s(ns):
    return ns / 1e9


def test_version_matches_pyproject():
    text = open(os.path.join(os.path.dirname(HERE), "pyproject.toml")).read()
    assert re.search(r'^version = "([^"]+)"', text, re.M).group(1) == yrdk.__version__
    # the fourth number counts changes to the binding alone
    assert yrdk.__version__.startswith(yrdk.version() + ".")


def test_exact_pieces():
    # Random123's known answers for philox4x32 10
    assert yrdk.philox(0, (0, 0, 0, 0)) == (0x6627E8D5, 0xE169C58D, 0xBC57AC4C, 0x9B00DBD8)
    assert yrdk.philox(0x299F31D0A4093822, (0x243F6A88, 0x85A308D3, 0x13198A2E, 0x03707344)) == (
        0xD16CFE09, 0x94FDCCEB, 0x5001E420, 0x24126EA1)
    assert yrdk.angle(90) == 0x40000000
    assert yrdk.angle(-90) == 0xC0000000
    assert yrdk.angle(1) == 11930465
    assert yrdk.sincos(0x40000000) == (1 << 30, 0)


# --- the golden digests ---------------------------------------------------------

VARY_C, VARY_DIR, VARY_SPEED = 1, 2, 4

# (name, desc, seed, drop, stall, vary, updates, digest): rdk_test.c's table
GOLDEN = [
    ("same_dir_circle", dict(w=10, count=200, coherence=0.3125, direction=30, speed=7.5),
     1, 50, -1, 0, 240, 0x46C7ADD13BEA5555),
    ("wn", dict(algorithm="WN", w=8, density=2.5, coherence=0.25, direction=200, speed=6),
     2, -1, -1, 0, 240, 0xAB0C3CE3AD1B3C70),
    ("mn_frames", dict(algorithm="MN", w=12, count=100, coherence=0.5, direction=45, speed=10,
                       clock="frames", frame_period=s(P), lifetime_frames=6),
     3, 70, -1, 0, 300, 0x890DE1AB1D616C84),
    ("ll", dict(algorithm="LL", w=9, count=90, coherence=0.6875, direction=-30, speed=4),
     4, -1, -1, 0, 300, 0x9FE3C4CA6FE8ED6D),
    ("bm_rect", dict(algorithm="BM", aperture="rect", w=12, h=5, count=150, coherence=0.4375, direction=100,
                     speed=9, stream=7),
     5, -1, 120, 0, 240, 0x34079EB0FE076C8F),
    ("replot_life", dict(signal="different", noise="position", edge="replot", w=10, count=120, lifetime=0.25,
                         coherence=0.5, speed=12),
     6, -1, -1, VARY_C, 240, 0xD4F6228848132C60),
    ("walk_rect_life", dict(select="bernoulli", noise="walk", aperture="rect", w=7, h=11, edge="replot",
                            lifetime_frames=5, frame_period=s(P), count=80, coherence=0.375, speed=5),
     7, -1, -1, VARY_DIR | VARY_SPEED, 300, 0xEB8733EF39ECA7DA),
    ("stall_circle", dict(signal="different", w=6, count=64, lifetime=1.3, coherence=0.5, direction=77.25,
                          speed=3.25),
     0xDEADBEEFCAFEF00D, 40, 60, 0, 200, 0x1749463C5A7DD3E8),
]


def schedule(k, drop, stall):
    t = T0 + k * P + ((k * 7919) % 101 - 50) * 1000
    if drop >= 0 and k >= drop:
        t += P
    if stall >= 0 and k >= stall:
        t += 37000000000
    return t


def params(k, d, vary):
    c, dr, sp = d.get("coherence", 0.0), d.get("direction", 0.0), d.get("speed", 0.0)
    if vary & VARY_C:
        c = (k % 17) / 16.0
    if vary & VARY_DIR:
        dr = (k % 8) * 45 + 0.5
    if vary & VARY_SPEED and 100 <= k < 140:
        sp = -sp
    return c, dr, sp


def golden_steps(g):
    _, d, _, drop, stall, vary, updates, _ = g
    return [(s(schedule(k, drop, stall)),) + params(k, d, vary) for k in range(1, updates + 1)]


@pytest.mark.parametrize("g", GOLDEN, ids=[g[0] for g in GOLDEN])
def test_golden_digest_by_updates(g):
    name, d, seed, _, _, _, _, digest = g
    f = yrdk.Field(**d)
    f.start(seed, s(T0))
    for t, c, dr, sp in golden_steps(g):
        f.coherence, f.direction, f.speed = c, dr, sp
        f.update(t)
    assert f.digest() == digest, "%s: %016x" % (name, f.digest())


@pytest.mark.parametrize("g", GOLDEN, ids=[g[0] for g in GOLDEN])
def test_golden_digest_by_replay(g):
    name, d, seed, _, _, _, _, digest = g
    f = yrdk.Field(**d)
    f.replay(seed, s(T0), golden_steps(g))
    assert f.digest() == digest
    xy, _, _ = f.trajectory(seed, s(T0), golden_steps(g))
    assert f.digest() == digest
    assert xy.shape == (len(golden_steps(g)), f.n, 2)


# --- a trial logged by C --------------------------------------------------------

C_DESC = dict(algorithm="LL", aperture="rect", w=8, h=6, count=60, lifetime=0.3, coherence=0.5, speed=4)


def load_c_trial():
    raw = open(os.path.join(HERE, "c_trial.bin"), "rb").read()
    assert raw[:4] == b"RDKT"
    version, seed, t0, k, n = struct.unpack_from("<IQqII", raw, 4)
    assert version == 1
    off = 4 + 28
    step_t = np.dtype([("t", "<i8"), ("coherence", "<f4"), ("direction", "<f4"), ("speed", "<f4"), ("set", "<i4")])
    steps = np.frombuffer(raw, step_t, k, off)
    off += k * step_t.itemsize
    xy = np.frombuffer(raw, "<f4", k * n * 2, off).reshape(k, n, 2)
    off += xy.nbytes
    dirs = np.frombuffer(raw, "<f4", k * n, off).reshape(k, n)
    off += dirs.nbytes
    sig = np.frombuffer(raw, "u1", k * n, off).reshape(k, n)
    off += sig.nbytes
    (digest,) = struct.unpack_from("<Q", raw, off)
    return seed, t0, steps, xy, dirs, sig, digest


def test_c_trial_replays_to_identical_arrays():
    seed, t0, steps, xy, dirs, sig, digest = load_c_trial()
    f = yrdk.Field(**C_DESC)
    rows = [(s(int(r["t"])), float(r["coherence"]), float(r["direction"]), float(r["speed"])) for r in steps]
    gxy, gdir, gsig = f.trajectory(seed, s(t0), rows)
    assert f.digest() == digest
    # bit for bit: compare the float32 words as integers
    assert np.array_equal(gxy.view(np.uint32), xy.view(np.uint32))
    assert np.array_equal(gdir.view(np.uint32), dirs.view(np.uint32))
    assert np.array_equal(gsig, sig)
    # a structured array's rows work as steps: seconds and the exact ns
    # beside them, as Field.last gives them
    log_t = np.dtype([("t", "<f8"), ("coherence", "<f4"), ("direction", "<f4"), ("speed", "<f4"),
                      ("set", "<i4"), ("ns", "<i8")])
    srows = np.array([(s(int(r["t"])), r["coherence"], r["direction"], r["speed"], r["set"], r["t"])
                      for r in steps], log_t)
    f.replay(seed, s(t0), srows)
    assert f.digest() == digest
    # the log of a live run (Field.last) replays to the same digest, and its
    # ns column is the C log's t
    g = yrdk.Field(**C_DESC)
    g.start(seed, s(t0))
    log = []
    for r in rows:
        g.coherence, g.direction, g.speed = r[1], r[2], r[3]
        g.update(r[0])
        log.append(g.last)
    assert g.digest() == digest
    assert np.array_equal(g.xy().view(np.uint32), xy[-1].view(np.uint32))
    assert [x.ns for x in log] == [int(r["t"]) for r in steps]
    assert all(x.t == x.ns / 1e9 for x in log)
    h = yrdk.Field(**C_DESC)
    h.replay(seed, s(t0), log)
    assert h.digest() == digest
    # the log without its ns column (seconds only) replays the same
    h.replay(seed, s(t0), [tuple(x)[:5] for x in log])
    assert h.digest() == digest


# --- frozen noise ---------------------------------------------------------------

@pytest.mark.parametrize("noise", ["position", "walk", "direction"])
def test_frozen_noise_across_coherence(noise):
    a = yrdk.Field(w=10, count=500, signal="different", noise=noise, coherence=0.2, speed=3, direction=10)
    b = yrdk.Field(w=10, count=500, signal="different", noise=noise, coherence=0.55, speed=3, direction=10)
    steps = [(s(T0 + k * P), 0.0, 10.0, 3.0) for k in range(1, 201)]
    sa = [(t, 0.2, d, s) for t, _, d, s in steps]
    sb = [(t, 0.55, d, s) for t, _, d, s in steps]
    xa, da, ga = a.trajectory(99, s(T0), sa)
    xb, db, gb = b.trajectory(99, s(T0), sb)
    assert not np.any(ga & ~gb), "the signal set at 0.2 must be inside the one at 0.55"
    both_noise = (ga == 0) & (gb == 0)
    assert both_noise.sum() > 10000
    if noise == "position":
        assert np.array_equal(xa[both_noise].view(np.uint32), xb[both_noise].view(np.uint32))
    else:
        assert np.array_equal(da[both_noise].view(np.uint32), db[both_noise].view(np.uint32))


# --- snapshots, outputs, refusals -----------------------------------------------

def test_snapshot_restore():
    f = yrdk.Field(algorithm="WN", w=8, count=100, coherence=0.5, speed=5, lifetime=0.3)
    f.start(7, s(T0))
    for k in range(1, 31):
        f.update(s(T0 + k * P))
    snap = f.snapshot()
    for k in range(31, 91):
        f.update(s(T0 + k * P))
    want, want_xy = f.digest(), f.xy()
    g = yrdk.Field(algorithm="WN", w=8, count=100, coherence=0.5, speed=5, lifetime=0.3)
    g.restore(snap)
    for k in range(31, 91):
        g.update(s(T0 + k * P))
    assert g.digest() == want
    assert np.array_equal(g.xy(), want_xy)
    with pytest.raises(yrdk.ArgumentError):
        yrdk.Field(w=8, count=101).restore(snap)


def test_outputs():
    f = yrdk.Field(w=10, count=50, coherence=0.5, speed=2, direction=-45, lifetime=0.5)
    f.start(90, s(T0))
    for k in range(1, 21):
        f.update(s(T0 + k * P))
    xy, d, a, sg, e = f.xy(), f.directions(), f.ages(), f.signals(), f.events()
    assert xy.shape == (50, 2) and xy.dtype == np.float32
    assert d.shape == a.shape == (50,) and d.dtype == a.dtype == np.float32
    assert sg.dtype == e.dtype == np.uint8
    assert int(sg.sum()) == 25
    assert np.all(sg == (e & yrdk.EV_SIGNAL))
    assert np.allclose(d[sg == 1], 315.0)
    assert np.all((a >= 0) & (a <= 0.5 + 1e-6))
    assert np.all(np.hypot(xy[:, 0], xy[:, 1]) <= 5.0 + 1e-5)
    assert f.n == 50 and f.total == 50
    assert f.last.ns == T0 + 20 * P and f.last.t == s(T0 + 20 * P) and f.last.set == 0
    assert f.stats["updates"] == 20
    m = yrdk.Field(algorithm=yrdk.MN, w=10, count=30)
    assert m.n == 30 and m.total == 90


def test_refusals():
    with pytest.raises(yrdk.ArgumentError, match="w"):
        yrdk.Field(count=10)
    with pytest.raises(yrdk.ArgumentError, match="preset"):
        yrdk.Field(algorithm="MN", noise="walk", w=10, count=10)
    with pytest.raises(yrdk.ArgumentError, match="noise"):
        yrdk.Field(noise="sideways", w=10, count=10)
    f = yrdk.Field(w=10, count=10)
    with pytest.raises(yrdk.OrderError):
        f.update(0)
    f.start(1, s(100))
    with pytest.raises(yrdk.OrderError):
        f.update(s(99))
    assert f.update(s(100)) == 10


# --- seconds ------------------------------------------------------------------

# ns values across the range, near 1e14 ns (28 h) and up to the bound of
# 2^23 s (about 97 days): each crosses as ns / 1e9 and must come back exact.
BOUND_NS = (1 << 23) * 10**9


def magnitudes():
    out = [0, 1, 999, 16666667, T0, 123456789012, 10**12 + 1, 99999999999999, 10**14, 10**14 + 1,
           123456789012345, 10**15 + 7, 2**52 + 3, BOUND_NS - 1]
    rng = np.random.default_rng(2026)
    for e in range(3, 16):
        out += [int(x) for x in rng.integers(10**e, 10**(e + 1), 200)]
    # 2^51..2^52 ns (26 to 52 days): s * 1e9 rounded once misses about 3 % of
    # these by 1 ns; the binding rounds the exact product (fma)
    out += [int(x) for x in rng.integers(2**51, 2**52, 2000)]
    out += [int(x) for x in rng.integers(BOUND_NS - 10**12, BOUND_NS, 2000)]
    return [x for x in out if x < BOUND_NS]


def test_seconds_in_ns_out_exact():
    f = yrdk.Field(w=10, count=10)
    bad = []
    for ns in magnitudes():
        f.start(1, ns / 1e9)
        f.update(ns / 1e9)
        if f.last.ns != ns or f.last.t != ns / 1e9:
            bad.append((ns, f.last.ns))
    assert not bad, bad[:5]
    # frame_period crosses the same way: 0.016666667 s is 16,666,667 ns
    g = yrdk.Field(w=10, count=10, clock="frames", frame_period=s(P), lifetime_frames=18)
    h = yrdk.Field(w=10, count=10, clock="frames", frame_period=0.016666667, lifetime_frames=18)
    for x in (g, h):
        x.start(5, s(T0))
        for k in range(1, 40):
            x.update(s(T0 + k * P))
    assert g.digest() == h.digest()


def test_logged_trial_replays_through_seconds():
    # a live run in seconds, its Field.last log, and the replay from it: the
    # same digest and positions, near 1e14 ns as near 1e9 ns
    for base in (T0, 10**14 + 12345):
        f = yrdk.Field(algorithm="WN", w=8, count=100, coherence=0.5, speed=5, lifetime=0.3)
        f.start(11, s(base))
        log = []
        for k in range(1, 121):
            f.direction = (k % 8) * 45.0
            f.update(s(base + k * P + (k * 7919) % 101))
            log.append(f.last)
        want, want_xy = f.digest(), f.xy()
        g = yrdk.Field(algorithm="WN", w=8, count=100, coherence=0.5, speed=5, lifetime=0.3)
        xy, _, _ = g.trajectory(11, s(base), log)
        assert g.digest() == want
        assert np.array_equal(xy[-1].view(np.uint32), want_xy.view(np.uint32))
        assert [x.ns for x in log] == [base + k * P + (k * 7919) % 101 for k in range(1, 121)]


def test_seconds_refusals():
    f = yrdk.Field(w=10, count=10)
    for bad in (-1e-9, float("nan"), float("inf"), float(1 << 23), 1e9):
        with pytest.raises(yrdk.ArgumentError, match="seconds"):
            f.start(1, bad)
    f.start(1, 1.0)
    with pytest.raises(yrdk.ArgumentError, match="seconds"):
        f.update(1_000_000_000_000)          # an old ns integer read as seconds
    with pytest.raises(yrdk.ArgumentError, match="step 0"):
        f.replay(1, 1.0, [(T0 + P, 0.5, 0.0, 1.0)])
    # a row whose seconds and ns disagree is refused
    with pytest.raises(yrdk.ArgumentError, match="ns / 1e9"):
        f.replay(1, 1.0, [(1.5, 0.5, 0.0, 1.0, 0, 1_500_000_001)])
    with pytest.raises(yrdk.ArgumentError, match="frame_period"):
        yrdk.Field(w=10, count=10, clock="frames", frame_period=-0.01)
    with pytest.raises(TypeError):
        yrdk.Field(w=10, count=10, frame_ns=P)
