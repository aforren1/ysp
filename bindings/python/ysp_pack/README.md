# ysp.pack Python binding

A CPython extension for the [ysp/pack.h](../../../include/ysp/pack.h)
single-header library: it opens a ysp pack (docs/pack.md: one store-only
zip file, entries aligned to 4096 bytes, XXH64 hashes over 1 MiB chunks, a
SHA-256 manifest), lists its entries, reads them, and verifies them. It
uses the plain CPython C API and has no dependencies. It is built against
the **Limited API / stable ABI** (`Py_LIMITED_API = 0x030A0000`), so one
`ysp/pack.abi3.so` works on CPython 3.10 and later.

The distribution is `ysp-pack` and the module is `ysp.pack`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-pack` installs alone or next to the other `ysp-*`
distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

In a development tree, setup.py finds `ysp/pack.h` in the repository's
`include/` directory. An isolated build (an sdist or a cibuildwheel run)
sees only this directory. Copy the header and `ysp/json.h`, which it
requires, into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/pack.h ../../../include/ysp/json.h include/ysp/
uv build --sdist
```

## How to read a pack

```python
import json
import ysp.pack as yp

with yp.Pack("study.ysppak") as pk:          # mapped read-only
    print(pk.describe())                     # format, pack ID, entries, policy
    for e in pk.entries():
        print(e.name, e.kind, e.size)
    table = pk.read("conditions.pstb")       # bytes; the entry is hashed first
    head = pk.read("video/intro.mp4", 0, 4096)   # a range: only its 1 MiB chunk is hashed
    definition = json.loads(pk.definition()) # the EXPERIMENT entry, or None
    manifest = pk.manifest()                 # ysp/manifest.json as a dict
```

`Pack.from_bytes(data)` reads a pack in memory. A `bytes` object is kept,
not copied; a `bytearray` or `memoryview` is copied once.

## How to verify a pack

```python
try:
    pk = yp.Pack(path, verify="open")        # every entry hashed at open
except yp.CorruptError as ex:
    print(ex)    # ysp_pack: 'readme.txt': does not match its entry hash (corrupt)

pk.verify()                 # every entry now, whatever the policy
pk.verify("data.bin")       # one entry now, whatever the policy
```

Each failure raises a subclass of `ysp.pack.Error` with the header's
message, which names the rule or the entry and the chunk:

| Exception | Code | Also a |
|---|---|---|
| `ArgumentError` | `YPAK_ERR_ARG`, or a closed pack | `ValueError` |
| `ReadError` | `YPAK_ERR_IO`: the file cannot be opened or mapped | `OSError` |
| `FormatError` | `YPAK_ERR_FORMAT`: outside the format | `ValueError` |
| `CorruptError` | `YPAK_ERR_CORRUPT`: a hash does not match | `ValueError` |
| `VersionError` | `YPAK_ERR_VERSION`: another pack format | `ValueError` |
| `NotFoundError` | `YPAK_ERR_NOT_FOUND`: no entry of that name | `KeyError` |

The reader checks the XXH64 hashes, and at open the manifest's SHA-256
against the pack ID. The SHA-256 of each entry in the manifest is for
citation; `hashlib` checks it (see `example.py`). None of these hashes is a
signature: they find corruption, not forgery.

## Reference

`Pack(path, verify="use")` and `Pack.from_bytes(data, verify="use")`.
`verify` is `"use"` (an entry is hashed when it is first read), `"open"`
(every entry at open) or `"none"` (no hashes after open; `verify()` and
`verify(name)` still hash).

| Member | What it gives |
|---|---|
| `entries()`, `names()` | Every entry, or its name, in pack order; the manifest is first |
| `entry(name)` | An `Entry`: `name`, `index`, `kind` (`"table"`, ...), `size`, `stream`, `flags`, `header_off`, `data_off`, `xxh64`, `crc32` |
| `read(name, offset=0, size=-1)`, `pk[name]` | The bytes, a copy. `name` can be an `Entry` |
| `verify(name=None)` | Hashes every entry, or one, whatever the policy; raises on a mismatch |
| `definition()`, `experiment` | The EXPERIMENT entry's bytes and its `Entry`, or `None` |
| `manifest()` | The manifest, parsed by the standard library's `json` |
| `id`, `format`, `file_size`, `zip_start`, `n_chunks`, `policy`, `hashed_bytes`, `closed` | The pack's properties |
| `describe()` | One line for a log |
| `len(pk)`, `name in pk`, `iter(pk)` | The entry count, a lookup, the names |
| `close()`, `with` | Unmaps the file |
| `xxh64(data, seed=0)`, `entry_hash(data)` | The hashes; `entry_hash` is the manifest's `"xxh64"` |

`read()` returns a copy, so its bytes stay valid after `close()`. A
`memoryview` of the mapping would avoid the copy, but the Limited API
before 3.11 cannot tie its life to the `Pack`, and a view that outlived
`close()` would read unmapped memory.

Calls that hash or copy entry data release the GIL, and the header lets
several threads read one pack at once. A `close()` from another thread
while such a call runs marks the pack closed; the last running call unmaps
it.

Not bound: the range-reader and incremental sources (a browser's), the
views of the pack's own forms (curve sets, artwork, glyph runs, shaders,
textures; the player reads them), `ypak_runs_key()`, the arena, SHA-256 and
CRC-32 (`hashlib` and `zlib` have them). docs/pack.md, Bindings, says why.
