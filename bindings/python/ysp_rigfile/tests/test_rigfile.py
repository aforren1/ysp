"""Tests for ysp.rigfile: the binding's surface on profiles it writes and
reads, the loopback checks, the user folder and its planted-folder refusal
(POSIX). tests/adapt/rigfile_test.c checks the header itself."""
import hashlib
import math
import os
import re
import shutil
import stat
import sys

import pytest

import ysp.rigfile as yr

LATFILE = (
    "# ysp output latency\n"
    "format ysp-out-latency 1\n"
    "date 2026-10-09T13:58:12Z\n"
    "n 200\n"
    "missed 0\n"
    "min_s 0.0001500\n"
    "p5_s 0.0002010\n"
    "median_s 0.0005770\n"
    "p95_s 0.0009830\n"
    "max_s 0.0021000\n"
    "mean_s 0.0006000\n"
)

BNDFILE = (
    "format ysp-in-bounds 1\n"
    "date 2026-10-13T10:00:00Z\n"
    "in_family line\n"
    "n 398\n"
    "missed 2\n"
    "lo_s -0.000312000\n"
    "median_s 0.000101000\n"
    "hi_s 0.001104000\n"
)


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def profile_text(lat_sha, bnd_sha):
    return (
        '{"format": "ysp-rig 1", "rig": "booth-2", "written": "2026-10-09T14:03:00Z",\n'
        ' "roles": {\n'
        '  "trig": {"family": "triggerbox", "key": "serial:0403:6001:TB0123:", "pulse_s": 0.002,\n'
        '           "latency": {"n": 200, "p5_s": 0.000201, "median_s": 0.000577, "p95_s": 0.000983,\n'
        '                       "max_s": 0.0021, "date": "2026-10-09T13:58:12Z", "sha256": "%s"}},\n'
        '  "resp": {"family": "line", "key": "serial:16C0:0483:B1:",\n'
        '           "options": {"buttons": {"1": "left", "2": "right"}},\n'
        '           "bounds": {"n": 398, "lo_s": -0.000312, "hi_s": 0.001104,\n'
        '                      "date": "2026-10-13T10:00:00Z", "sha256": "%s"}},\n'
        '  "eeg": {"family": "lsl", "key": "lsl:EEG:EEG:abc:host"}\n'
        ' },\n'
        ' "view": {"distance_mm": 570, "width_mm": 518.4, "width_px": 1920}\n'
        '}\n' % (lat_sha, bnd_sha))


@pytest.fixture
def rigdir(tmp_path):
    """A folder with the profile and its two loopback files."""
    d = tmp_path / "rig"
    (d / "loopback").mkdir(parents=True)
    for text in (LATFILE, BNDFILE):
        with open(d / "loopback" / (sha(text) + ".txt"), "w", newline="\n") as f:
            f.write(text)
    with open(d / "profile.json", "w", newline="\n") as f:
        f.write(profile_text(sha(LATFILE), sha(BNDFILE)))
    return d


def test_version_matches_pyproject():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    text = open(os.path.join(here, "pyproject.toml")).read()
    m = re.search(r'^version\s*=\s*"([^"]+)"', text, re.M)
    assert m and m.group(1) == yr.__version__


def test_new_profile_canonical_text_and_hash():
    p = yr.Profile("booth-2")
    assert p.rig == "booth-2" and p.roles == [] and p.view is None and p.ppd == 0.0
    p.bind("resp", "line", "serial:16C0:0483:B1:")
    p.bind("trig", 4, "serial:0403:6001:TB0123:")   # a TriggerBox by its number
    assert p.roles == ["resp", "trig"]
    assert p.role("trig")["family"] == "triggerbox"
    h0 = p.hash
    p.set_view(570, 518.4, 1920)
    assert p.view == {"distance_mm": 570.0, "width_mm": 518.4, "width_px": 1920}
    assert abs(p.ppd - 36.8496) < 1e-4          # docs/device.md's example
    assert p.hash != h0 and len(p.hash) == 64
    text = p.text
    assert text.endswith("}\n") and '"format": "ysp-rig 1"' in text
    q = yr.parse(text)
    assert q.hash == p.hash and q.text == text and q.notes == [] and q.dir == ""
    p.clear_view()
    assert p.hash == h0
    assert p.remove("resp") and not p.remove("resp")
    with pytest.raises(ValueError):
        p.set_view(0, 518.4, 1920)
    with pytest.raises(yr.Error):
        p.bind("bad name", "line", "serial:::")
    with pytest.raises(yr.Error):
        p.bind("e", "lsl", "serial:1:2::")       # lsl needs an lsl: key
    with pytest.raises(ValueError):
        p.bind("e", "nofamily", "serial:1:2::")


def test_desc_is_device_kwargs():
    p = yr.parse(profile_text("0" * 64, "1" * 64))
    d = p.desc("trig")
    assert d == {"role": "trig", "family": 4, "key": "serial:0403:6001:TB0123:", "baud": 0, "latched": False,
                 "pulse": 0.002}
    assert "pulse" not in p.desc("resp")
    with pytest.raises(yr.Missing):
        p.desc("nope")
    with pytest.raises(yr.Error) as ei:
        p.desc("eeg")
    assert "ysp/net.h" in ei.value.args[0] and ei.value.args[1] == yr.ERR_FORMAT


def test_role_dict_in_seconds():
    p = yr.parse(profile_text("0" * 64, "1" * 64))
    r = p.role("trig")
    assert r["pulse"] == 0.002 and r["ftdi_latency"] is None and r["bounds"] is None
    lat = r["latency"]
    assert lat["n"] == 200 and lat["median"] == 0.000577 and lat["max"] == 0.0021
    assert lat["check"] == yr.CHECK_NOT_RUN and lat["check_name"] == "not run"
    r = p.role("resp")
    assert r["buttons"] == {"1": "left", "2": "right"} and r["pulse"] is None
    assert r["bounds"]["lo"] == -0.000312 and r["bounds"]["lo_ns"] == -312000
    assert p.role("eeg")["family"] == "lsl" and p.role("eeg")["family_id"] == yr.FAMILY_LSL
    with pytest.raises(yr.Missing):
        p.role("nope")


def test_parse_errors_name_the_field():
    with pytest.raises(yr.Error) as ei:
        yr.parse("{")
    assert ei.value.args[1] == yr.ERR_JSON
    bad = profile_text("0" * 64, "1" * 64).replace('"pulse_s"', '"pulse_ms"')
    with pytest.raises(yr.Error) as ei:
        yr.parse(bad)
    assert ei.value.args[1] == yr.ERR_FORMAT and "pulse_ms" in ei.value.args[0]


def test_checks_at_load_and_tiers(rigdir):
    p = yr.load_file(str(rigdir / "profile.json"))
    assert p.notes == [], p.notes
    assert p.role("trig")["latency"]["check_name"] == "ok"
    assert p.role("resp")["bounds"]["check_name"] == "ok"
    s = p.source("resp")
    assert s["tier"] == yr.TIER_1 and s["lo"] == -0.000312 and s["hi"] == 0.001104
    assert p.source("trig")["tier"] == yr.TIER_UNKNOWN    # an output has no tier
    assert p.source("nope")["tier"] == yr.TIER_UNKNOWN
    # a changed bounds file: a note, and tier UNKNOWN; the numbers stay
    with open(rigdir / "loopback" / (sha(BNDFILE) + ".txt"), "a", newline="\n") as f:
        f.write("# edited\n")
    q = yr.load_file(str(rigdir / "profile.json"))
    assert len(q.notes) == 1 and "resp" in q.notes[0]
    assert q.role("resp")["bounds"]["check_name"] == "changed"
    assert q.source("resp")["tier"] == yr.TIER_UNKNOWN
    os.remove(rigdir / "loopback" / (sha(LATFILE) + ".txt"))
    q = yr.load_file(str(rigdir / "profile.json"))
    assert q.role("trig")["latency"]["check_name"] == "missing" and len(q.notes) == 2
    # without a folder no check runs
    assert yr.parse((rigdir / "profile.json").read_text()).role("resp")["bounds"]["check"] == yr.CHECK_NOT_RUN


def test_save_file_round_trip(tmp_path):
    p = yr.Profile("booth-3")
    p.bind("trig", "mmbts", "serial:2341:0043::")
    path = str(tmp_path / "x.json")
    p.save_file(path)
    q = yr.load_file(path)
    assert q.written != "1970-01-01T00:00:00Z" and q.roles == ["trig"]
    assert q.hash == p.hash        # save set written in p too


def user_base(tmp_path):
    # yrt_user_dir refuses a folder under one that all users can write
    # (/tmp), so on POSIX the base goes under HOME, as the C test does.
    if sys.platform == "win32":
        return str(tmp_path / "cfg")
    base = os.path.join(os.path.expanduser("~"), ".cache", "ysp_rigfile_pytest_%d" % os.getpid())
    os.makedirs(base, mode=0o700, exist_ok=True)
    return base


def point_config(monkeypatch, base):
    if sys.platform == "win32":
        monkeypatch.setenv("APPDATA", base)
    elif sys.platform == "darwin":
        monkeypatch.setenv("HOME", base)
    else:
        monkeypatch.setenv("XDG_CONFIG_HOME", base)


def test_user_folder(tmp_path, monkeypatch):
    base = user_base(tmp_path)
    try:
        point_config(monkeypatch, base)
        d = yr.dir()
        assert d.replace("\\", "/").endswith("ysp/rig")
        assert yr.path() == os.path.join(d, "profile.json")
        assert yr.path("booth2") == os.path.join(d, "booth2.json")
        with pytest.raises(yr.Error):
            yr.path("../x")
        with pytest.raises(yr.Missing):
            yr.load()
        p = yr.Profile("booth-2")
        p.bind("resp", "xid", "serial:0403:6001:FT4ABC12:")
        p.save()
        p.save("booth2")
        q = yr.load()
        assert q.dir == d and q.roles == ["resp"] and q.hash == p.hash
        assert yr.load("booth2").hash == p.hash
        with pytest.raises(yr.Missing):
            yr.load("booth3")
    finally:
        if sys.platform != "win32":
            shutil.rmtree(base, ignore_errors=True)


@pytest.mark.skipif(sys.platform == "win32", reason="yrt_user_dir reads no ACL on Windows")
def test_planted_folder_is_refused(tmp_path, monkeypatch):
    base = user_base(tmp_path)
    try:
        planted = os.path.join(base, "planted")
        os.makedirs(planted, exist_ok=True)
        os.chmod(planted, 0o777)
        point_config(monkeypatch, planted)
        with pytest.raises(yr.Folder) as ei:
            yr.dir()
        assert ei.value.args[1] == yr.ERR_FOLDER
        with pytest.raises(yr.Folder) as ei:
            yr.load()
        assert "another user can write it" in ei.value.args[0]
        with pytest.raises(yr.Folder):
            yr.Profile("x").save()
        assert not os.path.exists(os.path.join(planted, "ysp", "rig", "profile.json"))
        # a good base with a planted rig folder under it
        good = os.path.join(base, "good")
        os.makedirs(good, mode=0o700, exist_ok=True)
        point_config(monkeypatch, good)
        yr.Profile("x").save()
        rig = yr.dir()
        os.chmod(rig, 0o777)
        with pytest.raises(yr.Folder):
            yr.load()
        os.chmod(rig, 0o700)
        assert yr.load().rig == "x"
    finally:
        shutil.rmtree(base, ignore_errors=True)


def test_source_of_a_device(rigdir):
    yd = pytest.importorskip("ysp.device")
    p = yr.load_file(str(rigdir / "profile.json"))
    sim = yd.Sim("line", key="serial:16C0:0483:B1:1-1")
    with yd.Device(**p.desc("resp"), sim=sim) as dev:
        assert dev.wait_running(3.0), dev.error
        s = p.source(dev)
        assert s["tier"] == yr.TIER_1 and s["device"] == dev.device and s["kind"] == yd.KIND_BOX
        assert s["lo_ns"] == -312000
    # a device of another family than the role's gets no tier
    ts = yd.Sim("triggerbox")
    with yd.Device("resp", "triggerbox", ts.key, sim=ts) as dev:
        assert p.source(dev)["tier"] == yr.TIER_UNKNOWN
    with pytest.raises(TypeError):
        p.source(42)
