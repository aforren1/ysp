"""ysp.video: movies as stimuli (ysp/video.h).

On each display frame the movie shows the frame due at the frame's
PREDICTED ONSET, never a count of frames, and each display frame gets a
Record: the frame, when it was due, when the flip showed it, and the
decision (SHOWN, REPEATED, DROPPED) with its reason::

    import ysp.screen as yscr, ysp.gfx as ygfx, ysp.video as yvid
    with yscr.Screen() as s, ygfx.Gfx(s) as g, yvid.Movie(g, "clip.mp4") as m:
        m.play()                          # as soon as the frames are ready
        while True:
            f = s.begin()
            playing = m.update()          # between Screen.begin() and Gfx.begin()
            g.begin(); m.draw(); g.end()
            s.flip()
            if not playing:
                break
        s.wait_flip()
        frames = m.records()

A file must be in the canonical form (docs/video.md): a frame sequence
(SeqWriter), or MPEG-1 or MP4 (H.264, HEVC; Windows Media Foundation) with
its .yspvi index beside it (make_index()). The sound: Movie.soundtrack()
with a ysp.audio.Audio. The implementation is ysp._vis.
"""
from ysp import _vis
from ysp._vis import (Error, ArgumentError, BusyError, ClosedError, FormatError, Movie,  # noqa: F401
                      NotFoundError, NotImplementedError, OrderError, RefusedError, SeqWriter, make_index, probe)
from ysp._vis import MovieRecord as Record  # noqa: F401
from ysp._vis import VideoIOError as IOError  # noqa: F401,A001

__version__ = _vis.__version__        # the ysp-vis distribution's
HEADER_VERSION = _vis.VIDEO_VERSION   # ysp/video.h's

globals().update(_vis._video_constants)

#: Record.why as a name: WHY_NAMES[r.why]
WHY_NAMES = ("due", "cadence", "display_late", "decode_late", "drift", "seek", "loop", "manual")
#: Record.decision as a name: DECISION_NAMES[r.decision]
DECISION_NAMES = (None, "shown", "repeated", "dropped")


def options():
    """Movie()'s keyword options and what each does."""
    return _vis.video_options()


def from_pack(gfx, pack, name, **options):
    """A movie from a ysp.pack.Pack entry: the entry's bytes and, for MPEG-1
    or MP4, the index entry name + ".yspvi" (as the pack tool stores it).
    The whole entry is read into memory; a streamed entry is not bound."""
    index = None
    if name + ".yspvi" in pack.names():
        index = pack.read(name + ".yspvi")
    return Movie(gfx, data=pack.read(name), index=index, **options)
