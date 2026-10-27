"""Text in a real window: local only, not in CI.

    set YSP_VIS_WINDOW=1, then: pytest bindings/python/ysp_vis/tests/test_text_window.py

Texts made through the binding draw the same scene, bit for bit, as the same
texts made in C with pack/layout and ygfx_crun() (yv_ctext_setup() in
bindings/share/ysp_vis_text.h, as examples/layout/gfx_layout.c makes them);
the ink drawn lies inside the glyph boxes that glyphs() reports; and a text
changed and changed back draws its first scene again. Needs Segoe UI and
Microsoft YaHei (Windows's).
"""
import os

import pytest

import ysp.gfx as ygfx
import ysp.screen as yscr

pytestmark = pytest.mark.skipif(os.environ.get("YSP_VIS_WINDOW") != "1", reason="set YSP_VIS_WINDOW=1 (opens a window)")

FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
SEGOE = os.path.join(FONTS, "segoeui.ttf")
YAHEI = os.path.join(FONTS, "msyh.ttc")


def scene(s, g, draw):
    s.begin()
    g.begin()
    draw()
    g.end()
    w, h = (int(v) for v in g.size)
    out = bytes(g.read_scene(0, 0, w, h))   # waits for the GPU: a test, not a trial
    s.flip()
    return out


def twins(g):
    """The binding's texts of the C path's two (yv_ctext_setup())."""
    f = g.font([SEGOE, YAHEI])
    return [
        g.text("Hello, world", size=40, x=-120, y=-80, color=(0.9, 0.6, 0.2)),
        g.text("Ab 中文 مرحبا 12", font=f, size=36, width=220, align="start", line_height=1.2,
               spans=[(0, 2, (0.1, 0.8, 0.2))], color=(0.8, 0.8, 0.8), ori=20, place="top_left",
               anchor="top_left", x=60, y=300),
    ]


def test_texts_match_the_c_path():
    for p in (SEGOE, YAHEI):
        if not os.path.exists(p):
            pytest.skip("no %s" % p)
    with yscr.Screen(windowed=True, size=(800, 600)) as s, ygfx.Gfx(s, background=0.25) as g:
        g._c_text_setup()
        empty = scene(s, g, lambda: None)
        texts = twins(g)
        for k, t in enumerate(texts):
            c = scene(s, g, lambda: g._c_text(k))
            b = scene(s, g, lambda: g.draw(t))
            assert b != empty, k                    # the text is drawn
            assert b == c, k                        # the same scene, bit for bit, as the C path


def test_ink_inside_the_glyph_boxes_and_a_change_back():
    np = pytest.importorskip("numpy")
    if not os.path.exists(SEGOE):
        pytest.skip("no Segoe UI")
    with yscr.Screen(windowed=True, size=(800, 600)) as s, ygfx.Gfx(s, background=0.0) as g:
        t = g.text("Typography, kerning: AV To", size=44, color=1.0, x=-30, y=20)
        a = np.frombuffer(scene(s, g, lambda: g.draw(t)), np.float32).reshape(600, 800, 4)
        ink = a[:, :, 0] > 0
        mask = np.zeros_like(ink)
        for _, _, x0, y0, x1, y1 in t.glyphs():
            # a pixel is touched when its box meets the glyph's (box-filter coverage)
            mask[max(0, int(np.floor(y0))):int(np.ceil(y1)), max(0, int(np.floor(x0))):int(np.ceil(x1))] = True
        assert ink.sum() > 1000
        assert not (ink & ~mask).any()
        # the words' boxes hold their glyphs' ink too
        wmask = np.zeros_like(ink)
        for _, _, _, _, x0, y0, x1, y1 in t.words():
            wmask[int(np.floor(y0)):int(np.ceil(y1)), int(np.floor(x0)):int(np.ceil(x1))] = True
        assert not (ink & ~wmask).any()
        t.text = "something else"
        b = np.frombuffer(scene(s, g, lambda: g.draw(t)), np.float32)
        t.text = "Typography, kerning: AV To"
        c = np.frombuffer(scene(s, g, lambda: g.draw(t)), np.float32)
        assert (b != a.ravel()).any() and (c == a.ravel()).all()
