"""Tests for ysp.trials: the key properties of tests/adapt/trials_test.c,
through the binding."""
import collections
import math
import os
import re
import struct

import pytest

import ysp.trials as ytr

try:
    import ysp.stair as yst
except ImportError:  # ysp-stair is a separate distribution
    yst = None

needs_stair = pytest.mark.skipif(yst is None, reason="ysp-stair is not installed")


def run_all(t, outcome=1):
    order = []
    while (ti := t.next()) is not None:
        order.append(ti.condition)
        t.update(outcome)
    return order


def test_splitmix_matches_the_reference():
    u, s = ytr.splitmix(0)
    assert u == (0xE220A8397B1DCDAF >> 11) / 9007199254740992.0
    u, s = ytr.splitmix(s)
    assert u == (0x6E789E6AA1B965F4 >> 11) / 9007199254740992.0
    assert s == (2 * 0x9E3779B97F4A7C15) % 2 ** 64


def test_factorial_round_trip():
    t = ytr.Trials(factors=[("a", 2), ("b", 3), (None, 4)], reps=1)
    assert t.n_conditions == 24 and t.n_factors == 3
    for c in range(24):
        lv = t.levels(c)
        assert lv == (c // 12, (c // 4) % 3, c % 4)          # last factor fastest
        assert t.condition_from_levels(list(lv)) == c
        assert t.level(c, ytr.CONDITION) == c


def test_sequential_is_repetition_major():
    t = ytr.Trials(n_conditions=4, reps=3)
    assert run_all(t) == [0, 1, 2, 3] * 3
    t = ytr.Trials(n_conditions=3, cond_reps=[1, 3, 2])
    assert run_all(t) == [0, 1, 2, 1, 2, 1]


@pytest.mark.parametrize("seed", range(50))
def test_random_orders(seed):
    t = ytr.Trials(n_conditions=5, reps=4, order=ytr.ORDER_RANDOM, rng=seed)
    order = t.schedule()
    for r in range(4):
        assert sorted(order[5 * r:5 * r + 5]) == list(range(5))
    t = ytr.Trials(n_conditions=5, cond_reps=[1, 2, 3, 0, 6], order=ytr.ORDER_FULL_RANDOM,
                  rng=seed)
    assert collections.Counter(t.schedule()) == {0: 1, 1: 2, 2: 3, 4: 6}


def test_fisher_yates_draw_order():
    # u = 0: for i = 3..1 swap i and 0: [0 1 2 3] -> [1 2 3 0].
    t = ytr.Trials(n_conditions=4, reps=1, order=ytr.ORDER_FULL_RANDOM, rng=lambda: 0.0)
    assert t.schedule() == [1, 2, 3, 0]
    t = ytr.Trials(n_conditions=4, reps=1, order=ytr.ORDER_FULL_RANDOM,
                  rng=lambda: 1.0 - 2 ** -53)
    assert t.schedule() == [0, 1, 2, 3]
    calls = []

    def counting():
        calls.append(1)
        return 0.5
    ytr.Trials(n_conditions=10, reps=1, order=ytr.ORDER_FULL_RANDOM, n_practice=3, rng=counting)
    assert len(calls) == 3 + 9


def test_int_seed_equals_splitmix_callable():
    state = [12345]

    def rng():
        u, state[0] = ytr.splitmix(state[0])
        return u
    a = ytr.Trials(n_conditions=6, reps=5, order=ytr.ORDER_FULL_RANDOM, rng=12345)
    b = ytr.Trials(n_conditions=6, reps=5, order=ytr.ORDER_FULL_RANDOM, rng=rng)
    assert a.schedule() == b.schedule() and a.rng_state == state[0]


def max_run_ok(seq, key, n):
    run, prev = 0, object()
    for x in seq:
        k = key(x)
        run = run + 1 if k == prev else 1
        prev = k
        if k is not None and run > n:
            return False
    return True


@pytest.mark.parametrize("seed", range(30))
def test_constraints_hold_on_every_trial(seed):
    t = ytr.Trials(factors=[("orientation", 2), ("contrast", 5)], reps=10,
                  order=ytr.ORDER_CONSTRAINED,
                  constraints=[ytr.max_run("orientation", ytr.ANY_LEVEL, 3),
                               ytr.no_transition(ytr.CONDITION, 3, 3),
                               ytr.first_not(1, 0)],
                  rng=seed)
    order = run_all(t)
    assert len(order) == 100
    ori = [c // 5 for c in order]
    assert max_run_ok(ori, lambda x: x, 3)
    assert all(not (a == 3 and b == 3) for a, b in zip(order, order[1:]))
    assert order[0] % 5 != 0
    assert not any(h.violation for h in t.history())


def test_min_gap_and_window():
    t = ytr.Trials(n_conditions=4, cond_reps=[6, 14, 14, 14], order=ytr.ORDER_CONSTRAINED,
                  constraints=[ytr.min_gap(ytr.CONDITION, 0, 2),
                               ytr.max_in_window(ytr.CONDITION, 1, 5, 2)], rng=9)
    order = run_all(t)
    zeros = [i for i, c in enumerate(order) if c == 0]
    assert all(b - a > 2 for a, b in zip(zeros, zeros[1:]))
    assert all(order[i:i + 5].count(1) <= 2 for i in range(len(order)))


def test_impossible_design_is_error_with_the_header_message():
    with pytest.raises(ytr.Error, match=r"constraint 0, max_run\(cond,any,1\).*after 10 swaps") as e:
        ytr.Trials(n_conditions=2, cond_reps=[3, 5], order=ytr.ORDER_CONSTRAINED,
                  constraints=[ytr.max_run(ytr.CONDITION, ytr.ANY_LEVEL, 1)], max_swaps=10, rng=5)
    assert not isinstance(e.value, ytr.ArgumentError)


def test_bad_descs_are_argument_errors():
    with pytest.raises(ytr.ArgumentError):
        ytr.Trials()                                             # nothing to run
    with pytest.raises(ytr.ArgumentError, match="rng"):
        ytr.Trials(n_conditions=3, reps=1, order=ytr.ORDER_RANDOM)
    with pytest.raises(ytr.ArgumentError):
        ytr.Trials(n_conditions=3, reps=1, constraints=[ytr.max_run(ytr.CONDITION, 0, 1)])
    with pytest.raises(ytr.ArgumentError, match="cond_reps has 2"):
        ytr.Trials(n_conditions=3, cond_reps=[1, 1])
    with pytest.raises(ytr.ArgumentError, match="factor 'size'"):
        ytr.Trials(factors=[("a", 2)], reps=1, order=ytr.ORDER_CONSTRAINED, rng=1,
                  constraints=[ytr.max_run("size", 0, 1)])
    with pytest.raises(TypeError):
        ytr.Trials(n_conditions=1, reps=1, tracks=[object()], track_rate=0.5)


class CountingTrack:
    def __init__(self, n):
        self.left, self.asked = n, 0

    def is_done(self):
        self.asked += 1
        return self.left <= 0


def test_tracks_round_robin_and_finished_never_asked():
    tr = [CountingTrack(2), CountingTrack(3)]
    t = ytr.Trials(tracks=tr, interleave=ytr.INTERLEAVE_ROUND_ROBIN)
    seq = []
    while (ti := t.next()) is not None:
        seq.append(ti.track)
        tr[ti.track].left -= 1
        t.update(1)
    assert seq == [0, 1, 0, 1, 1]
    asked = [x.asked for x in tr]
    assert t.done
    assert [x.asked for x in tr] == asked       # finished tracks are not asked again


@needs_stair
def test_interleaved_staircases_with_catch_trials():
    stairs = [yst.Staircase(start=10.0 + i, steps=1.0, stop_trials=8) for i in range(3)]
    t = ytr.Trials(n_conditions=1, reps=6, order=ytr.ORDER_CONSTRAINED,
                  constraints=[ytr.min_gap(ytr.CONDITION, 0, 1)],
                  tracks=stairs, track_rate=0.9, rng=20260923, record_size=8)
    while (ti := t.next()) is not None:
        if ti.track >= 0:
            s = stairs[ti.track]
            x = s.next()
            s.update(x, 1)
            t.update(1, struct.pack("<d", x))
        else:
            t.update(0)
    assert all(s.done for s in stairs) and t.done
    h = t.history()
    assert len(h) == 3 * 8 + 6
    assert sum(1 for x in h if x.track >= 0) == 24
    # A catch trial that follows another only happens once the tracks ran out.
    for i in range(1, len(h)):
        if h[i].condition == 0 and h[i - 1].condition == 0:
            assert h[i].violation
    track_trials = [i for i, x in enumerate(h) if x.track == 0]
    levels = [struct.unpack("<d", t.record(i))[0] for i in track_trials]
    assert levels == [x.shown for x in stairs[0].history()]
    assert t.record(next(i for i, x in enumerate(h) if x.track < 0)) == bytes(8)


def test_requeue_and_tallies():
    t = ytr.Trials(n_conditions=2, reps=2)
    t.next()
    t.requeue()                                  # condition 0 goes to the end
    outcomes = []
    while (ti := t.next()) is not None:
        t.update(ti.condition)                   # outcome = the row, for the tally
        outcomes.append(ti.condition)
    h = t.history()
    assert h[0].outcome == ytr.REQUEUE and h[-1].requeued and h[-1].condition == 0
    assert t.n_valid(0) == 2 and t.count(0, ytr.REQUEUE) == 1
    assert t.proportion(1, 1) == 1.0 and math.isnan(t.proportion(0, ytr.INVALID))
    with pytest.raises(ytr.OutOfOrder):
        t.update(1)
    t = ytr.Trials(tracks=[CountingTrack(1)])
    t.next()
    with pytest.raises(ytr.ArgumentError):
        t.requeue()                              # the track decides, not the sequencer
    with pytest.raises(ytr.ArgumentError):
        t.update(-3)                             # below INVALID


def _session(rng, tracks=None):
    return ytr.Trials(factors=[("side", 2), ("size", 3)], reps=6, order=ytr.ORDER_CONSTRAINED,
                     constraints=[ytr.max_run("side", ytr.ANY_LEVEL, 2)], block_size=12,
                     n_practice=2, n_warmup=1, requeue_gap=3, rng=rng, record_size=4)


def _drive(t, n, k0=0):
    for k in range(k0, k0 + n):
        ti = t.next()
        if ti is None:
            return False
        if k % 7 == 3 and not (ti.practice or ti.warmup):
            t.requeue()
        else:
            t.update(k % 2, struct.pack("<i", k))
    return True


def test_save_load_round_trip():
    ref = _session(77)
    _drive(ref, 1000)
    for cut in (0, 1, 5, 17, 30):
        a = _session(77)
        _drive(a, cut)
        snap, state = a.save(), a.rng_state
        b = ytr.Trials.load(snap, factors=[("side", 2), ("size", 3)], reps=6,
                           order=ytr.ORDER_CONSTRAINED,
                           constraints=[ytr.max_run("side", ytr.ANY_LEVEL, 2)], block_size=12,
                           n_practice=2, n_warmup=1, requeue_gap=3, rng=state, record_size=4)
        assert b.save() == snap
        _drive(b, 1000, cut)
        assert b.history() == ref.history()
        assert [b.record(i) for i in range(b.n_run)] == [ref.record(i) for i in range(ref.n_run)]
        assert b.save() == ref.save()
    with pytest.raises(ytr.Error, match="reps"):
        ytr.Trials.load(ref.save(), factors=[("side", 2), ("size", 3)], reps=5,
                       order=ytr.ORDER_CONSTRAINED,
                       constraints=[ytr.max_run("side", ytr.ANY_LEVEL, 2)], block_size=12,
                       n_practice=2, n_warmup=1, requeue_gap=3, rng=1, record_size=4)
    with pytest.raises(ytr.Error):
        ytr.Trials.load(b"YTRL garbage", n_conditions=1, reps=1)


def test_restore_reproduces_the_run():
    ref = _session(31)
    _drive(ref, 1000)
    h = ref.history()
    recs = [ref.record(i) for i in range(ref.n_run)]
    t = _session(31)
    t.restore([x.outcome for x in h], recs)
    assert t.history() == h and t.rng_state == ref.rng_state
    assert t.save() == ref.save()
    with pytest.raises(ytr.OutOfOrder):
        t.restore([1])


def test_format_functions():
    tr = CountingTrack(1)
    t = ytr.Trials(factors=[("orientation", 2), (None, 3), ('a,"b"', 1)], reps=1,
                  n_practice=1, tracks=[tr], track_rate=1.0, record_size=8)
    assert t.format_header() == ("index,block,rep,condition,track,practice,warmup,requeued,"
                                 "after_break,outcome,orientation,factor1,\"a,\"\"b\"\"\"\n")
    t.next()
    assert t.format_row(0) == "0,-1,-1,0,-1,1,0,0,0,,0,0,0\n"
    t.update(1)
    assert t.format_row(0) == "0,-1,-1,0,-1,1,0,0,0,1,0,0,0\n"
    t.next()
    tr.left = 0
    t.update(ytr.INVALID)
    assert t.format_row(1) == "1,0,-1,-1,0,0,0,0,0,-1,,,\n"
    assert t.format_meta().startswith("ysp_trials=" + ytr.__version__ + " conditions=6 factors=3 levels=2x3x1")
    with pytest.raises(ytr.ArgumentError):
        t.format_row(9)


def test_callback_exceptions_propagate():
    class Broken:
        def is_done(self):
            raise RuntimeError("track failed")
    t = ytr.Trials(tracks=[Broken()])
    with pytest.raises(RuntimeError, match="track failed"):
        t.next()

    def bad_rng():
        raise KeyError("rng failed")
    with pytest.raises(KeyError):
        ytr.Trials(n_conditions=3, reps=2, order=ytr.ORDER_FULL_RANDOM, rng=bad_rng)


def test_version_matches_pyproject():
    # pyproject.toml's version is a copy; __version__ comes from the header
    # compiled into the extension. Fail when the two drift.
    path = os.path.join(os.path.dirname(__file__), "..", "pyproject.toml")
    if not os.path.exists(path):
        pytest.skip("pyproject.toml is not beside the tests")
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    assert m, "no version in pyproject.toml"
    assert ytr.__version__ == m.group(1)


# --------------------------------------------------------------------- v0.2
# Tables (ysp/table.h), order lists, draws, subsets, groups, units, balance,
# Latin squares and rules text, through the binding. The CSV parser and the
# number parser are checked against two oracles written by other people:
# Python's csv module and float().

import csv
import io
import random

COND_CSV = (
    "target,contrast,word,list,n\n"
    "a,0.25,cat,1,2\na,0.5,dog,1,1\nb,0.25,\"new york\",2,2\nb,0.5,ox,2,1\n"
    "catch,0,emu,1,1\ncatch,0,yak,2,1\n"
)


def test_table_basics_and_bytes():
    tab = ytr.Table(COND_CSV)
    assert tab.columns == ["target", "contrast", "word", "list", "n"]
    assert tab.types == ["string", "number", "string", "integer", "integer"]
    assert len(tab) == tab.n_rows == 6
    assert tab.value(2, "word") == "new york"
    assert tab.value(1, 1) == 0.5 and tab.value(0, "n") == 2
    assert tab.levels("target") == ["a", "b", "catch"]
    assert tab.find("contrast", "0.50") == 1
    assert tab.row(4) == {"target": "catch", "contrast": 0.0, "word": "emu", "list": 1, "n": 1}
    b = tab.to_bytes()
    assert b[:4] == b"PSTB" and len(b) % 8 == 0
    t2 = ytr.Table.from_bytes(b)
    assert t2.hash == tab.hash and t2.row(2) == tab.row(2)
    bad = bytearray(b)
    bad[60] ^= 1
    with pytest.raises(ytr.ArgumentError, match="hash"):
        ytr.Table.from_bytes(bytes(bad))
    with pytest.raises(ytr.ArgumentError,
                       match="line 3, row 2, column 'x' \\(1\\): '0,5' is not a number; the decimal"):
        ytr.Table('x,y\n1,a\n"0,5",b\n', types={"x": "number"})
    t3 = ytr.Table("code\n007\n7\n", types={"code": "string"})
    assert t3.levels("code") == ["007", "7"]


def _random_field(rng):
    alphabet = ["a", "b", "z", " ", ",", '"', "\n", "\r\n", "1", ".", "é", "中", "x"]
    return "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 8)))


@pytest.mark.parametrize("seed", range(40))
def test_csv_against_the_csv_module(seed):
    rng = random.Random(seed)
    ncol = rng.randint(1, 6)
    rows = []
    while len(rows) < rng.randint(1, 30):
        row = [_random_field(rng) for _ in range(ncol)]
        if any(row):                                   # all-empty rows are skipped by design
            rows.append(row)
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator=rng.choice(["\n", "\r\n"]))
    w.writerow(["c%d" % i for i in range(ncol)])
    w.writerows(rows)
    text = buf.getvalue()
    if rng.random() < 0.3:
        text = "﻿" + text                         # a byte order mark
    tab = ytr.Table(text, types={"c%d" % i: "string" for i in range(ncol)})
    want = list(csv.reader(io.StringIO(text.lstrip("﻿"), newline="")))[1:]
    assert tab.n_rows == len(want)
    for r, row in enumerate(want):
        assert [tab.value(r, c) for c in range(ncol)] == row


def _random_decimal(rng):
    nd = rng.randint(1, 19)
    digits = str(rng.randint(1, 9)) + "".join(str(rng.randint(0, 9)) for _ in range(nd - 1))
    pt_at = rng.randint(0, nd)
    s = digits[:pt_at] + ("." if pt_at < nd and rng.random() < 0.7 else "") + digits[pt_at:]
    if s.startswith("."):
        s = "0" + s
    if rng.random() < 0.5:
        s += "e%d" % rng.randint(-330, 300)
    if rng.random() < 0.3:
        s = "-" + s
    return s


@pytest.mark.parametrize("seed", range(10))
def test_numbers_against_float(seed):
    rng = random.Random(1000 + seed)
    vals = []
    while len(vals) < 500:
        s = _random_decimal(rng)
        v = float(s)
        if v == 0.0 or math.isinf(v):
            continue                                   # range errors are tested in C
        vals.append((s, v))
    text = "v\n" + "\n".join(s for s, _ in vals) + "\n"
    tab = ytr.Table(text, types={"v": "number"})
    for r, (s, v) in enumerate(vals):
        got = tab.value(r, 0)
        assert got == v and math.copysign(1, got) == math.copysign(1, v), s


def test_table_conditions_and_names():
    tab = ytr.Table(COND_CSV)
    t = ytr.Trials(table=tab, reps=4, order=ytr.ORDER_CONSTRAINED, rng=3,
                  constraints=[ytr.max_run("target", ytr.ANY_LEVEL, 1),
                               ytr.max_in_window("target", "catch", 2, 1)])
    assert t.n_conditions == 6 and t.n_factors == 5 and t.table is tab
    s = t.schedule()
    lv = [tab.level(c, "target") for c in s]
    assert all(a != b for a, b in zip(lv, lv[1:]))
    assert t.values(2)["word"] == "new york"
    assert t.format_header().endswith("target,contrast,word,list,n\n")
    with pytest.raises(ytr.ArgumentError, match="no level 'd'"):
        ytr.Trials(table=tab, reps=2, order=ytr.ORDER_CONSTRAINED, rng=1,
                  constraints=[ytr.max_run("target", "d", 1)])


def test_rules_give_the_c_schedule():
    tab = ytr.Table(COND_CSV)
    a = ytr.Trials(table=tab, reps=6, order=ytr.ORDER_CONSTRAINED, rng=99, block_size=12,
                  constraints=[ytr.max_run("target", ytr.ANY_LEVEL, 2),
                               ytr.max_in_window("target", "catch", 2, 1),
                               ytr.first_not("target", "catch"),
                               ytr.min_gap("word", "new york", 3)])
    rules = ("order constrained\nreps 6\nmax_run target 2\nmax_in_window target=catch 2 1\n"
             "first_not target=catch\nmin_gap word=\"new york\" 3\nblock_size 12\n")
    b = ytr.Trials(table=tab, rules=rules, rng=99)
    assert a.schedule() == b.schedule()
    c = ytr.Trials(table=tab, rules=b.format_rules(), rng=99)
    assert c.schedule() == b.schedule()
    with pytest.raises(ytr.ArgumentError, match="rules line 2.*did you mean 'max_run'"):
        ytr.Trials(table=tab, rules="reps 2\nmax_runn target 2\n", rng=1)
    with pytest.raises(ytr.ArgumentError, match="the statement here is 'min_gap'"):
        ytr.Trials(table=tab, rules="mindist target 2\n", rng=1)


def test_list_draws_subset():
    t = ytr.Trials(n_conditions=4, order=ytr.ORDER_LIST, order_list=[3, 0, 0, 2])
    assert t.schedule() == [3, 0, 0, 2]
    t = ytr.Trials(n_conditions=3, order=ytr.ORDER_WITH_REPLACEMENT, draws=3000,
                  weights=[1, 0, 3], rng=5)
    s = t.schedule()
    assert s.count(1) == 0 and abs(s.count(2) / 3000 - 0.75) < 0.04
    t = ytr.Trials(n_conditions=10, reps=1, subset=4, rng=8)
    s = t.schedule()
    assert len(s) == 4 and s == sorted(s)


def test_latin():
    for n in range(1, 13):
        for bal in (False, True):
            rows = ytr.latin_rows(n, bal)
            seen = collections.Counter()
            for r in range(rows):
                row = ytr.latin(n, r, bal)
                assert sorted(row) == list(range(n))
                if bal:
                    seen.update(zip(row, row[1:]))
            if bal:
                want = 2 if n % 2 else 1
                assert all(seen[(a, b)] == want for a in range(n) for b in range(n) if a != b)
    assert ytr.latin(4, 0, True) == [0, 1, 3, 2]


def test_groups_and_units():
    tab = ytr.Table("block,kind\nA,prime\nA,target\nA,fill\nB,prime\nB,target\nB,fill\n")
    t = ytr.Trials(table=tab, reps=3, order=ytr.ORDER_CONSTRAINED, rng=4,
                  groups=ytr.groups("block", "blocked", "balanced_latin", participant=1),
                  constraints=[ytr.followed_by("kind", "prime", "target")])
    s = t.schedule()
    blocks = [tab.value(c, "block") for c in s]
    assert blocks == ["B"] * 9 + ["A"] * 9
    kinds = [tab.value(c, "kind") for c in s]
    for i, k in enumerate(kinds):
        if k == "prime":
            assert kinds[i + 1] == "target"


def test_balance_pairs_and_leadin():
    tab = ytr.Table("lvl\nA\nB\nC\n")
    t = ytr.Trials(table=tab, reps=6, order=ytr.ORDER_CONSTRAINED, rng=12,
                  constraints=[ytr.balance("lvl")])
    s = t.schedule()
    assert len(s) == 19
    pairs = collections.Counter(zip(s, s[1:]))
    assert all(pairs[(a, b)] == 2 for a in range(3) for b in range(3))
    run_all(t)
    h = t.history()
    assert h[0].leadin and h[0].rep == -1 and not any(x.leadin for x in h[1:])
    t = ytr.Trials(table=tab, reps=4, order=ytr.ORDER_CONSTRAINED, rng=12,
                  constraints=[ytr.balance("lvl", no_repeat=True, no_leadin=True)])
    s = t.schedule()
    assert len(s) == 12 and all(a != b for a, b in zip(s, s[1:]))


# ------------------------------------------------------------------- v0.2.1
# Jitter: per-trial durations drawn from the session's generator, logged in
# the data line, reproduced by a replay.

def test_jitter_values_and_log():
    t = ytr.Trials(n_conditions=3, reps=4, order=ytr.ORDER_FULL_RANDOM, rng=31,
                  jitters=[ytr.uniform("iti", 0.8, 1.2, rate=(60000, 1001)),
                           ytr.exponential("fp", 0.5, 2.0, 0.4),
                           ytr.choice("soa", [0.1, 0.2, 0.4])])
    assert t.jitters == ["iti", "fp", "soa"]
    rows = []
    while (ti := t.next()) is not None:
        iti = t.jitter(ti.index, "iti")
        assert 48 <= iti.frames <= 71
        assert iti.ns == (iti.frames * 1001 * 10**9 + 30000) // 60000
        fp = t.jitter(ti.index, 1)
        assert 0.5 <= fp.s <= 2.0 and fp.frames == -1 and fp.ns == round(fp.s * 1e9)
        assert t.jitter(ti.index, 2).s in (0.1, 0.2, 0.4)
        t.update(1)
        rows.append(t.format_row(ti.index))
    assert t.format_header().endswith(",iti,fp,soa\n")
    last = rows[-1].rstrip("\n").split(",")
    assert int(round(float(last[-2]) * 1e9)) == t.jitter(len(rows) - 1, "fp").ns
    assert "jitter iti uniform 0.8 1.2 rate=60000/1001" in t.format_rules()
    # A replay from the same seed and outcomes gives the same durations.
    u = ytr.Trials(n_conditions=3, reps=4, order=ytr.ORDER_FULL_RANDOM, rng=31,
                  jitters=[ytr.uniform("iti", 0.8, 1.2, rate=(60000, 1001)),
                           ytr.exponential("fp", 0.5, 2.0, 0.4),
                           ytr.choice("soa", [0.1, 0.2, 0.4])])
    u.restore([1] * len(rows))
    assert [u.format_row(i) for i in range(len(rows))] == rows
    with pytest.raises(IndexError):
        t.jitter(0, 3)
    with pytest.raises(ytr.ArgumentError):
        t.jitter(0, "nope")


def test_jitter_columns_rules_and_errors():
    tab = ytr.Table("target,fp_lo,fp_hi\na,0.5,0.7\nb,1.0,1.5\n")
    t = ytr.Trials(table=tab, reps=5, order=ytr.ORDER_FULL_RANDOM, rng=2,
                  jitters=[ytr.uniform("fp", "fp_lo", "fp_hi")])
    while (ti := t.next()) is not None:
        lo, hi = tab.value(ti.condition, "fp_lo"), tab.value(ti.condition, "fp_hi")
        assert lo <= t.jitter(ti.index, "fp").s <= hi
        t.update(0)
    r = ytr.Trials(table=tab, rng=2, rules="order full_random\nreps 5\njitter fp uniform fp_lo fp_hi\n")
    s = ytr.Trials(table=tab, reps=5, order=ytr.ORDER_FULL_RANDOM, rng=2,
                  jitters=[ytr.uniform("fp", "fp_lo", "fp_hi")])
    for i in range(10):
        r.next(); s.next()
        assert r.jitter(i, 0) == s.jitter(i, 0)
        r.update(1); s.update(1)
    with pytest.raises(ytr.ArgumentError, match="no whole frame of 60/1 Hz"):
        ytr.Trials(n_conditions=1, reps=1, rng=1, jitters=[ytr.uniform("x", 0.801, 0.81, rate=60)])
    with pytest.raises(ytr.ArgumentError, match="desc.rng is required for jitter draws"):
        ytr.Trials(n_conditions=1, reps=1, jitters=[ytr.uniform("x", 0.1, 0.2)])
    with pytest.raises(ytr.ArgumentError, match="rules line 1"):
        ytr.Trials(table=tab, rng=1, rules="jitter fp gaussian 1 2\n")


def test_jitter_map_against_numpy_free_oracles():
    # The maps, written here from their definitions: uniform over the
    # nanoseconds in [lo, hi]; the exponential's inverse CDF folded into
    # [lo, hi) modulo hi - lo (exactly the truncated exponential).
    u_list = [0.0, 0.1, 0.37, 0.5, 0.9, 0.999999]
    for u in u_list:
        v = ytr.jitter_map(ytr.uniform("u", 0.8, 1.2), u)
        assert v.ns == 800000000 + int(u * 2**53) * 400000001 // 2**53
        assert v.s == v.ns / 1e9
        e_ns = 0.4e9 * -math.log1p(-u)
        want = 500000000 + math.fmod(e_ns, 1.5e9)
        assert abs(ytr.jitter_map(ytr.exponential("e", 0.5, 2.0, 0.4), u).ns - want) <= 1.0
    v = ytr.jitter_map(ytr.uniform("f", 0.8, 1.2, rate=(60000, 1001)), 0.0)
    assert v.frames == 48 and v.ns == 800800000
    with pytest.raises(ytr.ArgumentError, match="lo <= hi"):
        ytr.jitter_map(ytr.uniform("u", 1.2, 0.8), 0.5)


def test_jitter_golden_digests_match_c():
    # tests/adapt/trials_jitter_golden.h: the same 200000 draws per
    # distribution must give the same digests here as in every C build.
    vals = [0.1, 0.25, 0.333333333333, 0.5]
    specs = [ytr.uniform("a", 0.8, 1.2), ytr.uniform("b", 0.8, 1.2, rate=(60000, 1001)),
             ytr.choice("c", vals), ytr.choice("d", vals, rate=60),
             ytr.exponential("e", 0.5, 2.0, 0.4), ytr.exponential("f", 0.5, 2.0, 0.4, rate=(60000, 1001))]
    want = [0xd2e231de82a90abc, 0xfa91de69b65221e5, 0xc4d62850f9f90fd5,
            0xd74230f52068797a, 0xf4a23384d974cceb, 0xc2c66e03f8a80f1a]
    mask = (1 << 64) - 1
    for spec, w in zip(specs, want):
        st, h = 2026, 0xCBF29CE484222325
        for _ in range(200000):
            u, st = ytr.splitmix(st)
            v = ytr.jitter_map(spec, u)
            h = ((h ^ (v.ns & mask)) * 0x100000001B3) & mask
            h = ((h ^ (v.frames & mask)) * 0x100000001B3) & mask
        assert h == w, spec["name"]


# --- v0.6: variables, the trial streams, weight x reps, rate='display' -----

_M64 = (1 << 64) - 1
_G = 0x9E3779B97F4A7C15


def _mix64(z):
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _M64
    return z ^ (z >> 31)


def _stream(seed, i, k):
    # The manual's VARIABLES, written apart from the header.
    s = _mix64(((seed ^ 0x6A09E667F3BCC909) + (i + 1) * _G) & _M64)
    return _mix64((s + (k + 1) * _G) & _M64)


def _var_index(seed, i, j, n):
    return ((_stream(seed, i, 1 + j) >> 11) * n) >> 53


def test_vars_equal_an_independent_stream():
    assert _stream(0, 0, 0) == 0x96DCB1D7126A6EBA     # pinned in trials_test_vars.h
    assert _stream(20261021, 999, 2) == 0xB1F2FA923152EA67
    odd = ["a b", "c,d", "été"]
    seed = 2**64 - 5
    t = ytr.Trials(n_conditions=3, reps=6, order=ytr.ORDER_FULL_RANDOM, rng=5, seed=seed,
                   vars=[ytr.var_choice("side", ["left", "right"]), ytr.var_choice("odd", odd)])
    assert t.vars == ["side", "odd"]
    n = 0
    while (ti := t.next()) is not None:
        i = ti.index
        assert t.var(i, "side") == ["left", "right"][_var_index(seed, i, 0, 2)]
        assert t.var(i, 1) == odd[_var_index(seed, i, 1, 3)]
        assert t.var_index(i, "odd") == _var_index(seed, i, 1, 3)
        assert t.trial_seed(i) == _stream(seed, i, 0)
        t.update(1)
        n += 1
    assert n == 18
    with pytest.raises(IndexError):
        t.var(n, 0)
    with pytest.raises(ytr.ArgumentError, match="no var of that name"):
        t.var(0, "nope")
    # vars draw nothing from the session's generator: same schedule
    u = ytr.Trials(n_conditions=3, reps=6, order=ytr.ORDER_FULL_RANDOM, rng=5)
    assert run_all(u) == [h.condition for h in t.history()]


def test_vars_in_the_data_line_and_replay():
    t = ytr.Trials(n_conditions=2, reps=5, order=ytr.ORDER_RANDOM, rng=3, seed=9,
                   vars=[ytr.var_choice("side", ["left", "right"]), ytr.var_choice("w", ["x,y", "z"])])
    run_all(t)
    assert t.format_header().endswith(",outcome,side,w,trial_seed\n")
    for i in range(t.n_run):
        w = t.var(i, "w")
        assert t.format_row(i).endswith("," + t.var(i, "side") + "," + ('"x,y"' if w == "x,y" else w) +
                                        ",x%016x\n" % t.trial_seed(i))
    r = ytr.Trials(n_conditions=2, reps=5, order=ytr.ORDER_RANDOM, rng=3, seed=9,
                   vars=[ytr.var_choice("side", ["left", "right"]), ytr.var_choice("w", ["x,y", "z"])])
    r.restore([h.outcome for h in t.history()])
    assert [r.format_row(i) for i in range(r.n_run)] == [t.format_row(i) for i in range(t.n_run)]
    snap = t.save()
    assert snap[4] == 5
    with pytest.raises(ytr.Error, match="desc.seed does not match"):
        ytr.Trials.load(snap, n_conditions=2, reps=5, order=ytr.ORDER_RANDOM, rng=3, seed=10,
                        vars=[ytr.var_choice("side", ["left", "right"]), ytr.var_choice("w", ["x,y", "z"])])


def test_trial_seed_column_round_trips():
    # "x" and 16 hex digits: text in ysp/table.h, the csv module and pandas by
    # their defaults, so every bit comes back; a seed of 0 and seeds whose top
    # nibble is 0 keep their 16 digits.
    seeds = []
    for seed in (0, 1, 2**63, 2**64 - 1):
        t = ytr.Trials(n_conditions=2, reps=8, order=ytr.ORDER_RANDOM, rng=7, seed=seed,
                       vars=[ytr.var_choice("side", ["left", "right"])])
        run_all(t)
        text = t.format_header() + "".join(t.format_row(i) for i in range(t.n_run))
        want = [t.trial_seed(i) for i in range(t.n_run)]
        seeds += want
        tab = ytr.Table(text)
        assert tab.types[tab.columns.index("trial_seed")] == "string"
        got = [tab.value(i, "trial_seed") for i in range(len(tab))]
        assert got == ["x%016x" % v for v in want]
        assert [int(s[1:], 16) for s in got] == want
        assert [r["trial_seed"] for r in csv.DictReader(io.StringIO(text))] == got
        try:
            import pandas as pd
        except ImportError:   # optional: the CSV and table checks above stand
            pd = None
        if pd is not None:
            col = pd.read_csv(io.StringIO(text))["trial_seed"]
            assert list(col) == got
    assert any(v >> 60 == 0 for v in seeds)


def test_vars_rules_round_trip_and_errors():
    t = ytr.Trials(n_conditions=2, rng=4, seed=11,
                   rules="order random\nreps 3\nvar side choice left right\nvar q choice \"a b\" c\n")
    assert t.vars == ["side", "q"]
    assert "\nvar side choice left right\nvar q choice \"a b\" c\n" in t.format_rules()
    run_all(t)
    u = ytr.Trials(n_conditions=2, rng=4, seed=11, rules=t.format_rules())
    run_all(u)
    assert [u.var(i, "q") for i in range(u.n_run)] == [t.var(i, "q") for i in range(t.n_run)]
    with pytest.raises(ytr.ArgumentError, match="expected: var NAME choice"):
        ytr.Trials(n_conditions=2, rng=4, rules="reps 1\nvar side left right\n")
    with pytest.raises(ytr.ArgumentError, match=r"desc.vars\[0\] \(side\) needs 1 to 32 values"):
        ytr.Trials(n_conditions=2, reps=1, rng=4, vars=[ytr.var_choice("side", [])])


def test_weight_times_reps():
    tab = ytr.Table(COND_CSV)
    t = ytr.Trials(table=tab, rng=1, rules="order random\nweight n\nreps 3\n")
    assert t.n_scheduled == 3 * 8
    assert "\ncond_reps 6 3 6 3 3 3\n" in t.format_rules()
    t2 = ytr.Trials(table=tab, rng=1, rules="order random\nweight n\n")
    assert t2.n_scheduled == 8
    with pytest.raises(ytr.ArgumentError, match="reps with cond_reps"):
        ytr.Trials(table=tab, rng=1, rules="cond_reps 1 1 1 1 1 1\nreps 2\n")


def test_rate_display():
    kw = dict(n_conditions=2, reps=10, order=ytr.ORDER_RANDOM, rng=6)
    with pytest.raises(ytr.ArgumentError, match="snaps to the display's rate"):
        ytr.Trials(jitters=[ytr.uniform("soa_s", 0.8, 1.2, rate="display")], **kw)
    d = ytr.Trials(jitters=[ytr.uniform("soa_s", 0.8, 1.2, rate="display")], display_rate=(60000, 1001), **kw)
    f = ytr.Trials(jitters=[ytr.uniform("soa_s", 0.8, 1.2, rate=(60000, 1001))], **kw)
    run_all(d)
    run_all(f)
    assert [d.jitter(i, 0) for i in range(d.n_run)] == [f.jitter(i, 0) for i in range(f.n_run)]
    assert "jitter soa_s uniform 0.8 1.2 rate=display" in d.format_rules()
    assert " display_rate=60000/1001" in d.format_meta()
    r = ytr.Trials(display_rate=60, rules="order random\nreps 10\njitter soa_s uniform 0.8 1.2 rate=display\n",
                   **{k: v for k, v in kw.items() if k not in ("reps", "order")})
    assert r.jitter(r.next().index, 0).frames in range(48, 73)
    with pytest.raises(ytr.ArgumentError, match="rate is an int"):
        ytr.Trials(jitters=[ytr.uniform("soa_s", 0.8, 1.2, rate="screen")], display_rate=60, **kw)
    with pytest.raises(ytr.ArgumentError, match="YTR_RATE_DISPLAY needs a session"):
        ytr.jitter_map(ytr.uniform("soa_s", 0.8, 1.2, rate="display"), 0.5)
