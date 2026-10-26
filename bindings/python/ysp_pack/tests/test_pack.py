"""Tests for ysp.pack: the binding's surface over ysp/pack.h, on packs from
the pack tool (tests/pack/fixtures/small.ysppak, when the repository is
there) and from packwriter.py (independent of the tool). The header's own
test is tests/adapt/pack_test.c."""
import gc
import hashlib
import json
import os
import random
import re
import sys
import threading

import pytest

import ysp.pack as yp

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packwriter as pw  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "..", "..", "..", "tests", "pack", "fixtures", "small.ysppak")
CHUNK = 1 << 20

SMALL = [
    ("conditions.pstb", "table", b"target,contrast\na,0.25\nb,0.5\n"),
    ("experiment/main.json", "experiment", b'{"format": "ysp-experiment", "version": 1}\n'),
    ("readme.txt", "file", b"hello\n"),
    ("empty.bin", "file", b""),
]
BIG = bytes(random.Random(7).getrandbits(8) for _ in range(5000)) * ((5 * CHUNK // 2 + 77) // 5000 + 1)
BIG = BIG[:5 * CHUNK // 2 + 77]   # 2.5 MiB + 77: three chunks


def fast(data, seed=0):
    return yp.xxh64(data, seed)


@pytest.fixture(scope="module")
def small():
    return pw.build(SMALL)


@pytest.fixture(scope="module")
def big():
    return pw.build(SMALL + [("video/big.bin", "file", BIG, yp.F_STREAM)], xxh64=fast)


def test_version_matches_pyproject():
    text = open(os.path.join(HERE, "..", "pyproject.toml")).read()
    assert re.search(r'^version = "([^"]+)"', text, re.M).group(1) == yp.__version__ == yp.version()


def test_hashes():
    # XXH64's published answers, then the specification in pure Python
    assert yp.xxh64(b"") == 0xEF46DB3751D8E999
    assert yp.xxh64(b"a") == 0xD24EC4F1A98C6E5B
    assert yp.xxh64(b"abc") == 0x44BC2CF5AD770999
    rng = random.Random(1)
    for n in list(range(0, 70)) + [100, 1000, 4097]:
        d = bytes(rng.getrandbits(8) for _ in range(n))
        seed = rng.getrandbits(64)
        assert yp.xxh64(d, seed) == pw.xxh64_py(d, seed), n
    assert yp.xxh64(bytearray(b"abc")) == yp.xxh64(memoryview(b"abc")) == 0x44BC2CF5AD770999
    for d in (b"", b"abc", BIG):
        assert yp.entry_hash(d) == pw.entry_hash(d, fast)
    with pytest.raises(TypeError):
        yp.xxh64("abc")
    with pytest.raises(TypeError):
        yp.entry_hash(3)


def check_open(pk, entries):
    want = {n: bytes(d) for n, _, d, *_ in entries}
    assert len(pk) == len(want) + 2 and pk.format == yp.FORMAT
    assert pk.names()[0] == yp.MANIFEST and yp.CHUNKS in pk
    for n, d in want.items():
        assert pk.read(n) == d and pk[n] == d and n in pk
        e = pk.entry(n)
        assert e.name == n and e.size == len(d) and e.data_off % yp.ALIGN == 0
        assert e.xxh64 == yp.entry_hash(d) and pk.read(e) == d
    m = pk.manifest()
    assert pk.id == "sha256:" + hashlib.sha256(pk.read(yp.MANIFEST)).hexdigest()
    for me in m["entries"]:
        assert hashlib.sha256(pk.read(me["name"])).hexdigest() == me["sha256"]
        assert "%016x" % pk.entry(me["name"]).xxh64 == me["xxh64"]


def test_tool_fixture():
    if not os.path.exists(FIXTURE):
        pytest.skip("tests/pack/fixtures/small.ysppak is in the repository, not in the sdist")
    with yp.Pack(FIXTURE) as pk:
        assert pk.policy == "use"
        names = pk.names()
        assert names == ["ysp/manifest.json", "conditions.pstb", "experiment/main.json", "readme.txt", "ysp/chunks"]
        m = pk.manifest()
        assert m["format"] == "ysp-pack" and m["version"] == pk.format == 1
        assert pk.experiment.name == m["experiment"]["entry"] and pk.experiment.kind == "experiment"
        # the definition's canonical bytes: its SHA-256 is the experiment ID (D2)
        d = pk.definition()
        assert json.loads(d)["format"] == "ysp-experiment" and d.endswith(b"}\n")
        rec = [e for e in m["entries"] if e["name"] == pk.experiment.name][0]
        assert rec["derivation"]["op"] == "canonical" and hashlib.sha256(d).hexdigest() == rec["sha256"]
        for me in m["entries"]:
            data = pk.read(me["name"])
            assert len(data) == me["size"] and hashlib.sha256(data).hexdigest() == me["sha256"]
            assert "%016x" % pk.entry(me["name"]).xxh64 == me["xxh64"]
        assert pk.read("readme.txt") == open(os.path.join(os.path.dirname(FIXTURE), "small", "readme.txt"), "rb").read()
        pk.verify()
        pk.verify("conditions.pstb")
        assert pk.hashed_bytes >= sum(e.size for e in pk.entries()) - pk.entry(yp.MANIFEST).size
        assert pk.id in pk.describe() and "verified on use" in pk.describe()
        assert pk.id == "sha256:" + hashlib.sha256(pk.read(yp.MANIFEST)).hexdigest()


def test_memory_and_file(small, tmp_path):
    for src in (small, bytearray(small), memoryview(small)):
        with yp.Pack.from_bytes(src) as pk:
            check_open(pk, SMALL)
            assert pk.file_size == len(small) and pk.zip_start == 0 and pk.n_chunks == 0
            assert pk.experiment.name == "experiment/main.json"
            assert pk.definition() == SMALL[1][2]
            assert pk.read("empty.bin") == b""
    p = tmp_path / "small.ysppak"
    p.write_bytes(small)
    for path in (str(p), p):   # str and os.PathLike
        with yp.Pack(path, verify="open") as pk:
            check_open(pk, SMALL)
            assert pk.policy == "open"
    # a name outside ASCII in the path (UTF-8 on Windows, bytes on POSIX)
    q = tmp_path / ("pack " + chr(0xE9) + chr(0x4E2D) + ".ysppak")
    q.write_bytes(small)
    with yp.Pack(q) as pk:
        assert pk.read("readme.txt") == b"hello\n"


def test_ranges_and_chunks(big):
    with yp.Pack.from_bytes(big) as pk:
        e = pk.entry("video/big.bin")
        assert e.stream and e.flags & yp.F_STREAM and e.size == len(BIG) and pk.n_chunks == 3
        h0 = pk.hashed_bytes   # the manifest and ysp/chunks, hashed at open
        assert pk.read(e, 0, 100) == BIG[:100]
        assert pk.hashed_bytes - h0 == CHUNK   # a range hashes only its chunk
        for off, n in ((CHUNK - 5, 10), (2 * CHUNK + 70, 100), (len(BIG), 5), (len(BIG) + 9, 1), (7, 0), (3, -1)):
            assert pk.read(e, off, n) == BIG[off:off + n if n >= 0 else None], (off, n)
        assert pk.read(e) == BIG
        pk.verify()


def test_appended(small, tmp_path):
    player = b"MZ" + bytes(8190)   # a "player" padded to 4096, as ypak append does
    for absolute in (False, True):
        data = pw.build(SMALL, prefix=player, absolute=absolute)
        with yp.Pack.from_bytes(data) as pk:
            assert pk.zip_start == len(player)
            check_open(pk, SMALL)
            pk.verify()
            assert pk.id == yp.Pack.from_bytes(small).id   # the pack ID does not move


def test_corrupt_entry(small):
    off = small.index(b"hello\n")
    bad = bytearray(small)
    bad[off + 1] ^= 0x04
    bad = bytes(bad)
    with yp.Pack.from_bytes(bad) as pk:   # verify="use": the open checks no entry data
        assert pk.read("conditions.pstb") == SMALL[0][2]
        with pytest.raises(yp.CorruptError, match=r"'readme\.txt'.*corrupt"):
            pk.read("readme.txt")
        with pytest.raises(yp.CorruptError, match="readme.txt"):
            pk.verify("readme.txt")
        with pytest.raises(yp.CorruptError, match="readme.txt"):
            pk.verify()
        pk.verify("conditions.pstb")
    with pytest.raises(yp.CorruptError, match="readme.txt"):
        yp.Pack.from_bytes(bad, verify="open")
    with yp.Pack.from_bytes(bad, verify="none") as pk:   # no hashes after open: the damage reads through
        assert pk.read("readme.txt") == b"hallo\n"
        assert "NOT VERIFIED" in pk.describe()
        h0 = pk.hashed_bytes
        with pytest.raises(yp.CorruptError, match=r"'readme\.txt'.*corrupt"):
            pk.verify("readme.txt")   # ypak_verify_entry hashes whatever the policy
        assert pk.hashed_bytes > h0
        with pytest.raises(yp.CorruptError, match="readme.txt"):
            pk.verify("readme.txt")   # a failure sets no verified flag
        pk.verify("conditions.pstb")
        h1 = pk.hashed_bytes
        pk.verify("conditions.pstb")  # hashed once per open pack
        assert pk.hashed_bytes == h1
        with pytest.raises(yp.CorruptError, match="readme.txt"):
            pk.verify()   # verify_all hashes whatever the policy


def test_corrupt_chunk(big):
    e = yp.Pack.from_bytes(big).entry("video/big.bin")
    bad = bytearray(big)
    bad[e.data_off + CHUNK + 1000] ^= 0x80   # chunk 1
    with yp.Pack.from_bytes(bytes(bad)) as pk:
        assert pk.read(e, 0, 1000) == BIG[:1000]
        assert pk.read(e, 2 * CHUNK, 77) == BIG[2 * CHUNK:2 * CHUNK + 77]
        with pytest.raises(yp.CorruptError, match="chunk 1"):
            pk.read(e, CHUNK + 999, 2)
        with pytest.raises(yp.CorruptError, match="chunk 1"):
            pk.read(e)
        assert pk.read("readme.txt") == b"hello\n"


def refused_or_same(data, want, counts):
    """Each damaged pack is refused (at open, at a read or at verify) or
    reads the original bytes; nothing else, and never a crash."""
    try:
        pk = yp.Pack.from_bytes(data)
    except yp.Error as ex:
        assert str(ex).strip("'\"").startswith("ysp_pack: ") or isinstance(ex, MemoryError), str(ex)
        counts["open"] += 1
        return
    try:
        for n, d in want.items():
            assert pk.read(n) == d
        pk.verify()
        counts["same"] += 1
    except yp.Error:
        counts["use"] += 1
    finally:
        pk.close()


def test_truncated(small, tmp_path):
    for n in range(len(small)):
        with pytest.raises(yp.Error):
            yp.Pack.from_bytes(small[:n])
    p = tmp_path / "cut.ysppak"
    for n in (0, 21, 22, 4096, len(small) // 2, len(small) - 1):
        p.write_bytes(small[:n])
        with pytest.raises(yp.Error, match="ysp_pack: "):
            yp.Pack(p)


def flip_all(data, want, step=1):
    counts = {"open": 0, "use": 0, "same": 0}
    for i in range(0, len(data), step):
        for bit in (0x01, 0x80):
            b = bytearray(data)
            b[i] ^= bit
            refused_or_same(bytes(b), want, counts)
    return counts


def test_flipped_bytes(small):
    want = {n: d for n, _, d in SMALL}
    counts = flip_all(small, want)
    # format 1 leaves no byte unchecked: the comment holds the directory's
    # hash and the pack ID, the local headers and padding are checked on
    # first use and the data by its hashes, so no flip reads the same
    assert counts["open"] + counts["use"] + counts["same"] == 2 * len(small)
    assert counts["same"] == 0, counts


def test_flipped_bytes_tool_fixture():
    if not os.path.exists(FIXTURE):
        pytest.skip("tests/pack/fixtures/small.ysppak is in the repository, not in the sdist")
    data = open(FIXTURE, "rb").read()
    with yp.Pack.from_bytes(data) as pk:
        want = {n: pk.read(n) for n in pk.names()}
    counts = flip_all(data, want)
    assert counts["same"] == 0, counts


def test_versions_and_garbage(small):
    with pytest.raises(yp.VersionError, match="format 2"):
        yp.Pack.from_bytes(pw.build(SMALL, comment_format=2))
    for junk in (b"", b"\0" * 100, b"PK\x05\x06" + bytes(18), os.urandom(70000)):
        with pytest.raises(yp.FormatError):
            yp.Pack.from_bytes(junk)


def test_arguments(small, tmp_path):
    p = tmp_path / "a.ysppak"
    p.write_bytes(small)
    with pytest.raises(TypeError):
        yp.Pack()
    with pytest.raises(TypeError):
        yp.Pack(123)
    with pytest.raises(TypeError, match="from_bytes"):
        yp.Pack(small)
    with pytest.raises(TypeError):
        yp.Pack.from_bytes("a str")
    with pytest.raises(TypeError):
        yp.Pack(p, verify=1)
    with pytest.raises(yp.ArgumentError, match="verify"):
        yp.Pack(p, verify="later")
    with pytest.raises(yp.ReadError, match="ysp_pack: cannot open") as ex:
        yp.Pack(tmp_path / "missing.ysppak")
    assert isinstance(ex.value, OSError)
    with pytest.raises(ValueError):   # an embedded NUL
        yp.Pack(str(p) + "\0x")
    with pytest.raises(yp.Error):
        yp.Pack(tmp_path)   # a directory
    pk = yp.Pack(p)
    with pytest.raises(yp.NotFoundError, match="no entry 'nope'") as ex:
        pk.read("nope")
    assert isinstance(ex.value, KeyError)
    with pytest.raises(KeyError, match="case-sensitive.*readme.txt"):
        pk["README.TXT"]
    with pytest.raises(yp.NotFoundError):
        pk.read("readme.txt\0")
    assert "nope" not in pk and 3 not in pk
    with pytest.raises(TypeError):
        pk.read(3)
    with pytest.raises(TypeError):
        pk.read(b"readme.txt")
    with pytest.raises(yp.ArgumentError, match="offset"):
        pk.read("readme.txt", -1)
    with pytest.raises(yp.ArgumentError, match="size"):
        pk.read("readme.txt", 0, -2)
    with pytest.raises(yp.ArgumentError, match="already open"):
        pk.__init__(p)
    data = pk.read("readme.txt")
    pk.close()
    pk.close()
    assert pk.closed
    for f in (lambda: pk.read("readme.txt"), lambda: len(pk), lambda: pk.id, lambda: pk.entries(),
              lambda: pk.verify(), lambda: "readme.txt" in pk, lambda: pk.manifest()):
        with pytest.raises(yp.ArgumentError, match="closed"):
            f()
    del pk
    gc.collect()
    assert data == b"hello\n"   # read() returns a copy; it outlives the pack
    pk = yp.Pack(p)             # a closed Pack opens again through __init__
    pk.close()
    pk.__init__(p)
    assert pk.read("readme.txt") == b"hello\n"
    pk.close()


def test_threads(big):
    pk = yp.Pack.from_bytes(big)
    want = {n: d for n, _, d in SMALL}
    want["video/big.bin"] = BIG
    errors = []

    def reader(k):
        try:
            for _ in range(10):
                for n, d in want.items():
                    assert pk.read(n) == d
                assert pk.read("video/big.bin", k * 1000, 4000) == BIG[k * 1000:k * 1000 + 4000]
        except Exception as ex:   # pragma: no cover - reported below
            errors.append(ex)

    ts = [threading.Thread(target=reader, args=(k,)) for k in range(4)] + [threading.Thread(target=pk.verify)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not errors, errors
    pk.close()


def test_close_while_busy():
    # 48 MiB: a verify long enough that close() lands while it runs
    data = pw.build([("big.bin", "file", bytes(48 * CHUNK))], xxh64=fast)
    finished = 0
    for _ in range(5):
        pk = yp.Pack.from_bytes(data)
        started = threading.Event()
        out = []

        def work():
            started.set()
            try:
                pk.verify()
                out.append("ok")
            except yp.ArgumentError as ex:   # close() came before the call began
                out.append(str(ex))

        t = threading.Thread(target=work)
        t.start()
        started.wait()
        pk.close()
        assert pk.closed
        t.join()
        assert out and (out[0] == "ok" or "closed" in out[0])
        finished += out[0] == "ok"   # the verify ran on and the last call unmapped
        with pytest.raises(yp.ArgumentError, match="closed"):
            pk.read("big.bin", 0, 1)
    assert finished >= 1


@pytest.mark.skipif(not os.environ.get("YSP_PACK_CORPUS"), reason="set YSP_PACK_CORPUS to the tool test's corpus.ysppak")
def test_tool_corpus():
    """The pack tool's corpus (build/.../pack_tool_work/corpus.ysppak): every
    kind the core tool writes, multi-chunk entries, against the manifest."""
    with yp.Pack(os.environ["YSP_PACK_CORPUS"]) as pk:
        m = pk.manifest()
        assert len(m["entries"]) == len(pk) - 1
        for me in m["entries"]:
            d = pk.read(me["name"])
            assert hashlib.sha256(d).hexdigest() == me["sha256"], me["name"]
            assert yp.entry_hash(d) == pk.entry(me["name"]).xxh64
        pk.verify()
        assert pk.n_chunks > 0
