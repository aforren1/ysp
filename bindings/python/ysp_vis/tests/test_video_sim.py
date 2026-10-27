"""Tests for ysp.video (ysp-vis) on the simulated display: no window, no GPU.

tests/adapt/video_test.c checks ysp/video.h itself. Here: the binding drives
it as the C API does. The clips are made here: frame sequences by
SeqWriter (always), MPEG-1 and MP4 by ffmpeg when it is on PATH (skipped
otherwise), each MPEG-1 and MP4 with its index from make_index().

The frames per predicted onset: video_sim_run.py plays a clip on the
simulated display and ysp/rt.h's virtual clock, as examples/video/play.c
--sim does, and the test checks each display frame's frame, decision and
reason against the due rule written out here in Python (docs/video.md),
and, with YSP_VIDEO_PLAY set to a video_play executable, against the C
path line by line.
"""
import array
import csv
import math
import os
import shutil
import subprocess
import sys
import wave

import pytest

import ysp.audio as yau
import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl
import ysp.video as yvid

HERE = os.path.dirname(os.path.abspath(__file__))
FFMPEG = shutil.which("ffmpeg")
PERIOD = 0.004    # a fast simulated display keeps the real-clock tests short


def write_seq(path, w, h, fps, n, fmt="rgba8", **kw):
    """Frame i carries i in its first bytes, so a frame is recognizable."""
    with yvid.SeqWriter(path, w, h, fps, format=fmt, **kw) as wr:
        for i in range(n):
            if fmt == "rgba8":
                wr.write(bytes([i & 255, (i >> 8) & 255, 0, 255]) * (w * h))
            else:
                wr.write(bytes([i & 255]) * wr.frame_bytes)
    return path


def ff(*args):
    subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y"] + list(args), check=True)


@pytest.fixture(scope="module")
def clips(tmp_path_factory):
    d = tmp_path_factory.mktemp("clips")
    c = {
        "s30": write_seq(str(d / "s30.yspseq"), 64, 36, 30, 90),
        "s60": write_seq(str(d / "s60.yspseq"), 64, 36, 60, 60),
        "s2398": write_seq(str(d / "s2398.yspseq"), 64, 36, (24000, 1001), 48),
        "q25": write_seq(str(d / "q25.yspseq"), 64, 36, 25, 25, compression="qoi"),
        "y30": write_seq(str(d / "y30.yspseq"), 64, 36, 30, 30, fmt="i420", matrix="bt709", range="limited",
                         transfer="bt1886", primaries="bt709", siting="left"),
        "dir": str(d),
    }
    return c


@pytest.fixture(scope="module")
def mpeg(tmp_path_factory):
    if not FFMPEG:
        pytest.skip("ffmpeg is not on PATH (the MPEG-1 clips are made with it)")
    d = tmp_path_factory.mktemp("mpeg")
    out = {}
    for name, rate, n, g in (("m30", "30", 60, 15), ("m2398", "24000/1001", 48, 12)):
        p = str(d / (name + ".mpg"))
        ff("-f", "lavfi", "-i", "testsrc2=size=160x96:rate=%s" % rate, "-frames:v", str(n), "-c:v", "mpeg1video",
           "-q:v", "4", "-g", str(g), "-bf", "0", "-f", "mpeg", p)
        assert yvid.make_index(p) == n
        out[name] = p
    return out


@pytest.fixture
def sim():
    with yscr.Screen(backend="sim", sim_period=PERIOD) as s, ygfx.Gfx(s, background=0.5) as g:
        yield s, g


def play_all(s, g, m, limit=10000):
    """The frame loop of USAGE until the end; the update() results."""
    out = []
    for _ in range(limit):
        s.begin()
        playing = m.update()
        g.begin()
        m.draw()
        g.end()
        s.flip()
        out.append(playing)
        if not playing:
            break
    s.wait_flip()
    return out


def due_frame(movie_t_ns, lead_ns, num, den):
    """The largest i with t(i) = ceil(i den 1e9 / num) <= m + L (docs/video.md, the due rule)."""
    return (movie_t_ns + lead_ns) * num // (den * 1_000_000_000)


# --- the package ----------------------------------------------------------------------


def test_names_and_constants():
    assert yvid.HEADER_VERSION == yvid._vis.VIDEO_VERSION
    assert (yvid.SHOWN, yvid.REPEATED, yvid.DROPPED) == (1, 2, 3)
    assert yvid.WHY_NAMES[yvid.WHY_CADENCE] == "cadence" and yvid.WHY_NAMES[yvid.WHY_DECODE_LATE] == "decode_late"
    assert yvid.F_AUDIO_CLOCK == 0x10 and yvid.F_PENDING == 1
    assert issubclass(yvid.NotFoundError, FileNotFoundError) and issubclass(yvid.NotFoundError, yvid.Error)
    assert issubclass(yvid.FormatError, ValueError) and issubclass(yvid.IOError, OSError)
    # the video constants do not leak into ysp.screen
    assert not hasattr(yscr, "SHOWN") and not hasattr(yscr, "WHY_DUE")
    o = yvid.options()
    for k in ("loop", "ahead", "lead", "light", "backend", "hw_decode", "timeline", "base", "index", "data", "records"):
        assert k in o
    assert yvid._vis._shared()["video"] == yvid.HEADER_VERSION
    here = os.path.dirname(os.path.abspath(yvid._vis.__file__))
    assert os.path.exists(os.path.join(here, "ysp_vis-LICENSE.pl_mpeg.txt"))   # MPEG-1 is compiled in


# --- SeqWriter and probe ---------------------------------------------------------------


def test_seqwriter_and_probe(clips):
    i = yvid.probe(clips["s30"])
    assert (i["w"], i["h"], i["fps_num"], i["fps_den"], i["frames"]) == (64, 36, 30, 1, 90)
    assert i["duration"] == 3.0 and i["duration_ns"] == 3_000_000_000
    assert i["codec"] == "seq" and i["format"] == "rgba8" and i["gop"] == 1
    j = yvid.probe(clips["s2398"])
    assert (j["fps_num"], j["fps_den"]) == (24000, 1001)
    assert j["duration_ns"] == math.ceil(48 * 1001 * 10**9 / 24000)    # t(N), the ceiling
    y = yvid.probe(clips["y30"])
    assert (y["format"], y["matrix"], y["range"], y["siting"]) == ("i420", "bt709", "limited", "left")
    with open(clips["q25"], "rb") as f:
        assert yvid.probe(data=f.read())["frames"] == 25


def test_seqwriter_refusals(clips, tmp_path):
    p = str(tmp_path / "x.yspseq")
    with yvid.SeqWriter(p, 4, 2, 30) as w:
        assert w.frame_bytes == 32
        with pytest.raises(yvid.ArgumentError):
            w.write(b"\0" * 31)
        with pytest.raises(TypeError):
            w.write("not bytes")
        w.write(b"\1" * 32)
        assert w.frames == 1
    with pytest.raises(yvid.ClosedError):
        w.write(b"\1" * 32)
    w.close()                                        # twice is safe
    assert yvid.probe(p)["frames"] == 1
    with pytest.raises(yvid.ArgumentError):
        yvid.SeqWriter(p, 4, 2, 30, format="nosuch")
    with pytest.raises(yvid.ArgumentError):
        yvid.SeqWriter(p, 4, 2, 0)
    with pytest.raises(yvid.ArgumentError):          # QOI is for RGBA8
        yvid.SeqWriter(p, 4, 2, 30, format="r8", compression="qoi")
    with pytest.raises(yvid.ArgumentError):          # YUV states its color
        yvid.SeqWriter(p, 4, 2, 30, format="i420")
    with pytest.raises(yvid.ArgumentError):
        yvid.SeqWriter(p, 4, 2, 30, format="i420", matrix="bt709", range="nosuch")
    with yvid.SeqWriter(str(tmp_path / "r8.yspseq"), 3, 3, (30000, 1001), format="r8") as w:
        assert w.frame_bytes == 9
        w.write(bytes(range(9)))


def test_probe_and_open_errors(sim, clips, tmp_path):
    s, g = sim
    missing = str(tmp_path / "missing.yspseq")
    with pytest.raises(yvid.NotFoundError):
        yvid.Movie(g, missing)
    with pytest.raises(FileNotFoundError):
        yvid.Movie(g, missing)
    with pytest.raises(yvid.NotFoundError):
        yvid.probe(missing)
    bad = tmp_path / "bad.yspseq"
    bad.write_bytes(b"not a movie" * 100)
    with pytest.raises(yvid.Error):
        yvid.Movie(g, str(bad))
    with pytest.raises(yvid.ArgumentError):
        yvid.Movie(g)                                # neither a path nor data
    with pytest.raises(yvid.ArgumentError):
        yvid.Movie(g, clips["s30"], data=b"x")       # both
    with pytest.raises(TypeError):
        yvid.Movie(g, clips["s30"], nosuch=1)
    with pytest.raises(yvid.ArgumentError):
        yvid.Movie(g, clips["s30"], light="nosuch")
    with pytest.raises(yvid.Error):                  # the header refuses a lead outside [0, 1)
        yvid.Movie(g, clips["s30"], lead=2.0)
    with pytest.raises(TypeError):
        yvid.Movie(s, clips["s30"])                  # a Screen is not a Gfx


# --- playing ------------------------------------------------------------------------------


def test_play_to_end(sim, clips):
    s, g = sim
    with yvid.Movie(g, clips["s30"]) as m:
        i = m.info
        assert (i["w"], i["h"], i["frames"], i["fps"]) == (64, 36, 90, 30.0)
        assert i["duration"] == 3.0 and i["backend"] == "seq" and i["path"] == "upload"
        assert "frame sequence" in m.describe() and m.gfx is g and m.open
        assert m.last() is None
        pid = m.play()
        assert pid > 0 and m.result(pid) is None     # not happened yet
        got = play_all(s, g, m)
        assert got[-1] is False and all(got[:-1])
        r = m.records()
        # one record per display frame from the start (an ASAP start waits for
        # the decode thread's first frames) to the last before the end
        assert len(r) <= len(got) - 1 and [x.display for x in r] == list(range(r[0].display, r[0].display + len(r)))
        assert r[-1].display == len(got) - 2         # a fresh screen: display index = loop index
        assert [x.frame for x in r if x.decision == yvid.SHOWN][:3] == [0, 1, 2]
        assert r[-1].frame == 89 and m.records() == []
        for x in r:
            assert x.onset_ns > 0 and x.flags & yvid.F_PENDING == 0
            assert x.residual_ns == x.onset_ns - x.due_ns and x.onset == x.onset_ns / 1e9
            assert x.tier == yscr.TIER_SIM and x.path == yvid.PATH_UPLOAD
            assert x.frame == due_frame(x.movie_t_ns, int(PERIOD * 1e9) // 2, 30, 1) or x.why != yvid.WHY_DUE
        res = m.result(pid)
        assert res["kind"] == "play" and res["id"] == pid and res["frame"] == 0 and res["requested"] == 0.0
        ev = m.events()
        kinds = [e["kind"] for e in ev]
        assert kinds[0] == "open" and "play" in kinds and kinds[-1] == "end"
        assert ev[-1]["frame"] == 89 and ev[0]["fps_num"] == 30 and ev[0]["codec"] == "seq"
        info = m.info
        assert info["shown"] + info["repeated"] == len(r) and info["shown"] >= 1
        assert set(info["repeats_by"]) == set(yvid.WHY_NAMES)
        assert m.records_lost == 0 and m.events_lost == 0
        last = m.last()
        assert last.frame == 89


def test_formats_play(sim, clips):
    s, g = sim
    for k in ("q25", "y30", "s2398"):
        with yvid.Movie(g, clips[k], inline_decode=True) as m:
            m.play()
            play_all(s, g, m)
            r = m.records()
            assert r and r[-1].frame == m.info["frames"] - 1
            assert m.info["hash_mismatch"] == 0 and m.info["ts_mismatch"] == 0


def test_data_and_index_bytes(sim, clips):
    s, g = sim
    with open(clips["s60"], "rb") as f:
        data = f.read()
    with yvid.Movie(g, data=data) as m:
        assert m.info["frames"] == 60
        m.play()
        play_all(s, g, m)
        assert m.records()[-1].frame == 59

    class FakePack:   # ysp.pack.Pack's names() and read()
        def __init__(self, entries):
            self.entries = entries

        def names(self):
            return list(self.entries)

        def read(self, name):
            return self.entries[name]

    with yvid.from_pack(g, FakePack({"video/a.yspseq": data}), "video/a.yspseq") as m:
        assert m.info["frames"] == 60


def test_order_errors(sim, clips):
    s, g = sim
    with yvid.Movie(g, clips["s30"]) as m:
        m.play()
        with pytest.raises(yvid.OrderError):
            m.update()                               # before the screen's begin()
        s.begin()
        assert m.update() is True
        with pytest.raises(yvid.OrderError):
            m.update()                               # once a display frame
        with pytest.raises(yvid.OrderError):
            m.draw()                                 # outside the gfx's frame
        g.begin()
        with pytest.raises(yvid.OrderError):
            m.update()                               # a texture changes only between frames
        m.draw(10, -5, 32, 18, ori=15, opacity=0.5, linear=True, place="top_left", anchor="center")
        with pytest.raises(yvid.ArgumentError):
            m.draw(place="nowhere")
        g.end()
        s.flip()


def frames(s, g, m, n):
    for _ in range(n):
        play_all(s, g, m, limit=1)


def test_controls(sim, clips):
    s, g = sim
    with yvid.Movie(g, clips["s30"], inline_decode=True) as m:
        with pytest.raises(yvid.ArgumentError):
            m.play(at=0)
        with pytest.raises(yvid.ArgumentError):
            m.play(at=-1)
        m.play()
        frames(s, g, m, 10)
        pz = m.pause()
        frames(s, g, m, 5)
        p = m.result(pz)
        assert p["kind"] == "pause" and p["movie_t"] > 0 and p["id"] == pz
        m.records()
        sk = m.seek(2.0, resume="paused")
        frames(s, g, m, 10)
        r = m.result(sk)
        assert r["kind"] == "seek" and r["target"] == 60 and r["id"] == sk
        assert m.movie_time(yscr.now()) == pytest.approx(2.0)      # paused at t(60)
        assert m.records() == []                     # no FRAME records while paused
        m.play()
        frames(s, g, m, 10)
        r = m.records()
        assert r[0].frame == 60                      # the paused seek showed it; play goes on from it
        assert [x.frame for x in r if x.decision == yvid.SHOWN][0] == 61
        sf = m.seek_frame(15)                        # resume as soon as ready
        frames(s, g, m, 10)
        assert m.result(sf)["target"] == 15
        shown = [x.frame for x in m.records() if x.decision == yvid.SHOWN]
        assert 15 in shown and all(f >= 15 for f in shown[shown.index(15):])
        s.begin()
        m.show(40)                                   # manual: this display frame shows frame 40
        m.update()
        g.begin(); m.draw(); g.end()
        s.flip()
        assert m.last().frame == 40 and m.last().why == yvid.WHY_MANUAL
        with pytest.raises(yvid.ArgumentError):
            m.seek(-1.0)
        with pytest.raises(yvid.ArgumentError):
            m.seek(1.0, resume="later")
        with pytest.raises(yvid.NotFoundError):
            m.result(10_000)


def test_loop(sim, clips, tmp_path):
    s, g = sim
    p = write_seq(str(tmp_path / "s250.yspseq"), 16, 8, int(round(1 / PERIOD)), 10)   # 1:1 on the display
    with yvid.Movie(g, p, loop=True, inline_decode=True) as m:
        m.play()
        got = play_all(s, g, m, limit=25)           # 2.5 cycles
        assert all(got)
        loops = [e for e in m.events() if e["kind"] == "loop"]
        assert [e["cycle"] for e in loops] == [1, 2]
        f = [x.frame for x in m.records()]
        assert max(f) == 9 and f[-1] < 9            # the third cycle


def test_close_with_gfx_and_screen(clips):
    with yscr.Screen(backend="sim", sim_period=PERIOD) as s:
        g = ygfx.Gfx(s)
        m = yvid.Movie(g, clips["s30"], inline_decode=True)   # inline: the first frame is ready at once
        m.play()
        play_all(s, g, m, limit=5)                   # its wait_flip() completes the fifth record
        g.close()                                    # its movies close first
        assert not m.open
        with pytest.raises(yvid.ClosedError):
            m.update()
        assert len(m.records()) == 5                 # what was recorded stays readable
        m.close()                                    # safe after
        g2 = ygfx.Gfx(s)
        m2 = yvid.Movie(g2, clips["s30"])
    assert not m2.open                               # the screen closed it
    with pytest.raises(yvid.ClosedError):
        m2.play()
    del m, m2, g, g2


def test_records_ring(sim, clips):
    s, g = sim
    with yvid.Movie(g, clips["s60"], records=16, inline_decode=True) as m:
        m.play()
        got = play_all(s, g, m)
        r = m.records()
        assert len(r) == 16 and m.records_lost == len(got) - 1 - 16
        assert r[-1].frame == 59


# --- the frames per predicted onset ---------------------------------------------------------


def sim_run(clip, out, index=False):
    cmd = [sys.executable, os.path.join(HERE, "video_sim_run.py"), clip, out] + (["--index"] if index else [])
    subprocess.run(cmd, check=True, timeout=120)
    with open(out, newline="") as f:
        return [tuple(int(v) for v in row) for row in list(csv.reader(f))[1:]]


def check_rule(rows, num, den, frames):
    """Each display frame's frame, decision and reason against the due rule,
    the cadence and the shows (docs/video.md, "Decisions"); no drops on the
    virtual clock."""
    anchor = rows[0][2]
    period = rows[1][2] - rows[0][2]
    per = (10**9 * den) / (period * num)            # display frames per video frame
    # within 200 ppm of a whole number the cadence is a multiple (CADENCE AND SLIPS)
    limit = round(per) if abs(per - round(per)) <= 200e-6 * per else math.floor(per)
    shown, shows = -1, 0
    for display, vblank, onset, decision, why, frame, movie_t in rows:
        assert movie_t == onset - anchor
        i = due_frame(movie_t, period // 2, num, den)
        assert frame == i < frames
        if i != shown:
            assert decision == yvid.SHOWN and why == yvid.WHY_DUE
            shown, shows = i, 1
        else:
            shows += 1
            assert decision == yvid.REPEATED
            assert why == (yvid.WHY_CADENCE if shows > limit else yvid.WHY_DUE)
    assert shown == frames - 1


@pytest.mark.parametrize("name,num,den,n", [("s30", 30, 1, 90), ("s60", 60, 1, 60), ("s2398", 24000, 1001, 48),
                                             ("q25", 25, 1, 25)])
def test_frames_per_predicted_onset(clips, tmp_path, name, num, den, n):
    a = sim_run(clips[name], str(tmp_path / "a.csv"))
    b = sim_run(clips[name], str(tmp_path / "b.csv"))
    assert a == b                                    # the virtual clock: the same run each time
    check_rule(a, num, den, n)
    if name == "s2398":
        assert sum(1 for r in a if r[4] == yvid.WHY_CADENCE) == n // 2   # 3, 2, 3, 2 on 60 Hz
    play = os.environ.get("YSP_VIDEO_PLAY")
    if play:
        c = str(tmp_path / "c.csv")
        subprocess.run([play, "--sim", "--file", clips[name], "--csv", c], check=True, timeout=120)
        with open(c, newline="") as f:
            cref = [tuple(int(v) for v in row[:7]) for row in list(csv.reader(f))[1:]]
        assert a == cref                             # the C path, line by line


@pytest.mark.parametrize("name,num,den,n", [("m30", 30, 1, 60), ("m2398", 24000, 1001, 48)])
def test_mpeg1_frames_per_predicted_onset(mpeg, tmp_path, name, num, den, n):
    a = sim_run(mpeg[name], str(tmp_path / "a.csv"))
    check_rule(a, num, den, n)
    play = os.environ.get("YSP_VIDEO_PLAY")
    if play:
        c = str(tmp_path / "c.csv")
        subprocess.run([play, "--sim", "--file", mpeg[name], "--csv", c], check=True, timeout=120)
        with open(c, newline="") as f:
            assert a == [tuple(int(v) for v in row[:7]) for row in list(csv.reader(f))[1:]]


def test_mpeg1(sim, mpeg):
    s, g = sim
    i = yvid.probe(mpeg["m30"])
    assert (i["codec"], i["format"], i["frames"], i["fps_num"], i["w"]) == ("mpeg1", "i420", 60, 30, 160)
    assert (i["matrix"], i["range"], i["siting"]) == ("bt601", "limited", "center")
    with open(mpeg["m30"] + ".yspvi", "rb") as f:
        idx = f.read()
    with open(mpeg["m30"], "rb") as f:
        data = f.read()
    for m in (yvid.Movie(g, mpeg["m30"]), yvid.Movie(g, data=data, index=idx),
              yvid.Movie(g, mpeg["m30"], index=mpeg["m30"] + ".yspvi")):
        with m:
            assert m.info["backend"] == "plmpeg"
            m.play()
            play_all(s, g, m)
            r = m.records()
            assert r[-1].frame == 59
            assert m.info["hash_mismatch"] == 0 and m.info["ts_mismatch"] == 0
    with pytest.raises(yvid.Error):                  # MPEG-1 needs its index
        yvid.Movie(g, data=data)


@pytest.fixture(scope="module")
def mp4(tmp_path_factory):
    if not FFMPEG:
        pytest.skip("ffmpeg is not on PATH")
    d = tmp_path_factory.mktemp("mp4")
    p = str(d / "c30.mp4")
    vui = "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv:chromaloc=0"
    try:
        ff("-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30", "-frames:v", "60", "-c:v", "libx264", "-preset",
           "veryfast", "-pix_fmt", "yuv420p", "-profile:v", "high", "-bf", "0", "-g", "15", "-keyint_min", "15",
           "-sc_threshold", "0", "-x264-params", "bframes=0:scenecut=0:open-gop=0:" + vui, "-color_primaries",
           "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-color_range", "tv",
           "-chroma_sample_location", "left", "-video_track_timescale", "30000", "-an", p)
    except subprocess.CalledProcessError:
        pytest.skip("ffmpeg has no libx264")
    try:
        n = yvid.make_index(p, matrix="bt709", range="limited", transfer="bt1886", primaries="bt709", siting="left")
    except yvid.Error as e:
        pytest.skip("Media Foundation: %s" % e)
    assert n == 60
    return p


def test_mp4_media_foundation(sim, mp4):
    s, g = sim
    i = yvid.probe(mp4)
    assert (i["codec"], i["format"], i["frames"], i["fps_num"], i["fps_den"]) == ("h264", "nv12", 60, 30, 1)
    for hw in ("off", "auto"):
        with yvid.Movie(g, mp4, hw_decode=hw) as m:
            assert m.info["backend"] == "mf"
            m.play()
            play_all(s, g, m)
            assert m.records()[-1].frame == 59
            assert m.info["hash_mismatch"] == 0 and m.info["ts_mismatch"] == 0


# --- sound and the timeline ---------------------------------------------------------------


def write_wav(path, frames, rate=48000, channels=2):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        x = array.array("h", [0]) * (frames * channels)
        for k in range(0, frames * channels, 97):
            x[k] = 1000
        w.writeframes(x.tobytes())
    return path


def test_soundtrack_null_device(sim, clips, tmp_path):
    s, g = sim
    a = yau.Audio(backend="null")
    try:
        rate = a.caps()["rate"]
        wav = write_wav(str(tmp_path / "s30.wav"), 3 * rate)            # 90 frames at 30 fps: 3 s
        short = write_wav(str(tmp_path / "short.wav"), 3 * rate - 1)
        m = yvid.Movie(g, clips["s30"])
        with pytest.raises(yvid.RefusedError):
            m.soundtrack(a, short)                   # the length must be the movie's
        assert a.pins == 0                           # a refusal keeps no pin
        with pytest.raises(TypeError):
            m.soundtrack(object(), wav)              # not a ysp.audio.Audio
        m.soundtrack(a, wav, ramp=0.002)
        assert a.pins == 2                           # the soundtrack and the follow
        with pytest.raises(yvid.OrderError):
            m.soundtrack(a, wav)
        with pytest.raises(yau.BusyError):
            a.close()                                # the table's lifetime rule
        m.play()
        play_all(s, g, m)
        r = m.records()
        assert r and r[-1].frame == 89
        followed = [x for x in r if x.flags & yvid.F_AUDIO_CLOCK]
        assert followed                              # the movie clock followed the sound
        snd = [e for e in m.events() if e["kind"] == "sound"]
        assert snd and snd[0]["action"] == "start" and snd[0]["sample"] == 0 and snd[0]["play_id"] > 0
        on = a.wait(snd[0]["play_id"], 2.0)
        assert on.target_ns == snd[0]["target_ns"]
        m.follow_audio(None)
        assert a.pins == 1
        m.follow_audio(a)
        assert a.pins == 2
        m.close()
        assert a.pins == 0
    finally:
        a.close()


def test_soundtrack_released_when_screen_closes(clips, tmp_path):
    a = yau.Audio(backend="null")
    try:
        wav = write_wav(str(tmp_path / "s.wav"), 3 * a.caps()["rate"])
        s = yscr.Screen(backend="sim", sim_period=PERIOD)
        g = ygfx.Gfx(s)
        m = yvid.Movie(g, clips["s30"])
        m.soundtrack(a, wav, follow=False)
        assert a.pins == 1
        s.close()                                    # without the GIL: the pin goes back with it
        assert not m.open and a.pins == 0
        del m, g, s
    finally:
        a.close()


@pytest.mark.skipif(os.environ.get("YSP_VIS_AUDIO") != "1", reason="set YSP_VIS_AUDIO=1 (plays on the default output)")
def test_soundtrack_default_device(sim, clips, tmp_path):
    s, g = sim
    with yau.Audio() as a:
        rate = a.caps()["rate"]
        ch = a.caps()["channels"]
        wav = write_wav(str(tmp_path / "d.wav"), 3 * rate, rate=rate, channels=ch)
        with yvid.Movie(g, clips["s30"]) as m:
            m.soundtrack(a, wav, db=-40)
            m.play()
            play_all(s, g, m)
            r = m.records()
            assert r[-1].frame == 89 and any(x.flags & yvid.F_AUDIO_CLOCK for x in r)
            snd = [e for e in m.events() if e["kind"] == "sound"]
            on = a.wait(snd[0]["play_id"], 2.0)
            assert abs(on.residual) < 0.001


def test_timeline_base(sim, clips):
    s, g = sim
    tl = ytl.Timeline(["x"])
    m = yvid.Movie(g, clips["s30"], timeline=tl, base=1, inline_decode=True)
    with pytest.raises(ytl.BusyError):
        tl.close()                                   # pinned while the movie lives
    m.play()
    for _ in range(20):
        f = s.begin()
        m.update()
        tl.evaluate(f.onset, f.period)
        g.begin(); m.draw(); g.end()
        s.flip()
    assert tl.base_time(1, f.onset) == pytest.approx(m.movie_time(f.onset), abs=1e-9)
    m.close()
    tl.close()
    with pytest.raises(yvid.Error):
        yvid.Movie(g, clips["s30"], timeline=ytl.Timeline(["x"]), base=0)   # a base 1..7 with a timeline
