"""ysp.gfx on the simulated display (backend='sim'): gfx's null backend draws
nothing, so these tests check what the binding does: the stimuli and their
parameters against ysp/gfx.h's constructors and tables, the refusals, draw
lists and bulk sets, groups, instances and their element view, dot fields,
textures, the timeline bindings (a tween drives a parameter in C; a rewind
and a seek replay the same values), hit tests, the open started while the
screen settles, capture, and the lifetime rules (an object kept after its
gfx closes fails cleanly; a view owns its memory). tests/test_window.py
checks pixels against the C path in a real window (local only).
"""
import gc
import math
import os

import pytest

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl

np = pytest.importorskip("numpy")

PERIOD = 0.004


@pytest.fixture
def sg():
    with yscr.Screen(backend="sim", sim_period=PERIOD) as s:
        g = ygfx.Gfx(s, background=0.5)
        yield s, g
        g.close()


def frame(s, g, *items):
    f = s.begin()
    g.frame(*items)
    s.flip()
    return f


SHAPES = {
    "rect": dict(w=100, h=50, radius=8), "circle": dict(w=40), "annulus": dict(w=60, inner=20),
    "line": dict(w=80, width=4), "polygon": dict(vertices=[(-20, -10), (20, -10), (0, 20)]),
    "cross": dict(w=30, h=30, width=4), "rrect": dict(w=100, h=60, radii=(2, 4, 6, 8)),
    "arc": dict(w=80, sweep=270, thickness=6, start=45), "pie": dict(w=80, sweep=90),
    "capsule": dict(w=80, radii=(10, 6)), "ngon": dict(w=60, sides=6, rounding=2),
    "star": dict(w=80, points=5, inner=0.4), "ellipse": dict(w=80, h=40),
    "qbezier": dict(w=100, h=60, path=[(-40, 20), (0, -40), (40, 20)], width=3),
    "polyline": dict(w=100, h=60, path=[(-40, 20), (0, -20), (40, 20), (60, 0)], width=3),
}


def test_every_shape_kind_draws(sg):
    s, g = sg
    made = {name: g.shape(name, color=(1.0, 0.5, 0.0), **kw) for name, kw in SHAPES.items()}
    for name, st in made.items():
        assert st.kind == "shape" and st.shape == name
    frame(s, g, *made.values())
    assert made["annulus"].shape_p[0] == 20 and made["annulus"].inner == 20
    assert made["rrect"].radii == (2, 4, 6, 8)
    assert made["arc"].shape_p[:3] == (270, 6, 45)
    assert made["ngon"].sides == 6
    # what ysp/gfx.h's constructor derives from the desc
    assert made["star"].shape_p[1] == pytest.approx(0.4)
    assert g.shape("star", w=80, points=5).inner == 0.5                 # inner 0 in the desc: 0.5
    assert g.shape("capsule", w=80, radii=(10, 6)).h == 20              # h from the larger radius
    assert made["polygon"].w == 40 and made["polygon"].h == 40          # the vertices' extent
    assert made["polygon"].shape_p[0] == 3
    assert made["polyline"].path == [(-40, 20), (0, -20), (40, 20), (60, 0)]
    assert g.shape("rect", w=30).h == 30                                # h 0 = w


def test_kinds_and_their_constructors(sg):
    s, g = sg
    gr = g.grating(sf=0.05, w=200, aperture="circle", edge="cosine", edge_width=8, contrast=0.3, square=True)
    ga = g.gabor(sf=0.05, sigma=10, contrast=0.4, phase=0.25, aspect=2, ori=30)
    no = g.noise(w=64, check=4, seed=7, dist="binary", contrast=0.2)
    sx = g.noise(w=64, dist="simplex", scale=16, octaves=3, z=0.5)
    assert gr.kind == "grating" and gr.aperture == "circle" and gr.edge == "cosine" and gr.square is True
    assert gr.h == 200
    assert ga.kind == "gabor" and ga.w == 0 and ga.aspect == 2 and ga.ori == 30   # w 0: 8 sigma at draw
    assert no.dist == "binary" and no.check == 4 and no.seed == 7
    assert sx.dist == "simplex" and sx.octaves == 3 and sx.noise_scale == 16 and sx.noise_z == 0.5
    # a zero contrast is a zero (the desc's 0 = 1 default is not used for it)
    assert g.gabor(sf=0.05, sigma=10, contrast=0).contrast == 0
    assert g.shape("circle", w=10, opacity=0).opacity == 0
    frame(s, g, gr, ga, no, sx)


def test_attributes_round_trip(sg):
    s, g = sg
    r = g.shape("rect", w=100, h=40)
    r.x, r.y, r.ori = 12.5, -3.0, 45.0
    assert (r.x, r.y, r.ori) == (12.5, -3.0, 45.0)
    r.color = 0.25
    assert r.color == (0.25, 0.25, 0.25)
    r.color = (1, 0, 0.5)
    assert r.color == (1.0, 0.0, 0.5)
    r.anchor = "top_left"
    assert r.anchor == (0.0, 0.0) and (r.ax, r.ay) == (0.0, 0.0)
    r.anchor = (0.25, 0.75)
    assert r.anchor == (0.25, 0.75)
    r.place = "bottom_right"
    assert r.place == "bottom_right"
    r.edge = "gaussian"
    r.edge_width = 1.5
    assert r.edge == "gaussian" and r.edge_width == 1.5
    r.stroke, r.stroke_align, r.join = 3, "outside", "miter"
    assert (r.stroke, r.stroke_align, r.join) == (3, "outside", "miter")
    r.blend = "add"
    assert r.blend == "add"
    r.set(x=1, y=2, color=(0, 1, 0), visible=0)
    assert (r.x, r.y, r.color, r.visible) == (1, 2, (0, 1, 0), 0)
    assert "contrast" not in dir(r) and "color_r" in dir(r)
    frame(s, g, r)


def test_refusals(sg):
    s, g = sg
    r = g.shape("circle", w=20)
    with pytest.raises(AttributeError):
        r.no_such_thing = 1
    with pytest.raises(AttributeError):
        r.no_such_thing
    with pytest.raises(yscr.ArgumentError, match="not a parameter of this kind"):
        r.contrast = 0.5                       # a modulation parameter on a color stimulus
    with pytest.raises(yscr.ArgumentError):
        r.x = math.nan
    with pytest.raises(yscr.ArgumentError):
        r.color = (1, 0)
    with pytest.raises(yscr.ArgumentError, match="edge"):
        r.edge = "blurry"
    with pytest.raises(yscr.ArgumentError):
        g.shape("hexagon")
    with pytest.raises(yscr.ArgumentError, match="vertices are required"):
        g.shape("polygon")
    with pytest.raises(yscr.ArgumentError, match="path is required"):
        g.shape("polyline")
    with pytest.raises(yscr.ArgumentError, match="size"):
        g.gabor(sigma=10, w=50)
    with pytest.raises(yscr.ArgumentError, match="tex"):
        g.image()
    with pytest.raises(TypeError):
        g.gabor(1)
    with pytest.raises(yscr.ArgumentError, match="no parameter"):
        g.gabor(sigma=10, spatial_frequency=0.1)
    with pytest.raises(yscr.ArgumentError):
        g.shape("circle", w=10, aperture="rect")    # aperture is not a shape's
    # a stimulus the GPU could not draw is refused by draw, named
    bad = g.shape("rect", w=0, h=0)
    s.begin()
    with pytest.raises(yscr.ArgumentError):
        g.frame(bad)
    assert not g.in_frame                         # frame() ended the frame anyway
    s.flip()


def test_colors_need_a_calibration(sg):
    s, g = sg
    r = g.shape("circle", w=10)
    with pytest.raises(yscr.ArgumentError, match="calibration"):
        r.color = "#ff8000"
    with pytest.raises(yscr.ArgumentError, match="calibration"):
        g.shape("circle", w=10, color=(0, 90, 0.1), space="dkl")
    with pytest.raises(yscr.ArgumentError, match="calibration"):
        g.color("srgb", 1, 0, 0)
    assert g.color("rgb", 0.1, 0.2, 0.3) == pytest.approx((0.1, 0.2, 0.3))
    with pytest.raises(yscr.ArgumentError, match="space"):
        g.color("nope", 0, 0, 0)


def test_colors_through_a_calibration():
    ycol = pytest.importorskip("ysp.color")
    xy = [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06), (0.3127, 0.3290)]
    cal = ycol.Calibration.nominal(xy, 80.0, 2.2)
    with yscr.Screen(backend="sim", sim_period=PERIOD) as s, ygfx.Gfx(s, background=0.5, cal=cal) as g:
        assert g.calibrated
        r = g.shape("circle", w=10, color="#ffffff")
        assert r.color == pytest.approx((1, 1, 1), abs=2e-3)
        red = g.color("srgb", 1, 0, 0)
        assert red[0] > 0.9 and red[1] < 0.05 and red[2] < 0.05
        ga = g.gabor(sigma=10, sf=0.05, dir=(0.30, 0.32, 0.30), space="xyz")
        assert ga.dir == pytest.approx(g.dir("xyz", 0.30, 0.32, 0.30), abs=1e-6)
        frame(s, g, r, ga)
    with yscr.Screen(backend="sim", sim_period=PERIOD) as s:
        with pytest.raises(yscr.ArgumentError, match="cal"):
            ygfx.Gfx(s, cal=b"not a calibration")


def test_draw_list_bulk(sg):
    s, g = sg
    items = [g.gabor(sf=0.05, sigma=8) for _ in range(100)]
    L = g.list(items)
    assert len(L) == 100 and L[3] is items[3] and list(L)[99] is items[99]
    xs = np.linspace(-300, 300, 100)
    L.set("x", xs)
    assert np.allclose(np.asarray(L.get("x")), xs.astype(np.float32))
    L.set("contrast", 0.25)                        # one value for all
    assert set(np.asarray(L.get("contrast")).tolist()) == {0.25}
    L.set("dir", np.tile([0.1, 0.2, 0.3], (100, 1)))   # n x 3
    assert np.asarray(L.get("dir")).shape == (100, 3)
    with pytest.raises(yscr.ArgumentError, match="values"):
        L.set("x", [1, 2, 3])
    shapes = g.list([g.shape("circle", w=10) for _ in range(4)])
    shapes.set("color", [0.1, 0.5, 0.9, 0.7])      # n grays: one per item
    assert np.asarray(shapes.get("color")).tolist()[2] == pytest.approx([0.9] * 3)
    three = g.list([g.shape("circle", w=10) for _ in range(3)])
    three.set("color", (0.1, 0.5, 0.9))            # 3 values for 3 items: one color for all, not 3 grays
    assert np.asarray(three.get("color")).tolist()[0] == pytest.approx([0.1, 0.5, 0.9])
    shapes.set("color", (1, 0, 0))                 # 3 values for 3 items: one color for all
    assert np.asarray(shapes.get("color")).tolist()[1] == [1, 0, 0]
    for i in range(3):
        frame(s, g, L, shapes)
    popped = L.pop()
    assert popped is items[99] and len(L) == 99
    L.clear()
    assert len(L) == 0
    frame(s, g, L)
    with pytest.raises(TypeError):
        L.append("x")


def test_frame_order(sg):
    s, g = sg
    r = g.shape("circle", w=10)
    with pytest.raises(yscr.OrderError):
        g.frame(r)                                 # before the screen's begin()
    s.begin()
    with pytest.raises(yscr.OrderError):
        g.draw(r)                                  # before Gfx.begin()
    g.begin()
    g.draw(r)
    with pytest.raises(yscr.OrderError):
        g.begin()
    with pytest.raises(yscr.OrderError):
        s.flip()                                   # end() first
    g.end()
    s.flip()
    with pytest.raises(TypeError):
        s.begin()
        g.frame("not a stimulus")
    s.flip()


def test_groups(sg):
    s, g = sg
    grp = g.group(x=100, y=0, ori=90, scale=2)
    r = g.shape("rect", w=10, h=10, x=10, group=grp)
    assert r.group is grp and grp.x == 100 and grp.scale == 2
    assert r.resolve() == pytest.approx((400 + 100, 300 + 20), abs=1e-3)   # (10, 0) turned 90 and doubled
    grp.x = 0
    assert r.resolve() == pytest.approx((400, 320), abs=1e-3)
    grp.anchor = "top_left"
    assert grp.anchor == (0.0, 0.0)
    with pytest.raises(AttributeError):
        grp.nope = 1
    r.group = None
    assert r.group is None and r.resolve() == pytest.approx((410, 300))
    frame(s, g, r)


def test_hit_tests(sg):
    s, g = sg
    cx, cy = g.center
    assert (cx, cy) == (400, 300) and g.size == (800, 600)
    a = g.shape("rect", w=100, h=100)
    b = g.shape("circle", w=40, x=30)
    assert a.hit(cx, cy) and not a.hit(cx + 60, cy)
    assert b.hit(cx + 30, cy) and not b.hit(cx - 30, cy)
    L = g.list([a, b])
    assert L.hit(cx + 30, cy) == 1                  # the topmost
    assert L.hit(cx - 40, cy) == 0
    assert L.hit(0, 0) == -1
    b.visible = 0
    assert L.hit(cx + 30, cy) == 0                  # a hidden item is not hit
    assert a.bounds() == pytest.approx((350, 250, 450, 350))
    t = g.gabor(sf=0.05, sigma=5, size=20)
    I = g.instances(t, 3, fields=("xy", "gate"))
    I.set_elements("xy", [(-100, 0), (0, 0), (100, 0)])
    assert I.hit_index(cx + 100, cy) == 2 and I.hit_index(cx + 50, cy) == -1
    I.set_elements("gate", 0.0, first=2)
    assert I.hit_index(cx + 100, cy) == -1          # gate 0 is not hit
    assert I.resolve(0) == pytest.approx((300, 300))
    L2 = g.list([a, I])
    assert L2.hit_element(cx, cy) == (1, 1)


def test_instances_view_owns_its_memory(sg):
    s, g = sg
    t = g.shape("circle", w=8, color=1.0)
    I = g.instances(t, 400, fields=("xy", "ori", "gate", "color"), palette=[(1, 0, 0), (0, 0, 1)])
    assert I.kind == "instances" and I.n == 400
    e = np.asarray(I.elements)
    assert e.shape == (400, 8) and e.dtype == np.float32
    assert e[0].tolist() == [0, 0, 0, 0, 1, 1, 1, 0]            # contrast, scale, gate 1
    e[:, 0] = np.arange(400)                                    # x, straight into what draw reads
    I.set_elements("ori", np.full(400, 45.0))
    assert e[7, 2] == 45.0 and e[7, 0] == 7
    I.set_elements("color", 1.0, first=200)
    assert e[199, 7] == 0 and e[200, 7] == 1
    with pytest.raises(yscr.ArgumentError, match="not in the instances' fields"):
        I.set_elements("phase", 0.5)
    with pytest.raises(yscr.ArgumentError):
        I.set_elements("ori", [1, 2, 3], first=399)
    with pytest.raises(yscr.ArgumentError, match="palette"):
        g.instances(t, 4, fields=("xy", "color"))
    frame(s, g, I)
    view = I.elements
    del I, e
    gc.collect()
    assert view.tolist()[7][0] == 7.0          # the view keeps the bytes; nothing reads them now
    view[0, 0] = 5.0


def test_dots(sg):
    s, g = sg
    d = g.dots(n=100, dot_size=4, color=1.0, aperture="circle", w=200)
    assert d.kind == "dots" and d.n == 100 and d.count == 0
    xy = np.random.default_rng(1).uniform(-100, 100, (60, 2))
    d.positions = xy
    assert d.count == 60
    frame(s, g, d)
    s.begin()
    g.begin()
    with pytest.raises(yscr.OrderError):
        d.positions = xy                       # a queued draw reads the buffer at end()
    g.end()
    s.flip()
    with pytest.raises(yscr.ArgumentError, match="holds"):
        d.positions = np.zeros((101, 2))
    d.count = 10
    assert d.count == 10
    e = g.dots(positions=[(0, 0), (5, 5)], dot_size=3)
    assert e.n == 2 and e.count == 2
    with pytest.raises(yscr.ArgumentError):
        g.dots(dot_size=3)


def test_textures_and_images(sg):
    s, g = sg
    rgb = np.zeros((32, 64, 3), np.uint8)
    t = g.texture(rgb)
    assert (t.w, t.h, t.format) == (64, 32, "rgba8")
    im = g.image(t, x=10)
    assert im.kind == "image" and im.tex is t and (im.w, im.h) == (64, 32)   # w, h from the texels
    f32 = g.texture(np.ones((16, 16), np.float64))
    assert f32.format == "r32f"
    mod = g.image(f32, modulation=True, contrast=0.5, dir=(0.1, 0.1, 0.1))
    assert mod.modulation is True
    gray = g.texture(np.zeros((8, 8), np.uint8), srgb=True)
    assert gray.format == "r8"
    raw = g.texture(bytes(4 * 4 * 4), w=4, h=4, format="rgba8")
    assert raw.w == 4
    frame(s, g, im, mod, g.image(gray), g.image(raw, src=(0, 0, 2, 2)))
    t.update(np.full((32, 64, 3), 255, np.uint8))
    t.update(np.zeros((2, 2, 4), np.uint8), x=10, y=10)
    with pytest.raises(yscr.ArgumentError, match="outside"):
        t.update(np.zeros((2, 2, 4), np.uint8), x=63, y=0)
    with pytest.raises(yscr.ArgumentError):
        g.texture(np.zeros((4, 4), np.int8))
    with pytest.raises(yscr.ArgumentError, match="srgb"):
        g.texture(np.zeros((4, 4), np.float32), srgb=True)
    s.begin()
    g.begin()
    with pytest.raises(yscr.OrderError):
        t.update(np.zeros((32, 64, 3), np.uint8))
    g.end()
    s.flip()
    del im
    gc.collect()
    frame(s, g)


def test_bound_tween_drives_the_stimulus_and_replays(sg):
    s, g = sg
    tl = ytl.Timeline(["contrast", "x"])
    gab = g.gabor(sf=0.05, sigma=10)
    tl.set_keys("contrast", 1, [(0.0, 0.0), (0.2, 1.0)])
    tl.set_keys("x", 1, [(0.0, -100.0), (0.2, 100.0)])
    g.bind(gab, "contrast", tl, "contrast")
    g.bind(gab, "x", tl, 1)
    g.timeline(tl)                                 # the gfx evaluates it in begin()
    assert g.bindings == 2 and tl.pins == 1

    def run(n):
        # {trial time ns: (contrast, x)}: the sim display drops a frame when
        # the machine is busy, so the frames are keyed by their trial time
        f = s.begin()
        tl.anchor(1, f.onset)                      # trial time 0 is this frame
        t0 = f.onset_ns
        g.frame(gab)
        s.flip()
        out = {0: (gab.contrast, gab.x)}
        for _ in range(n):
            f = frame(s, g, gab)
            out[f.onset_ns - t0] = (gab.contrast, gab.x)
        return out

    first = run(60)
    # the channel's value at each frame's trial time, written into the stimulus in C
    for t, (c, x) in first.items():
        k = min(t / 0.2e9, 1.0)
        assert c == pytest.approx(k, abs=1e-6) and x == pytest.approx(-100 + 200 * k, abs=1e-4)
    assert first[0] == (0.0, -100.0) and max(first.items())[1] == (1.0, 100.0)
    again = run(60)                                # a rewind: the same values, bit for bit
    common = sorted(set(first) & set(again))
    assert len(common) >= 30
    assert [again[t] for t in common] == [first[t] for t in common]
    # a seek to a trial time of the first run: the same values there
    t = common[len(common) // 2]
    f = s.begin()
    tl.skip(1, f.onset, t / 1e9)
    g.frame(gab)
    s.flip()
    assert (gab.contrast, gab.x) == first[t]
    # unbinding one name leaves the other
    assert g.unbind(gab, "x") == 1 and g.bindings == 1
    gab.x = 7.0
    frame(s, g, gab)
    assert gab.x == 7.0
    g.close()
    assert tl.pins == 0                            # the close gave the pin back
    tl.close()


def test_bindings_without_evaluate_lists_groups_elements(sg):
    s, g = sg
    tl = ytl.Timeline(8)
    items = [g.shape("circle", w=10) for _ in range(4)]
    L = g.list(items)
    grp = g.group()
    t = g.gabor(sf=0.05, sigma=4)
    I = g.instances(t, 4, fields=("xy", "gate"))
    g.bind(L, "x", tl, [0, 1, 2, 3])               # one channel per item
    g.bind(grp, "y", tl, 4)
    g.bind(I, "gate", tl, [5, 6], element=2)       # elements 2 and 3
    g.bind(items[0], "color", tl, [7, 7, 7])       # a 3-wide parameter: 3 channels
    assert g.bindings == 4 + 1 + 2 + 3 and tl.pins == 1
    with pytest.raises(yscr.ArgumentError, match="channel"):
        g.bind(items[0], "color", tl, 7)
    with pytest.raises(yscr.ArgumentError, match="enums do not bind"):
        g.bind(items[0], "edge", tl, 0)
    with pytest.raises(yscr.ArgumentError, match="fields"):
        g.bind(I, "ori", tl, 0, element=0)
    with pytest.raises(yscr.ArgumentError):
        g.bind(L, "x", tl, [0, 1])                 # neither one per item nor one
    with pytest.raises(TypeError):
        g.bind(items[0], "x", object(), 0)
    # the values the user evaluated: written at begin()
    f = s.begin()
    tl.set_keys(0, 0, [(0.0, 11.0)])
    tl.set_keys(4, 0, [(0.0, -5.0)])
    tl.set_keys(5, 0, [(0.0, 0.25)])
    tl.set_keys(7, 0, [(0.0, 0.5)])
    tl.evaluate(f.onset, f.period)
    g.frame(L, I)
    s.flip()
    assert items[0].x == 11.0 and items[1].x == 0.0 and grp.y == -5.0
    assert np.asarray(I.elements)[2, 6] == 0.25 and np.asarray(I.elements)[0, 6] == 1.0
    assert items[0].color == (0.5, 0.5, 0.5)
    # binding the same place again replaces the binding
    g.bind(items[0], "x", tl, 4)
    assert g.bindings == 10
    assert g.unbind(L) == 4 + 3 and g.bindings == 3
    with pytest.raises(ytl.BusyError):
        tl.close()                                 # pinned while bound
    assert g.unbind(timeline=tl) == 3 and g.bindings == 0 and tl.pins == 0
    tl.close()


def test_evaluate_callback_error_comes_back(sg):
    s, g = sg
    tl = ytl.Timeline(1)
    fld = tl.field(0.0)
    g.timeline(tl)

    def boom():
        raise RuntimeError("from a tween callback")

    f = s.begin()
    tl.anchor(1, f.onset)
    tl.seq(1).to(fld, 1.0, duration=0.1, on_start=boom)
    with pytest.raises(RuntimeError, match="tween callback"):
        g.frame()
    assert not g.in_frame
    s.flip()
    frame(s, g)
    g.timeline(tl, evaluate=False)
    assert tl.pins == 0


def test_open_started_while_the_screen_settles():
    with yscr.Screen(backend="sim", sim_period=PERIOD, gfx={"background": 0.25, "dither": "ordered"}) as s:
        with pytest.raises(yscr.ArgumentError, match="started"):
            ygfx.Gfx(s, background=0.5)
        g = ygfx.Gfx(s)
        assert "dither ordered" in g.describe()
        frame(s, g, g.shape("circle", w=10))
    with yscr.Screen(backend="sim", sim_period=PERIOD, gfx=True) as s:
        pass                                       # started, never opened: freed with the screen
    with pytest.raises(yscr.ArgumentError, match="unknown gfx option"):
        yscr.Screen(backend="sim", gfx={"nope": 1})
    with pytest.raises(yscr.ArgumentError, match="gfx"):
        yscr.Screen(backend="sim", gfx=3)


def test_gfx_options():
    with yscr.Screen(backend="sim", sim_period=PERIOD) as s:
        with pytest.raises(yscr.ArgumentError, match="units"):
            ygfx.Gfx(s, units="mm")
        with pytest.raises(yscr.ArgumentError, match="unknown gfx option"):
            ygfx.Gfx(s, colour=1)
        g = ygfx.Gfx(s, units="deg", view=(570, 480), max_draws=64, cache=False, prime=False)
        assert "1 px/unit" not in g.describe() and "program cache off" in g.describe()
        assert g.programs()["cache"] == ""
        g.close()
        with ygfx.Gfx(s, background=(0.1, 0.2, 0.3)) as g2:
            assert g2.open


def test_lifetime_after_close():
    s = yscr.Screen(backend="sim", sim_period=PERIOD)
    g = ygfx.Gfx(s)
    r = g.shape("circle", w=10)
    L = g.list([r])
    grp = g.group()
    tex = g.texture(np.zeros((4, 4), np.uint8))
    im = g.image(tex)
    s.close()                                      # closes its gfx
    assert not g.open
    with pytest.raises(yscr.ClosedError):
        r.x = 1
    with pytest.raises(yscr.ClosedError):
        r.hit(0, 0)
    with pytest.raises(yscr.ClosedError):
        L.set("x", 1)
    with pytest.raises(yscr.ClosedError):
        grp.x = 1
    with pytest.raises(yscr.ClosedError):
        tex.update(np.zeros((4, 4), np.uint8))
    with pytest.raises(yscr.ClosedError):
        g.shape("circle", w=1)
    with pytest.raises(yscr.ClosedError):
        im.copy()
    assert r.x == 0.0                              # reads give the last values
    del g, s
    gc.collect()
    r.x                                            # the objects hold their memory
    del r, L, grp, tex, im
    gc.collect()


def test_copy_and_params(sg):
    s, g = sg
    p = g.shape("polyline", w=100, h=60, path=[(-40, 0), (40, 0)], width=2)
    q = p.copy()
    q.path = [(0, 0), (10, 10), (20, 0)]
    assert p.path == [(-40, 0), (40, 0)] and len(q.path) == 3
    frame(s, g, p, q)
    names = [r["name"] for r in ygfx.params("gabor")]
    assert "sigma" in names and "aspect" in names and "stroke" not in names
    edge = [r for r in ygfx.params("shape") if r["name"] == "edge"][0]
    assert "gaussian=2" in edge["values"]
    with pytest.raises(yscr.ArgumentError):
        ygfx.params("teapot")
    assert p.length() > 0


def test_capture_on_the_null_backend(tmp_path, sg):
    s, g = sg
    g.capture_start(path=str(tmp_path / "f"), every=2)
    for _ in range(4):
        frame(s, g, g.shape("circle", w=10))
    stats = g.capture_stop()
    assert stats["chosen"] == 2 and stats["written"] == 2
    assert sorted(os.listdir(tmp_path)) == ["f00000000.ppm", "f00000002.ppm"]


def test_version_and_header():
    assert ygfx.HEADER_VERSION == ygfx._vis.GFX_VERSION
    assert ygfx.__version__.startswith(yscr.HEADER_VERSION)
