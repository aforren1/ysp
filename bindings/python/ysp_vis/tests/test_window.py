"""A real window, briefly: local only, not in CI (no display worth timing there).

    set YSP_VIS_WINDOW=1, then: pytest bindings/python/ysp_vis/tests/test_window.py

Opens a 1280 x 720 window on the AUTO backend with the bundled ANGLE, draws
two seconds of frames with the phase A draw path, routes the window calls
through the window thread, idles 6 s without any call (past the 4.1 to
4.6 s after which Windows marks a window that pumps nothing as hung), and
checks the frames after the gap. On macOS the backend is METAL, with no
window thread: the window's calls run on the main thread, which runs the
test; a call from another thread on the open screen is refused.
"""
import collections
import os
import sys
import threading
import time

import pytest

import ysp.gfx as ygfx
import ysp.screen as yscr

pytestmark = pytest.mark.skipif(os.environ.get("YSP_VIS_WINDOW") != "1", reason="set YSP_VIS_WINDOW=1 (opens a window)")


def frames(s, g, n, color):
    out = []
    for i in range(n):
        f = s.begin()
        g.begin()
        g.rect(0, 0, 200, 120, color)
        g.end()
        s.flip()
        out.append(f)
    return out


# The bundled ANGLE (tools/vendor_vis.py), not another in the process.
ANGLE = "2.1.28042/b4f8ea825136" if sys.platform == "darwin" else "2.1.25848"


def test_window_frames_records_and_idle():
    with yscr.Screen(windowed=True, size=(1280, 720)) as s, ygfx.Gfx(s, background=0.25) as g:
        line = s.describe()
        assert ANGLE in line, line
        c = s.caps()
        if sys.platform == "darwin":
            # ysp/screen.h 0.12.0 and 0.15.0 (BACKENDS, METAL; INPUT)
            assert c["backend"] == "metal" and "backend=metal" in line
            assert c["max_in_flight"] == 2 and c["margin_ns"] == 2000000 and c["os_input_stamps"] is True
        s.title("ysp-vis test")
        s.cursor(True)
        s.grab(False)
        frames(s, g, 120, (1.0, 1.0, 1.0))
        s.wait_flip()
        recs = s.records()
        assert len(recs) == 120
        assert all(r.path in (yscr.PATH_COMPOSED, yscr.PATH_OVERLAY, yscr.PATH_INDEPENDENT) for r in recs)
        # The path and the tier are the system's (a small window is composed, tier 3); printed, not asserted.
        assert all(r.tier in (yscr.TIER_1, yscr.TIER_2, yscr.TIER_3) for r in recs)
        before = recs
        time.sleep(6.0)                            # no call at all: the window thread keeps the window alive
        after = frames(s, g, 60, (0.0, 1.0, 0.0))
        s.wait_flip()
        recs = s.records()
        assert len(recs) == 60
        assert after[0].index == 120
        assert s.poll() is not None
        if sys.platform == "darwin":
            # shown flips are tier 2 (presentedTime); a hidden or locked
            # screen gives estimated ones (tier 3)
            shown = [r for r in before if r.tier == yscr.TIER_2]
            assert len(shown) >= 100, collections.Counter(r.tier for r in before)
        print(line)
        print("before the gap:", collections.Counter((r.path, r.tier) for r in before))
        print("after the gap:", [(r.index, r.dropped, r.path, r.tier, hex(r.flags)) for r in recs[:5]])


@pytest.mark.skipif(sys.platform != "darwin", reason="macOS: a window screen runs on the main thread only")
def test_calls_off_the_main_thread_are_refused():
    with yscr.Screen(windowed=True, size=(640, 480)) as s, ygfx.Gfx(s, background=0.25) as g:
        errs = []

        def run():
            for call in (s.begin, s.flip, s.close, g.begin, g.close, s.describe):
                try:
                    call()
                    errs.append(None)
                except yscr.Error as e:
                    errs.append(e)

        t = threading.Thread(target=run)
        t.start()
        t.join()
        assert all(e is not None and "main thread" in str(e) for e in errs), errs
        assert s.open                              # nothing reached the screen
        frames(s, g, 30, (1.0, 1.0, 1.0))           # the main thread goes on
        s.wait_flip()
        assert len(s.records()) == 30


# The binding's stimuli, made by name, and the same ones made in C with
# ysp/gfx.h's constructors (yv_gfx_c_scene() in bindings/share/ysp_vis_gfx.h).
def c_twins(g):
    return [
        g.gabor(x=-100, y=40, ori=30, sf=1 / 16, phase=0.25, sigma=12, contrast=0.4),
        g.grating(x=80, y=-30, w=120, ori=-20, sf=1 / 20, contrast=0.3, aperture="circle", edge="cosine", edge_width=6),
        g.shape("rect", w=160, h=90, ori=10, radius=12, color=(0.9, 0.2, 0.1), stroke=4, stroke_align="inside",
                edge="gaussian", edge_width=1),
        g.shape("star", place="top_left", anchor="top_left", x=20, y=20, w=100, points=5, inner=0.4,
                color=(0.1, 0.8, 0.3), opacity=0.75),
        g.noise(x=150, y=60, w=64, check=4, seed=7, dist="binary", contrast=0.25, aperture="circle"),
    ]


def scene(s, g, draw):
    s.begin()
    g.begin()
    draw()
    g.end()
    w, h = (int(v) for v in g.size)
    out = bytes(g.read_scene(0, 0, w, h))   # waits for the GPU: a test, not a trial
    s.flip()
    return out


def test_pixels_match_the_c_path():
    with yscr.Screen(windowed=True, size=(800, 600)) as s, ygfx.Gfx(s, background=0.5) as g:
        empty = scene(s, g, lambda: None)
        for k, st in enumerate(c_twins(g)):
            c = scene(s, g, lambda: g._c_scene(k))
            b = scene(s, g, lambda: g.draw(st))
            assert b != empty, k                    # the stimulus is drawn
            assert b == c, k                        # the same scene, bit for bit, as the C path


def test_instances_match_single_stimuli():
    np = pytest.importorskip("numpy")
    with yscr.Screen(windowed=True, size=(800, 600)) as s, ygfx.Gfx(s, background=0.5) as g:
        xy = [(-150, -60), (0, 0), (120, 80)]
        oris = [0, 30, 75]
        singles = g.list([g.gabor(sf=1 / 12, sigma=8, x=x, y=y, ori=o, contrast=0.4) for (x, y), o in zip(xy, oris)])
        I = g.instances(g.gabor(sf=1 / 12, sigma=8, contrast=0.4), 3, fields=("xy", "ori"))
        e = np.asarray(I.elements)
        e[:, 0:2] = xy
        e[:, 2] = oris
        g.prime()
        a = np.frombuffer(scene(s, g, lambda: g.draw(singles)), np.float32)
        b = np.frombuffer(scene(s, g, lambda: g.draw(I)), np.float32)
        # ysp/gfx.h STATUS: each element within 9.2e-6 of the same stimulus drawn alone
        assert np.abs(a - b).max() < 2e-5
        assert np.abs(a - 0.5).max() > 0.1


def test_text_with_the_system_font():
    # The default font: Segoe UI or Arial on Windows, Arial or Helvetica on
    # macOS (bindings/share/ysp_vis_text.h, yv_default_font_path).
    with yscr.Screen(windowed=True, size=(800, 600)) as s, ygfx.Gfx(s, background=0.5) as g:
        empty = scene(s, g, lambda: None)
        t = g.text("Hello, world", size=40, color=(1.0, 1.0, 1.0))
        assert t.n_glyphs == 12 and t.missing == 0
        assert ("Arial.ttf#0=" if sys.platform == "darwin" else "segoeui.ttf#0=") in t.stamp, t.stamp
        b = scene(s, g, lambda: g.draw(t))
        assert b != empty
