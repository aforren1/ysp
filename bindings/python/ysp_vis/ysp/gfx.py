"""ysp.gfx: stimuli on a ysp.screen.Screen (ysp/gfx.h).

Stimuli are objects of a Gfx: shapes, gratings, gabors, noise, images, dot
fields and instances. Their parameters are attributes by ysp/gfx.h's names
(``params(kind)`` lists them); a DrawList draws many in one call and sets a
parameter of every item in one call; a timeline channel bound to a
parameter is written in C at each frame, so a tween costs no Python call::

    import ysp.screen as yscr, ysp.gfx as ygfx, ysp.timeline as ytl
    tl = ytl.Timeline(["contrast"])
    with yscr.Screen(gfx={"background": 0.5}) as s:      # programs compile while it settles
        g = ygfx.Gfx(s)
        gab = g.gabor(sf=1 / 32, sigma=24, x=-200)
        fix = g.shape("circle", w=8, color=1.0)
        scene = g.list([gab, fix])
        g.bind(gab, "contrast", tl, "contrast")          # applied in begin()
        g.timeline(tl)                                   # evaluated at each frame's onset
        f = s.begin()
        tl.anchor(1, f.onset)                            # trial time 0 is this frame
        tl.tween("contrast", 0.5, base=1, start=0.0, duration=0.5, from_=0.0)
        while True:
            g.frame(scene)                               # begin, bindings, draws, end
            s.flip()
            f = s.begin()

Units: the gfx's (px, or degrees with units="deg"); x, y from the place
point (the screen's center by default), y down, ori clockwise. Every time is
seconds. The implementation is ysp._vis.
"""
from ysp import _vis
from ysp._vis import DrawList, Font, Gfx, Group, Stim, Text, Texture  # noqa: F401

__version__ = _vis.__version__        # the ysp-vis distribution's
HEADER_VERSION = _vis.GFX_VERSION     # ysp/gfx.h's


def params(kind=None):
    """The parameter rows a kind of stimulus reads (all kinds when None):
    a list of dicts with name, type, min, max, default, unit, doc and, for
    names (enums), the values."""
    return _vis._gfx_params(kind)
