"""A real window, briefly: local only, not in CI (no display worth timing there).

    set YSP_VIS_WINDOW=1, then: pytest bindings/python/ysp_vis/tests/test_window.py

Opens a 1280 x 720 window on the AUTO backend with the bundled ANGLE, draws
two seconds of frames with the phase A draw path, routes the window calls
through the window thread, idles 6 s without any call (past the 4.1 to
4.6 s after which Windows marks a window that pumps nothing as hung), and
checks the frames after the gap.
"""
import collections
import os
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


def test_window_frames_records_and_idle():
    with yscr.Screen(windowed=True, size=(1280, 720)) as s, ygfx.Gfx(s, background=0.25) as g:
        line = s.describe()
        assert "2.1.25848" in line, line          # the bundled ANGLE, not another in the process
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
        print(line)
        print("before the gap:", collections.Counter((r.path, r.tier) for r in before))
        print("after the gap:", [(r.index, r.dropped, r.path, r.tier, hex(r.flags)) for r in recs[:5]])
