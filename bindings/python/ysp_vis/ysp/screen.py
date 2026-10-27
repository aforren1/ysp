"""ysp.screen: a stimulus display with flip at a time (ysp/screen.h).

The window, its GL ES context and its swap path; each frame learns the
predicted onset of its flip, presents for a target time on the ysp_rt clock,
and gets a record of the vblank it was shown on. Every time is seconds, with
the exact ns beside it in records, frames and events::

    import ysp.screen as yscr
    with yscr.Screen(windowed=True) as s:
        while True:
            try:
                f = s.begin()          # GIL released; Abort on Shift+Esc
            except yscr.Abort:
                break
            ...                        # draw for f.onset (ysp.gfx)
            s.flip()                   # GIL released
        records = s.records()

The window belongs to a thread of ysp/screen.h's own, so time.sleep() and
input() between trials do not hang it. Call gc.freeze() after setup.
The implementation is ysp._vis (one module for screen, gfx and video).
"""
import atexit as _atexit

from ysp import _vis
from ysp._vis import (Abort, ArgumentError, BusyError, ClosedError, Display, Error, Event, Frame,  # noqa: F401
                      LostError, Mode, NotImplementedError, OrderError, Record, RefusedError, Screen,
                      TimeoutError, displays, input_stats, mode_multiple, modes, now, now_ns, options,
                      request_abort)

__version__ = _vis.__version__          # the ysp-vis distribution's
HEADER_VERSION = _vis.SCREEN_VERSION    # ysp/screen.h's
ANGLE_DIR = _vis.ANGLE_DIR              # where libEGL.dll is loaded from: always this package's directory

for _n in dir(_vis):
    if _n.isupper() and not _n.endswith("_VERSION") and _n != "ANGLE_DIR":
        globals()[_n] = getattr(_vis, _n)
del _n

# A screen left open at exit puts back the gamma ramp and the foreground.
_atexit.register(_vis._close_all)
