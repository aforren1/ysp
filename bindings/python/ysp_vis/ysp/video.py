"""ysp.video: movies as stimuli (ysp/video.h).

A placeholder in phase A of ysp-vis: ysp/video.h is compiled into ysp._vis,
and its calls to ysp/audio.h and ysp/timeline.h go through the tables of
ysp.audio and ysp.timeline, so a movie's soundtrack plays on the
experiment's ysp.audio.Audio. The Movie API comes in phase B.
"""
from ysp import _vis

__version__ = _vis.__version__        # the ysp-vis distribution's
HEADER_VERSION = _vis.VIDEO_VERSION   # ysp/video.h's
