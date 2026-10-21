"""Tests for ysp.stair. The tracks are the hand-derived ones in
tests/adapt/stair_test.c, replayed through the binding."""
import math

import os
import re

import pytest

import ysp.stair as yst


def replay(s, responses):
    events = []
    for r in responses:
        events.append(s.update(s.next(), r))
    return events


def proposals(s):
    return [t.proposed for t in s.history()]


def test_updown_1_1_track():
    s = yst.Staircase(start=10.0, steps=1.0, stop_trials=10)
    replay(s, [1, 1, 0, 0, 1, 0, 1, 1, 1, 0])
    assert proposals(s) == [10, 9, 8, 9, 10, 9, 10, 9, 8, 7]
    assert s.reversals() == [(2, 8.0), (4, 10.0), (5, 9.0), (6, 10.0), (9, 7.0)]
    assert [s.reversal_level(i) for i in range(5)] == [8, 10, 9, 10, 7]
    assert [s.reversal_trial(i) for i in range(5)] == [2, 4, 5, 6, 9]
    with pytest.raises(IndexError):
        s.reversal_level(5)
    assert s.n_trials == 10 and s.n_reversals == 5
    assert s.done and s.stop_reason == yst.Stop.TRIALS
    assert s.next() == 8.0
    h = s.history()
    assert all(t.direction != 0 for t in h)
    assert (h[0].direction, h[2].direction) == (-1, 1)
    assert [t.reversal for t in h] == [False, False, True, False, True,
                                       True, True, False, False, True]
    assert s.estimate_count(yst.EST_REVERSALS) == 5
    assert s.estimate_count(yst.EST_TRIALS) == 10
    assert s.estimate_count(yst.EST_LAST) == 1
    assert s.estimate(yst.EST_REVERSALS) == pytest.approx(44.0 / 5.0, abs=1e-12)
    assert s.estimate(yst.EST_MEDIAN_REV) == pytest.approx(9.0, abs=1e-12)
    assert s.estimate(yst.EST_TRIALS) == pytest.approx(8.9, abs=1e-12)
    assert s.estimate() == s.estimate(yst.EST_REVERSALS)
    # Updates after the stop are recorded and the reason does not move.
    s.update(s.next(), 1)
    assert s.n_trials == 11 and s.stop_reason == yst.STOP_TRIALS


def test_updown_1_2_first_trial_moves_nothing():
    s = yst.Staircase(start=20.0, n_down=2, steps=2.0, stop_trials=14)
    ev = s.update(s.next(), 1)
    assert ev == 0 and isinstance(ev, yst.Event)
    replay(s, [1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1])
    assert proposals(s) == [20, 20, 18, 18, 20, 20, 18, 20, 22, 22, 20, 20, 18, 20]
    assert s.reversals() == [(3, 18.0), (5, 20.0), (6, 18.0), (9, 22.0), (12, 18.0)]
    assert yst.convergence_p(1, 2) == pytest.approx(0.70710678118654752, abs=1e-12)


def test_updown_1_3_event_mask():
    s = yst.Staircase(start=40.0, n_down=3, steps=4.0, stop_reversals=3)
    replay(s, [1, 1, 1, 1, 0, 1, 1, 1])
    assert not s.done
    ev = s.update(s.next(), 0)
    assert ev == yst.Event.STEP | yst.Event.REVERSAL | yst.Event.DONE
    assert ev == yst.EVENT_STEP | yst.EVENT_REVERSAL | yst.EVENT_DONE
    assert proposals(s) == [40, 40, 40, 36, 36, 40, 40, 40, 36]
    assert s.stop_reason == yst.Stop.REVERSALS
    assert yst.convergence_p(1, 3, 0.0) == pytest.approx(0.79370052598409974, abs=1e-12)


def test_schedule_and_initial_rule():
    kw = dict(start=40.0, n_down=3, initial_rule=True, steps=[8.0, 4.0, 2.0],
              stop_reversals=4)
    resp = [1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1]
    s = yst.Staircase(**kw)
    replay(s, resp)
    assert proposals(s) == [40, 32, 24, 28, 28, 28, 26, 28, 30, 30, 30]
    assert s.reversals() == [(2, 24.0), (5, 28.0), (6, 26.0), (10, 30.0)]
    assert [t.step_index for t in s.history()] == [0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2]
    assert s.estimate_count(yst.EST_REVERSALS) == 2
    assert s.estimate_count(yst.EST_TRIALS) == 6
    assert s.estimate(yst.EST_REVERSALS) == pytest.approx(28.0, abs=1e-12)
    assert s.estimate(yst.EST_TRIALS) == pytest.approx(172.0 / 6.0, abs=1e-12)
    s = yst.Staircase(est_reversals=3, est_trials=4, **kw)
    replay(s, resp)
    assert s.estimate(yst.EST_REVERSALS) == pytest.approx(84.0 / 3.0, abs=1e-12)
    assert s.estimate(yst.EST_TRIALS) == pytest.approx(118.0 / 4.0, abs=1e-12)


def test_log_step_is_geometric():
    s = yst.Staircase(start=0.5, step_type=yst.STEP_LOG, steps=[0.3], stop_trials=4)
    replay(s, [1, 1])
    assert proposals(s) == pytest.approx([0.5, 0.5 / 10 ** 0.3], rel=1e-12)
    with pytest.raises(yst.ArgumentError):
        s.update(0.0, 1)     # a non-positive shown level under LOG


def test_shown_level_is_recorded_not_stepped_from():
    s = yst.Staircase(start=10.0, steps=1.0, stop_trials=3)
    s.update(9.37, 1)
    t = s.history()[0]
    assert (t.proposed, t.shown) == (10.0, 9.37)
    assert s.next() == 9.0


def test_errors_map_to_the_hierarchy():
    assert issubclass(yst.ArgumentError, yst.Error)
    assert issubclass(yst.ArgumentError, ValueError)
    assert issubclass(yst.Full, yst.Error) and issubclass(yst.Closed, yst.Error)
    with pytest.raises(yst.ArgumentError, match="stop_reversals or stop_trials"):
        yst.Staircase(start=1.0, steps=0.1)
    with pytest.raises(yst.ArgumentError):
        yst.Staircase(start=1.0, steps=0.1, stop_trials=5, rule=yst.RULE_ASA)
    with pytest.raises(yst.ArgumentError):
        yst.Staircase(start=1.0, steps=[0.1] * (yst.MAX_STEPS + 1), stop_trials=5)
    with pytest.raises(TypeError):
        yst.Staircase(1.0)                 # keyword-only
    s = yst.Staircase(start=1.0, steps=0.1, stop_trials=5)
    with pytest.raises(yst.ArgumentError):
        s.update(1.0, 2)                  # response not 0 or 1
    with pytest.raises(yst.ArgumentError):
        s.update(float("nan"), 1)
    with pytest.raises(yst.ArgumentError):
        s.estimate(99)
    # A handle whose construction failed is closed.
    bad = yst.Staircase.__new__(yst.Staircase)
    with pytest.raises(yst.Closed):
        bad.next()
    with pytest.raises(yst.Closed):
        bad.update(1.0, 1)
    assert not bad.is_open and not bad.done


def test_full_at_max_trials():
    s = yst.Staircase(start=0.0, steps=1.0, stop_trials=5)
    for i in range(yst.MAX_TRIALS):
        s.update(s.next(), i % 2)
    assert s.stop_reason == yst.Stop.TRIALS
    with pytest.raises(yst.Full):
        s.update(s.next(), 1)


def test_asa_and_weighted():
    assert yst.weighted_scale(0.75) == pytest.approx(1.0 / 3.0)
    assert math.isnan(yst.weighted_scale(1.5))
    assert math.isnan(yst.convergence_p(2, 2))
    assert yst.convergence_p(1, 1, yst.weighted_scale(0.8)) == pytest.approx(0.8)
    s = yst.Staircase(start=0.0, rule=yst.Rule.ASA, target_p=0.75, steps=1.0, stop_trials=10)
    s.update(s.next(), 1)   # m = 1: level -= 1 * (1 - 0.75)
    assert s.next() == pytest.approx(-0.25)
    assert s.estimate(yst.EST_LAST) == s.next()


def test_simulate_response():
    assert yst.simulate_response(0.7, 0.69) == 1
    assert yst.simulate_response(0.7, 0.70) == 0
    assert yst.strerror(yst.ERR_FULL) == "trial history is full"


def test_enums_are_int_enums():
    assert yst.Rule.UPDOWN == 0 and yst.RULE_ASA is yst.Rule.ASA
    assert yst.StepType.DB == 2 and yst.Estimator.LAST == 3
    assert [m.name for m in yst.Stop] == ["NONE", "REVERSALS", "TRIALS", "LIMIT", "FULL"]
    assert yst.Trial._fields == ("proposed", "shown", "response", "reversal",
                                "direction", "step_index")


def test_limits_and_stop_at_limit():
    s = yst.Staircase(start=5.0, steps=1.0, min=1.0, max=5.0, stop_trials=50,
                     stop_at_limit=3)
    assert s.update(s.next(), 0) == yst.Event.STEP     # clamped, still a step
    assert s.next() == 5.0
    s.update(s.next(), 0)
    assert s.update(s.next(), 0) == yst.Event.STEP | yst.Event.DONE
    assert s.stop_reason == yst.Stop.LIMIT
    s = yst.Staircase(start=2.0, steps=1.0, min=0.0, max=4.0, use_limits=True, stop_trials=4)
    replay(s, [0, 0, 0])
    assert s.next() == 4.0


def test_version_matches_pyproject():
    # pyproject.toml's version is a copy; __version__ comes from the header
    # compiled into the extension. Fail when the two drift.
    path = os.path.join(os.path.dirname(__file__), "..", "pyproject.toml")
    if not os.path.exists(path):
        pytest.skip("pyproject.toml is not beside the tests")
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    assert m, "no version in pyproject.toml"
    assert yst.__version__ == m.group(1)
