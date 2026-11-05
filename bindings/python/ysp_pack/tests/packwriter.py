"""A small ysp pack writer for the tests, independent of the pack tool.

It follows docs/pack.md 2 (the layout of tests/adapt/pack_test.c's writer):
store-only zip, the manifest first, the other names in bytewise order, each
entry's data at a 4096 multiple through the 0xD935 alignment field, the
0x7379 record, ysp/chunks, and the comment with the pack ID and the
directory hash. No zip64: the tests stay far below 4 GiB.

The hash is a parameter so the large packs of the thread tests can use the
binding's fast XXH64; xxh64_py() is the specification's algorithm in pure
Python, for the known-answer tests.
"""
import hashlib
import json
import struct
import zlib

MASK = (1 << 64) - 1
P1, P2, P3, P4, P5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
                      0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)
CHUNK = 1 << 20
ALIGN = 4096
NO_CHUNK = 0xFFFFFFFF
KIND = {"manifest": 1, "chunks": 2, "experiment": 3, "file": 4, "table": 5, "calibration": 6, "font": 7,
        "curveset": 8, "glyphruns": 9, "artwork": 10, "audio": 11, "shader": 12, "texture": 13, "video": 14,
        "video_index": 15, "frameseq": 16, "alpha": 17}


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & MASK


def _round(acc, lane):
    return (_rotl((acc + lane * P2) & MASK, 31) * P1) & MASK


def _merge(acc, v):
    return ((acc ^ _round(0, v)) * P1 + P4) & MASK


def xxh64_py(data, seed=0):
    n = len(data)
    i = 0
    if n >= 32:
        v = [(seed + P1 + P2) & MASK, (seed + P2) & MASK, seed & MASK, (seed - P1) & MASK]
        while i + 32 <= n:
            for k in range(4):
                v[k] = _round(v[k], struct.unpack_from("<Q", data, i + 8 * k)[0])
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & MASK
        for k in range(4):
            h = _merge(h, v[k])
    else:
        h = (seed + P5) & MASK
    h = (h + n) & MASK
    while i + 8 <= n:
        h ^= _round(0, struct.unpack_from("<Q", data, i)[0])
        h = (_rotl(h, 27) * P1 + P4) & MASK
        i += 8
    if i + 4 <= n:
        h ^= (struct.unpack_from("<I", data, i)[0] * P1) & MASK
        h = (_rotl(h, 23) * P2 + P3) & MASK
        i += 4
    while i < n:
        h ^= (data[i] * P5) & MASK
        h = (_rotl(h, 11) * P1) & MASK
        i += 1
    h ^= h >> 33
    h = (h * P2) & MASK
    h ^= h >> 29
    h = (h * P3) & MASK
    h ^= h >> 32
    return h


def chunk_hashes(data, xxh64):
    k = max(1, (len(data) + CHUNK - 1) // CHUNK)
    return [xxh64(data[j * CHUNK:(j + 1) * CHUNK], 0) for j in range(k)]


def entry_hash(data, xxh64=xxh64_py):
    """docs/pack.md 2.8: XXH64, seed = size, over the chunk hashes as little-endian u64."""
    return xxh64(b"".join(struct.pack("<Q", c) for c in chunk_hashes(data, xxh64)), len(data))


def build(entries, xxh64=xxh64_py, prefix=b"", absolute=False, comment_format=1, extra_manifest=None):
    """entries: (name, kind, data[, flags]) tuples; kind is a KIND name.
    prefix: bytes of a "player" before the zip; absolute: offsets from the file
    start (an appended copy) instead of from the zip start. Returns the bytes."""
    ents = []
    for t in entries:
        name, kind, data = t[0], t[1], bytes(t[2])
        flags = t[3] if len(t) > 3 else 0
        ents.append({"name": name.encode(), "kind": KIND[kind], "kname": kind, "data": data, "flags": flags})
    ents.sort(key=lambda e: e["name"])
    for e in ents:
        e["ch"] = chunk_hashes(e["data"], xxh64)
        e["hash"] = xxh64(b"".join(struct.pack("<Q", c) for c in e["ch"]), len(e["data"]))
    table, k = [], 0
    for e in ents:
        if len(e["ch"]) > 1:
            e["chunk"] = k
            table += e["ch"]
            k += len(e["ch"])
        else:
            e["chunk"] = NO_CHUNK
    chunks = b"YSPCHNK1" + struct.pack("<IIQQ", 1, 20, len(table), 0) + b"".join(struct.pack("<Q", c) for c in table)
    ch = {"name": b"ysp/chunks", "kind": KIND["chunks"], "kname": "chunks", "data": chunks, "flags": 0, "chunk": NO_CHUNK}
    ch["hash"] = entry_hash(chunks, xxh64)
    ents.append(ch)
    ents.sort(key=lambda e: e["name"])
    man = {
        "entries": [{"crc32": "%08x" % zlib.crc32(e["data"]), "kind": e["kname"], "name": e["name"].decode(),
                     "sha256": hashlib.sha256(e["data"]).hexdigest(), "size": len(e["data"]),
                     "xxh64": "%016x" % e["hash"]} for e in ents],
        "format": "ysp-pack",
        "version": 1,
    }
    exp = [e for e in ents if e["kname"] == "experiment"]
    if exp:
        man["experiment"] = {"entry": exp[0]["name"].decode(), "media_type": "application/vnd.ysp.experiment"}
    if extra_manifest:
        man.update(extra_manifest)
    mbytes = (json.dumps(man, indent=2, sort_keys=True) + "\n").encode()
    m = {"name": b"ysp/manifest.json", "kind": KIND["manifest"], "data": mbytes, "flags": 0, "chunk": NO_CHUNK}
    m["hash"] = entry_hash(mbytes, xxh64)
    ents.insert(0, m)

    out = bytearray(prefix)
    base = 0 if absolute else len(prefix)
    for e in ents:
        e["crc"] = zlib.crc32(e["data"])
        at = len(out)
        nl = len(e["name"])
        drel = (at + 30 + nl + 6 + ALIGN - 1) & ~(ALIGN - 1)
        pad = drel - (at + 30 + nl + 6)
        e["hdr"], e["drel"] = at, drel
        size = len(e["data"])
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0x0800, 0, 0, 0x21, e["crc"], size, size, nl, 6 + pad)
        out += e["name"] + struct.pack("<HHH", 0xD935, 2 + pad, ALIGN) + bytes(pad) + e["data"]
    cd = bytearray()
    for e in ents:
        size = len(e["data"])
        ext = struct.pack("<HHBBHIQQI", 0x7379, 28, 1, 0, e["kind"], e["flags"], e["drel"] - len(prefix), e["hash"], e["chunk"])
        cd += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 0x0314, 20, 0x0800, 0, 0, 0x21, e["crc"], size, size,
                          len(e["name"]), len(ext), 0, 0, 0, 0o100644 << 16, e["hdr"] - base)
        cd += e["name"] + ext
    cd_off = len(out) - base
    out += cd
    comment = ("ysp-pack/%d manifest=sha256:%s cd=xxh64:%016x" % (comment_format, hashlib.sha256(mbytes).hexdigest(),
                                                                  xxh64(bytes(cd), 0))).encode()
    n = len(ents)
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, n, n, len(cd), cd_off, len(comment)) + comment
    return bytes(out)
