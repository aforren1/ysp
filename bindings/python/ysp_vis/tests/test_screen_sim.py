"""Tests for ysp-vis on the simulated display (backend='sim'): no window, no GPU.

tests/adapt/ checks ysp/screen.h itself. Here: the binding drives it as the C
API does. The package and its shared tables, the options and their refusals,
the frame loop (begin, flip, flip_at, wait_flip) and its records in seconds
with exact ns, the record ring, the order errors, aborts, events, window
calls without a window, the phase A draw path on gfx's null backend, close.
tests/test_window.py opens a real window (local only).
"""
import math
import os
import re
import threading

import pytest

import ysp.audio
import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline
import ysp.video as yvid
from ysp import _vis

HERE = os.path.dirname(os.path.abspath(__file__))
PERIOD = 0.004    # a fast simulated display keeps the tests short
PERIOD_NS = 4_000_000


def sim(**kw):
    kw.setdefault("backend", "sim")
    kw.setdefault("sim_period", PERIOD)
    return yscr.Screen(**kw)


def test_version_matches_pyproject():
    with open(os.path.join(HERE, "..", "pyproject.toml")) as f:
        v = re.search(r'^version = "([^"]+)"', f.read(), re.M).group(1)
    assert yscr.__version__ == v
    assert ygfx.__version__ == v and yvid.__version__ == v
    # The distribution tracks screen.h's version, with a fourth number for a
    # change of the binding alone (docs/bindings.md, ysp-vis).
    assert v == yscr.HEADER_VERSION or v.startswith(yscr.HEADER_VERSION + ".")


def test_headers_and_shared_tables():
    s = _vis._shared()
    assert s["screen"] == yscr.HEADER_VERSION
    assert s["gfx"] == ygfx.HEADER_VERSION
    assert s["video"] == yvid.HEADER_VERSION
    # The owners this module calls through: the installed ysp.audio and
    # ysp.timeline, of the major.minor it was built against.
    assert s["audio"] == ysp.audio.__version__
    assert s["timeline"].split(".")[:2] == ysp.timeline.__version__.split(".")[:2]
    assert s["audio"].split(".")[:2] == s["audio_built"].split(".")[:2]
    assert s["timeline"].split(".")[:2] == s["timeline_built"].split(".")[:2]
    assert s["abi"] == 1


def test_angle_dir_is_the_package_directory():
    d = os.path.normcase(os.path.abspath(yscr.ANGLE_DIR))
    assert d == os.path.normcase(os.path.dirname(os.path.abspath(_vis.__file__)))
    assert os.path.exists(os.path.join(d, "libEGL.dll"))
    assert os.path.exists(os.path.join(d, "libGLESv2.dll"))
    assert os.path.exists(os.path.join(d, "ysp_vis-LICENSE.SDL3.txt"))
    assert os.path.exists(os.path.join(d, "ysp_vis-LICENSE.ANGLE.txt"))


def test_no_sdl3_dll_beside_the_module():
    # SDL3 is linked statically (docs/av_probe.md, section 5).
    d = os.path.dirname(os.path.abspath(_vis.__file__))
    assert not os.path.exists(os.path.join(d, "SDL3.dll"))


def test_constants():
    assert yscr.TIER_1 == 1 and yscr.TIER_SIM == 4
    assert yscr.PATH_SIMULATED == 4
    assert yscr.ABORT_KEY == 1 and yscr.ABORT_PANIC == 0x20
    assert yscr.FLIP_LATE_TARGET == 4
    assert yscr.PRESS == 1 and yscr.KIND_MOUSE == 1


def test_options_list():
    o = yscr.options()
    for k in ("windowed", "size", "display", "mode", "backend", "lead", "onset_offset", "settle", "window_thread",
              "panic", "patch", "depth", "foreground"):
        assert k in o and isinstance(o[k], str)


def test_option_refusals():
    with pytest.raises(TypeError):
        yscr.Screen(backend="sim", no_such_option=1)
    with pytest.raises(TypeError):
        yscr.Screen("sim")
    with pytest.raises(yscr.ArgumentError):
        yscr.Screen(backend="nope")
    with pytest.raises(yscr.ArgumentError):
        yscr.Screen(backend="sim", sim_period=-0.01)
    with pytest.raises(yscr.ArgumentError):
        yscr.Screen(backend="sim", sim_period=math.nan)
    with pytest.raises(yscr.ArgumentError):
        yscr.Screen(backend="sim", size=(800,))
    with pytest.raises(yscr.ArgumentError):
        yscr.Screen(backend="sim", depth=1.5)
    # The header's own refusals come back as Error with its message.
    with pytest.raises(yscr.Error, match="lead"):
        yscr.Screen(backend="sim", lead=2.0)


def test_frame_loop_records():
    with sim() as s:
        assert s.open
        c = s.caps()
        assert c["backend"] == "sim" and c["period_ns"] == PERIOD_NS
        assert abs(c["period"] - PERIOD) < 1e-12
        assert "sim" in s.describe()
        last = None
        frames = []
        for i in range(30):
            f = s.begin()
            assert isinstance(f, yscr.Frame)
            assert f.index == i
            assert f.period_ns == PERIOD_NS
            assert f.onset == f.onset_ns / 1e9
            if last is not None:
                assert f.onset_ns > last.onset_ns
            for r in f.done:
                assert isinstance(r, yscr.Record)
            frames.append(f)
            last = f
            s.flip()
        r = s.wait_flip()
        assert r.index == 29
        recs = s.records()
        assert [x.index for x in recs] == list(range(30))
        for x in recs:
            assert x.path == yscr.PATH_SIMULATED and x.tier == yscr.TIER_SIM
            assert x.onset == x.onset_ns / 1e9 and x.residual_ns == x.onset_ns - x.target_ns
            assert x.missed == (x.dropped > 0)
            assert len(x.phases) == 6 and x.phases[yscr.PHASE_GPU] is None
        # flip() targets the predicted onset: the record's target is the frame's.
        assert recs[5].target_ns == frames[5].onset_ns
        assert s.records() == []
        assert s.records_lost == 0
        # frame.done carried every record once, in completion order
        done = [r.index for f in frames for r in f.done]
        assert done == list(range(len(done)))


def test_flip_at_later_and_late():
    with sim() as s:
        f = s.begin()
        s.flip_at(f.onset + 3 * f.period)       # three frames later
        r = s.wait_flip()
        assert r.target_ns == f.onset_ns + 3 * PERIOD_NS
        assert r.onset_ns == r.planned_ns
        assert abs(r.residual) < 1e-6
        assert not r.late
        f = s.begin()
        s.flip_at(f.onset - 10 * f.period)       # the caller was late
        r = s.wait_flip()
        assert r.late and (r.flags & yscr.FLIP_LATE_TARGET)
        with pytest.raises(yscr.ArgumentError):
            s.begin()
            s.flip_at(-1.0)
        s.flip()


def test_order_errors():
    with sim() as s:
        with pytest.raises(yscr.OrderError):
            s.flip()
        s.begin()
        with pytest.raises(yscr.OrderError):
            s.begin()
        s.flip()
        with pytest.raises(yscr.ArgumentError):
            s.mark(yscr.PHASE_SWAP)


def test_marks_go_to_phases():
    with sim() as s:
        s.begin()
        s.mark(yscr.PHASE_EVALUATE)
        s.flip()
        r = s.wait_flip()
        assert r.phases[yscr.PHASE_EVALUATE] is not None


def test_request_abort_raises_abort_once():
    with sim() as s:
        s.begin()
        s.flip()
        yscr.request_abort()
        with pytest.raises(yscr.Abort) as e:
            s.begin()
        assert e.value.reasons & yscr.ABORT_REQUEST
        assert e.value.panic is False
        assert isinstance(e.value.t, float) and isinstance(e.value.t_ns, int)
        assert not isinstance(e.value, yscr.Error)
        f = s.begin()   # the program decides: the next begin starts a frame
        s.flip()
        assert f.index >= 1


def test_events_and_window_calls_without_a_window():
    with sim() as s:
        assert s.poll() == []
        assert s.events_lost == 0
        with pytest.raises(yscr.NotImplementedError):
            s.title("block 2")
        with pytest.raises(yscr.NotImplementedError):
            s.cursor(True)
        with pytest.raises(yscr.NotImplementedError):
            s.grab(False)


def test_settle_sync_patch():
    with sim(patch=True, patch_corner="bottom_left") as s:
        st = s.settle()
        assert st["result"] == yscr.SETTLE_SKIPPED
        assert s.sync()["untimed"] is False
        s.set_patch(1.0)
        s.begin()
        s.flip()
        with pytest.raises(yscr.ArgumentError):
            s.set_patch(math.nan)


def test_closed_and_reopen():
    s = sim()
    s.close()
    assert not s.open
    with pytest.raises(yscr.ClosedError):
        s.begin()
    s.close()   # twice is fine
    with sim() as s2:
        s2.begin()
        s2.flip()


def test_busy_from_another_thread():
    with sim(sim_period=0.05) as s:
        s.begin()
        s.flip()
        started = threading.Event()
        err = []

        def other():
            started.set()
            try:
                s.wait_flip()
            except Exception as e:   # noqa: BLE001
                err.append(e)

        t = threading.Thread(target=other)
        t.start()
        started.wait()
        # wait_flip() runs without the GIL; a second call meanwhile is refused
        # or finds it done (both are fine), never both inside the header.
        try:
            s.records()
        except yscr.BusyError:
            pass
        t.join()
        assert not err


def test_gfx_phase_a_on_the_null_backend():
    with sim() as s:
        with ygfx.Gfx(s, background=0.5) as g:
            assert g.open and g.screen is s
            with pytest.raises(yscr.OrderError):
                g.begin()            # before the screen's begin()
            for _ in range(3):
                s.begin()
                g.begin()
                g.rect(0, 0, 100, 50, (1.0, 0.0, 0.0))
                g.rect(10, 10, 5, 5, 0.25)
                with pytest.raises(yscr.OrderError):
                    s.flip()         # end() first
                g.end()
                s.flip()
            with pytest.raises(yscr.ArgumentError):
                s.begin()
                g.begin()
                try:
                    g.rect(0, 0, 1, 1, (1, 2))
                finally:
                    g.end()
                    s.flip()
            with pytest.raises(yscr.RefusedError):
                ygfx.Gfx(s)          # one per screen in phase A
        assert not g.open
    # A closed screen closes its gfx.
    s = sim()
    g = ygfx.Gfx(s)
    s.close()
    assert not g.open
    with pytest.raises(yscr.ClosedError):
        g.begin()


def test_gfx_refuses_a_non_screen():
    with pytest.raises(TypeError):
        ygfx.Gfx(object())


def test_now():
    a = yscr.now()
    b = yscr.now_ns()
    assert abs(b / 1e9 - a) < 1.0
    # One clock with ysp.audio's (ysp/rt.h's real clock in both modules).
    assert abs(ysp.audio.now() - yscr.now()) < 0.5
