"""Movies in a real window, briefly: local only, not in CI.

    set YSP_VIS_WINDOW=1, then: pytest bindings/python/ysp_vis/tests/test_video_window.py

A 1280 x 720 window on the AUTO backend with the bundled ANGLE: a frame
sequence (RGBA8 and I420, uploaded through ysp/gfx.h), MPEG-1 (pl_mpeg) and
an MP4 (Media Foundation, DXVA and the software decoder) when ffmpeg is on
PATH. Each plays to its end; the records must show every frame, an OS onset
(not the simulated tier), and no timestamp or hash mismatch. Nothing here
is a timing claim: tests/probe/vis/movie.py measures.
"""
import os
import shutil
import subprocess

import pytest

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.video as yvid

pytestmark = pytest.mark.skipif(os.environ.get("YSP_VIS_WINDOW") != "1", reason="set YSP_VIS_WINDOW=1 (opens a window)")
FFMPEG = shutil.which("ffmpeg")


def low_contrast(w, h, i):
    """A gray gradient with a moving bar: the display may be somebody's working screen."""
    row = bytearray()
    bar = (i * 8) % w
    for x in range(w):
        v = 150 if bar <= x < bar + 8 else 112 + (x * 32) // w
        row += bytes((v, v, v, 255))
    return bytes(row) * h


@pytest.fixture(scope="module")
def clips(tmp_path_factory):
    d = tmp_path_factory.mktemp("win")
    out = {}
    p = str(d / "rgba.yspseq")
    with yvid.SeqWriter(p, 320, 180, 30) as w:
        for i in range(60):
            w.write(low_contrast(320, 180, i))
    out["rgba"] = p
    p = str(d / "i420.yspseq")
    with yvid.SeqWriter(p, 320, 180, 30, format="i420", matrix="bt709", range="limited", transfer="bt1886",
                        primaries="bt709", siting="left") as w:
        for i in range(60):
            w.write(bytes([16 + (i * 3) % 200]) * (320 * 180) + bytes([128]) * (2 * 160 * 90))
    out["i420"] = p
    if FFMPEG:
        ff = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i"]
        p = str(d / "m.mpg")
        subprocess.run(ff + ["testsrc2=size=320x180:rate=30", "-frames:v", "60", "-vf", "eq=contrast=0.3",
                             "-c:v", "mpeg1video", "-q:v", "4", "-g", "15", "-bf", "0", "-f", "mpeg", p], check=True)
        yvid.make_index(p)
        out["mpeg1"] = p
        p = str(d / "m.mp4")
        vui = "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv:chromaloc=0"
        r = subprocess.run(ff + ["testsrc2=size=640x360:rate=30", "-frames:v", "60", "-vf", "eq=contrast=0.3",
                                 "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-bf", "0", "-g", "15",
                                 "-keyint_min", "15", "-sc_threshold", "0", "-x264-params",
                                 "bframes=0:scenecut=0:open-gop=0:" + vui, "-color_primaries", "bt709", "-color_trc",
                                 "bt709", "-colorspace", "bt709", "-color_range", "tv", "-video_track_timescale", "30000",
                                 "-an", p])
        if r.returncode == 0:
            yvid.make_index(p, matrix="bt709", range="limited", transfer="bt1886", primaries="bt709", siting="left")
            out["mp4"] = p
    return out


@pytest.fixture(scope="module")
def win():
    with yscr.Screen(windowed=True, size=(1280, 720)) as s, ygfx.Gfx(s, background=0.45) as g:
        s.title("ysp-vis movie test")
        yield s, g


@pytest.mark.parametrize("name,opts", [("rgba", {}), ("i420", {}), ("mpeg1", {}), ("mp4", {"hw_decode": "dxva"}),
                                       ("mp4", {"hw_decode": "off"})])
def test_movie_in_window(win, clips, name, opts):
    if name not in clips:
        pytest.skip("ffmpeg (or its libx264) is not available")
    s, g = win
    with yvid.Movie(g, clips[name], **opts) as m:
        m.play()
        while True:
            s.begin()
            playing = m.update()
            g.begin()
            m.draw(w=640, h=360, linear=True)
            g.end()
            s.flip()
            if not playing:
                break
        s.wait_flip()
        r = m.records()
        info = m.info
    assert r[-1].frame == 59
    shown = {x.frame for x in r if x.decision == yvid.SHOWN}
    dropped = info["dropped"]
    assert len(shown) + dropped == 60
    assert all(x.tier != yscr.TIER_SIM and x.onset_ns > 0 for x in r)
    assert info["ts_mismatch"] == 0 and info["hash_mismatch"] == 0
