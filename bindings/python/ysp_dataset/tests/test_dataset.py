"""Tests for ysp.dataset: the binding's surface against ysp/dataset.h's own
rules (tests/adapt/dataset_test.c checks the header itself)."""
import csv
import gc
import io
import json
import math
import os
import re
import subprocess
import sys
import warnings
import zipfile

import pytest

import ysp.dataset as yds

COLS = [
    ("participant", "const", "P07"),
    ("trial_index", "int"),
    ("response", "string"),
    ("correct", "bool"),
    ("rt_s", "time"),
    ("contrast", "double"),
    ("big", "int64"),
]


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.reader(f))


def session(d):
    with open(os.path.join(d, "session.json"), "rb") as f:
        text = f.read()
    assert text.endswith(b"}\n") and b"\r" not in text
    return json.loads(text)


@pytest.fixture
def ds(tmp_path):
    d = yds.Dataset(str(tmp_path / "P07"), COLS, session={"experiment": "gabor", "seed": 20261021})
    yield d
    d.close()


def test_version_matches_pyproject():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(here, "pyproject.toml"), encoding="utf-8") as f:
        text = f.read()
    assert re.search(r'^version = "([^"]+)"', text, re.M).group(1) == yds.__version__ == yds.version()


def test_constants():
    assert yds.FORMAT == "ysp-session" and yds.FORMAT_VERSION == 1
    assert yds.MAX_STREAMS == 16 and yds.MAX_COLUMNS == 256
    assert len(yds.FRAME_COLUMNS) == 17 and yds.FRAME_COLUMNS[:6] == (
        "frame", "trial_index", "target_s", "planned_s", "onset_s", "residual_s")
    assert len(yds.EVENT_COLUMNS) == 20 and yds.EVENT_COLUMNS[0] == "t_s" and yds.EVENT_COLUMNS[-1] == "text"
    assert len(yds.CLOCK_COLUMNS) == 11 and yds.CLOCK_COLUMNS[:3] == ("role", "kind", "t_s")
    assert issubclass(yds.ArgumentError, ValueError) and issubclass(yds.ExistsError, FileExistsError)
    assert issubclass(yds.WriteError, OSError) and issubclass(yds.ClosedError, yds.Error)


def test_folder_and_session_at_open(ds, tmp_path):
    d = tmp_path / "P07"
    assert sorted(os.listdir(d)) == ["clock.csv", "events.csv", "frames.csv", "session.json", "streams",
                                     "trials.csv"]
    s = session(d)
    assert s["closed"] is False and s["format"] == "ysp-session" and s["version"] == 1
    assert s["experiment"] == "gabor" and s["seed"] == 20261021
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", s["start_utc"])
    assert "rows" not in s
    assert ds.dir == str(d) and ds.is_open and not ds.finished
    assert ds.trial_columns == tuple(c[0] for c in COLS)
    assert read_csv(d / "trials.csv") == [[c[0] for c in COLS]]
    assert read_csv(d / "frames.csv") == [list(yds.FRAME_COLUMNS)]
    assert read_csv(d / "events.csv") == [list(yds.EVENT_COLUMNS)]
    assert read_csv(d / "clock.csv") == [list(yds.CLOCK_COLUMNS)]


def test_trial_rows_and_missing_values(ds, tmp_path):
    ds.write_trial(trial_index=0, response='left, "far"\nline', correct=True, rt_s=0.5123456789, contrast=0.25,
                   big=2**62)
    ds.write_trial({"trial_index": 1, "correct": False, "rt_s": -0.0125, "contrast": 1e-7})
    ds.write_trial(trial_index=2, response=None, correct=None, rt_s=float("nan"), contrast=float("nan"))
    ds.write_trial(trial_index=3.0, correct=1, rt_s=1.184516667, response=b"bytes \xc3\xa9")
    ds.write_trial()
    rows = read_csv(tmp_path / "P07" / "trials.csv")[1:]
    assert rows[0] == ["P07", "0", 'left, "far"\nline', "1", "0.512345679", "0.25", str(2**62)]
    assert rows[1] == ["P07", "1", "", "0", "-0.012500000", "1e-07", ""]
    assert rows[2] == ["P07", "2", "", "", "", "", ""]
    assert rows[3] == ["P07", "3", "bytes \u00e9", "1", "1.184516667", "", ""]
    assert rows[4] == ["P07", "", "", "", "", "", ""]
    assert ds.rows()["trials"] == 5


def test_text_is_utf8(ds, tmp_path):
    ds.write_trial(response="l\u00e9ft \U0001F600 \u4e2d")
    ds.write_trial(response="\ud800 lone")   # not UTF-8: the writer writes U+FFFD
    with open(tmp_path / "P07" / "trials.csv", "rb") as f:
        lines = f.read().split(b"\n")
    assert lines[1] == "P07,,l\u00e9ft \U0001F600 \u4e2d,,,,".encode("utf-8")
    assert lines[2].decode("utf-8") == "P07,,\ufffd\ufffd\ufffd lone,,,,"


def test_values_refused(ds):
    with pytest.raises(yds.ArgumentError, match="no column 'nope'"):
        ds.write_trial(nope=1)
    with pytest.raises(yds.ArgumentError, match="participant is constant"):
        ds.write_trial(participant="P08")
    with pytest.raises(yds.ArgumentError, match="trial_index: an integer"):
        ds.write_trial(trial_index=2.5)
    with pytest.raises(yds.ArgumentError, match="trial_index: an integer"):
        ds.write_trial(trial_index="3")
    with pytest.raises(yds.ArgumentError, match="out of range"):
        ds.write_trial(trial_index=2**31)
    with pytest.raises(yds.ArgumentError, match="out of range"):
        ds.write_trial(big=2**63)
    with pytest.raises(yds.ArgumentError, match="correct: a bool"):
        ds.write_trial(correct=2)
    with pytest.raises(yds.ArgumentError, match="rt_s: seconds"):
        ds.write_trial(rt_s=math.inf)
    with pytest.raises(yds.ArgumentError, match="rt_s: seconds"):
        ds.write_trial(rt_s=1e10)
    with pytest.raises(yds.ArgumentError, match="not text"):
        ds.write_trial(rt_s="0.5")
    with pytest.raises(yds.ArgumentError, match="response: a str"):
        ds.write_trial(response=3)
    with pytest.raises(yds.ArgumentError, match="NUL"):
        ds.write_trial(response="a\0b")
    with pytest.raises(TypeError):
        ds.write_trial({"trial_index": 1}, response="x")
    with pytest.raises(TypeError):
        ds.write_trial([1, 2])
    # A refused row writes nothing and sets nothing for the next one.
    ds.write_trial(trial_index=9)
    assert ds.rows()["trials"] == 1


def test_columns_refused(tmp_path):
    bad = [
        ([("rt", "time")], "ends in _s"),
        ([("rt_ns", "time")], "ends in _s"),
        ([("x", "float32")], "is not int"),
        ([("p", "const")], "const column is"),
        ([("x", "int", 3)], "only double and time"),
        ([("x", "double", 18)], "decimals"),
        ([("1x", "int")], "1x"),
        ([("a", "int"), ("a", "int")], "a"),
        ([], "1 to 256"),
        ([("x",)], "not \\(name, type\\)"),
    ]
    for i, (cols, msg) in enumerate(bad):
        d = tmp_path / ("bad%d" % i)
        with pytest.raises(yds.ArgumentError, match=msg):
            yds.Dataset(str(d), cols)
        assert not d.exists()   # a refused column makes nothing


def test_open_refusals(tmp_path):
    (tmp_path / "there").mkdir()
    with pytest.raises(yds.ExistsError):
        yds.Dataset(str(tmp_path / "there"), [("x", "int")])
    with pytest.raises(yds.WriteError):
        yds.Dataset(str(tmp_path / "no" / "parent"), [("x", "int")])
    with pytest.raises(yds.ArgumentError, match="'closed'"):
        yds.Dataset(str(tmp_path / "a"), [("x", "int")], session={"closed": True})
    with pytest.raises(yds.ArgumentError, match="NaN"):
        yds.Dataset(str(tmp_path / "b"), [("x", "int")], session={"v": math.nan})
    with pytest.raises(yds.ArgumentError, match="keys are str"):
        yds.Dataset(str(tmp_path / "c"), [("x", "int")], session={1: 2})
    with pytest.raises(yds.ArgumentError, match="not set"):
        yds.Dataset(str(tmp_path / "d"), [("x", "int")], session={"v": {1, 2}})
    assert sorted(os.listdir(tmp_path)) == ["there"]
    # A pathlib path and a given start_utc.
    ds = yds.Dataset(tmp_path / "e", [("x", "int")], session={"start_utc": "2026-10-21T14:03:12Z"})
    ds.finish()
    assert session(tmp_path / "e")["start_utc"] == "2026-10-21T14:03:12Z"
    with pytest.raises(yds.Error, match="opened already"):
        ds.__init__(str(tmp_path / "f"), [("x", "int")])


def test_fixed_tables(ds, tmp_path):
    ds.write_frame(frame=120, trial_index=3, target_s=10.0, planned_s=10.0, onset_s=10.000412, residual_s=0.000412,
                   dropped=0, path=1, tier=1, flags=0, code_risk=0, evaluate_s=0.0001, script_s=0.0002,
                   draw_s=0.0003, upload_s=0.0004, swap_s=0.0005, gpu_s=0.0006)
    ds.write_frame({"frame": 121})
    ds.write_event(t_s=10.25, trial_index=3, kind="keyboard", role="kb", stimulus=None, frame=121, code=37,
                   control=80, type=1, value=1.0, x=0.5, y=-0.5, tier=2, flags=4, ticks=2**40, text="left")
    ds.write_event(t_s=10.0, kind="out", role="ttl", planned_s=10.0, deadline_s=10.001, end_s=10.00002,
                   residual_s=-0.000001, code=8)
    ds.write_clock(role="box", kind="pair", t_s=1.5, ticks=1000, width_s=0.00002)
    ds.write_clock(role="box", kind="fit", t_s=1.5, ticks=1000, ns_per_tick=1000.000123, ppm=0.123,
                   spread_s=0.000003, span_s=60.0, points=40, epoch=1)
    d = tmp_path / "P07"
    fr = read_csv(d / "frames.csv")
    assert fr[1] == ["120", "3", "10.000000000", "10.000000000", "10.000412000", "0.000412000", "0", "1", "1",
                     "0", "0", "0.000100000", "0.000200000", "0.000300000", "0.000400000", "0.000500000",
                     "0.000600000"]
    assert fr[2] == ["121"] + [""] * 16
    ev = read_csv(d / "events.csv")
    assert ev[1] == ["10.250000000", "3", "keyboard", "kb", "", "121", "", "", "37", "80", "1", "1", "0.5", "-0.5",
                     "", "", "2", "4", str(2**40), "left"]
    assert ev[2] == ["10.000000000", "", "out", "ttl", "", "", "10.000000000", "-0.000001000", "8", "", "", "", "",
                     "", "10.001000000", "10.000020000", "", "", "", ""]
    ck = read_csv(d / "clock.csv")
    assert ck[1] == ["box", "pair", "1.500000000", "1000", "0.000020000", "", "", "", "", "", ""]
    assert ck[2] == ["box", "fit", "1.500000000", "1000", "", "1000.000123", "0.123", "0.000003000",
                     "60.000000000", "40", "1"]
    with pytest.raises(yds.ArgumentError, match="no column 'onset'"):
        ds.write_frame(onset=1.0)
    with pytest.raises(yds.ArgumentError, match="kind: a str"):
        ds.write_event(kind=3)
    assert ds.rows() == {"trials": 0, "frames": 2, "events": 2, "clock": 2, "streams": {}}


def test_streams(ds, tmp_path):
    dots = ds.stream("dots", [("t_s", "time"), ("coherence", "double", 3), ("n", "int")])
    eye = ds.stream("eye", [("t_s", "time", 6), ("x", "double"), ("y", "double"), ("eye", "const", "L")])
    assert (dots, eye) == (0, 1) and ds.streams == ("dots", "eye")
    ds.write_stream(dots, t_s=0.0166, coherence=0.51234, n=200)
    ds.write_stream("dots", {"t_s": 0.0333})
    ds.write_stream("eye", t_s=0.0010004, x=1.5, y=-2.25)
    with pytest.raises(yds.ArgumentError, match="taken"):
        ds.stream("DOTS", [("x", "int")])
    with pytest.raises(yds.ArgumentError, match="not a name"):
        ds.stream("my dots", [("x", "int")])
    with pytest.raises(yds.ArgumentError, match="no such stream"):
        ds.write_stream("nope", x=1)
    with pytest.raises(yds.ArgumentError, match="no such stream"):
        ds.write_stream(5, x=1)
    with pytest.raises(yds.ArgumentError, match="no column 'x'"):
        ds.write_stream(dots, x=1)
    for i in range(14):
        ds.stream("s%d" % i, [("v", "int")])
    with pytest.raises(yds.FullError):
        ds.stream("s99", [("v", "int")])
    d = tmp_path / "P07" / "streams"
    assert read_csv(d / "dots.csv") == [["t_s", "coherence", "n"], ["0.016600000", "0.512", "200"],
                                        ["0.033300000", "", ""]]
    assert read_csv(d / "eye.csv") == [["t_s", "x", "y", "eye"], ["0.001000", "1.5", "-2.25", "L"]]
    r = ds.finish()
    assert r["streams"]["dots"] == 2 and r["streams"]["eye"] == 1 and r["streams"]["s13"] == 0
    assert session(tmp_path / "P07")["rows"]["streams"] == r["streams"]


def test_session_set_and_push(ds, tmp_path):
    ds.session_set("fields", {"participant": "P07", "age": 31, "big": 2**70, "list": [1, 2.5, None, True, "x"]})
    ds.session_push("changes", {"t_s": 61.5, "key": "contrast", "value": 0.2})
    ds.session_push("changes", {"t_s": 90.0, "key": "contrast", "value": 0.1})
    s = session(tmp_path / "P07")
    assert s["fields"]["list"] == [1, 2.5, None, True, "x"] and s["fields"]["big"] == 2**70
    assert [c["value"] for c in s["changes"]] == [0.2, 0.1] and s["closed"] is False
    for key in ("format", "version", "closed", "rows", "write_errors"):
        with pytest.raises(yds.ArgumentError, match="writer's key"):
            ds.session_set(key, 1)
    with pytest.raises(yds.ArgumentError, match="not an array"):
        ds.session_push("fields", 1)
    with pytest.raises(yds.ArgumentError, match="NaN"):
        ds.session_set("x", [math.inf])
    with pytest.raises(yds.ArgumentError, match="deeper"):
        v = []
        for _ in range(70):
            v = [v]
        ds.session_set("deep", v)
    ds.session_set("start_utc", "2026-10-21T14:03:12Z")
    assert session(tmp_path / "P07")["start_utc"] == "2026-10-21T14:03:12Z"
    assert not os.path.exists(tmp_path / "P07" / "session.json.tmp")


def test_finish_close_and_order(ds, tmp_path):
    ds.write_trial(trial_index=0)
    ds.write_frame(frame=0)
    with pytest.raises(yds.OrderError):
        ds.zip()
    r = ds.finish()
    assert r == {"trials": 1, "frames": 1, "events": 0, "clock": 0, "streams": {}}
    assert ds.rows() == r and not ds.is_open and ds.finished
    s = session(tmp_path / "P07")
    assert s["closed"] is True and s["rows"] == r and "write_errors" not in s
    with pytest.raises(yds.ClosedError):
        ds.write_trial(trial_index=1)
    with pytest.raises(yds.ClosedError):
        ds.write_frame(frame=1)
    with pytest.raises(yds.ClosedError):
        ds.session_set("x", 1)
    with pytest.raises(yds.ClosedError):
        ds.stream("late", [("x", "int")])
    with pytest.raises(yds.ClosedError):
        ds.finish()
    ds.close()
    ds.close()
    assert len(ds.zip()) > 0   # after close too


def test_zip(ds, tmp_path):
    ds.write_trial(trial_index=0, response="r\u00e9")
    s = ds.stream("dots", [("v", "int")])
    ds.write_stream(s, v=1)
    ds.finish()
    b = ds.zip()
    z = zipfile.ZipFile(io.BytesIO(b))
    assert z.testzip() is None
    names = ["P07/session.json", "P07/trials.csv", "P07/frames.csv", "P07/events.csv", "P07/clock.csv",
             "P07/streams/", "P07/streams/dots.csv"]
    assert z.namelist() == names
    for n in names[:5] + names[6:]:
        with open(tmp_path / n, "rb") as f:
            assert z.read(n) == f.read()
        assert z.getinfo(n).compress_type == zipfile.ZIP_STORED and z.getinfo(n).date_time == (1980, 1, 1, 0, 0, 0)
    ds.zip(str(tmp_path / "P07.zip"))
    with open(tmp_path / "P07.zip", "rb") as f:
        assert f.read() == b
    assert ds.zip() == b   # the same files give the same bytes


def unfinished(d, trials):
    """What an abnormal end leaves: "closed": false, no rows, every row."""
    s = session(d)
    assert s["closed"] is False and "rows" not in s
    assert not (d / "session.json.tmp").exists()
    assert len(read_csv(d / "trials.csv")) == trials + 1
    return s


def test_context_manager_and_dealloc(tmp_path):
    with yds.Dataset(str(tmp_path / "a"), [("x", "int")]) as ds:
        ds.write_trial(x=1)
    assert session(tmp_path / "a")["closed"] is True and not ds.is_open
    # An exception through the with block is an abnormal end: abandoned.
    with pytest.raises(RuntimeError, match="the experiment failed"):
        with yds.Dataset(str(tmp_path / "b"), [("x", "int")]) as ds:
            ds.write_trial(x=1)
            ds.write_frame(frame=0)
            raise RuntimeError("the experiment failed")
    unfinished(tmp_path / "b", 1)
    assert not ds.is_open and not ds.finished
    with pytest.raises(yds.OrderError):
        ds.zip()
    # A Dataset never finished or closed: abandoned, with a ResourceWarning.
    ds = yds.Dataset(str(tmp_path / "c"), [("x", "int")])
    ds.write_trial(x=2)
    with pytest.warns(ResourceWarning, match="never finished or closed"):
        del ds
        gc.collect()
    unfinished(tmp_path / "c", 1)
    # A finished one goes quietly.
    ds = yds.Dataset(str(tmp_path / "d"), [("x", "int")])
    ds.finish()
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        del ds
        gc.collect()
    assert session(tmp_path / "d")["closed"] is True


def test_abandon(tmp_path):
    d = tmp_path / "ab"
    ds = yds.Dataset(str(d), [("x", "int")], session={"experiment": "ab"})
    s = ds.stream("dots", [("t_s", "time")])
    ds.write_trial(x=1)
    ds.write_trial(x=2)
    ds.write_stream(s, t_s=0.5)
    ds.session_push("changes", {"k": 1})
    ds.abandon()
    assert not ds.is_open and not ds.finished
    st = unfinished(d, 2)
    assert st["changes"] == [{"k": 1}] and st["experiment"] == "ab"
    assert read_csv(d / "streams" / "dots.csv") == [["t_s"], ["0.500000000"]]
    with open(d / "trials.csv", "ab"):   # the writer no longer holds the file
        pass
    with pytest.raises(yds.ClosedError):
        ds.write_trial(x=3)
    with pytest.raises(yds.ClosedError):
        ds.finish()
    with pytest.raises(yds.OrderError):
        ds.zip()
    ds.abandon()
    ds.close()   # nothing to finish: still "closed": false
    unfinished(d, 2)
    # After a finish it changes nothing.
    ds = yds.Dataset(str(tmp_path / "fin"), [("x", "int")])
    ds.finish()
    ds.abandon()
    assert session(tmp_path / "fin")["closed"] is True and len(ds.zip()) > 0
    # In a script's own except block.
    try:
        ds = yds.Dataset(str(tmp_path / "exc"), [("x", "int")])
        ds.write_trial(x=1)
        raise KeyboardInterrupt
    except KeyboardInterrupt:
        ds.abandon()
    unfinished(tmp_path / "exc", 1)


def test_sync_flush(tmp_path):
    ds = yds.Dataset(str(tmp_path / "s"), [("x", "int")], sync=True)
    for i in range(3):
        ds.write_trial(x=i)
        ds.write_frame(frame=i)
    assert ds.finish()["trials"] == 3
    assert read_csv(tmp_path / "s" / "trials.csv")[1:] == [["0"], ["1"], ["2"]]


def test_readable_while_open(ds, tmp_path):
    # A monitor (or pandas) reads the files while the session writes them.
    d = tmp_path / "P07"
    for i in range(3):
        ds.write_trial(trial_index=i, correct=True)
        assert len(read_csv(d / "trials.csv")) == i + 2
    assert session(d)["closed"] is False


CHILD = r"""
import os, sys
import ysp.dataset as yds
ds = yds.Dataset(sys.argv[1], [("trial_index", "int"), ("response", "string"), ("rt_s", "time")],
                 session={"experiment": "crash"})
for i in range(5):
    ds.write_frame(frame=i, trial_index=0, onset_s=1.0 + i / 60)
ds.write_event(t_s=1.2, kind="keyboard", text="left", trial_index=0)
ds.write_trial(trial_index=0, response="left", rt_s=0.2)
ds.session_push("changes", {"key": "contrast", "value": 0.5})
ds.write_trial(trial_index=1, response="right", rt_s=0.3)
s = ds.stream("dots", [("t_s", "time")])
ds.write_stream(s, t_s=1.25)
sys.stdout.flush()
os._exit(3)   # no finish, no close, no atexit, no stdio flush: as a crash
"""


def test_crash_leaves_a_readable_unfinished_session(tmp_path):
    d = tmp_path / "crash"
    p = subprocess.run([sys.executable, "-c", CHILD, str(d)], capture_output=True, text=True)
    assert p.returncode == 3, p.stderr
    s = session(d)
    assert s["closed"] is False and "rows" not in s and s["changes"] == [{"key": "contrast", "value": 0.5}]
    assert not (d / "session.json.tmp").exists()
    assert read_csv(d / "trials.csv")[1:] == [["0", "left", "0.200000000"], ["1", "right", "0.300000000"]]
    assert len(read_csv(d / "frames.csv")) == 6
    assert read_csv(d / "events.csv")[1][2] == "keyboard"
    assert read_csv(d / "streams" / "dots.csv") == [["t_s"], ["1.250000000"]]


@pytest.mark.skipif(sys.platform != "win32", reason="Windows file locks: POSIX rename replaces an open file")
def test_rewrite_while_session_json_is_held_windows(ds, tmp_path):
    d = tmp_path / "P07"
    ds.session_set("a", 1)
    with open(d / "session.json", "rb") as f:   # Python opens without FILE_SHARE_DELETE
        before = f.read()
        with pytest.raises(yds.WriteError, match="old session.json stays"):
            ds.session_set("b", 2)
    with open(d / "session.json", "rb") as f:
        assert f.read() == before   # the old file, whole
    assert not (d / "session.json.tmp").exists()
    ds.session_set("c", 3)   # the next rewrite writes the value that failed too
    s = session(d)
    assert (s["a"], s["b"], s["c"]) == (1, 2, 3)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX rename semantics")
def test_rewrite_while_session_json_is_held_posix(ds, tmp_path):
    d = tmp_path / "P07"
    ds.session_set("a", 1)
    with open(d / "session.json", "rb") as f:
        ds.session_set("b", 2)   # the rename replaces the name; the reader keeps the old file
        old = json.loads(f.read())
    assert "b" not in old and old["a"] == 1
    assert session(d)["b"] == 2 and not (d / "session.json.tmp").exists()


def test_seed_text():
    assert yds.seed_text(0) == "x0000000000000000"
    assert yds.seed_text(2**64 - 1) == "xffffffffffffffff"
    assert yds.seed_text(0x467E6F7D45C92EBD) == "x467e6f7d45c92ebd"
    for bad in (-1, 2**64):
        with pytest.raises(yds.ArgumentError):
            yds.seed_text(bad)
    with pytest.raises(TypeError):
        yds.seed_text(1.0)


def test_no_growth_per_row(ds):
    # The binding allocates nothing per row once its text buffer has grown;
    # the count of live interpreter blocks must not climb with the rows.
    def burst(n):
        for i in range(n):
            ds.write_frame(frame=i, trial_index=1, onset_s=i / 60.0, residual_s=0.0001)
            ds.write_event(t_s=i / 60.0, kind="mark", stimulus="gabor", trial_index=1, code=i)
            ds.write_trial(trial_index=i, response="left", correct=True, rt_s=0.25)
    burst(200)
    gc.collect()
    before = sys.getallocatedblocks()
    burst(2000)
    gc.collect()
    assert sys.getallocatedblocks() - before < 50


def test_trial_rows_from_ysp_trials(tmp_path):
    # trials.csv has no session part (desc.trials NULL), so the player writes
    # the trials writer's columns as its own: they must equal ysp.trials'
    # data line, text for text.
    ytr = pytest.importorskip("ysp.trials")
    t = ytr.Trials(n_conditions=2, reps=3, order=ytr.ORDER_FULL_RANDOM, rng=1, seed=7,
                   vars=[ytr.var_choice("side", ["left", "right"])], jitters=[ytr.uniform("fp_s", 0.5, 1.0)])
    names = t.format_header().strip().split(",")
    types = {"index": "int", "block": "int", "rep": "int", "condition": "int", "track": "int", "practice": "bool",
             "warmup": "bool", "requeued": "bool", "after_break": "bool", "outcome": "int", "fp_s": "time",
             "side": "string", "trial_seed": "string"}
    assert set(names) == set(types)
    ds = yds.Dataset(str(tmp_path / "t"), [(n, types[n]) for n in names] + [("rt_s", "time")])
    lines = []
    while (ti := t.next()) is not None:
        t.update(ti.index % 2)
        h = t.history()[ti.index]
        row = {n: getattr(h, n) for n in ("index", "block", "rep", "condition", "track", "practice", "warmup",
                                          "requeued", "after_break", "outcome")}
        row.update(fp_s=t.jitter(ti.index, "fp_s").s, side=t.var(ti.index, "side"),
                   trial_seed=yds.seed_text(t.trial_seed(ti.index)), rt_s=0.25)
        ds.write_trial(row)
        lines.append(t.format_row(ti.index).strip())
    ds.finish()
    got = read_csv(tmp_path / "t" / "trials.csv")
    assert got[0] == names + ["rt_s"]
    assert [",".join(r[:-1]) for r in got[1:]] == lines


def test_pandas_reads_every_file(ds, tmp_path):
    pd = pytest.importorskip("pandas")
    ds.write_trial(trial_index=0, response="a,\nb", correct=True, rt_s=0.5)
    ds.write_trial(trial_index=1)
    ds.write_frame(frame=0, onset_s=1.0)
    ds.finish()
    t = pd.read_csv(tmp_path / "P07" / "trials.csv")
    assert list(t["response"].fillna("")) == ["a,\nb", ""] and t["rt_s"].isna().tolist() == [False, True]
    f = pd.read_csv(tmp_path / "P07" / "frames.csv")
    assert list(f.columns) == list(yds.FRAME_COLUMNS) and f["onset_s"][0] == 1.0
