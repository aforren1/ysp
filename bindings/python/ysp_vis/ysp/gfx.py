"""ysp.gfx: stimuli on a ysp.screen.Screen (ysp/gfx.h).

Phase A of ysp-vis: the draw path a frame loop needs to show something::

    import ysp.screen as yscr, ysp.gfx as ygfx
    with yscr.Screen(windowed=True) as s, ygfx.Gfx(s, background=0.5) as g:
        f = s.begin()
        g.begin()
        g.rect(0, 0, 200, 100, (1.0, 1.0, 1.0))   # center offset from the screen's center, px
        g.end()
        s.flip()

Shapes, gratings, gabors, dots, images, noise, text, groups and the timeline
bindings come in phase B. The implementation is ysp._vis.
"""
from ysp import _vis
from ysp._vis import Gfx  # noqa: F401

__version__ = _vis.__version__        # the ysp-vis distribution's
HEADER_VERSION = _vis.GFX_VERSION     # ysp/gfx.h's
