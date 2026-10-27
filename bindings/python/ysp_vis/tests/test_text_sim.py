"""ysp.gfx's text on the simulated display (backend='sim'): gfx's null backend
draws nothing, so these tests check what the binding does: fonts (paths,
bytes, fallback lists, the system's default), the layout (Skribidi with
HarfBuzz: bidi, scripts, wrapping, spans), a change of text between
trials, the glyph, word and line boxes and hit tests, text in lists and
bindings, the refusals, and the lifetime rules (a text kept after its gfx
closes fails cleanly; a Font lives as long as a text uses it).
tests/test_text_window.py checks pixels against the C path in a real window
(local only).

The fonts are Windows's (Segoe UI, Arial, Microsoft YaHei): a test that
needs one the machine lacks is skipped.
"""
import gc
import hashlib
import math
import os

import pytest

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl

FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
SEGOE = os.path.join(FONTS, "segoeui.ttf")
ARIAL = os.path.join(FONTS, "arial.ttf")
YAHEI = os.path.join(FONTS, "msyh.ttc")


def need(*paths):
    for p in paths:
        if not os.path.exists(p):
            pytest.skip("no %s on this machine" % p)


@pytest.fixture
def sg():
    need(SEGOE)
    with yscr.Screen(backend="sim", sim_period=0.004) as s:
        g = ygfx.Gfx(s, background=0.5)
        yield s, g
        g.close()


def frame(s, g, *items):
    f = s.begin()
    g.frame(*items)
    s.flip()
    return f


def inside(inner, outer, tol=1e-3):
    return (inner[0] >= outer[0] - tol and inner[1] >= outer[1] - tol and inner[2] <= outer[2] + tol
            and inner[3] <= outer[3] + tol)


def test_a_text_and_its_layout(sg):
    s, g = sg
    t = g.text("Hello, world", size=40, color=(1.0, 0.0, 0.0))
    assert isinstance(t, ygfx.Text) and isinstance(t, ygfx.Stim) and t.kind == "text"
    assert t.text == "Hello, world" and t.size == 40 and t.width == 0 and t.align == "center"
    assert t.dir == "auto" and t.resolved_dir == "ltr" and t.lang == "" and t.font is None
    assert t.n_lines == 1 and t.n_glyphs == 12 and t.n_chars == 12 and t.n_runs == 1 and t.missing == 0
    w, h = t.box
    assert 150 < w < 300 and 40 < h < 70                     # Segoe UI's line at 40 px per em
    assert t.color == (1.0, 0.0, 0.0) and t.x == 0 and t.anchor == (0.5, 0.5)
    assert t.layout_time > 0 and t.build_time >= 0 and t.upload_time >= 0 and t.new_glyphs > 0
    # the stamp: the layout's versions and the font's SHA-256
    with open(SEGOE, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    assert "skribidi=dee63d6" in t.stamp and "harfbuzz=14.6.0" in t.stamp and ("segoeui.ttf#0=" + sha) in t.stamp
    # the box is centered on the screen (800 x 600), as every stimulus's
    x0, y0, x1, y1 = t.bounds()
    assert x0 == pytest.approx(400 - w / 2) and y1 == pytest.approx(300 + h / 2)
    frame(s, g, t)


def test_layout_again_between_trials(sg):
    s, g = sg
    t = g.text("RED", size=48)
    n0 = t.n_glyphs
    t.text = "GREEN"
    assert t.text == "GREEN" and t.n_glyphs == 5 and n0 == 3
    t.set(text="BLUE", color=(0.0, 0.0, 1.0), x=10)
    assert t.text == "BLUE" and t.color == (0.0, 0.0, 1.0) and t.x == 10
    t.text = "RED"                                           # its glyphs are built already
    assert t.new_glyphs == 0
    t.size = 24
    assert t.box[1] < 40
    t.width = 0.0
    frame(s, g, t)
    # inside a frame a layout is refused (it uploads glyphs); the rest is not
    s.begin()
    g.begin()
    with pytest.raises(yscr.OrderError):
        t.text = "late"
    t.x = 20
    g.draw(t)
    g.end()
    s.flip()
    assert t.text == "RED" and t.x == 20


def test_bidi_scripts_and_wrapping(sg):
    s, g = sg
    ar = "مرحبا بالعالم"
    t = g.text(ar, size=40)
    assert t.resolved_dir == "rtl" and t.missing == 0
    words = t.words()
    assert [w[0] for w in words] == ["مرحبا", "بالعالم"]
    # right to left: the first word is to the right of the second
    assert words[0][4] > words[1][6] - 1e-3
    for w in words:
        assert w[0] == ar[w[1]:w[2]]
    t.dir = "ltr"
    assert t.resolved_dir == "ltr"
    # wrapping in a box of 200 px: more lines, each within the box
    p = g.text("The quick brown fox jumps over the lazy dog " * 3, size=24, width=200, align="start")
    assert p.n_lines > 3 and p.box[0] == 200
    lines = p.lines()
    assert len(lines) == p.n_lines
    # every word within the box; a line's box takes its trailing space, which may hang past it
    x0 = p.bounds()[0]
    assert all(w[6] <= x0 + 200 + 1e-3 for w in p.words())
    assert max(l[4] - l[2] for l in lines) > 200
    assert lines[0][0] == 0 and all(lines[i][1] <= lines[i + 1][0] for i in range(len(lines) - 1))
    p.line_height = 1.5
    assert p.lines()[1][3] - p.lines()[0][3] > lines[1][3] - lines[0][3]
    for a in ("start", "center", "end"):
        p.align = a
        assert p.align == a
    frame(s, g, t, p)


def test_fonts_paths_bytes_lists(sg):
    s, g = sg
    need(ARIAL)
    f = g.font(ARIAL)
    assert isinstance(f, ygfx.Font) and f.info[0]["name"] == "arial.ttf" and f.info[0]["units_per_em"] == 2048
    with open(ARIAL, "rb") as fp:
        raw = fp.read()
    assert f.info[0]["sha256"] == hashlib.sha256(raw).hexdigest()
    fb = g.font(raw, name="Arial (bytes)")
    assert fb.info[0]["sha256"] == f.info[0]["sha256"] and fb.info[0]["name"] == "Arial (bytes)"
    t = g.text("Arial", font=f, size=30)
    assert t.font is f and "arial.ttf#0=" in t.stamp
    t2 = g.text("Arial", font=fb, size=30)
    assert t2.box == t.box                                   # the same bytes, the same layout
    # a path: the gfx makes the font once and keeps it
    a = g.text("path", font=ARIAL, size=30)
    b = g.text("path", font=ARIAL, size=30)
    assert a.font is None and b.new_glyphs == 0               # b found a's font and glyphs
    # fallback: CJK in Segoe UI is missing; with YaHei after it, not
    if os.path.exists(YAHEI):
        m = g.text("abc 中文", font=SEGOE, size=30)
        assert m.missing == 2
        fl = g.font([SEGOE, YAHEI])
        c = g.text("abc 中文", font=fl, size=30)
        assert c.missing == 0 and c.n_runs == 2
        c2 = g.text("abc 中文", font=[SEGOE, YAHEI], size=30)  # a list of paths: made once too
        assert c2.missing == 0 and c2.box == c.box
        assert [i["name"] for i in fl.info] == ["segoeui.ttf", "msyh.ttc"]
        frame(s, g, m, c, c2)
    with pytest.raises(yscr.Error, match="does not open"):
        g.font("C:/no/such/font.ttf")
    with pytest.raises(yscr.ArgumentError):
        g.font(b"not a font, just bytes")
    with pytest.raises(yscr.ArgumentError):
        g.text("x", font=12)


def test_spans(sg):
    s, g = sg
    t = g.text("one two three", size=30, color=0.8, spans=[(0, 3, (1.0, 0.0, 0.0)), (8, 13, None, 45)])
    assert t.spans == [(0, 3, (1.0, 0.0, 0.0), 0.0), (8, 13, None, 45.0)]
    assert t.n_runs == 2                                     # two sizes
    big = [gl for gl in t.glyphs() if gl[0] >= 8]
    small = [gl for gl in t.glyphs() if gl[0] < 3]
    assert max(b[5] - b[3] for b in big) > max(b[5] - b[3] for b in small)
    frame(s, g, t)
    t.spans = []
    assert t.spans == [] and t.n_runs == 1
    with pytest.raises(yscr.ArgumentError, match="overlapping"):
        t.spans = [(0, 5), (3, 8)]
    with pytest.raises(yscr.ArgumentError, match="passes"):
        t.spans = [(0, 50)]
    with pytest.raises(yscr.ArgumentError, match="colors"):
        t.spans = [(i, i + 1, (0.1, 0.2, 0.3)) for i in range(16)]
    with pytest.raises(yscr.ArgumentError):
        t.spans = [(3, 1)]
    assert t.spans == []                                     # a refused set leaves the spans


def test_glyph_word_line_boxes_and_hits(sg):
    s, g = sg
    t = g.text("Read this word", size=40, x=-50, y=30)
    b = t.bounds()
    gl = t.glyphs()
    assert len(gl) == t.n_glyphs == 14
    assert [x[0] for x in gl] == list(range(14))             # one glyph per character here, in order
    for x in gl:
        assert inside(x[2:], b)
    words = t.words()
    assert [w[0] for w in words] == ["Read", "this", "word"]
    assert [(w[1], w[2]) for w in words] == [(0, 4), (5, 9), (10, 14)]
    for w in words:
        assert inside(w[4:], b) and w[3] == 0
        # every glyph of the word is inside the word's box
        for x in gl:
            if w[1] <= x[0] < w[2]:
                assert inside(x[2:], w[4:], 0.5)
    assert words[0][6] <= words[1][4] and words[1][6] <= words[2][4]   # left to right, apart
    # hit tests at each glyph's ink center
    for x in gl:
        if x[0] in (4, 9):
            continue                                         # the spaces have no ink
        cx, cy = (x[2] + x[4]) / 2, (x[3] + x[5]) / 2
        assert t.hit_index(cx, cy) == x[0]
        assert t.hit(cx, cy)
    assert t.hit_index(5, 5) == -1 and not t.hit(5, 5)
    # turned: the boxes follow the turn (axis-aligned bounds of the turned boxes)
    t.ori = 90
    w2 = t.words()
    assert w2[0][7] - w2[0][5] > w2[0][6] - w2[0][4]          # a word is now taller than wide
    g2 = t.glyphs()[0]
    assert t.hit_index((g2[2] + g2[4]) / 2, (g2[3] + g2[5]) / 2) == 0
    # lists: the topmost item and the glyph's character
    L = g.list([g.shape("rect", w=800, h=600, color=0.2), g.text("XYZ", size=60)])
    item, ch = L.hit_element(400, 300)
    assert item == 1 and ch in (0, 1, 2)


def test_lists_bulk_sets_and_bindings(sg):
    s, g = sg
    texts = [g.text(w, size=32, x=-200 + 100 * i) for i, w in enumerate(["red", "green", "blue", "pink"])]
    L = g.list(texts)
    L.set("color", (0.2, 0.4, 0.6))
    L.set("y", [1, 2, 3, 4])
    assert texts[2].color == pytest.approx((0.2, 0.4, 0.6)) and texts[3].y == 4
    with pytest.raises(yscr.ArgumentError, match="text by text"):
        L.set("size", 20)
    tl = ytl.Timeline(4)
    g.bind(texts[0], "opacity", tl, 0)
    g.bind(texts[1], "x", tl, 1)
    g.bind(texts[2], "color", tl, [2, 2, 2])
    with pytest.raises(yscr.ArgumentError, match="scale"):
        g.bind(texts[0], "size", tl, 3)
    f = s.begin()
    tl.set_keys(0, 0, [(0.0, 0.25)])
    tl.set_keys(1, 0, [(0.0, 77.0)])
    tl.set_keys(2, 0, [(0.0, 0.5)])
    tl.evaluate(f.onset, f.period)
    g.frame(L)
    s.flip()
    assert texts[0].opacity == 0.25 and texts[1].x == 77.0 and texts[2].color == (0.5, 0.5, 0.5)
    assert g.unbind(timeline=tl) == 5
    tl.close()
    # a group moves a text as any stimulus
    grp = g.group(x=50, scale=2)
    texts[3].group = grp
    frame(s, g, L)
    assert texts[3].group is grp


def test_refusals(sg):
    s, g = sg
    t = g.text("x")
    with pytest.raises(yscr.ArgumentError, match="width"):
        g.text("x", w=100)
    with pytest.raises(yscr.ArgumentError):
        g.text("x", size=0)
    with pytest.raises(yscr.ArgumentError, match="start, center or end"):
        g.text("x", align="left")
    with pytest.raises(yscr.ArgumentError):
        g.text("x", dir="up")
    with pytest.raises(yscr.ArgumentError, match="colour"):
        g.text("x", colour=1)
    with pytest.raises(yscr.ArgumentError):
        g.text(b"bytes")
    with pytest.raises(yscr.ArgumentError, match="copy"):
        t.copy()
    with pytest.raises(yscr.ArgumentError, match="template"):
        g.instances(t, 3)
    with pytest.raises(yscr.ArgumentError, match="not a parameter"):
        t.stroke = 2                                          # a run's edge is the box filter
    with pytest.raises(AttributeError):
        t.no_such_thing = 1
    with pytest.raises(ValueError):                          # UnicodeEncodeError
        t.text = "\ud800"                                    # a lone surrogate is not UTF-8
    frame(s, g, t)


def test_degrees(sg):
    s, _ = sg
    s2g = ygfx.Gfx  # noqa: F841
    with yscr.Screen(backend="sim") as s2:
        g = ygfx.Gfx(s2, units="deg", view=(570, 400))
        t = g.text("1 deg per em")
        assert t.size == 1.0 and 0.5 < t.box[1] < 2.0
        p = g.text("wrapped at five degrees of visual angle", size=0.5, width=5)
        assert p.box[0] == 5 and p.n_lines > 1
        frame(s2, g, t, p)
        g.close()


def test_empty_text_and_newlines(sg):
    s, g = sg
    e = g.text("")
    assert e.n_glyphs == 0 and e.words() == [] and e.glyphs() == []
    frame(s, g, e)
    t = g.text("one\ntwo\nthree", size=20)
    assert t.n_lines == 3 and [w[3] for w in t.words()] == [0, 1, 2]
    e.text = "now"
    assert e.n_glyphs == 3
    frame(s, g, e, t)


def test_lifetime(sg):
    s, g = sg
    need(ARIAL)
    f = g.font(ARIAL)
    t = g.text("kept", font=f)
    del f
    gc.collect()
    assert t.font.info[0]["name"] == "arial.ttf"             # the text holds its Font
    frame(s, g, t)
    w = t.words()
    g.close()
    with pytest.raises(yscr.ClosedError):
        t.text = "after"
    with pytest.raises(yscr.ClosedError):
        t.words()
    with pytest.raises(yscr.ClosedError):
        g.text("new")
    assert t.text == "kept" and w[0][0] == "kept"            # reads give the last values; lists are copies
    del t
    gc.collect()


def test_texts_kept_after_the_screen_closes():
    need(SEGOE)
    s = yscr.Screen(backend="sim")
    g = ygfx.Gfx(s)
    t = g.text("x", font=SEGOE)
    s.close()
    with pytest.raises(yscr.ClosedError):
        t.size = 10
    del g, s
    gc.collect()
    with pytest.raises(yscr.ClosedError):
        t.glyphs()
    del t
    gc.collect()


def test_many_texts_and_fonts(sg):
    s, g = sg
    need(ARIAL)
    fonts = [g.font(ARIAL) for _ in range(6)]
    texts = [g.text("font %d" % i, font=f) for i, f in enumerate(fonts)]
    frame(s, g, *texts)
    # one curve set per font file of each list: 1 (the default) + 6, under the 32 the core allows
    assert all(t.missing == 0 for t in texts)
